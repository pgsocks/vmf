#ifndef VMF_AUTHOR_HPP_INCLUDED
#define VMF_AUTHOR_HPP_INCLUDED

#include <vmf/author.h>

namespace vmf
{
using author_address = ::vmf_author_address;
using scope_info = ::vmf_scope_info;
using author_field = ::vmf_author_field;
using author_control = ::vmf_author_control;
using author_options = ::vmf_author_options;
using author_error = ::vmf_author_error;
using author_result = ::vmf_author_result;

inline vmf_status schema_scope_info(const vmf_schema_set *set,
	const vmf_message_schema *message, vmf_scope_ref scope,
	vmf_scope_info *result)
{
	return ::vmf_schema_scope_info(set, message, scope, result);
}
inline vmf_status author_validate_address(const vmf_schema_set *set,
	const vmf_author_address *address)
{
	return ::vmf_author_validate_address(set, address);
}
inline vmf_status author_validate_field(const vmf_schema_set *set,
	const vmf_author_field *field, uint64_t *units = nullptr)
{
	return ::vmf_author_validate_field(set, field, units);
}
inline vmf_status author_validate_control(const vmf_schema_set *set,
	const vmf_author_control *control)
{
	return ::vmf_author_validate_control(set, control);
}
inline vmf_status author_validate_header(const vmf_author_options *options,
	const vmf_envelope_event *envelope, vmf_author_error *error = nullptr)
{
	return ::vmf_author_validate_header(options, envelope, error);
}
inline vmf_author_options author_options_for_schemas(const vmf_schema_set *set)
{
	return ::vmf_author_options_for_schemas(set);
}
inline vmf_status author_encode(const vmf_author_options *options,
	const vmf_envelope_event *envelope, const vmf_author_field *fields,
	size_t field_count, const vmf_author_control *controls,
	size_t control_count, vmf_author_result **result,
	vmf_author_error *error = nullptr)
{
	return ::vmf_author_encode(options, envelope, fields, field_count,
		controls, control_count, result, error);
}
inline const uint8_t *author_result_data(const vmf_author_result *result)
{
	return ::vmf_author_result_data(result);
}
inline size_t author_result_size(const vmf_author_result *result)
{
	return ::vmf_author_result_size(result);
}
inline size_t author_result_descriptor_count(const vmf_author_result *result)
{
	return ::vmf_author_result_descriptor_count(result);
}
inline vmf_status author_result_measurement(const vmf_author_result *result,
	size_t descriptor, vmf_measurement *measurement)
{
	return ::vmf_author_result_measurement(result, descriptor, measurement);
}
inline bool author_result_field_active(const vmf_author_result *result,
	size_t entry)
{
	return ::vmf_author_result_field_active(result, entry);
}
inline bool author_result_control_active(const vmf_author_result *result,
	size_t entry)
{
	return ::vmf_author_result_control_active(result, entry);
}
inline void author_result_destroy(vmf_author_result *result)
{
	::vmf_author_result_destroy(result);
}
} /* namespace vmf */
#endif
