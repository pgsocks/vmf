#include <gtest/gtest.h>
#include <cucumber-cpp/autodetect.hpp>
#include <vmf/schema_json.h>
#include <cstring>
#include <string>
#include <vector>

namespace
{

struct json_context {
	std::string input;
	std::string example;
	std::string expected_path;
	std::string expected_related_path;
	uint32_t addresses[4][3] = {{1}, {2}, {3}, {4}};
	vmf_group_id groups[4] = {1, 1, 1, 1};
	vmf_index_schema indexes[4] = {};
	vmf_index_refinements refinements[4] = {};
	vmf_repeat_component memberships[4] = {};
	vmf_repeat_component inner_memberships[2] = {};
	vmf_repeat_definition repeat = {};
	vmf_condition condition = {};
	vmf_condition_term term = {};
	vmf_code_range allowed = {};
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

static void field(json_context *c, size_t position, uint64_t width,
	const char *key = nullptr)
{
	c->indexes[position].index_number = {c->addresses[position], 1};
	c->indexes[position].number_of_bits = width;
	if (key)
		c->indexes[position].key = {key, std::strlen(key)};
}

GIVEN("^a JSON definition of a synthetic "
	"(scalar|recurrence|presence|absence|field-count|UTF-8 text|"
	"literal text|group recurrence|group absence|nested recurrence|"
	"recurring optional field|single-field group|named dependencies|"
	"counted optional group) "
	"payload$")
{
	REGEX_PARAM(std::string, example);
	cucumber::ScenarioScope<json_context> context;
	auto *c = context.get();
	std::string elements;

	c->example = example;
	c->schema.message_number = {1, 1};
	c->schema.indexes = c->indexes;
	c->schema.index_count = 1;
	c->native.envelope = VMF_ENVELOPE_C;
	c->native.messages = &c->schema;
	c->native.message_count = 1;
	if (example == "scalar") {
		field(c, 0, 5, "first");
		field(c, 1, 13, "second");
		c->schema.index_count = 2;
		elements = "{\"key\":\"first\",\"number_of_bits\":5},"
			"{\"key\":\"second\",\"number_of_bits\":13}";
		attach_payload(c, {0x83, 0x46, 0x02});
	} else if (example == "recurrence") {
		field(c, 0, 0);
		field(c, 1, 4, "value");
		c->addresses[1][0] = 1;
		c->addresses[1][1] = 1;
		c->indexes[1].index_number.count = 2;
		c->indexes[0].indicator = VMF_INDICATOR_FRI;
		c->memberships[0] = {1, {true, 5}};
		c->memberships[1].id = 1;
		c->indexes[0].repeat_code = {&c->memberships[0], 1};
		c->indexes[1].repeat_code = {&c->memberships[1], 1};
		c->schema.index_count = 2;
		elements = "{\"recurrence\":{\"maximum\":5,\"body\":{"
			"\"key\":\"value\",\"number_of_bits\":4}}}";
		attach_payload(c, {0x07, 0x03});
	} else if (example == "group recurrence" ||
		example == "group absence") {
		for (size_t i = 0; i < 4; ++i) {
			field(c, i, i < 2 ? 0 : i == 2 ? 7 : 9,
				i < 2 ? nullptr : i == 2 ? "x" : "y");
			c->addresses[i][0] = 1;
			if (!i)
				continue;
			c->addresses[i][1] = 1;
			c->addresses[i][2] = (uint32_t)(i - 1);
			c->indexes[i].index_number.count = i == 1 ? 2 : 3;
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
		elements = "{\"optional\":{\"recurrence\":{\"maximum\":4,"
			"\"body\":[{\"key\":\"x\",\"number_of_bits\":7},"
			"{\"key\":\"y\",\"number_of_bits\":9}]}}}";
		if (example == "group absence")
			attach_payload(c, {0});
		else
			attach_payload(c, {0x0f, 0x02, 0x2a, 0x08, 0});
	} else if (example == "nested recurrence") {
		field(c, 0, 0);
		field(c, 1, 0);
		field(c, 2, 3, "value");
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
		elements = "{\"recurrence\":{\"maximum\":3,\"body\":[{"
			"\"recurrence\":{\"maximum\":2,\"body\":[{"
			"\"key\":\"value\",\"number_of_bits\":3}]}}]}}";
		attach_payload(c, {0x4e, 0x01});
	} else if (example == "presence" || example == "absence") {
		field(c, 0, 0);
		field(c, 1, 9, "value");
		c->addresses[1][0] = 1;
		c->addresses[1][1] = 1;
		c->indexes[1].index_number.count = 2;
		c->indexes[0].indicator = VMF_INDICATOR_FPI;
		c->indexes[0].target.kind = VMF_TARGET_INDEX;
		c->indexes[0].target.as.index = c->indexes[1].index_number;
		c->schema.index_count = 2;
		elements = "{\"optional\":{\"key\":\"value\","
			"\"number_of_bits\":9}}";
		if (example == "presence")
			attach_payload(c, {0xab, 0x02});
		else
			attach_payload(c, {0x00});
	} else if (example == "field-count") {
		field(c, 0, 2, "count");
		field(c, 1, 3, "value");
		c->addresses[1][1] = 1;
		c->indexes[1].index_number.count = 2;
		c->memberships[1].id = 1;
		c->indexes[1].repeat_code = {&c->memberships[1], 1};
		c->repeat.id = 1;
		c->repeat.kind = VMF_REPEAT_FIELD;
		c->repeat.count_from = c->indexes[0].index_number;
		c->repeat.maximum = 3;
		c->schema.repeats = &c->repeat;
		c->schema.repeat_count = 1;
		c->schema.index_count = 2;
		elements = "{\"key\":\"count\",\"number_of_bits\":2},"
			"{\"repeat\":{\"count_from\":\"count\",\"maximum\":3,"
			"\"body\":{\"key\":\"value\",\"number_of_bits\":3}}}";
		attach_payload(c, {0x76});
	} else if (example == "counted optional group") {
		field(c, 0, 2, "count");
		field(c, 1, 0);
		field(c, 2, 3, "value");
		c->addresses[1][1] = 1;
		c->addresses[2][0] = 2;
		c->addresses[2][1] = c->addresses[2][2] = 1;
		c->indexes[1].index_number.count = 2;
		c->indexes[2].index_number.count = 3;
		c->indexes[1].indicator = VMF_INDICATOR_GPI;
		c->indexes[1].target.kind = VMF_TARGET_GROUP;
		c->indexes[1].target.as.group = 1;
		c->indexes[2].group_code = {&c->groups[2], 1};
		for (size_t i = 1; i < 3; ++i) {
			c->memberships[i].id = 1;
			c->indexes[i].repeat_code = {&c->memberships[i], 1};
		}
		c->repeat.id = 1;
		c->repeat.kind = VMF_REPEAT_FIELD;
		c->repeat.count_from = c->indexes[0].index_number;
		c->repeat.maximum = 3;
		c->schema.repeats = &c->repeat;
		c->schema.repeat_count = 1;
		c->schema.index_count = 3;
		elements = "{\"key\":\"count\",\"number_of_bits\":2},"
			"{\"repeat\":{\"count_from\":\"count\",\"maximum\":3,"
			"\"body\":{\"optional\":[{\"key\":\"value\","
			"\"number_of_bits\":3}]}}}";
		attach_payload(c, {0x2e});
	} else if (example == "recurring optional field") {
		field(c, 0, 0);
		field(c, 1, 0);
		field(c, 2, 3, "value");
		c->addresses[1][0] = c->addresses[2][0] = 1;
		c->addresses[1][1] = c->addresses[2][1] = 1;
		c->addresses[2][2] = 1;
		c->indexes[1].index_number.count = 2;
		c->indexes[2].index_number.count = 3;
		c->indexes[0].indicator = VMF_INDICATOR_GRI;
		c->indexes[1].indicator = VMF_INDICATOR_FPI;
		c->indexes[1].target.kind = VMF_TARGET_INDEX;
		c->indexes[1].target.as.index = c->indexes[2].index_number;
		for (size_t i = 0; i < 3; ++i) {
			c->memberships[i].id = 1;
			c->indexes[i].repeat_code = {&c->memberships[i], 1};
		}
		c->memberships[0].maximum = {true, 3};
		c->schema.index_count = 3;
		elements = "{\"recurrence\":{\"maximum\":3,\"body\":{"
			"\"optional\":{\"key\":\"value\","
			"\"number_of_bits\":3}}}}";
		attach_payload(c, {0x59});
	} else if (example == "single-field group") {
		field(c, 0, 0);
		field(c, 1, 3, "value");
		c->addresses[1][0] = c->addresses[1][1] = 1;
		c->indexes[1].index_number.count = 2;
		c->indexes[0].indicator = VMF_INDICATOR_GRI;
		c->memberships[0] = {1, {true, 3}};
		c->memberships[1].id = 1;
		c->indexes[0].repeat_code = {&c->memberships[0], 1};
		c->indexes[1].repeat_code = {&c->memberships[1], 1};
		c->schema.index_count = 2;
		elements = "{\"recurrence\":{\"maximum\":3,\"body\":[{"
			"\"key\":\"value\",\"number_of_bits\":3}]}}";
		attach_payload(c, {0xa7});
	} else if (example == "named dependencies") {
		field(c, 0, 2, "length");
		field(c, 1, 3, "payload");
		c->indexes[1].encode_type = VMF_ENCODE_BITS;
		c->indexes[1].refinements = &c->refinements[1];
		c->refinements[1].length_from = c->indexes[0].index_number;
		c->allowed = {2, 2};
		c->term.field = c->indexes[0].index_number;
		c->term.values = &c->allowed;
		c->term.value_count = 1;
		c->condition.terms = &c->term;
		c->condition.term_count = 1;
		c->refinements[1].when = &c->condition;
		c->schema.index_count = 2;
		elements = "{\"key\":\"length\",\"number_of_bits\":2},"
			"{\"key\":\"payload\",\"type\":\"bits\","
			"\"number_of_bits\":3,\"length_from\":\"length\","
			"\"when\":{\"all\":[{\"field\":\"length\","
			"\"values\":[{\"first\":2,\"last\":2}]}]}}";
		attach_payload(c, {0x06});
	} else if (example == "UTF-8 text") {
		field(c, 0, 64, "text");
		c->indexes[0].encode_type = VMF_ENCODE_TEXT;
		c->indexes[0].refinements = &c->refinements[0];
		c->refinements[0].text_encoding = VMF_TEXT_UTF_8;
		elements = "{\"key\":\"text\",\"number_of_bits\":64,"
			"\"type\":\"text\",\"text_encoding\":\"utf_8\"}";
		attach_payload(c,
			{0x41, 0xe2, 0x82, 0xac, 0xf0, 0x90, 0x8d, 0x88});
	} else {
		field(c, 0, 14, "text");
		c->indexes[0].encode_type = VMF_ENCODE_LITERAL;
		elements = "{\"key\":\"text\",\"number_of_bits\":14,"
			"\"type\":\"literal\"}";
		attach_payload(c, {0xc1, 0x3f});
	}
	c->input = "{\"format\":\"vmf-schema/1\",\"envelope\":\"C\","
		"\"messages\":[{\"message_number\":\"K01.1\",\"elements\":[" +
		elements + "]}]}";
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
	if (c->example == "counted optional group")
		EXPECT_EQ((std::vector<uint64_t>{2, 5}), c->numbers);
	if (c->example == "group recurrence")
		EXPECT_EQ((std::vector<uint64_t>{3, 257, 5, 2}), c->numbers);
	if (c->example == "group absence")
		EXPECT_TRUE(c->numbers.empty());
	if (c->example == "nested recurrence")
		EXPECT_EQ((std::vector<uint64_t>{3, 5}), c->numbers);
	if (c->example == "recurring optional field")
		EXPECT_EQ((std::vector<uint64_t>{5}), c->numbers);
	if (c->example == "single-field group")
		EXPECT_EQ((std::vector<uint64_t>{3, 5}), c->numbers);
	if (c->example == "named dependencies")
		EXPECT_EQ((std::vector<uint64_t>{2}), c->numbers);
}

GIVEN("^a JSON schema set with a valid entry followed by an invalid entry$")
{
	cucumber::ScenarioScope<json_context> c;

	c->input = "{\"format\":\"vmf-schema/1\",\"envelope\":\"C\","
		"\"messages\":[{\"message_number\":\"K01.1\",\"elements\":[{"
		"\"key\":\"value\",\"number_of_bits\":8}]},"
		"{\"message_number\":\"K01.2\",\"elements\":[{"
		"\"key\":\"value\",\"number_of_bits\":0}]}]}";
}

GIVEN("^a nested JSON schema containing "
	"(an unknown length reference|a duplicate field key)$")
{
	REGEX_PARAM(std::string, problem);
	cucumber::ScenarioScope<json_context> c;
	std::string elements;

	if (problem == "an unknown length reference") {
		elements = "{\"optional\":{\"key\":\"payload\","
			"\"type\":\"bits\",\"number_of_bits\":8,"
			"\"length_from\":\"missing\"}}";
		c->expected_path =
			"/messages/0/elements/0/optional/length_from";
	} else {
		elements = "{\"key\":\"value\",\"number_of_bits\":2},"
			"{\"optional\":{\"key\":\"value\","
			"\"number_of_bits\":8}}";
		c->expected_path = "/messages/0/elements/1/optional/key";
		c->expected_related_path = "/messages/0/elements/0/key";
	}
	c->input = "{\"format\":\"vmf-schema/1\",\"envelope\":\"C\","
		"\"messages\":[{\"message_number\":\"K01.1\",\"elements\":[" +
		elements + "]}]}";
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
	EXPECT_STREQ("/messages/1/elements/0/number_of_bits",
		c->error.path);
}

THEN("^the failing authored property is diagnosed after source release$")
{
	cucumber::ScenarioScope<json_context> c;

	c->input.assign(c->input.size(), 'x');
	c->input.clear();
	c->input.shrink_to_fit();
	EXPECT_EQ(VMF_INVALID_SCHEMA, c->status);
	EXPECT_EQ(nullptr, c->document);
	EXPECT_STREQ(c->expected_path.c_str(), c->error.path);
	EXPECT_FALSE(c->error.path_truncated);
	EXPECT_STREQ(c->expected_related_path.c_str(), c->error.related_path);
	EXPECT_FALSE(c->error.related_path_truncated);
	EXPECT_NE(nullptr, c->error.message);
}

struct portable_context {
	vmf_schema_json_document *document = nullptr;
	const vmf_schema_set *schemas = nullptr;
	const vmf_message_schema *message = nullptr;
	const vmf_index_schema *index = nullptr;
	vmf_decoder *reader = nullptr;
	vmf_encoder *writer = nullptr;
	vmf_status status = VMF_OK;
	std::vector<vmf_message_standard> editions;
	size_t matching_envelopes = 0;
	size_t requests = 0;

	~portable_context()
	{
		vmf_decoder_destroy(reader);
		vmf_encoder_destroy(writer);
		vmf_schema_json_destroy(document);
	}
};

GIVEN("^an envelope-independent JSON registry for "
	"(MIL-STD-6017B|MIL-STD-6017E|two editions of the same message)$")
{
	REGEX_PARAM(std::string, definition);
	cucumber::ScenarioScope<portable_context> c;
	std::string edition = definition == "MIL-STD-6017E" ?
		definition : "MIL-STD-6017B";
	std::string message = "{\"message_number\":\"K01.1\","
		"\"message_standard\":\"" + edition + "\",\"elements\":[{"
		"\"key\":\"value\",\"number_of_bits\":8}]}";
	std::string input = "{\"format\":\"vmf-schema/1\",\"messages\":[" +
		message;

	c->editions.push_back(edition == "MIL-STD-6017E" ?
		VMF_STANDARD_6017E : VMF_STANDARD_6017B);
	if (definition == "two editions of the same message") {
		input += ",{\"message_number\":\"K01.1\","
			"\"message_standard\":\"MIL-STD-6017C\",\"elements\":[{"
			"\"key\":\"value\",\"number_of_bits\":8}]}";
		c->editions.push_back(VMF_STANDARD_6017C);
	}
	input += "]}";
	auto options = vmf_schema_json_default_options();
	vmf_schema_json_error error = {};

	ASSERT_EQ(VMF_OK, vmf_schema_json_load(input.data(), input.size(),
		&options, &c->document, &error));
	c->schemas = vmf_schema_json_schemas(c->document);
	ASSERT_NE(nullptr, c->schemas);
	ASSERT_EQ(c->editions.size(), c->schemas->message_count);
	c->message = c->schemas->messages;
	c->index = c->message->indexes;
	input.assign(input.size(), 'x');
}

static vmf_event_action portable_value(void *user,
	const vmf_encode_request *request, vmf_encode_reply *reply)
{
	auto *c = static_cast<portable_context *>(user);

	++c->requests;
	if (request->kind != VMF_ENCODE_REQUEST_FIELD ||
		request->schema != c->message || request->index != c->index ||
		request->field_kind != VMF_FIELD_UNSIGNED)
		return VMF_EVENT_ABORT;
	reply->kind = VMF_ENCODE_REQUEST_FIELD;
	reply->as.field.unsigned_integer.value = 0x5a;
	return VMF_EVENT_CONTINUE;
}

static void portable_roundtrip(portable_context *c,
	vmf_envelope_version profile)
{
	vmf_message_header message = {};
	vmf_header_fields fields = {};
	vmf_header header = {};

	message.message_size_octets = {true, 1};
	message.precedence = profile <= VMF_ENVELOPE_C ? 7 : 0;
	fields.messages = &message;
	fields.message_count = 1;
	switch (profile) {
	case VMF_ENVELOPE_B:
		header.b = {fields};
		break;
	case VMF_ENVELOPE_C:
		header.c = {fields, false};
		break;
	case VMF_ENVELOPE_D:
		header.d = {fields, false};
		break;
	case VMF_ENVELOPE_D1:
		header.d1 = {fields, false};
		break;
	default:
		FAIL() << "unsupported test profile";
	}
	vmf_options options = vmf_options_for_schemas(c->schemas);

	options.profile = profile;
	const vmf_message_schema *bindings[] = {c->message};

	ASSERT_EQ(VMF_OK,
		vmf_encoder_create(&options, false, &c->writer, nullptr));
	ASSERT_EQ(VMF_OK, vmf_encoder_begin_schema_pdu(c->writer, &header,
		bindings, 1));
	EXPECT_EQ(0u, message.id.user_message_format);
	EXPECT_FALSE(message.id.identification_present);
	EXPECT_FALSE(message.has_body);
	std::vector<uint8_t> wire;
	size_t before = c->requests;
	vmf_status status = VMF_OK;

	for (size_t attempt = 0; attempt < 128; ++attempt) {
		uint8_t byte = 0;
		vmf_output output = {&byte, 1, 0};
		size_t delivered = 0;

		status = vmf_encoder_dispatch(c->writer, &output,
			portable_value, c, &delivered);
		if (output.used)
			wire.push_back(byte);
		if (status != VMF_NEED_OUTPUT)
			break;
	}
	ASSERT_EQ(VMF_DONE, status);
	ASSERT_FALSE(wire.empty());
	EXPECT_EQ(static_cast<unsigned>(profile), wire.front() & 0x0fu);
	EXPECT_EQ(0x5au, wire.back());
	EXPECT_EQ(before + 1, c->requests);
	vmf_encoder_destroy(c->writer);
	c->writer = nullptr;
	ASSERT_EQ(VMF_OK,
		vmf_decoder_create(&options, &c->reader, nullptr));
	ASSERT_EQ(VMF_OK, vmf_decoder_begin_pdu(c->reader));
	vmf_input input = {wire.data(), wire.size(), 0, true};
	size_t header_count = 0;
	size_t field_count = 0;
	size_t begin_count = 0;

	for (size_t attempt = 0; attempt < 32; ++attempt) {
		vmf_event event = {};

		status = vmf_decoder_next(c->reader, &input, &event);
		if (status != VMF_OK)
			break;
		if (event.kind == VMF_EVENT_ENVELOPE) {
			++header_count;
			EXPECT_EQ(profile, event.as.envelope.wire_version);
		} else if (event.kind == VMF_EVENT_MESSAGE_BEGIN) {
			++begin_count;
			EXPECT_EQ(c->message, event.schema);
			const auto &id = event.as.message.id;

			EXPECT_EQ(2u, id.user_message_format);
			EXPECT_EQ(1u, id.functional_area);
			EXPECT_EQ(1u, id.message_number);
			EXPECT_EQ(profile != VMF_ENVELOPE_B,
				id.standard_version.present);
			if (id.standard_version.present)
				EXPECT_EQ(profile == VMF_ENVELOPE_C ? 8u : 7u,
					id.standard_version.value);
		} else if (event.kind == VMF_EVENT_FIELD) {
			++field_count;
			EXPECT_EQ(c->message, event.schema);
			EXPECT_EQ(c->index, event.index);
			EXPECT_EQ(VMF_FIELD_UNSIGNED, event.field_kind);
			EXPECT_EQ(0x5au,
				event.as.field.value.unsigned_integer.value);
		}
	}
	EXPECT_EQ(VMF_DONE, status);
	EXPECT_EQ(wire.size(), input.used);
	EXPECT_EQ(1u, header_count);
	EXPECT_EQ(1u, begin_count);
	EXPECT_EQ(1u, field_count);
	if (status == VMF_DONE && header_count == 1 && begin_count == 1 &&
		field_count == 1)
		++c->matching_envelopes;
	vmf_decoder_destroy(c->reader);
	c->reader = nullptr;
}

WHEN("^I encode and decode the same payload with B, C, D and D1 envelopes$")
{
	cucumber::ScenarioScope<portable_context> c;

	for (unsigned p = VMF_ENVELOPE_B; p <= VMF_ENVELOPE_D1; ++p) {
		SCOPED_TRACE(p);
		portable_roundtrip(c.get(), static_cast<vmf_envelope_version>(p));
		ASSERT_FALSE(::testing::Test::HasFatalFailure());
	}
}

static void unchanged_registry(const portable_context *c)
{
	ASSERT_EQ(c->schemas, vmf_schema_json_schemas(c->document));
	EXPECT_EQ(VMF_ENVELOPE_UNSPECIFIED, c->schemas->envelope);
	EXPECT_EQ(VMF_STANDARD_INHERIT, c->schemas->standard.kind);
	ASSERT_EQ(c->message, c->schemas->messages);
	ASSERT_EQ(c->editions.size(), c->schemas->message_count);
	for (size_t i = 0; i < c->editions.size(); ++i) {
		const auto *message = &c->schemas->messages[i];

		EXPECT_EQ(VMF_STANDARD_EDITION, message->standard.kind);
		EXPECT_EQ(c->editions[i], message->standard.as.edition);
		EXPECT_EQ(1u, message->index_count);
		EXPECT_EQ(8u, message->indexes[0].number_of_bits);
	}
	EXPECT_EQ(c->index, c->message->indexes);
}

THEN("^every envelope carries the same payload using the unchanged registry$")
{
	cucumber::ScenarioScope<portable_context> c;

	EXPECT_EQ(4u, c->matching_envelopes);
	EXPECT_EQ(4u, c->requests);
	unchanged_registry(c.get());
}

WHEN("^I bind that registry to envelope (B|C)$")
{
	REGEX_PARAM(std::string, envelope);
	cucumber::ScenarioScope<portable_context> c;
	vmf_options options = vmf_options_for_schemas(c->schemas);

	options.profile = envelope == "B" ? VMF_ENVELOPE_B : VMF_ENVELOPE_C;
	c->status = vmf_decoder_create(&options, &c->reader, nullptr);
}

THEN("^binding fails with (invalid_schema|duplicate_message) "
	"without changing the registry$")
{
	REGEX_PARAM(std::string, status);
	cucumber::ScenarioScope<portable_context> c;

	EXPECT_EQ(status == "invalid_schema" ? VMF_INVALID_SCHEMA :
		VMF_DUPLICATE_MESSAGE, c->status);
	EXPECT_EQ(nullptr, c->reader);
	unchanged_registry(c.get());
}

} /* namespace */
