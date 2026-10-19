#include <gtest/gtest.h>
#include <vmf/wire.h>
#include "fixtures.hpp"
#include "../acceptance/vectors.h"
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" int vmf_managed_c_consumer(uint8_t *, size_t, size_t *);
int vmf_managed_cpp_consumer(uint8_t *, size_t, size_t *);

namespace
{

struct managed_fixture {
	uint32_t address = 1;
	vmf_index_schema index = {};
	vmf_message_schema message = {};
	vmf_schema_set set = {};
	vmf_decoder *reader = nullptr;
	vmf_encoder *writer = nullptr;
	vmf_envelope_event envelope = {};

	~managed_fixture()
	{
		vmf_encoder_destroy(writer);
		vmf_decoder_destroy(reader);
	}
};

void initialize(managed_fixture *f, vmf_envelope_version profile,
	const std::vector<uint8_t> &bytes, bool counting = false)
{
	f->index.index_number = {&f->address, 1};
	f->index.number_of_bits = 8;
	f->message.message_number = {1, 1};
	f->message.indexes = &f->index;
	f->message.index_count = 1;
	f->set = {profile, &f->message, 1, {}};
	if (profile != VMF_ENVELOPE_B) {
		f->set.standard.kind = VMF_STANDARD_RAW_CODE;
		f->set.standard.as.raw_code = 0;
	}
	auto options = vmf_options_for_schemas(&f->set);

	ASSERT_EQ(VMF_OK,
		vmf_decoder_create(&options, &f->reader, nullptr));
	ASSERT_EQ(VMF_OK,
		vmf_encoder_create(&options, counting, &f->writer, nullptr));
	ASSERT_EQ(VMF_OK, vmf_decoder_begin_envelope(f->reader));
	vmf_input input = {bytes.data(), bytes.size(), 0, true};
	vmf_event event = {};

	ASSERT_EQ(VMF_OK, vmf_decoder_next(f->reader, &input, &event));
	ASSERT_EQ(VMF_EVENT_ENVELOPE, event.kind);
	f->envelope = event.as.envelope;
}

void recreate(managed_fixture *f, bool counting = false,
	const vmf_limits *limits = nullptr)
{
	vmf_encoder_destroy(f->writer);
	f->writer = nullptr;
	auto options = vmf_options_for_schemas(&f->set);

	if (limits)
		options.limits = *limits;
	ASSERT_EQ(VMF_OK,
		vmf_encoder_create(&options, counting, &f->writer, nullptr));
}

vmf_encode_reply scalar_reply(uint64_t value)
{
	vmf_encode_reply reply = {};

	reply.kind = VMF_ENCODE_REQUEST_FIELD;
	reply.as.field.unsigned_integer.value = value;
	return reply;
}

struct provider_state {
	std::vector<vmf_encode_reply> replies;
	std::vector<vmf_encode_request> requests;
	std::vector<std::vector<vmf_repeat_occurrence>> paths;
	vmf_event_action action = VMF_EVENT_CONTINUE;
	size_t calls = 0;
};

vmf_event_action provide(void *user, const vmf_encode_request *request,
	vmf_encode_reply *reply)
{
	auto *state = static_cast<provider_state *>(user);

	state->requests.push_back(*request);
	state->paths.emplace_back();
	if (request->path_count)
		state->paths.back().assign(request->path,
			request->path + request->path_count);
	if (state->calls >= state->replies.size()) {
		ADD_FAILURE() << "unexpected provider invocation";
		return VMF_EVENT_ABORT;
	}
	*reply = state->replies[state->calls++];
	return state->action;
}

vmf_event_action other_provider(void *user,
	const vmf_encode_request *request, vmf_encode_reply *reply)
{
	return provide(user, request, reply);
}

vmf_status dispatch_all(managed_fixture *f, provider_state *state,
	std::vector<uint8_t> *encoded, size_t capacity = 1,
	bool counting = false)
{
	for (size_t attempt = 0; attempt < 4096; ++attempt) {
		std::vector<uint8_t> block(capacity);
		vmf_output output = {block.data(), block.size(), 0};
		size_t delivered = 999;
		size_t before = state->calls;
		auto status = vmf_encoder_dispatch(f->writer,
			counting ? nullptr : &output, provide, state,
			&delivered);

		EXPECT_EQ(state->calls - before, delivered);
		encoded->insert(encoded->end(), block.begin(),
			block.begin() + output.used);
		if (status != VMF_NEED_OUTPUT)
			return status;
	}
	ADD_FAILURE() << "managed dispatch did not terminate";
	return VMF_INVALID_STATE;
}

TEST(ManagedEncoding, BeginCopiesHeaderWithoutProducingOutput)
{
	managed_fixture f;
	std::vector<uint8_t> bytes(vector_vmf_d1,
		vector_vmf_d1 + sizeof(vector_vmf_d1));

	initialize(&f, VMF_ENVELOPE_D1, bytes);
	ASSERT_NE(nullptr, f.writer);
	EXPECT_EQ(VMF_OK, vmf_encoder_begin_pdu(f.writer, &f.envelope));
	EXPECT_EQ(0u, vmf_encoder_bit_count(f.writer));
	uint8_t output_bytes[64] = {};
	vmf_output output = {output_bytes, sizeof(output_bytes), 0};
	vmf_encode_request request = {};

	ASSERT_EQ(VMF_OK,
		vmf_encoder_next_request(f.writer, &output, &request));
	EXPECT_EQ(VMF_ENCODE_REQUEST_FIELD, request.kind);
	EXPECT_EQ(&f.message, request.schema);
	EXPECT_EQ(&f.index, request.index);
	EXPECT_EQ(0u, request.descriptor_index);
	EXPECT_EQ(VMF_FIELD_UNSIGNED, request.field_kind);
	EXPECT_EQ(0u, request.offset_units);
	EXPECT_EQ(1u, request.total_units);
}

TEST(ManagedEncoding, PublicOnlyNestedC11AndCpp17ConsumersMeasureAndReplay)
{
	uint8_t c_bytes[128] = {};
	uint8_t cpp_bytes[128] = {};
	size_t c_size = 0;
	size_t cpp_size = 0;

	ASSERT_EQ(1, vmf_managed_c_consumer(c_bytes, sizeof(c_bytes), &c_size));
	ASSERT_EQ(1, vmf_managed_cpp_consumer(cpp_bytes, sizeof(cpp_bytes),
		&cpp_size));
	ASSERT_GT(c_size, 4u);
	EXPECT_EQ(c_size, cpp_size);
	EXPECT_EQ(0, std::memcmp(c_bytes, cpp_bytes, c_size));
	const uint8_t values[] = {0, 7, 11, 255};

	EXPECT_EQ(0, std::memcmp(c_bytes + c_size - 4, values, 4));
}

TEST(ManagedEncoding, LiteralProfilesMatchManualReplayAtEveryOutputSplit)
{
	for (unsigned revision = 1; revision <= 4; ++revision) {
		auto profile = static_cast<vmf_envelope_version>(revision);
		auto bytes = fixtures::packet(profile, {0x5a});
		managed_fixture reference;

		initialize(&reference, profile, bytes);
		vmf_decoder_reset(reference.reader);
		ASSERT_EQ(VMF_OK, vmf_decoder_begin_pdu(reference.reader));
		vmf_input input = {bytes.data(), bytes.size(), 0, true};
		std::vector<uint8_t> manual;

		for (size_t attempt = 0; attempt < 32; ++attempt) {
			vmf_event event = {};
			auto status = vmf_decoder_next(reference.reader,
				&input, &event);

			if (status == VMF_DONE)
				break;
			ASSERT_EQ(VMF_OK, status);
			fixtures::write(reference.writer, &event, &manual);
		}
		ASSERT_EQ(bytes, manual);

		for (size_t split = 0; split <= bytes.size(); ++split) {
			SCOPED_TRACE(revision);
			SCOPED_TRACE(split);
			managed_fixture f;

			initialize(&f, profile, bytes);
			ASSERT_NE(nullptr, f.writer);
			ASSERT_EQ(VMF_OK,
				vmf_encoder_begin_pdu(f.writer, &f.envelope));
			std::vector<uint8_t> block(split);
			vmf_output output = {block.data(), block.size(), 0};
			provider_state state;

			state.replies.push_back(scalar_reply(0x5a));
			size_t delivered = 999;
			auto status = vmf_encoder_dispatch(f.writer, &output,
				provide, &state, &delivered);
			std::vector<uint8_t> encoded(block.begin(),
				block.begin() + output.used);

			EXPECT_EQ(state.calls, delivered);
			if (status == VMF_NEED_OUTPUT)
				status = dispatch_all(&f, &state, &encoded);
			EXPECT_EQ(VMF_DONE, status);
			EXPECT_EQ(bytes, encoded);
			EXPECT_EQ(manual, encoded);
			EXPECT_EQ(1u, state.calls);
			EXPECT_EQ(bytes.size() * 8,
				vmf_encoder_bit_count(f.writer));
		}
	}
}

TEST(ManagedEncoding, MultipleBodiesPreserveDescriptorIndexesAndMeasurements)
{
	for (unsigned revision = 1; revision <= 4; ++revision) {
		SCOPED_TRACE(revision);
		auto profile = static_cast<vmf_envelope_version>(revision);
		auto bytes = fixtures::packet(profile, {0x5a}, true, 2);
		managed_fixture f;

		initialize(&f, profile, bytes);
		ASSERT_EQ(VMF_OK, vmf_encoder_begin_pdu(f.writer, &f.envelope));
		provider_state state;
		std::vector<uint8_t> encoded;

		state.replies = {scalar_reply(0x5a), scalar_reply(0x5a)};
		ASSERT_EQ(VMF_DONE, dispatch_all(&f, &state, &encoded));
		EXPECT_EQ(bytes, encoded);
		ASSERT_EQ(2u, state.requests.size());
		for (size_t descriptor = 0; descriptor < 2; ++descriptor) {
			const auto &request = state.requests[descriptor];

			EXPECT_EQ(VMF_ENCODE_REQUEST_FIELD, request.kind);
			EXPECT_EQ(descriptor, request.descriptor_index);
			EXPECT_EQ(&f.message, request.schema);
			EXPECT_EQ(&f.index, request.index);
			EXPECT_EQ(0u, request.path_count);
			EXPECT_EQ(0u, request.offset_units);
			EXPECT_EQ(1u, request.total_units);
			vmf_measurement measurement = {};

			ASSERT_EQ(VMF_OK, vmf_encoder_measurement(f.writer,
				descriptor, &measurement));
			EXPECT_EQ(1u, measurement.body_octets);
			EXPECT_EQ(bytes.size() - 2, measurement.header_octets);
			EXPECT_EQ(bytes.size(), measurement.pdu_octets);
		}
		vmf_encoder_reset(f.writer);
		vmf_decoder_reset(f.reader);
		ASSERT_EQ(VMF_OK, vmf_decoder_begin_pdu(f.reader));
		vmf_input input = {bytes.data(), bytes.size(), 0, true};
		std::vector<uint8_t> manual;
		vmf_status status = VMF_OK;

		for (size_t attempt = 0; attempt < 32; ++attempt) {
			vmf_event event = {};

			status = vmf_decoder_next(f.reader, &input, &event);
			if (status == VMF_DONE)
				break;
			ASSERT_EQ(VMF_OK, status);
			fixtures::write(f.writer, &event, &manual);
		}
		EXPECT_EQ(VMF_DONE, status);
		EXPECT_EQ(encoded, manual);
	}
}

TEST(ManagedEncoding, RequestsRemainStableUntilAValidReplyIsConsumed)
{
	managed_fixture f;
	auto bytes = fixtures::packet(VMF_ENVELOPE_D1, {0x5a});

	initialize(&f, VMF_ENVELOPE_D1, bytes);
	ASSERT_EQ(VMF_OK, vmf_encoder_begin_pdu(f.writer, &f.envelope));
	uint8_t buffer[128] = {};
	vmf_output output = {buffer, sizeof(buffer), 0};
	vmf_encode_request first = {};
	vmf_encode_request again = {};

	ASSERT_EQ(VMF_OK,
		vmf_encoder_next_request(f.writer, &output, &first));
	size_t used = output.used;

	ASSERT_EQ(VMF_OK,
		vmf_encoder_next_request(f.writer, &output, &again));
	EXPECT_EQ(first.kind, again.kind);
	EXPECT_EQ(first.index, again.index);
	EXPECT_EQ(first.path, again.path);
	EXPECT_EQ(first.path_count, again.path_count);
	EXPECT_EQ(used, output.used);
	vmf_encode_reply wrong = {};

	wrong.kind = VMF_ENCODE_REQUEST_PRESENCE;
	EXPECT_EQ(VMF_TYPE_MISMATCH,
		vmf_encoder_respond(f.writer, &output, &wrong));
	wrong.kind = static_cast<vmf_encode_request_kind>(3);
	EXPECT_EQ(VMF_INVALID_ARGUMENT,
		vmf_encoder_respond(f.writer, &output, &wrong));
	EXPECT_EQ(VMF_INVALID_ARGUMENT,
		vmf_encoder_respond(f.writer, &output, nullptr));
	ASSERT_EQ(VMF_OK,
		vmf_encoder_next_request(f.writer, &output, &again));
	EXPECT_EQ(first.index, again.index);
	vmf_encode_reply value = scalar_reply(0x5a);

	ASSERT_EQ(VMF_OK, vmf_encoder_respond(f.writer, &output, &value));
	again.index = &f.index;
	EXPECT_EQ(VMF_DONE,
		vmf_encoder_next_request(f.writer, &output, &again));
	EXPECT_EQ(nullptr, again.index);
	EXPECT_EQ(nullptr, again.schema);
	EXPECT_EQ(nullptr, again.path);
	EXPECT_EQ(0u, again.path_count);
	EXPECT_EQ(bytes,
		std::vector<uint8_t>(buffer, buffer + output.used));
}

TEST(ManagedEncoding, RespondRetriesPreserveBorrowedChunksAndEntryPoint)
{
	managed_fixture f;
	const uint8_t value[] = {1, 2, 3};
	auto bytes = fixtures::packet(VMF_ENVELOPE_D1, {1, 2, 3});

	initialize(&f, VMF_ENVELOPE_D1, bytes);
	f.index.number_of_bits = 24;
	f.index.encode_type = VMF_ENCODE_OCTETS;
	recreate(&f);
	ASSERT_EQ(VMF_OK, vmf_encoder_begin_pdu(f.writer, &f.envelope));
	uint8_t buffer[128] = {};
	vmf_output output = {buffer, sizeof(buffer), 0};
	vmf_encode_request request = {};

	ASSERT_EQ(VMF_OK,
		vmf_encoder_next_request(f.writer, &output, &request));
	EXPECT_EQ(3u, request.total_units);
	vmf_encode_reply reply = {};

	reply.kind = VMF_ENCODE_REQUEST_FIELD;
	reply.as.field.octets = {value, sizeof(value), true};
	vmf_output blocked = {nullptr, 0, 0};

	ASSERT_EQ(VMF_NEED_OUTPUT,
		vmf_encoder_respond(f.writer, &blocked, &reply));
	vmf_encode_reply changed = reply;

	changed.as.field.octets.size = 2;
	EXPECT_EQ(VMF_INVALID_ARGUMENT,
		vmf_encoder_respond(f.writer, &output, &changed));
	EXPECT_EQ(VMF_INVALID_STATE,
		vmf_encoder_next_request(f.writer, &output, &request));
	EXPECT_EQ(nullptr, request.index);
	provider_state state;
	size_t delivered = 999;

	EXPECT_EQ(VMF_INVALID_STATE, vmf_encoder_dispatch(f.writer,
		&output, provide, &state, &delivered));
	EXPECT_EQ(0u, delivered);
	EXPECT_EQ(0u, state.calls);
	vmf_encode_reply moved = reply;

	ASSERT_EQ(VMF_OK,
		vmf_encoder_respond(f.writer, &output, &moved));
	EXPECT_EQ(VMF_DONE,
		vmf_encoder_next_request(f.writer, &output, &request));
	EXPECT_EQ(bytes,
		std::vector<uint8_t>(buffer, buffer + output.used));
}

TEST(ManagedEncoding, CallbackReplyIsCachedAndPauseWaitsForConsumption)
{
	for (auto action : {VMF_EVENT_CONTINUE, VMF_EVENT_PAUSE}) {
		managed_fixture f;
		const uint8_t value[] = {1, 2, 3};
		auto bytes = fixtures::packet(VMF_ENVELOPE_D1, {1, 2, 3});

		initialize(&f, VMF_ENVELOPE_D1, bytes);
		f.index.number_of_bits = 24;
		f.index.encode_type = VMF_ENCODE_OCTETS;
		recreate(&f);
		ASSERT_EQ(VMF_OK,
			vmf_encoder_begin_pdu(f.writer, &f.envelope));
		provider_state state;
		vmf_encode_reply reply = {};

		reply.kind = VMF_ENCODE_REQUEST_FIELD;
		reply.as.field.octets = {value, sizeof(value), true};
		state.replies.push_back(reply);
		state.action = action;
		std::vector<uint8_t> header(bytes.size() - sizeof(value));
		vmf_output output = {header.data(), header.size(), 0};
		size_t delivered = 999;

		ASSERT_EQ(VMF_NEED_OUTPUT, vmf_encoder_dispatch(f.writer,
			&output, provide, &state, &delivered));
		EXPECT_EQ(1u, state.calls);
		EXPECT_EQ(1u, delivered);
		EXPECT_EQ(header.size(), output.used);
		provider_state other;
		vmf_output blocked = {nullptr, 0, 0};

		EXPECT_EQ(VMF_INVALID_ARGUMENT, vmf_encoder_dispatch(f.writer,
			&blocked, other_provider, &state, &delivered));
		EXPECT_EQ(0u, delivered);
		EXPECT_EQ(VMF_INVALID_ARGUMENT, vmf_encoder_dispatch(f.writer,
			&blocked, provide, &other, &delivered));
		vmf_encode_request request = {};

		EXPECT_EQ(VMF_INVALID_STATE,
			vmf_encoder_next_request(f.writer, &blocked, &request));
		EXPECT_EQ(VMF_INVALID_STATE,
			vmf_encoder_respond(f.writer, &blocked, &reply));
		EXPECT_EQ(VMF_NEED_OUTPUT, vmf_encoder_dispatch(f.writer,
			&blocked, provide, &state, &delivered));
		EXPECT_EQ(0u, delivered);
		EXPECT_EQ(1u, state.calls);
		std::vector<uint8_t> encoded = header;
		auto status = dispatch_all(&f, &state, &encoded);

		EXPECT_EQ(action == VMF_EVENT_PAUSE ? VMF_PAUSED : VMF_DONE,
			status);
		EXPECT_EQ(1u, state.calls);
		if (status == VMF_PAUSED) {
			state.action = VMF_EVENT_CONTINUE;
			EXPECT_EQ(VMF_DONE,
				dispatch_all(&f, &state, &encoded));
		}
		EXPECT_EQ(bytes, encoded);
	}
}

TEST(ManagedEncoding, CallbackAbortAndUnknownActionsAreTerminal)
{
	for (auto action : {VMF_EVENT_ABORT,
		static_cast<vmf_event_action>(3)}) {
		managed_fixture f;
		auto bytes = fixtures::packet(VMF_ENVELOPE_D1, {0x5a});

		initialize(&f, VMF_ENVELOPE_D1, bytes, true);
		ASSERT_EQ(VMF_OK,
			vmf_encoder_begin_pdu(f.writer, &f.envelope));
		provider_state state;
		std::vector<uint8_t> ignored;

		state.action = action;
		state.replies.push_back(scalar_reply(0x5a));
		auto status = action == VMF_EVENT_ABORT ? VMF_ABORTED :
			VMF_INVALID_ARGUMENT;

		EXPECT_EQ(status,
			dispatch_all(&f, &state, &ignored, 0, true));
		EXPECT_EQ(1u, state.calls);
		vmf_encode_request request = {};

		EXPECT_EQ(status,
			vmf_encoder_next_request(f.writer, nullptr, &request));
		EXPECT_EQ(nullptr, request.schema);
	}
}

TEST(ManagedEncoding, ChunkedTextOctetsAndBitsExposeWireOffsets)
{
	for (auto kind : {VMF_ENCODE_TEXT, VMF_ENCODE_OCTETS,
		VMF_ENCODE_BITS, VMF_ENCODE_LITERAL}) {
		managed_fixture f;
		bool text = kind == VMF_ENCODE_TEXT ||
			kind == VMF_ENCODE_LITERAL;
		bool literal = kind == VMF_ENCODE_LITERAL;
		std::vector<uint8_t> body = {0x81, 0x03};
		/* ASCII7 A/B; literal adds a DELETE terminator. */
		if (text) {
			body = {0x41, 0x21};
			if (literal)
				body = {0x41, 0xe1, 0x1f};
		}
		auto bytes = fixtures::packet(VMF_ENVELOPE_D1, body);

		initialize(&f, VMF_ENVELOPE_D1, bytes);
		f.index.encode_type = kind;
		f.index.number_of_bits = text ? (literal ? 28 : 14) :
			kind == VMF_ENCODE_BITS ? 10 : 16;
		recreate(&f);
		ASSERT_EQ(VMF_OK,
			vmf_encoder_begin_pdu(f.writer, &f.envelope));
		provider_state state;
		vmf_encode_reply first = {};
		vmf_encode_reply last = {};
		const uint8_t octets[] = {0x81, 0x03};

		first.kind = last.kind = VMF_ENCODE_REQUEST_FIELD;
		if (text) {
			first.as.field.text = {"A", 1, false};
			last.as.field.text = {"B", 1, true};
		} else if (kind == VMF_ENCODE_BITS) {
			first.as.field.bits = {octets, 8, false};
			last.as.field.bits = {octets + 1, 2, true};
		} else {
			first.as.field.octets = {octets, 1, false};
			last.as.field.octets = {octets + 1, 1, true};
		}
		state.replies = {first, last};
		std::vector<uint8_t> encoded;

		ASSERT_EQ(VMF_DONE, dispatch_all(&f, &state, &encoded));
		EXPECT_EQ(bytes, encoded);
		ASSERT_EQ(2u, state.requests.size());
		EXPECT_EQ(0u, state.requests[0].offset_units);
		EXPECT_EQ(kind == VMF_ENCODE_BITS ? 8u : 1u,
			state.requests[1].offset_units);
		uint64_t total = literal ? VMF_UNITS_UNKNOWN :
			text ? 2u : kind == VMF_ENCODE_BITS ? 10u : 2u;

		EXPECT_EQ(total, state.requests[0].total_units);
		EXPECT_EQ(total, state.requests[1].total_units);
	}
}

TEST(ManagedEncoding, InvalidCallbackReplyCanBeCorrectedWithPrimitives)
{
	managed_fixture f;
	auto bytes = fixtures::packet(VMF_ENVELOPE_D1, {0x5a});

	initialize(&f, VMF_ENVELOPE_D1, bytes);
	ASSERT_EQ(VMF_OK, vmf_encoder_begin_pdu(f.writer, &f.envelope));
	provider_state state;
	vmf_encode_reply wrong = {};

	wrong.kind = VMF_ENCODE_REQUEST_PRESENCE;
	state.replies.push_back(wrong);
	uint8_t buffer[128] = {};
	vmf_output output = {buffer, sizeof(buffer), 0};
	size_t delivered = 999;

	EXPECT_EQ(VMF_TYPE_MISMATCH, vmf_encoder_dispatch(f.writer, &output,
		provide, &state, &delivered));
	EXPECT_EQ(1u, delivered);
	vmf_encode_request request = {};

	ASSERT_EQ(VMF_OK,
		vmf_encoder_next_request(f.writer, &output, &request));
	EXPECT_EQ(&f.index, request.index);
	vmf_encode_reply correct = scalar_reply(0x5a);

	ASSERT_EQ(VMF_OK,
		vmf_encoder_respond(f.writer, &output, &correct));
	EXPECT_EQ(VMF_DONE,
		vmf_encoder_next_request(f.writer, &output, &request));
	EXPECT_EQ(bytes,
		std::vector<uint8_t>(buffer, buffer + output.used));
}

TEST(ManagedEncoding, CorrectedNullChunkArgumentPreservesOutstandingRequest)
{
	managed_fixture f;
	auto bytes = fixtures::packet(VMF_ENVELOPE_D1, {0x5a});

	initialize(&f, VMF_ENVELOPE_D1, bytes);
	f.index.encode_type = VMF_ENCODE_OCTETS;
	recreate(&f);
	ASSERT_EQ(VMF_OK, vmf_encoder_begin_pdu(f.writer, &f.envelope));
	uint8_t buffer[128] = {};
	vmf_output output = {buffer, sizeof(buffer), 0};
	vmf_encode_request request = {};

	ASSERT_EQ(VMF_OK,
		vmf_encoder_next_request(f.writer, &output, &request));
	vmf_encode_reply reply = {};

	reply.kind = VMF_ENCODE_REQUEST_FIELD;
	reply.as.field.octets = {nullptr, 1, true};
	EXPECT_EQ(VMF_INVALID_ARGUMENT,
		vmf_encoder_respond(f.writer, &output, &reply));
	ASSERT_EQ(VMF_OK,
		vmf_encoder_next_request(f.writer, &output, &request));
	EXPECT_EQ(0u, request.offset_units);
	const uint8_t value = 0x5a;

	reply.as.field.octets.data = &value;
	ASSERT_EQ(VMF_OK,
		vmf_encoder_respond(f.writer, &output, &reply));
	EXPECT_EQ(VMF_DONE,
		vmf_encoder_next_request(f.writer, &output, &request));
	EXPECT_EQ(bytes,
		std::vector<uint8_t>(buffer, buffer + output.used));
}

TEST(ManagedEncoding, TextRequestsCountWireUnitsRatherThanUtf8ApiBytes)
{
	for (auto encoding : {VMF_TEXT_LATIN_1, VMF_TEXT_UTF_8}) {
		managed_fixture f;
		auto body = encoding == VMF_TEXT_LATIN_1 ?
			std::vector<uint8_t>{0xe9, 0x41} :
			std::vector<uint8_t>{0xc3, 0xa9, 0x41};
		auto bytes = fixtures::packet(VMF_ENVELOPE_D1, body);

		initialize(&f, VMF_ENVELOPE_D1, bytes);
		vmf_index_refinements refinements = {};

		refinements.text_encoding = encoding;
		f.index.refinements = &refinements;
		f.index.encode_type = VMF_ENCODE_TEXT;
		f.index.number_of_bits = body.size() * 8;
		recreate(&f);
		ASSERT_EQ(VMF_OK,
			vmf_encoder_begin_pdu(f.writer, &f.envelope));
		provider_state state;
		vmf_encode_reply first = {};
		vmf_encode_reply last = {};

		first.kind = last.kind = VMF_ENCODE_REQUEST_FIELD;
		first.as.field.text = {"\xc3\xa9", 2, false};
		last.as.field.text = {"A", 1, true};
		state.replies = {first, last};
		std::vector<uint8_t> encoded;

		ASSERT_EQ(VMF_DONE, dispatch_all(&f, &state, &encoded));
		EXPECT_EQ(bytes, encoded);
		ASSERT_EQ(2u, state.requests.size());
		EXPECT_EQ(body.size(), state.requests[0].total_units);
		EXPECT_EQ(body.size() - 1, state.requests[1].offset_units);
	}
}

TEST(ManagedEncoding, BeginCopiesCallerOwnedHeaderAndNestedPointers)
{
	managed_fixture f;
	auto bytes = fixtures::packet(VMF_ENVELOPE_D1, {0x5a});

	initialize(&f, VMF_ENVELOPE_D1, bytes);
	vmf_header_d1 header =
		*static_cast<const vmf_header_d1 *>(f.envelope.header);
	vmf_message_header message = header.fields.messages[0];
	char filename = 'x';

	message.file_name.value = {&filename, 1};
	header.fields.messages = &message;
	vmf_envelope_event envelope = f.envelope;

	envelope.header = &header;
	ASSERT_EQ(VMF_OK, vmf_encoder_begin_pdu(f.writer, &envelope));
	header.fields.message_count = 0;
	message.id = {};
	message.message_size_octets = {true, 99};
	filename = 'z';
	provider_state state;
	std::vector<uint8_t> encoded;

	state.replies.push_back(scalar_reply(0x5a));
	ASSERT_EQ(VMF_DONE, dispatch_all(&f, &state, &encoded));
	EXPECT_EQ(bytes, encoded);
}

TEST(ManagedEncoding, NestedGpiAndFpiDistinguishAbsenceFromPresentZero)
{
	for (unsigned present = 0; present <= 3; ++present) {
		managed_fixture f;
		uint8_t body = static_cast<uint8_t>((1u << present) - 1);
		auto bytes = fixtures::packet(VMF_ENVELOPE_D1, {body});

		initialize(&f, VMF_ENVELOPE_D1, bytes);
		uint32_t addresses[] = {1, 2, 3, 4};
		vmf_group_id groups[] = {10, 20};
		vmf_index_schema indexes[4] = {};

		for (size_t i = 0; i < 4; ++i)
			indexes[i].index_number = {addresses + i, 1};
		indexes[0].indicator = VMF_INDICATOR_GPI;
		indexes[0].target.kind = VMF_TARGET_GROUP;
		indexes[0].target.as.group = 10;
		indexes[1].indicator = VMF_INDICATOR_GPI;
		indexes[1].target.kind = VMF_TARGET_GROUP;
		indexes[1].target.as.group = 20;
		indexes[1].group_code = {groups, 1};
		indexes[2].indicator = VMF_INDICATOR_FPI;
		indexes[2].target.kind = VMF_TARGET_INDEX;
		indexes[2].target.as.index = indexes[3].index_number;
		indexes[2].group_code = {groups, 2};
		indexes[3].group_code = {groups, 2};
		indexes[3].number_of_bits = 5;
		f.message.indexes = indexes;
		f.message.index_count = 4;
		recreate(&f);
		ASSERT_EQ(VMF_OK,
			vmf_encoder_begin_pdu(f.writer, &f.envelope));
		provider_state state;

		for (unsigned level = 0; level <= present && level < 3;
			++level) {
			vmf_encode_reply reply = {};

			reply.kind = VMF_ENCODE_REQUEST_PRESENCE;
			reply.as.present = level < present;
			state.replies.push_back(reply);
		}
		if (present == 3)
			state.replies.push_back(scalar_reply(0));
		std::vector<uint8_t> encoded;

		ASSERT_EQ(VMF_DONE, dispatch_all(&f, &state, &encoded));
		EXPECT_EQ(bytes, encoded);
		ASSERT_EQ(state.replies.size(), state.requests.size());
		EXPECT_EQ(nullptr, state.requests[0].index);
		EXPECT_EQ(VMF_SCOPE_GROUP, state.requests[0].scope.kind);
		EXPECT_EQ(10u, state.requests[0].scope.id);
		if (present >= 1) {
			EXPECT_EQ(nullptr, state.requests[1].index);
			EXPECT_EQ(20u, state.requests[1].scope.id);
		}
		if (present >= 2) {
			EXPECT_EQ(&indexes[3], state.requests[2].index);
			EXPECT_EQ(VMF_SCOPE_NONE, state.requests[2].scope.kind);
		}
		for (const auto &request : state.requests) {
			EXPECT_EQ(&f.message, request.schema);
			EXPECT_EQ(0u, request.path_count);
		}
	}
}

TEST(ManagedEncoding, RawConditionsAreAutomaticAndConstantsRemainValues)
{
	for (uint64_t source : {2u, 3u}) {
		managed_fixture f;
		auto body = source == 2 ? std::vector<uint8_t>{2, 0, 7} :
			std::vector<uint8_t>{3, 7};
		auto bytes = fixtures::packet(VMF_ENVELOPE_D1, body);

		initialize(&f, VMF_ENVELOPE_D1, bytes);
		uint32_t addresses[] = {1, 2, 3};
		vmf_index_schema indexes[3] = {};
		vmf_index_refinements refinements[3] = {};
		vmf_scale_definition scale = {{10, 1}, {100, 1}, nullptr, 0};
		vmf_code_range range = {2, 2};
		vmf_condition_term term = {};
		vmf_condition condition = {VMF_CONDITION_ALL, &term, 1, false};

		for (size_t i = 0; i < 3; ++i) {
			indexes[i].index_number = {addresses + i, 1};
			indexes[i].number_of_bits = 8;
			indexes[i].refinements = refinements + i;
		}
		indexes[0].encode_type = VMF_ENCODE_SCALED;
		refinements[0].scale = &scale;
		term.field = indexes[0].index_number;
		term.values = &range;
		term.value_count = 1;
		refinements[1].when = &condition;
		refinements[2].constant = {true, 7};
		f.message.indexes = indexes;
		f.message.index_count = 3;
		recreate(&f);
		ASSERT_EQ(VMF_OK,
			vmf_encoder_begin_pdu(f.writer, &f.envelope));
		provider_state state;

		state.replies.push_back(scalar_reply(source));
		if (source == 2)
			state.replies.push_back(scalar_reply(0));
		state.replies.push_back(scalar_reply(7));
		std::vector<uint8_t> encoded;

		ASSERT_EQ(VMF_DONE, dispatch_all(&f, &state, &encoded));
		EXPECT_EQ(bytes, encoded);
		ASSERT_EQ(source == 2 ? 3u : 2u, state.requests.size());
		EXPECT_EQ(VMF_FIELD_SCALED, state.requests[0].field_kind);
		EXPECT_EQ(&indexes[2], state.requests.back().index);
		for (const auto &request : state.requests)
			EXPECT_EQ(VMF_ENCODE_REQUEST_FIELD, request.kind);
	}
}

TEST(ManagedEncoding, ScalarRepeatCountsAndEmptyOctetsNeedNoControlReplies)
{
	for (uint64_t count : {0u, 2u}) {
		managed_fixture f;
		auto body = count ? std::vector<uint8_t>{2, 9, 8} :
			std::vector<uint8_t>{0};
		auto bytes = fixtures::packet(VMF_ENVELOPE_D1, body);

		initialize(&f, VMF_ENVELOPE_D1, bytes);
		uint32_t addresses[] = {1, 2, 3};
		vmf_index_schema indexes[3] = {};
		vmf_repeat_component membership = {10, {}};
		vmf_repeat_definition repeat = {};
		vmf_index_refinements empty = {};

		for (size_t i = 0; i < 3; ++i) {
			indexes[i].index_number = {addresses + i, 1};
			indexes[i].number_of_bits = 8;
		}
		indexes[1].repeat_code = {&membership, 1};
		repeat.id = 10;
		repeat.kind = VMF_REPEAT_FIELD;
		repeat.count_from = indexes[0].index_number;
		repeat.maximum = 2;
		/* Zero-capacity variable data receives one empty value. */
		indexes[2].number_of_bits = 0;
		indexes[2].encode_type = VMF_ENCODE_OCTETS;
		empty.length_from = indexes[0].index_number;
		indexes[2].refinements = &empty;
		f.message.indexes = indexes;
		f.message.index_count = count ? 2 : 3;
		f.message.repeats = &repeat;
		f.message.repeat_count = 1;
		recreate(&f);
		ASSERT_EQ(VMF_OK,
			vmf_encoder_begin_pdu(f.writer, &f.envelope));
		provider_state state;

		state.replies.push_back(scalar_reply(count));
		if (count) {
			state.replies.push_back(scalar_reply(9));
			state.replies.push_back(scalar_reply(8));
		} else {
			vmf_encode_reply reply = {};

			reply.kind = VMF_ENCODE_REQUEST_FIELD;
			reply.as.field.octets = {nullptr, 0, true};
			state.replies.push_back(reply);
		}
		std::vector<uint8_t> encoded;

		ASSERT_EQ(VMF_DONE, dispatch_all(&f, &state, &encoded));
		EXPECT_EQ(bytes, encoded);
		for (const auto &request : state.requests)
			EXPECT_EQ(VMF_ENCODE_REQUEST_FIELD, request.kind);
		if (count) {
			ASSERT_EQ(3u, state.paths.size());
			for (size_t i = 1; i < 3; ++i) {
				ASSERT_EQ(1u, state.paths[i].size());
				EXPECT_EQ(10u, state.paths[i][0].id);
				EXPECT_EQ(i - 1, state.paths[i][0].occurrence);
			}
		} else {
			EXPECT_EQ(0u, state.requests.back().total_units);
		}
	}
}

TEST(ManagedEncoding, NestedIndicatorCountsIncludeAllOuterOccurrences)
{
	managed_fixture f;
	std::vector<uint8_t> body(4, 0);
	size_t bit = 0;

	auto put = [&](uint64_t value, unsigned width) {
		for (unsigned i = 0; i < width; ++i, ++bit)
			body[bit / 8] |= ((value >> i) & 1) << (bit % 8);
	};

	put(0, 1);
	put(5, 8);
	put(1, 1);
	put(6, 8);
	put(0, 1);
	put(7, 8);
	body.resize((bit + 7) / 8);
	auto bytes = fixtures::packet(VMF_ENVELOPE_D1, body);

	initialize(&f, VMF_ENVELOPE_D1, bytes);
	uint32_t addresses[] = {1, 2};
	vmf_index_schema indexes[2] = {};
	vmf_repeat_component controller[] = {{10, {}}, {20, {true, 2}}};
	vmf_repeat_component member[] = {{10, {}}, {20, {}}};
	vmf_repeat_definition outer = {};

	outer.id = 10;
	outer.count = 2;
	indexes[0].index_number = {addresses, 1};
	indexes[0].indicator = VMF_INDICATOR_FRI;
	indexes[0].repeat_code = {controller, 2};
	indexes[1].index_number = {addresses + 1, 1};
	indexes[1].number_of_bits = 8;
	indexes[1].repeat_code = {member, 2};
	f.message.indexes = indexes;
	f.message.index_count = 2;
	f.message.repeats = &outer;
	f.message.repeat_count = 1;
	recreate(&f);
	ASSERT_EQ(VMF_OK, vmf_encoder_begin_pdu(f.writer, &f.envelope));
	provider_state state;
	vmf_encode_reply count = {};

	count.kind = VMF_ENCODE_REQUEST_REPEAT_COUNT;
	count.as.repeat_count = 1;
	state.replies.push_back(count);
	state.replies.push_back(scalar_reply(5));
	count.as.repeat_count = 2;
	state.replies.push_back(count);
	state.replies.push_back(scalar_reply(6));
	state.replies.push_back(scalar_reply(7));
	std::vector<uint8_t> encoded;

	ASSERT_EQ(VMF_DONE, dispatch_all(&f, &state, &encoded));
	EXPECT_EQ(bytes, encoded);
	ASSERT_EQ(5u, state.requests.size());
	for (size_t i = 0; i < state.requests.size(); ++i) {
		bool count_request = i == 0 || i == 2;

		ASSERT_EQ(count_request ? 1u : 2u, state.paths[i].size());
		EXPECT_EQ(10u, state.paths[i][0].id);
		EXPECT_EQ(i >= 2 ? 1u : 0u, state.paths[i][0].occurrence);
		if (count_request) {
			EXPECT_EQ(nullptr, state.requests[i].index);
			EXPECT_EQ(VMF_SCOPE_REPEAT,
				state.requests[i].scope.kind);
			EXPECT_EQ(20u, state.requests[i].scope.id);
		} else {
			EXPECT_EQ(&indexes[1], state.requests[i].index);
			EXPECT_EQ(20u, state.paths[i][1].id);
			EXPECT_EQ(i == 4 ? 1u : 0u,
				state.paths[i][1].occurrence);
		}
	}
}

TEST(ManagedEncoding, FailedBeginRetainsMeasurementsAndBitCountAccumulates)
{
	managed_fixture f;
	auto bytes = fixtures::packet(VMF_ENVELOPE_D1, {0x5a});

	initialize(&f, VMF_ENVELOPE_D1, bytes, true);
	ASSERT_EQ(VMF_OK, vmf_encoder_begin_pdu(f.writer, &f.envelope));
	provider_state state;
	std::vector<uint8_t> ignored;

	state.replies.push_back(scalar_reply(0x5a));
	ASSERT_EQ(VMF_DONE, dispatch_all(&f, &state, &ignored, 0, true));
	vmf_measurement measured = {};

	ASSERT_EQ(VMF_OK, vmf_encoder_measurement(f.writer, 0, &measured));
	EXPECT_EQ(bytes.size(), measured.pdu_octets);
	EXPECT_EQ(1u, measured.body_octets);
	vmf_envelope_event bad = f.envelope;

	bad.object_size = 0;
	EXPECT_EQ(VMF_INVALID_ARGUMENT,
		vmf_encoder_begin_pdu(f.writer, &bad));
	vmf_measurement retained = {};

	ASSERT_EQ(VMF_OK, vmf_encoder_measurement(f.writer, 0, &retained));
	EXPECT_EQ(measured.pdu_octets, retained.pdu_octets);
	ASSERT_EQ(VMF_OK, vmf_encoder_begin_pdu(f.writer, &f.envelope));
	EXPECT_EQ(VMF_INVALID_STATE,
		vmf_encoder_measurement(f.writer, 0, &retained));
	EXPECT_EQ(0u, retained.pdu_octets);
	state.calls = 0;
	ASSERT_EQ(VMF_DONE, dispatch_all(&f, &state, &ignored, 0, true));
	EXPECT_EQ(bytes.size() * 16, vmf_encoder_bit_count(f.writer));
	vmf_encoder_reset(f.writer);
	EXPECT_EQ(0u, vmf_encoder_bit_count(f.writer));
	EXPECT_EQ(VMF_INVALID_ARGUMENT,
		vmf_encoder_measurement(f.writer, 0, &retained));
}

} /* namespace */
