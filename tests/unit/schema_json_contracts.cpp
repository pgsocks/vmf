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
	return "{\"key\":\"" + std::to_string(index) +
	       ".\",\"number_of_bits\":" + std::to_string(width) + "}";
}

static std::string message(const std::string &indexes,
	const std::string &extra = "")
{
	return "{\"message_number\":\"K01.1\",\"elements\":[" + indexes +
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

TEST(SchemaJson, NestedAuthoringGeneratesControlsAndResolvesNamedReferences)
{
	const std::string input = "{\"format\":\"vmf-schema/1\","
		"\"envelope\":\"C\",\"messages\":[{\"message_number\":\"K01.1\","
		"\"elements\":[{\"key\":\"count\",\"number_of_bits\":3},"
		"{\"optional\":{\"key\":\"enabled\",\"type\":\"bool\"}},"
		"{\"recurrence\":{\"maximum\":4,\"body\":[{\"key\":\"x\","
		"\"number_of_bits\":7},{\"key\":\"y\",\"number_of_bits\":9}]}},"
		"{\"repeat\":{\"count_from\":\"count\",\"maximum\":7,"
		"\"body\":{\"key\":\"tag\",\"number_of_bits\":8}}}]}]}";
	json_owner owner;
	vmf_schema_json_error error = {};

	ASSERT_EQ(VMF_OK, load(input, &owner, &error));
	const auto *set = vmf_schema_json_schemas(owner.document);
	const auto *schema = &set->messages[0];

	ASSERT_EQ(7u, schema->index_count);
	EXPECT_EQ(VMF_INDICATOR_FPI, schema->indexes[1].indicator);
	EXPECT_EQ(VMF_INDICATOR_GRI, schema->indexes[3].indicator);
	EXPECT_EQ(1u, schema->indexes[2].number_of_bits);
	ASSERT_EQ(1u, schema->repeat_count);
	EXPECT_EQ(VMF_REPEAT_FIELD, schema->repeats[0].kind);
	ASSERT_EQ(1u, schema->repeats[0].count_from.count);
	EXPECT_EQ(1u, schema->repeats[0].count_from.parts[0]);
	EXPECT_STREQ("", error.path);
	EXPECT_STREQ("", error.related_path);
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
	EXPECT_STREQ("", error.path);
	EXPECT_FALSE(error.path_truncated);
	EXPECT_STREQ("", error.related_path);
	EXPECT_FALSE(error.related_path_truncated);
}

TEST(SchemaJson, OnePortableDefinitionBindsToEveryEnvelope)
{
	const std::string input = "{\"format\":\"vmf-schema/1\","
		"\"message_standard\":\"MIL-STD-6017B\",\"messages\":[" +
		message(field(1, 8)) + "]}";
	json_owner owner;

	ASSERT_EQ(VMF_OK, load(input, &owner));
	const auto *set = vmf_schema_json_schemas(owner.document);

	ASSERT_NE(nullptr, set);
	EXPECT_EQ(0, set->envelope);
	EXPECT_EQ(VMF_STANDARD_EDITION, set->standard.kind);
	EXPECT_EQ(VMF_STANDARD_6017B, set->standard.as.edition);
	for (auto profile : {VMF_ENVELOPE_B, VMF_ENVELOPE_C,
		     VMF_ENVELOPE_D, VMF_ENVELOPE_D1}) {
		SCOPED_TRACE(profile);
		vmf_options options = {};

		options.profile = profile;
		options.limits = vmf_default_limits();
		options.schemas = set;
		vmf_decoder *decoder = nullptr;

		EXPECT_EQ(VMF_OK,
			vmf_decoder_create(&options, &decoder, nullptr));
		vmf_decoder_destroy(decoder);
	}
	EXPECT_EQ(0, set->envelope);
	EXPECT_EQ(VMF_STANDARD_6017B, set->standard.as.edition);
}

TEST(SchemaJson, PortableValidationRetainsEditionsAcrossProfileFamilies)
{
	const std::string input = "{\"format\":\"vmf-schema/1\","
		"\"messages\":[" + message(field(1, 8),
		",\"message_standard\":\"MIL-STD-6017D\"") + "," +
		message(field(1, 8),
			",\"message_standard\":\"TIDP-TE-R7\"") + "]}";
	json_owner owner;

	ASSERT_EQ(VMF_OK, load(input, &owner));
	const auto *set = vmf_schema_json_schemas(owner.document);

	ASSERT_EQ(2u, set->message_count);
	EXPECT_EQ(VMF_STANDARD_6017D, set->messages[0].standard.as.edition);
	EXPECT_EQ(VMF_STANDARD_TIDP_TE_R7,
		set->messages[1].standard.as.edition);
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
		definition("{\"key\":null,\"number_of_bits\":8}"),
		definition("{\"key\":\"1.\",\"number_of_bits\":null}"),
		definition("{\"key\":\"1.\",\"number_of_bits\":8,"
			"\"number_of_bits\":8}"),
		definition("{\"key\":\"1.\",\"number_of_bits\":8,"
			"\"name\":\"display\"}"),
		definition("{\"key\":\"1.\",\"number_of_bits\":8,"
			"\"type\":\"unknown\"}"),
		registry(message("", ",\"title\":\"display\"")),
		registry(message("", ",\"subtype\":null"))};

	for (const auto &input : inputs) {
		SCOPED_TRACE(input);
		json_owner owner;

		EXPECT_EQ(VMF_INVALID_SCHEMA, load(input, &owner));
		EXPECT_EQ(nullptr, owner.document);
	}
}

