#include <gtest/gtest.h>
#include "../../src/payload.hpp"
#include "../../src/bits.hpp"
#include "fixtures.hpp"
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>

using namespace vmf_internal;

struct payload_test_state {
	void *memory;
	payload_state *state;
};

static payload_test_state payload_memory(vmf_limits limits)
{
	payload_test_state result = {};
	size_t size = 0;

	EXPECT_EQ(VMF_OK, payload_storage_size(&limits, &size));
	result.memory = std::malloc(size);
	EXPECT_NE(nullptr, result.memory);
	EXPECT_EQ(VMF_OK, payload_init(result.memory, &limits, &result.state));
	return result;
}

static vmf_node scalar(vmf_node_id id, vmf_field_kind kind, uint8_t width,
	vmf_integer_representation representation = VMF_INTEGER_UNSIGNED)
{
	vmf_node node = {};

	node.id = id;
	node.kind = VMF_NODE_FIELD;
	node.as.field.kind = kind;
	node.as.field.as.scalar.encoding = {
		width, VMF_BIT_LSB_FIRST, representation};
	return node;
}

static vmf_message_schema tree(vmf_node *root, vmf_node *children, size_t count)
{
	*root = {};
	root->id = 100;
	root->kind = VMF_NODE_GROUP;
	root->as.group = {children, count};
	vmf_message_schema schema = {};

	schema.root = root;
	schema.octet_order = VMF_BIT_LSB_FIRST;
	return schema;
}

struct payload_observation {
	vmf_event_kind kind;
	vmf_node_id node;
	uint64_t offset;
	uint64_t total;
	uint64_t code;
	int64_t signed_value;
	bool last;
	bool more;
	bool more_present;
	std::string text;
};

static std::vector<payload_observation> replay(const vmf_message_schema *schema,
	const std::vector<uint8_t> &bytes, bool unknown = false)
{
	vmf_limits limits = vmf_default_limits();

	limits.field_chunk_bytes = 4;
	auto reader = payload_memory(limits);
	auto writer = payload_memory(limits);
	uint64_t bound = unknown ? VMF_LENGTH_UNKNOWN : bytes.size();

	payload_begin(reader.state, schema, bound, 0, false);
	payload_begin(writer.state, schema, bound, 0, false);
	std::vector<payload_observation> observed;
	std::vector<uint8_t> encoded;
	size_t position = 0;
	vmf_input input = {bytes.empty() ? nullptr : bytes.data(),
		bytes.empty() ? 0u : 1u, 0, bytes.size() <= 1};

	for (size_t attempts = 0; attempts < 10000; ++attempts) {
		vmf_event event = {};
		vmf_status status =
			payload_decode(reader.state, &input, &event);

		if (status == VMF_NEED_INPUT) {
			EXPECT_EQ(input.size, input.used);
			position += input.used;
			EXPECT_LT(position, bytes.size());
			if (position >= bytes.size())
				break;
			input = {bytes.data() + position, 1, 0,
				position + 1 == bytes.size()};
			continue;
		}
		if (status == VMF_DONE) {
			event.kind = VMF_EVENT_MESSAGE_END;
		} else {
			EXPECT_EQ(VMF_OK, status);
			if (status != VMF_OK)
				break;
			payload_observation value = {};

			value.kind = event.kind;
			value.node = event.node->id;
			if (event.kind == VMF_EVENT_FIELD) {
				value.offset = event.as.field.offset_units;
				value.total = event.as.field.total_units;
				value.code = event.as.field.value.scaled.code;
				value.signed_value =
					event.as.field.value.signed_integer
						.value;
				if (event.node->as.field.kind ==
					VMF_FIELD_TEXT) {
					value.text.assign(
						event.as.field.value.text.data,
						event.as.field.value.text.size);
					value.last =
						event.as.field.value.text.last;
				}
				if (event.node->as.field.kind ==
					VMF_FIELD_OCTETS) {
					value.text.assign(
						reinterpret_cast<const char *>(
							event.as.field.value
								.octets.data),
						event.as.field.value.octets
							.size);
					value.last = event.as.field.value.octets
							     .last;
				}
			} else if (event.kind == VMF_EVENT_GROUP_BEGIN) {
				value.more = event.as.group.more;
				value.more_present =
					event.as.group.more_present;
			}
			observed.push_back(value);
		}
		vmf_status written;

		do {
			uint8_t octet;
			vmf_output output = {&octet, 1, 0};

			written = payload_encode(
				writer.state, &output, &event, false);
			if (output.used)
				encoded.push_back(octet);
		} while (written == VMF_NEED_OUTPUT);
		EXPECT_EQ(VMF_OK, written);
		if (status == VMF_DONE)
			break;
	}
	EXPECT_TRUE(payload_complete(reader.state));
	EXPECT_TRUE(payload_complete(writer.state));
	EXPECT_EQ(bytes, encoded);
	std::free(reader.memory);
	std::free(writer.memory);
	return observed;
}

