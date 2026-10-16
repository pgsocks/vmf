#include <gtest/gtest.h>
#include "../../src/codec.hpp"
#include <cstddef>
#include <cstdlib>
#include <vector>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

static vmf_node schema_scalar(uint32_t id)
{
	vmf_node node = {};

	node.id = id;
	node.kind = VMF_NODE_FIELD;
	node.as.field.kind = VMF_FIELD_UNSIGNED;
	node.as.field.as.scalar.encoding = {
		8, VMF_BIT_LSB_FIRST, VMF_INTEGER_UNSIGNED};
	return node;
}

static vmf_message_schema schema_with(
	vmf_node *root, const vmf_node *children, size_t count)
{
	*root = {};
	root->id = 1;
	root->kind = VMF_NODE_GROUP;
	root->as.group = {children, count};
	vmf_message_schema schema = {};

	schema.root = root;
	schema.octet_order = VMF_BIT_LSB_FIRST;
	return schema;
}

TEST(SchemaContracts, LookupFailureNeverPublishesAnEarlierMatch)
{
	vmf_node root;
	auto schema = schema_with(&root, nullptr, 0);
	const vmf_message_schema *entries[] = {&schema, nullptr};
	vmf_schema_set set = {entries, 2};
	const vmf_message_schema *result = &schema;

	EXPECT_EQ(VMF_INVALID_ARGUMENT,
		vmf_schema_select(&set, &schema.id, &result));
	EXPECT_EQ(nullptr, result);
	entries[1] = &schema;
	EXPECT_EQ(VMF_DUPLICATE_MESSAGE,
		vmf_schema_select(&set, &schema.id, &result));
	EXPECT_EQ(nullptr, result);
}

TEST(SchemaContracts, LabelsAndSentinelsAreValidLengthDelimitedUtf8)
{
	vmf_node root;
	vmf_node field = schema_scalar(2);
	auto schema = schema_with(&root, &field, 1);
	auto limits = vmf_default_limits();
	const char valid[] = {
		'A', '\0', (char)0xf0, (char)0x9f, (char)0xa7, (char)0xad};
	vmf_code_label label = {1, {valid, sizeof(valid)}};

	field.as.field.as.scalar.labels = &label;
	field.as.field.as.scalar.label_count = 1;
	EXPECT_EQ(VMF_OK, vmf_schema_validate(&schema, &limits, nullptr));
	const char bad[][4] = {{(char)0xc0, (char)0x80, 0, 0},
		{(char)0xed, (char)0xa0, (char)0x80, 0},
		{(char)0xf4, (char)0x90, (char)0x80, (char)0x80},
		{(char)0xe2, (char)0x82, 0, 0}};
	const size_t lengths[] = {2, 3, 4, 2};

	for (size_t i = 0; i < 4; ++i) {
		label.name = {bad[i], lengths[i]};
		EXPECT_EQ(VMF_INVALID_SCHEMA,
			vmf_schema_validate(&schema, &limits, nullptr));
	}
	field.as.field.kind = VMF_FIELD_SCALED;
	field.as.field.as.scalar.labels = nullptr;
	field.as.field.as.scalar.label_count = 0;
	field.as.field.as.scalar.scale.factor = {1, 1};
	field.as.field.as.scalar.scale.bias = {0, 1};
	field.as.field.as.scalar.scale.sentinels = &label;
	field.as.field.as.scalar.scale.sentinel_count = 1;
	EXPECT_EQ(VMF_INVALID_SCHEMA,
		vmf_schema_validate(&schema, &limits, nullptr));
	label.name = {valid, sizeof(valid)};
	EXPECT_EQ(VMF_OK, vmf_schema_validate(&schema, &limits, nullptr));
}

struct schema_allocation {
	void *base = nullptr;
	size_t allocated = 0;
	size_t released = 0;
};

static void *schema_misaligned_allocate(void *user, size_t size)
{
	auto *log = static_cast<schema_allocation *>(user);

	log->base = std::malloc(size + alignof(std::max_align_t));
	if (!log->base)
		return nullptr;
	++log->allocated;
	return static_cast<uint8_t *>(log->base) + 1;
}

static void schema_misaligned_free(void *user, void *memory)
{
	auto *log = static_cast<schema_allocation *>(user);

	EXPECT_EQ(static_cast<uint8_t *>(log->base) + 1, memory);
	++log->released;
	std::free(log->base);
	log->base = nullptr;
}

