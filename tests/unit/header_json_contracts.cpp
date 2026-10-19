#include <cstdlib>
#include <cstring>
#include <gtest/gtest.h>
#include <string>
#include <vmf/header_json.hpp>

static const char minimal_c[] =
	"{\"fields\":{\"messages\":[{\"id\":{\"user_message_format\":2,"
	"\"standard_version\":0,\"identification\":{\"functional_area\":1,"
	"\"message_number\":1}}}]}}";

TEST(HeaderJson, OmittedOptionalPropertiesUseValidProfileDefaults)
{
	auto options = vmf::header_json::default_options();
	vmf::header_json::document *document = nullptr;
	vmf::header_json::error error;

	ASSERT_EQ(VMF_OK, vmf::header_json::load(VMF_ENVELOPE_C, minimal_c,
						 strlen(minimal_c), &options,
						 &document, &error));
	ASSERT_NE(nullptr, vmf::header_json::header(document));
	const auto &fields = vmf::header_json::header(document)->c.fields;

	ASSERT_EQ(1u, fields.message_count);
	EXPECT_EQ(2u, fields.messages[0].precedence);
	EXPECT_FALSE(fields.originator.present);
	EXPECT_FALSE(fields.header_size_octets.present);
	EXPECT_FALSE(fields.messages[0].file_name.present);
	EXPECT_TRUE(fields.messages[0].has_body);
	EXPECT_EQ(VMF_ENVELOPE_C, vmf::header_json::profile(document));
	EXPECT_EQ(VMF_HEADER_JSON_NONE, error.reason);
	EXPECT_STREQ("", error.path);
	vmf::header_json::destroy(document);
}

TEST(HeaderJson, ErrorsHaveStablePathsAndOwnersClear)
{
	auto options = vmf_header_json_default_options();
	const char *inputs[] = {
	    "{\"fields\":{\"messages\":[{}]}}",
	    "{\"fields\":{\"messages\":[],\"messages\":[]}}",
	    "{\"fields\":{\"header_size_octets\":null,\"messages\":[]}}",
	    "{\"fields\":{\"messages\":[{\"id\":false}]}}"};
	const char *paths[] = {"/fields/messages/0/id", "/fields/messages",
			       "/fields/header_size_octets",
			       "/fields/messages/0/id"};
	vmf_header_json_reason reasons[] = {
	    VMF_HEADER_JSON_MISSING, VMF_HEADER_JSON_DUPLICATE,
	    VMF_HEADER_JSON_PROPERTY, VMF_HEADER_JSON_TYPE};

	for (size_t i = 0; i < 4; ++i) {
		vmf_header_json_document *document = nullptr;
		vmf_header_json_error error;

		EXPECT_EQ(VMF_MALFORMED,
			  vmf_header_json_load(VMF_ENVELOPE_B, inputs[i],
					       strlen(inputs[i]), &options,
					       &document, &error));
		EXPECT_EQ(nullptr, document);
		EXPECT_EQ(reasons[i], error.reason);
		EXPECT_STREQ(paths[i], error.path);
	}
}

extern "C" int vmf_header_json_c_consumer(void);

TEST(HeaderJson, C11ConsumerAndNullSafeCleanup)
{
	EXPECT_TRUE(vmf_header_json_c_consumer());
	EXPECT_EQ(nullptr, vmf_header_json_header(nullptr));
	EXPECT_EQ(0, vmf_header_json_profile(nullptr));
	EXPECT_EQ(nullptr, vmf_header_json_text_data(nullptr));
	EXPECT_EQ(0u, vmf_header_json_text_size(nullptr));
	vmf_header_json_destroy(nullptr);
	vmf_header_json_text_destroy(nullptr);
}

