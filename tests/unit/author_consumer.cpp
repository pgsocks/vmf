#include <vmf/author.hpp>

/* C++17 procedural forwards include every authoring operation. */
int vmf_author_cpp_consumer()
{
	vmf::author_result *result = nullptr;
	vmf::author_error error = {};
	vmf::author_field field = {};
	vmf::author_control control = {};
	vmf::scope_info info = {};
	vmf::author_options options = vmf::author_options_for_schemas(nullptr);
	vmf_measurement measured = {};

	vmf::author_result_destroy(result);
	return vmf::schema_scope_info(nullptr, nullptr, {}, &info) ==
			VMF_INVALID_ARGUMENT &&
		vmf::author_validate_address(nullptr, &field.address) ==
			VMF_INVALID_ARGUMENT &&
		vmf::author_validate_field(nullptr, &field) ==
			VMF_INVALID_ARGUMENT &&
		vmf::author_validate_control(nullptr, &control) ==
			VMF_INVALID_ARGUMENT &&
		vmf::author_validate_header(&options, nullptr, &error) ==
			VMF_INVALID_ARGUMENT &&
		vmf::author_encode(&options, nullptr, nullptr, 0, nullptr, 0,
			&result, &error) == VMF_INVALID_ARGUMENT &&
		vmf::author_result_data(result) == nullptr &&
		vmf::author_result_size(result) == 0 &&
		vmf::author_result_descriptor_count(result) == 0 &&
		vmf::author_result_measurement(result, 0, &measured) ==
			VMF_INVALID_ARGUMENT &&
		!vmf::author_result_field_active(result, 0) &&
		!vmf::author_result_control_active(result, 0);
}