TEST(SchemaJson, RejectsInvalidNumericTokensAndFieldKeys)
{
	const char *numbers[] = {"-1", "1.0", "1e0", "\"8\"", "true",
		"18446744073709551616"};

	for (const char *number : numbers) {
		SCOPED_TRACE(number);
		json_owner owner;

		EXPECT_EQ(VMF_INVALID_SCHEMA, load(definition(
			"{\"key\":\"value\",\"number_of_bits\":" +
			std::string(number) + "}"), &owner));
	}
	const char *keys[] = {"", "bad\\u0000key"};

	for (const char *key : keys) {
		SCOPED_TRACE(key);
		json_owner owner;

		EXPECT_EQ(VMF_INVALID_SCHEMA, load(definition(
			"{\"key\":\"" + std::string(key) +
			"\",\"number_of_bits\":8}"), &owner));
	}
	json_owner missing;

	EXPECT_EQ(VMF_INVALID_SCHEMA,
		load(definition("{\"number_of_bits\":8}"), &missing));
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
	std::string input = definition(R"([
		[{"key":"binding","number_of_bits":64,
		  "constant":18446744073709551615,
		  "allowed":[{"first":0,"last":18446744073709551615}]}]
	],{"key":"scaled","number_of_bits":7,
	   "scale":{"factor":{"numerator":1,"denominator":10},
	     "bias":{"numerator":-2,"denominator":3},"sentinels":[127]}})");
	json_owner owner;

	ASSERT_EQ(VMF_OK, load(input, &owner));
	input.assign(input.size(), 'x');
	const auto *set = vmf_schema_json_schemas(owner.document);

	ASSERT_EQ(1u, set->message_count);
	const auto *schema = &set->messages[0];
	const auto *index = vmf_schema_find_key(schema, {"binding", 7});

	ASSERT_NE(nullptr, index);
	ASSERT_EQ(3u, index->index_number.count);
	EXPECT_EQ(1u, index->index_number.parts[0]);
	EXPECT_EQ(1u, index->index_number.parts[1]);
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
	EXPECT_EQ(VMF_FIELD_SCALED, info.kind);
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
	const std::string elements = R"(
		{"optional":{"key":"optional","number_of_bits":9}},
		{"key":"count","number_of_bits":3},
		{"repeat":{"count_from":"count","maximum":4,
		  "minimum":0,"bias":0,
		  "body":{"key":"value","number_of_bits":8}}})";

	ASSERT_EQ(VMF_OK, load(definition(elements), &owner));
	const auto *set = vmf_schema_json_schemas(owner.document);
	const auto *schema = &set->messages[0];
	vmf_index_info info = {};

	ASSERT_EQ(4u, schema->index_count);
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
	ASSERT_EQ(1u, schema->repeats[0].count_from.count);
	EXPECT_EQ(2u, schema->repeats[0].count_from.parts[0]);
	ASSERT_EQ(1u, schema->indexes[3].repeat_code.count);
	EXPECT_EQ(1u, schema->indexes[3].repeat_code.parts[0].id);
}

