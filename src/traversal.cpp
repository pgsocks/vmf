#include "codec.hpp"
#include <cstring>

using namespace vmf_internal;

static uint64_t pdu_limit(const configuration *config)
{
	uint64_t limit = config->limits.max_pdu_octets;

	if (config->profile <= VMF_ENVELOPE_C && limit > 1048575)
		limit = 1048575;
	return limit;
}

static vmf_status decoder_fail(vmf_decoder *decoder, vmf_status status)
{
	if (status == VMF_NEED_INPUT)
		return status;
	decoder->error.status = status;
	if (decoder->phase != DECODER_HEADER) {
		decoder->error.offset = decoder->envelope.header_octets * 8 +
					decoder->completed_body_octets * 8;
		if (decoder->phase == DECODER_PAYLOAD)
			decoder->error.offset += payload_bits(decoder->payload);
		decoder->error.message_index = decoder->descriptor;
		decoder->error.node_id =
			decoder->phase == DECODER_PAYLOAD
				? payload_error_node(decoder->payload)
				: 0;
	}
	decoder->phase = DECODER_FAILED;
	return status;
}

static vmf_status encoder_fail(vmf_encoder *encoder, vmf_status status)
{
	if (status == VMF_NEED_OUTPUT)
		return status;
	encoder->error.status = status;
	encoder->error.offset = encoder->header_written * 8 +
				encoder->completed_body_octets * 8;
	encoder->error.message_index = encoder->descriptor;
	encoder->error.node_id = 0;
	if (encoder->phase == ENCODER_PAYLOAD) {
		encoder->error.offset += payload_bits(encoder->payload);
		encoder->error.node_id = payload_error_node(encoder->payload);
	}
	encoder->phase = ENCODER_FAILED;
	return status;
}

static vmf_status begin_decoder(vmf_decoder *decoder, bool header_only)
{
	if (!decoder)
		return VMF_INVALID_ARGUMENT;
	if (decoder->phase != DECODER_IDLE && decoder->phase != DECODER_DONE) {
		if (decoder->phase != DECODER_FAILED)
			decoder->error.status = VMF_INVALID_STATE;
		return VMF_INVALID_STATE;
	}
	vmf_decoder_reset(decoder);
	decoder->header_only = header_only;
	decoder->phase = DECODER_HEADER;
	return VMF_OK;
}

vmf_status vmf_decoder_begin_pdu(vmf_decoder *decoder)
{
	return begin_decoder(decoder, false);
}

vmf_status vmf_decoder_begin_envelope(vmf_decoder *decoder)
{
	return begin_decoder(decoder, true);
}

vmf_status vmf_encoder_begin_envelope(vmf_encoder *encoder)
{
	if (!encoder)
		return VMF_INVALID_ARGUMENT;
	if (encoder->phase != ENCODER_IDLE && encoder->phase != ENCODER_DONE) {
		if (encoder->phase != ENCODER_FAILED)
			encoder->error.status = VMF_INVALID_STATE;
		return VMF_INVALID_STATE;
	}
	encoder->phase = ENCODER_IDLE;
	encoder->header_only = true;
	encoder->error = make_error(VMF_OK);
	return VMF_OK;
}

static vmf_status read_header(
	vmf_decoder *decoder, vmf_input *input, vmf_event *event)
{
	for (;;) {
		if (decoder->header_needed >
			decoder->config.limits.max_header_bytes)
			return VMF_LIMIT_EXCEEDED;
		while (decoder->header_size < decoder->header_needed &&
			input->used < input->size)
			decoder->header_bytes[decoder->header_size++] =
				input->data[input->used++];
		vmf_status status = parse_header(decoder->config.profile,
			decoder->header_bytes, decoder->header_size,
			input->final && input->used == input->size,
			decoder->arena, decoder->config.limits.max_header_bytes,
			&decoder->config.limits, false, &decoder->envelope,
			&decoder->header_needed, &decoder->error);

		if (status == VMF_NEED_INPUT) {
			if (decoder->header_needed <= decoder->header_size)
				decoder->header_needed =
					decoder->header_size + 1;
			if (input->used == input->size)
				return input->final ? VMF_TRUNCATED
						    : VMF_NEED_INPUT;
			continue;
		}
		if (status != VMF_OK)
			return status;
		if (decoder->envelope.header_octets >
				pdu_limit(&decoder->config) ||
			(decoder->envelope.pdu_octets != VMF_LENGTH_UNKNOWN &&
				decoder->envelope.pdu_octets >
					pdu_limit(&decoder->config)))
			return VMF_LIMIT_EXCEEDED;
		event->kind = VMF_EVENT_ENVELOPE;
		event->as.envelope = decoder->envelope;
		decoder->phase =
			decoder->header_only ? DECODER_DONE : DECODER_SELECT;
		return VMF_OK;
	}
}

