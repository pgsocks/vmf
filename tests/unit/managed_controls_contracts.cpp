#include <gtest/gtest.h>
#include <vmf/wire.h>
#include "managed_consumer.h"

namespace
{

struct managed_writer {
	vmf_encoder *handle = nullptr;

	~managed_writer()
	{
		vmf_encoder_destroy(handle);
	}
};

vmf_event_action unexpected_value(void *user, const vmf_encode_request *,
	vmf_encode_reply *)
{
	++*static_cast<size_t *>(user);
	return VMF_EVENT_ABORT;
}

vmf_event_action nested_value(void *user,
	const vmf_encode_request *request, vmf_encode_reply *reply)
{
	return managed_consumer_reply(
		static_cast<managed_consumer *>(user), request, reply)
		? VMF_EVENT_CONTINUE : VMF_EVENT_ABORT;
}

TEST(ManagedControls, FixedZeroRepeatKeepsContainerAndSkipsItsValues)
{
	managed_consumer source;

	managed_consumer_initialize(&source);
	source.repeats[0].count = 0;
	auto options = vmf_options_for_schemas(&source.set);

	/* Root and the empty repeat each contribute BEGIN/END, plus the two
	 * message boundaries: exactly six events without any field request.
	 */
	options.limits.max_events_per_message = 6;
	managed_writer counter;

	ASSERT_EQ(VMF_OK,
		vmf_encoder_create(&options, true, &counter.handle, nullptr));
	ASSERT_EQ(VMF_OK,
		vmf_encoder_begin_pdu(counter.handle, &source.envelope));
	size_t calls = 0, delivered = 99;

	ASSERT_EQ(VMF_DONE, vmf_encoder_dispatch(counter.handle, nullptr,
		unexpected_value, &calls, &delivered));
	EXPECT_EQ(0u, calls);
	EXPECT_EQ(0u, delivered);
	vmf_measurement measurement = {};

	ASSERT_EQ(VMF_OK,
		vmf_encoder_measurement(counter.handle, 0, &measurement));
	EXPECT_EQ(0u, measurement.body_octets);
	EXPECT_EQ(measurement.header_octets, measurement.pdu_octets);
	source.header.fields.header_size_octets.value =
		measurement.header_octets;
	source.message.message_size_octets.value = 0;
	managed_writer writer;

	ASSERT_EQ(VMF_OK,
		vmf_encoder_create(&options, false, &writer.handle, nullptr));
	ASSERT_EQ(VMF_OK,
		vmf_encoder_begin_pdu(writer.handle, &source.envelope));
	uint8_t bytes[256] = {};
	vmf_output output = {bytes, sizeof(bytes), 0};

	EXPECT_EQ(VMF_DONE, vmf_encoder_dispatch(writer.handle, &output,
		unexpected_value, &calls, &delivered));
	EXPECT_EQ(measurement.pdu_octets, output.used);
	EXPECT_EQ(0u, calls);
	EXPECT_EQ(0u, source.cursor);
}

TEST(ManagedControls, AutomaticEventsRespectTheMessageEventLimit)
{
	managed_consumer source;

	managed_consumer_initialize(&source);
	source.repeats[0].count = 0;
	auto options = vmf_options_for_schemas(&source.set);

	options.limits.max_events_per_message = 5;
	managed_writer writer;

	ASSERT_EQ(VMF_OK,
		vmf_encoder_create(&options, true, &writer.handle, nullptr));
	ASSERT_EQ(VMF_OK,
		vmf_encoder_begin_pdu(writer.handle, &source.envelope));
	size_t calls = 0, delivered = 99;

	EXPECT_EQ(VMF_LIMIT_EXCEEDED,
		vmf_encoder_dispatch(writer.handle, nullptr,
			unexpected_value, &calls, &delivered));
	EXPECT_EQ(0u, calls);
	EXPECT_EQ(0u, delivered);
	vmf_encode_request request = {};

	request.schema = &source.schema;
	EXPECT_EQ(VMF_LIMIT_EXCEEDED,
		vmf_encoder_next_request(writer.handle, nullptr, &request));
	EXPECT_EQ(nullptr, request.schema);
	EXPECT_EQ(VMF_LIMIT_EXCEEDED,
		vmf_encoder_error(writer.handle).status);
	vmf_measurement measurement = {};

	EXPECT_EQ(VMF_INVALID_STATE,
		vmf_encoder_measurement(writer.handle, 0, &measurement));
}

TEST(ManagedControls, ExactDepthBoundCoversNestedRepeatBodies)
{
	managed_consumer source;

	managed_consumer_initialize(&source);
	auto options = vmf_options_for_schemas(&source.set);

	/* Root, outer repeat/body, inner repeat/body, field: depth five. */
	options.limits.max_depth = 5;
	managed_writer writer;

	ASSERT_EQ(VMF_OK,
		vmf_encoder_create(&options, true, &writer.handle, nullptr));
	ASSERT_EQ(VMF_OK,
		vmf_encoder_begin_pdu(writer.handle, &source.envelope));
	size_t delivered = 99;

	EXPECT_EQ(VMF_DONE, vmf_encoder_dispatch(writer.handle, nullptr,
		nested_value, &source, &delivered));
	EXPECT_EQ(4u, delivered);
	EXPECT_EQ(4u, source.cursor);
	options.limits.max_depth = 4;
	managed_writer too_shallow;
	vmf_error error = {};

	EXPECT_EQ(VMF_LIMIT_EXCEEDED, vmf_encoder_create(&options, true,
		&too_shallow.handle, &error));
	EXPECT_EQ(nullptr, too_shallow.handle);
	EXPECT_EQ(VMF_LIMIT_EXCEEDED, error.status);
}

TEST(ManagedControls, IndicatorCountsRejectUnknownAndBoundViolations)
{
	struct invalid_count {
		uint64_t count;
		uint64_t limit;
		vmf_status expected;
	};
	const invalid_count cases[] = {
		{VMF_UNITS_UNKNOWN, 4, VMF_LENGTH_REQUIRED},
		{0, 4, VMF_OUT_OF_RANGE},
		{5, 4, VMF_OUT_OF_RANGE},
		{3, 2, VMF_LIMIT_EXCEEDED},
	};

	for (const auto &test : cases) {
		SCOPED_TRACE(test.count);
		managed_consumer source;

		managed_consumer_initialize(&source);
		uint32_t addresses[] = {1, 2};
		vmf_index_schema indexes[2] = {};
		vmf_repeat_component controller = {10, {true, 4}};
		vmf_repeat_component member = {10, {}};

		indexes[0].index_number = {addresses, 1};
		indexes[0].indicator = VMF_INDICATOR_FRI;
		indexes[0].repeat_code = {&controller, 1};
		indexes[1].index_number = {addresses + 1, 1};
		indexes[1].number_of_bits = 8;
		indexes[1].repeat_code = {&member, 1};
		source.schema.indexes = indexes;
		source.schema.index_count = 2;
		source.schema.repeats = nullptr;
		source.schema.repeat_count = 0;
		auto options = vmf_options_for_schemas(&source.set);

		options.limits.max_repetitions = test.limit;
		managed_writer writer;

		ASSERT_EQ(VMF_OK, vmf_encoder_create(&options, true,
			&writer.handle, nullptr));
		ASSERT_EQ(VMF_OK,
			vmf_encoder_begin_pdu(writer.handle, &source.envelope));
		vmf_encode_request request = {};

		ASSERT_EQ(VMF_OK, vmf_encoder_next_request(
			writer.handle, nullptr, &request));
		EXPECT_EQ(VMF_ENCODE_REQUEST_REPEAT_COUNT, request.kind);
		EXPECT_EQ(VMF_SCOPE_REPEAT, request.scope.kind);
		EXPECT_EQ(10u, request.scope.id);
		EXPECT_EQ(0u, request.path_count);
		EXPECT_EQ(nullptr, request.path);
		vmf_encode_reply reply = {};

		reply.kind = VMF_ENCODE_REQUEST_REPEAT_COUNT;
		reply.as.repeat_count = test.count;
		EXPECT_EQ(test.expected,
			vmf_encoder_respond(writer.handle, nullptr, &reply));
		EXPECT_EQ(test.expected, vmf_encoder_next_request(
			writer.handle, nullptr, &request));
		EXPECT_EQ(nullptr, request.schema);
		vmf_encoder_reset(writer.handle);
		EXPECT_EQ(VMF_OK,
			vmf_encoder_begin_pdu(writer.handle, &source.envelope));
		EXPECT_EQ(VMF_OK, vmf_encoder_next_request(
			writer.handle, nullptr, &request));
		reply.as.repeat_count = 1;
		EXPECT_EQ(VMF_OK,
			vmf_encoder_respond(writer.handle, nullptr, &reply));
	}
}

} /* namespace */
