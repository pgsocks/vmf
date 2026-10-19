#include "codec.hpp"
#include <cstring>

using namespace vmf_internal;

static bool has_reference(const vmf_event *event)
{
	return event->index || event->scope.kind != VMF_SCOPE_NONE;
}

static vmf_status select_runtime(const vmf_runtime_set *set,
	const vmf_message_id *id, const vmf_runtime_schema **result)
{
	*result = nullptr;
	for (size_t i = 0; i < set->count; ++i) {
		if (vmf_message_id_equal(&set->schemas[i]->id, id)) {
			*result = set->schemas[i];
			return VMF_OK;
		}
	}
	return VMF_UNKNOWN_MESSAGE;
}

static void payload_location(const vmf_runtime_schema *schema,
	vmf_node_id id, vmf_error *error)
{
	error->index_position = SIZE_MAX;
	if (!schema)
		return;
	for (size_t i = 0; i < schema->binding_count; ++i) {
		const vmf_node *node = schema->bindings[i];

		if (node->id == id && node->source) {
			error->index_position = node->source -
				schema->source->indexes;
			error->related_scope_id = node->scope.id;
			error->property = node->kind == VMF_NODE_FIELD
				? VMF_PROPERTY_ENCODING
				: node->kind == VMF_NODE_REPEAT
				? VMF_PROPERTY_REPEAT : VMF_PROPERTY_GROUP;
			return;
		}
	}
}

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
	decoder->error.schema_index =
		decoder->schema && decoder->config.schemas
		? static_cast<size_t>(decoder->schema->source -
			decoder->config.schemas->messages) : SIZE_MAX;
	if (decoder->phase != DECODER_HEADER) {
		decoder->error.offset = decoder->envelope.header_octets * 8 +
					decoder->completed_body_octets * 8;
		if (decoder->phase == DECODER_PAYLOAD)
			decoder->error.offset += payload_bits(decoder->payload);
		decoder->error.message_index = decoder->descriptor;
		payload_location(decoder->schema,
			decoder->phase == DECODER_PAYLOAD
				? payload_error_node(decoder->payload) : 0,
			&decoder->error);
	}
	decoder->phase = DECODER_FAILED;
	return status;
}