TEST(SchemaContracts, MisalignedValidationStorageIsReleasedBeforeUse)
{
	vmf_node root;
	auto schema = schema_with(&root, nullptr, 0);
	auto limits = vmf_default_limits();
	const vmf_message_schema *entries[] = {&schema};
	vmf_schema_set set = {entries, 1};
	schema_allocation log;
	vmf_allocator allocator = {
		schema_misaligned_allocate, schema_misaligned_free, &log};
	vmf_error error = {};

	EXPECT_EQ(VMF_INVALID_ARGUMENT,
		vmf_internal::validate_schemas_allocated(
			&set, &limits, &allocator, &error));
	EXPECT_EQ(VMF_INVALID_ARGUMENT, error.status);
	EXPECT_EQ(log.allocated, log.released);
	EXPECT_EQ(nullptr, log.base);
}

TEST(SchemaContracts, FixedAndMinimumFieldAndRepeatExtentsRespectLimits)
{
	vmf_node root;
	vmf_node field = schema_scalar(2);
	auto schema = schema_with(&root, &field, 1);
	auto limits = vmf_default_limits();

	field.as.field = {};
	field.as.field.kind = VMF_FIELD_OCTETS;
	field.as.field.as.octets.order = VMF_BIT_LSB_FIRST;
	auto *length = &field.as.field.as.octets.length;

	length->kind = VMF_EXTENT_FIXED;
	length->fixed = length->minimum = length->maximum = 2;
	limits.max_field_bits = 8;
	EXPECT_EQ(VMF_LIMIT_EXCEEDED,
		vmf_schema_validate(&schema, &limits, nullptr));
	length->kind = VMF_EXTENT_PREFIX;
	length->prefix = {8, VMF_BIT_LSB_FIRST, VMF_INTEGER_UNSIGNED};
	EXPECT_EQ(VMF_LIMIT_EXCEEDED,
		vmf_schema_validate(&schema, &limits, nullptr));
	vmf_node scalar = schema_scalar(4);
	vmf_node body = {};

	body.id = 3;
	body.kind = VMF_NODE_GROUP;
	body.as.group = {&scalar, 1};
	field.kind = VMF_NODE_REPEAT;
	field.as.repeat = {};
	field.as.repeat.body = &body;
	field.as.repeat.count.kind = VMF_EXTENT_FIXED;
	field.as.repeat.count.fixed = 2;
	field.as.repeat.count.minimum = 2;
	field.as.repeat.count.maximum = 2;
	limits.max_repetitions = 1;
	EXPECT_EQ(VMF_LIMIT_EXCEEDED,
		vmf_schema_validate(&schema, &limits, nullptr));
}

TEST(SchemaContracts, SaturatedMinimumArithmeticCannotInventAZeroBitBody)
{
	vmf_node root;
	vmf_node repeat = {};
	vmf_node body = {};
	vmf_node field = {};
	auto schema = schema_with(&root, &repeat, 1);
	auto limits = vmf_default_limits();

	repeat.id = 2;
	repeat.kind = VMF_NODE_REPEAT;
	repeat.as.repeat.body = &body;
	repeat.as.repeat.count.kind = VMF_EXTENT_FIXED;
	repeat.as.repeat.count.fixed = 1;
	repeat.as.repeat.count.minimum = 1;
	repeat.as.repeat.count.maximum = 1;
	body.id = 3;
	body.kind = VMF_NODE_GROUP;
	body.as.group = {&field, 1};
	field.id = 4;
	field.kind = VMF_NODE_FIELD;
	field.as.field.kind = VMF_FIELD_BITS;
	auto &length = field.as.field.as.bits.length;

	length.kind = VMF_EXTENT_PREFIX;
	length.prefix = {63, VMF_BIT_LSB_FIRST, VMF_INTEGER_UNSIGNED};
	length.bias = INT64_MAX;
	length.minimum = UINT64_MAX - 62;
	length.maximum = UINT64_MAX - 1;
	limits.max_field_bits = UINT64_MAX;
	EXPECT_EQ(VMF_OK, vmf_schema_validate(&schema, &limits, nullptr));
}