TEST(PayloadBits, FragmentedCodesAndPhysicalOrderRemainIndependent)
{
	bit_cursor cursor = {};
	const uint8_t first = 0xa3;
	vmf_input input = {&first, 1, 0, false};
	uint64_t value = 999;

	EXPECT_EQ(
		VMF_NEED_INPUT, read_bits(&cursor, &input, 12,
					VMF_BIT_MSB_FIRST, UINT64_MAX, &value));
	EXPECT_EQ(999u, value);
	EXPECT_EQ(1u, input.used);
	const uint8_t second = 0x05;

	input = {&second, 1, 0, true};
	EXPECT_EQ(VMF_OK, read_bits(&cursor, &input, 12, VMF_BIT_MSB_FIRST,
				  UINT64_MAX, &value));
	EXPECT_EQ(0xc5au, value);
	EXPECT_EQ(12u, cursor.offset);
}

TEST(Payload, SignedRepresentationsAndExactRawCodesReplay)
{
	vmf_node children[] = {
		scalar(1, VMF_FIELD_SIGNED, 5, VMF_INTEGER_TWOS_COMPLEMENT),
		scalar(2, VMF_FIELD_SIGNED, 5, VMF_INTEGER_ONES_COMPLEMENT),
		scalar(3, VMF_FIELD_SIGNED, 5, VMF_INTEGER_SIGN_MAGNITUDE),
		scalar(4, VMF_FIELD_SCALED, 5, VMF_INTEGER_ONES_COMPLEMENT),
	};
	vmf_node root;
	auto schema = tree(&root, children, 4);
	/* Raw codes 29, 28, 19, 31, followed by four zero pad bits. */
	auto values = replay(&schema, {0x9d, 0xcf, 0x0f});

	ASSERT_EQ(6u, values.size());
	EXPECT_EQ(-3, values[1].signed_value);
	EXPECT_EQ(-3, values[2].signed_value);
	EXPECT_EQ(-3, values[3].signed_value);
	EXPECT_EQ(31u, values[4].code);
}

TEST(Payload, LeadingRecurrenceRetainsLastFlagOnBodyBegin)
{
	vmf_node value = scalar(3, VMF_FIELD_UNSIGNED, 4);
	vmf_node body = {};

	body.id = 2;
	body.kind = VMF_NODE_GROUP;
	body.as.group = {&value, 1};
	vmf_node repeat = {};

	repeat.id = 1;
	repeat.kind = VMF_NODE_REPEAT;
	repeat.as.repeat.body = &body;
	repeat.as.repeat.count.kind = VMF_EXTENT_RECURRENCE_BEFORE;
	repeat.as.repeat.count.more_bit = true;
	repeat.as.repeat.count.minimum = 1;
	repeat.as.repeat.count.maximum = 5;
	vmf_node root;
	auto schema = tree(&root, &repeat, 1);
	auto values = replay(&schema, {0x07, 0x03});
	std::vector<bool> flags;
	std::vector<uint64_t> numbers;

	for (const auto &event : values) {
		if (event.node == 2 && event.kind == VMF_EVENT_GROUP_BEGIN) {
			EXPECT_TRUE(event.more_present);
			flags.push_back(event.more);
		}
		if (event.node == 3 && event.kind == VMF_EVENT_FIELD)
			numbers.push_back(event.code);
	}
	EXPECT_EQ((std::vector<bool>{true, false}), flags);
	EXPECT_EQ((std::vector<uint64_t>{3, 12}), numbers);
}

