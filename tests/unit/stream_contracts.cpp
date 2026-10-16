#include <gtest/gtest.h>
#include <vmf/wire.h>
#include "../acceptance/vectors.h"
#include "fixtures.hpp"
#include <vector>

namespace
{

struct stream_schema {
	vmf_node root = {};
	vmf_node field = {};
	vmf_message_schema schema = {};
	const vmf_message_schema *entry = &schema;
	vmf_schema_set set = {&entry, 1};
};

void initialize(stream_schema *s, uint8_t umf = 1)
{
	fixtures::scalar(&s->field, 2, 8);
	fixtures::group(&s->root, 1, &s->field, 1);
	s->schema.root = &s->root;
	s->schema.id.user_message_format = umf;
	s->schema.octet_order = VMF_BIT_LSB_FIRST;
}

vmf_status finish(vmf_decoder *reader, vmf_input *input,
	std::vector<vmf_event_kind> *kinds = nullptr)
{
	for (size_t i = 0; i < 100; ++i) {
		vmf_event event = {};
		vmf_status status = vmf_decoder_next(reader, input, &event);

		if (status != VMF_OK) {
			EXPECT_EQ(nullptr, event.node);
			return status;
		}
		if (kinds)
			kinds->push_back(event.kind);
	}
	ADD_FAILURE() << "decoder did not terminate";
	return VMF_INVALID_STATE;
}

TEST(Streaming, EverySplitPreservesSizedConcatenatedPdus)
{
	stream_schema s;

	initialize(&s);
	auto options = fixtures::options(VMF_ENVELOPE_D1, &s.set);
	vmf_decoder *reader = nullptr;

	ASSERT_EQ(VMF_OK, vmf_decoder_create(&options, &reader, nullptr));
	std::vector<uint8_t> bytes(vector_d1, vector_d1 + sizeof(vector_d1));

	bytes.insert(bytes.end(), vector_d1, vector_d1 + sizeof(vector_d1));
	for (size_t split = 0; split <= sizeof(vector_d1); ++split) {
		SCOPED_TRACE(split);
		vmf_decoder_reset(reader);
		ASSERT_EQ(VMF_OK, vmf_decoder_begin_pdu(reader));
		vmf_input first = {bytes.data(), split, 0, false};
		std::vector<vmf_event_kind> kinds;
		vmf_status status = finish(reader, &first, &kinds);

		EXPECT_EQ(split, first.used);
		if (split < sizeof(vector_d1))
			EXPECT_EQ(VMF_NEED_INPUT, status);
		else
			EXPECT_EQ(VMF_DONE, status);
		vmf_input rest = {
			bytes.data() + split, bytes.size() - split, 0, true};

		EXPECT_EQ(VMF_DONE, finish(reader, &rest, &kinds));
		EXPECT_EQ(sizeof(vector_d1) - split, rest.used);
		EXPECT_EQ(
			(std::vector<vmf_event_kind>{VMF_EVENT_ENVELOPE,
				VMF_EVENT_MESSAGE_BEGIN, VMF_EVENT_GROUP_BEGIN,
				VMF_EVENT_FIELD, VMF_EVENT_GROUP_END,
				VMF_EVENT_MESSAGE_END, VMF_EVENT_PDU_END}),
			kinds);
		ASSERT_EQ(VMF_OK, vmf_decoder_begin_pdu(reader));
		EXPECT_EQ(VMF_DONE, finish(reader, &rest));
		EXPECT_EQ(bytes.size() - split, rest.used);
	}
	vmf_decoder_destroy(reader);
}

TEST(Streaming, UnsizedFixedBodyWaitsForEmptyFinalAndRejectsExtraOctet)
{
	stream_schema s;

	initialize(&s, 0);
	auto options = fixtures::options(VMF_ENVELOPE_D1, &s.set);
	vmf_decoder *reader = nullptr;

	ASSERT_EQ(VMF_OK, vmf_decoder_create(&options, &reader, nullptr));
	ASSERT_EQ(VMF_OK, vmf_decoder_begin_pdu(reader));
	vmf_input input = {vector_missing, sizeof(vector_missing), 0, false};

	EXPECT_EQ(VMF_NEED_INPUT, finish(reader, &input));
	EXPECT_EQ(sizeof(vector_missing), input.used);
	vmf_input final = {nullptr, 0, 0, true};

	EXPECT_EQ(VMF_DONE, finish(reader, &final));
	vmf_decoder_reset(reader);
	ASSERT_EQ(VMF_OK, vmf_decoder_begin_pdu(reader));
	input.used = 0;
	EXPECT_EQ(VMF_NEED_INPUT, finish(reader, &input));
	const uint8_t extra = 0;
	vmf_input trailing = {&extra, 1, 0, true};

	EXPECT_EQ(VMF_MALFORMED, finish(reader, &trailing));
	EXPECT_LE(trailing.used, trailing.size);
	EXPECT_EQ(VMF_MALFORMED, finish(reader, &final));
	vmf_decoder_reset(reader);
	ASSERT_EQ(VMF_OK, vmf_decoder_begin_pdu(reader));
	input.used = 0;
	input.final = true;
	EXPECT_EQ(VMF_DONE, finish(reader, &input));
	vmf_decoder_destroy(reader);
}

struct callback_log {
	vmf_event_kind stop;
	vmf_event_action action;
	size_t calls;
};

vmf_event_action callback(void *user, const vmf_event *event)
{
	auto *log = static_cast<callback_log *>(user);

	++log->calls;
	return event->kind == log->stop ? log->action : VMF_EVENT_CONTINUE;
}

TEST(Streaming, CallbackPauseConsumesEventAndAbortRequiresReset)
{
	stream_schema s;

	initialize(&s);
	auto options = fixtures::options(VMF_ENVELOPE_D1, &s.set);
	vmf_decoder *reader = nullptr;

	ASSERT_EQ(VMF_OK, vmf_decoder_create(&options, &reader, nullptr));
	ASSERT_EQ(VMF_OK, vmf_decoder_begin_pdu(reader));
	vmf_input input = {vector_d1, sizeof(vector_d1), 0, true};
	callback_log log = {VMF_EVENT_FIELD, VMF_EVENT_PAUSE, 0};
	size_t delivered = 999;

	EXPECT_EQ(VMF_PAUSED, vmf_decoder_dispatch(reader, &input, callback,
				      &log, &delivered));
	EXPECT_EQ(4u, delivered);
	vmf_event event = {};

	ASSERT_EQ(VMF_OK, vmf_decoder_next(reader, &input, &event));
	EXPECT_EQ(VMF_EVENT_GROUP_END, event.kind);
	EXPECT_EQ(VMF_DONE, finish(reader, &input));
	ASSERT_EQ(VMF_OK, vmf_decoder_begin_pdu(reader));
	input.used = 0;
	log.action = VMF_EVENT_ABORT;
	EXPECT_EQ(VMF_ABORTED, vmf_decoder_dispatch(reader, &input, callback,
				       &log, &delivered));
	EXPECT_EQ(4u, delivered);
	EXPECT_EQ(VMF_ABORTED, finish(reader, &input));
	EXPECT_EQ(VMF_INVALID_STATE, vmf_decoder_begin_pdu(reader));
	EXPECT_EQ(VMF_ABORTED, finish(reader, &input));
	vmf_error error = vmf_decoder_error(reader);

	EXPECT_EQ(112u, error.offset);
	EXPECT_EQ(2u, error.node_id);
	EXPECT_EQ(0u, error.message_index);
	vmf_decoder_reset(reader);
	ASSERT_EQ(VMF_OK, vmf_decoder_begin_pdu(reader));
	input.used = 0;
	EXPECT_EQ(VMF_DONE, finish(reader, &input));
	vmf_decoder_destroy(reader);
}

TEST(Streaming, EventBudgetIncludesMessageBoundaries)
{
	stream_schema s;

	initialize(&s);
	for (uint64_t limit : {4u, 5u}) {
		auto options = fixtures::options(VMF_ENVELOPE_D1, &s.set);
		vmf_decoder *reader = nullptr;

		options.limits.max_events_per_message = limit;
		ASSERT_EQ(
			VMF_OK, vmf_decoder_create(&options, &reader, nullptr));
		ASSERT_EQ(VMF_OK, vmf_decoder_begin_pdu(reader));
		vmf_input input = {vector_d1, sizeof(vector_d1), 0, true};

		EXPECT_EQ(limit == 5 ? VMF_DONE : VMF_LIMIT_EXCEEDED,
			finish(reader, &input));
		vmf_decoder_destroy(reader);
	}
}

TEST(Streaming, FailedBeginPreservesDecodedHeaderAndNextEvent)
{
	stream_schema s;

	initialize(&s);
	auto options = fixtures::options(VMF_ENVELOPE_D1, &s.set);
	vmf_decoder *reader = nullptr;

	ASSERT_EQ(VMF_OK, vmf_decoder_create(&options, &reader, nullptr));
	ASSERT_EQ(VMF_OK, vmf_decoder_begin_pdu(reader));
	vmf_input input = {vector_d1, sizeof(vector_d1), 0, true};
	vmf_event event = {};

	ASSERT_EQ(VMF_OK, vmf_decoder_next(reader, &input, &event));
	const auto *header =
		static_cast<const vmf_header_d1 *>(event.as.envelope.header);
	const char *name = header->fields.messages[0].file_name.value.data;

	EXPECT_EQ('x', *name);
	EXPECT_EQ(VMF_INVALID_STATE, vmf_decoder_begin_envelope(reader));
	EXPECT_EQ('x', *name);
	ASSERT_EQ(VMF_OK, vmf_decoder_next(reader, &input, &event));
	EXPECT_EQ(VMF_EVENT_MESSAGE_BEGIN, event.kind);
	EXPECT_EQ('x', *name);
	EXPECT_EQ(VMF_DONE, finish(reader, &input));
	vmf_decoder_destroy(reader);
}

TEST(Streaming, FieldAdapterRetriesOriginalOffsetAfterPartialProgress)
{
	stream_schema s;

	initialize(&s);
	s.field.as.field = {};
	s.field.as.field.kind = VMF_FIELD_OCTETS;
	auto *octets = &s.field.as.field.as.octets;

	octets->order = VMF_BIT_LSB_FIRST;
	octets->length.kind = VMF_EXTENT_FIXED;
	octets->length.fixed = octets->length.minimum = octets->length.maximum =
		9;
	std::vector<uint8_t> body = {0, 1, 2, 3, 4, 5, 6, 7, 8};
	auto bytes = fixtures::packet(VMF_ENVELOPE_D1, body);
	auto options = fixtures::options(VMF_ENVELOPE_D1, &s.set);
	vmf_decoder *reader = nullptr;
	vmf_encoder *writer = nullptr;

	ASSERT_EQ(VMF_OK, vmf_decoder_create(&options, &reader, nullptr));
	ASSERT_EQ(
		VMF_OK, vmf_encoder_create(&options, false, &writer, nullptr));
	ASSERT_EQ(VMF_OK, vmf_decoder_begin_pdu(reader));
	vmf_input input = {bytes.data(), bytes.size(), 0, true};
	std::vector<uint8_t> encoded;

	for (size_t i = 0; i < 20; ++i) {
		vmf_event event = {};
		vmf_status status = vmf_decoder_next(reader, &input, &event);

		if (status == VMF_DONE)
			break;
		ASSERT_EQ(VMF_OK, status);
		if (event.kind != VMF_EVENT_FIELD) {
			fixtures::write(writer, &event, &encoded);
			continue;
		}
		vmf_octets_field value = event.as.field.value.octets;
		uint8_t byte = 0;
		vmf_output output = {&byte, 1, 0};

		ASSERT_EQ(VMF_NEED_OUTPUT,
			vmf_encoder_write_field(writer, &output, &s.field,
				&value, sizeof(value), 9));
		ASSERT_EQ(1u, output.used);
		encoded.push_back(byte);
		vmf_octets_field changed = value;

		changed.size = 8;
		output.used = 0;
		EXPECT_EQ(VMF_INVALID_ARGUMENT,
			vmf_encoder_write_field(writer, &output, &s.field,
				&changed, sizeof(changed), 9));
		EXPECT_EQ(0u, output.used);
		for (size_t retry = 0; retry < 20; ++retry) {
			output.used = 0;
			status = vmf_encoder_write_field(writer, &output,
				&s.field, &value, sizeof(value), 9);
			if (output.used)
				encoded.push_back(byte);
			if (status != VMF_NEED_OUTPUT)
				break;
		}
		ASSERT_EQ(VMF_OK, status);
	}
	EXPECT_EQ(bytes, encoded);
	EXPECT_EQ(bytes.size() * 8, vmf_encoder_bit_count(writer));
	vmf_encoder_destroy(writer);
	vmf_decoder_destroy(reader);
}

static void check_known_recurrence(
	bool counting, bool more_bit, uint64_t count, int metadata)
{
	SCOPED_TRACE(counting);
	SCOPED_TRACE(more_bit);
	SCOPED_TRACE(count);
	SCOPED_TRACE(metadata);
	stream_schema s;

	initialize(&s);
	vmf_node value = {}, body = {};

	fixtures::scalar(&value, 4, 4);
	fixtures::group(&body, 3, &value, 1);
	s.field = {};
	s.field.id = 2;
	s.field.kind = VMF_NODE_REPEAT;
	s.field.as.repeat.body = &body;
	auto *extent = &s.field.as.repeat.count;

	extent->kind = VMF_EXTENT_RECURRENCE_BEFORE;
	extent->more_bit = more_bit;
	extent->minimum = 1;
	extent->maximum = 5;
	std::vector<uint8_t> payload = count == 1
		? std::vector<uint8_t>{(uint8_t)(more_bit ? 6 : 7)}
		: std::vector<uint8_t>{(uint8_t)(more_bit ? 7 : 0x26), 3};
	auto bytes = fixtures::packet(VMF_ENVELOPE_D1, payload);
	auto options = fixtures::options(VMF_ENVELOPE_D1, &s.set);
	vmf_decoder *reader = nullptr;
	vmf_encoder *writer = nullptr;

	ASSERT_EQ(VMF_OK, vmf_decoder_create(&options, &reader, nullptr));
	ASSERT_EQ(VMF_OK,
		vmf_encoder_create(&options, counting, &writer, nullptr));
	ASSERT_EQ(VMF_OK, vmf_decoder_begin_pdu(reader));
	vmf_input input = {bytes.data(), bytes.size(), 0, true};
	std::vector<uint8_t> encoded;
	vmf_status status = VMF_OK;

	for (size_t i = 0; i < 30; ++i) {
		vmf_event event = {};

		status = vmf_decoder_next(reader, &input, &event);
		if (status == VMF_DONE)
			break;
		ASSERT_EQ(VMF_OK, status);
		if (event.kind == VMF_EVENT_GROUP_BEGIN &&
			event.node == &s.field)
			event.as.group.count = count;
		bool occurrence = event.kind == VMF_EVENT_GROUP_BEGIN &&
			event.node == &body;
		bool conflict = metadata == 2 ? event.occurrence == 0
			: metadata == 3 && event.occurrence == count - 1;

		if (occurrence) {
			event.as.group.more_present = metadata != 0;
			if (conflict)
				event.as.group.more = !event.as.group.more;
		}
		do {
			uint8_t byte = 0;
			vmf_output output = {&byte, 1, 0};

			status = vmf_encoder_write(writer,
				counting ? nullptr : &output, &event);
			if (output.used)
				encoded.push_back(byte);
		} while (status == VMF_NEED_OUTPUT);
		if (occurrence && conflict) {
			EXPECT_EQ(VMF_INVALID_STATE, status);
			break;
		}
		EXPECT_EQ(VMF_OK, status);
		if (status != VMF_OK)
			break;
	}
	if (metadata < 2) {
		EXPECT_EQ(VMF_DONE, status);
		EXPECT_EQ(bytes.size() * 8,
			vmf_encoder_bit_count(writer));
		if (!counting)
			EXPECT_EQ(bytes, encoded);
	}
	vmf_encoder_destroy(writer);
	vmf_decoder_destroy(reader);
}

TEST(Streaming, KnownRecurrenceCountsDeriveAndValidateContinuationFlags)
{
	for (bool counting : {false, true})
		for (bool more_bit : {false, true})
			for (uint64_t count : {1u, 2u})
				for (int metadata = 0; metadata < 4; ++metadata)
					check_known_recurrence(counting, more_bit,
						count, metadata);
}

TEST(Streaming, StandardBodyBoundStillAppliesWhenCallerRaisesLimits)
{
	stream_schema s;

	initialize(&s, 0);
	auto options = fixtures::options(VMF_ENVELOPE_D1, &s.set);

	options.limits.max_message_octets = 2097152;
	vmf_decoder *reader = nullptr;
	vmf_encoder *writer = nullptr;

	ASSERT_EQ(VMF_OK, vmf_decoder_create(&options, &reader, nullptr));
	ASSERT_EQ(VMF_OK,
		vmf_encoder_create(&options, true, &writer, nullptr));
	ASSERT_EQ(VMF_OK, vmf_decoder_begin_envelope(reader));
	vmf_input input = {vector_missing, sizeof(vector_missing), 0, true};
	vmf_event event = {};

	ASSERT_EQ(VMF_OK, vmf_decoder_next(reader, &input, &event));
	ASSERT_EQ(VMF_OK, vmf_encoder_write(writer, nullptr, &event));
	event = {};
	event.kind = VMF_EVENT_MESSAGE_BEGIN;
	event.as.message.payload_octets = 1048576;
	EXPECT_EQ(VMF_LIMIT_EXCEEDED,
		vmf_encoder_write(writer, nullptr, &event));
	vmf_encoder_destroy(writer);
	vmf_decoder_destroy(reader);
}

} /* namespace */