static vmf_status select_decoder(vmf_decoder *decoder, vmf_event *event)
{
	const vmf_header_fields *header = fields(&decoder->envelope);

	while (decoder->descriptor < header->message_count &&
		!header->messages[decoder->descriptor].has_body)
		++decoder->descriptor;
	if (decoder->descriptor == header->message_count) {
		decoder->phase = DECODER_PDU_END;
		return VMF_DONE;
	}
	const vmf_message_header *message =
		&header->messages[decoder->descriptor];

	if (header->compression.present ||
		(message->security.present && message->security.spi != 0))
		return VMF_UNSUPPORTED_ENVELOPE_FEATURE;
	vmf_status status = vmf_schema_select(
		&decoder->config.schemas, &message->id, &decoder->schema);

	if (status != VMF_OK)
		return status;
	uint64_t length = message->message_size_octets.present
				  ? message->message_size_octets.value
				  : VMF_LENGTH_UNKNOWN;
	uint64_t before = decoder->envelope.header_octets +
			  decoder->completed_body_octets;
	uint64_t budget = pdu_limit(&decoder->config) - before;

	if (budget > 1048575)
		budget = 1048575;

	if (length != VMF_LENGTH_UNKNOWN &&
		(length > budget ||
			length > decoder->config.limits.max_message_octets))
		return VMF_LIMIT_EXCEEDED;
	payload_begin(decoder->payload, decoder->schema, length, before * 8,
		false, budget);
	decoder->events = 1;
	decoder->phase = DECODER_PAYLOAD;
	event->kind = VMF_EVENT_MESSAGE_BEGIN;
	event->schema = decoder->schema;
	event->as.message = {message->id, decoder->descriptor, length};
	return VMF_OK;
}

vmf_status vmf_decoder_next(
	vmf_decoder *decoder, vmf_input *input, vmf_event *event)
{
	if (event)
		*event = {};
	if (!decoder || !input || !event || input->used > input->size ||
		(input->size && !input->data)) {
		if (decoder && decoder->phase != DECODER_FAILED)
			decoder->error.status = VMF_INVALID_ARGUMENT;
		return VMF_INVALID_ARGUMENT;
	}
	if (decoder->phase == DECODER_FAILED)
		return decoder->error.status;
	for (;;) {
		vmf_status status;

		switch (decoder->phase) {
		case DECODER_IDLE:
			decoder->error.status = VMF_INVALID_STATE;
			return VMF_INVALID_STATE;
		case DECODER_DONE:
			return VMF_DONE;
		case DECODER_HEADER:
			status = read_header(decoder, input, event);
			break;
		case DECODER_SELECT:
			status = select_decoder(decoder, event);
			if (status == VMF_DONE)
				continue;
			break;
		case DECODER_PAYLOAD:
			if (decoder->events >=
				decoder->config.limits.max_events_per_message)
				return decoder_fail(
					decoder, VMF_LIMIT_EXCEEDED);
			status = payload_decode(decoder->payload, input, event);
			if (status == VMF_DONE) {
				decoder->phase = DECODER_MESSAGE_END;
				continue;
			}
			if (status == VMF_OK)
				++decoder->events;
			break;
		case DECODER_MESSAGE_END: {
			if (decoder->events >=
				decoder->config.limits.max_events_per_message)
				return decoder_fail(
					decoder, VMF_LIMIT_EXCEEDED);
			const vmf_header_fields *header =
				fields(&decoder->envelope);
			uint64_t length = payload_bits(decoder->payload) / 8;

			event->kind = VMF_EVENT_MESSAGE_END;
			event->schema = decoder->schema;
			event->as.message = {
				header->messages[decoder->descriptor].id,
				decoder->descriptor, length};
			decoder->completed_body_octets += length;
			++decoder->events;
			++decoder->descriptor;
			decoder->phase = DECODER_SELECT;
			decoder->error = make_error(VMF_OK);
			return VMF_OK;
		}
		case DECODER_PDU_END:
			event->kind = VMF_EVENT_PDU_END;
			decoder->phase = DECODER_DONE;
			return VMF_OK;
		default:
			return VMF_INVALID_STATE;
		}
		if (status != VMF_OK) {
			*event = {};
			return decoder_fail(decoder, status);
		}
		decoder->error = make_error(VMF_OK);
		return VMF_OK;
	}
}