TEST(Payload, BoundedAsciiLiteralTerminatesBelowMaximum)
{
	vmf_node text = {};

	text.id = 1;
	text.kind = VMF_NODE_FIELD;
	text.as.field.kind = VMF_FIELD_TEXT;
	text.as.field.as.text.encoding = VMF_TEXT_ASCII_7;
	text.as.field.as.text.order = VMF_BIT_LSB_FIRST;
	text.as.field.as.text.length.kind = VMF_EXTENT_LITERAL;
	text.as.field.as.text.length.maximum = 3;
	vmf_node root;
	auto schema = tree(&root, &text, 1);
	auto values = replay(&schema, {0xc1, 0x3f});

	ASSERT_EQ(3u, values.size());
	EXPECT_EQ("A", values[1].text);
	EXPECT_EQ(VMF_UNITS_UNKNOWN, values[1].total);
	EXPECT_TRUE(values[1].last);
	values = replay(&schema, {0x41, 0xe1, 0x10});
	ASSERT_EQ(3u, values.size());
	EXPECT_EQ("ABC", values[1].text);
}

TEST(Payload, UnknownUtf8RemainderPreservesChunkOffsetsAndLastness)
{
	vmf_node text = {};

	text.id = 1;
	text.kind = VMF_NODE_FIELD;
	text.as.field.kind = VMF_FIELD_TEXT;
	text.as.field.as.text.encoding = VMF_TEXT_UTF_8;
	text.as.field.as.text.order = VMF_BIT_LSB_FIRST;
	text.as.field.as.text.length.kind = VMF_EXTENT_REMAINING;
	text.as.field.as.text.length.maximum = 100;
	vmf_node root;
	auto schema = tree(&root, &text, 1);
	auto values = replay(&schema,
		{0x41, 0xe2, 0x82, 0xac, 0xf0, 0x90, 0x8d, 0x88}, true);

	ASSERT_EQ(4u, values.size());
	EXPECT_EQ(0u, values[1].offset);
	EXPECT_EQ(4u, values[2].offset);
	EXPECT_EQ(VMF_UNITS_UNKNOWN, values[1].total);
	EXPECT_FALSE(values[1].last);
	EXPECT_TRUE(values[2].last);
	EXPECT_EQ("A\xe2\x82\xac", values[1].text);
	EXPECT_EQ("\xf0\x90\x8d\x88", values[2].text);
}

TEST(Payload, RejectsNonzeroPaddingAndTrailingBytes)
{
	vmf_node value = scalar(1, VMF_FIELD_UNSIGNED, 3);
	vmf_node root;
	auto schema = tree(&root, &value, 1);
	auto limits = vmf_default_limits();
	auto reader = payload_memory(limits);
	const uint8_t bytes[] = {0x85, 0x00};
	vmf_input input = {bytes, 1, 0, true};
	vmf_event event;

	payload_begin(reader.state, &schema, 1, 0, false);
	ASSERT_EQ(VMF_OK, payload_decode(reader.state, &input, &event));
	ASSERT_EQ(VMF_OK, payload_decode(reader.state, &input, &event));
	ASSERT_EQ(VMF_OK, payload_decode(reader.state, &input, &event));
	EXPECT_EQ(VMF_MALFORMED, payload_decode(reader.state, &input, &event));
	input = {bytes + 1, 1, 0, true};
	payload_begin(reader.state, &schema, VMF_LENGTH_UNKNOWN, 0, false);
	ASSERT_EQ(VMF_OK, payload_decode(reader.state, &input, &event));
	ASSERT_EQ(VMF_OK, payload_decode(reader.state, &input, &event));
	ASSERT_EQ(VMF_OK, payload_decode(reader.state, &input, &event));
	EXPECT_EQ(VMF_DONE, payload_decode(reader.state, &input, &event));
	std::free(reader.memory);
}

