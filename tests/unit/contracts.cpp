#include <gtest/gtest.h>
#include <vmf/wire.hpp>
#include <cstdlib>
#include <cstring>

extern "C" int vmf_c_consumer(void);

static vmf_options options()
{
	vmf_options result = {};

	result.profile = VMF_ENVELOPE_D1;
	result.limits = vmf_default_limits();
	return result;
}

TEST(Headers, CAndCppConsumersLink)
{
	EXPECT_EQ(1, vmf_c_consumer());
	EXPECT_STREQ(
		"not_implemented", vmf::status_string(VMF_NOT_IMPLEMENTED));
	EXPECT_STREQ("unknown", vmf_status_string((vmf_status)999));
	EXPECT_EQ(sizeof(vmf_header_d1),
		vmf::header_layout(VMF_ENVELOPE_D1).size);
	EXPECT_EQ(0u, vmf_field_layout((vmf_field_kind)999).size);
	EXPECT_EQ(0u, vmf_header_layout(15).size);
}

TEST(Identity, PresenceIsExactAndAbsentValuesAreIgnored)
{
	vmf_message_id a = {};
	vmf_message_id b = {};

	b.standard_version.value = 9;
	b.message_number = 7;
	EXPECT_TRUE(vmf_message_id_equal(&a, &b));
	b.standard_version.present = true;
	EXPECT_FALSE(vmf_message_id_equal(&a, &b));
	a.standard_version = b.standard_version;
	a.identification_present = b.identification_present = true;
	EXPECT_FALSE(vmf_message_id_equal(&a, &b));
	a.message_number = b.message_number;
	EXPECT_TRUE(vmf_message_id_equal(&a, &b));
	b.subtype.present = true;
	EXPECT_FALSE(vmf_message_id_equal(&a, &b));
}

TEST(Errors, FailureOutputsAreDeterministic)
{
	vmf_options config = options();
	vmf_decoder *reader = nullptr;
	vmf_error error = {};

	config.profile = (vmf_envelope_version)0;
	EXPECT_EQ(VMF_INVALID_ARGUMENT,
		vmf_decoder_create(&config, &reader, &error));
	EXPECT_EQ(nullptr, reader);
	EXPECT_EQ(VMF_INVALID_ARGUMENT, error.status);
	EXPECT_EQ(SIZE_MAX, error.message_index);
	vmf_event event = {};
	event.kind = VMF_EVENT_PDU_END;
	vmf_input input = {nullptr, 0, 0, true};
	EXPECT_EQ(VMF_INVALID_ARGUMENT,
		vmf_decoder_next(nullptr, &input, &event));
	EXPECT_EQ(nullptr, event.node);
	EXPECT_EQ(0u, input.used);
	vmf_measurement measured = {1, 2, 3};
	EXPECT_EQ(VMF_INVALID_ARGUMENT,
		vmf_encoder_measurement(nullptr, 0, &measured));
	EXPECT_EQ(0u, measured.pdu_octets);
}

/* Product tests assert observable public contracts. */
TEST(Allocation, DefaultCreateResetAndDestroy)
{
	vmf_options config = options();
	vmf_decoder *reader = nullptr;
	vmf_error error = {};

	ASSERT_EQ(VMF_OK, vmf_decoder_create(&config, &reader, &error));
	ASSERT_NE(nullptr, reader);
	vmf_decoder_reset(reader);
	EXPECT_EQ(VMF_OK, vmf_decoder_error(reader).status);
	vmf_decoder_destroy(reader);
}

struct allocation_log {
	size_t attempts = 0;
	size_t live = 0;
	size_t fail_at = SIZE_MAX;
};

static void *allocate(void *user, size_t size)
{
	auto *log = static_cast<allocation_log *>(user);

	if (log->attempts++ == log->fail_at)
		return nullptr;
	void *memory = std::malloc(size);

	if (memory)
		++log->live;
	return memory;
}

static void deallocate(void *user, void *memory)
{
	auto *log = static_cast<allocation_log *>(user);

	--log->live;
	std::free(memory);
}

TEST(Allocation, EveryPartialFailureRollsBackAndCanRecover)
{
	allocation_log log;
	vmf_allocator allocator = {allocate, deallocate, &log};
	vmf_options config = options();
	vmf_decoder *reader = nullptr;

	config.allocator = &allocator;
	ASSERT_EQ(VMF_OK, vmf_decoder_create(&config, &reader, nullptr));
	size_t allocations = log.attempts;

	ASSERT_GT(allocations, 0u);
	vmf_decoder_destroy(reader);
	ASSERT_EQ(0u, log.live);
	for (size_t i = 0; i < allocations; ++i) {
		log.attempts = 0;
		log.fail_at = i;
		reader = nullptr;
		EXPECT_EQ(VMF_OUT_OF_MEMORY,
			vmf_decoder_create(&config, &reader, nullptr));
		EXPECT_EQ(nullptr, reader);
		EXPECT_EQ(0u, log.live);
	}
	log.fail_at = SIZE_MAX;
	ASSERT_EQ(VMF_OK, vmf_decoder_create(&config, &reader, nullptr));
	/* Destroy must use its copied allocator, not this changed struct. */
	allocator.deallocate = nullptr;
	vmf_decoder_destroy(reader);
	EXPECT_EQ(0u, log.live);
}

