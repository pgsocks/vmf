#include <gtest/gtest.h>
#include <vmf/wire.h>
#include "fixtures.hpp"
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{

struct allocation_log {
	size_t attempts = 0;
	size_t live = 0;
	size_t fail_at = SIZE_MAX;
	bool deny = false;
};

void *allocate(void *user, size_t size)
{
	auto *log = static_cast<allocation_log *>(user);
	size_t attempt = log->attempts++;

	if (log->deny || attempt == log->fail_at)
		return nullptr;
	void *memory = std::malloc(size);

	if (memory)
		++log->live;
	return memory;
}

void deallocate(void *user, void *memory)
{
	auto *log = static_cast<allocation_log *>(user);

	EXPECT_NE(nullptr, memory);
	EXPECT_GT(log->live, 0u);
	--log->live;
	std::free(memory);
}

struct state_fixture {
	uint32_t address = 1;
	vmf_index_schema index = {};
	vmf_repeat_component component = {};
	vmf_repeat_definition repeat = {};
	vmf_message_schema message = {};
	vmf_schema_set schemas = {};
	vmf_decoder *reader = nullptr;
	vmf_encoder *writer = nullptr;
	vmf_encoder *probe = nullptr;
	vmf_envelope_event envelope = {};
	std::vector<uint8_t> bytes;

	~state_fixture()
	{
		vmf_encoder_destroy(probe);
		vmf_encoder_destroy(writer);
		vmf_decoder_destroy(reader);
	}
};

void initialize(state_fixture *f, bool counting = false,
	const vmf_allocator *allocator = nullptr,
	const vmf_limits *limits = nullptr, uint64_t width = 8,
	bool repeated = false)
{
	f->index.index_number = {&f->address, 1};
	f->index.number_of_bits = width;
	if (width > 64)
		f->index.encode_type = VMF_ENCODE_OCTETS;
	f->message.message_number = {1, 1};
	f->message.indexes = &f->index;
	f->message.index_count = 1;
	if (repeated) {
		f->component.id = 17;
		f->repeat.id = 17;
		f->repeat.count = 1;
		f->index.repeat_code = {&f->component, 1};
		f->message.repeats = &f->repeat;
		f->message.repeat_count = 1;
	}
	f->schemas = {VMF_ENVELOPE_D1, &f->message, 1, {}};
	f->schemas.standard.kind = VMF_STANDARD_RAW_CODE;
	f->schemas.standard.as.raw_code = 0;
	f->bytes = fixtures::packet(VMF_ENVELOPE_D1,
		std::vector<uint8_t>(width / 8, 0x5a));
	auto options = vmf_options_for_schemas(&f->schemas);

	ASSERT_EQ(VMF_OK,
		vmf_decoder_create(&options, &f->reader, nullptr));
	ASSERT_EQ(VMF_OK, vmf_decoder_begin_envelope(f->reader));
	vmf_input input = {f->bytes.data(), f->bytes.size(), 0, true};
	vmf_event event = {};

	ASSERT_EQ(VMF_OK, vmf_decoder_next(f->reader, &input, &event));
	f->envelope = event.as.envelope;
	options.allocator = allocator;
	if (limits)
		options.limits = *limits;
	ASSERT_EQ(VMF_OK,
		vmf_encoder_create(&options, counting, &f->writer, nullptr));
}

vmf_event_action provide_scalar(void *user,
	const vmf_encode_request *request, vmf_encode_reply *reply)
{
	EXPECT_EQ(VMF_ENCODE_REQUEST_FIELD, request->kind);
	reply->kind = VMF_ENCODE_REQUEST_FIELD;
	reply->as.field.unsigned_integer.value =
		*static_cast<const uint64_t *>(user);
	return VMF_EVENT_CONTINUE;
}

vmf_status finish(vmf_encoder *writer, bool counting,
	std::vector<uint8_t> *bytes = nullptr)
{
	uint64_t value = 0x5a;

	for (size_t attempt = 0; attempt < 128; ++attempt) {
		uint8_t block[3] = {};
		vmf_output output = {block, sizeof(block), 0};
		size_t delivered = 999;
		auto status = vmf_encoder_dispatch(writer,
			counting ? nullptr : &output, provide_scalar, &value,
			&delivered);

		if (bytes)
			bytes->insert(bytes->end(), block, block + output.used);
		if (status != VMF_NEED_OUTPUT)
			return status;
	}
	ADD_FAILURE() << "managed state fixture did not complete";
	return VMF_INVALID_STATE;
}

TEST(ManagedState, EveryCreationAllocationRollsBackAndRecovers)
{
	allocation_log log;
	vmf_allocator allocator = {allocate, deallocate, &log};
	state_fixture f;

	initialize(&f, false, &allocator);
	ASSERT_NE(nullptr, f.writer);
	size_t attempts = log.attempts;

	vmf_encoder_destroy(f.writer);
	f.writer = nullptr;
	ASSERT_EQ(0u, log.live);
	ASSERT_GT(attempts, 2u);
	auto options = vmf_options_for_schemas(&f.schemas);

	options.allocator = &allocator;
	for (size_t failure = 0; failure < attempts; ++failure) {
		SCOPED_TRACE(failure);
		log.attempts = 0;
		log.fail_at = failure;
		vmf_error error = {};

		EXPECT_EQ(VMF_OUT_OF_MEMORY, vmf_encoder_create(
			&options, false, &f.writer, &error));
		EXPECT_EQ(VMF_OUT_OF_MEMORY, error.status);
		EXPECT_EQ(nullptr, f.writer);
		EXPECT_EQ(0u, log.live);
	}
	log.fail_at = SIZE_MAX;
	ASSERT_EQ(VMF_OK,
		vmf_encoder_create(&options, false, &f.writer, nullptr));
	ASSERT_EQ(VMF_OK, vmf_encoder_begin_pdu(f.writer, &f.envelope));
	std::vector<uint8_t> encoded;

	EXPECT_EQ(VMF_DONE, finish(f.writer, false, &encoded));
	EXPECT_EQ(f.bytes, encoded);
}

TEST(ManagedState, PrimitivesDispatchCountingAndResetAllocateNothing)
{
	allocation_log log;
	vmf_allocator allocator = {allocate, deallocate, &log};
	state_fixture f;

	initialize(&f, false, &allocator);
	ASSERT_NE(nullptr, f.writer);
	auto options = vmf_options_for_schemas(&f.schemas);

	options.allocator = &allocator;
	ASSERT_EQ(VMF_OK,
		vmf_encoder_create(&options, true, &f.probe, nullptr));
	size_t attempts = log.attempts;

	log.deny = true;
	ASSERT_EQ(VMF_OK, vmf_encoder_begin_pdu(f.probe, &f.envelope));
	ASSERT_EQ(VMF_DONE, finish(f.probe, true));
	ASSERT_EQ(VMF_OK, vmf_encoder_begin_pdu(f.writer, &f.envelope));
	vmf_output empty = {nullptr, 0, 0};
	vmf_encode_request request = {};

	EXPECT_EQ(VMF_NEED_OUTPUT,
		vmf_encoder_next_request(f.writer, &empty, &request));
	EXPECT_EQ(nullptr, request.schema);
	uint8_t block[128] = {};
	vmf_output output = {block, sizeof(block), 0};

	ASSERT_EQ(VMF_OK,
		vmf_encoder_next_request(f.writer, &output, &request));
	vmf_encode_reply reply = {};

	reply.kind = VMF_ENCODE_REQUEST_FIELD;
	reply.as.field.unsigned_integer.value = 0x5a;
	ASSERT_EQ(VMF_OK, vmf_encoder_respond(f.writer, &output, &reply));
	ASSERT_EQ(VMF_DONE,
		vmf_encoder_next_request(f.writer, &output, &request));
	EXPECT_EQ(f.bytes,
		std::vector<uint8_t>(block, block + output.used));
	vmf_encoder_reset(f.writer);
	vmf_encoder_reset(f.probe);
	EXPECT_EQ(0u, vmf_encoder_bit_count(f.writer));
	EXPECT_EQ(0u, vmf_encoder_bit_count(f.probe));
	ASSERT_EQ(VMF_OK, vmf_encoder_begin_pdu(f.writer, &f.envelope));
	EXPECT_EQ(VMF_DONE, finish(f.writer, false));
	EXPECT_EQ(attempts, log.attempts);
}

TEST(ManagedState, FailedHeaderCopyRetainsCompletedMeasurements)
{
	state_fixture f;
	auto limits = vmf_default_limits();

	limits.max_header_bytes = sizeof(vmf_header_d1) +
		sizeof(vmf_message_header) + 128;
	initialize(&f, true, nullptr, &limits);
	ASSERT_NE(nullptr, f.writer);
	ASSERT_EQ(VMF_OK, vmf_encoder_begin_pdu(f.writer, &f.envelope));
	ASSERT_EQ(VMF_DONE, finish(f.writer, true));
	vmf_measurement measurement = {};

	ASSERT_EQ(VMF_OK,
		vmf_encoder_measurement(f.writer, 0, &measurement));
	uint64_t bits = vmf_encoder_bit_count(f.writer);
	vmf_header_d1 candidate =
		*static_cast<const vmf_header_d1 *>(f.envelope.header);
	vmf_message_header descriptors[16] = {};

	for (auto &descriptor : descriptors)
		descriptor = candidate.fields.messages[0];
	candidate.fields.messages = descriptors;
	candidate.fields.message_count = 16;
	vmf_envelope_event envelope = f.envelope;

	envelope.header = &candidate;
	/* The wire fits, but the copied candidate exceeds the arena. */
	auto options = vmf_options_for_schemas(&f.schemas);

	ASSERT_EQ(VMF_OK,
		vmf_encoder_create(&options, true, &f.probe, nullptr));
	ASSERT_EQ(VMF_OK, vmf_encoder_begin_pdu(f.probe, &envelope));
	vmf_encode_request request = {};

	ASSERT_EQ(VMF_OK,
		vmf_encoder_next_request(f.probe, nullptr, &request));
	ASSERT_LT(vmf_encoder_bit_count(f.probe) / 8,
		limits.max_header_bytes);
	ASSERT_GT(sizeof(descriptors), limits.max_header_bytes);
	EXPECT_EQ(VMF_LIMIT_EXCEEDED,
		vmf_encoder_begin_pdu(f.writer, &envelope));
	vmf_measurement retained = {};

	ASSERT_EQ(VMF_OK,
		vmf_encoder_measurement(f.writer, 0, &retained));
	EXPECT_EQ(measurement.header_octets, retained.header_octets);
	EXPECT_EQ(measurement.body_octets, retained.body_octets);
	EXPECT_EQ(measurement.pdu_octets, retained.pdu_octets);
	EXPECT_EQ(bits, vmf_encoder_bit_count(f.writer));
	EXPECT_EQ(VMF_DONE,
		vmf_encoder_next_request(f.writer, nullptr, &request));
	EXPECT_EQ(nullptr, request.schema);
	ASSERT_EQ(VMF_OK, vmf_encoder_begin_pdu(f.writer, &f.envelope));
	EXPECT_EQ(VMF_INVALID_STATE,
		vmf_encoder_measurement(f.writer, 0, &retained));
	EXPECT_EQ(0u, retained.header_octets);
	EXPECT_EQ(0u, retained.body_octets);
	EXPECT_EQ(0u, retained.pdu_octets);
	EXPECT_EQ(bits, vmf_encoder_bit_count(f.writer));
	EXPECT_EQ(VMF_DONE, finish(f.writer, true));
	EXPECT_EQ(bits * 2, vmf_encoder_bit_count(f.writer));
}

TEST(ManagedState, InvalidDataIsTerminalUntilReset)
{
	state_fixture f;

	initialize(&f, true);
	ASSERT_NE(nullptr, f.writer);
	ASSERT_EQ(VMF_OK, vmf_encoder_begin_pdu(f.writer, &f.envelope));
	vmf_encode_request request = {};

	ASSERT_EQ(VMF_OK,
		vmf_encoder_next_request(f.writer, nullptr, &request));
	vmf_encode_reply reply = {};

	reply.kind = VMF_ENCODE_REQUEST_FIELD;
	reply.as.field.unsigned_integer.value = 256;
	ASSERT_EQ(VMF_OUT_OF_RANGE,
		vmf_encoder_respond(f.writer, nullptr, &reply));
	vmf_error error = vmf_encoder_error(f.writer);

	EXPECT_EQ(VMF_OUT_OF_RANGE, error.status);
	EXPECT_EQ(0u, error.schema_index);
	EXPECT_EQ(0u, error.index_position);
	EXPECT_EQ(0u, error.message_index);
	EXPECT_EQ(VMF_OUT_OF_RANGE,
		vmf_encoder_next_request(f.writer, nullptr, &request));
	EXPECT_EQ(nullptr, request.schema);
	reply.as.field.unsigned_integer.value = 0x5a;
	EXPECT_EQ(VMF_OUT_OF_RANGE,
		vmf_encoder_respond(f.writer, nullptr, &reply));
	EXPECT_EQ(VMF_INVALID_STATE,
		vmf_encoder_begin_pdu(f.writer, &f.envelope));
	EXPECT_EQ(VMF_OUT_OF_RANGE, vmf_encoder_error(f.writer).status);
	vmf_encoder_reset(f.writer);
	EXPECT_EQ(VMF_OK, vmf_encoder_error(f.writer).status);
	EXPECT_EQ(0u, vmf_encoder_bit_count(f.writer));
	ASSERT_EQ(VMF_OK, vmf_encoder_begin_pdu(f.writer, &f.envelope));
	EXPECT_EQ(VMF_DONE, finish(f.writer, true));
}

TEST(ManagedState, SuccessfulBeginClearsPreviousSchemaContext)
{
	state_fixture f;

	initialize(&f, true);
	ASSERT_NE(nullptr, f.writer);
	ASSERT_EQ(VMF_OK, vmf_encoder_begin_pdu(f.writer, &f.envelope));
	ASSERT_EQ(VMF_DONE, finish(f.writer, true));
	vmf_header_d1 header =
		*static_cast<const vmf_header_d1 *>(f.envelope.header);

	header.fields.compression = {true, 0};
	vmf_envelope_event envelope = f.envelope;

	envelope.header = &header;
	ASSERT_EQ(VMF_OK, vmf_encoder_begin_pdu(f.writer, &envelope));
	vmf_encode_request request = {};

	EXPECT_EQ(VMF_UNSUPPORTED_ENVELOPE_FEATURE,
		vmf_encoder_next_request(f.writer, nullptr, &request));
	vmf_error error = vmf_encoder_error(f.writer);

	EXPECT_EQ(VMF_UNSUPPORTED_ENVELOPE_FEATURE, error.status);
	EXPECT_EQ(SIZE_MAX, error.schema_index);
	EXPECT_EQ(SIZE_MAX, error.index_position);
	EXPECT_EQ(nullptr, request.schema);
}

void finish_manual(state_fixture *f)
{
	vmf_event event = {};
	const auto *header =
		static_cast<const vmf_header_d1 *>(f->envelope.header);

	event.kind = VMF_EVENT_MESSAGE_BEGIN;
	event.as.message = {header->fields.messages[0].id, 0, 1};
	ASSERT_EQ(VMF_OK, vmf_encoder_write(f->writer, nullptr, &event));
	event = {};
	event.kind = VMF_EVENT_GROUP_BEGIN;
	event.scope = {VMF_SCOPE_ROOT, 0};
	event.as.group.count = 1;
	ASSERT_EQ(VMF_OK, vmf_encoder_write(f->writer, nullptr, &event));
	vmf_unsigned_field value = {0x5a};

	ASSERT_EQ(VMF_OK, vmf_encoder_write_field(f->writer, nullptr,
		&f->index, &value, sizeof(value), 1));
	event.kind = VMF_EVENT_GROUP_END;
	ASSERT_EQ(VMF_OK, vmf_encoder_write(f->writer, nullptr, &event));
	event = {};
	event.kind = VMF_EVENT_MESSAGE_END;
	event.as.message = {header->fields.messages[0].id, 0, 1};
	ASSERT_EQ(VMF_OK, vmf_encoder_write(f->writer, nullptr, &event));
	event = {};
	event.kind = VMF_EVENT_PDU_END;
	ASSERT_EQ(VMF_OK, vmf_encoder_write(f->writer, nullptr, &event));
}

TEST(ManagedState, ModesSwitchOnlyAfterPduCompletion)
{
	state_fixture f;

	initialize(&f, true);
	ASSERT_NE(nullptr, f.writer);
	ASSERT_EQ(VMF_OK, vmf_encoder_begin_pdu(f.writer, &f.envelope));
	vmf_event event = {};

	event.kind = VMF_EVENT_ENVELOPE;
	event.as.envelope = f.envelope;
	EXPECT_EQ(VMF_INVALID_STATE,
		vmf_encoder_write(f.writer, nullptr, &event));
	vmf_unsigned_field value = {0x5a};

	EXPECT_EQ(VMF_INVALID_STATE, vmf_encoder_write_field(f.writer,
		nullptr, &f.index, &value, sizeof(value), 1));
	EXPECT_EQ(VMF_INVALID_STATE, vmf_encoder_begin_envelope(f.writer));
	ASSERT_EQ(VMF_DONE, finish(f.writer, true));
	ASSERT_EQ(VMF_OK, vmf_encoder_write(f.writer, nullptr, &event));
	vmf_encode_request request = {};

	EXPECT_EQ(VMF_INVALID_STATE,
		vmf_encoder_next_request(f.writer, nullptr, &request));
	EXPECT_EQ(nullptr, request.schema);
	EXPECT_EQ(VMF_INVALID_STATE,
		vmf_encoder_begin_pdu(f.writer, &f.envelope));
	finish_manual(&f);
	ASSERT_EQ(VMF_OK, vmf_encoder_begin_pdu(f.writer, &f.envelope));
	EXPECT_EQ(VMF_DONE, finish(f.writer, true));
	EXPECT_EQ(f.bytes.size() * 24, vmf_encoder_bit_count(f.writer));
}

TEST(ManagedState, ModeMistakesPreserveManualOutputRetry)
{
	state_fixture f;

	initialize(&f);
	ASSERT_NE(nullptr, f.writer);
	vmf_event event = {};

	event.kind = VMF_EVENT_ENVELOPE;
	event.as.envelope = f.envelope;
	vmf_output output = {nullptr, 0, 0};

	ASSERT_EQ(VMF_NEED_OUTPUT,
		vmf_encoder_write(f.writer, &output, &event));
	EXPECT_EQ(VMF_INVALID_STATE,
		vmf_encoder_begin_pdu(f.writer, &f.envelope));
	vmf_encode_request request = {};

	EXPECT_EQ(VMF_INVALID_STATE,
		vmf_encoder_next_request(f.writer, &output, &request));
	EXPECT_EQ(nullptr, request.schema);
	uint8_t block[128] = {};

	output = {block, sizeof(block), 0};
	EXPECT_EQ(VMF_OK, vmf_encoder_write(f.writer, &output, &event));
	EXPECT_EQ(f.envelope.header_octets, output.used);
	EXPECT_EQ(0, std::memcmp(block, f.bytes.data(), output.used));
	vmf_encoder_reset(f.writer);
	EXPECT_EQ(0u, vmf_encoder_bit_count(f.writer));
	ASSERT_EQ(VMF_OK, vmf_encoder_begin_pdu(f.writer, &f.envelope));
	EXPECT_EQ(VMF_DONE, finish(f.writer, false));
}

vmf_header_fields *header_fields(vmf_header *header,
	vmf_envelope_version profile)
{
	switch (profile) {
	case VMF_ENVELOPE_B:
		return &header->b.fields;
	case VMF_ENVELOPE_C:
		return &header->c.fields;
	case VMF_ENVELOPE_D:
		return &header->d.fields;
	case VMF_ENVELOPE_D1:
		return &header->d1.fields;
	}
	return nullptr;
}

void response_descriptor(vmf_message_header *message,
	vmf_envelope_version profile)
{
	*message = {};
	message->precedence = profile <= VMF_ENVELOPE_C ? 7 : 0;
	message->message_size_octets = {true, 0};
	message->response.present = true;
	message->response.acknowledged =
		{true, 26, 10, 15, 12, 30, 59, {true, 7}};
	message->response.receipt_compliance = 1;
}

vmf_event_action unexpected_provider(void *user,
	const vmf_encode_request *, vmf_encode_reply *)
{
	++*static_cast<size_t *>(user);
	ADD_FAILURE() << "response-only descriptor requested application input";
	return VMF_EVENT_ABORT;
}

TEST(ManagedState, ResponseOnlyPdusNeedNoSchemasOrApplicationReplies)
{
	for (unsigned revision = 1; revision <= 4; ++revision) {
		SCOPED_TRACE(revision);
		auto profile = static_cast<vmf_envelope_version>(revision);
		vmf_header header = {};
		auto *fields = header_fields(&header, profile);
		vmf_message_header message = {};
		vmf_address recipient = {true, {true, 2}, {}};

		fields->originator = {true, {true, 1}, {}};
		fields->recipients = &recipient;
		fields->recipient_count = 1;
		fields->messages = &message;
		fields->message_count = 1;
		response_descriptor(&message, profile);
		vmf_envelope_event envelope = {
			static_cast<uint8_t>(profile), &header,
			vmf_header_layout(profile).size, 0, VMF_LENGTH_UNKNOWN};
		auto options = fixtures::options(profile);
		state_fixture f;

		ASSERT_EQ(VMF_OK, vmf_encoder_create(
			&options, false, &f.writer, nullptr));
		ASSERT_EQ(VMF_OK, vmf_encoder_begin_pdu(f.writer, &envelope));
		size_t calls = 0;
		size_t delivered = 999;
		vmf_output empty = {nullptr, 0, 0};

		EXPECT_EQ(VMF_NEED_OUTPUT,
			vmf_encoder_dispatch(f.writer, &empty,
				unexpected_provider, &calls, &delivered));
		EXPECT_EQ(0u, delivered);
		std::vector<uint8_t> bytes;
		vmf_status status = VMF_NEED_OUTPUT;

		for (size_t attempt = 0; attempt < 128; ++attempt) {
			uint8_t octet = 0;
			vmf_output output = {&octet, 1, 0};

			status = vmf_encoder_dispatch(f.writer, &output,
				unexpected_provider, &calls, &delivered);
			EXPECT_EQ(0u, delivered);
			if (output.used)
				bytes.push_back(octet);
			if (status != VMF_NEED_OUTPUT)
				break;
		}
		ASSERT_EQ(VMF_DONE, status);
		EXPECT_EQ(0u, calls);
		vmf_measurement measurement = {};

		ASSERT_EQ(VMF_OK,
			vmf_encoder_measurement(f.writer, 0, &measurement));
		EXPECT_EQ(bytes.size(), measurement.header_octets);
		EXPECT_EQ(0u, measurement.body_octets);
		EXPECT_EQ(bytes.size(), measurement.pdu_octets);
		ASSERT_EQ(VMF_OK,
			vmf_decoder_create(&options, &f.reader, nullptr));
		ASSERT_EQ(VMF_OK, vmf_decoder_begin_pdu(f.reader));
		vmf_input input = {bytes.data(), bytes.size(), 0, true};
		vmf_event event = {};

		ASSERT_EQ(VMF_OK, vmf_decoder_next(f.reader, &input, &event));
		EXPECT_EQ(VMF_EVENT_ENVELOPE, event.kind);
		ASSERT_EQ(VMF_OK, vmf_decoder_next(f.reader, &input, &event));
		EXPECT_EQ(VMF_EVENT_PDU_END, event.kind);
		EXPECT_EQ(VMF_DONE,
			vmf_decoder_next(f.reader, &input, &event));
		EXPECT_EQ(bytes.size(), input.used);
	}
}

TEST(ManagedState, MixedDescriptorsPreserveOriginalIndexesAndMeasurements)
{
	state_fixture f;

	initialize(&f, true);
	ASSERT_NE(nullptr, f.writer);
	vmf_header_d1 header =
		*static_cast<const vmf_header_d1 *>(f.envelope.header);
	vmf_message_header descriptors[3] = {};
	vmf_address recipient = {true, {true, 2}, {}};

	header.fields.originator = {true, {true, 1}, {}};
	header.fields.recipients = &recipient;
	header.fields.recipient_count = 1;
	descriptors[1] = header.fields.messages[0];
	response_descriptor(&descriptors[0], VMF_ENVELOPE_D1);
	response_descriptor(&descriptors[2], VMF_ENVELOPE_D1);
	header.fields.messages = descriptors;
	header.fields.message_count = 3;
	vmf_envelope_event envelope = f.envelope;

	envelope.header = &header;
	ASSERT_EQ(VMF_OK, vmf_encoder_begin_pdu(f.writer, &envelope));
	vmf_encode_request request = {};

	ASSERT_EQ(VMF_OK,
		vmf_encoder_next_request(f.writer, nullptr, &request));
	EXPECT_EQ(1u, request.descriptor_index);
	EXPECT_EQ(&f.message, request.schema);
	EXPECT_EQ(&f.index, request.index);
	vmf_measurement measurement = {};

	ASSERT_EQ(VMF_OK,
		vmf_encoder_measurement(f.writer, 0, &measurement));
	EXPECT_EQ(0u, measurement.body_octets);
	EXPECT_EQ(0u, measurement.pdu_octets);
	EXPECT_EQ(VMF_INVALID_STATE,
		vmf_encoder_measurement(f.writer, 1, &measurement));
	vmf_encode_reply reply = {};

	reply.kind = VMF_ENCODE_REQUEST_FIELD;
	reply.as.field.unsigned_integer.value = 0x5a;
	ASSERT_EQ(VMF_OK, vmf_encoder_respond(f.writer, nullptr, &reply));
	ASSERT_EQ(VMF_DONE,
		vmf_encoder_next_request(f.writer, nullptr, &request));
	uint64_t total = 0;

	for (size_t descriptor = 0; descriptor < 3; ++descriptor) {
		SCOPED_TRACE(descriptor);
		ASSERT_EQ(VMF_OK, vmf_encoder_measurement(
			f.writer, descriptor, &measurement));
		EXPECT_EQ(descriptor == 1 ? 1u : 0u, measurement.body_octets);
		EXPECT_EQ(measurement.header_octets + 1,
			measurement.pdu_octets);
		total = measurement.pdu_octets;
	}
	header.fields.header_size_octets.value =
		static_cast<uint32_t>(measurement.header_octets);
	descriptors[1].message_size_octets.value = 1;
	auto options = vmf_options_for_schemas(&f.schemas);

	ASSERT_EQ(VMF_OK,
		vmf_encoder_create(&options, false, &f.probe, nullptr));
	ASSERT_EQ(VMF_OK, vmf_encoder_begin_pdu(f.probe, &envelope));
	std::vector<uint8_t> bytes;

	ASSERT_EQ(VMF_DONE, finish(f.probe, false, &bytes));
	ASSERT_EQ(total, bytes.size());
	EXPECT_EQ(0x5a, bytes.back());
	ASSERT_EQ(VMF_OK, vmf_decoder_begin_pdu(f.reader));
	vmf_input input = {bytes.data(), bytes.size(), 0, true};
	size_t body_count = 0;
	vmf_status status = VMF_OK;

	for (size_t attempt = 0; attempt < 16; ++attempt) {
		vmf_event event = {};

		status = vmf_decoder_next(f.reader, &input, &event);
		if (status != VMF_OK)
			break;
		if (event.kind == VMF_EVENT_MESSAGE_BEGIN) {
			EXPECT_EQ(1u, event.as.message.index);
			++body_count;
		}
	}
	EXPECT_EQ(VMF_DONE, status);
	EXPECT_EQ(1u, body_count);
	EXPECT_EQ(bytes.size(), input.used);
}

struct borrowed_request_state {
	const vmf_encode_request *request = nullptr;
	vmf_encode_request snapshot = {};
	vmf_repeat_occurrence path = {};
	uint8_t data[9] = {};
	size_t calls = 0;
};

vmf_event_action retain_request(void *user, const vmf_encode_request *request,
	vmf_encode_reply *reply)
{
	auto *state = static_cast<borrowed_request_state *>(user);

	++state->calls;
	state->request = request;
	state->snapshot = *request;
	EXPECT_EQ(1u, request->path_count);
	if (request->path_count == 1)
		state->path = request->path[0];
	reply->kind = VMF_ENCODE_REQUEST_FIELD;
	reply->as.field.octets = {state->data, sizeof(state->data), true};
	return VMF_EVENT_CONTINUE;
}

void expect_borrowed_request(const borrowed_request_state *state)
{
	ASSERT_NE(nullptr, state->request);
	const auto *request = state->request;

	EXPECT_EQ(state->snapshot.kind, request->kind);
	EXPECT_EQ(state->snapshot.descriptor_index, request->descriptor_index);
	EXPECT_EQ(state->snapshot.schema, request->schema);
	EXPECT_EQ(state->snapshot.index, request->index);
	EXPECT_EQ(state->snapshot.field_kind, request->field_kind);
	EXPECT_EQ(state->snapshot.offset_units, request->offset_units);
	EXPECT_EQ(state->snapshot.total_units, request->total_units);
	EXPECT_EQ(state->snapshot.path, request->path);
	ASSERT_EQ(1u, request->path_count);
	EXPECT_EQ(state->path.id, request->path[0].id);
	EXPECT_EQ(state->path.occurrence, request->path[0].occurrence);
}

TEST(ManagedState, ProviderRequestStorageSurvivesPendingOutputRetries)
{
	state_fixture f;

	initialize(&f, false, nullptr, nullptr, 72, true);
	ASSERT_NE(nullptr, f.writer);
	ASSERT_EQ(VMF_OK, vmf_encoder_begin_pdu(f.writer, &f.envelope));
	borrowed_request_state state;

	std::memset(state.data, 0x5a, sizeof(state.data));
	uint8_t block[128] = {};
	vmf_output output = {
		block, static_cast<size_t>(f.envelope.header_octets) + 1, 0};
	size_t delivered = 999;

	ASSERT_EQ(VMF_NEED_OUTPUT, vmf_encoder_dispatch(f.writer, &output,
		retain_request, &state, &delivered));
	EXPECT_EQ(1u, state.calls);
	EXPECT_EQ(1u, delivered);
	expect_borrowed_request(&state);
	std::vector<uint8_t> bytes(block, block + output.used);
	vmf_encode_request rejected = {};
	vmf_output empty = {nullptr, 0, 0};

	EXPECT_EQ(VMF_INVALID_STATE,
		vmf_encoder_next_request(f.writer, &empty, &rejected));
	EXPECT_EQ(nullptr, rejected.schema);
	expect_borrowed_request(&state);
	vmf_status status = VMF_NEED_OUTPUT;

	for (size_t attempt = 0; attempt < 16; ++attempt) {
		expect_borrowed_request(&state);
		uint8_t octet = 0;

		output = {&octet, 1, 0};
		status = vmf_encoder_dispatch(f.writer, &output,
			retain_request, &state, &delivered);
		EXPECT_EQ(0u, delivered);
		if (output.used)
			bytes.push_back(octet);
		if (status != VMF_NEED_OUTPUT)
			break;
	}
	EXPECT_EQ(VMF_DONE, status);
	EXPECT_EQ(1u, state.calls);
	EXPECT_EQ(f.bytes, bytes);
}

} /* namespace */