struct synthetic_code {
	uint64_t value;
	uint8_t width;
};

static std::vector<uint8_t> synthetic_wire(
	std::initializer_list<synthetic_code> codes)
{
	std::vector<uint8_t> result;
	size_t bits = 0;

	for (auto code : codes) {
		for (unsigned int i = 0; i < code.width; ++i, ++bits) {
			if (!(bits % 8))
				result.push_back(0);
			result[bits / 8] |= ((code.value >> i) & 1)
					    << (bits % 8);
		}
	}
	return result;
}

TEST(Payload, PrefixLengthsBiasConditionsAndChoiceUseRawDependency)
{
	vmf_node children[3] = {};

	children[0] = scalar(1, VMF_FIELD_UNSIGNED, 2);
	vmf_node bytes = {};

	bytes.id = 4;
	bytes.kind = VMF_NODE_FIELD;
	bytes.as.field.kind = VMF_FIELD_OCTETS;
	bytes.as.field.as.octets.order = VMF_BIT_LSB_FIRST;
	bytes.as.field.as.octets.length.kind = VMF_EXTENT_PREFIX;
	bytes.as.field.as.octets.length.prefix = {
		3, VMF_BIT_LSB_FIRST, VMF_INTEGER_UNSIGNED};
	bytes.as.field.as.octets.length.bias = 1;
	bytes.as.field.as.octets.length.maximum = 7;
	vmf_code_range select = {2, 2};
	vmf_choice_case arm = {&select, 1, &bytes};

	children[1].id = 2;
	children[1].kind = VMF_NODE_CHOICE;
	children[1].as.choice = {1, 3, &arm, 1, nullptr};
	children[2] = scalar(3, VMF_FIELD_BOOL, 1);
	vmf_condition_term term = {1, UINT64_MAX, &select, 1, false};

	children[2].presence.kind = VMF_PRESENCE_CONDITION;
	children[2].presence.condition = {VMF_CONDITION_ALL, &term, 1, false};
	vmf_node root;
	auto schema = tree(&root, children, 3);
	auto values = replay(&schema,
		synthetic_wire({{2, 2}, {1, 3}, {0x80, 8}, {0x01, 8}, {1, 1}}));

	ASSERT_EQ(7u, values.size());
	EXPECT_EQ(std::string("\x80\x01", 2), values[3].text);
	EXPECT_EQ(2u, values[3].total);
	EXPECT_EQ(1u, values[5].code);
}

TEST(Payload, PresenceBitsProduceSingleAbsentEvents)
{
	vmf_node children[] = {
		scalar(1, VMF_FIELD_UNSIGNED, 4),
		scalar(2, VMF_FIELD_UNSIGNED, 4),
	};

	children[0].presence.kind = VMF_PRESENCE_BIT;
	children[0].presence.present_bit = true;
	children[1].presence.kind = VMF_PRESENCE_BIT;
	children[1].presence.present_bit = false;
	vmf_node root;
	auto schema = tree(&root, children, 2);
	auto values = replay(&schema, synthetic_wire({{0, 1}, {0, 1}, {9, 4}}));

	ASSERT_EQ(4u, values.size());
	EXPECT_EQ(VMF_EVENT_ABSENT, values[1].kind);
	EXPECT_EQ(9u, values[2].code);
}

