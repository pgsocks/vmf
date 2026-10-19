#include <gtest/gtest.h>
#include <cucumber-cpp/autodetect.hpp>
#include <vmf/author.h>
#include "../unit/managed_consumer.h"
#include <vector>

namespace
{
struct author_context {
	managed_consumer source;
	vmf_author_field fields[4] = {};
	vmf_repeat_occurrence paths[4][2] = {};
	vmf_author_result *result = nullptr;
	vmf_author_error error = {};
	vmf_status status = VMF_INVALID_STATE;
	~author_context()
	{
		vmf_author_result_destroy(result);
	}
};

GIVEN("^complete raw values with canonical nested VMF addresses$")
{
	cucumber::ScenarioScope<author_context> context;
	auto *c = context.get();

	managed_consumer_initialize(&c->source);
	for (size_t i = 0; i < 4; ++i) {
		c->paths[i][0] = {10, i / 2};
		c->paths[i][1] = {20, i % 2};
		c->fields[i].address = {0, &c->source.schema,
			&c->source.index, {}, c->paths[i], 2};
		c->fields[i].kind = VMF_FIELD_UNSIGNED;
		c->fields[i].value.unsigned_integer.value =
			c->source.rows[i / 2][i % 2];
	}
}

WHEN("^I author the complete PDU using one-octet output slices$")
{
	cucumber::ScenarioScope<author_context> context;
	auto *c = context.get();
	auto options = vmf_author_options_for_schemas(&c->source.set);

	options.output_slice_bytes = 1;
	c->status = vmf_author_encode(&options, &c->source.envelope,
		c->fields, 4, nullptr, 0, &c->result, &c->error);
}

THEN("^the owned PDU decodes to the complete nested values$")
{
	cucumber::ScenarioScope<author_context> context;
	auto *c = context.get();

	ASSERT_EQ(VMF_OK, c->status);
	vmf_decoder *reader = nullptr;
	auto options = vmf_options_for_schemas(&c->source.set);

	ASSERT_EQ(VMF_OK, vmf_decoder_create(&options, &reader, nullptr));
	ASSERT_EQ(VMF_OK, vmf_decoder_begin_pdu(reader));
	vmf_input input = {vmf_author_result_data(c->result),
		vmf_author_result_size(c->result), 0, true};
	std::vector<uint64_t> values;
	vmf_status status = VMF_OK;

	while (status == VMF_OK) {
		vmf_event event = {};

		status = vmf_decoder_next(reader, &input, &event);
		if (status == VMF_OK && event.kind == VMF_EVENT_FIELD)
			values.push_back(event.as.field.value
				.unsigned_integer.value);
	}
	EXPECT_EQ(VMF_DONE, status);
	EXPECT_EQ((std::vector<uint64_t>{0, 7, 11, 255}), values);
	EXPECT_EQ(input.size, input.used);
	vmf_decoder_destroy(reader);
}

THEN("^the owned PDU size agrees with its measurement$")
{
	cucumber::ScenarioScope<author_context> context;
	vmf_measurement measured = {};

	ASSERT_EQ(VMF_OK, vmf_author_result_measurement(
		context->result, 0, &measured));
	EXPECT_EQ(4u, measured.body_octets);
	EXPECT_EQ(vmf_author_result_size(context->result), measured.pdu_octets);
}

WHEN("^I author the PDU with a missing mandatory nested value$")
{
	cucumber::ScenarioScope<author_context> context;
	auto *c = context.get();
	auto options = vmf_author_options_for_schemas(&c->source.set);

	c->status = vmf_author_encode(&options, &c->source.envelope,
		c->fields, 3, nullptr, 0, &c->result, &c->error);
}

THEN("^no PDU is published and the missing repeat occurrence is identified$")
{
	cucumber::ScenarioScope<author_context> context;
	auto *c = context.get();

	EXPECT_EQ(VMF_LENGTH_REQUIRED, c->status);
	EXPECT_EQ(nullptr, c->result);
	EXPECT_EQ(&c->source.index, c->error.index);
	ASSERT_EQ(2u, c->error.path_count);
	EXPECT_EQ(1u, c->error.path[0].occurrence);
	EXPECT_EQ(1u, c->error.path[1].occurrence);
}
} /* namespace */
