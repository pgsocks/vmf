#include "codec.hpp"
#include <cstring>

using namespace vmf_internal;

vmf_status vmf_decoder_dispatch(vmf_decoder *decoder, vmf_input *input,
	vmf_event_handler handler, void *user, size_t *delivered)
{
	if (delivered)
		*delivered = 0;
	if (!decoder || !input || !handler || !delivered)
		return VMF_INVALID_ARGUMENT;
	for (;;) {
		vmf_event event = {};
		vmf_status status = vmf_decoder_next(decoder, input, &event);

		if (status != VMF_OK)
			return status;
		++*delivered;
		vmf_event_action action = handler(user, &event);

		if (action == VMF_EVENT_CONTINUE)
			continue;
		if (action == VMF_EVENT_PAUSE)
			return VMF_PAUSED;
		decoder->error = make_error(action == VMF_EVENT_ABORT
						    ? VMF_ABORTED
						    : VMF_INVALID_ARGUMENT);
		decoder->error.offset = decoder->envelope.header_octets * 8 +
					decoder->completed_body_octets * 8;
		if (decoder->phase == DECODER_PAYLOAD)
			decoder->error.offset += payload_bits(decoder->payload);
		if (event.schema)
			decoder->error.message_index =
				event.kind == VMF_EVENT_MESSAGE_END
					? event.as.message.index
					: decoder->descriptor;
		decoder->error.node_id = event.node ? event.node->id : 0;
		decoder->phase = DECODER_FAILED;
		return decoder->error.status;
	}
}

vmf_status vmf_encoder_write_field(vmf_encoder *encoder, vmf_output *output,
	const vmf_node *node, const void *value, size_t value_size,
	uint64_t total_units)
{
	if (!encoder || !node || !value || node->kind != VMF_NODE_FIELD)
		return VMF_INVALID_ARGUMENT;
	vmf_layout layout = vmf_field_layout(node->as.field.kind);

	if (value_size != layout.size || !layout.size ||
		reinterpret_cast<uintptr_t>(value) % layout.alignment)
		return VMF_INVALID_ARGUMENT;
	if (encoder->event_pending && !encoder->field_pending)
		return VMF_INVALID_STATE;
	if (encoder->phase != ENCODER_PAYLOAD)
		return VMF_INVALID_STATE;
	vmf_event event = {};

	event.kind = VMF_EVENT_FIELD;
	event.node = node;
	vmf_status status;

	if (encoder->field_pending) {
		event.depth = encoder->field_event.depth;
		event.occurrence = encoder->field_event.occurrence;
		event.as.field.offset_units =
			encoder->field_event.as.field.offset_units;
	} else {
		status = payload_field_context(encoder->payload, node,
			&event.depth, &event.occurrence,
			&event.as.field.offset_units);
		if (status != VMF_OK)
			return status;
	}
	event.as.field.total_units = total_units;
	std::memcpy(&event.as.field.value, value, value_size);
	/* Public write checks active value metadata against its retry copy. */
	encoder->field_event = event;
	encoder->field_pending = true;
	status = vmf_encoder_write(encoder, output, &encoder->field_event);
	if (status != VMF_NEED_OUTPUT && !encoder->event_pending)
		encoder->field_pending = false;
	return status;
}

vmf_status vmf_event_copy_field(
	const vmf_event *event, void *value, size_t value_size)
{
	if (!event || !value || event->kind != VMF_EVENT_FIELD ||
		!event->node || event->node->kind != VMF_NODE_FIELD)
		return VMF_INVALID_ARGUMENT;
	vmf_layout layout = vmf_field_layout(event->node->as.field.kind);

	if (!layout.size || value_size != layout.size ||
		reinterpret_cast<uintptr_t>(value) % layout.alignment)
		return VMF_INVALID_ARGUMENT;
	std::memcpy(value, &event->as.field.value, value_size);
	return VMF_OK;
}