TEST(HeaderJson, MinimalTemplatesAreExactAndLoadForEveryProfile)
{
	vmf_message_id ids[2] = {};

	for (unsigned p = VMF_ENVELOPE_B; p <= VMF_ENVELOPE_D1; ++p) {
		auto profile = static_cast<vmf_envelope_version>(p);
		auto options = vmf_header_json_default_options();

		for (auto &id : ids) {
			id.user_message_format = 2;
			id.identification_present = true;
			id.functional_area = 1;
			id.message_number = 1;
			id.standard_version = {p != VMF_ENVELOPE_B, 0};
		}
		ids[1].message_number = 2;
		for (size_t count : {1u, 2u}) {
			vmf_header_json_text *text = nullptr;
			vmf_header_json_error error;

			ASSERT_EQ(VMF_OK, vmf_header_json_template(
					      profile, ids, count, &options,
					      &text, &error))
			    << error.message << " " << error.path;
			const char *data = vmf_header_json_text_data(text);
			size_t size = vmf_header_json_text_size(text);
			vmf_header_json_document *document = nullptr;

			EXPECT_EQ(size, strlen(data));
			EXPECT_EQ(nullptr, strstr(data, "originator"));
			ASSERT_EQ(VMF_OK, vmf_header_json_load(
					      profile, data, size, &options,
					      &document, &error));
			const auto &fields =
			    vmf_header_json_header(document)->b.fields;

			ASSERT_EQ(count, fields.message_count);
			for (size_t i = 0; i < count; ++i) {
				EXPECT_TRUE(vmf_message_id_equal(
				    &ids[i], &fields.messages[i].id));
				EXPECT_EQ(count > 1,
					  fields.messages[i]
					      .message_size_octets.present);
			}
			vmf_header_json_text_destroy(text);
			vmf_header_json_destroy(document);
		}
	}
}

TEST(HeaderJson, RejectsMalformedTypesNativeRangesAndProfileProperties)
{
	auto options = vmf_header_json_default_options();
	std::string base = minimal_c;
	struct example {
		const char *before;
		const char *after;
		vmf_status status;
		const char *path;
	};
	const example cases[] = {
	    {"\"messages\":[", "\"messages\":false,\"unused\":[", VMF_MALFORMED,
	     "/fields/unused"},
	    {"\"standard_version\":0", "\"standard_version\":256",
	     VMF_MALFORMED, "/fields/messages/0/id/standard_version"},
	    {"\"id\":{", "\"classification\":7,\"id\":{", VMF_OUT_OF_RANGE,
	     "/fields/messages/0"},
	    {"\"id\":{", "\"markings\":[],\"id\":{", VMF_MALFORMED,
	     "/fields/messages/0/markings"},
	    {"\"id\":{", "\"operation\":null,\"id\":{", VMF_MALFORMED,
	     "/fields/messages/0/operation"}};

	for (const auto &test : cases) {
		std::string input = base;
		size_t position = input.find(test.before);

		ASSERT_NE(std::string::npos, position);
		input.replace(position, strlen(test.before), test.after);
		vmf_header_json_document *document = nullptr;
		vmf_header_json_error error;

		EXPECT_EQ(test.status,
			  vmf_header_json_load(VMF_ENVELOPE_C, input.data(),
					       input.size(), &options,
					       &document, &error));
		EXPECT_EQ(nullptr, document);
		EXPECT_STREQ(test.path, error.path);
	}
	const char invalid_utf8[] = {'{', '"', '\xff', '"', ':', '0', '}'};
	vmf_header_json_document *document = nullptr;
	vmf_header_json_error error;

	EXPECT_EQ(VMF_MALFORMED,
		  vmf_header_json_load(VMF_ENVELOPE_C, invalid_utf8,
				       sizeof(invalid_utf8), &options,
				       &document, &error));
	EXPECT_EQ(VMF_HEADER_JSON_SYNTAX, error.reason);
	EXPECT_NE(SIZE_MAX, error.byte_offset);
	EXPECT_EQ(VMF_TRUNCATED,
		  vmf_header_json_load(VMF_ENVELOPE_C, "{", 1, &options,
				       &document, &error));
	EXPECT_EQ(1u, error.byte_offset);
}

struct header_allocations {
	size_t calls = 0;
	size_t fail_at = SIZE_MAX;
	size_t live = 0;
};

static void *header_allocate(void *user, size_t size)
{
	auto *state = static_cast<header_allocations *>(user);

	if (state->calls++ == state->fail_at)
		return nullptr;
	void *memory = malloc(size);

	if (memory)
		++state->live;
	return memory;
}

static void header_deallocate(void *user, void *memory)
{
	auto *state = static_cast<header_allocations *>(user);

	if (memory)
		--state->live;
	free(memory);
}

