#include <gtest/gtest.h>
#include <vmf/header_json.h>
#include "../../tools/cli.hpp"
#include "fixtures.hpp"
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{

struct cli_fixture {
	uint32_t address[1] = {1};
	vmf_index_refinements refinements = {};
	vmf_scale_definition scale = {{1, 1}, {0, 1}, nullptr, 0};
	vmf_index_schema field = {};
	vmf_message_schema schema = {};
	vmf_schema_set schemas = {};
	vmf_message_id id = {};
	cli_arena arena = {};

	cli_fixture()
	{
		field.index_number = {address, 1};
		field.number_of_bits = 64;
		field.refinements = &refinements;
		schema.message_number = {1, 1};
		schema.indexes = &field;
		schema.index_count = 1;
		schemas.envelope = VMF_ENVELOPE_D1;
		schemas.messages = &schema;
		schemas.message_count = 1;
		schemas.standard.kind = VMF_STANDARD_RAW_CODE;
		schemas.standard.as.raw_code = 0;
		vmf_schema_message_id(&schemas, &schema, &id);
	}

	~cli_fixture()
	{
		cli_arena_destroy(&arena);
	}
};

vmf_status parse(cli_fixture *fixture, const char *text, vmf_event *event,
	uint64_t *time = nullptr, const vmf_message_schema **selected = nullptr)
{
	cli_json context;

	cli_json_init(&context);
	yyjson_doc *document = cli_json_read(&context, text, strlen(text));

	if (!document)
		return VMF_MALFORMED;
	const vmf_message_schema *active = &fixture->schema;
	uint64_t timestamp = 0;
	vmf_status status = cli_event_from_json(yyjson_doc_get_root(document),
		VMF_ENVELOPE_D1, &fixture->schemas, &active, &fixture->arena,
		event, &timestamp);

	yyjson_doc_free(document);
	if (time)
		*time = timestamp;
	if (selected)
		*selected = active;
	return status;
}

vmf_status round_trip(cli_fixture *fixture, const vmf_event *source,
	vmf_event *event, vmf_envelope_version profile = VMF_ENVELOPE_D1,
	uint64_t *time = nullptr)
{
	cli_json context;

	cli_json_init(&context);
	yyjson_mut_doc *document =
		cli_event_to_json(&context, source, profile, 1234567890123456);

	if (!document)
		return VMF_OUT_OF_MEMORY;
	size_t size = 0;
	char *line = yyjson_mut_write(document, 0, &size);

	yyjson_mut_doc_free(document);
	if (!line)
		return VMF_OUT_OF_MEMORY;
	yyjson_doc *input = cli_json_read(&context, line, size);

	std::free(line);
	if (!input)
		return VMF_MALFORMED;
	const vmf_message_schema *active = &fixture->schema;
	uint64_t timestamp = 0;
	vmf_status status = cli_event_from_json(yyjson_doc_get_root(input),
		profile, &fixture->schemas, &active, &fixture->arena, event,
		&timestamp);

	yyjson_doc_free(input);
	if (time)
		*time = timestamp;
	return status;
}

vmf_event field_event(cli_fixture *fixture, vmf_field_kind kind)
{
	const vmf_encode_type types[] = {VMF_ENCODE_BOOL, VMF_ENCODE_UNSIGNED,
		VMF_ENCODE_SIGNED, VMF_ENCODE_ENUM, VMF_ENCODE_SCALED,
		VMF_ENCODE_TEXT, VMF_ENCODE_OCTETS, VMF_ENCODE_BITS};

	fixture->field.encode_type = types[kind];
	fixture->field.number_of_bits = kind == VMF_FIELD_BOOL ? 1 : 64;
	fixture->refinements = {};
	if (kind == VMF_FIELD_SIGNED)
		fixture->refinements.representation =
			VMF_INTEGER_TWOS_COMPLEMENT;
	if (kind == VMF_FIELD_SCALED)
		fixture->refinements.scale = &fixture->scale;
	if (kind == VMF_FIELD_TEXT)
		fixture->refinements.text_encoding = VMF_TEXT_UTF_8;
	vmf_event event = {};

	event.kind = VMF_EVENT_FIELD;
	event.schema = &fixture->schema;
	event.index = &fixture->field;
	event.field_kind = kind;
	event.depth = 1;
	event.occurrence = 1234567890123456;
	event.as.field.total_units = 1;
	return event;
}

TEST(CliJson, ScalarFieldsPreserveExactValuesAndDescriptorPointers)
{
	cli_fixture fixture;
	vmf_event decoded = {};

	for (auto kind : {VMF_FIELD_BOOL, VMF_FIELD_UNSIGNED,
		     VMF_FIELD_SIGNED, VMF_FIELD_ENUM, VMF_FIELD_SCALED}) {
		auto source = field_event(&fixture, kind);

		source.as.field.value.unsigned_integer.value = UINT64_MAX;
		if (kind == VMF_FIELD_BOOL)
			source.as.field.value.boolean.value = true;
		if (kind == VMF_FIELD_SIGNED)
			source.as.field.value.signed_integer.value = INT64_MIN;
		ASSERT_EQ(VMF_OK, round_trip(&fixture, &source, &decoded));
		EXPECT_EQ(&fixture.field, decoded.index);
		EXPECT_EQ(&fixture.schema, decoded.schema);
		EXPECT_EQ(1u, decoded.depth);
		EXPECT_EQ(source.occurrence, decoded.occurrence);
		EXPECT_EQ(0u, decoded.as.field.offset_units);
		EXPECT_EQ(1u, decoded.as.field.total_units);
		if (kind == VMF_FIELD_BOOL) {
			EXPECT_TRUE(decoded.as.field.value.boolean.value);
		} else if (kind == VMF_FIELD_SIGNED) {
			EXPECT_EQ(INT64_MIN,
				decoded.as.field.value.signed_integer.value);
		} else {
			EXPECT_EQ(UINT64_MAX,
				decoded.as.field.value.unsigned_integer.value);
		}
	}
}

TEST(CliJson, ChunkFieldsOwnTextOctetsBitsAndPreserveUnknownTotals)
{
	cli_fixture fixture;
	vmf_event decoded = {};
	auto source = field_event(&fixture, VMF_FIELD_TEXT);
	const char text[] = "\"\\\n\xc3\xa9\0x";

	source.as.field.offset_units = 9007199254740993;
	source.as.field.total_units = VMF_UNITS_UNKNOWN;
	source.as.field.value.text = {text, sizeof(text) - 1, false};
	ASSERT_EQ(VMF_OK, round_trip(&fixture, &source, &decoded));
	EXPECT_EQ(source.as.field.offset_units, decoded.as.field.offset_units);
	EXPECT_EQ(VMF_UNITS_UNKNOWN, decoded.as.field.total_units);
	EXPECT_FALSE(decoded.as.field.value.text.last);
	EXPECT_EQ(std::string(text, sizeof(text) - 1),
		std::string(decoded.as.field.value.text.data,
			decoded.as.field.value.text.size));
	const uint8_t bytes[] = {0, 1, 127, 128, 255};

	source = field_event(&fixture, VMF_FIELD_OCTETS);
	source.as.field.value.octets = {bytes, sizeof(bytes), true};
	ASSERT_EQ(VMF_OK, round_trip(&fixture, &source, &decoded));
	ASSERT_EQ(sizeof(bytes), decoded.as.field.value.octets.size);
	EXPECT_EQ(0, memcmp(bytes, decoded.as.field.value.octets.data,
		sizeof(bytes)));
	EXPECT_TRUE(decoded.as.field.value.octets.last);
	source = field_event(&fixture, VMF_FIELD_BITS);
	const uint8_t bit_bytes[] = {0, 1, 127, 128, 1};

	source.as.field.value.bits = {bit_bytes, 33, true};
	ASSERT_EQ(VMF_OK, round_trip(&fixture, &source, &decoded));
	EXPECT_EQ(33u, decoded.as.field.value.bits.bit_count);
	EXPECT_EQ(0, memcmp(bit_bytes, decoded.as.field.value.bits.data,
		sizeof(bytes)));
	source.as.field.value.bits = {nullptr, 0, true};
	source.as.field.total_units = 0;
	ASSERT_EQ(VMF_OK, round_trip(&fixture, &source, &decoded));
	EXPECT_EQ(0u, decoded.as.field.value.bits.bit_count);
}

TEST(CliJson, EveryBoundaryKindPreservesItsActiveMetadata)
{
	cli_fixture fixture;

	for (auto kind : {VMF_EVENT_GROUP_BEGIN, VMF_EVENT_GROUP_END,
		     VMF_EVENT_ABSENT, VMF_EVENT_MESSAGE_BEGIN,
		     VMF_EVENT_MESSAGE_END, VMF_EVENT_PDU_END}) {
		vmf_event source = {};
		vmf_event decoded = {};

		source.kind = kind;
		if (kind == VMF_EVENT_GROUP_BEGIN ||
			kind == VMF_EVENT_GROUP_END ||
			kind == VMF_EVENT_ABSENT) {
			source.schema = &fixture.schema;
			source.scope = {VMF_SCOPE_ROOT, 0};
			source.depth = 4;
			source.occurrence = 19;
			source.as.group = {VMF_UNITS_UNKNOWN, true, false};
		} else if (kind != VMF_EVENT_PDU_END) {
			source.schema = &fixture.schema;
			source.as.message.id = fixture.id;
			source.as.message.index = 12;
			source.as.message.payload_octets = VMF_LENGTH_UNKNOWN;
		}
		ASSERT_EQ(VMF_OK, round_trip(&fixture, &source, &decoded));
		EXPECT_EQ(kind, decoded.kind);
		EXPECT_EQ(source.index, decoded.index);
		EXPECT_EQ(source.schema, decoded.schema);
		EXPECT_EQ(source.depth, decoded.depth);
		EXPECT_EQ(source.occurrence, decoded.occurrence);
		if (kind == VMF_EVENT_GROUP_BEGIN ||
			kind == VMF_EVENT_GROUP_END) {
			EXPECT_EQ(VMF_UNITS_UNKNOWN, decoded.as.group.count);
			EXPECT_TRUE(decoded.as.group.more_present);
			EXPECT_FALSE(decoded.as.group.more);
		} else if (kind == VMF_EVENT_MESSAGE_BEGIN ||
			kind == VMF_EVENT_MESSAGE_END) {
			EXPECT_EQ(12u, decoded.as.message.index);
			EXPECT_EQ(VMF_LENGTH_UNKNOWN,
				decoded.as.message.payload_octets);
			EXPECT_TRUE(vmf_message_id_equal(
				&source.as.message.id, &decoded.as.message.id));
		}
	}
}

TEST(CliJson, EnvelopePreservesEveryProfileAndOptionalHeaderData)
{
	cli_fixture fixture;
	const uint8_t key[] = {0, 0x81, 0xff};
	const uint8_t block[8] = {};
	vmf_header_bytes tokens[] = {{true, block, 8}, {true, block, 8}};
	vmf_message_header message = {};
	vmf_dtg dtg = {true, 26, 10, 15, 12, 34, 56, {true, 1234}};
	vmf_reference reference = {{true, {}, {true, {"ref", 3}}},
		dtg, 8, 9};
	uint16_t markings[] = {0, 511};

	message.id = fixture.id;
	message.id.standard_version = {true, 0};
	message.id.identification_present = true;
	message.id.functional_area = 7;
	message.id.message_number = 8;
	message.id.subtype = {true, 0};
	message.file_name = {true, {"a\"b", 3}};
	message.message_size_octets = {true, 1};
	message.security.present = true;
	message.security.keying_material = {true, key, sizeof(key)};
	message.security.tokens_present = true;
	message.security.tokens = tokens;
	message.security.token_count = 2;
	message.security.spi = 3;
	message.security.initialization = {true, block, 8};
	message.security.authentication_a = {true, block, 8};
	message.security.authentication_b = {};
	message.security.signed_ack_request = true;
	message.security.padding_present = true;
	message.security.padding_length = 3;
	message.security.padding = {true, key, sizeof(key)};
	message.operation = 2;
	message.retransmit = true;
	message.precedence = 5;
	message.classification = 3;
	message.originator_dtg = dtg;
	message.perishability = dtg;
	message.perishability.extension = {};
	message.acknowledgment = {true, true, false, true};
	message.has_body = true;
	message.references = &reference;
	message.reference_count = 1;
	vmf_address recipients[] = {{true, {true, 0}, {}}};
	vmf_address information = {true, {true, 1}, {}};

	for (auto profile : {VMF_ENVELOPE_B, VMF_ENVELOPE_C,
		     VMF_ENVELOPE_D, VMF_ENVELOPE_D1}) {
		vmf_header header = {};

		message.id.standard_version.present =
			profile != VMF_ENVELOPE_B;
		message.security.present = profile != VMF_ENVELOPE_B;
		reference.functional_area = profile == VMF_ENVELOPE_B ? 8 : 0;
		reference.message_number = profile == VMF_ENVELOPE_B ? 9 : 0;
		message.marking_b = {profile == VMF_ENVELOPE_B, 123};
		message.marking_c = {profile == VMF_ENVELOPE_C, {"mark", 4}};
		message.markings = markings;
		message.marking_count = profile >= VMF_ENVELOPE_D ? 2 : 0;
		header.b.fields.compression = {true, 0};
		header.b.fields.header_size_octets = {
			profile != VMF_ENVELOPE_B, 123};
		header.b.fields.originator = {true, {}, {true, {"name", 4}}};
		header.b.fields.information_addressees = &information;
		header.b.fields.information_addressee_count = 1;
		header.b.fields.messages = &message;
		header.b.fields.message_count = 1;
		header.b.fields.recipients = recipients;
		header.b.fields.recipient_count = 1;
		vmf_event source = {};
		vmf_event decoded = {};
		uint64_t time = 0;

		source.kind = VMF_EVENT_ENVELOPE;
		source.as.envelope = {static_cast<uint8_t>(profile), &header,
			vmf_header_layout(profile).size, 123,
			VMF_LENGTH_UNKNOWN};
		auto json_options = vmf_header_json_default_options();
		vmf_header_json_text *json_text = nullptr;
		vmf_header_json_error json_error;

		ASSERT_EQ(VMF_OK, vmf_header_json_serialize(profile, &header,
			&json_options, &json_text, &json_error))
			<< profile << " " << json_error.message
			<< " " << json_error.path;
		vmf_header_json_text_destroy(json_text);
		ASSERT_EQ(VMF_OK,
			round_trip(&fixture, &source, &decoded, profile,
				&time)) << profile;
		EXPECT_EQ(1234567890123456u, time);
		EXPECT_EQ(VMF_EVENT_ENVELOPE, decoded.kind);
		EXPECT_EQ(profile, decoded.as.envelope.wire_version);
		EXPECT_EQ(vmf_header_layout(profile).size,
			decoded.as.envelope.object_size);
		EXPECT_EQ(123u, decoded.as.envelope.header_octets);
		EXPECT_EQ(VMF_LENGTH_UNKNOWN, decoded.as.envelope.pdu_octets);
		auto *fields = static_cast<const vmf_header_fields *>(
			decoded.as.envelope.header);

		ASSERT_EQ(1u, fields->message_count);
		EXPECT_TRUE(fields->compression.present);
		EXPECT_EQ(0u, fields->compression.value);
		EXPECT_TRUE(fields->originator.name.present);
		EXPECT_EQ("name", std::string(
			fields->originator.name.value.data,
			fields->originator.name.value.size));
		EXPECT_EQ(1u, fields->information_addressee_count);
		const auto *copy = &fields->messages[0];

		EXPECT_EQ(2u, copy->operation);
		EXPECT_TRUE(copy->retransmit);
		EXPECT_EQ(5u, copy->precedence);
		EXPECT_EQ(3u, copy->classification);
		EXPECT_EQ(26u, copy->originator_dtg.year);
		EXPECT_EQ(10u, copy->originator_dtg.month);
		EXPECT_EQ(15u, copy->originator_dtg.day);
		EXPECT_EQ(12u, copy->originator_dtg.hour);
		EXPECT_EQ(34u, copy->originator_dtg.minute);
		EXPECT_EQ(56u, copy->originator_dtg.second);
		EXPECT_EQ(1234u, copy->originator_dtg.extension.value);
		EXPECT_TRUE(copy->originator_dtg.extension.present);
		EXPECT_TRUE(copy->perishability.present);
		EXPECT_FALSE(copy->perishability.extension.present);
		EXPECT_TRUE(copy->acknowledgment.machine);
		EXPECT_FALSE(copy->acknowledgment.operator_ack);
		EXPECT_TRUE(copy->acknowledgment.operator_reply);
		EXPECT_FALSE(copy->response.present);
		EXPECT_TRUE(copy->has_body);
		ASSERT_EQ(1u, copy->reference_count);
		EXPECT_EQ("ref", std::string(
			copy->references[0].originator.name.value.data, 3));
		EXPECT_EQ(1234u, copy->references[0].dtg.extension.value);
		if (profile == VMF_ENVELOPE_B) {
			EXPECT_TRUE(copy->marking_b.present);
			EXPECT_EQ(123u, copy->marking_b.value);
			EXPECT_EQ(8u, copy->references[0].functional_area);
			EXPECT_EQ(9u, copy->references[0].message_number);
		} else if (profile == VMF_ENVELOPE_C) {
			EXPECT_EQ("mark", std::string(
				copy->marking_c.value.data, 4));
		} else {
			ASSERT_EQ(2u, copy->marking_count);
			EXPECT_EQ(511u, copy->markings[1]);
		}
		EXPECT_EQ(0u, fields->recipients[0].urn.value);
		EXPECT_TRUE(fields->recipients[0].urn.present);
		EXPECT_TRUE(fields->messages[0].file_name.present);
		EXPECT_EQ("a\"b", std::string(
			fields->messages[0].file_name.value.data, 3));
		if (profile != VMF_ENVELOPE_B) {
			const auto *security = &fields->messages[0].security;

			EXPECT_EQ(0, memcmp(key, security->keying_material.data,
				sizeof(key)));
			ASSERT_EQ(2u, security->token_count);
			EXPECT_TRUE(security->tokens[0].present);
			EXPECT_EQ(8u, security->tokens[0].size);
			EXPECT_TRUE(security->tokens[1].present);
			EXPECT_EQ(3u, security->spi);
			EXPECT_TRUE(security->initialization.present);
			EXPECT_EQ(8u, security->initialization.size);
			EXPECT_EQ(8u, security->authentication_a.size);
			EXPECT_FALSE(security->authentication_b.present);
			EXPECT_EQ(0u, security->authentication_b.size);
			EXPECT_TRUE(security->signed_ack_request);
			EXPECT_TRUE(security->padding_present);
			EXPECT_EQ(3u, security->padding_length);
			EXPECT_EQ(0, memcmp(key, security->padding.data, 3));
		}
	}
}

TEST(CliJson, ShortenedHeadersPreserveVersionFifteenAndAddresses)
{
	cli_fixture fixture;
	vmf_address recipient = {true, {}, {true, {"recipient", 9}}};

	for (auto profile : {VMF_ENVELOPE_C, VMF_ENVELOPE_D,
		     VMF_ENVELOPE_D1}) {
		vmf_header header = {};

		header.c.shortened = true;
		header.c.fields.originator = {true, {true, 0}, {}};
		header.c.fields.recipients = &recipient;
		header.c.fields.recipient_count = 1;
		vmf_event source = {};
		vmf_event decoded = {};

		source.kind = VMF_EVENT_ENVELOPE;
		source.as.envelope = {15, &header,
			vmf_header_layout(profile).size, 8, 8};
		ASSERT_EQ(VMF_OK,
			round_trip(&fixture, &source, &decoded, profile));
		EXPECT_EQ(15u, decoded.as.envelope.wire_version);
		auto *copy = static_cast<const vmf_header_c *>(
			decoded.as.envelope.header);

		EXPECT_TRUE(copy->shortened);
		EXPECT_TRUE(copy->fields.originator.urn.present);
		EXPECT_EQ(0u, copy->fields.originator.urn.value);
		EXPECT_EQ(0u, copy->fields.message_count);
		ASSERT_EQ(1u, copy->fields.recipient_count);
		EXPECT_EQ("recipient", std::string(
			copy->fields.recipients[0].name.value.data, 9));
	}
}

std::string header_line(const vmf_header *header)
{
	cli_json context;

	cli_json_init(&context);
	yyjson_mut_doc *doc = cli_json_new(&context);

	yyjson_mut_doc_set_root(doc,
		cli_header_to_json(doc, VMF_ENVELOPE_D1, header));
	char *line = yyjson_mut_write(doc, 0, nullptr);
	std::string result = line ? line : "";

	std::free(line);
	yyjson_mut_doc_free(doc);
	return result;
}

bool read_header(cli_fixture *fixture, const std::string &line)
{
	cli_json context;

	cli_json_init(&context);
	yyjson_doc *doc = cli_json_read(&context, line.data(), line.size());
	vmf_header output;

	memset(&output, 0x5a, sizeof(output));
	vmf_header original = output;
	bool result = cli_header_from_json(yyjson_doc_get_root(doc),
		VMF_ENVELOPE_D1, &fixture->arena, &output);

	if (!result)
		EXPECT_EQ(0, memcmp(&original, &output, sizeof(output)));
	yyjson_doc_free(doc);
	return result;
}

TEST(CliJson, HeaderObjectsRejectUnknownDuplicatesAndMissingRequiredData)
{
	cli_fixture fixture;
	vmf_header header = {};
	vmf_message_header message = {};
	vmf_reference reference = {};
	vmf_address recipient = {true, {true, 2}, {}};

	header.d1.fields.originator = {true, {true, 1}, {}};
	header.d1.fields.messages = &message;
	header.d1.fields.message_count = 1;
	message.id = fixture.id;
	message.originator_dtg = {true, 26, 10, 15, 12, 34, 56, {}};
	message.response.present = true;
	message.response.receipt_compliance = 4;
	message.response.acknowledged = message.originator_dtg;
	reference.originator = {true, {true, 3}, {}};
	reference.dtg = message.originator_dtg;
	header.d1.fields.recipients = &recipient;
	header.d1.fields.recipient_count = 1;
	message.references = &reference;
	message.reference_count = 1;
	message.security.present = true;
	message.security.spi = 3;
	std::string baseline = header_line(&header);

	ASSERT_FALSE(baseline.empty());
	ASSERT_TRUE(read_header(&fixture, baseline));
	const char *keys[] = {"fields", "compression", "operation", "urn",
		"year", "receipt_compliance", "dtg", "spi"};

	for (auto *key : keys) {
		std::string needle = std::string("\"") + key + "\":";
		size_t position = baseline.find(needle);

		ASSERT_NE(std::string::npos, position) << key;
		std::string unknown = baseline;

		unknown.insert(position, "\"unknown_member\":false,");
		EXPECT_FALSE(read_header(&fixture, unknown)) << key;
		std::string duplicate = baseline;

		duplicate.insert(position, needle + "null,");
		EXPECT_FALSE(read_header(&fixture, duplicate)) << key;
	}
	const char *optional[] = {",\"shortened\":false",
		"\"compression\":null,", "\"operation\":0,"};

	for (auto *member : optional) {
		std::string incomplete = baseline;
		size_t position = incomplete.find(member);

		ASSERT_NE(std::string::npos, position) << member;
		incomplete.erase(position, strlen(member));
		EXPECT_TRUE(read_header(&fixture, incomplete)) << member;
	}
	const char *missing[] = {"\"urn\":1,", "\"year\":26,",
		"\"receipt_compliance\":4,", "\"spi\":3,"};

	for (auto *member : missing) {
		std::string incomplete = baseline;
		size_t position = incomplete.find(member);

		ASSERT_NE(std::string::npos, position) << member;
		incomplete.erase(position, strlen(member));
		EXPECT_FALSE(read_header(&fixture, incomplete)) << member;
	}
}

TEST(CliJson, RejectsMalformedRecordsWithoutPublishingAnEvent)
{
	cli_fixture fixture;
	const char *invalid[] = {
		"[]", "{}", "{\"event\":\"bad\"}",
		"{\"event\":\"PDU_END\",\"event\":\"PDU_END\"}",
		"{\"event\":\"PDU_END\",\"unexpected\":0}",
		"{\"event\":\"FIELD\",\"index_number\":[],\"depth\":0,"
		"\"occurrence\":\"0\"}",
		"{\"event\":\"FIELD\",\"index_number\":[1],\"depth\":0,"
		"\"occurrence\":0,\"offset_units\":\"0\","
		"\"total_units\":\"1\",\"value\":{\"kind\":\"unsigned\","
		"\"value\":\"1\"}}",
		"{\"event\":\"FIELD\",\"index_number\":[1],\"depth\":0,"
		"\"occurrence\":\"0\",\"offset_units\":\"0\","
		"\"total_units\":\"1\",\"value\":{\"kind\":\"signed\","
		"\"value\":\"1\"}}"
	};

	for (auto *line : invalid) {
		SCOPED_TRACE(line);
		vmf_event event;

		memset(&event, 0x5a, sizeof(event));
		vmf_event original = event;

		EXPECT_NE(VMF_OK, parse(&fixture, line, &event));
		EXPECT_EQ(0, memcmp(&event, &original, sizeof(event)));
	}
}

TEST(CliJson, UnknownIdentityAndIndexReturnExplicitStatuses)
{
	cli_fixture fixture;
	vmf_event event = {};
	const vmf_message_schema *selected = nullptr;

	EXPECT_EQ(VMF_UNKNOWN_MESSAGE, parse(&fixture,
		"{\"event\":\"MESSAGE_BEGIN\",\"id\":{"
		"\"user_message_format\":2},\"index\":0,"
		"\"payload_octets\":null}", &event, nullptr, &selected));
	EXPECT_EQ(&fixture.schema, selected);
	EXPECT_EQ(VMF_INVALID_SCHEMA, parse(&fixture,
		"{\"event\":\"ABSENT\",\"index_number\":[999],\"depth\":0,"
		"\"occurrence\":\"0\"}", &event));
}

TEST(CliJson, PrimitiveDecimalsRejectNoncanonicalAndOverflowValues)
{
	const char *unsigned_invalid[] = {"0", "-1", "null", "\"\"",
		"\"+1\"", "\"01\"", "\" 1\"", "\"1e2\"",
		"\"18446744073709551616\"", "\"1\\u0000x\""};
	const char *signed_invalid[] = {"1", "null", "\"-0\"",
		"\"-01\"", "\"9223372036854775808\"",
		"\"-9223372036854775809\""};

	for (auto *text : unsigned_invalid) {
		cli_json context;

		cli_json_init(&context);
		yyjson_doc *doc = cli_json_read(&context, text, strlen(text));
		uint64_t result = 123;

		ASSERT_NE(nullptr, doc);
		EXPECT_FALSE(cli_json_u64(yyjson_doc_get_root(doc),
			&result, false)) << text;
		EXPECT_EQ(123u, result);
		yyjson_doc_free(doc);
		EXPECT_EQ(0u, context.used);
	}
	for (auto *text : signed_invalid) {
		cli_json context;

		cli_json_init(&context);
		yyjson_doc *doc = cli_json_read(&context, text, strlen(text));
		int64_t result = 123;

		ASSERT_NE(nullptr, doc);
		EXPECT_FALSE(cli_json_i64(yyjson_doc_get_root(doc),
			&result)) << text;
		EXPECT_EQ(123, result);
		yyjson_doc_free(doc);
		EXPECT_EQ(0u, context.used);
	}
}

TEST(CliJson, RejectsMalformedNestedValuesAndDecorativeAnnotations)
{
	cli_fixture fixture;
	vmf_event event = {};
	const std::string field = "{\"event\":\"FIELD\",\"index_number\":[1],"
		"\"depth\":1,\"occurrence\":\"0\",\"offset_units\":\"0\","
		"\"total_units\":\"1\",\"value\":";
	const char *invalid[] = {
		"{\"kind\":\"unsigned\",\"value\":\"1\",\"value\":\"2\"}",
		"{\"kind\":\"unsigned\",\"value\":\"1\",\"extra\":false}",
		"{\"kind\":\"unsigned\",\"value\":\"01\"}",
		"{\"kind\":\"unsigned\",\"value\":1}"
	};

	for (auto *value : invalid) {
		std::string line = field + value + "}";

		EXPECT_NE(VMF_OK, parse(&fixture, line.c_str(), &event));
	}
	std::string line = field + "{\"kind\":\"unsigned\",\"value\":\"1\"},"
		"\"name\":\"edited name\",\"schema_name\":\"annotation\"}";

	EXPECT_EQ(VMF_MALFORMED, parse(&fixture, line.c_str(), &event));
	fixture.field.encode_type = VMF_ENCODE_BITS;
	const char *bits[] = {
		"{\"kind\":\"bits\",\"hex\":\"ff\",\"bit_count\":\"1\","
		"\"last\":true}",
		"{\"kind\":\"bits\",\"hex\":\"0\",\"bit_count\":\"1\","
		"\"last\":true}",
		"{\"kind\":\"bits\",\"hex\":\"gg\",\"bit_count\":\"8\","
		"\"last\":true}",
		"{\"kind\":\"bits\",\"hex\":\"00\",\"bit_count\":\"0\","
		"\"last\":true}"
	};

	for (auto *value : bits) {
		line = field + value + "}";
		EXPECT_NE(VMF_OK, parse(&fixture, line.c_str(), &event));
	}
}

TEST(CliJson, ArchiveManifestsResolveDefaultsAndRejectChangedDefinitions)
{
	cli_fixture fixture;
	cli_json context;

	cli_json_init(&context);
	yyjson_mut_doc *metadata = cli_metadata_to_json(&context,
		VMF_ENVELOPE_D1, &fixture.schemas);

	ASSERT_NE(nullptr, metadata);
	yyjson_doc *input = yyjson_mut_doc_imut_copy(metadata, nullptr);

	ASSERT_NE(nullptr, input);
	vmf_envelope_version profile = VMF_ENVELOPE_B;

	EXPECT_EQ(VMF_OK, cli_metadata_from_json(yyjson_doc_get_root(input),
		&fixture.schemas, &profile));
	EXPECT_EQ(VMF_ENVELOPE_D1, profile);
	fixture.field.encode_type = VMF_ENCODE_UNSIGNED;
	fixture.refinements.bit_order = VMF_BIT_LSB_FIRST;
	fixture.refinements.representation = VMF_INTEGER_UNSIGNED;
	fixture.schema.octet_order = VMF_BIT_LSB_FIRST;
	EXPECT_EQ(VMF_OK, cli_metadata_from_json(yyjson_doc_get_root(input),
		&fixture.schemas, &profile));
	fixture.field.number_of_bits = 63;
	EXPECT_EQ(VMF_INVALID_SCHEMA,
		cli_metadata_from_json(yyjson_doc_get_root(input),
			&fixture.schemas, &profile));
	yyjson_doc_free(input);
	yyjson_mut_doc_free(metadata);
	EXPECT_EQ(0u, context.used);
}

TEST(CliJson, ArchiveRejectsUnsupportedVersionsAndAcceptsUnrelatedMessages)
{
	cli_fixture fixture;
	cli_json context;

	cli_json_init(&context);
	const char unsupported[] =
		"{\"format\":\"vmf-events\",\"version\":99,"
		"\"profile\":\"D1\",\"schemas\":[]}";
	yyjson_doc *input = cli_json_read(
		&context, unsupported, sizeof(unsupported) - 1);
	vmf_envelope_version profile = VMF_ENVELOPE_B;

	ASSERT_NE(nullptr, input);
	EXPECT_EQ(VMF_MALFORMED,
		cli_metadata_from_json(yyjson_doc_get_root(input),
			&fixture.schemas, &profile));
	yyjson_doc_free(input);
	yyjson_mut_doc *metadata = cli_metadata_to_json(&context,
		VMF_ENVELOPE_D1, &fixture.schemas);

	ASSERT_NE(nullptr, metadata);
	input = yyjson_mut_doc_imut_copy(metadata, nullptr);
	ASSERT_NE(nullptr, input);
	vmf_message_schema messages[] = {fixture.schema, fixture.schema};
	vmf_schema_set schemas = fixture.schemas;

	messages[1].message_number.number = 2;
	schemas.messages = messages;
	schemas.message_count = 2;
	EXPECT_EQ(VMF_OK, cli_metadata_from_json(yyjson_doc_get_root(input),
		&schemas, &profile));
	schemas.envelope = VMF_ENVELOPE_C;
	EXPECT_EQ(VMF_INVALID_SCHEMA,
		cli_metadata_from_json(yyjson_doc_get_root(input),
			&schemas, &profile));
	yyjson_doc_free(input);
	yyjson_mut_doc_free(metadata);
}

struct test_clock {
	uint64_t now;
	std::vector<uint64_t> deadlines;
	bool fail = false;
};

uint64_t clock_now(void *user)
{
	return static_cast<test_clock *>(user)->now;
}

bool clock_sleep(void *user, uint64_t deadline)
{
	auto *state = static_cast<test_clock *>(user);

	state->deadlines.push_back(deadline);
	return !state->fail;
}

TEST(CliTiming, UsesAbsoluteDeadlinesAndRejectsRegressionsAndOverflow)
{
	test_clock state = {5000, {}};
	cli_clock clock = {clock_now, clock_sleep, &state, 0, 0, 0, false};

	ASSERT_TRUE(cli_schedule(&clock, 100, true));
	EXPECT_TRUE(state.deadlines.empty());
	state.now = 5020;
	ASSERT_TRUE(cli_schedule(&clock, 150, true));
	ASSERT_TRUE(cli_schedule(&clock, 180, true));
	EXPECT_EQ((std::vector<uint64_t>{5050, 5080}), state.deadlines);
	EXPECT_FALSE(cli_schedule(&clock, 179, true));
	EXPECT_TRUE(cli_schedule(&clock, 180, false));
	EXPECT_FALSE(cli_schedule(&clock, 179, false));
	state.now = UINT64_MAX - 10;
	clock = {clock_now, clock_sleep, &state, 0, 0, 0, false};
	ASSERT_TRUE(cli_schedule(&clock, 1, true));
	EXPECT_FALSE(cli_schedule(&clock, 12, true));
	state.now = 0;
	state.fail = true;
	clock = {clock_now, clock_sleep, &state, 0, 0, 0, false};
	ASSERT_TRUE(cli_schedule(&clock, 0, true));
	EXPECT_FALSE(cli_schedule(&clock, 1, true));
}

TEST(CliJson, DecodedPduSurvivesJsonAndCoreEncoder)
{
	cli_fixture fixture;

	fixture.field.number_of_bits = 8;
	auto options = vmf_options_for_schemas(&fixture.schemas);
	vmf_decoder *reader = nullptr;
	vmf_encoder *writer = nullptr;
	vmf_encoder *source = nullptr;

	ASSERT_EQ(VMF_OK, vmf_decoder_create(&options, &reader, nullptr));
	ASSERT_EQ(VMF_OK, vmf_encoder_create(&options, false,
		&writer, nullptr));
	ASSERT_EQ(VMF_OK, vmf_encoder_create(&options, false,
		&source, nullptr));
	vmf_message_header message = {};
	vmf_header_d1 header = {};

	message.id = fixture.id;
	message.has_body = true;
	message.message_size_octets = {true, 1};
	header.fields.messages = &message;
	header.fields.message_count = 1;
	std::vector<uint8_t> original;
	vmf_event event = {};

	event.kind = VMF_EVENT_ENVELOPE;
	event.as.envelope = {4, &header, sizeof(header),
		VMF_LENGTH_UNKNOWN, VMF_LENGTH_UNKNOWN};
	fixtures::write(source, &event, &original);
	event = {};
	event.kind = VMF_EVENT_MESSAGE_BEGIN;
	event.as.message = {fixture.id, 0, 1};
	fixtures::write(source, &event, &original);
	event = {};
	event.kind = VMF_EVENT_GROUP_BEGIN;
	event.scope = {VMF_SCOPE_ROOT, 0};
	event.as.group.count = 1;
	fixtures::write(source, &event, &original);
	event = field_event(&fixture, VMF_FIELD_UNSIGNED);
	fixture.field.number_of_bits = 8;
	event.occurrence = 0;
	event.as.field.value.unsigned_integer.value = 90;
	fixtures::write(source, &event, &original);
	event = {};
	event.kind = VMF_EVENT_GROUP_END;
	event.scope = {VMF_SCOPE_ROOT, 0};
	event.as.group.count = 1;
	fixtures::write(source, &event, &original);
	event = {};
	event.kind = VMF_EVENT_MESSAGE_END;
	event.as.message = {fixture.id, 0, 1};
	fixtures::write(source, &event, &original);
	event = {};
	event.kind = VMF_EVENT_PDU_END;
	fixtures::write(source, &event, &original);
	ASSERT_EQ(VMF_OK, vmf_decoder_begin_pdu(reader));
	vmf_input input = {original.data(), original.size(), 0, true};
	std::vector<uint8_t> encoded;

	for (size_t attempts = 0; attempts < 100; ++attempts) {
		vmf_event decoded = {};
		vmf_status status = vmf_decoder_next(reader, &input, &decoded);

		if (status == VMF_DONE)
			break;
		ASSERT_EQ(VMF_OK, status);
		vmf_event replayed = {};

		ASSERT_EQ(VMF_OK, round_trip(&fixture, &decoded, &replayed));
		fixtures::write(writer, &replayed, &encoded);
	}
	EXPECT_EQ(original, encoded);
	vmf_encoder_destroy(source);
	vmf_encoder_destroy(writer);
	vmf_decoder_destroy(reader);
}

} /* namespace */