static vmf_status encoder_fail(vmf_encoder *encoder, vmf_status status)
{
	if (status == VMF_NEED_OUTPUT)
		return status;
	encoder->error.status = status;
	encoder->error.schema_index =
		encoder->schema && encoder->config.schemas
		? static_cast<size_t>(encoder->schema->source -
			encoder->config.schemas->messages) : SIZE_MAX;
	encoder->error.offset = encoder->header_written * 8 +
				encoder->completed_body_octets * 8;
	encoder->error.message_index = encoder->descriptor;
	encoder->error.index_position = SIZE_MAX;
	if (encoder->phase == ENCODER_PAYLOAD) {
		encoder->error.offset += payload_bits(encoder->payload);
		payload_location(encoder->schema,
			payload_error_node(encoder->payload), &encoder->error);
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
	encoder->mode = ENCODER_MANUAL;
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
	vmf_status status = select_runtime(
		&decoder->config.resolved, &message->id, &decoder->schema);

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
	event->schema = decoder->schema->source;
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
			event->schema = decoder->schema->source;
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
		a->index != b->index ||
		a->scope.kind != b->scope.kind ||
		(a->scope.kind != VMF_SCOPE_NONE &&
		 a->scope.id != b->scope.id) ||
		(a->kind == VMF_EVENT_FIELD &&
		 a->field_kind != b->field_kind) ||
		a->depth != b->depth ||
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
		if (!a->index ||
			a->as.field.offset_units != b->as.field.offset_units ||
			a->as.field.total_units != b->as.field.total_units)
			return false;
		const vmf_field *x = &a->as.field.value,
				*y = &b->as.field.value;

		switch (a->field_kind) {
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

static vmf_status prepare_header(vmf_encoder *encoder,
	const vmf_envelope_event *envelope)
{
	size_t size = 0;
	vmf_status status = serialize_header(encoder->config.profile,
		envelope, encoder->counting, encoder->header_bytes,
		encoder->config.limits.max_header_bytes, &size,
		&encoder->config.limits, &encoder->error);

	if (status != VMF_OK)
		return status;
	vmf_envelope_event candidate = {};
	size_t needed = 0;

	status = parse_header(encoder->config.profile,
		encoder->header_bytes, size, true, encoder->candidate_arena,
		encoder->config.limits.max_header_bytes,
		&encoder->config.limits, encoder->counting, &candidate, &needed,
		&encoder->error);
	if (status != VMF_OK)
		return status;
	if (size > pdu_limit(&encoder->config) ||
		(!encoder->counting &&
			candidate.pdu_octets != VMF_LENGTH_UNKNOWN &&
			candidate.pdu_octets > pdu_limit(&encoder->config)))
		return VMF_LIMIT_EXCEEDED;
	/* Candidate storage cannot invalidate a previously committed header. */
	void *previous = encoder->arena;

	encoder->arena = encoder->candidate_arena;
	encoder->candidate_arena = previous;
	encoder->envelope = candidate;
	encoder->header_size = size;
	encoder->header_written = 0;
	encoder->descriptor = 0;
	encoder->schema = nullptr;
	encoder->events = 0;
	encoder->begin_length = VMF_LENGTH_UNKNOWN;
	encoder->completed_body_octets = 0;
	std::memset(encoder->measured, 0, sizeof(encoder->measured));
	std::memset(encoder->measurements, 0, sizeof(encoder->measurements));
	encoder->header_pending = true;
	return VMF_OK;
}

static vmf_status flush_header(vmf_encoder *encoder, vmf_output *output)
{
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

static vmf_status write_header(vmf_encoder *encoder, vmf_output *output,
	const vmf_event *event)
{
	if (!encoder->header_pending) {
		vmf_status status =
			prepare_header(encoder, &event->as.envelope);

		if (status != VMF_OK)
			return status;
	}
	return flush_header(encoder, output);
}

static vmf_status complete_message(vmf_encoder *encoder,
	uint64_t end_length)
{
	uint64_t length = payload_bits(encoder->payload) / 8;

	if ((end_length != VMF_LENGTH_UNKNOWN && end_length != length) ||
		(encoder->begin_length != VMF_LENGTH_UNKNOWN &&
			encoder->begin_length != length))
		return VMF_OUT_OF_RANGE;
	encoder->measurements[encoder->descriptor].body_octets = length;
	encoder->measured[encoder->descriptor] = true;
	encoder->completed_body_octets += length;
	++encoder->descriptor;
	encoder->phase = ENCODER_SELECT;
	return VMF_OK;
}

static vmf_status start_message(vmf_encoder *encoder, const vmf_event *event)
{
	const vmf_header_fields *header = fields(&encoder->envelope);

	while (encoder->descriptor < header->message_count &&
		!header->messages[encoder->descriptor].has_body)
		++encoder->descriptor;
	if (encoder->descriptor == header->message_count ||
		has_reference(event) ||
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
	vmf_status status = select_runtime(
		&encoder->config.resolved, &message->id, &encoder->schema);

	if (status != VMF_OK)
		return status;
	if (event->schema && event->schema != encoder->schema->source)
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
		if (event->kind != VMF_EVENT_ENVELOPE || has_reference(event) ||
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
		if (event->kind != VMF_EVENT_PDU_END || has_reference(event) ||
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
	if (event->schema && event->schema != encoder->schema->source)
		return VMF_TYPE_MISMATCH;
	if (encoder->events >= encoder->config.limits.max_events_per_message)
		return VMF_LIMIT_EXCEEDED;
	if (event->kind == VMF_EVENT_MESSAGE_END) {
		const vmf_message_header *message =
			&fields(&encoder->envelope)
				 ->messages[encoder->descriptor];

		if (has_reference(event) || event->depth || event->occurrence ||
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
	if (event->kind == VMF_EVENT_MESSAGE_END)
		return complete_message(
			encoder, event->as.message.payload_octets);
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
	if (encoder->mode == ENCODER_MANAGED) {
		if (encoder->phase != ENCODER_DONE)
			return VMF_INVALID_STATE;
		encoder->phase = ENCODER_IDLE;
		encoder->mode = ENCODER_MANUAL;
		encoder->header_only = false;
	}
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

static vmf_status managed_argument(vmf_encoder *encoder, vmf_status status)
{
	if (encoder && encoder->phase != ENCODER_FAILED)
		encoder->error.status = status;
	return status;
}

static bool valid_output(const vmf_encoder *encoder, const vmf_output *output)
{
	return encoder->counting ? output == nullptr
		: output && output->used <= output->capacity &&
			(!output->capacity || output->data);
}

vmf_status vmf_encoder_begin_pdu(vmf_encoder *encoder,
	const vmf_envelope_event *envelope)
{
	if (!encoder || !envelope)
		return managed_argument(encoder, VMF_INVALID_ARGUMENT);
	if (encoder->phase != ENCODER_IDLE && encoder->phase != ENCODER_DONE)
		return managed_argument(encoder, VMF_INVALID_STATE);
	vmf_status status = prepare_header(encoder, envelope);

	if (status != VMF_OK)
		return managed_argument(encoder, status);
	encoder->phase = ENCODER_HEADER;
	encoder->mode = ENCODER_MANAGED;
	encoder->header_only = false;
	encoder->field_pending = encoder->event_pending = false;
	encoder->request_pending = false;
	encoder->request = {};
	encoder->reply = {};
	encoder->reply_owner = REPLY_NONE;
	encoder->provider = nullptr;
	encoder->provider_user = nullptr;
	encoder->error = make_error(VMF_OK);
	return VMF_OK;
}

static vmf_status managed_progress(vmf_encoder *encoder, uint64_t before,
	uint64_t events, vmf_status status)
{
	uint64_t advanced = payload_bits(encoder->payload) - before;

	if (advanced > UINT64_MAX - encoder->bits)
		return encoder_fail(encoder, VMF_LIMIT_EXCEEDED);
	encoder->bits += advanced;
	encoder->events += events;
	if (status == VMF_INVALID_ARGUMENT)
		return managed_argument(encoder, status);
	if (status != VMF_OK && status != VMF_DONE && status != VMF_NEED_OUTPUT)
		return encoder_fail(encoder, status);
	encoder->error = make_error(VMF_OK);
	return status;
}

vmf_status vmf_encoder_next_request(vmf_encoder *encoder,
	vmf_output *output, vmf_encode_request *request)
{
	if (request)
		*request = {};
	if (!encoder || !request || !valid_output(encoder, output))
		return managed_argument(encoder, VMF_INVALID_ARGUMENT);
	if (encoder->phase == ENCODER_FAILED)
		return encoder->error.status;
	if (encoder->mode != ENCODER_MANAGED ||
		encoder->reply_owner != REPLY_NONE)
		return managed_argument(encoder, VMF_INVALID_STATE);
	if (encoder->request_pending) {
		*request = encoder->request;
		encoder->error = make_error(VMF_OK);
		return VMF_OK;
	}
	const vmf_limits *limits = &encoder->config.limits;

	for (;;) {
		vmf_status status;

		switch (encoder->phase) {
		case ENCODER_DONE:
			return VMF_DONE;
		case ENCODER_HEADER:
			status = flush_header(encoder, output);
			if (status != VMF_OK)
				return status == VMF_NEED_OUTPUT ? status
					: encoder_fail(encoder, status);
			break;
		case ENCODER_SELECT: {
			const vmf_header_fields *header =
				fields(&encoder->envelope);

			while (encoder->descriptor < header->message_count &&
				!header->messages[encoder->descriptor].has_body)
				++encoder->descriptor;
			vmf_event event = {};

			if (encoder->descriptor == header->message_count) {
				event.kind = VMF_EVENT_PDU_END;
				status = write_event(encoder, output, &event);
				if (status != VMF_OK)
					return encoder_fail(encoder, status);
				encoder->phase = ENCODER_DONE;
				encoder->error = make_error(VMF_OK);
				return VMF_DONE;
			}
			if (limits->max_events_per_message < 2)
				return encoder_fail(
					encoder, VMF_LIMIT_EXCEEDED);
			event.kind = VMF_EVENT_MESSAGE_BEGIN;
			event.as.message = {
				header->messages[encoder->descriptor].id,
				encoder->descriptor, VMF_LENGTH_UNKNOWN};
			status = start_message(encoder, &event);
			if (status != VMF_OK)
				return encoder_fail(encoder, status);
			break;
		}
		case ENCODER_PAYLOAD: {
			uint64_t before = payload_bits(encoder->payload);
			uint64_t events = 0;
			uint64_t limit = limits->max_events_per_message - 2;

			status = payload_managed_next(encoder->payload, output,
				&encoder->request, encoder->request_path,
				limits->max_depth + 1,
				limit, &events);
			status = managed_progress(
				encoder, before, events, status);
			if (status == VMF_OK) {
				encoder->request.descriptor_index =
					encoder->descriptor;
				encoder->request_pending = true;
				*request = encoder->request;
				return VMF_OK;
			}
			if (status != VMF_DONE)
				return status;
			++encoder->events;
			status = complete_message(encoder, VMF_LENGTH_UNKNOWN);
			if (status != VMF_OK)
				return encoder_fail(encoder, status);
			break;
		}
		default:
			return managed_argument(encoder, VMF_INVALID_STATE);
		}
	}
}

static bool reply_equal(const vmf_encode_request *request,
	const vmf_encode_reply *a, const vmf_encode_reply *b)
{
	if (a->kind != b->kind)
		return false;
	if (a->kind == VMF_ENCODE_REQUEST_PRESENCE)
		return a->as.present == b->as.present;
	if (a->kind == VMF_ENCODE_REQUEST_REPEAT_COUNT)
		return a->as.repeat_count == b->as.repeat_count;
	vmf_event x = {}, y = {};

	x.kind = VMF_EVENT_FIELD;
	x.index = request->index;
	x.field_kind = request->field_kind;
	x.as.field.value = a->as.field;
	y = x;
	y.as.field.value = b->as.field;
	return event_equal(&x, &y);
}

static vmf_status respond_managed(vmf_encoder *encoder, vmf_output *output,
	const vmf_encode_reply *reply, reply_owner owner)
{
	if (!encoder || !reply || !valid_output(encoder, output))
		return managed_argument(encoder, VMF_INVALID_ARGUMENT);
	if (encoder->phase == ENCODER_FAILED)
		return encoder->error.status;
	if (encoder->mode != ENCODER_MANAGED || !encoder->request_pending ||
		(encoder->reply_owner != REPLY_NONE &&
			encoder->reply_owner != owner))
		return managed_argument(encoder, VMF_INVALID_STATE);
	if (reply->kind < VMF_ENCODE_REQUEST_FIELD ||
		reply->kind > VMF_ENCODE_REQUEST_REPEAT_COUNT)
		return managed_argument(encoder, VMF_INVALID_ARGUMENT);
	if (encoder->reply_owner != REPLY_NONE &&
		!reply_equal(&encoder->request, reply, &encoder->reply))
		return managed_argument(encoder, VMF_INVALID_ARGUMENT);
	if (reply->kind != encoder->request.kind)
		return managed_argument(encoder, VMF_TYPE_MISMATCH);
	uint64_t before = payload_bits(encoder->payload);
	uint64_t events = 0;
	uint64_t limit = encoder->config.limits.max_events_per_message - 2;
	vmf_status status = payload_managed_respond(encoder->payload, output,
		reply, limit, &events);

	status = managed_progress(encoder, before, events, status);
	if (status == VMF_NEED_OUTPUT) {
		encoder->reply = *reply;
		encoder->reply_owner = owner;
	} else if (status == VMF_OK) {
		encoder->request_pending = false;
		encoder->reply_owner = REPLY_NONE;
	}
	return status;
}

vmf_status vmf_encoder_respond(vmf_encoder *encoder, vmf_output *output,
	const vmf_encode_reply *reply)
{
	return respond_managed(encoder, output, reply, REPLY_DIRECT);
}

vmf_status vmf_encoder_dispatch(vmf_encoder *encoder, vmf_output *output,
	vmf_encode_provider provider, void *user, size_t *delivered)
{
	if (delivered)
		*delivered = 0;
	if (!encoder || !provider || !delivered ||
		!valid_output(encoder, output))
		return managed_argument(encoder, VMF_INVALID_ARGUMENT);
	if (encoder->phase == ENCODER_FAILED)
		return encoder->error.status;
	if (encoder->mode != ENCODER_MANAGED ||
		encoder->reply_owner == REPLY_DIRECT)
		return managed_argument(encoder, VMF_INVALID_STATE);
	if (encoder->reply_owner == REPLY_PROVIDER &&
		(provider != encoder->provider ||
			user != encoder->provider_user))
		return managed_argument(encoder, VMF_INVALID_ARGUMENT);
	for (;;) {
		if (encoder->reply_owner == REPLY_NONE) {
			vmf_encode_request request = {};
			vmf_status status = vmf_encoder_next_request(
				encoder, output, &request);

			if (status != VMF_OK)
				return status;
			encoder->reply = {};
			++*delivered;
			encoder->provider_action = provider(user,
				&encoder->request, &encoder->reply);
			if (encoder->provider_action != VMF_EVENT_CONTINUE &&
				encoder->provider_action != VMF_EVENT_PAUSE) {
				vmf_status failure = encoder->provider_action ==
					VMF_EVENT_ABORT ? VMF_ABORTED
							: VMF_INVALID_ARGUMENT;

				return encoder_fail(encoder, failure);
			}
			encoder->provider = provider;
			encoder->provider_user = user;
			encoder->reply_owner = REPLY_PROVIDER;
		}
		vmf_status status = respond_managed(encoder, output,
			&encoder->reply, REPLY_PROVIDER);

		if (status == VMF_INVALID_ARGUMENT ||
			status == VMF_TYPE_MISMATCH) {
			encoder->reply_owner = REPLY_NONE;
			return status;
		}
		if (status != VMF_OK)
			return status;
		if (encoder->provider_action == VMF_EVENT_PAUSE)
			return VMF_PAUSED;
	}
}