TEST(SchemaJson, FieldLengthsAndConditionsPreserveExplicitOptionalValues)
{
	json_owner owner;
	const std::string elements = R"(
		{"key":"length","number_of_bits":3},
		{"key":"text","number_of_bits":28,"type":"text",
		 "text_encoding":"ascii_7","length_from":"length",
		 "length_bias":0,"when":{"any":[{"field":"length",
		   "mask":0,"negate":false,"values":[{"first":0,"last":0}]}],
		   "negate":false}})";

	ASSERT_EQ(VMF_OK, load(definition(elements), &owner));
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
	ASSERT_EQ(1u, r->length_from.count);
	EXPECT_EQ(1u, r->length_from.parts[0]);
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
	const char *elements = R"(
		{"key":"flag","type":"bool"},
		{"key":"raw","number_of_bits":64},
		{"key":"signed","number_of_bits":9,"type":"ones_complement"},
		{"key":"enum","number_of_bits":3,"type":"enum"},
		{"key":"bits","number_of_bits":65,"type":"bits"},
		{"key":"octets","number_of_bits":16,"type":"octets",
		 "bit_order":"msb_first"},
		{"key":"ascii","number_of_bits":14,"type":"text"},
		{"key":"latin","number_of_bits":16,"type":"text",
		 "text_encoding":"latin_1"},
		{"key":"literal","number_of_bits":14,"type":"literal"})";

	ASSERT_EQ(VMF_OK, load(definition(elements), &owner));
	const auto *set = vmf_schema_json_schemas(owner.document);
	const auto *message = &set->messages[0];
	const vmf_field_kind kinds[] = {VMF_FIELD_BOOL, VMF_FIELD_UNSIGNED,
		VMF_FIELD_SIGNED, VMF_FIELD_ENUM, VMF_FIELD_BITS,
		VMF_FIELD_OCTETS, VMF_FIELD_TEXT, VMF_FIELD_TEXT,
		VMF_FIELD_TEXT};

	ASSERT_EQ(9u, message->index_count);
	for (size_t i = 0; i < message->index_count; ++i) {
		vmf_index_info info = {};

		ASSERT_EQ(VMF_OK, vmf_schema_index_info(set, message,
			&message->indexes[i], &info));
		EXPECT_EQ(kinds[i], info.kind);
		if (i == 0)
			EXPECT_EQ(1u, info.number_of_bits);
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
		"{\"key\":\"2\",\"number_of_bits\":0,"
		"\"type\":\"bits\",\"length_from\":\"1.\"}"), &owner));
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

