#include <gtest/gtest.h>
#include "../../src/schema_validation.hpp"
#include <cstddef>
#include <cstdlib>
#include <vector>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif
static vmf_status validate_runtime(const vmf_runtime_schema *schema,
	const vmf_limits *limits, vmf_error *error)
{
	return runtime_schema_validate(schema, limits, nullptr, error);
}

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

static vmf_runtime_schema schema_with(
	vmf_node *root, const vmf_node *children, size_t count)
{
	*root = {};
	root->id = 1;
	root->kind = VMF_NODE_GROUP;
	root->as.group = {children, count};
	vmf_runtime_schema schema = {};

	schema.root = root;
	schema.octet_order = VMF_BIT_LSB_FIRST;
	return schema;
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
	EXPECT_EQ(VMF_OK, validate_runtime(&schema, &limits, nullptr));
	const char bad[][4] = {{(char)0xc0, (char)0x80, 0, 0},
		{(char)0xed, (char)0xa0, (char)0x80, 0},
		{(char)0xf4, (char)0x90, (char)0x80, (char)0x80},
		{(char)0xe2, (char)0x82, 0, 0}};
	const size_t lengths[] = {2, 3, 4, 2};

	for (size_t i = 0; i < 4; ++i) {
		label.name = {bad[i], lengths[i]};
		EXPECT_EQ(VMF_INVALID_SCHEMA,
			validate_runtime(&schema, &limits, nullptr));
	}
	field.as.field.kind = VMF_FIELD_SCALED;
	field.as.field.as.scalar.labels = nullptr;
	field.as.field.as.scalar.label_count = 0;
	field.as.field.as.scalar.scale.factor = {1, 1};
	field.as.field.as.scalar.scale.bias = {0, 1};
	field.as.field.as.scalar.scale.sentinels = &label;
	field.as.field.as.scalar.scale.sentinel_count = 1;
	EXPECT_EQ(VMF_INVALID_SCHEMA,
		validate_runtime(&schema, &limits, nullptr));
	label.name = {valid, sizeof(valid)};
	EXPECT_EQ(VMF_OK, validate_runtime(&schema, &limits, nullptr));
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
	const vmf_runtime_schema *entries[] = {&schema};
	vmf_runtime_set set = {entries, 1};
	schema_allocation log;
	vmf_allocator allocator = {
		schema_misaligned_allocate, schema_misaligned_free, &log};
	vmf_error error = {};

	EXPECT_EQ(VMF_INVALID_ARGUMENT,
		runtime_set_validate(
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
		validate_runtime(&schema, &limits, nullptr));
	length->kind = VMF_EXTENT_PREFIX;
	length->prefix = {8, VMF_BIT_LSB_FIRST, VMF_INTEGER_UNSIGNED};
	EXPECT_EQ(VMF_LIMIT_EXCEEDED,
		validate_runtime(&schema, &limits, nullptr));
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
		validate_runtime(&schema, &limits, nullptr));
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
	EXPECT_EQ(VMF_OK, validate_runtime(&schema, &limits, nullptr));
}

