#include <gtest/gtest.h>
#include <vmf/schema_json.hpp>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" int vmf_schema_json_c_consumer(void);

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
	const char input[] = "{\"schemas\":[]}";
	auto options = vmf_schema_json_default_options();
	vmf_schema_json_document *document = nullptr;
	vmf_schema_json_error error = {};

	ASSERT_EQ(VMF_OK, vmf_schema_json_load(input, sizeof(input) - 1,
		&options, &document, &error));
	ASSERT_NE(nullptr, document);
	const vmf_schema_set *set = vmf_schema_json_schemas(document);

	ASSERT_NE(nullptr, set);
	EXPECT_EQ(0u, set->count);
	EXPECT_EQ(VMF_OK, error.status);
	EXPECT_STREQ("ok", error.message);
	EXPECT_EQ(SIZE_MAX, error.byte_offset);
	EXPECT_EQ(SIZE_MAX, error.schema_index);
	EXPECT_EQ(0u, error.node_id);
	vmf_schema_json_destroy(document);
}

namespace
{

struct json_owner {
	vmf_schema_json_document *document = nullptr;

	~json_owner()
	{
		vmf_schema_json_destroy(document);
	}
};

static std::string group(unsigned id, const std::string &children)
{
	return "{\"id\":" + std::to_string(id) +
	       ",\"kind\":\"group\",\"group\":{\"children\":[" +
	       children + "]}}";
}

static std::string encoding(unsigned width,
	const std::string &representation = "unsigned",
	const std::string &order = "lsb_first")
{
	return "{\"bit_width\":" + std::to_string(width) +
	       ",\"order\":\"" + order + "\",\"representation\":\"" +
	       representation + "\"}";
}

static std::string scalar(unsigned id, const std::string &kind = "unsigned",
	unsigned width = 8, const std::string &representation = "unsigned",
	const std::string &metadata = "")
{
	return "{\"id\":" + std::to_string(id) +
	       ",\"kind\":\"field\",\"field\":{\"kind\":\"" + kind +
	       "\",\"scalar\":{\"encoding\":" +
	       encoding(width, representation) + metadata + "}}}";
}

static std::string schema(const std::string &root,
	const std::string &identity = "{\"user_message_format\":1}",
	const std::string &extra = "")
{
	return "{\"id\":" + identity +
	       ",\"octet_order\":\"lsb_first\",\"root\":" + root + extra + "}";
}

static std::string set(const std::string &schemas)
{
	return "{\"schemas\":[" + schemas + "]}";
}

static std::string extent(const std::string &kind,
	const std::string &active, unsigned minimum = 0,
	const std::string &maximum = "8")
{
	return "{\"kind\":\"" + kind + "\",\"minimum\":" +
	       std::to_string(minimum) + ",\"maximum\":" +
	       maximum + active + "}";
}

static std::string field(unsigned id, const std::string &kind,
	const std::string &definition)
{
	return "{\"id\":" + std::to_string(id) +
	       ",\"kind\":\"field\",\"field\":{\"kind\":\"" + kind + "\",\"" +
	       kind + "\":" + definition + "}}";
}

static vmf_status load(const std::string &input, json_owner *owner,
	vmf_schema_json_error *error = nullptr,
	const vmf_schema_json_options *configured = nullptr)
{
	auto defaults = vmf_schema_json_default_options();

	return vmf_schema_json_load(input.data(), input.size(),
		configured ? configured : &defaults, &owner->document, error);
}

static const vmf_message_schema *first(const json_owner &owner)
{
	const vmf_schema_set *schemas = vmf_schema_json_schemas(owner.document);

	return schemas && schemas->count ? schemas->schemas[0] : nullptr;
}

TEST(SchemaJson, ApiMisuseInitializesOutputs)
{
	const char input[] = "{\"schemas\":[]}";
	auto options = vmf_schema_json_default_options();
	vmf_schema_json_document *owner = nullptr;
	vmf_schema_json_error error = {};

	EXPECT_EQ(VMF_INVALID_ARGUMENT, vmf_schema_json_load(nullptr, 0,
		&options, &owner, &error));
	EXPECT_EQ(nullptr, owner);
	EXPECT_EQ(VMF_INVALID_ARGUMENT, error.status);
	EXPECT_EQ(SIZE_MAX, error.byte_offset);
	EXPECT_EQ(SIZE_MAX, error.schema_index);
	EXPECT_EQ(0u, error.node_id);
	EXPECT_NE(nullptr, error.message);
	EXPECT_EQ(VMF_INVALID_ARGUMENT, vmf_schema_json_load(input,
		sizeof(input) - 1, nullptr, &owner, &error));
	EXPECT_EQ(nullptr, owner);
	EXPECT_EQ(VMF_INVALID_ARGUMENT, vmf_schema_json_load(input,
		sizeof(input) - 1, &options, nullptr, &error));
	EXPECT_EQ(VMF_INVALID_ARGUMENT, error.status);
}

TEST(SchemaJson, SyntaxFailuresHaveByteLocations)
{
	const char *truncated[] = {
		"", "{", "{\"schemas\":[", "{\"schemas\":[]"};

	for (const char *input : truncated) {
		SCOPED_TRACE(input);
		json_owner owner;
		vmf_schema_json_error error = {};

		EXPECT_EQ(VMF_TRUNCATED, load(input, &owner, &error));
		EXPECT_EQ(nullptr, owner.document);
		EXPECT_LE(error.byte_offset, std::strlen(input));
		EXPECT_EQ(SIZE_MAX, error.schema_index);
	}
	const char *malformed[] = {"{\"schemas\":[],}", "{\"schemas\":[]}x",
		"{\"schemas\":[NaN]}", "{/*comment*/\"schemas\":[]}",
		"{\"schemas\":[],\"x\":\"\\ud800\"}"};

	for (const char *input : malformed) {
		SCOPED_TRACE(input);
		json_owner owner;
		vmf_schema_json_error error = {};

		EXPECT_EQ(VMF_MALFORMED, load(input, &owner, &error));
		EXPECT_EQ(nullptr, owner.document);
		EXPECT_LT(error.byte_offset, std::strlen(input));
	}
	std::string invalid = "{\"schemas\":[],\"x\":\"";

	invalid += static_cast<char>(0xff);
	invalid += "\"}";
	json_owner owner;

	EXPECT_EQ(VMF_MALFORMED, load(invalid, &owner));
}

TEST(SchemaJson, RejectsUnknownDuplicateNullAndWrongTypedProperties)
{
	const char *invalid[] = {"[]", "{}", "{\"schemas\":null}",
		"{\"schemas\":{},\"extra\":0}", "{\"schemas\":[],\"extra\":0}",
		"{\"schemas\":[],\"schemas\":[]}"};

	for (const char *input : invalid) {
		SCOPED_TRACE(input);
		json_owner owner;
		vmf_schema_json_error error = {};

		EXPECT_EQ(VMF_INVALID_SCHEMA, load(input, &owner, &error));
		EXPECT_EQ(nullptr, owner.document);
		EXPECT_EQ(SIZE_MAX, error.byte_offset);
	}
	const char *roots[] = {
		"{\"id\":1,\"kind\":\"group\",\"group\":{\"children\":[]},"
		"\"field\":{}}",
		"{\"id\":1,\"id\":2,\"kind\":\"group\","
		"\"group\":{\"children\":[]}}",
		"{\"id\":1,\"kind\":\"group\","
		"\"group\":{\"children\":[],\"x\":0}}",
		"{\"id\":1,\"kind\":\"group\",\"group\":{\"children\":[]},"
		"\"presence\":null}",
		"{\"id\":1,\"kind\":\"GROUP\",\"group\":{\"children\":[]}}"};

	for (const char *root : roots) {
		SCOPED_TRACE(root);
		json_owner owner;

		EXPECT_EQ(VMF_INVALID_SCHEMA, load(set(schema(root)), &owner));
	}
}

TEST(SchemaJson, OptionalIdentitiesDistinguishOmissionAndPresentZero)
{
	json_owner owner;
	std::string absent = schema(group(1, ""));
	std::string present = schema(group(1, ""),
		"{\"user_message_format\":1,\"standard_version\":0}");
	std::string identified = schema(group(1, ""),
		"{\"user_message_format\":1,\"standard_version\":0,"
		"\"identification\":{\"functional_area\":0,"
		"\"message_number\":1,"
		"\"subtype\":0}}");

	ASSERT_EQ(VMF_OK, load(set(absent + "," + present + "," + identified),
		&owner));
	const vmf_schema_set *schemas = vmf_schema_json_schemas(owner.document);

	ASSERT_EQ(3u, schemas->count);
	EXPECT_FALSE(schemas->schemas[0]->id.standard_version.present);
	EXPECT_TRUE(schemas->schemas[1]->id.standard_version.present);
	EXPECT_EQ(0u, schemas->schemas[1]->id.standard_version.value);
	EXPECT_FALSE(schemas->schemas[1]->id.identification_present);
	EXPECT_TRUE(schemas->schemas[2]->id.identification_present);
	EXPECT_TRUE(schemas->schemas[2]->id.subtype.present);
	EXPECT_EQ(0u, schemas->schemas[2]->id.subtype.value);
	EXPECT_FALSE(vmf_message_id_equal(&schemas->schemas[0]->id,
		&schemas->schemas[1]->id));
}

TEST(SchemaJson, LoadsEveryScalarKindAndIntegerRepresentation)
{
	const char *kinds[] = {"bool", "unsigned", "signed", "enum", "scaled"};
	const vmf_field_kind expected[] = {VMF_FIELD_BOOL, VMF_FIELD_UNSIGNED,
		VMF_FIELD_SIGNED, VMF_FIELD_ENUM, VMF_FIELD_SCALED};
	std::string children;

	for (unsigned i = 0; i < 5; ++i) {
		if (i)
			children += ",";
		children += scalar(i + 2, kinds[i], i ? 8 : 1,
			i == 2 ? "twos_complement" : "unsigned",
			i == 4 ? ",\"scale\":{\"factor\":{\"numerator\":1,"
				 "\"denominator\":2},\"bias\":{"
				 "\"numerator\":-1,\"denominator\":3}}" : "");
	}
	json_owner owner;

	ASSERT_EQ(VMF_OK, load(set(schema(group(1, children))), &owner));
	const vmf_node *root = first(owner)->root;

	ASSERT_EQ(5u, root->as.group.child_count);
	for (unsigned i = 0; i < 5; ++i) {
		const vmf_node *node = &root->as.group.children[i];

		EXPECT_EQ(i + 2, node->id);
		EXPECT_EQ(expected[i], node->as.field.kind);
		EXPECT_EQ(VMF_PRESENCE_ALWAYS, node->presence.kind);
		EXPECT_EQ(0u, node->as.field.as.scalar.allowed_count);
		EXPECT_EQ(0u, node->as.field.as.scalar.label_count);
	}
	EXPECT_EQ(1, root->as.group.children[4].as.field.as.scalar
		.scale.factor.numerator);
	EXPECT_EQ(3u, root->as.group.children[4].as.field.as.scalar
		.scale.bias.denominator);
	const char *representations[] = {
		"twos_complement", "ones_complement", "sign_magnitude"};

	for (unsigned i = 0; i < 3; ++i) {
		json_owner signed_owner;

		ASSERT_EQ(VMF_OK, load(set(schema(group(1,
			scalar(2, "signed", 64, representations[i])))),
			&signed_owner));
		EXPECT_EQ(static_cast<vmf_integer_representation>(i + 1),
			first(signed_owner)->root->as.group.children[0]
				.as.field.as.scalar.encoding.representation);
	}
}

TEST(SchemaJson, PreservesExactIntegerBoundariesAndLengthDelimitedLabels)
{
	std::string metadata = ",\"allowed\":[{\"first\":0,"
		"\"last\":18446744073709551615}],\"labels\":[{"
		"\"code\":18446744073709551615,\"name\":\"A\\u0000B\"}],"
		"\"scale\":{\"factor\":{\"numerator\":-9223372036854775808,"
		"\"denominator\":18446744073709551615},\"bias\":{"
		"\"numerator\":9223372036854775807,\"denominator\":1},"
		"\"sentinels\":[{\"code\":18446744073709551615,"
		"\"name\":\"max\"}]}";
	json_owner owner;

	ASSERT_EQ(VMF_OK, load(set(schema(group(1,
		scalar(2, "scaled", 64, "unsigned", metadata)))), &owner));
	const vmf_scalar_definition *value =
		&first(owner)->root->as.group.children[0].as.field.as.scalar;

	ASSERT_EQ(1u, value->allowed_count);
	EXPECT_EQ(UINT64_MAX, value->allowed[0].last);
	ASSERT_EQ(1u, value->label_count);
	EXPECT_EQ(UINT64_MAX, value->labels[0].code);
	ASSERT_EQ(3u, value->labels[0].name.size);
	EXPECT_EQ(0, std::memcmp("A\0B", value->labels[0].name.data, 3));
	EXPECT_EQ(INT64_MIN, value->scale.factor.numerator);
	EXPECT_EQ(UINT64_MAX, value->scale.factor.denominator);
	EXPECT_EQ(INT64_MAX, value->scale.bias.numerator);
	ASSERT_EQ(1u, value->scale.sentinel_count);
	EXPECT_EQ(UINT64_MAX, value->scale.sentinels[0].code);
}

TEST(SchemaJson, RejectsNonIntegerOrOutOfRangeNumbers)
{
	const char *ids[] = {"0", "-1", "1.0", "1e0", "\"1\"",
		"4294967296", "18446744073709551616"};

	for (const char *id : ids) {
		SCOPED_TRACE(id);
		std::string root = "{\"id\":" + std::string(id) +
			",\"kind\":\"group\",\"group\":{\"children\":[]}}";
		json_owner owner;

		EXPECT_EQ(VMF_INVALID_SCHEMA, load(set(schema(root)), &owner));
	}
	const char *numerators[] = {"-9223372036854775809",
		"9223372036854775808", "0.5", "\"0\""};

	for (const char *numerator : numerators) {
		SCOPED_TRACE(numerator);
		std::string scale = ",\"scale\":{\"factor\":{\"numerator\":" +
			std::string(numerator) + ",\"denominator\":1},"
			"\"bias\":{\"numerator\":0,\"denominator\":1}}";
		json_owner owner;

		EXPECT_EQ(VMF_INVALID_SCHEMA, load(set(schema(group(1,
			scalar(2, "scaled", 64, "unsigned", scale)))), &owner));
	}
}

TEST(SchemaJson, CopiesNamesAndArraysWithoutRetainingTheInput)
{
	std::string text = set(schema(group(1, scalar(2)),
		"{\"user_message_format\":1}", ",\"name\":\"message\""));
	auto location = text.find("\"id\":2");

	text.insert(location, "\"name\":\"value\",");
	const std::string original = text;
	json_owner owner;

	ASSERT_EQ(VMF_OK, load(text, &owner));
	EXPECT_EQ(original, text);
	const vmf_message_schema *loaded = first(owner);
	const vmf_node *child = loaded->root->as.group.children;

	text.assign(text.size(), 'x');
	EXPECT_STREQ("message", loaded->name);
	EXPECT_STREQ("value", child->name);
	EXPECT_EQ(2u, child->id);
	EXPECT_EQ(child, vmf_schema_find(loaded, 2));
	auto limits = vmf_default_limits();

	EXPECT_EQ(VMF_OK, vmf_schema_validate(loaded, &limits, nullptr));
	const char *names[] = {"\"a\\u0000b\"", "null"};

	for (const char *name : names) {
		json_owner rejected;

		EXPECT_EQ(VMF_INVALID_SCHEMA, load(set(schema(group(1, ""),
			"{\"user_message_format\":1}",
			",\"name\":" + std::string(name))), &rejected));
	}
}

} /* namespace */

