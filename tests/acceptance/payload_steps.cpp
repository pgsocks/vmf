#include <gtest/gtest.h>
#include <cucumber-cpp/autodetect.hpp>
#include <vmf/wire.h>
#include "vectors.h"
#include "../unit/fixtures.hpp"
#include <string>
#include <vector>

namespace
{

struct payload_context {
	uint32_t parts[3] = {1, 2, 3};
	vmf_index_schema indexes[3] = {};
	vmf_index_refinements refinements[3] = {};
	vmf_repeat_component controller = {1, {true, 5}};
	vmf_repeat_component member = {1, {}};
	vmf_code_range range = {2, 2};
	vmf_condition_term term = {};
	vmf_condition condition = {};
	vmf_message_schema schema = {};
	vmf_schema_set schemas = {};
	vmf_decoder *reader = nullptr;
	vmf_encoder *writer = nullptr;
	std::vector<uint8_t> wire;
	std::vector<uint8_t> encoded;
	std::vector<uint64_t> numbers;
	std::vector<bool> more;
	std::vector<std::string> texts;
	std::vector<uint64_t> offsets;
	vmf_status status = VMF_OK;
	vmf_status encoder_status = VMF_OK;
	std::string example;
	bool known_count = false;
	std::vector<vmf_index_schema> lookup_indexes;
	std::vector<uint32_t> lookup_parts;

