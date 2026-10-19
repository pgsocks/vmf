#include <gtest/gtest.h>
#include <cucumber-cpp/autodetect.hpp>
#include "../unit/managed_consumer.h"
#include <string>
#include <vector>

extern "C" int vmf_managed_c_consumer(uint8_t *, size_t, size_t *);
int vmf_managed_cpp_consumer(uint8_t *, size_t, size_t *);

namespace
{

struct managed_context {
	managed_consumer source;
	uint8_t bytes[128] = {};
	size_t size = 0;
	vmf_decoder *reader = nullptr;
	vmf_encoder *writer = nullptr;
	vmf_envelope_event envelope = {};
	std::vector<uint8_t> encoded;
	vmf_event_action action = VMF_EVENT_CONTINUE;
	size_t calls = 0;
	size_t abort_calls = 0;
	bool deferred_pause = false;

	~managed_context()
	{
		vmf_encoder_destroy(writer);
		vmf_decoder_destroy(reader);
	}
};

vmf_event_action nested_reply(void *user, const vmf_encode_request *request,
	vmf_encode_reply *reply)
{
	auto *c = static_cast<managed_context *>(user);

	++c->calls;
	if (!managed_consumer_reply(&c->source, request, reply))
		return VMF_EVENT_ABORT;
	return c->action;
}

void prepare_stream(managed_context *c)
{
	ASSERT_EQ(1,
		vmf_managed_c_consumer(c->bytes, sizeof(c->bytes), &c->size));
	auto options = vmf_options_for_schemas(&c->source.set);

	ASSERT_EQ(VMF_OK, vmf_decoder_create(&options, &c->reader, nullptr));
	ASSERT_EQ(VMF_OK, vmf_decoder_begin_envelope(c->reader));
	vmf_input input = {c->bytes, c->size, 0, true};
	vmf_event event = {};

	ASSERT_EQ(VMF_OK, vmf_decoder_next(c->reader, &input, &event));
	c->envelope = event.as.envelope;
	ASSERT_EQ(VMF_OK,
		vmf_encoder_create(&options, false, &c->writer, nullptr));
	ASSERT_EQ(VMF_OK, vmf_encoder_begin_pdu(c->writer, &c->envelope));
}

vmf_status stream_dispatch(managed_context *c)
{
	for (size_t attempt = 0; attempt < 1024; ++attempt) {
		uint8_t byte = 0;
		vmf_output output = {&byte, 1, 0};
		size_t delivered = 999;
		size_t before = c->calls;
		auto status = vmf_encoder_dispatch(c->writer, &output,
			nested_reply, c, &delivered);

		EXPECT_EQ(c->calls - before, delivered);
		if (output.used)
			c->encoded.push_back(byte);
		if (status != VMF_NEED_OUTPUT)
			return status;
	}
	ADD_FAILURE() << "managed callback did not finish";
	return VMF_INVALID_STATE;
}

GIVEN("^a caller-owned two-by-two object and its native VMF schema$")
{
	cucumber::ScenarioScope<managed_context> context;

	managed_consumer_initialize(&context->source);
}

WHEN("^I encode it with the public (C11 primitive|C\\+\\+17 callback) "
	"interface$")
{
	REGEX_PARAM(std::string, interface);
	cucumber::ScenarioScope<managed_context> context;
	auto *c = context.get();
	int success = interface == "C11 primitive" ?
		vmf_managed_c_consumer(c->bytes, sizeof(c->bytes), &c->size) :
		vmf_managed_cpp_consumer(c->bytes, sizeof(c->bytes), &c->size);

	ASSERT_EQ(1, success);
}

THEN("^decoding returns all four nested values in schema order$")
{
	cucumber::ScenarioScope<managed_context> context;
	auto *c = context.get();
	auto options = vmf_options_for_schemas(&c->source.set);

	ASSERT_EQ(VMF_OK, vmf_decoder_create(&options, &c->reader, nullptr));
	ASSERT_EQ(VMF_OK, vmf_decoder_begin_pdu(c->reader));
	vmf_input input = {c->bytes, c->size, 0, true};
	std::vector<uint64_t> values;
	vmf_status status = VMF_OK;

	for (size_t attempt = 0; attempt < 64; ++attempt) {
		vmf_event event = {};

		status = vmf_decoder_next(c->reader, &input, &event);
		if (status != VMF_OK)
			break;
		if (event.kind == VMF_EVENT_FIELD)
			values.push_back(event.as.field.value
				.unsigned_integer.value);
	}
	EXPECT_EQ(VMF_DONE, status);
	EXPECT_EQ(c->size, input.used);
	EXPECT_EQ((std::vector<uint64_t>{0, 7, 11, 255}), values);
}

WHEN("^I stream its callback replies through one-octet output buffers$")
{
	cucumber::ScenarioScope<managed_context> context;
	auto *c = context.get();

	prepare_stream(c);
	ASSERT_EQ(VMF_DONE, stream_dispatch(c));
}

THEN("^the callback runs once for each of the four nested values$")
{
	cucumber::ScenarioScope<managed_context> context;

	EXPECT_EQ(4u, context->calls);
	EXPECT_EQ(4u, context->source.cursor);
}

WHEN("^its callback pauses while output is exhausted$")
{
	cucumber::ScenarioScope<managed_context> context;
	auto *c = context.get();

	prepare_stream(c);
	c->action = VMF_EVENT_PAUSE;
	std::vector<uint8_t> header(c->envelope.header_octets);
	vmf_output output = {header.data(), header.size(), 0};
	size_t delivered = 999;

	ASSERT_EQ(VMF_NEED_OUTPUT, vmf_encoder_dispatch(c->writer, &output,
		nested_reply, c, &delivered));
	ASSERT_EQ(1u, delivered);
	ASSERT_EQ(1u, c->calls);
	c->encoded = header;
	uint8_t byte = 0;
	vmf_output reply_output = {&byte, 1, 0};

	ASSERT_EQ(VMF_PAUSED, vmf_encoder_dispatch(c->writer, &reply_output,
		nested_reply, c, &delivered));
	ASSERT_EQ(0u, delivered);
	ASSERT_EQ(1u, c->calls);
	if (reply_output.used)
		c->encoded.push_back(byte);
	c->deferred_pause = true;
	c->action = VMF_EVENT_CONTINUE;
	ASSERT_EQ(VMF_DONE, stream_dispatch(c));
}

THEN("^the pause is reported after consuming the cached reply$")
{
	cucumber::ScenarioScope<managed_context> context;

	EXPECT_TRUE(context->deferred_pause);
	EXPECT_EQ(4u, context->calls);
}

WHEN("^its callback aborts and I reset the encoder$")
{
	cucumber::ScenarioScope<managed_context> context;
	auto *c = context.get();

	prepare_stream(c);
	c->action = VMF_EVENT_ABORT;
	ASSERT_EQ(VMF_ABORTED, stream_dispatch(c));
	ASSERT_EQ(1u, c->calls);
	c->abort_calls = c->calls;
	vmf_encode_request request = {};
	uint8_t byte = 0;
	vmf_output output = {&byte, 1, 0};

	EXPECT_EQ(VMF_ABORTED,
		vmf_encoder_next_request(c->writer, &output, &request));
	vmf_encoder_reset(c->writer);
	c->action = VMF_EVENT_CONTINUE;
	c->calls = 0;
	c->source.cursor = 0;
	c->encoded.clear();
	ASSERT_EQ(VMF_OK, vmf_encoder_begin_pdu(c->writer, &c->envelope));
	ASSERT_EQ(VMF_DONE, stream_dispatch(c));
}

THEN("^the reset encoding asks once for each nested value$")
{
	cucumber::ScenarioScope<managed_context> context;

	EXPECT_EQ(1u, context->abort_calls);
	EXPECT_EQ(4u, context->calls);
	EXPECT_EQ(4u, context->source.cursor);
}

THEN("^the streamed output matches the measured encoding$")
{
	cucumber::ScenarioScope<managed_context> context;

	EXPECT_EQ(std::vector<uint8_t>(context->bytes,
		context->bytes + context->size), context->encoded);
}

} /* namespace */