static bool event_equal(const vmf_event *a, const vmf_event *b)
{
	if (a->kind != b->kind || a->schema != b->schema ||
		a->node != b->node || a->depth != b->depth ||
		a->occurrence != b->occurrence)
		return false;
	switch (a->kind) {
	case VMF_EVENT_ENVELOPE:
		return a->as.envelope.wire_version ==
			       b->as.envelope.wire_version &&
		       a->as.envelope.header == b->as.envelope.header &&
		       a->as.envelope.object_size ==
			       b->as.envelope.object_size &&
		       a->as.envelope.header_octets ==
			       b->as.envelope.header_octets &&
		       a->as.envelope.pdu_octets == b->as.envelope.pdu_octets;
	case VMF_EVENT_MESSAGE_BEGIN:
	case VMF_EVENT_MESSAGE_END:
		return vmf_message_id_equal(
			       &a->as.message.id, &b->as.message.id) &&
		       a->as.message.index == b->as.message.index &&
		       a->as.message.payload_octets ==
			       b->as.message.payload_octets;
	case VMF_EVENT_GROUP_BEGIN:
	case VMF_EVENT_GROUP_END:
		return a->as.group.count == b->as.group.count &&
		       a->as.group.more_present == b->as.group.more_present &&
		       (!a->as.group.more_present ||
			       a->as.group.more == b->as.group.more);
	case VMF_EVENT_FIELD: {
		if (!a->node ||
			a->as.field.offset_units != b->as.field.offset_units ||
			a->as.field.total_units != b->as.field.total_units)
			return false;
		const vmf_field *x = &a->as.field.value,
				*y = &b->as.field.value;

		switch (a->node->as.field.kind) {
		case VMF_FIELD_BOOL:
			return x->boolean.value == y->boolean.value;
		case VMF_FIELD_UNSIGNED:
			return x->unsigned_integer.value ==
			       y->unsigned_integer.value;
		case VMF_FIELD_SIGNED:
			return x->signed_integer.value ==
			       y->signed_integer.value;
		case VMF_FIELD_ENUM:
			return x->enumeration.code == y->enumeration.code;
		case VMF_FIELD_SCALED:
			return x->scaled.code == y->scaled.code;
		case VMF_FIELD_TEXT:
			return x->text.data == y->text.data &&
			       x->text.size == y->text.size &&
			       x->text.last == y->text.last;
		case VMF_FIELD_OCTETS:
			return x->octets.data == y->octets.data &&
			       x->octets.size == y->octets.size &&
			       x->octets.last == y->octets.last;
		case VMF_FIELD_BITS:
			return x->bits.data == y->bits.data &&
			       x->bits.bit_count == y->bits.bit_count &&
			       x->bits.last == y->bits.last;
		}
		return false;
	}
	default:
		return true;
	}
}

