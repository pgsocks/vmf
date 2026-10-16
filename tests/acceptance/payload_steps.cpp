#include <gtest/gtest.h>
#include <cucumber-cpp/autodetect.hpp>
#include <vmf/wire.h>
#include "vectors.h"
#include <string>
#include <vector>

namespace
{

struct payload_context {
	vmf_node root = {};
	vmf_node children[3] = {};
	vmf_node body = {};
	vmf_node value = {};
	vmf_code_range range = {2, 2};
	vmf_choice_case arm = {};
	vmf_condition_term term = {};
	vmf_message_schema schema = {};
	const vmf_message_schema *entry = &schema;
	vmf_schema_set schemas = {&entry, 1};
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
	std::vector<vmf_node> lookup_nodes;

	~payload_context()
	{
		vmf_decoder_destroy(reader);
		vmf_encoder_destroy(writer);
	}
};

static vmf_node unsigned_node(vmf_node_id id, uint8_t width)
{
	vmf_node result = {};

	result.id = id;
	result.kind = VMF_NODE_FIELD;
	result.as.field.kind = VMF_FIELD_UNSIGNED;
	result.as.field.as.scalar.encoding = {
		width, VMF_BIT_LSB_FIRST, VMF_INTEGER_UNSIGNED};
	return result;
}

static void attach_payload(
	payload_context *context, std::initializer_list<uint8_t> body)
{
	context->wire.assign(vector_d1, vector_d1 + sizeof(vector_d1) - 1);
	/* D1 Table I MessageSize occupies bits 53..72 in this header. */
	for (unsigned int bit = 0; bit < 20; ++bit) {
		size_t offset = 53 + bit;
		uint8_t mask = 1u << (offset % 8);

		context->wire[offset / 8] &= ~mask;
		if ((body.size() >> bit) & 1)
			context->wire[offset / 8] |= mask;
	}
	context->wire.insert(context->wire.end(), body);
}

GIVEN("^a synthetic (leading recurrence|conditional choice|UTF-8 text) "
	"body in a D1 envelope$")
{
	REGEX_PARAM(std::string, example);
	cucumber::ScenarioScope<payload_context> context;
	auto *c = context.get();

	c->example = example;
	c->root.id = 100;
	c->root.kind = VMF_NODE_GROUP;
	c->root.as.group = {c->children, 1};
	c->schema.id.user_message_format = 1;
	c->schema.root = &c->root;
	c->schema.octet_order = VMF_BIT_LSB_FIRST;
	if (example == "leading recurrence") {
		c->value = unsigned_node(3, 4);
		c->body.id = 2;
		c->body.kind = VMF_NODE_GROUP;
		c->body.as.group = {&c->value, 1};
		c->children[0].id = 1;
		c->children[0].kind = VMF_NODE_REPEAT;
		c->children[0].as.repeat.body = &c->body;
		auto *extent = &c->children[0].as.repeat.count;

		extent->kind = VMF_EXTENT_RECURRENCE_BEFORE;
		extent->more_bit = true;
		extent->minimum = 1;
		extent->maximum = 5;
		attach_payload(c, {0x07, 0x03});
	} else if (example == "conditional choice") {
		c->root.as.group.child_count = 3;
		c->children[0] = unsigned_node(1, 2);
		c->value.id = 4;
		c->value.kind = VMF_NODE_FIELD;
		c->value.as.field.kind = VMF_FIELD_OCTETS;
		c->value.as.field.as.octets.order = VMF_BIT_LSB_FIRST;
		auto *extent = &c->value.as.field.as.octets.length;

		extent->kind = VMF_EXTENT_PREFIX;
		extent->prefix = {3, VMF_BIT_LSB_FIRST, VMF_INTEGER_UNSIGNED};
		extent->bias = 1;
		extent->maximum = 7;
		c->arm = {&c->range, 1, &c->value};
		c->children[1].id = 2;
		c->children[1].kind = VMF_NODE_CHOICE;
		c->children[1].as.choice = {1, 3, &c->arm, 1, nullptr};
		c->children[2] = unsigned_node(3, 1);
		c->term = {1, UINT64_MAX, &c->range, 1, false};
		c->children[2].presence.kind = VMF_PRESENCE_CONDITION;
		c->children[2].presence.condition = {
			VMF_CONDITION_ALL, &c->term, 1, false};
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
		attach_payload(
			c, {0x41, 0xe2, 0x82, 0xac, 0xf0, 0x90, 0x8d, 0x88});
	}
}

GIVEN("^a payload schema with a dependency between alternative choice arms$")
{
	cucumber::ScenarioScope<payload_context> context;
	auto *c = context.get();

	c->root.id = 100;
	c->root.kind = VMF_NODE_GROUP;
	c->root.as.group = {c->children, 2};
	c->schema.root = &c->root;
	c->schema.octet_order = VMF_BIT_LSB_FIRST;
	c->children[0] = unsigned_node(1, 8);
	c->value = unsigned_node(3, 8);
	c->children[2] = unsigned_node(5, 8);
	c->term = {3, UINT64_MAX, &c->range, 1, false};
	c->children[2].presence.kind = VMF_PRESENCE_CONDITION;
	c->children[2].presence.condition = {
		VMF_CONDITION_ALL, &c->term, 1, false};
	c->body.id = 4;
	c->body.kind = VMF_NODE_GROUP;
	c->body.as.group = {&c->children[2], 1};
	c->arm = {&c->range, 1, &c->value};
	c->children[1].id = 2;
	c->children[1].kind = VMF_NODE_CHOICE;
	c->children[1].as.choice = {
		1, UINT64_MAX, &c->arm, 1, &c->body};
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

GIVEN("^a valid payload schema with 4096 nested nodes$")
{
	cucumber::ScenarioScope<payload_context> c;

	c->lookup_nodes.resize(4096);
	for (size_t i = 0; i < c->lookup_nodes.size(); ++i) {
		auto *node = &c->lookup_nodes[i];

		*node = unsigned_node((vmf_node_id)i + 1, 8);
		if (i + 1 < c->lookup_nodes.size()) {
			node->kind = VMF_NODE_GROUP;
			node->as.group = {node + 1, 1};
		}
	}
	c->schema.root = c->lookup_nodes.data();
	c->schema.octet_order = VMF_BIT_LSB_FIRST;
	auto limits = vmf_default_limits();

	limits.max_depth = c->lookup_nodes.size() - 1;
	ASSERT_EQ(VMF_OK,
		vmf_schema_validate(&c->schema, &limits, nullptr));
}

THEN("^schema lookup returns the deepest original node "
	"and rejects missing IDs$")
{
	cucumber::ScenarioScope<payload_context> c;

	EXPECT_EQ(&c->lookup_nodes.back(), vmf_schema_find(&c->schema, 4096));
	EXPECT_EQ(nullptr, vmf_schema_find(&c->schema, 4097));
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
			event.node == &c->body) {
			EXPECT_TRUE(event.as.group.more_present);
			c->more.push_back(event.as.group.more);
		}
		if (event.kind == VMF_EVENT_FIELD) {
			if (event.node->as.field.kind == VMF_FIELD_UNSIGNED)
				c->numbers.push_back(event.as.field.value
						.unsigned_integer.value);
			if (event.node->as.field.kind == VMF_FIELD_TEXT) {
				c->texts.emplace_back(
					event.as.field.value.text.data,
					event.as.field.value.text.size);
				c->offsets.push_back(
					event.as.field.offset_units);
			}
			if (event.node->as.field.kind == VMF_FIELD_OCTETS)
				c->texts.emplace_back(
					reinterpret_cast<const char *>(
						event.as.field.value.octets
							.data),
					event.as.field.value.octets.size);
		}
		if (c->known_count && event.kind == VMF_EVENT_GROUP_BEGIN) {
			if (event.node == &c->children[0])
				event.as.group.count = 2;
			if (event.node == &c->body)
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

THEN("^the choice bytes and conditional value are preserved$")
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