TEST(HeaderJson, AllocationFailuresDistinguishBudgetsAndReleaseEveryOwner)
{
	header_allocations state;
	vmf_allocator allocator = {header_allocate, header_deallocate, &state};
	auto options = vmf_header_json_default_options();

	options.allocator = &allocator;
	vmf_header_json_document *document = nullptr;
	vmf_header_json_error error;

	ASSERT_EQ(VMF_OK, vmf_header_json_load(VMF_ENVELOPE_C, minimal_c,
					       strlen(minimal_c), &options,
					       &document, &error));
	size_t load_calls = state.calls;

	vmf_header_json_destroy(document);
	ASSERT_EQ(0u, state.live);
	for (size_t i = 0; i < load_calls; ++i) {
		state = {};
		state.fail_at = i;
		document = nullptr;
		EXPECT_EQ(VMF_OUT_OF_MEMORY,
			  vmf_header_json_load(VMF_ENVELOPE_C, minimal_c,
					       strlen(minimal_c), &options,
					       &document, &error));
		EXPECT_EQ(nullptr, document);
		EXPECT_EQ(VMF_HEADER_JSON_ALLOCATION, error.reason);
		EXPECT_EQ(0u, state.live);
	}
	state = {};
	options.max_memory_bytes = 1;
	EXPECT_EQ(VMF_LIMIT_EXCEEDED,
		  vmf_header_json_load(VMF_ENVELOPE_C, minimal_c,
				       strlen(minimal_c), &options, &document,
				       &error));
	EXPECT_EQ(VMF_HEADER_JSON_LIMIT, error.reason);
	EXPECT_EQ(0u, state.live);
	options = vmf_header_json_default_options();
	options.max_json_bytes = strlen(minimal_c) - 1;
	EXPECT_EQ(VMF_LIMIT_EXCEEDED,
		  vmf_header_json_load(VMF_ENVELOPE_C, minimal_c,
				       strlen(minimal_c), &options, &document,
				       &error));
	allocator.deallocate = nullptr;
	options = vmf_header_json_default_options();
	options.allocator = &allocator;
	EXPECT_EQ(VMF_INVALID_ARGUMENT,
		  vmf_header_json_load(VMF_ENVELOPE_C, minimal_c,
				       strlen(minimal_c), &options, &document,
				       &error));
}

TEST(HeaderJson, SerializedTextOwnsBytesAndAllAllocationFailuresCleanUp)
{
	auto options = vmf_header_json_default_options();
	vmf_header_json_document *document = nullptr;

	ASSERT_EQ(VMF_OK, vmf_header_json_load(VMF_ENVELOPE_C, minimal_c,
					       strlen(minimal_c), &options,
					       &document, nullptr));
	header_allocations state;
	vmf_allocator allocator = {header_allocate, header_deallocate, &state};

	options.allocator = &allocator;
	vmf_header_json_text *text = nullptr;
	vmf_header_json_error error;

	ASSERT_EQ(VMF_OK, vmf_header_json_serialize(
			      VMF_ENVELOPE_C, vmf_header_json_header(document),
			      &options, &text, &error));
	size_t calls = state.calls;
	std::string expected = vmf_header_json_text_data(text);

	vmf_header_json_text_destroy(text);
	EXPECT_EQ(0u, state.live);
	for (size_t i = 0; i < calls; ++i) {
		state = {};
		state.fail_at = i;
		text = nullptr;
		EXPECT_EQ(VMF_OUT_OF_MEMORY,
			  vmf_header_json_serialize(
			      VMF_ENVELOPE_C, vmf_header_json_header(document),
			      &options, &text, &error));
		EXPECT_EQ(nullptr, text);
		EXPECT_EQ(0u, state.live);
	}
	state = {};
	ASSERT_EQ(VMF_OK, vmf_header_json_serialize(
			      VMF_ENVELOPE_C, vmf_header_json_header(document),
			      &options, &text, &error));
	vmf_header_json_destroy(document);
	EXPECT_EQ(expected, vmf_header_json_text_data(text));
	vmf_header_json_text_destroy(text);
	EXPECT_EQ(0u, state.live);
}