TEST(SchemaContracts, DependenciesStayWithinDominatingOccurrenceScopes)
{
	vmf_node root;
	vmf_node fields[] = {schema_scalar(2), schema_scalar(3)};
	auto schema = schema_with(&root, fields, 2);
	auto limits = vmf_default_limits();
	vmf_code_range range = {0, 1};
	vmf_runtime_condition_term term = {2, UINT64_MAX, &range, 1, false};

	fields[1].presence.kind = VMF_PRESENCE_CONDITION;
	fields[1].presence.condition = {VMF_CONDITION_ALL, &term, 1, false};
	EXPECT_EQ(VMF_OK, validate_runtime(&schema, &limits, nullptr));
	fields[0].presence.kind = VMF_PRESENCE_BIT;
	EXPECT_EQ(VMF_INVALID_SCHEMA,
		validate_runtime(&schema, &limits, nullptr));
	fields[0].presence.kind = VMF_PRESENCE_ALWAYS;
	vmf_node nested_scalar = schema_scalar(4);

	fields[0].kind = VMF_NODE_GROUP;
	fields[0].as.group = {&nested_scalar, 1};
	term.field = 4;
	EXPECT_EQ(VMF_INVALID_SCHEMA,
		validate_runtime(&schema, &limits, nullptr));
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
	vmf_runtime_condition_term term = {4, UINT64_MAX, ranges, 1, false};
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
			validate_runtime(&schema, &limits, &error));
		EXPECT_EQ(6u, error.index_position);
		term.field = 2;
		EXPECT_EQ(VMF_OK,
			validate_runtime(&schema, &limits, nullptr));
		term.field = 4;
		dependent.presence = {};
		dependent.as.field = {};
		dependent.as.field.kind = VMF_FIELD_OCTETS;
		dependent.as.field.as.octets.order = VMF_BIT_MSB_FIRST;
		auto *length = &dependent.as.field.as.octets.length;

		length->kind = VMF_EXTENT_FIELD;
		length->field = 4;
		length->maximum = 255;
		EXPECT_EQ(VMF_INVALID_SCHEMA,
			validate_runtime(&schema, &limits, nullptr));
		length->field = 2;
		EXPECT_EQ(VMF_OK,
			validate_runtime(&schema, &limits, nullptr));
		vmf_node nested = schema_scalar(7);

		dependent.kind = VMF_NODE_CHOICE;
		dependent.as.choice = {4, UINT64_MAX, nullptr, 0, &nested};
		EXPECT_EQ(VMF_INVALID_SCHEMA,
			validate_runtime(&schema, &limits, nullptr));
		dependent.as.choice.selector = 2;
		EXPECT_EQ(VMF_OK,
			validate_runtime(&schema, &limits, nullptr));
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
	vmf_runtime_schema schema = {};

	schema.root = nodes.data();
	schema.octet_order = VMF_BIT_LSB_FIRST;
	EXPECT_EQ(VMF_OK, validate_runtime(&schema, &limits, nullptr));
	--limits.max_depth;
	EXPECT_EQ(VMF_LIMIT_EXCEEDED,
		validate_runtime(&schema, &limits, nullptr));
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
	ASSERT_EQ(VMF_OK, validate_runtime(&schema, &limits, nullptr));
	const vmf_node *expected[] = {&root, &fields[0], &fields[1],
		&fields[2], &first, &repeat, &body, &value, &fallback,
		&fallback_value};

	for (const auto *node : expected)
		EXPECT_EQ(node, runtime_schema_find(&schema, node->id));
	EXPECT_EQ(nullptr, runtime_schema_find(&schema, 11));
	fields[1].as.choice.otherwise = nullptr;
	ASSERT_EQ(VMF_OK, validate_runtime(&schema, &limits, nullptr));
	EXPECT_EQ(&value, runtime_schema_find(&schema, 8));
	EXPECT_EQ(nullptr, runtime_schema_find(&schema, 9));
	EXPECT_EQ(&fields[2], runtime_schema_find(&schema, 4));
	fields[1].as.choice.case_count = 0;
	fields[1].as.choice.otherwise = &fallback;
	ASSERT_EQ(VMF_OK, validate_runtime(&schema, &limits, nullptr));
	EXPECT_EQ(nullptr, runtime_schema_find(&schema, 5));
	EXPECT_EQ(&fallback_value, runtime_schema_find(&schema, 10));
	EXPECT_EQ(&fields[2], runtime_schema_find(&schema, 4));
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
		vmf_runtime_schema schema = {};

		schema.root = nodes.data();
		schema.octet_order = VMF_BIT_LSB_FIRST;
		ASSERT_EQ(VMF_OK,
			validate_runtime(&schema, &limits, nullptr));
		auto lookup = [&]() {
			for (size_t i : {size_t(0), size_t(2), count / 2,
				count - 1})
				if (runtime_schema_find(&schema, nodes[i].id) !=
					&nodes[i])
					return 1;
			return runtime_schema_find(&schema,
				(uint32_t)count + 1) ||
				runtime_schema_find(&schema, 0) ||
				runtime_schema_find(nullptr, 1) ? 1 : 0;
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

TEST(NativeSchemaContracts, OrdinaryInitializersResolveWithoutJson)
{
	const uint32_t parts[] = {1};
	vmf_index_schema index = {};
	index.index_number = {parts, 1};
	index.number_of_bits = 5;
	vmf_message_schema message = {};
	message.message_number = {1, 1};
	message.indexes = &index;
	message.index_count = 1;
	vmf_schema_set set = {VMF_ENVELOPE_C, &message, 1, {}};
	auto limits = vmf_default_limits();
	ASSERT_EQ(VMF_OK,
		vmf_schema_set_validate(&set, &limits, nullptr, nullptr));
}

struct native_table {
	uint32_t numbers[8];
	vmf_index_schema indexes[8];
	vmf_message_schema message;
	vmf_schema_set set;
};

static void initialize_native(native_table *table, size_t count)
{
	*table = {};
	for (size_t i = 0; i < count; ++i) {
		table->numbers[i] = (uint32_t)i + 1;
		table->indexes[i].index_number = {&table->numbers[i], 1};
		table->indexes[i].number_of_bits = 5;
	}
	table->message.message_number = {1, 1};
	table->message.indexes = table->indexes;
	table->message.index_count = count;
	table->set = {VMF_ENVELOPE_C, &table->message, 1, {}};
}

static vmf_status validate_native(native_table *table, vmf_error *error =
	nullptr)
{
	auto limits = vmf_default_limits();
	return vmf_schema_set_validate(&table->set, &limits, nullptr, error);
}

TEST(NativeSchemaContracts, ProfileVersionsResolveAndPresenceRemainsExact)
{
	native_table table;
	initialize_native(&table, 1);
	vmf_message_id id = {};
	ASSERT_EQ(VMF_OK,
		vmf_schema_message_id(&table.set, &table.message, &id));
	EXPECT_EQ(2u, id.user_message_format);
	EXPECT_FALSE(id.standard_version.present);
	table.set.standard.kind = VMF_STANDARD_EDITION;
	table.set.standard.as.edition = VMF_STANDARD_6017B;
	ASSERT_EQ(VMF_OK,
		vmf_schema_message_id(&table.set, &table.message, &id));
	EXPECT_EQ(8u, id.standard_version.value);
	table.set.envelope = VMF_ENVELOPE_D1;
	ASSERT_EQ(VMF_OK,
		vmf_schema_message_id(&table.set, &table.message, &id));
	EXPECT_EQ(7u, id.standard_version.value);
	table.message.standard.kind = VMF_STANDARD_RAW_CODE;
	table.message.standard.as.raw_code = 0;
	ASSERT_EQ(VMF_OK,
		vmf_schema_message_id(&table.set, &table.message, &id));
	EXPECT_TRUE(id.standard_version.present);
	EXPECT_EQ(0u, id.standard_version.value);
	table.message.standard.kind = VMF_STANDARD_ABSENT;
	EXPECT_EQ(VMF_INVALID_SCHEMA, validate_native(&table));
	table.set.envelope = VMF_ENVELOPE_B;
	table.set.standard.kind = VMF_STANDARD_ABSENT;
	EXPECT_EQ(VMF_OK, validate_native(&table));
	table.message.standard.kind = VMF_STANDARD_INHERIT;
	table.set.standard.kind = VMF_STANDARD_EDITION;
	EXPECT_EQ(VMF_INVALID_SCHEMA, validate_native(&table));
	table.set.envelope = VMF_ENVELOPE_C;
	table.set.standard.as.edition = VMF_STANDARD_6017D;
	EXPECT_EQ(VMF_INVALID_SCHEMA, validate_native(&table));
}

TEST(NativeSchemaContracts, ReflectionUsesCanonicalBorrowedDefinitions)
{
	native_table table;
	initialize_native(&table, 2);
	table.indexes[1].key = {"binding", 7};
	vmf_index_info info = {};
	ASSERT_EQ(VMF_OK, validate_native(&table));
	ASSERT_EQ(VMF_OK, vmf_schema_index_info(&table.set, &table.message,
		&table.indexes[1], &info));
	EXPECT_EQ(VMF_FIELD_UNSIGNED, info.kind);
	EXPECT_EQ(5u, info.integer.bit_width);
	EXPECT_EQ(VMF_BIT_LSB_FIRST, info.integer.order);
	EXPECT_EQ(VMF_INTEGER_UNSIGNED, info.integer.representation);
	EXPECT_EQ(&table.indexes[1], vmf_schema_find(&table.message,
		table.indexes[1].index_number));
	EXPECT_EQ(&table.indexes[1], vmf_schema_find_key(&table.message,
		{"binding", 7}));
	EXPECT_EQ(table.indexes[1].key.data, info.key.data);
	vmf_index_schema foreign = table.indexes[1];
	EXPECT_EQ(VMF_INVALID_ARGUMENT, vmf_schema_index_info(&table.set,
		&table.message, &foreign, &info));
	EXPECT_EQ(0u, info.number_of_bits);
	table.indexes[0].key = table.indexes[1].key;
	vmf_error error;
	EXPECT_EQ(VMF_INVALID_SCHEMA, validate_native(&table, &error));
	EXPECT_EQ(VMF_PROPERTY_KEY, error.property);
	EXPECT_EQ(1u, error.index_position);
	EXPECT_EQ(0u, error.related_index_position);
}

TEST(NativeSchemaContracts, WidthInterpretationAndTextCapacitiesAreExplicit)
{
	native_table table;
	initialize_native(&table, 1);
	table.indexes[0].number_of_bits = 65;
	EXPECT_EQ(VMF_INVALID_SCHEMA, validate_native(&table));
	table.indexes[0].encode_type = VMF_ENCODE_BITS;
	EXPECT_EQ(VMF_OK, validate_native(&table));
	table.indexes[0].encode_type = VMF_ENCODE_TEXT;
	table.indexes[0].number_of_bits = 112;
	EXPECT_EQ(VMF_OK, validate_native(&table));
	vmf_index_info info;
	ASSERT_EQ(VMF_OK, vmf_schema_index_info(&table.set, &table.message,
		&table.indexes[0], &info));
	EXPECT_EQ(16u, info.capacity_units);
	EXPECT_EQ(VMF_TEXT_ASCII_7, info.text_encoding);
	EXPECT_FALSE(info.variable);
	table.indexes[0].encode_type = VMF_ENCODE_LITERAL;
	ASSERT_EQ(VMF_OK, validate_native(&table));
	table.indexes[0].number_of_bits = 111;
	EXPECT_EQ(VMF_INVALID_SCHEMA, validate_native(&table));
	table.indexes[0].number_of_bits = 8;
	table.indexes[0].encode_type = VMF_ENCODE_SIGNED;
	EXPECT_EQ(VMF_INVALID_SCHEMA, validate_native(&table));
	vmf_index_refinements refinements = {};
	refinements.representation = VMF_INTEGER_ONES_COMPLEMENT;
	table.indexes[0].refinements = &refinements;
	EXPECT_EQ(VMF_OK, validate_native(&table));
}

TEST(NativeSchemaContracts, IndicatorsMapOnceAndNormalizeStableScopes)
{
	native_table table;
	initialize_native(&table, 4);
	table.indexes[0].number_of_bits = 0;
	table.indexes[0].indicator = VMF_INDICATOR_GPI;
	table.indexes[0].target.kind = VMF_TARGET_GROUP;
	table.indexes[0].target.as.group = 1;
	const vmf_group_id group = 1;
	vmf_repeat_component repeats[] = {{1, {true, 4}}, {1, {false, 0}}};
	for (size_t i = 1; i < 4; ++i) {
		table.indexes[i].group_code = {&group, 1};
		table.indexes[i].repeat_code = {&repeats[i == 1 ? 0 : 1], 1};
	}
	table.indexes[1].number_of_bits = 0;
	table.indexes[1].indicator = VMF_INDICATOR_GRI;
	auto limits = vmf_default_limits();
	vmf_runtime_set normalized = {};
	ASSERT_EQ(VMF_OK, vmf_internal::normalize_schemas(&table.set, &limits,
		nullptr, &normalized, nullptr));
	ASSERT_EQ(1u, normalized.count);
	const vmf_node *root = normalized.schemas[0]->root;
	ASSERT_EQ(1u, root->as.group.child_count);
	const vmf_node *g = root->as.group.children;
	EXPECT_EQ(VMF_SCOPE_GROUP, g->scope.kind);
	EXPECT_EQ(VMF_PRESENCE_BIT, g->presence.kind);
	ASSERT_EQ(1u, g->as.group.child_count);
	const vmf_node *repeat = g->as.group.children;
	EXPECT_EQ(VMF_SCOPE_REPEAT, repeat->scope.kind);
	EXPECT_EQ(VMF_EXTENT_RECURRENCE_BEFORE,
		repeat->as.repeat.count.kind);
	const vmf_node *body = repeat->as.repeat.body;
	EXPECT_EQ(VMF_SCOPE_REPEAT_BODY, body->scope.kind);
	ASSERT_EQ(2u, body->as.group.child_count);
	EXPECT_EQ(&table.indexes[2], body->as.group.children[0].source);
	EXPECT_EQ(&table.indexes[3], body->as.group.children[1].source);
	EXPECT_EQ(6u, normalized.schemas[0]->binding_count);
	vmf_internal::destroy_normalized_set(&normalized, nullptr);
	table.indexes[1].number_of_bits = 2;
	EXPECT_EQ(VMF_INVALID_SCHEMA, validate_native(&table));
	table.indexes[1].number_of_bits = 0;
	table.indexes[1].indicator = VMF_INDICATOR_FRI;
	EXPECT_EQ(VMF_INVALID_SCHEMA, validate_native(&table));
}

TEST(NativeSchemaContracts, AmbiguousReopenedAndCrossingScopesFail)
{
	native_table table;
	initialize_native(&table, 3);
	vmf_group_id group = 1;
	vmf_repeat_component repeat = {1, {false, 0}};
	vmf_repeat_definition declaration = {};
	declaration.id = 1;
	declaration.count = 2;
	table.message.repeats = &declaration;
	table.message.repeat_count = 1;
	for (size_t i = 0; i < 3; ++i) {
		table.indexes[i].group_code = {&group, 1};
		table.indexes[i].repeat_code = {&repeat, 1};
	}
	EXPECT_EQ(VMF_INVALID_SCHEMA, validate_native(&table));
	table.message.repeat_count = 0;
	for (size_t i = 0; i < 3; ++i)
		table.indexes[i].repeat_code = {};
	table.indexes[1].group_code = {};
	EXPECT_EQ(VMF_INVALID_SCHEMA, validate_native(&table));
	table.indexes[1].group_code = {&group, 1};
	table.indexes[2].group_code = {};
	table.indexes[1].repeat_code = {&repeat, 1};
	table.indexes[2].repeat_code = {&repeat, 1};
	table.message.repeat_count = 1;
	EXPECT_EQ(VMF_INVALID_SCHEMA, validate_native(&table));
}

TEST(NativeSchemaContracts, CountAndConditionDependenciesStayInTheirOccurrence)
{
	native_table table;
	initialize_native(&table, 3);
	vmf_repeat_component repeat = {1, {false, 0}};
	vmf_repeat_definition declaration = {};
	declaration.id = 1;
	declaration.kind = VMF_REPEAT_FIELD;
	declaration.count_from = table.indexes[0].index_number;
	declaration.maximum = 4;
	table.message.repeats = &declaration;
	table.message.repeat_count = 1;
	table.indexes[1].repeat_code = {&repeat, 1};
	table.indexes[2].repeat_code = {&repeat, 1};
	EXPECT_EQ(VMF_OK, validate_native(&table));
	declaration.count_from = table.indexes[2].index_number;
	EXPECT_EQ(VMF_INVALID_SCHEMA, validate_native(&table));
	declaration.count_from = table.indexes[0].index_number;
	vmf_code_range range = {0, 2};
	vmf_condition_term term = {};
	term.field = table.indexes[1].index_number;
	term.values = &range;
	term.value_count = 1;
	vmf_condition condition = {VMF_CONDITION_ALL, &term, 1, false};
	vmf_index_refinements refinements = {};
	refinements.when = &condition;
	table.indexes[2].refinements = &refinements;
	EXPECT_EQ(VMF_OK, validate_native(&table));
	table.indexes[2].repeat_code = {};
	EXPECT_EQ(VMF_INVALID_SCHEMA, validate_native(&table));
}

struct native_allocations {
	size_t attempts, live, fail_at;
};

static void *native_allocate(void *user, size_t size)
{
	auto *state = static_cast<native_allocations *>(user);
	if (state->attempts++ == state->fail_at)
		return nullptr;
	void *memory = std::malloc(size);
	if (memory)
		++state->live;
	return memory;
}

static void native_free(void *user, void *memory)
{
	auto *state = static_cast<native_allocations *>(user);
	--state->live;
	std::free(memory);
}

TEST(NativeSchemaContracts, ValidationAllocationFailuresReleaseAllStorage)
{
	native_table table;
	initialize_native(&table, 2);
	auto limits = vmf_default_limits();
	native_allocations state = {0, 0, SIZE_MAX};
	vmf_allocator allocator = {native_allocate, native_free, &state};
	ASSERT_EQ(VMF_OK, vmf_schema_set_validate(&table.set, &limits,
		&allocator, nullptr));
	EXPECT_EQ(0u, state.live);
	size_t attempts = state.attempts;
	for (size_t failure = 0; failure < attempts; ++failure) {
		state = {0, 0, failure};
		vmf_error error;
		EXPECT_EQ(VMF_OUT_OF_MEMORY, vmf_schema_set_validate(&table.set,
			&limits, &allocator, &error));
		EXPECT_EQ(VMF_OUT_OF_MEMORY, error.status);
		EXPECT_EQ(0u, state.live);
	}
}

TEST(NativeSchemaContracts, EmptyRegistriesStillValidateExplicitDefaults)
{
	auto limits = vmf_default_limits();
	vmf_schema_set set = {VMF_ENVELOPE_C, nullptr, 0, {}};
	EXPECT_EQ(VMF_OK,
		vmf_schema_set_validate(&set, &limits, nullptr, nullptr));
	set.standard.kind = VMF_STANDARD_RAW_CODE;
	set.standard.as.raw_code = 15;
	EXPECT_EQ(VMF_INVALID_SCHEMA,
		vmf_schema_set_validate(&set, &limits, nullptr, nullptr));
	set.envelope = VMF_ENVELOPE_B;
	set.standard.kind = VMF_STANDARD_EDITION;
	set.standard.as.edition = VMF_STANDARD_6017B;
	EXPECT_EQ(VMF_INVALID_SCHEMA,
		vmf_schema_set_validate(&set, &limits, nullptr, nullptr));
}

TEST(NativeSchemaContracts, OnlyExplicitVariableModesAllowZeroCapacity)
{
	native_table table;
	initialize_native(&table, 2);
	table.indexes[1].number_of_bits = 0;
	table.indexes[1].encode_type = VMF_ENCODE_TEXT;
	EXPECT_EQ(VMF_INVALID_SCHEMA, validate_native(&table));
	vmf_index_refinements refinements = {};
	refinements.length_from = table.indexes[0].index_number;
	table.indexes[1].refinements = &refinements;
	EXPECT_EQ(VMF_OK, validate_native(&table));
	table.indexes[1].refinements = nullptr;
	table.indexes[1].encode_type = VMF_ENCODE_LITERAL;
	EXPECT_EQ(VMF_OK, validate_native(&table));
}

TEST(NativeSchemaContracts, SingleValidationKeepsOriginalRegistryPosition)
{
	vmf_message_schema messages[2] = {};
	uint32_t number = 1;
	vmf_index_schema index = {};
	index.index_number = {&number, 1};
	messages[0].message_number = {1, 1};
	messages[1].message_number = {1, 2};
	messages[1].indexes = &index;
	messages[1].index_count = 1;
	vmf_schema_set set = {VMF_ENVELOPE_C, messages, 2, {}};
	auto limits = vmf_default_limits();
	vmf_error error;
	EXPECT_EQ(VMF_INVALID_SCHEMA, vmf_schema_validate(&set, &messages[1],
		&limits, nullptr, &error));
	EXPECT_EQ(1u, error.schema_index);
	EXPECT_EQ(0u, error.index_position);
	set.message_count = 0;
	limits.max_schema_nodes = 0;
	limits.max_depth = 0;
	EXPECT_EQ(VMF_OK,
		vmf_schema_set_validate(&set, &limits, nullptr, nullptr));
}

TEST(NativeSchemaContracts, OversizedSourceCollectionsFailBeforeTraversal)
{
	native_table table;
	initialize_native(&table, 2);
	auto limits = vmf_default_limits();
	vmf_index_refinements refinements = {};
	table.indexes[1].refinements = &refinements;
	refinements.allowed_count = SIZE_MAX;
	EXPECT_EQ(VMF_LIMIT_EXCEEDED, vmf_schema_set_validate(&table.set,
		&limits, nullptr, nullptr));
	refinements.allowed_count = 0;
	refinements.length_from = {&table.numbers[0], SIZE_MAX};
	table.indexes[1].encode_type = VMF_ENCODE_BITS;
	EXPECT_EQ(VMF_LIMIT_EXCEEDED, vmf_schema_set_validate(&table.set,
		&limits, nullptr, nullptr));
	refinements.length_from = {};
	vmf_code_range range = {0, 1};
	vmf_condition_term term = {};
	term.field = {&table.numbers[0], SIZE_MAX};
	term.values = &range;
	term.value_count = 1;
	vmf_condition condition = {VMF_CONDITION_ALL, &term, 1, false};
	refinements.when = &condition;
	EXPECT_EQ(VMF_LIMIT_EXCEEDED, vmf_schema_set_validate(&table.set,
		&limits, nullptr, nullptr));
}

TEST(NativeSchemaContracts, ControllerIndexAncestryCannotContradictScopes)
{
	native_table table;
	initialize_native(&table, 3);
	const uint32_t outer_number[] = {4, 1};
	const uint32_t inner_number[] = {4};
	const uint32_t data_number[] = {4, 1, 2};
	const vmf_group_id outer_group[] = {1};
	const vmf_group_id inner_groups[] = {1, 2};
	table.indexes[0].index_number = {outer_number, 2};
	table.indexes[1].index_number = {inner_number, 1};
	table.indexes[2].index_number = {data_number, 3};
	for (size_t i = 0; i < 2; ++i) {
		table.indexes[i].number_of_bits = 0;
		table.indexes[i].indicator = VMF_INDICATOR_GPI;
		table.indexes[i].target.kind = VMF_TARGET_GROUP;
		table.indexes[i].target.as.group = (uint32_t)i + 1;
	}
	table.indexes[1].group_code = {outer_group, 1};
	table.indexes[2].group_code = {inner_groups, 2};
	EXPECT_EQ(VMF_INVALID_SCHEMA, validate_native(&table));
	table.indexes[0].index_number = {inner_number, 1};
	table.indexes[1].index_number = {outer_number, 2};
	EXPECT_EQ(VMF_OK, validate_native(&table));
}

TEST(NativeSchemaContracts, RawBitsRejectInapplicableOrderRefinements)
{
	native_table table;

	initialize_native(&table, 1);
	table.indexes[0].encode_type = VMF_ENCODE_BITS;
	vmf_index_refinements refinements = {};

	table.indexes[0].refinements = &refinements;
	EXPECT_EQ(VMF_OK, validate_native(&table));
	refinements.bit_order = VMF_BIT_LSB_FIRST;
	EXPECT_EQ(VMF_INVALID_SCHEMA, validate_native(&table));
	refinements.bit_order = VMF_BIT_MSB_FIRST;
	EXPECT_EQ(VMF_INVALID_SCHEMA, validate_native(&table));
}