	~payload_context()
	{
		vmf_decoder_destroy(reader);
		vmf_encoder_destroy(writer);
	}
};

static void initialize(payload_context *c, size_t count)
{
	c->schema.message_number = {1, 1};
	c->schema.indexes = c->indexes;
	c->schema.index_count = count;
	c->schemas = {VMF_ENVELOPE_D1, &c->schema, 1, {}};
	c->schemas.standard.kind = VMF_STANDARD_RAW_CODE;
	c->schemas.standard.as.raw_code = 0;
	for (size_t i = 0; i < count; ++i) {
		c->indexes[i].index_number = {&c->parts[i], 1};
		c->indexes[i].number_of_bits = 8;
	}
}

static void attach_payload(payload_context *c,
	std::initializer_list<uint8_t> body)
{
	c->wire = fixtures::packet(VMF_ENVELOPE_D1,
		std::vector<uint8_t>(body));
}

GIVEN("^a synthetic (leading recurrence|conditional fields|UTF-8 text) "
	"body in a D1 envelope$")
{
	REGEX_PARAM(std::string, example);
	cucumber::ScenarioScope<payload_context> context;
	auto *c = context.get();

	c->example = example;
	initialize(c, example == "conditional fields" ? 3 :
		example == "leading recurrence" ? 2 : 1);
	if (example == "leading recurrence") {
		c->indexes[0].number_of_bits = 0;
		c->indexes[0].indicator = VMF_INDICATOR_FRI;
		c->indexes[0].repeat_code = {&c->controller, 1};
		c->indexes[1].number_of_bits = 4;
		c->indexes[1].repeat_code = {&c->member, 1};
		attach_payload(c, {0x07, 0x03});
	} else if (example == "conditional fields") {
		c->indexes[0].number_of_bits = 2;
		c->indexes[1].encode_type = VMF_ENCODE_OCTETS;
		c->indexes[1].number_of_bits = 56;
		c->indexes[1].refinements = &c->refinements[1];
		c->refinements[1].length_from = c->indexes[0].index_number;
		c->indexes[2].number_of_bits = 1;
		c->indexes[2].refinements = &c->refinements[2];
		c->term.field = c->indexes[0].index_number;
		c->term.values = &c->range;
		c->term.value_count = 1;
		c->condition = {VMF_CONDITION_ALL, &c->term, 1, false};
		c->refinements[2].when = &c->condition;
		/* Count 2, octets 80/01, conditional one-bit value 1. */
		attach_payload(c, {0x02, 0x06, 0x04});
	} else {
		c->indexes[0].encode_type = VMF_ENCODE_TEXT;
		c->indexes[0].number_of_bits = 64;
		c->indexes[0].refinements = &c->refinements[0];
		c->refinements[0].text_encoding = VMF_TEXT_UTF_8;
		attach_payload(c,
			{0x41, 0xe2, 0x82, 0xac, 0xf0, 0x90, 0x8d, 0x88});
	}
}

GIVEN("^a payload schema with a dependency on a future field$")
{
	cucumber::ScenarioScope<payload_context> c;

	initialize(c.get(), 2);
	c->term.field = c->indexes[1].index_number;
	c->term.values = &c->range;
	c->term.value_count = 1;
	c->condition = {VMF_CONDITION_ALL, &c->term, 1, false};
	c->refinements[0].when = &c->condition;
	c->indexes[0].refinements = &c->refinements[0];
}

WHEN("^I try to create codecs for that payload schema$")
{
	cucumber::ScenarioScope<payload_context> c;
	vmf_options options = {};

	options.profile = VMF_ENVELOPE_D1;
	options.limits = vmf_default_limits();
	options.schemas = &c->schemas;
	c->status = vmf_decoder_create(&options, &c->reader, nullptr);
	c->encoder_status =
		vmf_encoder_create(&options, false, &c->writer, nullptr);
}

THEN("^both codecs reject the schema before reading or writing$")
{
	cucumber::ScenarioScope<payload_context> c;

	EXPECT_EQ(VMF_INVALID_SCHEMA, c->status);
	EXPECT_EQ(VMF_INVALID_SCHEMA, c->encoder_status);
	EXPECT_EQ(nullptr, c->reader);
	EXPECT_EQ(nullptr, c->writer);
}

GIVEN("^the recurrence count is known before replay$")
{
	cucumber::ScenarioScope<payload_context> c;

	c->known_count = true;
}

GIVEN("^a valid payload schema with 4096 indexes$")
{
	cucumber::ScenarioScope<payload_context> c;

	initialize(c.get(), 0);
	c->lookup_indexes.resize(4096);
	c->lookup_parts.resize(4096);
	for (size_t i = 0; i < c->lookup_indexes.size(); ++i) {
		c->lookup_parts[i] = i + 1;
		c->lookup_indexes[i].index_number = {&c->lookup_parts[i], 1};
		c->lookup_indexes[i].number_of_bits = 8;
	}
	c->schema.indexes = c->lookup_indexes.data();
	c->schema.index_count = c->lookup_indexes.size();
	auto limits = vmf_default_limits();

	limits.max_schema_nodes = 4097;
	ASSERT_EQ(VMF_OK, vmf_schema_set_validate(
		&c->schemas, &limits, nullptr, nullptr));
}

THEN("^schema lookup returns the original last index "
	"and rejects missing addresses$")
{
	cucumber::ScenarioScope<payload_context> c;
	uint32_t address = 4096;

	EXPECT_EQ(&c->lookup_indexes.back(),
		vmf_schema_find(&c->schema, {&address, 1}));
	++address;
	EXPECT_EQ(nullptr, vmf_schema_find(&c->schema, {&address, 1}));
}

static vmf_status replay_payload_event(
	payload_context *context, const vmf_event *event)
{
	vmf_status status;

	do {
		uint8_t byte;
		vmf_output output = {&byte, 1, 0};

		status = vmf_encoder_write(context->writer, &output, event);
		if (output.used)
			context->encoded.push_back(byte);
	} while (status == VMF_NEED_OUTPUT);
	return status;
}

WHEN("^I decode and replay its payload with single-octet buffers$")
{
	cucumber::ScenarioScope<payload_context> context;
	auto *c = context.get();
	vmf_options options = {};

	options.profile = VMF_ENVELOPE_D1;
	options.limits = vmf_default_limits();
	options.limits.field_chunk_bytes = 4;
	options.schemas = &c->schemas;
	c->status = vmf_decoder_create(&options, &c->reader, nullptr);
	ASSERT_EQ(VMF_OK, c->status);
	c->status = vmf_encoder_create(&options, false, &c->writer, nullptr);
	ASSERT_EQ(VMF_OK, c->status);
	ASSERT_EQ(VMF_OK, vmf_decoder_begin_pdu(c->reader));
	size_t position = 0;
	vmf_input input = {c->wire.data(), 1, 0, false};

	for (size_t attempts = 0; attempts < 1000; ++attempts) {
		vmf_event event;

		c->status = vmf_decoder_next(c->reader, &input, &event);
		if (c->status == VMF_NEED_INPUT) {
			position += input.used;
			ASSERT_LT(position, c->wire.size());
			input = {c->wire.data() + position, 1, 0,
				position + 1 == c->wire.size()};
			continue;
		}
		if (c->status == VMF_DONE)
			break;
		ASSERT_EQ(VMF_OK, c->status);
		if (event.kind == VMF_EVENT_GROUP_BEGIN &&
			event.scope.kind == VMF_SCOPE_REPEAT_BODY) {
			EXPECT_TRUE(event.as.group.more_present);
			c->more.push_back(event.as.group.more);
		}
		if (event.kind == VMF_EVENT_FIELD) {
			if (event.field_kind == VMF_FIELD_UNSIGNED)
				c->numbers.push_back(event.as.field.value
						.unsigned_integer.value);
			if (event.field_kind == VMF_FIELD_TEXT) {
				c->texts.emplace_back(
					event.as.field.value.text.data,
					event.as.field.value.text.size);
				c->offsets.push_back(
					event.as.field.offset_units);
			}
			if (event.field_kind == VMF_FIELD_OCTETS)
				c->texts.emplace_back(
					reinterpret_cast<const char *>(
						event.as.field.value.octets
							.data),
					event.as.field.value.octets.size);
		}
		if (c->known_count && event.kind == VMF_EVENT_GROUP_BEGIN) {
			if (event.scope.kind == VMF_SCOPE_REPEAT)
				event.as.group.count = 2;
			if (event.scope.kind == VMF_SCOPE_REPEAT_BODY)
				event.as.group.more_present = false;
		}
		ASSERT_EQ(VMF_OK, replay_payload_event(c, &event));
	}
	EXPECT_EQ(VMF_DONE, c->status);
}

THEN("^the recurrence body flags and values are preserved$")
{
	cucumber::ScenarioScope<payload_context> c;

	EXPECT_EQ((std::vector<bool>{true, false}), c->more);
	EXPECT_EQ((std::vector<uint64_t>{3, 12}), c->numbers);
}

THEN("^the field-length bytes and conditional value are preserved$")
{
	cucumber::ScenarioScope<payload_context> c;

	EXPECT_EQ((std::vector<uint64_t>{2, 1}), c->numbers);
	ASSERT_EQ(1u, c->texts.size());
	EXPECT_EQ(std::string("\x80\x01", 2), c->texts[0]);
}

THEN("^the text chunks preserve complete characters and wire offsets$")
{
	cucumber::ScenarioScope<payload_context> c;

	ASSERT_EQ(2u, c->texts.size());
	EXPECT_EQ("A\xe2\x82\xac", c->texts[0]);
	EXPECT_EQ("\xf0\x90\x8d\x88", c->texts[1]);
	EXPECT_EQ((std::vector<uint64_t>{0, 4}), c->offsets);
}

THEN("^the payload control replay matches every original octet$")
{
	cucumber::ScenarioScope<payload_context> c;

	EXPECT_EQ(VMF_DONE, c->status);
	EXPECT_EQ(c->wire, c->encoded);
}

} /* namespace */
