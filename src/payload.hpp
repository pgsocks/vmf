#ifndef VMF_PRIVATE_PAYLOAD_HPP
#define VMF_PRIVATE_PAYLOAD_HPP

#include "runtime_schema.hpp"

namespace vmf_internal
{

struct payload_state;

/* Caller provides max_align_t-aligned creation-time storage of this size. */
vmf_status payload_storage_size(const vmf_limits *limits, size_t *size);
vmf_status payload_init(
	void *memory, const vmf_limits *limits, payload_state **result);
void payload_begin(payload_state *state, const vmf_runtime_schema *schema,
	uint64_t payload_octets, uint64_t body_start_bit, bool counting,
	uint64_t max_octets = VMF_LENGTH_UNKNOWN);
/* Decode returns DONE only after length and zero-padding validation. */
vmf_status runtime_payload_decode(
	payload_state *state, vmf_input *input, vmf_runtime_event *event);
/* MESSAGE_END is a finish sentinel, permitting resumable padding output. */
vmf_status runtime_payload_encode(payload_state *state, vmf_output *output,
	const vmf_runtime_event *event, bool counting);
/* Public event bridge; all identities refer to borrowed canonical schemas. */
vmf_status payload_decode(
	payload_state *state, vmf_input *input, vmf_event *event);
vmf_status payload_encode(payload_state *state, vmf_output *output,
	const vmf_event *event, bool counting);
/* Managed traversal yields only application decisions. event_limit reserves
 * message boundaries outside this layer; events reports all progress.
 * The caller supplies creation-time path storage, retained through the reply.
 */
vmf_status payload_managed_next(payload_state *state, vmf_output *output,
	vmf_encode_request *request, vmf_repeat_occurrence *path,
	size_t path_capacity, uint64_t event_limit, uint64_t *events);
vmf_status payload_managed_respond(payload_state *state, vmf_output *output,
	const vmf_encode_reply *reply, uint64_t event_limit, uint64_t *events);
const vmf_runtime_schema *payload_schema(const payload_state *state);
const vmf_node *payload_event_node(
	const payload_state *state, const vmf_event *event);
uint64_t payload_bits(const payload_state *state);
vmf_node_id payload_error_node(const payload_state *state);
bool payload_complete(const payload_state *state);
/* Current expected field context for the public scalar/chunk convenience. */
vmf_status payload_field_context(const payload_state *state,
	const vmf_node *node, size_t *depth, uint64_t *occurrence,
	uint64_t *offset);

} /* namespace vmf_internal */
#endif