TEST(SchemaJson, RejectsRetiredPropertiesAndMalformedWrappers)
{
	const char *properties[] = {
		R"("index_number":"1")", R"("group_code":"G1")",
		R"("repeat_code":"R1")", R"("indicator":"fpi")",
		R"("target":"value")", R"("encode_type":"unsigned")",
		R"("representation":"twos_complement")"};

	for (const char *property : properties) {
		SCOPED_TRACE(property);
		json_owner owner;

		EXPECT_EQ(VMF_INVALID_SCHEMA, load(definition(
			"{\"key\":\"value\",\"number_of_bits\":8," +
			std::string(property) + "}"), &owner));
	}
	const char *elements[] = {
		"[]", R"({"optional":[]})", R"({"optional":null})",
		R"({"optional":{"key":"v","number_of_bits":8},"key":"x"})",
		R"({"repeat":{"count":2,"body":[]}})",
		R"({"repeat":{"maximum":2,"body":{"key":"v",
			"number_of_bits":8}}})",
		R"({"repeat":{"count":2,"maximum":2,"body":{"key":"v",
			"number_of_bits":8}}})",
		R"({"repeat":{"count":2,"count_from":"v","body":{
			"key":"v","number_of_bits":8}}})",
		R"({"repeat":{"count_from":"v","body":{
			"key":"v","number_of_bits":8}}})",
		R"({"recurrence":{"maximum":0,"body":{
			"key":"v","number_of_bits":8}}})",
		R"({"recurrence":{"count":2,"maximum":2,"body":{
			"key":"v","number_of_bits":8}}})",
		R"({"recurrence":{"maximum":2}})",
		R"({"key":"v","type":"bool","number_of_bits":2})",
		R"({"key":"v","type":"signed","number_of_bits":8})",
		R"({"key":"v","type":"scaled","number_of_bits":8})",
		R"({"key":"v","number_of_bits":8,"when":{"all":[],
			"any":[]}})",
		R"({"key":"v","number_of_bits":8,"when":{"all":[]}})"};

	for (const char *element : elements) {
		SCOPED_TRACE(element);
		json_owner owner;

		EXPECT_EQ(VMF_INVALID_SCHEMA,
			load(definition(element), &owner));
		EXPECT_EQ(nullptr, owner.document);
	}
	json_owner old;

	EXPECT_EQ(VMF_INVALID_SCHEMA, load(registry(
		R"({"message_number":"K01.1","indexes":[]})"), &old));
}

TEST(SchemaJson, WrapperOrderAndSingleFieldGroupsDetermineControllers)
{
	const char *elements[] = {
		R"({"optional":{"recurrence":{"maximum":2,"body":[
			{"key":"v","number_of_bits":3}]}}})",
		R"({"recurrence":{"maximum":2,"body":{"optional":[
			{"key":"v","number_of_bits":3}]}}})"};

	for (unsigned i = 0; i < 2; ++i) {
		SCOPED_TRACE(i);
		json_owner owner;

		ASSERT_EQ(VMF_OK, load(definition(elements[i]), &owner));
		const auto *set = vmf_schema_json_schemas(owner.document);
		const auto *schema = &set->messages[0];

		ASSERT_EQ(3u, schema->index_count);
		EXPECT_EQ(i ? VMF_INDICATOR_GRI : VMF_INDICATOR_GPI,
			schema->indexes[0].indicator);
		EXPECT_EQ(i ? VMF_INDICATOR_GPI : VMF_INDICATOR_GRI,
			schema->indexes[1].indicator);
		ASSERT_EQ(3u, schema->indexes[2].index_number.count);
		EXPECT_EQ(1u, schema->indexes[2].index_number.parts[2]);
		ASSERT_EQ(1u, schema->indexes[2].group_code.count);
		ASSERT_EQ(1u, schema->indexes[2].repeat_code.count);
		EXPECT_EQ(1u, schema->indexes[2].group_code.parts[0]);
		EXPECT_EQ(1u, schema->indexes[2].repeat_code.parts[0].id);
		EXPECT_EQ(i ? 1u : 0u, schema->indexes[0].repeat_code.count);
		EXPECT_EQ(i ? 0u : 1u, schema->indexes[1].group_code.count);
	}
}

TEST(SchemaJson, PlainGroupsAreTransparentAndRepeatNestingIsUnambiguous)
{
	json_owner owner;
	const char *elements = R"([
		{"repeat":{"count":2,"body":[
			{"repeat":{"count":3,"body":
				{"key":"v","number_of_bits":8}}}]}},
		[{"key":"w","number_of_bits":4}]
	])";

	ASSERT_EQ(VMF_OK, load(definition(elements), &owner));
	const auto *schema = &vmf_schema_json_schemas(owner.document)->messages[0];

	ASSERT_EQ(2u, schema->index_count);
	ASSERT_EQ(2u, schema->repeat_count);
	EXPECT_EQ(0u, schema->indexes[0].group_code.count);
	EXPECT_EQ(0u, schema->indexes[1].group_code.count);
	ASSERT_EQ(2u, schema->indexes[0].repeat_code.count);
	EXPECT_EQ(1u, schema->indexes[0].repeat_code.parts[0].id);
	EXPECT_EQ(2u, schema->indexes[0].repeat_code.parts[1].id);
	EXPECT_EQ(2u, schema->repeats[0].count);
	EXPECT_EQ(3u, schema->repeats[1].count);
}

