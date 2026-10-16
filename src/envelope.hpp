#ifndef VMF_PRIVATE_ENVELOPE_HPP
#define VMF_PRIVATE_ENVELOPE_HPP

#include <vmf/wire.h>

namespace vmf_internal
{

/* Parse one padded header without reading a body. NEED_INPUT reports the next
 * absolute prefix size worth retrying; known spans are requested in one piece.
 * Successful objects and spans live in the supplied arena. No allocation.
 */
vmf_status parse_header(vmf_envelope_version profile, const uint8_t *data,
	size_t size, bool final, void *arena, size_t capacity,
	const vmf_limits *limits, bool counting, vmf_envelope_event *event,
	size_t *needed, vmf_error *error);

/* Serialize into private staging storage. Failure returns zero size. Counting
 * preserves size presence but writes zero placeholders for numeric sizes.
 */
vmf_status serialize_header(vmf_envelope_version profile,
	const vmf_envelope_event *event, bool counting, uint8_t *data,
	size_t capacity, size_t *size, const vmf_limits *limits,
	vmf_error *error);

} /* namespace vmf_internal */
#endif