TEST(HeaderJson, EveryPopulatedHeaderValueSurvivesJsonAndPublicWireDecoding)
{
	const uint8_t material[8] = {0, 1, 2, 3, 4, 5, 0x80, 0xff};
	vmf_header_bytes tokens[] = {{true, material, 8}, {true, material, 8}};
	uint16_t markings[] = {0, 1, 511};
	vmf_address recipients[] = {{true, {true, 2}, {}},
				    {true, {}, {true, {"recipient", 9}}}};
	vmf_address information = {true, {true, 3}, {}};
	vmf_reference references[2] = {};
	auto options = vmf_header_json_default_options();

	for (unsigned p = VMF_ENVELOPE_B; p <= VMF_ENVELOPE_D1; ++p) {
		auto profile = static_cast<vmf_envelope_version>(p);
		vmf_header header = {};
		vmf_message_header message = {};
		auto &fields = header.b.fields;

		fields.originator = {true, {}, {true, {"origin", 6}}};
		fields.compression = {true, 0};
		fields.header_size_octets = {p != VMF_ENVELOPE_B, 0};
		fields.recipients = recipients;
		fields.recipient_count = 2;
		fields.information_addressees = &information;
		fields.information_addressee_count = 1;
		fields.messages = &message;
		fields.message_count = 1;
		message.id = {2,	{p != VMF_ENVELOPE_B, 0}, true, 2, 1,
			      {true, 3}};
		message.file_name = {true, {"file", 4}};
		message.message_size_octets = {true, 3};
		message.has_body = true;
		message.operation = 2;
		message.retransmit = true;
		message.precedence = p <= VMF_ENVELOPE_C ? 7 : 5;
		message.classification = 3;
		message.originator_dtg = {true, 26, 10, 15,
					  12,	30, 59, {true, 4095}};
		message.perishability = message.originator_dtg;
		message.perishability.extension = {};
		message.acknowledgment = {true, true, false, true};
		message.references = references;
		message.reference_count = 2;
		for (auto &reference : references) {
			reference = {};
			reference.originator = {true, {true, 4}, {}};
			reference.dtg = message.originator_dtg;
			if (p == VMF_ENVELOPE_B) {
				reference.functional_area = 2;
				reference.message_number = 9;
			}
		}
		if (p == VMF_ENVELOPE_B)
			message.marking_b = {true, 123};
		else if (p == VMF_ENVELOPE_C)
			message.marking_c = {true, {"marking", 7}};
		else {
			message.markings = markings;
			message.marking_count = 3;
		}
		if (p != VMF_ENVELOPE_B) {
			message.security.present = true;
			message.security.spi = 3;
			message.security.keying_material = {true, material, 3};
			message.security.initialization = {true, material, 8};
			message.security.tokens_present = true;
			message.security.tokens = tokens;
			message.security.token_count = 2;
			message.security.authentication_a = {true, material, 8};
			message.security.signed_ack_request = true;
			message.security.padding_present = true;
			message.security.padding_length = 3;
			message.security.padding = {true, material, 3};
		}
		vmf_header_json_text *first = nullptr;
		vmf_header_json_error error;

		ASSERT_EQ(VMF_OK,
			  vmf_header_json_serialize(profile, &header, &options,
						    &first, &error))
		    << error.path;
		vmf_header_json_document *document = nullptr;

		ASSERT_EQ(VMF_OK, vmf_header_json_load(
				      profile, vmf_header_json_text_data(first),
				      vmf_header_json_text_size(first),
				      &options, &document, &error))
		    << error.path;
		vmf_header_json_text *second = nullptr;

		ASSERT_EQ(VMF_OK, vmf_header_json_serialize(
				      profile, vmf_header_json_header(document),
				      &options, &second, &error));
		EXPECT_STREQ(vmf_header_json_text_data(first),
			     vmf_header_json_text_data(second));
		vmf_header_json_text_destroy(first);
		vmf_header_json_text_destroy(second);
		vmf_options codec = {};

		codec.profile = profile;
		codec.limits = vmf_default_limits();
		vmf_encoder *encoder = nullptr;

		ASSERT_EQ(VMF_OK,
			  vmf_encoder_create(&codec, true, &encoder, nullptr));
		ASSERT_EQ(VMF_OK, vmf_encoder_begin_envelope(encoder));
		vmf_event event = {};

		event.kind = VMF_EVENT_ENVELOPE;
		event.as.envelope = {static_cast<uint8_t>(profile), &header,
				     vmf_header_layout(profile).size, 0,
				     VMF_LENGTH_UNKNOWN};
		ASSERT_EQ(VMF_OK, vmf_encoder_write(encoder, nullptr, &event));
		size_t size = vmf_encoder_bit_count(encoder) / 8;

		vmf_encoder_destroy(encoder);
		fields.header_size_octets.value = static_cast<uint32_t>(size);
		ASSERT_EQ(VMF_OK,
			  vmf_encoder_create(&codec, false, &encoder, nullptr));
		ASSERT_EQ(VMF_OK, vmf_encoder_begin_envelope(encoder));
		std::string bytes(size, '\0');
		vmf_output output = {reinterpret_cast<uint8_t *>(&bytes[0]),
				     bytes.size(), 0};

		ASSERT_EQ(VMF_OK, vmf_encoder_write(encoder, &output, &event));
		vmf_encoder_destroy(encoder);
		vmf_decoder *decoder = nullptr;

		ASSERT_EQ(VMF_OK,
			  vmf_decoder_create(&codec, &decoder, nullptr));
		ASSERT_EQ(VMF_OK, vmf_decoder_begin_envelope(decoder));
		vmf_input input = {
		    reinterpret_cast<const uint8_t *>(bytes.data()),
		    bytes.size(), 0, true};
		vmf_event decoded;

		ASSERT_EQ(VMF_OK, vmf_decoder_next(decoder, &input, &decoded));
		ASSERT_EQ(VMF_OK,
			  vmf_header_json_serialize(profile, &header, &options,
						    &first, &error));
		ASSERT_EQ(VMF_OK, vmf_header_json_serialize(
				      profile, decoded.as.envelope.header,
				      &options, &second, &error));
		EXPECT_STREQ(vmf_header_json_text_data(first),
			     vmf_header_json_text_data(second));
		vmf_header_json_text_destroy(first);
		vmf_header_json_text_destroy(second);
		vmf_decoder_destroy(decoder);
		vmf_header_json_destroy(document);
	}
}