TEST(SchemaContracts, DependenciesStayWithinDominatingOccurrenceScopes)
{
	vmf_node root;
	vmf_node fields[] = {schema_scalar(2), schema_scalar(3)};
	auto schema = schema_with(&root, fields, 2);
	auto limits = vmf_default_limits();
	vmf_code_range range = {0, 1};
	vmf_condition_term term = {2, UINT64_MAX, &range, 1, false};

	fields[1].presence.kind = VMF_PRESENCE_CONDITION;
	fields[1].presence.condition = {VMF_CONDITION_ALL, &term, 1, false};
	EXPECT_EQ(VMF_OK, vmf_schema_validate(&schema, &limits, nullptr));
	fields[0].presence.kind = VMF_PRESENCE_BIT;
	EXPECT_EQ(VMF_INVALID_SCHEMA,
		vmf_schema_validate(&schema, &limits, nullptr));
	fields[0].presence.kind = VMF_PRESENCE_ALWAYS;
	vmf_node nested_scalar = schema_scalar(4);

	fields[0].kind = VMF_NODE_GROUP;
	fields[0].as.group = {&nested_scalar, 1};
	term.field = 4;
	EXPECT_EQ(VMF_INVALID_SCHEMA,
		vmf_schema_validate(&schema, &limits, nullptr));
}

TEST(SchemaContracts, AlternativeChoiceArmsCannotSupplyDependencies)
{
	vmf_node root;
	vmf_node fields[] = {schema_scalar(2), {}};
	vmf_node first = schema_scalar(4);
	vmf_node dependent = schema_scalar(6);
	vmf_node second = {};
	vmf_code_range ranges[] = {{0, 0}, {1, 1}};
	vmf_choice_case arms[] = {
		{&ranges[0], 1, &first}, {&ranges[1], 1, &second}};
	vmf_condition_term term = {4, UINT64_MAX, ranges, 1, false};
	auto schema = schema_with(&root, fields, 2);
	auto limits = vmf_default_limits();

	fields[1].id = 3;
	fields[1].kind = VMF_NODE_CHOICE;
	fields[1].as.choice = {2, UINT64_MAX, arms, 2, nullptr};
	second.id = 5;
	second.kind = VMF_NODE_GROUP;
	second.as.group = {&dependent, 1};
	for (bool fallback : {false, true}) {
		SCOPED_TRACE(fallback);
		fields[1].as.choice.case_count = fallback ? 1 : 2;
		fields[1].as.choice.otherwise = fallback ? &second : nullptr;
		dependent = schema_scalar(6);
		dependent.presence.kind = VMF_PRESENCE_CONDITION;
		dependent.presence.condition = {
			VMF_CONDITION_ALL, &term, 1, false};
		vmf_error error = {};

		EXPECT_EQ(VMF_INVALID_SCHEMA,
			vmf_schema_validate(&schema, &limits, &error));
		EXPECT_EQ(6u, error.node_id);
		term.field = 2;
		EXPECT_EQ(VMF_OK,
			vmf_schema_validate(&schema, &limits, nullptr));
		term.field = 4;
		dependent.presence = {};
		dependent.as.field = {};
		dependent.as.field.kind = VMF_FIELD_OCTETS;
		auto *length = &dependent.as.field.as.octets.length;

		length->kind = VMF_EXTENT_FIELD;
		length->field = 4;
		length->maximum = 255;
		EXPECT_EQ(VMF_INVALID_SCHEMA,
			vmf_schema_validate(&schema, &limits, nullptr));
		length->field = 2;
		EXPECT_EQ(VMF_OK,
			vmf_schema_validate(&schema, &limits, nullptr));
		vmf_node nested = schema_scalar(7);

		dependent.kind = VMF_NODE_CHOICE;
		dependent.as.choice = {4, UINT64_MAX, nullptr, 0, &nested};
		EXPECT_EQ(VMF_INVALID_SCHEMA,
			vmf_schema_validate(&schema, &limits, nullptr));
		dependent.as.choice.selector = 2;
		EXPECT_EQ(VMF_OK,
			vmf_schema_validate(&schema, &limits, nullptr));
	}
}

TEST(SchemaContracts, DeepValidationUsesConfiguredStorage)
{
	const size_t count = 4096;
	std::vector<vmf_node> nodes(count);
	auto limits = vmf_default_limits();

	limits.max_schema_nodes = count;
	limits.max_depth = count - 1;
	for (size_t i = 0; i < count; ++i) {
		nodes[i].id = (uint32_t)(i + 1);
		nodes[i].kind = VMF_NODE_GROUP;
		if (i + 1 < count)
			nodes[i].as.group = {&nodes[i + 1], 1};
	}
	vmf_message_schema schema = {};

	schema.root = nodes.data();
	schema.octet_order = VMF_BIT_LSB_FIRST;
	EXPECT_EQ(VMF_OK, vmf_schema_validate(&schema, &limits, nullptr));
	--limits.max_depth;
	EXPECT_EQ(VMF_LIMIT_EXCEEDED,
		vmf_schema_validate(&schema, &limits, nullptr));
}

