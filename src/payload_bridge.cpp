#include "payload.hpp"
#include <cstring>

namespace vmf_internal
{

static bool same_scope(vmf_scope_ref a, vmf_scope_ref b)
{
	return a.kind == b.kind && a.id == b.id;
}

const vmf_node *payload_event_node(
	const payload_state *state, const vmf_event *event)
{
	const vmf_runtime_schema *schema = payload_schema(state);

	if (!schema || !event)
		return nullptr;
	for (size_t i = 0; i < schema->binding_count; ++i) {
		const vmf_node *node = schema->bindings[i];

		if (event->index) {
			if (node->source == event->index &&
				node->kind == VMF_NODE_FIELD &&
				event->scope.kind == VMF_SCOPE_NONE)
				return node;
		} else if (node->kind != VMF_NODE_FIELD &&
			same_scope(node->scope, event->scope)) {
			return node;
		}
	}
	return nullptr;
}

vmf_status payload_decode(
	payload_state *state, vmf_input *input, vmf_event *event)
{
	vmf_runtime_event native = {};
	vmf_status status = runtime_payload_decode(state, input, &native);

	if (status != VMF_OK)
		return status;
	*event = {};
	event->kind = native.kind;
	event->schema = native.schema->source;
	event->depth = native.depth;
	event->occurrence = native.occurrence;
	if (native.node->kind == VMF_NODE_FIELD) {
		event->index = native.node->source;
		event->field_kind = native.node->as.field.kind;
	} else {
		event->scope = native.node->scope;
	}
	std::memcpy(&event->as, &native.as, sizeof(event->as));
	return VMF_OK;
}

vmf_status payload_encode(payload_state *state, vmf_output *output,
	const vmf_event *event, bool counting)
{
	vmf_runtime_event native = {};
	const vmf_runtime_schema *schema = payload_schema(state);

	if (!event || !schema)
		return VMF_INVALID_ARGUMENT;
	native.kind = event->kind;
	native.schema = schema;
	native.node = payload_event_node(state, event);
	native.depth = event->depth;
	native.occurrence = event->occurrence;
	if (event->kind != VMF_EVENT_MESSAGE_END && !native.node)
		return VMF_TYPE_MISMATCH;
	if (event->kind == VMF_EVENT_FIELD &&
		(native.node->kind != VMF_NODE_FIELD ||
			event->field_kind != native.node->as.field.kind))
		return VMF_TYPE_MISMATCH;
	std::memcpy(&native.as, &event->as, sizeof(native.as));
	return runtime_payload_encode(state, output, &native, counting);
}

} /* namespace vmf_internal */