TEST(HeaderJson, ResponseMetadataAndBothAuthenticationSpansRemainRaw)
{
	const uint8_t authentication[8] = {0, 1, 2, 3, 4, 5, 6, 7};
	auto options = vmf_header_json_default_options();
	vmf_message_header message = {};
	vmf_header header = {};
	vmf_address recipient = {true, {true, 2}, {}};

	header.d1.fields.originator = {true, {true, 1}, {}};
	header.d1.fields.recipients = &recipient;
	header.d1.fields.recipient_count = 1;
	header.d1.fields.messages = &message;
	header.d1.fields.message_count = 1;
	message.id = {2, {true, 0}, true, 1, 1, {}};
	message.response.present = true;
	message.response.acknowledged = {true, 26, 10, 15,
					 12,   30, 59, {true, 17}};
	message.response.amplification = {true, {"reply", 5}};
	message.security.present = true;
	message.security.spi = 3;
	message.security.authentication_a = {true, authentication, 8};
	message.security.authentication_b = {true, authentication, 8};
	for (uint8_t rc : {2, 6}) {
		message.response.receipt_compliance = rc;
		message.response.cantpro_reason = {rc == 2, 7};
		message.response.cantco_reason = {rc == 6, 0};
		vmf_header_json_text *text = nullptr;
		vmf_header_json_error error;

		ASSERT_EQ(VMF_OK,
			  vmf_header_json_serialize(VMF_ENVELOPE_D1, &header,
						    &options, &text, &error));
		vmf_header_json_document *document = nullptr;

		ASSERT_EQ(VMF_OK,
			  vmf_header_json_load(VMF_ENVELOPE_D1,
					       vmf_header_json_text_data(text),
					       vmf_header_json_text_size(text),
					       &options, &document, &error));
		const auto &copy =
		    vmf_header_json_header(document)->d1.fields.messages[0];

		EXPECT_FALSE(copy.has_body);
		EXPECT_EQ(rc, copy.response.receipt_compliance);
		EXPECT_EQ(rc == 2, copy.response.cantpro_reason.present);
		EXPECT_EQ(rc == 6, copy.response.cantco_reason.present);
		EXPECT_EQ(26u, copy.response.acknowledged.year);
		EXPECT_EQ(10u, copy.response.acknowledged.month);
		EXPECT_EQ(15u, copy.response.acknowledged.day);
		EXPECT_EQ(12u, copy.response.acknowledged.hour);
		EXPECT_EQ(30u, copy.response.acknowledged.minute);
		EXPECT_EQ(59u, copy.response.acknowledged.second);
		EXPECT_EQ(17u, copy.response.acknowledged.extension.value);
		EXPECT_TRUE(copy.response.acknowledged.present);
		EXPECT_TRUE(copy.response.acknowledged.extension.present);
		if (rc == 2)
			EXPECT_EQ(7u, copy.response.cantpro_reason.value);
		else
			EXPECT_EQ(0u, copy.response.cantco_reason.value);
		vmf_header_json_text *repeated = nullptr;

		ASSERT_EQ(VMF_OK, vmf_header_json_serialize(VMF_ENVELOPE_D1,
			vmf_header_json_header(document), &options, &repeated,
			&error));
		EXPECT_STREQ(vmf_header_json_text_data(text),
			vmf_header_json_text_data(repeated));
		vmf_header_json_text_destroy(repeated);
		EXPECT_EQ("reply",
			  std::string(copy.response.amplification.value.data,
				      copy.response.amplification.value.size));
		EXPECT_EQ(0, memcmp(authentication,
				    copy.security.authentication_b.data, 8));
		vmf_header_json_text_destroy(text);
		vmf_header_json_destroy(document);
	}
}