namespace
{

TEST(SchemaJson, LoadsTextEncodingsAndFieldLengthControls)
{
	const char *encodings[] = {"ascii_7", "ascii_8", "latin_1", "utf_8"};

	for (unsigned i = 0; i < 4; ++i) {
		json_owner owner;
		std::string definition = "{\"encoding\":\"" +
			std::string(encodings[i]) +
			"\",\"order\":\"msb_first\",\"length\":" +
			extent("fixed", ",\"fixed\":2", 2, "2") + "}";

		ASSERT_EQ(VMF_OK, load(set(schema(group(1,
			field(2, "text", definition)))), &owner));
		const vmf_node *child = first(owner)->root->as.group.children;
		const vmf_text_definition *text = &child->as.field.as.text;

		EXPECT_EQ(static_cast<vmf_text_encoding>(i), text->encoding);
		EXPECT_EQ(VMF_BIT_MSB_FIRST, text->order);
		EXPECT_EQ(2u, text->length.fixed);
	}
	const char *kinds[] = {"fixed", "prefix", "field", "remaining"};
	std::string active[] = {",\"fixed\":1",
		",\"prefix\":" + encoding(8) + ",\"bias\":-1",
		",\"field\":2", ""};

	for (unsigned i = 0; i < 4; ++i) {
		json_owner owner;
		std::string definition =
			"{\"order\":\"lsb_first\",\"length\":" +
			extent(kinds[i], active[i], 0,
				i == 3 ? "18446744073709551615" : "8") + "}";

		ASSERT_EQ(VMF_OK, load(set(schema(group(1, scalar(2) + "," +
			field(3, "octets", definition)))), &owner));
		const vmf_extent_definition *length = &first(owner)->root
			->as.group.children[1].as.field.as.octets.length;

		EXPECT_EQ(static_cast<vmf_extent_kind>(i), length->kind);
		EXPECT_EQ(i == 1 ? -1 : 0, length->bias);
		if (i == 3)
			EXPECT_EQ(UINT64_MAX, length->maximum);
	}
	json_owner bits;
	std::string definition = "{\"length\":" +
		extent("fixed", ",\"fixed\":12", 12, "12") + "}";

	ASSERT_EQ(VMF_OK, load(set(schema(group(1,
		field(2, "bits", definition)))), &bits));
	EXPECT_EQ(12u, first(bits)->root->as.group.children[0]
		.as.field.as.bits.length.fixed);
	json_owner literal;

	definition = "{\"encoding\":\"ascii_7\",\"order\":\"lsb_first\","
		"\"length\":" + extent("literal", "") + "}";
	ASSERT_EQ(VMF_OK, load(set(schema(group(1,
		field(2, "text", definition)))), &literal));
	EXPECT_EQ(VMF_EXTENT_LITERAL, first(literal)->root->as.group.children[0]
		.as.field.as.text.length.kind);
}

TEST(SchemaJson, LoadsAllRepeatControlsAndStableBodies)
{
	const char *kinds[] = {"fixed", "prefix", "field", "more_before",
		"more_after", "recurrence_before"};
	const vmf_extent_kind expected[] = {VMF_EXTENT_FIXED, VMF_EXTENT_PREFIX,
		VMF_EXTENT_FIELD, VMF_EXTENT_MORE_BEFORE, VMF_EXTENT_MORE_AFTER,
		VMF_EXTENT_RECURRENCE_BEFORE};
	std::string active[] = {",\"fixed\":1", ",\"prefix\":" + encoding(4),
		",\"field\":2", ",\"more_bit\":true", ",\"more_bit\":false",
		",\"more_bit\":true"};

	for (unsigned i = 0; i < 6; ++i) {
		SCOPED_TRACE(kinds[i]);
		json_owner owner;
		std::string repeat = "{\"id\":3,\"kind\":\"repeat\","
			"\"repeat\":{\"count\":" +
			extent(kinds[i], active[i], i >= 4 ? 1 : 0) +
			",\"body\":" + group(4, scalar(5)) + "}}";

		ASSERT_EQ(VMF_OK, load(set(schema(group(1,
			scalar(2) + "," + repeat))), &owner));
		const vmf_repeat_schema *value =
			&first(owner)->root->as.group.children[1].as.repeat;

		EXPECT_EQ(expected[i], value->count.kind);
		EXPECT_EQ(4u, value->body->id);
		EXPECT_EQ(5u, value->body->as.group.children[0].id);
		EXPECT_EQ(value->body, vmf_schema_find(first(owner), 4));
	}
}

TEST(SchemaJson, LoadsPresenceConditionsAndChoiceArms)
{
	std::string present = scalar(3);
	std::string condition = "{\"kind\":\"condition\",\"condition\":{"
		"\"mode\":\"any\",\"terms\":[{\"field\":2,\"values\":[{"
		"\"first\":1,\"last\":2}]}]}}";

	present.insert(1, "\"presence\":" + condition + ",");
	std::string bit = scalar(4);

	bit.insert(1, "\"presence\":{\"kind\":\"bit\",\"present_bit\":true},");
	std::string choice = "{\"id\":5,\"kind\":\"choice\",\"choice\":{"
		"\"selector\":2,\"cases\":[{\"values\":[{"
		"\"first\":1,\"last\":2}],\"body\":" + scalar(6) +
		"}],\"otherwise\":" + group(7, "") + "}}";
	json_owner owner;

	std::string children_json =
		scalar(2) + "," + present + "," + bit + "," + choice;

	ASSERT_EQ(VMF_OK, load(set(schema(group(1, children_json))), &owner));
	const vmf_node *children = first(owner)->root->as.group.children;
	const vmf_condition *loaded = &children[1].presence.condition;

	EXPECT_EQ(VMF_PRESENCE_CONDITION, children[1].presence.kind);
	EXPECT_EQ(VMF_CONDITION_ANY, loaded->mode);
	EXPECT_FALSE(loaded->negate);
	ASSERT_EQ(1u, loaded->term_count);
	EXPECT_EQ(2u, loaded->terms[0].field);
	EXPECT_EQ(UINT64_MAX, loaded->terms[0].mask);
	EXPECT_FALSE(loaded->terms[0].negate);
	EXPECT_EQ(VMF_PRESENCE_BIT, children[2].presence.kind);
	EXPECT_TRUE(children[2].presence.present_bit);
	const vmf_choice_schema *arm = &children[3].as.choice;

	EXPECT_EQ(2u, arm->selector);
	EXPECT_EQ(UINT64_MAX, arm->mask);
	ASSERT_EQ(1u, arm->case_count);
	EXPECT_EQ(1u, arm->cases[0].values[0].first);
	EXPECT_EQ(6u, arm->cases[0].body->id);
	EXPECT_EQ(7u, arm->otherwise->id);
}

TEST(SchemaJson, ExplicitMasksNegationsAndFallbackOmissionArePreserved)
{
	std::string controlled = scalar(3);

	controlled.insert(1, "\"presence\":{\"kind\":\"condition\","
		"\"condition\":{\"mode\":\"all\",\"negate\":true,\"terms\":[{"
		"\"field\":2,\"mask\":3,\"negate\":true,\"values\":[{"
		"\"first\":1,\"last\":1}]}]}},");
	std::string choice = "{\"id\":4,\"kind\":\"choice\",\"choice\":{"
		"\"selector\":2,\"mask\":3,\"cases\":[{\"values\":[{"
		"\"first\":1,\"last\":1}],\"body\":" + scalar(5) + "}]}}";
	json_owner owner;

	ASSERT_EQ(VMF_OK, load(set(schema(group(1,
		scalar(2) + "," + controlled + "," + choice))), &owner));
	const vmf_node *children = first(owner)->root->as.group.children;

	EXPECT_EQ(VMF_CONDITION_ALL, children[1].presence.condition.mode);
	EXPECT_TRUE(children[1].presence.condition.negate);
	EXPECT_TRUE(children[1].presence.condition.terms[0].negate);
	EXPECT_EQ(3u, children[1].presence.condition.terms[0].mask);
	EXPECT_EQ(3u, children[2].as.choice.mask);
	EXPECT_EQ(nullptr, children[2].as.choice.otherwise);
}

TEST(SchemaJson, RequiresActiveControlsAndRejectsInactiveMembers)
{
	std::vector<std::string> definitions = {
		"{\"id\":2,\"kind\":\"field\",\"field\":{\"kind\":\"unsigned\","
		"\"scalar\":{\"encoding\":{\"bit_width\":8,"
		"\"representation\":\"unsigned\"}}}}",
		"{\"id\":2,\"kind\":\"field\",\"field\":{\"kind\":\"unsigned\","
		"\"scalar\":{\"encoding\":" + encoding(8) + "},\"bits\":{}}}",
		scalar(2, "unsigned", 8, "unsigned", ",\"scale\":{}"),
		scalar(2, "scaled")};
	std::string omitted = scalar(2);

	omitted.insert(1, "\"presence\":{\"kind\":\"bit\"},");
	definitions.push_back(omitted);
	omitted = scalar(2);
	omitted.insert(1,
		"\"presence\":{\"kind\":\"always\",\"present_bit\":true},");
	definitions.push_back(omitted);
	const char *lengths[] = {
		"{\"kind\":\"fixed\",\"fixed\":1,\"maximum\":1}",
		"{\"kind\":\"fixed\",\"minimum\":0,\"maximum\":1}",
		"{\"kind\":\"prefix\",\"minimum\":0,\"maximum\":1}",
		"{\"kind\":\"field\",\"minimum\":0,\"maximum\":1}",
		"{\"kind\":\"fixed\",\"fixed\":1,\"minimum\":0,"
		"\"maximum\":1,\"prefix\":{}}"};

	for (const char *length : lengths)
		definitions.push_back(field(2, "octets",
			"{\"order\":\"lsb_first\",\"length\":" +
			std::string(length) + "}"));
	for (const std::string &definition : definitions) {
		SCOPED_TRACE(definition);
		json_owner owner;

		EXPECT_EQ(VMF_INVALID_SCHEMA, load(set(schema(group(1,
			definition))), &owner));
	}
}

TEST(SchemaJson, NativeValidationErrorsKeepSchemaAndNodeLocations)
{
	json_owner owner;
	vmf_schema_json_error error = {};
	std::string valid = schema(group(1, ""));
	std::string invalid = schema(group(1, scalar(2, "unsigned", 0)),
		"{\"user_message_format\":2}");

	EXPECT_EQ(VMF_INVALID_SCHEMA, load(set(valid + "," + invalid),
		&owner, &error));
	EXPECT_EQ(nullptr, owner.document);
	EXPECT_EQ(1u, error.schema_index);
	EXPECT_EQ(2u, error.node_id);
	EXPECT_EQ(SIZE_MAX, error.byte_offset);
	EXPECT_NE(nullptr, error.message);
	json_owner duplicate;

	EXPECT_EQ(VMF_DUPLICATE_MESSAGE, load(set(valid + "," + valid),
		&duplicate, &error));
	EXPECT_EQ(nullptr, duplicate.document);
	EXPECT_EQ(1u, error.schema_index);
}

TEST(SchemaJson, EnforcesInputMemorySchemaNodeDepthAndFieldLimits)
{
	const std::string input = set(schema(group(1, scalar(2))));
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
	options.limits.max_schema_nodes = 1;
	EXPECT_EQ(VMF_LIMIT_EXCEEDED, load(input, &owner, nullptr, &options));
	options = vmf_schema_json_default_options();
	options.limits.max_depth = 0;
	EXPECT_EQ(VMF_LIMIT_EXCEEDED, load(input, &owner, nullptr, &options));
	options = vmf_schema_json_default_options();
	options.limits.max_field_bits = 7;
	EXPECT_EQ(VMF_LIMIT_EXCEEDED, load(input, &owner, nullptr, &options));
	EXPECT_EQ(nullptr, owner.document);
	std::string nested = scalar(300);

	for (unsigned id = 299; id; --id)
		nested = group(id, nested);
	options = vmf_schema_json_default_options();
	options.limits.max_depth = 8;
	EXPECT_EQ(VMF_LIMIT_EXCEEDED,
		load(set(schema(nested)), &owner, nullptr, &options));
	EXPECT_EQ(nullptr, owner.document);
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
	const std::string input = set(schema(group(1, scalar(2))));
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

	EXPECT_EQ(VMF_INVALID_SCHEMA, load(set(schema(group(1,
		scalar(2, "unsigned", 0)))), &owner, nullptr, &options));
	EXPECT_EQ(0u, log.live);
	EXPECT_EQ(nullptr, owner.document);
	allocator.deallocate = nullptr;
	EXPECT_EQ(VMF_INVALID_ARGUMENT,
		load("{\"schemas\":[]}", &owner, nullptr, &options));
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

	for (unsigned i = 2; i < 100; ++i) {
		if (!children.empty())
			children += ",";
		children += scalar(i);
	}
	std::string input = set(schema(group(1, children)));
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
		load("{\"schemas\":[]}", &owner, nullptr, &options));
	EXPECT_EQ(nullptr, owner.document);
	EXPECT_EQ(0u, log.live);
	EXPECT_EQ(nullptr, log.base);
}