static vmf_status write_header(
	vmf_encoder *encoder, vmf_output *output, const vmf_event *event)
{
	if (!encoder->header_pending) {
		vmf_status status = serialize_header(encoder->config.profile,
			&event->as.envelope, encoder->counting,
			encoder->header_bytes,
			encoder->config.limits.max_header_bytes,
			&encoder->header_size, &encoder->config.limits,
			&encoder->error);

		if (status != VMF_OK)
			return status;
		size_t needed = 0;

		status = parse_header(encoder->config.profile,
			encoder->header_bytes, encoder->header_size, true,
			encoder->arena, encoder->config.limits.max_header_bytes,
			&encoder->config.limits, encoder->counting,
			&encoder->envelope, &needed, &encoder->error);
		if (status != VMF_OK)
			return status;
		if (encoder->header_size > pdu_limit(&encoder->config) ||
			(!encoder->counting &&
				encoder->envelope.pdu_octets !=
					VMF_LENGTH_UNKNOWN &&
				encoder->envelope.pdu_octets >
					pdu_limit(&encoder->config)))
			return VMF_LIMIT_EXCEEDED;
		encoder->header_written = 0;
		encoder->descriptor = 0;
		encoder->completed_body_octets = 0;
		std::memset(encoder->measured, 0, sizeof(encoder->measured));
		std::memset(encoder->measurements, 0,
			sizeof(encoder->measurements));
		encoder->header_pending = true;
	}
	if (encoder->counting) {
		if (encoder->header_size > (UINT64_MAX - encoder->bits) / 8)
			return VMF_LIMIT_EXCEEDED;
		encoder->bits += encoder->header_size * 8;
		encoder->header_written = encoder->header_size;
	} else {
		while (encoder->header_written < encoder->header_size &&
			output->used < output->capacity) {
			if (encoder->bits > UINT64_MAX - 8)
				return VMF_LIMIT_EXCEEDED;
			output->data[output->used++] =
				encoder->header_bytes[encoder
						->header_written++];
			encoder->bits += 8;
		}
		if (encoder->header_written != encoder->header_size)
			return VMF_NEED_OUTPUT;
	}
	encoder->header_pending = false;
	encoder->phase = encoder->header_only ? ENCODER_DONE : ENCODER_SELECT;
	const vmf_header_fields *header = fields(&encoder->envelope);

	for (size_t i = 0; i < header->message_count; ++i) {
		encoder->measurements[i].header_octets = encoder->header_size;
		if (!header->messages[i].has_body)
			encoder->measured[i] = true;
	}
	return VMF_OK;
}

static vmf_status start_message(vmf_encoder *encoder, const vmf_event *event)
{
	const vmf_header_fields *header = fields(&encoder->envelope);

	while (encoder->descriptor < header->message_count &&
		!header->messages[encoder->descriptor].has_body)
		++encoder->descriptor;
	if (encoder->descriptor == header->message_count || event->node ||
		event->depth || event->occurrence ||
		event->as.message.index != encoder->descriptor)
		return VMF_INVALID_STATE;
	const vmf_message_header *message =
		&header->messages[encoder->descriptor];

	if (!vmf_message_id_equal(&message->id, &event->as.message.id))
		return VMF_TYPE_MISMATCH;
	if (header->compression.present ||
		(message->security.present && message->security.spi != 0))
		return VMF_UNSUPPORTED_ENVELOPE_FEATURE;
	vmf_status status = vmf_schema_select(
		&encoder->config.schemas, &message->id, &encoder->schema);

	if (status != VMF_OK)
		return status;
	if (event->schema && event->schema != encoder->schema)
		return VMF_TYPE_MISMATCH;
	uint64_t length = event->as.message.payload_octets;

	if (!encoder->counting && message->message_size_octets.present) {
		if (length != VMF_LENGTH_UNKNOWN &&
			length != message->message_size_octets.value)
			return VMF_OUT_OF_RANGE;
		length = message->message_size_octets.value;
	}
	uint64_t before = encoder->header_size + encoder->completed_body_octets;
	uint64_t budget = pdu_limit(&encoder->config) - before;

	if (budget > 1048575)
		budget = 1048575;

	if (length != VMF_LENGTH_UNKNOWN &&
		(length > budget ||
			length > encoder->config.limits.max_message_octets))
		return VMF_LIMIT_EXCEEDED;
	payload_begin(encoder->payload, encoder->schema, length, before * 8,
		encoder->counting, budget);
	encoder->begin_length = event->as.message.payload_octets;
	encoder->events = 1;
	encoder->phase = ENCODER_PAYLOAD;
	return VMF_OK;
}