TEST(Payload, AllRepeatCountModesReplayWithoutBackpatching)
{
	vmf_node value = scalar(3, VMF_FIELD_UNSIGNED, 3);
	vmf_node body = {};

	body.id = 2;
	body.kind = VMF_NODE_GROUP;
	body.as.group = {&value, 1};
	vmf_node repeat = {};

	repeat.id = 1;
	repeat.kind = VMF_NODE_REPEAT;
	repeat.as.repeat.body = &body;
	repeat.as.repeat.count.fixed = 2;
	repeat.as.repeat.count.maximum = 4;
	repeat.as.repeat.count.more_bit = true;
	vmf_node root;
	auto schema = tree(&root, &repeat, 1);

	replay(&schema, synthetic_wire({{1, 3}, {5, 3}}));
	repeat.as.repeat.count.kind = VMF_EXTENT_PREFIX;
	repeat.as.repeat.count.prefix = {
		2, VMF_BIT_LSB_FIRST, VMF_INTEGER_UNSIGNED};
	replay(&schema, synthetic_wire({{2, 2}, {1, 3}, {5, 3}}));
	repeat.as.repeat.count.kind = VMF_EXTENT_MORE_BEFORE;
	replay(&schema,
		synthetic_wire({{1, 1}, {1, 3}, {1, 1}, {5, 3}, {0, 1}}));
	replay(&schema, {0});
	repeat.as.repeat.count.kind = VMF_EXTENT_MORE_AFTER;
	repeat.as.repeat.count.minimum = 1;
	replay(&schema, synthetic_wire({{1, 3}, {1, 1}, {5, 3}, {0, 1}}));
	vmf_node children[] = {scalar(4, VMF_FIELD_UNSIGNED, 2), repeat};

	children[1].as.repeat.count.kind = VMF_EXTENT_FIELD;
	children[1].as.repeat.count.field = 4;
	schema = tree(&root, children, 2);
	replay(&schema, synthetic_wire({{2, 2}, {1, 3}, {5, 3}}));
}

TEST(Payload, Latin1UsesUtf8ApiAndBitsHaveIndependentBitNumbering)
{
	vmf_node children[2] = {};

	children[0].id = 1;
	children[0].kind = VMF_NODE_FIELD;
	children[0].as.field.kind = VMF_FIELD_TEXT;
	children[0].as.field.as.text.encoding = VMF_TEXT_LATIN_1;
	children[0].as.field.as.text.order = VMF_BIT_LSB_FIRST;
	children[0].as.field.as.text.length.fixed = 3;
	children[0].as.field.as.text.length.maximum = 3;
	children[1].id = 2;
	children[1].kind = VMF_NODE_FIELD;
	children[1].as.field.kind = VMF_FIELD_BITS;
	children[1].as.field.as.bits.length.fixed = 35;
	children[1].as.field.as.bits.length.maximum = 35;
	vmf_node root;
	auto schema = tree(&root, children, 2);
	auto values = replay(
		&schema, {0x41, 0xff, 0x80, 0x12, 0x34, 0xab, 0x81, 0x05});
	std::string text;

	for (const auto &event : values)
		if (event.node == 1 && event.kind == VMF_EVENT_FIELD)
			text += event.text;
	EXPECT_EQ("A\xc3\xbf\xc2\x80", text);
}

TEST(Payload, EofRemainderAtUnalignedOffsetLeavesOnlyZeroPadding)
{
	vmf_node children[2] = {scalar(1, VMF_FIELD_UNSIGNED, 3), {}};

	children[1].id = 2;
	children[1].kind = VMF_NODE_FIELD;
	children[1].as.field.kind = VMF_FIELD_OCTETS;
	children[1].as.field.as.octets.order = VMF_BIT_LSB_FIRST;
	children[1].as.field.as.octets.length.kind = VMF_EXTENT_REMAINING;
	children[1].as.field.as.octets.length.maximum = 20;
	vmf_node root;
	auto schema = tree(&root, children, 2);
	auto values = replay(
		&schema, synthetic_wire({{5, 3}, {0x41, 8}, {0x42, 8}}), true);

	ASSERT_EQ(4u, values.size());
	EXPECT_EQ("AB", values[2].text);
	EXPECT_EQ(VMF_UNITS_UNKNOWN, values[2].total);
}

TEST(Payload, LimitIsCheckedBeforeUndeliverableCodeConsumesInput)
{
	vmf_node value = scalar(1, VMF_FIELD_UNSIGNED, 16);
	vmf_node root;
	auto schema = tree(&root, &value, 1);
	auto limits = vmf_default_limits();
	auto reader = payload_memory(limits);
	const uint8_t bytes[] = {1, 2};
	vmf_input input = {bytes, 2, 0, true};
	vmf_event event;

	payload_begin(reader.state, &schema, VMF_LENGTH_UNKNOWN, 0, false, 1);
	ASSERT_EQ(VMF_OK, payload_decode(reader.state, &input, &event));
	EXPECT_EQ(VMF_LIMIT_EXCEEDED,
		payload_decode(reader.state, &input, &event));
	EXPECT_EQ(0u, input.used);
	std::free(reader.memory);
}

