#include <gtest/gtest.h>
#include <vmf/schema_json.hpp>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" int vmf_schema_json_c_consumer(void);

namespace
{

static const char empty_definition[] =
	"{\"format\":\"vmf-schema/1\",\"envelope\":\"C\",\"messages\":[]}";

struct json_owner {
	vmf_schema_json_document *document = nullptr;

	~json_owner()
	{
		vmf_schema_json_destroy(document);
	}
};

static std::string field(unsigned index, unsigned width)
{
	return "{\"index_number\":\"" + std::to_string(index) +
	       ".\",\"number_of_bits\":" + std::to_string(width) + "}";
}

static std::string member(unsigned index, const std::string &properties)
{
	std::string result = field(index, 1);

	result.pop_back();
	return result + properties + "}";
}

static std::string message(const std::string &indexes,
	const std::string &extra = "")
{
	return "{\"message_number\":\"K01.1\",\"indexes\":[" + indexes +
	       "]" + extra + "}";
}

static std::string registry(const std::string &messages,
	const std::string &extra = "")
{
	return "{\"format\":\"vmf-schema/1\",\"envelope\":\"C\","
	       "\"messages\":[" + messages + "]" + extra + "}";
}

static std::string definition(const std::string &indexes)
{
	return registry(message(indexes));
}

static vmf_status load(const std::string &input, json_owner *owner,
	vmf_schema_json_error *error = nullptr,
	const vmf_schema_json_options *configuration = nullptr)
{
	auto options = vmf_schema_json_default_options();

	return vmf_schema_json_load(input.data(), input.size(),
		configuration ? configuration : &options,
		&owner->document, error);
}

TEST(SchemaJson, CAndProceduralCppConsumersLink)
{
	EXPECT_EQ(1, vmf_schema_json_c_consumer());
	vmf::schema_json::options options = vmf::schema_json::default_options();

	EXPECT_EQ(16u * 1024u * 1024u, options.max_json_bytes);
	EXPECT_EQ(64u * 1024u * 1024u, options.max_memory_bytes);
	EXPECT_EQ(nullptr, vmf::schema_json::schemas(nullptr));
	vmf::schema_json::destroy(nullptr);
}

TEST(SchemaJson, EmptySetLoadsAndClearsDiagnostics)
{
	json_owner owner;
	vmf_schema_json_error error = {};

	ASSERT_EQ(VMF_OK, load(empty_definition, &owner, &error));
	ASSERT_NE(nullptr, vmf_schema_json_schemas(owner.document));
	EXPECT_EQ(VMF_OK, error.status);
	EXPECT_STREQ("ok", error.message);
	EXPECT_EQ(SIZE_MAX, error.byte_offset);
	EXPECT_EQ(SIZE_MAX, error.schema_index);
	EXPECT_EQ(SIZE_MAX, error.index_position);
	EXPECT_EQ(VMF_PROPERTY_NONE, error.property);
}

TEST(SchemaJson, ApiMisuseInitializesOutputs)
{
	auto options = vmf_schema_json_default_options();
	vmf_schema_json_document *owner = nullptr;
	vmf_schema_json_error error = {};

	EXPECT_EQ(VMF_INVALID_ARGUMENT, vmf_schema_json_load(nullptr, 0,
		&options, &owner, &error));
	EXPECT_EQ(nullptr, owner);
	EXPECT_EQ(VMF_INVALID_ARGUMENT, error.status);
	EXPECT_EQ(SIZE_MAX, error.schema_index);
	EXPECT_EQ(SIZE_MAX, error.index_position);
	EXPECT_EQ(VMF_INVALID_ARGUMENT, vmf_schema_json_load(empty_definition,
		sizeof(empty_definition) - 1, nullptr, &owner, &error));
	EXPECT_EQ(VMF_INVALID_ARGUMENT, vmf_schema_json_load(empty_definition,
		sizeof(empty_definition) - 1, &options, nullptr, &error));
}

TEST(SchemaJson, SyntaxFailuresHaveByteLocations)
{
	for (const std::string input : {"", " ", "{", "{\"format\":"}) {
		SCOPED_TRACE(input);
		json_owner owner;
		vmf_schema_json_error error = {};

		EXPECT_EQ(VMF_TRUNCATED, load(input, &owner, &error));
		EXPECT_EQ(nullptr, owner.document);
		EXPECT_NE(SIZE_MAX, error.byte_offset);
	}
	const char *malformed[] = {"{}{}", "{,}", "{/*x*/}",
		"{\"format\":\"vmf-schema/1\",}", "[NaN]", "[Infinity]"};

	for (const char *input : malformed) {
		SCOPED_TRACE(input);
		json_owner owner;
		vmf_schema_json_error error = {};

		EXPECT_EQ(VMF_MALFORMED, load(input, &owner, &error));
		EXPECT_EQ(nullptr, owner.document);
		EXPECT_NE(SIZE_MAX, error.byte_offset);
	}
	std::string invalid_utf8 = "{\"format\":\"";

	invalid_utf8.push_back(static_cast<char>(0xff));
	invalid_utf8 += "\"}";
	json_owner owner;

	EXPECT_EQ(VMF_MALFORMED, load(invalid_utf8, &owner));
}

TEST(SchemaJson, RejectsUnrecognizedFormatsExplicitly)
{
	const char *inputs[] = {"{\"schemas\":[]}",
		"{\"format\":\"vmf-schema/99\",\"schemas\":[]}",
		"{\"format\":\"unknown\",\"envelope\":\"C\",\"messages\":[]}"};

	for (const char *input : inputs) {
		SCOPED_TRACE(input);
		json_owner owner;
		vmf_schema_json_error error = {};

		EXPECT_EQ(VMF_UNSUPPORTED_SCHEMA_FEATURE,
			load(input, &owner, &error));
		EXPECT_EQ(nullptr, owner.document);
		EXPECT_STREQ("unsupported schema format", error.message);
	}
}

TEST(SchemaJson, RejectsUnknownDuplicateNullAndWrongTypedProperties)
{
	const std::string inputs[] = {
		"[]", "null", "{}", registry("", ",\"name\":\"display\""),
		registry("", ",\"messages\":[]"),
		registry("", ",\"envel\\u006fpe\":\"C\""),
		"{\"format\":\"old\",\"format\":\"vmf-schema/1\","
			"\"envelope\":\"C\",\"messages\":[]}",
		definition("null"), definition("{}"),
		definition("{\"index_number\":null,\"number_of_bits\":8}"),
		definition("{\"index_number\":\"1.\",\"number_of_bits\":null}"),
		definition("{\"index_number\":\"1.\",\"number_of_bits\":8,"
			"\"number_of_bits\":8}"),
		definition("{\"index_number\":\"1.\",\"number_of_bits\":8,"
			"\"name\":\"display\"}"),
		definition("{\"index_number\":\"1.\",\"number_of_bits\":8,"
			"\"encode_type\":\"unknown\"}"),
		registry(message("", ",\"title\":\"display\"")),
		registry(message("", ",\"subtype\":null"))};

	for (const auto &input : inputs) {
		SCOPED_TRACE(input);
		json_owner owner;

		EXPECT_EQ(VMF_INVALID_SCHEMA, load(input, &owner));
		EXPECT_EQ(nullptr, owner.document);
	}
}

TEST(SchemaJson, RejectsInvalidNumericTokensAndNotation)
{
	const char *numbers[] = {"-1", "1.0", "1e0", "\"8\"", "true",
		"18446744073709551616"};

	for (const char *number : numbers) {
		SCOPED_TRACE(number);
		json_owner owner;

		EXPECT_EQ(VMF_INVALID_SCHEMA, load(definition(
			"{\"index_number\":\"1.\",\"number_of_bits\":" +
			std::string(number) + "}"), &owner));
	}
	const char *indexes[] = {"", ".", "1..2", "-1", "1.x", "1/2",
		"4294967296", "1.2..", "1 ", "1\\u0000"};

	for (const char *index : indexes) {
		SCOPED_TRACE(index);
		json_owner owner;

		EXPECT_EQ(VMF_INVALID_SCHEMA, load(definition(
			"{\"index_number\":\"" + std::string(index) +
			"\",\"number_of_bits\":8}"), &owner));
	}
}

TEST(SchemaJson, AtomicCoreValidationRetainsOwnerIndependentLocations)
{
	json_owner owner;
	vmf_schema_json_error error = {};
	std::string invalid = message(field(7, 0));

	invalid.replace(invalid.find("K01.1"), 5, "K01.2");
	EXPECT_EQ(VMF_INVALID_SCHEMA,
		load(registry(message(field(1, 8)) + "," + invalid),
			&owner, &error));
	EXPECT_EQ(nullptr, owner.document);
	EXPECT_EQ(1u, error.schema_index);
	EXPECT_EQ(0u, error.index_position);
	EXPECT_NE(nullptr, error.message);
	json_owner duplicate;

	EXPECT_EQ(VMF_DUPLICATE_MESSAGE, load(registry(
		message(field(1, 8)) + "," + message(field(1, 8))),
		&duplicate, &error));
	EXPECT_EQ(nullptr, duplicate.document);
}

TEST(SchemaJson, EnforcesInputMemoryAndSchemaLimits)
{
	const std::string input = definition(field(1, 8));
	auto options = vmf_schema_json_default_options();
	json_owner owner;

	options.max_json_bytes = input.size() - 1;
	EXPECT_EQ(VMF_LIMIT_EXCEEDED, load(input, &owner, nullptr, &options));
	options = vmf_schema_json_default_options();
	options.max_memory_bytes = 1;
	EXPECT_EQ(VMF_LIMIT_EXCEEDED, load(input, &owner, nullptr, &options));
	options = vmf_schema_json_default_options();
	options.limits.max_schemas = 0;
	EXPECT_EQ(VMF_LIMIT_EXCEEDED, load(input, &owner, nullptr, &options));
	options = vmf_schema_json_default_options();
	options.limits.max_field_bits = 7;
	EXPECT_EQ(VMF_LIMIT_EXCEEDED, load(input, &owner, nullptr, &options));
}

TEST(SchemaJson, CopiesIndexesKeysAndExactScaleWithoutRetainingInput)
{
	std::string input = definition(
		"{\"index_number\":\"2.3.1\",\"number_of_bits\":64,"
		"\"key\":\"binding\",\"constant\":18446744073709551615,"
		"\"allowed\":[{\"first\":0,\"last\":18446744073709551615}]},"
		"{\"index_number\":\"7.\",\"number_of_bits\":7,"
		"\"encode_type\":\"scaled\",\"representation\":\"unsigned\","
		"\"scale\":{\"factor\":{\"numerator\":1,\"denominator\":10},"
		"\"bias\":{\"numerator\":-2,\"denominator\":3},"
		"\"sentinels\":[127]}}");
	json_owner owner;

	ASSERT_EQ(VMF_OK, load(input, &owner));
	input.assign(input.size(), 'x');
	const auto *set = vmf_schema_json_schemas(owner.document);

	ASSERT_EQ(1u, set->message_count);
	const auto *schema = &set->messages[0];
	const auto *index = vmf_schema_find_key(schema, {"binding", 7});

	ASSERT_NE(nullptr, index);
	ASSERT_EQ(3u, index->index_number.count);
	EXPECT_EQ(2u, index->index_number.parts[0]);
	EXPECT_EQ(3u, index->index_number.parts[1]);
	EXPECT_EQ(1u, index->index_number.parts[2]);
	vmf_index_info info = {};

	ASSERT_EQ(VMF_OK, vmf_schema_index_info(set, schema, index, &info));
	EXPECT_EQ(VMF_FIELD_UNSIGNED, info.kind);
	EXPECT_EQ(VMF_BIT_LSB_FIRST, info.integer.order);
	EXPECT_EQ(UINT64_MAX, info.constant.value);
	ASSERT_EQ(1u, info.allowed_count);
	EXPECT_EQ(UINT64_MAX, info.allowed[0].last);
	ASSERT_EQ(VMF_OK, vmf_schema_index_info(set, schema,
		&schema->indexes[1], &info));
	ASSERT_NE(nullptr, info.scale);
	EXPECT_EQ(1, info.scale->factor.numerator);
	EXPECT_EQ(10u, info.scale->factor.denominator);
	EXPECT_EQ(-2, info.scale->bias.numerator);
	EXPECT_EQ(3u, info.scale->bias.denominator);
	ASSERT_EQ(1u, info.scale->sentinel_count);
	EXPECT_EQ(127u, info.scale->sentinels[0]);
}

TEST(SchemaJson, NamedEditionResolutionAndCompleteOverridesBelongToCore)
{
	for (const char *profile : {"C", "D", "D1"}) {
		SCOPED_TRACE(profile);
		std::string input = registry(message(field(1, 8)),
			",\"message_standard\":\"MIL-STD-6017B\"");
		size_t position = input.find("\"envelope\":\"C\"");

		input.replace(position, 14,
			"\"envelope\":\"" + std::string(profile) + "\"");
		json_owner owner;

		ASSERT_EQ(VMF_OK, load(input, &owner));
		const auto *set = vmf_schema_json_schemas(owner.document);
		vmf_message_id id = {};

		ASSERT_EQ(VMF_OK,
			vmf_schema_message_id(set, &set->messages[0], &id));
		EXPECT_EQ(2u, id.user_message_format);
		EXPECT_TRUE(id.standard_version.present);
		EXPECT_EQ(std::strcmp(profile, "C") ? 7u : 8u,
			id.standard_version.value);
	}
	json_owner owner;

	ASSERT_EQ(VMF_OK, load(registry(message(field(1, 8),
		",\"standard_version\":\"absent\",\"subtype\":0"),
		",\"message_standard\":\"MIL-STD-6017B\""), &owner));
	const auto *set = vmf_schema_json_schemas(owner.document);
	vmf_message_id id = {};

	ASSERT_EQ(VMF_OK,
		vmf_schema_message_id(set, &set->messages[0], &id));
	EXPECT_FALSE(id.standard_version.present);
	EXPECT_TRUE(id.subtype.present);
	EXPECT_EQ(0u, id.subtype.value);
}

TEST(SchemaJson, OptionalZeroIdentityRemainsDistinctFromAbsence)
{
	json_owner owner;

	ASSERT_EQ(VMF_OK, load(registry(message(field(1, 8)) + "," +
		message(field(1, 8), ",\"standard_version\":0") + "," +
		message(field(1, 8), ",\"subtype\":0")), &owner));
	const auto *set = vmf_schema_json_schemas(owner.document);
	vmf_message_id identities[3] = {};

	for (size_t i = 0; i < 3; ++i)
		ASSERT_EQ(VMF_OK, vmf_schema_message_id(set,
			&set->messages[i], &identities[i]));
	EXPECT_FALSE(identities[0].standard_version.present);
	EXPECT_TRUE(identities[1].standard_version.present);
	EXPECT_EQ(0u, identities[1].standard_version.value);
	EXPECT_TRUE(identities[2].subtype.present);
	EXPECT_FALSE(vmf_message_id_equal(&identities[0], &identities[1]));
	EXPECT_FALSE(vmf_message_id_equal(&identities[0], &identities[2]));
}

TEST(SchemaJson, RetainsTypedControlsPathsAndCountDeclarations)
{
	json_owner owner;
	const std::string indexes =
		"{\"index_number\":\"1.\",\"indicator\":\"fpi\","
		"\"target\":\"1.1\"},"
		"{\"index_number\":\"1.1\",\"number_of_bits\":9},"
		"{\"index_number\":\"2.\",\"number_of_bits\":3},"
		"{\"index_number\":\"3.\",\"number_of_bits\":8,"
		"\"repeat_code\":\"R1\"}";

	ASSERT_EQ(VMF_OK, load(registry(message(indexes,
		",\"repeats\":[{\"repeat_code\":\"R1\",\"count_from\":\"2.\","
		"\"maximum\":4,\"minimum\":0,\"bias\":0}]")), &owner));
	const auto *set = vmf_schema_json_schemas(owner.document);
	const auto *schema = &set->messages[0];
	vmf_index_info info = {};

	ASSERT_EQ(VMF_OK, vmf_schema_index_info(set, schema,
		&schema->indexes[0], &info));
	EXPECT_EQ(VMF_INDICATOR_FPI, info.indicator);
	EXPECT_EQ(1u, info.number_of_bits);
	ASSERT_EQ(2u, info.target.as.index.count);
	EXPECT_EQ(1u, info.target.as.index.parts[1]);
	ASSERT_EQ(1u, schema->repeat_count);
	EXPECT_EQ(VMF_REPEAT_FIELD, schema->repeats[0].kind);
	EXPECT_TRUE(schema->repeats[0].minimum.present);
	EXPECT_TRUE(schema->repeats[0].bias.present);
	EXPECT_EQ(0, schema->repeats[0].bias.value);
}

TEST(SchemaJson, FieldLengthsAndConditionsPreserveExplicitOptionalValues)
{
	json_owner owner;

	ASSERT_EQ(VMF_OK, load(definition(field(1, 3) + ","
		"{\"index_number\":\"2.\",\"number_of_bits\":28,"
		"\"encode_type\":\"text\",\"text_encoding\":\"ascii_7\","
		"\"length_from\":\"1.\",\"length_bias\":0,"
		"\"when\":{\"mode\":\"any\",\"negate\":false,\"terms\":[{"
		"\"field\":\"1.\",\"mask\":0,\"negate\":false,"
		"\"values\":[{\"first\":0,\"last\":0}]}]}}"), &owner));
	const auto *set = vmf_schema_json_schemas(owner.document);
	const auto *r = set->messages[0].indexes[1].refinements;

	ASSERT_NE(nullptr, r);
	EXPECT_TRUE(r->length_bias.present);
	EXPECT_EQ(0, r->length_bias.value);
	ASSERT_NE(nullptr, r->when);
	EXPECT_EQ(VMF_CONDITION_ANY, r->when->mode);
	EXPECT_FALSE(r->when->negate);
	ASSERT_EQ(1u, r->when->term_count);
	EXPECT_TRUE(r->when->terms[0].mask.present);
	EXPECT_EQ(0u, r->when->terms[0].mask.value);
}

TEST(SchemaJson, NativeAndJsonSemanticFailuresHaveMatchingLocations)
{
	uint32_t parts[] = {1};
	vmf_index_schema index = {};
	vmf_message_schema schema = {};
	vmf_schema_set set = {};

	index.index_number = {parts, 1};
	schema.message_number = {1, 1};
	schema.indexes = &index;
	schema.index_count = 1;
	set.envelope = VMF_ENVELOPE_C;
	set.messages = &schema;
	set.message_count = 1;
	auto limits = vmf_default_limits();
	vmf_error native = {};
	json_owner owner;
	vmf_schema_json_error json = {};

	EXPECT_EQ(vmf_schema_set_validate(&set, &limits, nullptr, &native),
		load(definition(field(1, 0)), &owner, &json));
	EXPECT_EQ(native.schema_index, json.schema_index);
	EXPECT_EQ(native.index_position, json.index_position);
	EXPECT_EQ(native.property, json.property);
	EXPECT_EQ(native.related_index_position, json.related_index_position);
}

TEST(SchemaJson, EveryEncodingUsesTheNativeResolvedInterpretation)
{
	json_owner owner;
	const char *indexes =
		"{\"index_number\":\"1\",\"number_of_bits\":1,"
		"\"encode_type\":\"bool\"},"
		"{\"index_number\":\"2\",\"number_of_bits\":64},"
		"{\"index_number\":\"3\",\"number_of_bits\":9,"
		"\"encode_type\":\"signed\","
		"\"representation\":\"ones_complement\"},"
		"{\"index_number\":\"4\",\"number_of_bits\":3,"
		"\"encode_type\":\"enum\"},"
		"{\"index_number\":\"5\",\"number_of_bits\":65,"
		"\"encode_type\":\"bits\"},"
		"{\"index_number\":\"6\",\"number_of_bits\":16,"
		"\"encode_type\":\"octets\",\"bit_order\":\"msb_first\"},"
		"{\"index_number\":\"7\",\"number_of_bits\":14,"
		"\"encode_type\":\"text\"},"
		"{\"index_number\":\"8\",\"number_of_bits\":16,"
		"\"encode_type\":\"text\",\"text_encoding\":\"latin_1\"},"
		"{\"index_number\":\"9\",\"number_of_bits\":14,"
		"\"encode_type\":\"literal\"}";

	ASSERT_EQ(VMF_OK, load(definition(indexes), &owner));
	const auto *set = vmf_schema_json_schemas(owner.document);
	const auto *message = &set->messages[0];
	const vmf_field_kind kinds[] = {VMF_FIELD_BOOL, VMF_FIELD_UNSIGNED,
		VMF_FIELD_SIGNED, VMF_FIELD_ENUM, VMF_FIELD_BITS,
		VMF_FIELD_OCTETS, VMF_FIELD_TEXT, VMF_FIELD_TEXT,
		VMF_FIELD_TEXT};

	for (size_t i = 0; i < message->index_count; ++i) {
		vmf_index_info info = {};

		ASSERT_EQ(VMF_OK, vmf_schema_index_info(set, message,
			&message->indexes[i], &info));
		EXPECT_EQ(kinds[i], info.kind);
		if (i == 2)
			EXPECT_EQ(VMF_INTEGER_ONES_COMPLEMENT,
				info.integer.representation);
		if (i == 5)
			EXPECT_EQ(VMF_BIT_MSB_FIRST, info.integer.order);
		if (i == 6)
			EXPECT_EQ(VMF_TEXT_ASCII_7, info.text_encoding);
		if (i == 7)
			EXPECT_EQ(VMF_TEXT_LATIN_1, info.text_encoding);
		if (i == 8)
			EXPECT_TRUE(info.literal);
	}
}

TEST(SchemaJson, ZeroCapacityVariableFieldHasNativeParity)
{
	uint32_t addresses[] = {1, 2};
	vmf_index_schema indexes[2] = {};
	vmf_index_refinements refinements = {};
	vmf_message_schema message = {};
	vmf_schema_set set = {};

	indexes[0].index_number = {addresses, 1};
	indexes[0].number_of_bits = 3;
	indexes[1].index_number = {addresses + 1, 1};
	indexes[1].encode_type = VMF_ENCODE_BITS;
	indexes[1].refinements = &refinements;
	refinements.length_from = indexes[0].index_number;
	message.message_number = {1, 1};
	message.indexes = indexes;
	message.index_count = 2;
	set.envelope = VMF_ENVELOPE_C;
	set.messages = &message;
	set.message_count = 1;
	auto limits = vmf_default_limits();
	json_owner owner;

	ASSERT_EQ(VMF_OK,
		vmf_schema_set_validate(&set, &limits, nullptr, nullptr));
	ASSERT_EQ(VMF_OK, load(definition(field(1, 3) + ","
		"{\"index_number\":\"2\",\"number_of_bits\":0,"
		"\"encode_type\":\"bits\",\"length_from\":\"1\"}"), &owner));
	const auto *loaded = vmf_schema_json_schemas(owner.document);
	vmf_index_info native = {}, json = {};

	ASSERT_EQ(VMF_OK,
		vmf_schema_index_info(&set, &message, &indexes[1], &native));
	ASSERT_EQ(VMF_OK, vmf_schema_index_info(loaded, &loaded->messages[0],
		&loaded->messages[0].indexes[1], &json));
	EXPECT_TRUE(json.variable);
	EXPECT_EQ(native.capacity_units, json.capacity_units);
	EXPECT_EQ(0u, json.capacity_units);
}

TEST(SchemaJson, RejectsMalformedScopeSpellingAndInactiveRepeatMembers)
{
	const char *paths[] = {"R", "R1(", "R1()", "R1(-1)", "R1(2))",
		"R1(18446744073709551616)", "R1/", "R1//R2", "G1"};

	for (const char *path : paths) {
		SCOPED_TRACE(path);
		json_owner owner;

		EXPECT_EQ(VMF_INVALID_SCHEMA, load(definition(
			"{\"index_number\":\"1\",\"number_of_bits\":8,"
			"\"repeat_code\":\"" + std::string(path) + "\"}"),
			&owner));
	}
	const char *declarations[] = {
		"{\"repeat_code\":\"R1\"}",
		"{\"repeat_code\":\"R1\",\"count\":2,\"maximum\":2}",
		"{\"repeat_code\":\"R1\",\"count\":2,\"count_from\":\"1\"}",
		"{\"repeat_code\":\"R1\",\"count_from\":\"1\"}",
		"{\"repeat_code\":\"R1(2)\",\"count\":2}"};

	for (const char *declaration : declarations) {
		SCOPED_TRACE(declaration);
		json_owner owner;

		EXPECT_EQ(VMF_INVALID_SCHEMA, load(registry(message(
			field(1, 8), ",\"repeats\":[" +
			std::string(declaration) + "]")), &owner));
	}
}

TEST(SchemaJson, NativeAndJsonScopeTopologyHaveIdenticalValidation)
{
	for (unsigned example = 0; example < 5; ++example) {
		SCOPED_TRACE(example);
		uint32_t numbers[] = {1, 2, 3};
		vmf_group_id groups[] = {1, 2};
		vmf_repeat_component memberships[3] = {};
		vmf_index_schema indexes[3] = {};
		vmf_repeat_definition repeat = {};
		vmf_message_schema message = {};
		vmf_schema_set set = {};
		std::string json;

		for (size_t i = 0; i < 3; ++i) {
			indexes[i].index_number = {numbers + i, 1};
			indexes[i].number_of_bits = 1;
			memberships[i].id = 1;
		}
		message.message_number = {1, 1};
		message.indexes = indexes;
		message.index_count = example < 2 ? 3 : 2;
		set.envelope = VMF_ENVELOPE_C;
		set.messages = &message;
		set.message_count = 1;
		if (example == 0 || example == 2) {
			repeat.id = 1;
			repeat.kind = VMF_REPEAT_FIXED;
			repeat.count = 2;
			message.repeats = &repeat;
			message.repeat_count = 1;
			indexes[0].group_code = {groups, 1};
			indexes[1].group_code = {groups, 1};
			indexes[1].repeat_code = {memberships + 1, 1};
			if (example == 0) {
				indexes[2].repeat_code = {memberships + 2, 1};
				json = member(1,
					",\"group_code\":\"G1\"") + "," +
					member(2, ",\"group_code\":\"G1\","
					"\"repeat_code\":\"R1\"") + "," +
					member(3, ",\"repeat_code\":\"R1\"");
			} else {
				indexes[0].repeat_code = {memberships, 1};
				json = member(1, ",\"group_code\":\"G1\","
					"\"repeat_code\":\"R1\"") + "," +
					member(2, ",\"group_code\":\"G1\","
					"\"repeat_code\":\"R1\"");
			}
			json = registry(::message(json, ",\"repeats\":[{"
				"\"repeat_code\":\"R1\",\"count\":2}]"));
		} else if (example == 1) {
			indexes[0].group_code = {groups, 1};
			indexes[2].group_code = {groups, 1};
			json = definition(
				"{\"index_number\":\"1\",\"number_of_bits\":1,"
				"\"group_code\":\"G1\"},"
				"{\"index_number\":\"2\",\"number_of_bits\":1},"
				"{\"index_number\":\"3\",\"number_of_bits\":1,"
				"\"group_code\":\"G1\"}");
		} else {
			indexes[0].group_code = {groups, 2};
			indexes[1].group_code = example == 3 ?
				vmf_group_path{groups + 1, 1} :
				vmf_group_path{groups, 2};
			json = definition(
				"{\"index_number\":\"1\",\"number_of_bits\":1,"
				"\"group_code\":\"G1/G2\"},"
				"{\"index_number\":\"2\",\"number_of_bits\":1,"
				"\"group_code\":\"" +
				std::string(example == 3 ? "G2" : "G1/G2") +
				"\"}");
		}
		auto limits = vmf_default_limits();
		vmf_error native = {};
		vmf_schema_json_error loaded = {};
		json_owner owner;
		vmf_status expected = example == 4 ?
			VMF_OK : VMF_INVALID_SCHEMA;

		EXPECT_EQ(expected, vmf_schema_set_validate(&set, &limits,
			nullptr, &native));
		EXPECT_EQ(expected, load(json, &owner, &loaded));
		EXPECT_EQ(native.schema_index, loaded.schema_index);
		EXPECT_EQ(native.index_position, loaded.index_position);
		EXPECT_EQ(native.property, loaded.property);
		EXPECT_EQ(native.related_index_position,
			loaded.related_index_position);
		EXPECT_EQ(native.related_scope_id, loaded.related_scope_id);
	}
}

TEST(SchemaJson, InactiveRefinementsAreRejectedByNativeValidation)
{
	const char *indexes[] = {
		"{\"index_number\":\"1\",\"number_of_bits\":8,"
			"\"encode_type\":\"bits\",\"bit_order\":\"msb_first\"}",
		"{\"index_number\":\"1\",\"number_of_bits\":8,"
			"\"encode_type\":\"bits\",\"bit_order\":\"lsb_first\"}",
		"{\"index_number\":\"1\",\"number_of_bits\":8,"
			"\"length_bias\":0}",
		"{\"index_number\":\"1\",\"number_of_bits\":8,"
			"\"text_encoding\":\"ascii_8\"}",
		"{\"index_number\":\"1\",\"number_of_bits\":8,"
			"\"encode_type\":\"signed\"}",
		"{\"index_number\":\"1\",\"number_of_bits\":8,"
			"\"indicator\":\"fpi\",\"target\":\"2\"},"
			"{\"index_number\":\"2\",\"number_of_bits\":8}"};

	for (const char *index : indexes) {
		SCOPED_TRACE(index);
		json_owner owner;
		vmf_schema_json_error error = {};

		EXPECT_EQ(VMF_INVALID_SCHEMA, load(definition(index),
			&owner, &error));
		EXPECT_EQ(0u, error.schema_index);
		EXPECT_EQ(0u, error.index_position);
		EXPECT_EQ(VMF_PROPERTY_ENCODING, error.property);
	}
}

struct json_allocation_log {
	size_t attempts = 0;
	size_t live = 0;
	size_t fail_at = SIZE_MAX;
};

static void *json_allocate(void *user, size_t size)
{
	auto *log = static_cast<json_allocation_log *>(user);

	if (log->attempts++ == log->fail_at)
		return nullptr;
	void *memory = std::malloc(size);

	if (memory)
		++log->live;
	return memory;
}

static void json_deallocate(void *user, void *memory)
{
	auto *log = static_cast<json_allocation_log *>(user);

	EXPECT_NE(nullptr, memory);
	EXPECT_GT(log->live, 0u);
	--log->live;
	std::free(memory);
}

TEST(SchemaJson, EveryAllocatorFailureRollsBackAllOwnedAndScratchStorage)
{
	const std::string input = registry(message(
		"{\"index_number\":\"1\",\"number_of_bits\":3,"
		"\"key\":\"count\",\"constant\":2,"
		"\"allowed\":[{\"first\":0,\"last\":4}]},"
		"{\"index_number\":\"2\",\"number_of_bits\":28,"
		"\"encode_type\":\"text\",\"length_from\":\"1\","
		"\"length_bias\":0,\"when\":{\"mode\":\"any\",\"terms\":[{"
		"\"field\":\"1\",\"mask\":0,\"negate\":false,"
		"\"values\":[{\"first\":0,\"last\":0}]}]}},"
		"{\"index_number\":\"3\",\"number_of_bits\":7,"
		"\"encode_type\":\"scaled\",\"scale\":{"
		"\"factor\":{\"numerator\":1,\"denominator\":10},"
		"\"bias\":{\"numerator\":0,\"denominator\":1},"
		"\"sentinels\":[127]}},"
		"{\"index_number\":\"4\",\"indicator\":\"gpi\","
		"\"target\":\"G1\"},"
		"{\"index_number\":\"4.1\",\"number_of_bits\":8,"
		"\"group_code\":\"G1\"},"
		"{\"index_number\":\"5\",\"indicator\":\"fri\","
		"\"repeat_code\":\"R1(2)\"},"
		"{\"index_number\":\"5.1\",\"number_of_bits\":3,"
		"\"repeat_code\":\"R1\"},"
		"{\"index_number\":\"6\",\"number_of_bits\":8,"
		"\"repeat_code\":\"R2\"}",
		",\"repeats\":[{\"repeat_code\":\"R2\",\"count\":1}]"));
	json_allocation_log log;
	vmf_allocator allocator = {json_allocate, json_deallocate, &log};
	auto options = vmf_schema_json_default_options();

	options.allocator = &allocator;
	{
		json_owner owner;

		ASSERT_EQ(VMF_OK, load(input, &owner, nullptr, &options));
		EXPECT_GT(log.live, 0u);
	}
	EXPECT_EQ(0u, log.live);
	size_t total = log.attempts;

	ASSERT_GT(total, 0u);
	for (size_t failure = 0; failure < total; ++failure) {
		SCOPED_TRACE(failure);
		log = {};
		log.fail_at = failure;
		json_owner owner;
		vmf_schema_json_error error = {};

		EXPECT_EQ(VMF_OUT_OF_MEMORY,
			load(input, &owner, &error, &options));
		EXPECT_EQ(VMF_OUT_OF_MEMORY, error.status);
		EXPECT_EQ(nullptr, owner.document);
		EXPECT_EQ(0u, log.live);
	}
	log = {};
	json_owner recovered;

	ASSERT_EQ(VMF_OK, load(input, &recovered, nullptr, &options));
	/* The owner keeps its allocator after its configuration is changed. */
	allocator = {};
}

TEST(SchemaJson, InvalidDefinitionsAlsoReleaseEveryCustomAllocation)
{
	json_allocation_log log;
	vmf_allocator allocator = {json_allocate, json_deallocate, &log};
	auto options = vmf_schema_json_default_options();

	options.allocator = &allocator;
	json_owner owner;

	EXPECT_EQ(VMF_INVALID_SCHEMA, load(definition(field(1, 0)),
		&owner, nullptr, &options));
	EXPECT_EQ(0u, log.live);
	EXPECT_EQ(nullptr, owner.document);
	allocator.deallocate = nullptr;
	EXPECT_EQ(VMF_INVALID_ARGUMENT,
		load(empty_definition, &owner, nullptr, &options));
	EXPECT_EQ(0u, log.live);
}

} /* namespace */