TEST(HeaderJson, AddressDiagnosticsIdentifyElementAndAggregatePaths)
{
	auto options = vmf_header_json_default_options();
	std::string input = minimal_c;
	size_t position = input.find("\"fields\":{");

	input.insert(position + strlen("\"fields\":{"),
		     "\"recipients\":[null],");
	vmf_header_json_document *document = nullptr;
	vmf_header_json_error error;

	EXPECT_EQ(VMF_MALFORMED, vmf_header_json_load(
				     VMF_ENVELOPE_C, input.data(), input.size(),
				     &options, &document, &error));
	EXPECT_EQ(VMF_HEADER_JSON_TYPE, error.reason);
	EXPECT_STREQ("/fields/recipients/0", error.path);
	input = "{\"fields\":{\"recipients\":[";
	for (unsigned i = 0; i < 9; ++i) {
		if (i)
			input += ',';
		input += "{\"urn\":" + std::to_string(i) + '}';
	}
	input += "],\"information_addressees\":[";
	for (unsigned i = 9; i < 17; ++i) {
		if (i > 9)
			input += ',';
		input += "{\"urn\":" + std::to_string(i) + '}';
	}
	input += "],\"messages\":[]}}";
	EXPECT_EQ(VMF_MALFORMED, vmf_header_json_load(
				     VMF_ENVELOPE_C, input.data(), input.size(),
				     &options, &document, &error));
	EXPECT_EQ(VMF_HEADER_JSON_RANGE, error.reason);
	EXPECT_STREQ("/fields/information_addressees", error.path);
	EXPECT_EQ(nullptr, document);
}

TEST(HeaderJson, ShortenedHeadersNeedNoFullHeaderMembers)
{
	auto options = vmf_header_json_default_options();
	const char input[] =
	    "{\"shortened\":true,\"fields\":{"
	    "\"originator\":{\"urn\":1},\"recipients\":[{\"urn\":2}]}}";

	for (auto profile : {VMF_ENVELOPE_C, VMF_ENVELOPE_D, VMF_ENVELOPE_D1}) {
		vmf_header_json_document *document = nullptr;

		ASSERT_EQ(VMF_OK,
			  vmf_header_json_load(profile, input, strlen(input),
					       &options, &document, nullptr));
		EXPECT_TRUE(vmf_header_json_header(document)->c.shortened);
		EXPECT_EQ(
		    0u,
		    vmf_header_json_header(document)->c.fields.message_count);
		vmf_header_json_destroy(document);
	}
}

TEST(HeaderJson, DiagnosticPathTruncationPreservesUtf8AndMarksNulKeys)
{
	auto options = vmf_header_json_default_options();
	std::string input = "{\"X";

	for (size_t i = 0; i < 300; ++i)
		input += "\xc3\xa9";
	input += "\":false}";
	vmf_header_json_document *document = nullptr;
	vmf_header_json_error error;

	EXPECT_EQ(VMF_MALFORMED, vmf_header_json_load(VMF_ENVELOPE_C,
		input.data(), input.size(), &options, &document, &error));
	EXPECT_EQ(VMF_HEADER_JSON_PROPERTY, error.reason);
	EXPECT_TRUE(error.path_truncated);
	EXPECT_EQ(0u, strlen(error.path) % 2);
	input = "{\"fields\":{\"X\\u0000suffix\":false}}";
	EXPECT_EQ(VMF_MALFORMED, vmf_header_json_load(VMF_ENVELOPE_C,
		input.data(), input.size(), &options, &document, &error));
	EXPECT_EQ(VMF_HEADER_JSON_PROPERTY, error.reason);
	EXPECT_TRUE(error.path_truncated);
	EXPECT_STREQ("/fields/X", error.path);
}