TEST(Payload, InvalidUtf8AndSignedNegativeZeroAreRejected)
{
	vmf_node field =
		scalar(1, VMF_FIELD_SIGNED, 5, VMF_INTEGER_ONES_COMPLEMENT);
	vmf_node root;
	auto schema = tree(&root, &field, 1);
	auto limits = vmf_default_limits();
	auto reader = payload_memory(limits);
	const uint8_t negative_zero = 31;
	vmf_input input = {&negative_zero, 1, 0, true};
	vmf_event event;

	payload_begin(reader.state, &schema, 1, 0, false);
	ASSERT_EQ(VMF_OK, payload_decode(reader.state, &input, &event));
	EXPECT_EQ(
		VMF_OUT_OF_RANGE, payload_decode(reader.state, &input, &event));
	field = {};
	field.id = 1;
	field.kind = VMF_NODE_FIELD;
	field.as.field.kind = VMF_FIELD_TEXT;
	field.as.field.as.text.encoding = VMF_TEXT_UTF_8;
	field.as.field.as.text.order = VMF_BIT_LSB_FIRST;
	field.as.field.as.text.length.fixed = 3;
	field.as.field.as.text.length.maximum = 3;
	const uint8_t surrogate[] = {0xed, 0xa0, 0x80};

	input = {surrogate, 3, 0, true};
	payload_begin(reader.state, &schema, 3, 0, false);
	ASSERT_EQ(VMF_OK, payload_decode(reader.state, &input, &event));
	EXPECT_EQ(VMF_MALFORMED, payload_decode(reader.state, &input, &event));
	std::free(reader.memory);
}

TEST(Payload, FixedConstraintResolvesRemainingBitsAndSevenBitText)
{
	vmf_node field = {};

	field.id = 1;
	field.kind = VMF_NODE_FIELD;
	field.as.field.kind = VMF_FIELD_BITS;
	field.as.field.as.bits.length.kind = VMF_EXTENT_REMAINING;
	field.as.field.as.bits.length.minimum = 5;
	field.as.field.as.bits.length.maximum = 5;
	vmf_node root;
	auto schema = tree(&root, &field, 1);

	replay(&schema, {0x15});
	replay(&schema, {0x15}, true);
	field.as.field.kind = VMF_FIELD_TEXT;
	field.as.field.as.text.encoding = VMF_TEXT_ASCII_7;
	field.as.field.as.text.order = VMF_BIT_LSB_FIRST;
	field.as.field.as.text.length.kind = VMF_EXTENT_REMAINING;
	field.as.field.as.text.length.minimum = 2;
	field.as.field.as.text.length.maximum = 2;
	auto values = replay(&schema, {0x41, 0x21}, true);

	ASSERT_EQ(3u, values.size());
	EXPECT_EQ("AB", values[1].text);
	EXPECT_EQ(2u, values[1].total);
}

