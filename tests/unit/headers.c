#include <vmf/wire.h>

/* Compile and link a C11 caller against the C++17 library. */
int vmf_c_consumer(void)
{
	vmf_limits limits = vmf_default_limits();

	return limits.field_chunk_bytes >= 4 &&
	       vmf_header_layout(VMF_ENVELOPE_B).size == sizeof(vmf_header_b);
}