TEST(Allocation, RejectsUnpairedCallbacksAndMissingProfile)
{
	allocation_log log;
	vmf_allocator allocator = {allocate, nullptr, &log};
	vmf_options config = options();
	vmf_encoder *writer = nullptr;

	config.allocator = &allocator;
	EXPECT_EQ(VMF_INVALID_ARGUMENT,
		vmf_encoder_create(&config, false, &writer, nullptr));
	EXPECT_EQ(0u, log.attempts);
	config.allocator = nullptr;
	config.profile = (vmf_envelope_version)0;
	EXPECT_EQ(VMF_INVALID_ARGUMENT,
		vmf_encoder_create(&config, false, &writer, nullptr));
}

static vmf_message_schema empty_schema(vmf_node *root)
{
	*root = {};
	root->id = 1;
	root->kind = VMF_NODE_GROUP;
	vmf_message_schema schema = {};

	schema.root = root;
	schema.octet_order = VMF_BIT_LSB_FIRST;
	return schema;
}

TEST(Schema, EmptyRootIsValidButZeroIdAndCyclesAreInvalid)
{
	vmf_node root;
	auto schema = empty_schema(&root);
	auto limits = vmf_default_limits();
	vmf_error error = {};

	EXPECT_EQ(VMF_OK, vmf_schema_validate(&schema, &limits, &error));
	root.id = 0;
	EXPECT_EQ(VMF_INVALID_SCHEMA,
		vmf_schema_validate(&schema, &limits, &error));
	root.id = 1;
	root.as.group.children = &root;
	root.as.group.child_count = 1;
	EXPECT_EQ(VMF_INVALID_SCHEMA,
		vmf_schema_validate(&schema, &limits, &error));
}

TEST(Schema, NodeAndDepthLimitsIncludeRootAndRejectOverflow)
{
	vmf_node root;
	auto schema = empty_schema(&root);
	auto limits = vmf_default_limits();

	limits.max_schema_nodes = 1;
	EXPECT_EQ(VMF_OK, vmf_schema_validate(&schema, &limits, nullptr));
	vmf_node child = root;

	child.id = 2;
	root.as.group = {&child, 1};
	EXPECT_EQ(VMF_LIMIT_EXCEEDED,
		vmf_schema_validate(&schema, &limits, nullptr));
	limits.max_schema_nodes = 2;
	limits.max_depth = 0;
	EXPECT_EQ(VMF_LIMIT_EXCEEDED,
		vmf_schema_validate(&schema, &limits, nullptr));
}

TEST(Schema, DuplicateIdentityAndExactSelection)
{
	vmf_node root;
	auto a = empty_schema(&root);
	auto b = a;
	const vmf_message_schema *entries[] = {&a, &b};
	vmf_schema_set set = {entries, 2};
	auto limits = vmf_default_limits();
	const vmf_message_schema *selected = nullptr;

	EXPECT_EQ(VMF_DUPLICATE_MESSAGE,
		vmf_schema_set_validate(&set, &limits, nullptr));
	b.id.standard_version = {true, 1};
	EXPECT_EQ(VMF_OK, vmf_schema_set_validate(&set, &limits, nullptr));
	EXPECT_EQ(VMF_OK, vmf_schema_select(&set, &b.id, &selected));
	EXPECT_EQ(&b, selected);
	vmf_message_id missing = b.id;

	missing.standard_version.value = 2;
	EXPECT_EQ(VMF_UNKNOWN_MESSAGE,
		vmf_schema_select(&set, &missing, &selected));
	EXPECT_EQ(nullptr, selected);
}

TEST(Errors, NullInspectionHasDeterministicContext)
{
	vmf_error error = vmf_decoder_error(nullptr);

	EXPECT_EQ(VMF_INVALID_ARGUMENT, error.status);
	EXPECT_EQ(0u, error.offset);
	EXPECT_EQ(0u, error.node_id);
	EXPECT_EQ(SIZE_MAX, error.message_index);
}