namespace
{

struct allocation_size {
	void *memory;
	size_t size;
};

struct json_memory_log {
	std::vector<allocation_size> allocations;
	size_t live = 0;
	size_t peak = 0;
};

static void *json_memory_allocate(void *user, size_t size)
{
	auto *log = static_cast<json_memory_log *>(user);
	void *memory = std::malloc(size);

	if (memory) {
		log->allocations.push_back({memory, size});
		log->live += size;
		if (log->live > log->peak)
			log->peak = log->live;
	}
	return memory;
}

static void json_memory_deallocate(void *user, void *memory)
{
	auto *log = static_cast<json_memory_log *>(user);

	for (auto &allocation : log->allocations) {
		if (allocation.memory != memory)
			continue;
		log->live -= allocation.size;
		allocation.memory = nullptr;
		std::free(memory);
		return;
	}
	ADD_FAILURE() << "Deallocation did not match a live allocation";
}

TEST(SchemaJson, LiveMemoryBudgetIncludesAllCustomAllocatorRequests)
{
	std::string children;

	for (unsigned i = 1; i < 100; ++i) {
		if (!children.empty())
			children += ",";
		children += field(i, 8);
	}
	std::string input = definition(children);
	json_memory_log log;
	vmf_allocator allocator = {
		json_memory_allocate, json_memory_deallocate, &log};
	auto options = vmf_schema_json_default_options();

	options.allocator = &allocator;
	{
		json_owner owner;

		ASSERT_EQ(VMF_OK, load(input, &owner, nullptr, &options));
		EXPECT_GT(log.live, 0u);
	}
	ASSERT_EQ(0u, log.live);
	size_t required = log.peak;

	ASSERT_GT(required, 1u);
	log = {};
	options.max_memory_bytes = required;
	{
		json_owner owner;

		ASSERT_EQ(VMF_OK, load(input, &owner, nullptr, &options));
		EXPECT_LE(log.peak, required);
	}
	EXPECT_EQ(0u, log.live);
	log = {};
	options.max_memory_bytes = required - 1;
	json_owner limited;

	EXPECT_EQ(VMF_LIMIT_EXCEEDED,
		load(input, &limited, nullptr, &options));
	EXPECT_EQ(nullptr, limited.document);
	EXPECT_LE(log.peak, options.max_memory_bytes);
	EXPECT_EQ(0u, log.live);
}

struct json_misaligned_log {
	void *base = nullptr;
	size_t live = 0;
};

static void *json_misaligned_allocate(void *user, size_t size)
{
	auto *log = static_cast<json_misaligned_log *>(user);

	log->base = std::malloc(size + alignof(std::max_align_t));
	if (!log->base)
		return nullptr;
	++log->live;
	return static_cast<uint8_t *>(log->base) + 1;
}

static void json_misaligned_deallocate(void *user, void *memory)
{
	auto *log = static_cast<json_misaligned_log *>(user);

	EXPECT_EQ(static_cast<uint8_t *>(log->base) + 1, memory);
	--log->live;
	std::free(log->base);
	log->base = nullptr;
}

TEST(SchemaJson, RejectsMisalignedAllocatorStorageBeforeUsingIt)
{
	json_misaligned_log log;
	vmf_allocator allocator = {
		json_misaligned_allocate, json_misaligned_deallocate, &log};
	auto options = vmf_schema_json_default_options();

	options.allocator = &allocator;
	json_owner owner;

	EXPECT_EQ(VMF_INVALID_ARGUMENT,
		load(empty_definition, &owner, nullptr, &options));
	EXPECT_EQ(nullptr, owner.document);
	EXPECT_EQ(0u, log.live);
	EXPECT_EQ(nullptr, log.base);
}

} /* namespace */