TEST(SchemaJson, CountedRepeatsCanContainAnOptionalSingleFieldGroup)
{
	for (const char *count : {R"("count":2)",
	     R"("count_from":"n","maximum":3)"}) {
		SCOPED_TRACE(count);
		json_owner owner;
		std::string elements = R"({"key":"n","number_of_bits":2},
			{"repeat":{)" + std::string(count) + R"(,"body":
			{"optional":[{"key":"v","number_of_bits":8}]}}})";

		ASSERT_EQ(VMF_OK, load(definition(elements), &owner));
		const auto *schema =
			&vmf_schema_json_schemas(owner.document)->messages[0];

		ASSERT_EQ(3u, schema->index_count);
		EXPECT_EQ(VMF_INDICATOR_GPI, schema->indexes[1].indicator);
		ASSERT_EQ(1u, schema->indexes[1].repeat_code.count);
		ASSERT_EQ(1u, schema->indexes[2].group_code.count);
		ASSERT_EQ(1u, schema->indexes[2].repeat_code.count);
	}
}

TEST(SchemaJson, InactiveRefinementsAreRejectedByNativeValidation)
{
	const char *elements[] = {
		R"({"key":"v","number_of_bits":8,
			"type":"bits","bit_order":"msb_first"})",
		R"({"key":"v","number_of_bits":8,
			"type":"bits","bit_order":"lsb_first"})",
		R"({"key":"v","number_of_bits":8,"length_bias":0})",
		R"({"key":"v","number_of_bits":8,"text_encoding":"ascii_8"})"};

	for (const char *element : elements) {
		SCOPED_TRACE(element);
		json_owner owner;
		vmf_schema_json_error error = {};

		EXPECT_EQ(VMF_INVALID_SCHEMA, load(definition(element),
			&owner, &error));
		EXPECT_EQ(0u, error.schema_index);
		EXPECT_EQ(0u, error.index_position);
		EXPECT_EQ(VMF_PROPERTY_ENCODING, error.property);
		EXPECT_EQ(0, std::strncmp("/messages/0/elements/0",
			error.path, std::strlen("/messages/0/elements/0")));
	}
}

TEST(SchemaJson, FieldKeysAreMessageLocalAndReferencesNeverParseAddresses)
{
	const std::string elements = R"(
		{"key":"99.1","number_of_bits":3},
		{"key":"value","type":"bits","number_of_bits":7,
		 "length_from":"99.1"})";
	std::string second = message(elements);

	second.replace(second.find("K01.1"), 5, "K01.2");
	json_owner owner;

	ASSERT_EQ(VMF_OK, load(registry(message(elements) + "," + second),
		&owner));
	const auto *set = vmf_schema_json_schemas(owner.document);

	for (size_t i = 0; i < 2; ++i) {
		const auto *r = set->messages[i].indexes[1].refinements;

		ASSERT_NE(nullptr, r);
		ASSERT_EQ(1u, r->length_from.count);
		EXPECT_EQ(1u, r->length_from.parts[0]);
	}
	json_owner invalid;
	vmf_schema_json_error error = {};
	std::string unknown = elements;

	unknown.replace(unknown.find("\"length_from\":\"99.1\""),
		std::strlen("\"length_from\":\"99.1\""),
		"\"length_from\":\"1\"");
	EXPECT_EQ(VMF_INVALID_SCHEMA,
		load(definition(unknown), &invalid, &error));
	EXPECT_STREQ("unknown field reference", error.message);
	EXPECT_STREQ("/messages/0/elements/1/length_from", error.path);
}

