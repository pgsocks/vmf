#include <gtest/gtest.h>
#include <cucumber-cpp/autodetect.hpp>
#include <cstring>
#include <string>
#include <vmf/header_json.h>

namespace
{

struct header_context {
	vmf_message_id id = {};
	std::string input;
	vmf_status status = VMF_OK;
	vmf_header_json_error error = {};
	size_t matching_templates = 0;
};

GIVEN("^an exact public header identity$")
{
	auto *context = cucumber::ScenarioScope<header_context>().get();

	context->id = {2, {}, true, 1, 1, {true, 3}};
}

WHEN("^I generate and load the public minimal header templates$")
{
	auto *context = cucumber::ScenarioScope<header_context>().get();
	auto options = vmf_header_json_default_options();

	for (unsigned p = VMF_ENVELOPE_B; p <= VMF_ENVELOPE_D1; ++p) {
		auto profile = static_cast<vmf_envelope_version>(p);

		context->id.standard_version = {p != VMF_ENVELOPE_B, 0};
		vmf_header_json_text *text = nullptr;

		ASSERT_EQ(VMF_OK, vmf_header_json_template(
				      profile, &context->id, 1, &options, &text,
				      &context->error));
		vmf_header_json_document *document = nullptr;

		ASSERT_EQ(VMF_OK, vmf_header_json_load(
				      profile, vmf_header_json_text_data(text),
				      vmf_header_json_text_size(text), &options,
				      &document, &context->error));
		const auto &fields = vmf_header_json_header(document)->b.fields;

		if (fields.message_count == 1 &&
		    vmf_message_id_equal(&context->id, &fields.messages[0].id))
			++context->matching_templates;
		vmf_header_json_destroy(document);
		vmf_header_json_text_destroy(text);
	}
}

THEN("^every loaded template preserves that identity$")
{
	auto *context = cucumber::ScenarioScope<header_context>().get();

	EXPECT_EQ(4u, context->matching_templates);
}

GIVEN("^a B header with a later-profile property$")
{
	auto *context = cucumber::ScenarioScope<header_context>().get();

	context->input = "{\"fields\":{\"header_size_octets\":null}}";
}

WHEN("^I load that header through the public JSON companion$")
{
	auto *context = cucumber::ScenarioScope<header_context>().get();
	auto options = vmf_header_json_default_options();
	vmf_header_json_document *document = nullptr;

	context->status = vmf_header_json_load(
	    VMF_ENVELOPE_B, context->input.data(), context->input.size(),
	    &options, &document, &context->error);
	EXPECT_EQ(nullptr, document);
	vmf_header_json_destroy(document);
}

THEN("^the public header error identifies the incompatible property$")
{
	auto *context = cucumber::ScenarioScope<header_context>().get();

	EXPECT_EQ(VMF_MALFORMED, context->status);
	EXPECT_EQ(VMF_HEADER_JSON_PROPERTY, context->error.reason);
	EXPECT_STREQ("/fields/header_size_octets", context->error.path);
}

} /* namespace */