TEST(Schema, ScalarWidthsAndAllowedIntervalsHaveBoundaries)
{
	vmf_node root;
	auto schema = empty_schema(&root);
	vmf_node field = {};

	field.id = 2;
	field.kind = VMF_NODE_FIELD;
	field.as.field.kind = VMF_FIELD_UNSIGNED;
	field.as.field.as.scalar.encoding = {
		64, VMF_BIT_LSB_FIRST, VMF_INTEGER_UNSIGNED};
	root.as.group = {&field, 1};
	auto limits = vmf_default_limits();

	EXPECT_EQ(VMF_OK, vmf_schema_validate(&schema, &limits, nullptr));
	field.as.field.as.scalar.encoding.bit_width = 0;
	EXPECT_EQ(VMF_INVALID_SCHEMA,
		vmf_schema_validate(&schema, &limits, nullptr));
	field.as.field.as.scalar.encoding.bit_width = 65;
	EXPECT_EQ(VMF_INVALID_SCHEMA,
		vmf_schema_validate(&schema, &limits, nullptr));
	field.as.field.as.scalar.encoding.bit_width = 8;
	vmf_code_range overlapping[] = {{0, 3}, {3, 4}};

	field.as.field.as.scalar.allowed = overlapping;
	field.as.field.as.scalar.allowed_count = 2;
	EXPECT_EQ(VMF_INVALID_SCHEMA,
		vmf_schema_validate(&schema, &limits, nullptr));
}

TEST(Events, CopyRejectsWrongLayoutWithoutTouchingOutput)
{
	vmf_event event = {};
	vmf_unsigned_field output = {123};

	event.kind = VMF_EVENT_PDU_END;
	EXPECT_EQ(VMF_INVALID_ARGUMENT,
		vmf_event_copy_field(&event, &output, sizeof(output)));
	EXPECT_EQ(123u, output.value);
}

TEST(Allocation, StreamingAndResetDoNotAllocate)
{
	allocation_log log;
	vmf_allocator allocator = {allocate, deallocate, &log};
	vmf_options config = options();
	vmf_decoder *reader = nullptr;
	vmf_encoder *writer = nullptr;

	config.allocator = &allocator;
	ASSERT_EQ(VMF_OK, vmf_decoder_create(&config, &reader, nullptr));
	vmf_status status =
		vmf_encoder_create(&config, false, &writer, nullptr);

	if (status != VMF_OK) {
		vmf_decoder_destroy(reader);
		FAIL() << vmf_status_string(status);
	}
	size_t attempts = log.attempts;

	EXPECT_EQ(VMF_OK, vmf_decoder_begin_envelope(reader));
	vmf_input input = {nullptr, 0, 0, false};
	vmf_event event = {};

	EXPECT_EQ(VMF_NEED_INPUT, vmf_decoder_next(reader, &input, &event));
	EXPECT_EQ(VMF_OK, vmf_encoder_begin_envelope(writer));
	vmf_decoder_reset(reader);
	vmf_encoder_reset(writer);
	EXPECT_EQ(attempts, log.attempts);
	vmf_decoder_destroy(reader);
	vmf_encoder_destroy(writer);
	EXPECT_EQ(0u, log.live);
}

TEST(Allocation, EncoderFailureRollsBack)
{
	allocation_log log;
	vmf_allocator allocator = {allocate, deallocate, &log};
	vmf_options config = options();
	vmf_encoder *writer = nullptr;

	config.allocator = &allocator;
	ASSERT_EQ(VMF_OK, vmf_encoder_create(&config, true, &writer, nullptr));
	size_t allocations = log.attempts;

	vmf_encoder_destroy(writer);
	ASSERT_GT(allocations, 0u);
	for (size_t i = 0; i < allocations; ++i) {
		log.attempts = 0;
		log.fail_at = i;
		writer = nullptr;
		EXPECT_EQ(VMF_OUT_OF_MEMORY,
			vmf_encoder_create(&config, true, &writer, nullptr));
		EXPECT_EQ(nullptr, writer);
		EXPECT_EQ(0u, log.live);
	}
}

TEST(Allocation, SchemaScratchAndCodecStorageRollbackIndependently)
{
	allocation_log log;
	vmf_allocator allocator = {allocate, deallocate, &log};
	vmf_node root;
	auto schema = empty_schema(&root);
	const vmf_message_schema *entry = &schema;
	vmf_schema_set schemas = {&entry, 1};
	vmf_options config = options();
	vmf_encoder *writer = nullptr;

	config.schemas = &schemas;
	config.allocator = &allocator;
	ASSERT_EQ(VMF_OK, vmf_encoder_create(&config, true, &writer, nullptr));
	size_t attempts = log.attempts;

	EXPECT_GE(attempts, 3u);
	vmf_encoder_destroy(writer);
	for (size_t failure = 0; failure < attempts; ++failure) {
		log.attempts = 0;
		log.fail_at = failure;
		writer = nullptr;
		EXPECT_EQ(VMF_OUT_OF_MEMORY,
			vmf_encoder_create(&config, true, &writer, nullptr));
		EXPECT_EQ(nullptr, writer);
		EXPECT_EQ(0u, log.live);
	}
	log.attempts = 0;
	log.fail_at = SIZE_MAX;
	ASSERT_EQ(VMF_OK, vmf_encoder_create(&config, true, &writer, nullptr));
	vmf_encoder_destroy(writer);
	EXPECT_EQ(0u, log.live);
}