TEST(Payload, UnsizedFixedSchemaWaitsForFinalAndRejectsTrailingRecordData)
{
	vmf_node field = scalar(1, VMF_FIELD_UNSIGNED, 8);
	vmf_node root;
	auto schema = tree(&root, &field, 1);
	auto limits = vmf_default_limits();
	auto reader = payload_memory(limits);
	const uint8_t byte = 0x41;
	vmf_input input = {&byte, 1, 0, false};
	vmf_event event;

	payload_begin(reader.state, &schema, VMF_LENGTH_UNKNOWN, 0, false);
	ASSERT_EQ(VMF_OK, payload_decode(reader.state, &input, &event));
	ASSERT_EQ(VMF_OK, payload_decode(reader.state, &input, &event));
	ASSERT_EQ(VMF_OK, payload_decode(reader.state, &input, &event));
	EXPECT_EQ(VMF_NEED_INPUT, payload_decode(reader.state, &input, &event));
	input = {nullptr, 0, 0, true};
	EXPECT_EQ(VMF_DONE, payload_decode(reader.state, &input, &event));
	payload_begin(reader.state, &schema, VMF_LENGTH_UNKNOWN, 0, false);
	const uint8_t extra[] = {0x41, 0x42};

	input = {extra, 2, 0, true};
	ASSERT_EQ(VMF_OK, payload_decode(reader.state, &input, &event));
	ASSERT_EQ(VMF_OK, payload_decode(reader.state, &input, &event));
	ASSERT_EQ(VMF_OK, payload_decode(reader.state, &input, &event));
	EXPECT_EQ(VMF_MALFORMED, payload_decode(reader.state, &input, &event));
	EXPECT_EQ(1u, input.used);
	std::free(reader.memory);
}

TEST(Payload, PhysicalMsbOrderAndCodeLsbOrderHaveSeparateEffects)
{
	vmf_node field = scalar(1, VMF_FIELD_UNSIGNED, 5);
	vmf_node root;
	auto schema = tree(&root, &field, 1);

	schema.octet_order = VMF_BIT_MSB_FIRST;
	auto values = replay(&schema, {0xb0});

	ASSERT_EQ(3u, values.size());
	EXPECT_EQ(13u, values[1].code);
	field.as.field.as.scalar.encoding.order = VMF_BIT_MSB_FIRST;
	values = replay(&schema, {0xb0});
	ASSERT_EQ(3u, values.size());
	EXPECT_EQ(22u, values[1].code);
}

TEST(Payload, EmptyUnknownRemainderEmitsOneFinalEmptyField)
{
	vmf_node field = {};

	field.id = 1;
	field.kind = VMF_NODE_FIELD;
	field.as.field.kind = VMF_FIELD_OCTETS;
	field.as.field.as.octets.length.kind = VMF_EXTENT_REMAINING;
	field.as.field.as.octets.length.maximum = 10;
	vmf_node root;
	auto schema = tree(&root, &field, 1);
	auto values = replay(&schema, {}, true);

	ASSERT_EQ(3u, values.size());
	EXPECT_TRUE(values[1].text.empty());
	EXPECT_TRUE(values[1].last);
	EXPECT_EQ(VMF_UNITS_UNKNOWN, values[1].total);
}

TEST(Payload, SixtyFourBitIntegerBoundariesHaveNoNarrowingOrShiftOverflow)
{
	vmf_node field = scalar(1, VMF_FIELD_UNSIGNED, 64);
	vmf_node root;
	auto schema = tree(&root, &field, 1);
	auto values = replay(
		&schema, {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff});

	ASSERT_EQ(3u, values.size());
	EXPECT_EQ(UINT64_MAX, values[1].code);
	field.as.field.kind = VMF_FIELD_SIGNED;
	field.as.field.as.scalar.encoding.representation =
		VMF_INTEGER_TWOS_COMPLEMENT;
	values = replay(&schema, {0, 0, 0, 0, 0, 0, 0, 0x80});
	ASSERT_EQ(3u, values.size());
	EXPECT_EQ(INT64_MIN, values[1].signed_value);
	field.as.field.as.scalar.encoding.representation =
		VMF_INTEGER_ONES_COMPLEMENT;
	values = replay(&schema, {0, 0, 0, 0, 0, 0, 0, 0x80});
	ASSERT_EQ(3u, values.size());
	EXPECT_EQ(-INT64_MAX, values[1].signed_value);
	field.as.field.as.scalar.encoding.representation =
		VMF_INTEGER_SIGN_MAGNITUDE;
	values = replay(
		&schema, {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff});
	ASSERT_EQ(3u, values.size());
	EXPECT_EQ(-INT64_MAX, values[1].signed_value);
}

