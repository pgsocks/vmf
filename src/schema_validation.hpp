#ifndef VMF_PRIVATE_SCHEMA_VALIDATION_HPP
#define VMF_PRIVATE_SCHEMA_VALIDATION_HPP

#include <vmf/wire.h>

namespace vmf_internal
{

/* Shared native validation with explicit allocation, without codec state. */
vmf_status validate_schemas_allocated(const vmf_schema_set *set,
	const vmf_limits *limits, const vmf_allocator *allocator,
	vmf_error *error);

} /* namespace vmf_internal */
#endif /* VMF_PRIVATE_SCHEMA_VALIDATION_HPP */