TEST(SchemaJson, RejectsNodeNamesContainingEmbeddedNul)
{
	std::string node = scalar(2);

	node.insert(1, "\"name\":\"a\\u0000b\",");
	json_owner owner;

	EXPECT_EQ(VMF_INVALID_SCHEMA,
		load(set(schema(group(1, node))), &owner));
}

} /* namespace */

namespace
{

TEST(SchemaJson, PreservesUnsupportedSchemaFeatureStatus)
{
	std::string definition = "{\"length\":" +
		extent("remaining", "") + "}";
	json_owner owner;
	vmf_schema_json_error error = {};

	EXPECT_EQ(VMF_UNSUPPORTED_SCHEMA_FEATURE,
		load(set(schema(group(1, field(2, "bits", definition)))),
			&owner, &error));
	EXPECT_EQ(nullptr, owner.document);
	EXPECT_EQ(VMF_UNSUPPORTED_SCHEMA_FEATURE, error.status);
	EXPECT_EQ(0u, error.schema_index);
	EXPECT_EQ(2u, error.node_id);
}

} /* namespace */

namespace
{

TEST(SchemaJson, DepthLimitCountsTheRootAsZeroAndAllowsDeepValidTrees)
{
	std::string node = scalar(300);

	for (unsigned id = 299; id; --id)
		node = group(id, node);
	std::string input = set(schema(node));
	auto options = vmf_schema_json_default_options();

	options.limits.max_depth = 299;
	{
		json_owner owner;

		ASSERT_EQ(VMF_OK, load(input, &owner, nullptr, &options));
		EXPECT_NE(nullptr, vmf_schema_find(first(owner), 300));
	}
	options.limits.max_depth = 298;
	json_owner owner;

	EXPECT_EQ(VMF_LIMIT_EXCEEDED, load(input, &owner, nullptr, &options));
	EXPECT_EQ(nullptr, owner.document);
}

TEST(SchemaJson, EmptySetPermitsZeroSchemaNodeAndDepthLimits)
{
	auto options = vmf_schema_json_default_options();

	options.limits.max_schemas = 0;
	options.limits.max_schema_nodes = 0;
	options.limits.max_depth = 0;
	json_owner owner;

	ASSERT_EQ(VMF_OK,
		load("{\"schemas\":[]}", &owner, nullptr, &options));
	EXPECT_EQ(0u, vmf_schema_json_schemas(owner.document)->count);
}

} /* namespace */