static vmf_status write_event(
	vmf_encoder *encoder, vmf_output *output, const vmf_event *event)
{
	if (encoder->phase == ENCODER_IDLE) {
		if (event->kind != VMF_EVENT_ENVELOPE || event->node ||
			event->schema || event->depth || event->occurrence)
			return VMF_INVALID_STATE;
		encoder->phase = ENCODER_HEADER;
	}
	if (encoder->phase == ENCODER_HEADER) {
		if (event->kind != VMF_EVENT_ENVELOPE)
			return VMF_INVALID_STATE;
		return write_header(encoder, output, event);
	}
	if (encoder->phase == ENCODER_SELECT) {
		if (event->kind == VMF_EVENT_MESSAGE_BEGIN)
			return start_message(encoder, event);
		if (event->kind != VMF_EVENT_PDU_END || event->node ||
			event->schema || event->depth || event->occurrence)
			return VMF_INVALID_STATE;
		const vmf_header_fields *header = fields(&encoder->envelope);

		while (encoder->descriptor < header->message_count &&
			!header->messages[encoder->descriptor].has_body)
			++encoder->descriptor;
		if (encoder->descriptor != header->message_count)
			return VMF_INVALID_STATE;
		uint64_t total =
			encoder->header_size + encoder->completed_body_octets;

		for (size_t i = 0; i < header->message_count; ++i)
			encoder->measurements[i].pdu_octets = total;
		encoder->phase = ENCODER_IDLE;
		return VMF_OK;
	}
	if (encoder->phase != ENCODER_PAYLOAD)
		return VMF_INVALID_STATE;
	if (event->schema && event->schema != encoder->schema)
		return VMF_TYPE_MISMATCH;
	if (encoder->events >= encoder->config.limits.max_events_per_message)
		return VMF_LIMIT_EXCEEDED;
	if (event->kind == VMF_EVENT_MESSAGE_END) {
		const vmf_message_header *message =
			&fields(&encoder->envelope)
				 ->messages[encoder->descriptor];

		if (event->node || event->depth || event->occurrence ||
			event->as.message.index != encoder->descriptor ||
			!vmf_message_id_equal(
				&event->as.message.id, &message->id))
			return VMF_TYPE_MISMATCH;
	}
	uint64_t before = payload_bits(encoder->payload);
	vmf_status status = payload_encode(
		encoder->payload, output, event, encoder->counting);

	uint64_t advanced = payload_bits(encoder->payload) - before;

	if (advanced > UINT64_MAX - encoder->bits)
		return VMF_LIMIT_EXCEEDED;
	encoder->bits += advanced;
	if (status != VMF_OK)
		return status;
	++encoder->events;
	if (event->kind == VMF_EVENT_MESSAGE_END) {
		uint64_t length = payload_bits(encoder->payload) / 8;

		if ((event->as.message.payload_octets != VMF_LENGTH_UNKNOWN &&
			    event->as.message.payload_octets != length) ||
			(encoder->begin_length != VMF_LENGTH_UNKNOWN &&
				encoder->begin_length != length))
			return VMF_OUT_OF_RANGE;
		encoder->measurements[encoder->descriptor].body_octets = length;
		encoder->measured[encoder->descriptor] = true;
		encoder->completed_body_octets += length;
		++encoder->descriptor;
		encoder->phase = ENCODER_SELECT;
	}
	return VMF_OK;
}

vmf_status vmf_encoder_write(
	vmf_encoder *encoder, vmf_output *output, const vmf_event *event)
{
	if (!encoder || !event ||
		(encoder->counting
				? output != nullptr
				: (!output || output->used > output->capacity ||
					  (output->capacity &&
						  !output->data)))) {
		if (encoder && encoder->phase != ENCODER_FAILED)
			encoder->error.status = VMF_INVALID_ARGUMENT;
		return VMF_INVALID_ARGUMENT;
	}
	if (encoder->phase == ENCODER_FAILED)
		return encoder->error.status;
	if (encoder->field_pending && event != &encoder->field_event)
		return VMF_INVALID_STATE;
	if (encoder->event_pending &&
		!event_equal(event, &encoder->pending_event)) {
		encoder->error.status = VMF_INVALID_ARGUMENT;
		return VMF_INVALID_ARGUMENT;
	}
	vmf_status status = write_event(encoder, output, event);

	if (status == VMF_INVALID_ARGUMENT) {
		encoder->error.status = status;
		return status;
	}
	if (status == VMF_NEED_OUTPUT) {
		encoder->pending_event = *event;
		encoder->event_pending = true;
		return status;
	}
	encoder->event_pending = false;
	if (status != VMF_OK)
		return encoder_fail(encoder, status);
	encoder->error = make_error(VMF_OK);
	return VMF_OK;
}

uint64_t vmf_encoder_bit_count(const vmf_encoder *encoder)
{
	return encoder ? encoder->bits : 0;
}

vmf_status vmf_encoder_measurement(
	const vmf_encoder *encoder, size_t descriptor, vmf_measurement *result)
{
	if (result)
		*result = {};
	if (!encoder || !result || !encoder->envelope.header ||
		descriptor >= fields(&encoder->envelope)->message_count)
		return VMF_INVALID_ARGUMENT;
	if (!encoder->measured[descriptor])
		return VMF_INVALID_STATE;
	*result = encoder->measurements[descriptor];
	return VMF_OK;
}