TEST(SchemaJson, DuplicateKeysHaveCopiedPrimaryAndRelatedSourcePaths)
{
	json_owner owner;
	vmf_schema_json_error error = {};
	std::string input = definition(R"(
		{"key":"same","number_of_bits":3},
		{"optional":{"key":"same","number_of_bits":4}})");

	EXPECT_EQ(VMF_INVALID_SCHEMA, load(input, &owner, &error));
	input.assign(input.size(), 'x');
	EXPECT_EQ(nullptr, owner.document);
	EXPECT_STREQ("/messages/0/elements/1/optional/key", error.path);
	EXPECT_STREQ("/messages/0/elements/0/key", error.related_path);
	EXPECT_FALSE(error.path_truncated);
	EXPECT_FALSE(error.related_path_truncated);
}

TEST(SchemaJson, ReferencesCannotCrossMessagesAndUnknownSourcesAreLocated)
{
	std::string second = message(R"({"key":"text","type":"bits",
		"number_of_bits":8,"length_from":"other"})");

	second.replace(second.find("K01.1"), 5, "K01.2");
	json_owner cross;
	vmf_schema_json_error error = {};

	EXPECT_EQ(VMF_INVALID_SCHEMA, load(registry(message(
		R"({"key":"other","number_of_bits":3})") + "," + second),
		&cross, &error));
	EXPECT_STREQ("/messages/1/elements/0/length_from", error.path);
	const char *elements[] = {
		R"({"repeat":{"count_from":"missing","maximum":3,
		 "body":{"key":"v","number_of_bits":8}}})",
		R"({"key":"v","number_of_bits":8,
		 "when":{"all":[{"field":"missing",
			"values":[{"first":0,"last":0}]}]}})"};
	const char *paths[] = {
		"/messages/0/elements/0/repeat/count_from",
		"/messages/0/elements/0/when/all/0/field"};

	for (size_t i = 0; i < 2; ++i) {
		json_owner owner;

		EXPECT_EQ(VMF_INVALID_SCHEMA,
			load(definition(elements[i]), &owner, &error));
		EXPECT_STREQ(paths[i], error.path);
	}
}

TEST(SchemaJson, UnknownPropertiesHaveEscapedJsonPointers)
{
	json_owner owner;
	vmf_schema_json_error error = {};

	EXPECT_EQ(VMF_INVALID_SCHEMA, load(definition(
		R"({"key":"v","number_of_bits":8,"bad/~":0})"),
		&owner, &error));
	EXPECT_STREQ("/messages/0/elements/0/bad~1~0", error.path);
	EXPECT_FALSE(error.path_truncated);
}

TEST(SchemaJson, NativeDependencyFailuresIdentifyTheAuthoredReference)
{
	const char *elements[] = {
		R"({"key":"v","number_of_bits":3,
		 "when":{"all":[{"field":"later",
			"values":[{"first":1,"last":1}]}]}},
		 {"key":"later","number_of_bits":1})",
		R"({"optional":{"key":"n","number_of_bits":3}},
		 {"repeat":{"count_from":"n","maximum":7,
			"body":{"key":"v","number_of_bits":8}}})"};
	const char *paths[] = {
		"/messages/0/elements/0/when/all/0/field",
		"/messages/0/elements/1/repeat/count_from"};

	for (size_t i = 0; i < 2; ++i) {
		json_owner owner;
		vmf_schema_json_error error = {};

		EXPECT_EQ(VMF_INVALID_SCHEMA,
			load(definition(elements[i]), &owner, &error));
		EXPECT_STREQ(paths[i], error.path);
	}
}

TEST(SchemaJson, AmbiguousNativeDiagnosticsDoNotBlameAValidRefinement)
{
	json_owner owner;
	vmf_schema_json_error error = {};
	const char *elements = R"(
		{"key":"length","number_of_bits":3},
		{"key":"v","type":"bits","number_of_bits":8,
		 "length_from":"length","when":{"all":[{"field":"later",
			"values":[{"first":1,"last":1}]}]}},
		{"key":"later","number_of_bits":1})";

	EXPECT_EQ(VMF_INVALID_SCHEMA,
		load(definition(elements), &owner, &error));
	EXPECT_STREQ("/messages/0/elements/1", error.path);
	json_owner encoding;

	EXPECT_EQ(VMF_INVALID_SCHEMA, load(definition(
		R"({"key":"v","number_of_bits":8,"text_encoding":"ascii_8"})"),
		&encoding, &error));
	EXPECT_STREQ("/messages/0/elements/0", error.path);
}

