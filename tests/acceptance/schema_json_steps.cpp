#include <gtest/gtest.h>
#include <cucumber-cpp/autodetect.hpp>
#include <vmf/schema_json.h>
#include "vectors.h"
#include <string>
#include <vector>

namespace
{

struct json_context {
	std::string input;
	std::string example;
	vmf_node root = {};
	vmf_node children[3] = {};
	vmf_node body = {};
	vmf_node value = {};
	vmf_choice_case arm = {};
	vmf_code_range range = {2, 2};
	vmf_condition_term term = {};
	vmf_message_schema schema = {};
	const vmf_message_schema *entry = &schema;
	vmf_schema_set native = {&entry, 1};
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

static std::string json_scalar(unsigned id, unsigned width)
{
	return "{\"id\":" + std::to_string(id) +
	       ",\"kind\":\"field\",\"field\":{\"kind\":\"unsigned\","
	       "\"scalar\":{\"encoding\":{\"bit_width\":" +
	       std::to_string(width) + ",\"order\":\"lsb_first\","
	       "\"representation\":\"unsigned\"}}}}";
}

static std::string json_group(unsigned id, const std::string &children)
{
	return "{\"id\":" + std::to_string(id) +
	       ",\"kind\":\"group\",\"group\":{\"children\":[" +
	       children + "]}}";
}

static vmf_node unsigned_node(vmf_node_id id, uint8_t width)
{
	vmf_node node = {};

	node.id = id;
	node.kind = VMF_NODE_FIELD;
	node.as.field.kind = VMF_FIELD_UNSIGNED;
	node.as.field.as.scalar.encoding = {
		width, VMF_BIT_LSB_FIRST, VMF_INTEGER_UNSIGNED};
	return node;
}

static void attach_payload(json_context *c,
	std::initializer_list<uint8_t> body)
{
	c->wire.assign(vector_d1, vector_d1 + sizeof(vector_d1) - 1);
	for (unsigned bit = 0; bit < 20; ++bit) {
		size_t offset = 53 + bit;
		uint8_t mask = 1u << (offset % 8);

		c->wire[offset / 8] &= ~mask;
		if ((body.size() >> bit) & 1)
			c->wire[offset / 8] |= mask;
	}
	c->wire.insert(c->wire.end(), body);
}

GIVEN("^a JSON definition of a synthetic "
	"(scalar|recurrence|choice|UTF-8 text) payload$")
{
	REGEX_PARAM(std::string, example);
	cucumber::ScenarioScope<json_context> context;
	auto *c = context.get();
	std::string children;

	c->example = example;
	c->root.id = 100;
	c->root.kind = VMF_NODE_GROUP;
	c->root.as.group = {c->children, 1};
	c->schema.id.user_message_format = 1;
	c->schema.root = &c->root;
	c->schema.octet_order = VMF_BIT_LSB_FIRST;
	if (example == "scalar") {
		c->children[0] = unsigned_node(1, 8);
		children = json_scalar(1, 8);
		attach_payload(c, {0x5a});
	} else if (example == "recurrence") {
		c->value = unsigned_node(3, 4);
		c->body.id = 2;
		c->body.kind = VMF_NODE_GROUP;
		c->body.as.group = {&c->value, 1};
		c->children[0].id = 1;
		c->children[0].kind = VMF_NODE_REPEAT;
		c->children[0].as.repeat.body = &c->body;
		auto *count = &c->children[0].as.repeat.count;

		count->kind = VMF_EXTENT_RECURRENCE_BEFORE;
		count->more_bit = true;
		count->minimum = 1;
		count->maximum = 5;
		children = "{\"id\":1,\"kind\":\"repeat\",\"repeat\":{"
			"\"count\":{\"kind\":\"recurrence_before\","
			"\"more_bit\":true,"
			"\"minimum\":1,\"maximum\":5},\"body\":" +
			json_group(2, json_scalar(3, 4)) + "}}";
		attach_payload(c, {0x07, 0x03});
	} else if (example == "choice") {
		c->root.as.group.child_count = 3;
		c->children[0] = unsigned_node(1, 2);
		c->value.id = 4;
		c->value.kind = VMF_NODE_FIELD;
		c->value.as.field.kind = VMF_FIELD_OCTETS;
		c->value.as.field.as.octets.order = VMF_BIT_LSB_FIRST;
		auto *length = &c->value.as.field.as.octets.length;

		length->kind = VMF_EXTENT_PREFIX;
		length->prefix = {3, VMF_BIT_LSB_FIRST, VMF_INTEGER_UNSIGNED};
		length->bias = 1;
		length->maximum = 7;
		c->arm = {&c->range, 1, &c->value};
		c->children[1].id = 2;
		c->children[1].kind = VMF_NODE_CHOICE;
		c->children[1].as.choice = {1, 3, &c->arm, 1, nullptr};
		c->children[2] = unsigned_node(3, 1);
		c->term = {1, UINT64_MAX, &c->range, 1, false};
		c->children[2].presence.kind = VMF_PRESENCE_CONDITION;
		c->children[2].presence.condition = {
			VMF_CONDITION_ALL, &c->term, 1, false};
		std::string conditional = json_scalar(3, 1);

		conditional.insert(1, "\"presence\":{\"kind\":\"condition\","
			"\"condition\":{\"mode\":\"all\",\"terms\":[{"
			"\"field\":1,"
			"\"values\":[{\"first\":2,\"last\":2}]}]}},");
		children = json_scalar(1, 2) +
			",{\"id\":2,\"kind\":\"choice\",\"choice\":{"
			"\"selector\":1,\"mask\":3,\"cases\":[{\"values\":[{"
			"\"first\":2,\"last\":2}],"
			"\"body\":{\"id\":4,\"kind\":\"field\",\"field\":{"
			"\"kind\":\"octets\",\"octets\":{"
			"\"order\":\"lsb_first\",\"length\":{"
			"\"kind\":\"prefix\","
			"\"prefix\":{\"bit_width\":3,\"order\":\"lsb_first\","
			"\"representation\":\"unsigned\"},"
			"\"bias\":1,\"minimum\":0,\"maximum\":7}}}}}]}}," +
			conditional;
		attach_payload(c, {0x06, 0x30, 0x20});
	} else {
		c->children[0].id = 1;
		c->children[0].kind = VMF_NODE_FIELD;
		c->children[0].as.field.kind = VMF_FIELD_TEXT;
		auto *text = &c->children[0].as.field.as.text;

		text->encoding = VMF_TEXT_UTF_8;
		text->order = VMF_BIT_LSB_FIRST;
		text->length.fixed = 8;
		text->length.minimum = text->length.maximum = 8;
		children = "{\"id\":1,\"kind\":\"field\",\"field\":{"
			"\"kind\":\"text\",\"text\":{\"encoding\":\"utf_8\","
			"\"order\":\"lsb_first\","
			"\"length\":{\"kind\":\"fixed\",\"fixed\":8,"
			"\"minimum\":8,\"maximum\":8}}}}";
		attach_payload(c,
			{0x41, 0xe2, 0x82, 0xac, 0xf0, 0x90, 0x8d, 0x88});
	}
	c->input = "{\"schemas\":[{\"id\":{\"user_message_format\":1},"
		"\"octet_order\":\"lsb_first\",\"root\":" +
		json_group(100, children) + "}]}";
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

static vmf_status replay(const vmf_schema_set *schemas,
	const std::vector<uint8_t> &wire, std::vector<uint8_t> *encoded,
	std::vector<std::string> *events, std::vector<uint64_t> *numbers)
{
	vmf_options options = {};

	options.profile = VMF_ENVELOPE_D1;
	options.limits = vmf_default_limits();
	options.limits.field_chunk_bytes = 4;
	options.schemas = schemas;
	vmf_decoder *reader = nullptr;
	vmf_encoder *writer = nullptr;
	vmf_status status = vmf_decoder_create(&options, &reader, nullptr);

	if (status == VMF_OK)
		status = vmf_encoder_create(&options, false, &writer, nullptr);
	if (status == VMF_OK)
		status = vmf_decoder_begin_pdu(reader);
	size_t position = 0;
	vmf_input input = {wire.data(), 1, 0, false};

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
		std::string observed = std::to_string(event.kind) + ":" +
			std::to_string(event.node ? event.node->id : 0);

		if (event.kind == VMF_EVENT_FIELD) {
			observed += ":" +
				std::to_string(event.as.field.offset_units);
			observed += ":" +
				std::to_string(event.as.field.total_units);
			if (event.node->as.field.kind == VMF_FIELD_UNSIGNED) {
				uint64_t value = event.as.field.value
					.unsigned_integer.value;

				numbers->push_back(value);
				observed += ":" + std::to_string(value);
			} else if (event.node->as.field.kind ==
				VMF_FIELD_TEXT) {
				observed.append(event.as.field.value.text.data,
					event.as.field.value.text.size);
			} else if (event.node->as.field.kind ==
				VMF_FIELD_OCTETS) {
				observed.append(reinterpret_cast<const char *>(
					event.as.field.value.octets.data),
					event.as.field.value.octets.size);
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
		EXPECT_EQ((std::vector<uint64_t>{0x5a}), c->numbers);
	if (c->example == "recurrence")
		EXPECT_EQ((std::vector<uint64_t>{3, 12}), c->numbers);
	if (c->example == "choice")
		EXPECT_EQ((std::vector<uint64_t>{2, 1}), c->numbers);
}

GIVEN("^a JSON schema set with a valid entry followed by an invalid entry$")
{
	cucumber::ScenarioScope<json_context> c;
	std::string valid = "{\"id\":{\"user_message_format\":1},"
		"\"octet_order\":\"lsb_first\",\"root\":" +
		json_group(1, "") + "}";
	std::string invalid = "{\"id\":{\"user_message_format\":2},"
		"\"octet_order\":\"lsb_first\",\"root\":" +
		json_group(1, json_scalar(2, 0)) + "}";

	c->input = "{\"schemas\":[" + valid + "," + invalid + "]}";
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
	EXPECT_EQ(2u, c->error.node_id);
}

} /* namespace */