TEST(Payload, EncoderChecksChunkOffsetsTotalsAndFinalBitPadding)
{
	vmf_node field = {};

	field.id = 1;
	field.kind = VMF_NODE_FIELD;
	field.as.field.kind = VMF_FIELD_BITS;
	field.as.field.as.bits.length.fixed = 3;
	field.as.field.as.bits.length.maximum = 3;
	vmf_node root;
	auto schema = tree(&root, &field, 1);
	auto limits = vmf_default_limits();
	auto writer = payload_memory(limits);
	uint8_t bytes[1];
	vmf_output output = {bytes, sizeof(bytes), 0};
	vmf_event event = {};

	for (unsigned int failure = 0; failure < 3; ++failure) {
		payload_begin(writer.state, &schema, 1, 0, false);
		event = {};
		event.kind = VMF_EVENT_GROUP_BEGIN;
		event.node = &root;
		event.as.group.count = 1;
		ASSERT_EQ(VMF_OK,
			payload_encode(writer.state, &output, &event, false));
		event = {};
		event.kind = VMF_EVENT_FIELD;
		event.node = &field;
		event.depth = 1;
		event.as.field.total_units = failure == 1 ? 4 : 3;
		event.as.field.offset_units = failure == 0 ? 1 : 0;
		uint8_t bits = failure == 2 ? 0x85 : 0x05;

		event.as.field.value.bits = {&bits, 3, true};
		EXPECT_EQ(VMF_OUT_OF_RANGE,
			payload_encode(writer.state, &output, &event, false));
		EXPECT_EQ(0u, payload_bits(writer.state));
	}
	std::free(writer.memory);
}

TEST(Payload, CorrectedNullChunkArgumentDoesNotRepeatItsPrefix)
{
	vmf_node field = {};

	field.id = 1;
	field.kind = VMF_NODE_FIELD;
	field.as.field.kind = VMF_FIELD_OCTETS;
	field.as.field.as.octets.order = VMF_BIT_LSB_FIRST;
	field.as.field.as.octets.length.kind = VMF_EXTENT_PREFIX;
	field.as.field.as.octets.length.prefix = {
		3, VMF_BIT_LSB_FIRST, VMF_INTEGER_UNSIGNED};
	field.as.field.as.octets.length.maximum = 7;
	vmf_node root;
	auto schema = tree(&root, &field, 1);

	schema.id.user_message_format = 1;
	const vmf_message_schema *entry = &schema;
	vmf_schema_set set = {&entry, 1};
	auto options = fixtures::options(VMF_ENVELOPE_D1, &set);
	auto bytes = fixtures::packet(VMF_ENVELOPE_D1, {0x0a, 0x95, 0x05});
	vmf_decoder *reader = nullptr;
	vmf_encoder *writer = nullptr;

	ASSERT_EQ(VMF_OK, vmf_decoder_create(&options, &reader, nullptr));
	ASSERT_EQ(
		VMF_OK, vmf_encoder_create(&options, false, &writer, nullptr));
	ASSERT_EQ(VMF_OK, vmf_decoder_begin_pdu(reader));
	vmf_input input = {bytes.data(), bytes.size(), 0, true};
	std::vector<uint8_t> encoded;
	bool corrected = false;

	for (size_t i = 0; i < 20; ++i) {
		vmf_event event = {};
		vmf_status status = vmf_decoder_next(reader, &input, &event);

		if (status == VMF_DONE)
			break;
		ASSERT_EQ(VMF_OK, status);
		if (event.kind == VMF_EVENT_FIELD) {
			vmf_event invalid = event;

			invalid.as.field.value.octets.data = nullptr;
			uint8_t byte;
			vmf_output output = {&byte, 1, 0};
			uint64_t before = vmf_encoder_bit_count(writer);

			ASSERT_EQ(VMF_INVALID_ARGUMENT,
				vmf_encoder_write(writer, &output, &invalid));
			EXPECT_EQ(0u, output.used);
			EXPECT_EQ(before + 3, vmf_encoder_bit_count(writer));
			corrected = true;
		}
		fixtures::write(writer, &event, &encoded);
	}
	EXPECT_TRUE(corrected);
	EXPECT_EQ(bytes, encoded);
	vmf_decoder_destroy(reader);
	vmf_encoder_destroy(writer);
}