TEST(SchemaJson, ReferenceAvailabilityStillUsesNativeScopeRules)
{
	const char *elements[] = {
		R"({"key":"v","type":"bits","number_of_bits":8,
			"length_from":"missing"})",
		R"({"key":"v","type":"bits","number_of_bits":8,
			"length_from":"later"},{"key":"later","number_of_bits":3})",
		R"({"key":"v","number_of_bits":3,
			"when":{"all":[{"field":"v",
				"values":[{"first":0,"last":1}]}]}})",
		R"({"optional":{"key":"n","number_of_bits":3}},
			{"repeat":{"count_from":"n","maximum":7,
				"body":{"key":"v","number_of_bits":8}}})",
		R"({"repeat":{"count":2,"body":
			{"key":"n","number_of_bits":3}}},
			{"repeat":{"count_from":"n","maximum":7,
				"body":{"key":"v","number_of_bits":8}}})",
		R"({"repeat":{"count":0,"body":
			{"key":"v","type":"bits","number_of_bits":8,
				"length_from":"missing"}}})"};

	for (const char *element : elements) {
		SCOPED_TRACE(element);
		json_owner owner;
		vmf_schema_json_error error = {};

		EXPECT_EQ(VMF_INVALID_SCHEMA,
			load(definition(element), &owner, &error));
		EXPECT_EQ(nullptr, owner.document);
		EXPECT_NE('\0', error.path[0]);
	}
}

TEST(SchemaJson, SignedRepresentationsAndScaleHaveOneTypeDeclaration)
{
	const char *types[] = {"twos_complement", "ones_complement",
		"sign_magnitude"};
	const vmf_integer_representation representations[] = {
		VMF_INTEGER_TWOS_COMPLEMENT, VMF_INTEGER_ONES_COMPLEMENT,
		VMF_INTEGER_SIGN_MAGNITUDE};
	const std::string scale = R"("scale":{
		"factor":{"numerator":1,"denominator":2},
		"bias":{"numerator":0,"denominator":1}})";

	for (size_t i = 0; i < 3; ++i) {
		json_owner owner;
		std::string elements = "{\"key\":\"raw\",\"type\":\"" +
			std::string(types[i]) + "\",\"number_of_bits\":9}," +
			"{\"key\":\"scaled\",\"type\":\"" + types[i] +
			"\",\"number_of_bits\":9," + scale + "}";

		ASSERT_EQ(VMF_OK, load(definition(elements), &owner));
		const auto *set = vmf_schema_json_schemas(owner.document);
		const auto *schema = &set->messages[0];

		for (size_t j = 0; j < 2; ++j) {
			vmf_index_info info = {};

			ASSERT_EQ(VMF_OK, vmf_schema_index_info(set, schema,
				&schema->indexes[j], &info));
			EXPECT_EQ(j ? VMF_FIELD_SCALED : VMF_FIELD_SIGNED,
				info.kind);
			EXPECT_EQ(representations[i],
				info.integer.representation);
		}
	}
	for (const char *type : {"bool", "enum", "bits", "octets", "text",
	     "literal"}) {
		SCOPED_TRACE(type);
		json_owner owner;
		std::string element = "{\"key\":\"v\",\"type\":\"" +
			std::string(type) + "\",\"number_of_bits\":8," +
			scale + "}";

		EXPECT_EQ(VMF_INVALID_SCHEMA, load(definition(element), &owner));
	}
}