TEST(SchemaContracts, LookupVisitsEveryChoiceArmAndRepeatBody)
{
	vmf_node root;
	vmf_node fields[] = {schema_scalar(2), {}, schema_scalar(4)};
	vmf_node first = schema_scalar(5), value = schema_scalar(8);
	vmf_node fallback_value = schema_scalar(10);
	vmf_node repeat = {}, body = {}, fallback = {};
	vmf_code_range ranges[] = {{0, 0}, {1, 1}};
	vmf_choice_case arms[] = {
		{&ranges[0], 1, &first}, {&ranges[1], 1, &repeat}};
	auto schema = schema_with(&root, fields, 3);
	auto limits = vmf_default_limits();

	fields[1].id = 3;
	fields[1].kind = VMF_NODE_CHOICE;
	fields[1].as.choice = {2, UINT64_MAX, arms, 2, &fallback};
	repeat.id = 6;
	repeat.kind = VMF_NODE_REPEAT;
	repeat.as.repeat.body = &body;
	repeat.as.repeat.count.fixed = 1;
	repeat.as.repeat.count.maximum = 1;
	body.id = 7;
	body.kind = VMF_NODE_GROUP;
	body.as.group = {&value, 1};
	fallback.id = 9;
	fallback.kind = VMF_NODE_GROUP;
	fallback.as.group = {&fallback_value, 1};
	ASSERT_EQ(VMF_OK, vmf_schema_validate(&schema, &limits, nullptr));
	const vmf_node *expected[] = {&root, &fields[0], &fields[1],
		&fields[2], &first, &repeat, &body, &value, &fallback,
		&fallback_value};

	for (const auto *node : expected)
		EXPECT_EQ(node, vmf_schema_find(&schema, node->id));
	EXPECT_EQ(nullptr, vmf_schema_find(&schema, 11));
	fields[1].as.choice.otherwise = nullptr;
	ASSERT_EQ(VMF_OK, vmf_schema_validate(&schema, &limits, nullptr));
	EXPECT_EQ(&value, vmf_schema_find(&schema, 8));
	EXPECT_EQ(nullptr, vmf_schema_find(&schema, 9));
	EXPECT_EQ(&fields[2], vmf_schema_find(&schema, 4));
	fields[1].as.choice.case_count = 0;
	fields[1].as.choice.otherwise = &fallback;
	ASSERT_EQ(VMF_OK, vmf_schema_validate(&schema, &limits, nullptr));
	EXPECT_EQ(nullptr, vmf_schema_find(&schema, 5));
	EXPECT_EQ(&fallback_value, vmf_schema_find(&schema, 10));
	EXPECT_EQ(&fields[2], vmf_schema_find(&schema, 4));
}

TEST(SchemaContracts, DeepLookupPreservesOriginalNodesOnSmallStacks)
{
	for (bool branching : {false, true}) {
		SCOPED_TRACE(branching);
		const size_t count = branching ? 4095 : 4096;
		std::vector<vmf_node> nodes(count);
		auto limits = vmf_default_limits();

		limits.max_depth = count - 1;
		for (size_t i = 0; i < count; ++i)
			nodes[i] = schema_scalar((uint32_t)(i + 1));
		for (size_t i = 0;;) {
			size_t next = i + (branching && i ? 2 : 1);

			if (next >= count)
				break;
			nodes[i].kind = VMF_NODE_GROUP;
			nodes[i].as.group = {
				&nodes[next], branching ? 2u : 1u};
			i = next;
		}
		vmf_message_schema schema = {};

		schema.root = nodes.data();
		schema.octet_order = VMF_BIT_LSB_FIRST;
		ASSERT_EQ(VMF_OK,
			vmf_schema_validate(&schema, &limits, nullptr));
		auto lookup = [&]() {
			for (size_t i : {size_t(0), size_t(2), count / 2,
				     count - 1})
				if (vmf_schema_find(&schema, nodes[i].id) !=
					&nodes[i])
					return 1;
			return vmf_schema_find(&schema, (uint32_t)count + 1) ||
				vmf_schema_find(&schema, 0) ||
				vmf_schema_find(nullptr, 1) ? 1 : 0;
		};

#if defined(__unix__) || defined(__APPLE__)
		ASSERT_EXIT({
			rlimit limit = {};

			if (getrlimit(RLIMIT_STACK, &limit))
				std::_Exit(2);
			limit.rlim_cur = 128 * 1024;
			if (setrlimit(RLIMIT_STACK, &limit))
				std::_Exit(2);
			std::_Exit(lookup());
		}, ::testing::ExitedWithCode(0), "");
#else
		EXPECT_EQ(0, lookup());
#endif
	}
}
