#include <gtest/gtest.h>
#include <cucumber-cpp/autodetect.hpp>
#include <vmf/schema_json.h>
#include <string>
#include <vector>

namespace
{

struct json_context {
	std::string input;
	std::string example;
	uint32_t addresses[4][3] = {{1}, {2}, {3}, {4}};
	vmf_group_id groups[4] = {1, 1, 1, 1};
	vmf_index_schema indexes[4] = {};
	vmf_index_refinements refinements[4] = {};
	vmf_repeat_component memberships[4] = {};
	vmf_repeat_component inner_memberships[2] = {};
	vmf_repeat_definition repeat = {};
	vmf_message_schema schema = {};
	vmf_schema_set native = {};
	vmf_schema_json_document *document = nullptr;
	vmf_schema_json_error error = {};
	vmf_status status = VMF_OK;
	std::vector<uint8_t> wire;
	std::vector<uint8_t> encoded;
	std::vector<std::string> loaded_events;
	std::vector<std::string> native_events;
	std::vector<uint64_t> numbers;

	~json_context()
	{
		vmf_schema_json_destroy(document);
	}
};

static void attach_payload(json_context *c,
	std::initializer_list<uint8_t> body)
{
	vmf_message_header message = {};
	vmf_header_c header = {};

	ASSERT_EQ(VMF_OK,
		vmf_schema_message_id(&c->native, &c->schema, &message.id));
	message.has_body = true;
	message.precedence = 7;
	message.message_size_octets = {true, (uint32_t)body.size()};
	header.fields.messages = &message;
	header.fields.message_count = 1;
	vmf_options options = vmf_options_for_schemas(&c->native);

	options.schemas = nullptr;
	vmf_encoder *writer = nullptr;

	ASSERT_EQ(VMF_OK,
		vmf_encoder_create(&options, false, &writer, nullptr));
	ASSERT_EQ(VMF_OK, vmf_encoder_begin_envelope(writer));
	vmf_event event = {};

	event.kind = VMF_EVENT_ENVELOPE;
	event.as.envelope.wire_version = VMF_ENVELOPE_C;
	event.as.envelope.header = &header;
	event.as.envelope.object_size = sizeof(header);
	vmf_status status;

	do {
		uint8_t octet;
		vmf_output output = {&octet, 1, 0};

		status = vmf_encoder_write(writer, &output, &event);
		if (output.used)
			c->wire.push_back(octet);
	} while (status == VMF_NEED_OUTPUT);
	vmf_encoder_destroy(writer);
	ASSERT_EQ(VMF_OK, status);
	/* Bodies below are manually derived independent payload vectors. */
	c->wire.insert(c->wire.end(), body);
}

static void field(json_context *c, size_t position, uint64_t width)
{
	c->indexes[position].index_number = {c->addresses[position], 1};
	c->indexes[position].number_of_bits = width;
}

GIVEN("^a JSON definition of a synthetic "
	"(scalar|recurrence|presence|absence|field-count|UTF-8 text|"
	"literal text|group recurrence|group absence|nested recurrence) "
	"payload$")
{
	REGEX_PARAM(std::string, example);
	cucumber::ScenarioScope<json_context> context;
	auto *c = context.get();
	std::string indexes;
	std::string extra;

	c->example = example;
	c->schema.message_number = {1, 1};
	c->schema.indexes = c->indexes;
	c->schema.index_count = 1;
	c->native.envelope = VMF_ENVELOPE_C;
	c->native.messages = &c->schema;
	c->native.message_count = 1;
	if (example == "scalar") {
		field(c, 0, 5);
		field(c, 1, 13);
		c->schema.index_count = 2;
		indexes = "{\"index_number\":\"1.\",\"number_of_bits\":5},"
			"{\"index_number\":\"2.\",\"number_of_bits\":13}";
		attach_payload(c, {0x83, 0x46, 0x02});
	} else if (example == "recurrence") {
		field(c, 0, 0);
		field(c, 1, 4);
		c->addresses[1][0] = 1;
		c->addresses[1][1] = 1;
		c->indexes[1].index_number.count = 2;
		c->indexes[0].indicator = VMF_INDICATOR_FRI;
		c->memberships[0] = {1, {true, 5}};
		c->memberships[1].id = 1;
		c->indexes[0].repeat_code = {&c->memberships[0], 1};
		c->indexes[1].repeat_code = {&c->memberships[1], 1};
		c->schema.index_count = 2;
		indexes = "{\"index_number\":\"1.\",\"indicator\":\"fri\","
			"\"repeat_code\":\"R1(5)\"},"
			"{\"index_number\":\"1.1\",\"number_of_bits\":4,"
			"\"repeat_code\":\"R1\"}";
		attach_payload(c, {0x07, 0x03});
	} else if (example == "group recurrence" ||
		example == "group absence") {
		for (size_t i = 0; i < 4; ++i) {
			field(c, i, i < 2 ? 0 : i == 2 ? 7 : 9);
			c->addresses[i][0] = 4;
			if (!i)
				continue;
			c->addresses[i][1] = 1;
			c->addresses[i][2] = (uint32_t)i;
			c->indexes[i].index_number.count = 3;
			c->indexes[i].group_code = {&c->groups[i], 1};
			c->memberships[i].id = 1;
			c->indexes[i].repeat_code = {&c->memberships[i], 1};
		}
		c->indexes[0].indicator = VMF_INDICATOR_GPI;
		c->indexes[0].target.kind = VMF_TARGET_GROUP;
		c->indexes[0].target.as.group = 1;
		c->indexes[1].indicator = VMF_INDICATOR_GRI;
		c->memberships[1].maximum = {true, 4};
		c->schema.index_count = 4;
		indexes = "{\"index_number\":\"4.\",\"indicator\":\"gpi\","
			"\"target\":\"G1\"},"
			"{\"index_number\":\"4.1.1\",\"indicator\":\"gri\","
			"\"group_code\":\"G1\",\"repeat_code\":\"R1(4)\"},"
			"{\"index_number\":\"4.1.2\",\"number_of_bits\":7,"
			"\"group_code\":\"G1\",\"repeat_code\":\"R1\"},"
			"{\"index_number\":\"4.1.3\",\"number_of_bits\":9,"
			"\"group_code\":\"G1\",\"repeat_code\":\"R1\"}";
		if (example == "group absence")
			attach_payload(c, {0});
		else
			attach_payload(c, {0x0f, 0x02, 0x2a, 0x08, 0});
	} else if (example == "nested recurrence") {
		field(c, 0, 0);
		field(c, 1, 0);
		field(c, 2, 3);
		c->addresses[1][0] = c->addresses[2][0] = 1;
		c->addresses[1][1] = c->addresses[2][1] = 1;
		c->addresses[2][2] = 1;
		c->indexes[1].index_number.count = 2;
		c->indexes[2].index_number.count = 3;
		c->indexes[0].indicator = c->indexes[1].indicator =
			VMF_INDICATOR_GRI;
		c->memberships[0] = {1, {true, 3}};
		c->memberships[1] = {1, {false, 0}};
		c->memberships[2] = {2, {true, 2}};
		c->inner_memberships[0].id = 1;
		c->inner_memberships[1].id = 2;
		c->indexes[0].repeat_code = {c->memberships, 1};
		c->indexes[1].repeat_code = {c->memberships + 1, 2};
		c->indexes[2].repeat_code = {c->inner_memberships, 2};
		c->schema.index_count = 3;
		indexes = "{\"index_number\":\"1\",\"indicator\":\"gri\","
			"\"repeat_code\":\"R1(3)\"},"
			"{\"index_number\":\"1.1\",\"indicator\":\"gri\","
			"\"repeat_code\":\"R1/R2(2)\"},"
			"{\"index_number\":\"1.1.1\",\"number_of_bits\":3,"
			"\"repeat_code\":\"R1/R2\"}";
		attach_payload(c, {0x4e, 0x01});
	} else if (example == "presence" || example == "absence") {
		field(c, 0, 0);
		field(c, 1, 9);
		c->addresses[1][0] = 1;
		c->addresses[1][1] = 1;
		c->indexes[1].index_number.count = 2;
		c->indexes[0].indicator = VMF_INDICATOR_FPI;
		c->indexes[0].target.kind = VMF_TARGET_INDEX;
		c->indexes[0].target.as.index = c->indexes[1].index_number;
		c->schema.index_count = 2;
		indexes = "{\"index_number\":\"1.\",\"indicator\":\"fpi\","
			"\"target\":\"1.1\"},"
			"{\"index_number\":\"1.1\",\"number_of_bits\":9}";
		if (example == "presence")
			attach_payload(c, {0xab, 0x02});
		else
			attach_payload(c, {0x00});
	} else if (example == "field-count") {
		field(c, 0, 2);
		field(c, 1, 3);
		c->memberships[1].id = 1;
		c->indexes[1].repeat_code = {&c->memberships[1], 1};
		c->repeat.id = 1;
		c->repeat.kind = VMF_REPEAT_FIELD;
		c->repeat.count_from = c->indexes[0].index_number;
		c->repeat.maximum = 3;
		c->schema.repeats = &c->repeat;
		c->schema.repeat_count = 1;
		c->schema.index_count = 2;
		indexes = "{\"index_number\":\"1.\",\"number_of_bits\":2},"
			"{\"index_number\":\"2.\",\"number_of_bits\":3,"
			"\"repeat_code\":\"R1\"}";
		extra = ",\"repeats\":[{\"repeat_code\":\"R1\","
			"\"count_from\":\"1.\",\"maximum\":3}]";
		attach_payload(c, {0x76});
	} else if (example == "UTF-8 text") {
		field(c, 0, 64);
		c->indexes[0].encode_type = VMF_ENCODE_TEXT;
		c->indexes[0].refinements = &c->refinements[0];
		c->refinements[0].text_encoding = VMF_TEXT_UTF_8;
		indexes = "{\"index_number\":\"1.\",\"number_of_bits\":64,"
			"\"encode_type\":\"text\",\"text_encoding\":\"utf_8\"}";
		attach_payload(c,
			{0x41, 0xe2, 0x82, 0xac, 0xf0, 0x90, 0x8d, 0x88});
	} else {
		field(c, 0, 14);
		c->indexes[0].encode_type = VMF_ENCODE_LITERAL;
		indexes = "{\"index_number\":\"1.\",\"number_of_bits\":14,"
			"\"encode_type\":\"literal\"}";
		attach_payload(c, {0xc1, 0x3f});
	}
	c->input = "{\"format\":\"vmf-schema/1\",\"envelope\":\"C\","
		"\"messages\":[{\"message_number\":\"K01.1\",\"indexes\":[" +
		indexes + "]" + extra + "}]}";
}

WHEN("^I load the JSON payload schema and release its source bytes$")
{
	cucumber::ScenarioScope<json_context> c;
	auto options = vmf_schema_json_default_options();

	c->status = vmf_schema_json_load(c->input.data(), c->input.size(),
		&options, &c->document, &c->error);
	ASSERT_EQ(VMF_OK, c->status);
	c->input.assign(c->input.size(), 'x');
}

static std::string identity(const vmf_event *event)
{
	std::string result = std::to_string(event->kind) + ":";

	if (event->index) {
		for (size_t i = 0; i < event->index->index_number.count; ++i)
			result += std::to_string(
				event->index->index_number.parts[i]) + ".";
	} else {
		result += std::to_string(event->scope.kind) + "/" +
			std::to_string(event->scope.id);
	}
	return result;
}

static vmf_status replay(const vmf_schema_set *schemas,
	const std::vector<uint8_t> &wire, std::vector<uint8_t> *encoded,
	std::vector<std::string> *events, std::vector<uint64_t> *numbers)
{
	vmf_options options = vmf_options_for_schemas(schemas);

	options.limits.field_chunk_bytes = 4;
	vmf_decoder *reader = nullptr;
	vmf_encoder *writer = nullptr;
	vmf_status status = vmf_decoder_create(&options, &reader, nullptr);

	if (status == VMF_OK)
		status = vmf_encoder_create(&options, false, &writer, nullptr);
	if (status == VMF_OK)
		status = vmf_decoder_begin_pdu(reader);
	size_t position = 0;
	vmf_input input = {wire.data(), wire.empty() ? 0u : 1u, 0, false};

	for (size_t attempts = 0; status == VMF_OK && attempts < 1000;
		++attempts) {
		vmf_event event = {};

		status = vmf_decoder_next(reader, &input, &event);
		if (status == VMF_NEED_INPUT) {
			position += input.used;
			if (position >= wire.size())
				break;
			input = {wire.data() + position, 1, 0,
				position + 1 == wire.size()};
			status = VMF_OK;
			continue;
		}
		if (status != VMF_OK)
			break;
		std::string observed = identity(&event);

		if (event.kind == VMF_EVENT_FIELD) {
			observed += ":" +
				std::to_string(event.as.field.offset_units);
			observed += ":" +
				std::to_string(event.as.field.total_units);
			if (event.field_kind == VMF_FIELD_UNSIGNED) {
				uint64_t value = event.as.field.value
					.unsigned_integer.value;

				numbers->push_back(value);
				observed += ":" + std::to_string(value);
			} else if (event.field_kind == VMF_FIELD_TEXT) {
				observed.append(event.as.field.value.text.data,
					event.as.field.value.text.size);
			}
		} else if (event.kind == VMF_EVENT_GROUP_BEGIN) {
			observed += event.as.group.more_present
				? ":flag" : ":none";
			observed += event.as.group.more ? ":more" : ":last";
		}
		events->push_back(observed);
		do {
			uint8_t octet;
			vmf_output output = {&octet, 1, 0};

			status = vmf_encoder_write(writer, &output, &event);
			if (output.used)
				encoded->push_back(octet);
		} while (status == VMF_NEED_OUTPUT);
	}
	vmf_decoder_destroy(reader);
	vmf_encoder_destroy(writer);
	return status;
}

WHEN("^I decode and replay the JSON payload using single-octet buffers$")
{
	cucumber::ScenarioScope<json_context> c;
	std::vector<uint8_t> native_bytes;
	std::vector<uint64_t> native_numbers;

	ASSERT_EQ(VMF_DONE, replay(&c->native, c->wire, &native_bytes,
		&c->native_events, &native_numbers));
	EXPECT_EQ(c->wire, native_bytes);
	c->status = replay(vmf_schema_json_schemas(c->document), c->wire,
		&c->encoded, &c->loaded_events, &c->numbers);
}

THEN("^the JSON schema produces the handwritten schema's events and bytes$")
{
	cucumber::ScenarioScope<json_context> c;

	EXPECT_EQ(VMF_DONE, c->status);
	EXPECT_EQ(c->wire, c->encoded);
	EXPECT_EQ(c->native_events, c->loaded_events);
	if (c->example == "scalar")
		EXPECT_EQ((std::vector<uint64_t>{3, 0x1234}), c->numbers);
	if (c->example == "recurrence")
		EXPECT_EQ((std::vector<uint64_t>{3, 12}), c->numbers);
	if (c->example == "presence")
		EXPECT_EQ((std::vector<uint64_t>{0x155}), c->numbers);
	if (c->example == "absence")
		EXPECT_TRUE(c->numbers.empty());
	if (c->example == "field-count")
		EXPECT_EQ((std::vector<uint64_t>{2, 5, 3}), c->numbers);
	if (c->example == "group recurrence")
		EXPECT_EQ((std::vector<uint64_t>{3, 257, 5, 2}), c->numbers);
	if (c->example == "group absence")
		EXPECT_TRUE(c->numbers.empty());
	if (c->example == "nested recurrence")
		EXPECT_EQ((std::vector<uint64_t>{3, 5}), c->numbers);
}

GIVEN("^a JSON schema set with a valid entry followed by an invalid entry$")
{
	cucumber::ScenarioScope<json_context> c;

	c->input = "{\"format\":\"vmf-schema/1\",\"envelope\":\"C\","
		"\"messages\":[{\"message_number\":\"K01.1\",\"indexes\":[{"
		"\"index_number\":\"1.\",\"number_of_bits\":8}]},"
		"{\"message_number\":\"K01.2\",\"indexes\":[{"
		"\"index_number\":\"2.\",\"number_of_bits\":0}]}]}";
}

WHEN("^I attempt to load the complete JSON schema set$")
{
	cucumber::ScenarioScope<json_context> c;
	auto options = vmf_schema_json_default_options();

	c->status = vmf_schema_json_load(c->input.data(), c->input.size(),
		&options, &c->document, &c->error);
}

THEN("^no schema owner is published and the second entry is diagnosed$")
{
	cucumber::ScenarioScope<json_context> c;

	EXPECT_EQ(VMF_INVALID_SCHEMA, c->status);
	EXPECT_EQ(nullptr, c->document);
	EXPECT_EQ(1u, c->error.schema_index);
	EXPECT_EQ(0u, c->error.index_position);
}

} /* namespace */