TEST(SchemaJson, ZeroCountsAndEmptyMessageRemainExplicitAndValid)
{
	json_owner empty;

	ASSERT_EQ(VMF_OK, load(definition(""), &empty));
	EXPECT_EQ(0u,
		vmf_schema_json_schemas(empty.document)->messages[0].index_count);
	const char *elements = R"(
		{"key":"n","type":"bool","number_of_bits":1},
		{"repeat":{"count":0,
		 "body":{"key":"fixed","number_of_bits":8}}},
		{"repeat":{"count_from":"n","maximum":0,
		 "body":{"key":"counted","number_of_bits":8}}})";
	json_owner owner;

	ASSERT_EQ(VMF_OK, load(definition(elements), &owner));
	const auto *schema = &vmf_schema_json_schemas(owner.document)->messages[0];

	ASSERT_EQ(2u, schema->repeat_count);
	EXPECT_EQ(0u, schema->repeats[0].count);
	EXPECT_EQ(0u, schema->repeats[1].maximum);
	EXPECT_EQ(0u, schema->indexes[1].indicator);
	EXPECT_EQ(0u, schema->indexes[2].indicator);
}

TEST(SchemaJson, OptionalFieldConditionsRequireAnExplicitGroup)
{
	const std::string field = R"({"key":"v","number_of_bits":3,
		"when":{"all":[{"field":"n",
			"values":[{"first":1,"last":1}]}]}})";
	const std::string prefix = R"({"key":"n","number_of_bits":1},)";
	json_owner direct;
	json_owner grouped;

	EXPECT_EQ(VMF_INVALID_SCHEMA,
		load(definition(prefix + "{\"optional\":" + field + "}"),
			&direct));
	ASSERT_EQ(VMF_OK,
		load(definition(prefix + "{\"optional\":[" + field + "]}"),
			&grouped));
}

TEST(SchemaJson, NestedDiagnosticsTruncateExplicitlyAndSurviveRollback)
{
	std::string element = R"({"key":"v","number_of_bits":0})";

	for (size_t i = 0; i < 70; ++i)
		element = "{\"optional\":" + element + "}";
	auto options = vmf_schema_json_default_options();

	options.limits.max_depth = 256;
	json_owner owner;
	vmf_schema_json_error error = {};

	EXPECT_EQ(VMF_INVALID_SCHEMA,
		load(definition(element), &owner, &error, &options));
	EXPECT_EQ(nullptr, owner.document);
	EXPECT_TRUE(error.path_truncated);
	EXPECT_EQ(0, std::strncmp("/messages/0/elements/0/optional",
		error.path,
		std::strlen("/messages/0/elements/0/optional")));
	EXPECT_LT(std::strlen(error.path), sizeof(error.path));
}

TEST(SchemaJson, AuthoredAndGeneratedStructuresRespectDepthAndNodeLimits)
{
	std::string element = R"({"key":"v","number_of_bits":8})";

	for (size_t i = 0; i < 5000; ++i)
		element = "[" + element + "]";
	auto options = vmf_schema_json_default_options();

	options.limits.max_depth = 8;
	json_owner deep;

	EXPECT_EQ(VMF_LIMIT_EXCEEDED,
		load(definition(element), &deep, nullptr, &options));
	EXPECT_EQ(nullptr, deep.document);
	options = vmf_schema_json_default_options();
	options.limits.max_schema_nodes = 1;
	json_owner nodes;

	EXPECT_EQ(VMF_LIMIT_EXCEEDED, load(definition(
		R"({"optional":{"key":"v","number_of_bits":8}})"),
		&nodes, nullptr, &options));
	EXPECT_EQ(nullptr, nodes.document);
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
	const std::string input = definition(R"(
		{"key":"count","number_of_bits":3,"constant":2,
		 "allowed":[{"first":0,"last":4}]},
		{"key":"text","number_of_bits":28,"type":"text",
		 "length_from":"count","length_bias":0,
		 "when":{"any":[{"field":"count","mask":0,
			"negate":false,"values":[{"first":0,"last":0}]}]}},
		{"key":"scaled","number_of_bits":7,
		 "scale":{"factor":{"numerator":1,"denominator":10},
			"bias":{"numerator":0,"denominator":1},"sentinels":[127]}},
		{"optional":[{"key":"optional","number_of_bits":8}]},
		{"recurrence":{"maximum":2,
		 "body":{"key":"recurring","number_of_bits":3}}},
		{"repeat":{"count":1,
		 "body":{"key":"fixed","number_of_bits":8}}},
		{"repeat":{"count_from":"count","maximum":4,
		 "body":{"key":"counted","number_of_bits":8}}})");
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
