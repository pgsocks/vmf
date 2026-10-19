#ifndef VMF_PRIVATE_SCHEMA_VALIDATION_HPP
#define VMF_PRIVATE_SCHEMA_VALIDATION_HPP

#include <vmf/wire.h>
#include "runtime_schema.hpp"

namespace vmf_internal
{
bool message_version_valid(vmf_envelope_version profile,
	const vmf_message_id *id);
vmf_status normalize_schemas(const vmf_schema_set *set,
	const vmf_limits *limits, const vmf_allocator *allocator,
	vmf_runtime_set *result, vmf_error *error);
void destroy_normalized_set(vmf_runtime_set *set,
	const vmf_allocator *allocator);

/* Shared native validation with explicit allocation, without codec state. */
vmf_status validate_schemas_allocated(const vmf_schema_set *set,
	const vmf_limits *limits, const vmf_allocator *allocator,
	vmf_error *error);

} /* namespace vmf_internal */
#endif /* VMF_PRIVATE_SCHEMA_VALIDATION_HPP */
