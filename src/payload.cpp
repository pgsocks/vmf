#include "payload.hpp"
#include "bits.hpp"
#include <cstring>
#include <cstddef>
#include <climits>

namespace vmf_internal
{

enum traversal_phase { PRESENCE, EXTENT, BEGIN, VALUE, CHILDREN, END, POP };

struct scalar_slot {
	const vmf_node *node;
	uint64_t value;
	size_t scope;
};

struct payload_frame {
	const vmf_node *node;
	const vmf_node *selected;
	uint64_t occurrence;
	uint64_t extent;
	uint64_t index;
	uint64_t units;
	uint64_t chunk_units;
	uint64_t declared_total;
	size_t chunk_bytes;
	size_t event_index;
	uint32_t character;
	uint8_t utf8[4];
	uint8_t utf8_size;
	uint8_t utf8_need;
	uint8_t pending[4];
	uint8_t pending_size;
	uint8_t pending_units;
	traversal_phase phase;
	bool unknown;
	bool terminal;
	bool present;
	bool more;
	bool more_present;
	bool extent_ready;
	bool event_active;
	bool literal_done;
	bool total_seen;
};

struct payload_state {
	vmf_limits limits;
	const vmf_message_schema *schema;
	payload_frame *frames;
	scalar_slot *scalars;
	uint8_t *chunk;
	size_t stack;
	size_t scalar_count;
	uint64_t bound;
	uint64_t configured_maximum;
	uint64_t events;
	bit_cursor bits;
	vmf_node_id error_node;
	vmf_status begin_error;
	bool finished;
	bool counting;
};

static size_t aligned(size_t size)
{
	size_t alignment = alignof(std::max_align_t);

	return (size + alignment - 1) & ~(alignment - 1);
}

vmf_status payload_storage_size(const vmf_limits *limits, size_t *size)
{
	if (!limits || !size)
		return VMF_INVALID_ARGUMENT;
	*size = 0;
	size_t alignment = alignof(std::max_align_t);
	size_t total = aligned(sizeof(payload_state));

	if (limits->max_depth == SIZE_MAX ||
		limits->max_depth + 1 >
			(SIZE_MAX - alignment) / sizeof(payload_frame) ||
		limits->max_schema_nodes >
			(SIZE_MAX - alignment) / sizeof(scalar_slot))
		return VMF_LIMIT_EXCEEDED;
	size_t frames =
		aligned((limits->max_depth + 1) * sizeof(payload_frame));
	size_t scalars =
		aligned(limits->max_schema_nodes * sizeof(scalar_slot));

	if (frames > SIZE_MAX - total)
		return VMF_LIMIT_EXCEEDED;
	total += frames;
	if (scalars > SIZE_MAX - total)
		return VMF_LIMIT_EXCEEDED;
	total += scalars;
	if (limits->field_chunk_bytes > SIZE_MAX - total)
		return VMF_LIMIT_EXCEEDED;
	*size = total + limits->field_chunk_bytes;
	return VMF_OK;
}

vmf_status payload_init(
	void *memory, const vmf_limits *limits, payload_state **result)
{
	if (!memory || !limits || !result)
		return VMF_INVALID_ARGUMENT;
	*result = nullptr;
	size_t size;
	vmf_status status = payload_storage_size(limits, &size);

	if (status != VMF_OK)
		return status;
	std::memset(memory, 0, size);
	auto *state = static_cast<payload_state *>(memory);
	auto *next =
		static_cast<uint8_t *>(memory) + aligned(sizeof(payload_state));

	state->limits = *limits;
	state->configured_maximum = limits->max_message_octets;
	state->frames = reinterpret_cast<payload_frame *>(next);
	next += aligned((limits->max_depth + 1) * sizeof(payload_frame));
	state->scalars = reinterpret_cast<scalar_slot *>(next);
	next += aligned(limits->max_schema_nodes * sizeof(scalar_slot));
	state->chunk = next;
	*result = state;
	return VMF_OK;
}

static void push(payload_state *state, const vmf_node *node,
	uint64_t occurrence, bool more_present = false, bool more = false)
{
	payload_frame *frame = &state->frames[state->stack++];

	*frame = {};
	frame->node = node;
	frame->phase = PRESENCE;
	frame->occurrence = occurrence;
	frame->more_present = more_present;
	frame->more = more;
}

void payload_begin(payload_state *state, const vmf_message_schema *schema,
	uint64_t payload_octets, uint64_t body_start_bit, bool counting,
	uint64_t max_octets)
{
	(void)body_start_bit;
	state->schema = schema;
	state->limits.max_message_octets = state->configured_maximum;
	if (max_octets < state->limits.max_message_octets)
		state->limits.max_message_octets = max_octets;
	state->stack = 0;
	state->scalar_count = 0;
	state->bound = payload_octets == VMF_LENGTH_UNKNOWN
			       ? VMF_LENGTH_UNKNOWN
			       : payload_octets * 8;
	state->begin_error = VMF_OK;
	if (payload_octets != VMF_LENGTH_UNKNOWN &&
		(payload_octets > UINT64_MAX / 8 ||
			(!counting &&
				payload_octets >
					state->limits.max_message_octets)))
		state->begin_error = VMF_LIMIT_EXCEEDED;
	state->events = 0;
	state->bits = {};
	state->bits.msb_octets = schema->octet_order == VMF_BIT_MSB_FIRST;
	state->error_node = 0;
	state->finished = false;
	state->counting = counting;
	push(state, schema->root, 0);
}

uint64_t payload_bits(const payload_state *state)
{
	return state ? state->bits.offset : 0;
}

vmf_node_id payload_error_node(const payload_state *state)
{
	return state ? state->error_node : 0;
}

bool payload_complete(const payload_state *state)
{
	return state && state->finished;
}

static bool lookup(const payload_state *state, vmf_node_id id, uint64_t *value)
{
	for (size_t i = state->scalar_count; i; --i) {
		if (state->scalars[i - 1].node->id == id) {
			*value = state->scalars[i - 1].value;
			return true;
		}
	}
	return false;
}

static void clear_scope(payload_state *state, size_t depth)
{
	size_t count = 0;

	for (size_t i = 0; i < state->scalar_count; ++i)
		if (state->scalars[i].scope < depth)
			state->scalars[count++] = state->scalars[i];
	state->scalar_count = count;
}

static vmf_status remember(
	payload_state *state, const vmf_node *node, uint64_t value)
{
	if (state->scalar_count == state->limits.max_schema_nodes)
		return VMF_LIMIT_EXCEEDED;
	scalar_slot *slot = &state->scalars[state->scalar_count++];

	slot->node = node;
	slot->value = value;
	slot->scope = state->stack > 1 ? state->stack - 2 : 0;
	return VMF_OK;
}

static bool in_ranges(
	uint64_t value, const vmf_code_range *ranges, size_t count)
{
	for (size_t i = 0; i < count; ++i)
		if (value >= ranges[i].first && value <= ranges[i].last)
			return true;
	return false;
}

static vmf_status condition(const payload_state *state,
	const vmf_condition *definition, bool *result)
{
	bool all = definition->mode == VMF_CONDITION_ALL;
	bool value = all;

	for (size_t i = 0; i < definition->term_count; ++i) {
		const vmf_condition_term *term = &definition->terms[i];
		uint64_t code;

		if (!lookup(state, term->field, &code))
			return VMF_INVALID_SCHEMA;
		bool term_value = in_ranges(code & term->mask, term->values,
					  term->value_count) != term->negate;

		value = all ? value && term_value : value || term_value;
	}
	*result = value != definition->negate;
	return VMF_OK;
}

static vmf_status code_io(payload_state *state, vmf_input *input,
	vmf_output *output, uint8_t width, vmf_bit_order order, uint64_t *value,
	bool encoding)
{
	vmf_status status;
	uint64_t maximum = state->limits.max_message_octets;

	if (maximum <= UINT64_MAX / 8 &&
		(state->bits.offset > maximum * 8 ||
			(uint64_t)(width - state->bits.completed) >
				maximum * 8 - state->bits.offset))
		return VMF_LIMIT_EXCEEDED;
	if (encoding)
		status = write_bits(&state->bits, output, width, order, *value,
			state->counting);
	else
		status = read_bits(
			&state->bits, input, width, order, state->bound, value);
	if (state->bits.offset / 8 > state->limits.max_message_octets)
		return VMF_LIMIT_EXCEEDED;
	return status;
}

static vmf_status extent_bias(uint64_t value, int64_t bias, uint64_t *result)
{
	if (bias < 0) {
		uint64_t magnitude = (uint64_t)(-(bias + 1)) + 1;

		if (value < magnitude)
			return VMF_OUT_OF_RANGE;
		value -= magnitude;
	} else {
		if ((uint64_t)bias >= UINT64_MAX - value)
			return VMF_OUT_OF_RANGE;
		value += (uint64_t)bias;
	}
	if (value == UINT64_MAX)
		return VMF_OUT_OF_RANGE;
	*result = value;
	return VMF_OK;
}

static const vmf_extent_definition *field_extent(const vmf_node *node)
{
	switch (node->as.field.kind) {
	case VMF_FIELD_TEXT:
		return &node->as.field.as.text.length;
	case VMF_FIELD_OCTETS:
		return &node->as.field.as.octets.length;
	case VMF_FIELD_BITS:
		return &node->as.field.as.bits.length;
	default:
		return nullptr;
	}
}

static uint8_t unit_width(const vmf_node *node)
{
	if (node->as.field.kind == VMF_FIELD_BITS)
		return 1;
	if (node->as.field.kind == VMF_FIELD_TEXT &&
		node->as.field.as.text.encoding == VMF_TEXT_ASCII_7)
		return 7;
	return 8;
}

static vmf_status resolve_extent(payload_state *state, payload_frame *frame,
	const vmf_extent_definition *extent, vmf_input *input,
	vmf_output *output, const vmf_event *event, bool encoding)
{
	uint64_t value = 0;
	vmf_status status;
	uint8_t width = frame->node->kind == VMF_NODE_REPEAT
				? 1
				: unit_width(frame->node);

	if (frame->extent_ready)
		return VMF_OK;
	switch (extent->kind) {
	case VMF_EXTENT_FIXED:
		value = extent->fixed;
		break;
	case VMF_EXTENT_PREFIX:
		if (encoding) {
			value = frame->node->kind == VMF_NODE_REPEAT
					? event->as.group.count
					: event->as.field.total_units;
			if (value == VMF_UNITS_UNKNOWN)
				return VMF_LENGTH_REQUIRED;
			if (extent->bias == INT64_MIN) {
				if (value > INT64_MAX - UINT64_C(1))
					return VMF_OUT_OF_RANGE;
				value += UINT64_C(1) << 63;
			} else {
				status = extent_bias(
					value, -extent->bias, &value);
				if (status != VMF_OK)
					return status;
			}
			if (extent->prefix.bit_width < 64 &&
				value >> extent->prefix.bit_width)
				return VMF_OUT_OF_RANGE;
		}
		status = code_io(state, input, output, extent->prefix.bit_width,
			extent->prefix.order, &value, encoding);
		if (status != VMF_OK)
			return status;
		status = extent_bias(value, extent->bias, &value);
		if (status != VMF_OK)
			return status;
		break;
	case VMF_EXTENT_FIELD:
		if (!lookup(state, extent->field, &value))
			return VMF_INVALID_SCHEMA;
		status = extent_bias(value, extent->bias, &value);
		if (status != VMF_OK)
			return status;
		break;
	case VMF_EXTENT_REMAINING:
		if (extent->minimum == extent->maximum) {
			value = extent->minimum;
		} else if (state->bound == VMF_LENGTH_UNKNOWN) {
			if (width != 8)
				return VMF_LENGTH_REQUIRED;
			value = VMF_UNITS_UNKNOWN;
		} else {
			if (state->bits.offset > state->bound)
				return VMF_MALFORMED;
			value = (state->bound - state->bits.offset) / width;
		}
		break;
	case VMF_EXTENT_LITERAL:
		value = VMF_UNITS_UNKNOWN;
		break;
	case VMF_EXTENT_MORE_BEFORE:
	case VMF_EXTENT_MORE_AFTER:
	case VMF_EXTENT_RECURRENCE_BEFORE:
		value = VMF_UNITS_UNKNOWN;
		break;
	default:
		return VMF_INVALID_SCHEMA;
	}
	if (value != VMF_UNITS_UNKNOWN &&
		(value < extent->minimum || value > extent->maximum))
		return VMF_OUT_OF_RANGE;
	if (value != VMF_UNITS_UNKNOWN) {
		uint64_t maximum =
			frame->node->kind == VMF_NODE_REPEAT
				? state->limits.max_repetitions
				: state->limits.max_field_bits / width;

		if (value > maximum)
			return VMF_LIMIT_EXCEEDED;
	}
	frame->extent = value;
	frame->unknown = value == VMF_UNITS_UNKNOWN;
	if (encoding && frame->unknown &&
		frame->node->kind == VMF_NODE_REPEAT) {
		uint64_t count = event->as.group.count;

		if (count != VMF_UNITS_UNKNOWN &&
			(count < extent->minimum || count > extent->maximum))
			return VMF_OUT_OF_RANGE;
		frame->extent = count;
	}
	frame->extent_ready = true;
	return VMF_OK;
}

static void event_base(payload_state *state, payload_frame *frame,
	vmf_event *event, vmf_event_kind kind)
{
	*event = {};
	event->kind = kind;
	event->schema = state->schema;
	event->node = frame->node;
	event->depth = state->stack - 1;
	event->occurrence = frame->occurrence;
}

static vmf_status check_event(payload_state *state, payload_frame *frame,
	const vmf_event *event, vmf_event_kind kind)
{
	if (event->kind != kind || event->node != frame->node ||
		(event->schema && event->schema != state->schema) ||
		event->depth != state->stack - 1 ||
		event->occurrence != frame->occurrence)
		return VMF_INVALID_STATE;
	return VMF_OK;
}

static vmf_status delivered(payload_state *state)
{
	if (state->events == state->limits.max_events_per_message)
		return VMF_LIMIT_EXCEEDED;
	++state->events;
	return VMF_OK;
}

static vmf_status scalar_decode(payload_state *state, payload_frame *frame,
	vmf_input *input, vmf_event *event)
{
	const vmf_scalar_definition *definition =
		&frame->node->as.field.as.scalar;
	uint8_t width = definition->encoding.bit_width;
	uint64_t code;
	vmf_status status = code_io(state, input, nullptr, width,
		definition->encoding.order, &code, false);

	if (status != VMF_OK)
		return status;
	if (definition->allowed_count && !in_ranges(code, definition->allowed,
						 definition->allowed_count))
		return VMF_OUT_OF_RANGE;
	event_base(state, frame, event, VMF_EVENT_FIELD);
	event->as.field.total_units = 1;
	switch (frame->node->as.field.kind) {
	case VMF_FIELD_BOOL:
		event->as.field.value.boolean.value = code != 0;
		break;
	case VMF_FIELD_UNSIGNED:
		event->as.field.value.unsigned_integer.value = code;
		break;
	case VMF_FIELD_ENUM:
		event->as.field.value.enumeration.code = code;
		break;
	case VMF_FIELD_SCALED:
		event->as.field.value.scaled.code = code;
		break;
	case VMF_FIELD_SIGNED: {
		uint64_t sign = UINT64_C(1) << (width - 1);
		uint64_t mask =
			width == 64 ? UINT64_MAX : (UINT64_C(1) << width) - 1;
		int64_t value;

		if (!(code & sign)) {
			value = (int64_t)code;
		} else if (definition->encoding.representation ==
			   VMF_INTEGER_TWOS_COMPLEMENT) {
			value = -(int64_t)((~code) & mask) - 1;
		} else {
			uint64_t magnitude =
				definition->encoding.representation ==
						VMF_INTEGER_ONES_COMPLEMENT
					? (~code & mask)
					: (code & ~sign);

			if (!magnitude)
				return VMF_OUT_OF_RANGE;
			value = -(int64_t)magnitude;
		}
		event->as.field.value.signed_integer.value = value;
		break;
	}
	default:
		return VMF_INVALID_SCHEMA;
	}
	status = remember(state, frame->node, code);
	if (status == VMF_OK)
		frame->phase = POP;
	return status;
}

static vmf_status scalar_encode(payload_state *state, payload_frame *frame,
	vmf_output *output, const vmf_event *event)
{
	vmf_status status = check_event(state, frame, event, VMF_EVENT_FIELD);

	if (status != VMF_OK)
		return status;
	if (event->as.field.offset_units || event->as.field.total_units != 1)
		return VMF_OUT_OF_RANGE;
	const vmf_scalar_definition *definition =
		&frame->node->as.field.as.scalar;
	uint8_t width = definition->encoding.bit_width;
	uint64_t code;

	switch (frame->node->as.field.kind) {
	case VMF_FIELD_BOOL:
		code = event->as.field.value.boolean.value;
		break;
	case VMF_FIELD_UNSIGNED:
		code = event->as.field.value.unsigned_integer.value;
		break;
	case VMF_FIELD_ENUM:
		code = event->as.field.value.enumeration.code;
		break;
	case VMF_FIELD_SCALED:
		code = event->as.field.value.scaled.code;
		break;
	case VMF_FIELD_SIGNED: {
		int64_t value = event->as.field.value.signed_integer.value;
		uint64_t sign = UINT64_C(1) << (width - 1);
		uint64_t magnitude = value < 0 ? (uint64_t)(-(value + 1)) + 1
					       : (uint64_t)value;
		bool twos = definition->encoding.representation ==
			    VMF_INTEGER_TWOS_COMPLEMENT;

		if ((value >= 0 && magnitude >= sign) ||
			(value < 0 && magnitude > sign - !twos))
			return VMF_OUT_OF_RANGE;
		if (value >= 0)
			code = magnitude;
		else if (twos)
			code = UINT64_C(0) - magnitude;
		else if (definition->encoding.representation ==
			 VMF_INTEGER_ONES_COMPLEMENT)
			code = ~magnitude;
		else
			code = sign | magnitude;
		if (width < 64)
			code &= (UINT64_C(1) << width) - 1;
		break;
	}
	default:
		return VMF_INVALID_SCHEMA;
	}
	if ((width < 64 && code >> width) ||
		(definition->allowed_count &&
			!in_ranges(code, definition->allowed,
				definition->allowed_count)))
		return VMF_OUT_OF_RANGE;
	status = code_io(state, nullptr, output, width,
		definition->encoding.order, &code, true);
	if (status != VMF_OK)
		return status;
	status = remember(state, frame->node, code);
	if (status == VMF_OK)
		frame->phase = POP;
	return status;
}

static vmf_status append_utf8(payload_frame *frame, uint8_t code)
{
	if (!frame->utf8_need) {
		frame->utf8_size = 0;
		if (code < 0x80) {
			frame->pending[0] = code;
			frame->pending_size = 1;
			frame->pending_units = 1;
			return VMF_OK;
		}
		if (code >= 0xc2 && code <= 0xdf)
			frame->utf8_need = 2;
		else if (code >= 0xe0 && code <= 0xef)
			frame->utf8_need = 3;
		else if (code >= 0xf0 && code <= 0xf4)
			frame->utf8_need = 4;
		else
			return VMF_MALFORMED;
	} else if ((code & 0xc0) != 0x80) {
		return VMF_MALFORMED;
	}
	frame->utf8[frame->utf8_size++] = code;
	if (frame->utf8_size != frame->utf8_need)
		return VMF_OK;
	uint8_t first = frame->utf8[0];
	uint8_t second = frame->utf8[1];

	if ((first == 0xe0 && second < 0xa0) ||
		(first == 0xed && second >= 0xa0) ||
		(first == 0xf0 && second < 0x90) ||
		(first == 0xf4 && second >= 0x90))
		return VMF_MALFORMED;
	std::memcpy(frame->pending, frame->utf8, frame->utf8_need);
	frame->pending_size = frame->utf8_need;
	frame->pending_units = frame->utf8_need;
	frame->utf8_need = 0;
	frame->utf8_size = 0;
	return VMF_OK;
}

static vmf_status field_bounds(
	payload_state *state, payload_frame *frame, uint64_t units)
{
	const vmf_extent_definition *extent = field_extent(frame->node);

	if (units > extent->maximum)
		return VMF_OUT_OF_RANGE;
	if (units > state->limits.max_field_bits / unit_width(frame->node))
		return VMF_LIMIT_EXCEEDED;
	return VMF_OK;
}

static vmf_status emit_chunk(
	payload_state *state, payload_frame *frame, vmf_event *event, bool last)
{
	const vmf_extent_definition *extent = field_extent(frame->node);

	if (last && (frame->units < extent->minimum ||
			    frame->units > extent->maximum))
		return VMF_OUT_OF_RANGE;
	event_base(state, frame, event, VMF_EVENT_FIELD);
	event->as.field.offset_units =
		frame->units - frame->chunk_units - frame->pending_units;
	event->as.field.total_units =
		frame->unknown ? VMF_UNITS_UNKNOWN : frame->extent;
	switch (frame->node->as.field.kind) {
	case VMF_FIELD_TEXT:
		event->as.field.value.text.data =
			reinterpret_cast<const char *>(state->chunk);
		event->as.field.value.text.size = frame->chunk_bytes;
		event->as.field.value.text.last = last;
		break;
	case VMF_FIELD_OCTETS:
		event->as.field.value.octets.data = state->chunk;
		event->as.field.value.octets.size = frame->chunk_bytes;
		event->as.field.value.octets.last = last;
		break;
	case VMF_FIELD_BITS:
		event->as.field.value.bits.data = state->chunk;
		event->as.field.value.bits.bit_count = frame->chunk_units;
		event->as.field.value.bits.last = last;
		break;
	default:
		return VMF_INVALID_SCHEMA;
	}
	frame->chunk_units = 0;
	frame->chunk_bytes = 0;
	if (last)
		frame->phase = POP;
	return VMF_OK;
}

static vmf_status field_decode(payload_state *state, payload_frame *frame,
	vmf_input *input, vmf_event *event)
{
	const vmf_node *node = frame->node;
	const vmf_extent_definition *extent = field_extent(node);
	uint8_t width = unit_width(node);
	vmf_bit_order order = node->as.field.kind == VMF_FIELD_TEXT
				      ? node->as.field.as.text.order
			      : node->as.field.kind == VMF_FIELD_OCTETS
				      ? node->as.field.as.octets.order
				      : VMF_BIT_LSB_FIRST;

	for (;;) {
		if (frame->pending_size) {
			if (frame->chunk_bytes + frame->pending_size >
				state->limits.field_chunk_bytes)
				return emit_chunk(state, frame, event, false);
			std::memcpy(state->chunk + frame->chunk_bytes,
				frame->pending, frame->pending_size);
			frame->chunk_bytes += frame->pending_size;
			frame->chunk_units += frame->pending_units;
			frame->pending_size = 0;
			frame->pending_units = 0;
		}
		bool done = !frame->unknown && frame->units == frame->extent;

		if (extent->kind == VMF_EXTENT_LITERAL &&
			(frame->literal_done ||
				frame->units == extent->maximum))
			done = true;
		if (frame->unknown && extent->kind == VMF_EXTENT_REMAINING &&
			state->bits.available < width &&
			input->used == input->size) {
			if (!input->final)
				return VMF_NEED_INPUT;
			done = true;
		}
		if (done) {
			if (frame->utf8_need)
				return VMF_MALFORMED;
			return emit_chunk(state, frame, event, true);
		}
		if (node->as.field.kind == VMF_FIELD_BITS &&
			frame->chunk_units ==
				state->limits.field_chunk_bytes * 8)
			return emit_chunk(state, frame, event, false);
		uint64_t code;
		vmf_status status = code_io(
			state, input, nullptr, width, order, &code, false);

		if (status != VMF_OK)
			return status;
		if (extent->kind == VMF_EXTENT_LITERAL && code == 127) {
			frame->literal_done = true;
			continue;
		}
		if (frame->units == UINT64_MAX - 1)
			return VMF_LIMIT_EXCEEDED;
		++frame->units;
		status = field_bounds(state, frame, frame->units);
		if (status != VMF_OK)
			return status;
		if (node->as.field.kind == VMF_FIELD_BITS) {
			size_t position = (size_t)frame->chunk_units;

			if (!(position % 8))
				state->chunk[position / 8] = 0;
			state->chunk[position / 8] |= code << (position % 8);
			++frame->chunk_units;
			frame->chunk_bytes = (frame->chunk_units + 7) / 8;
			continue;
		}
		if (node->as.field.kind == VMF_FIELD_OCTETS) {
			frame->pending[0] = (uint8_t)code;
			frame->pending_size = 1;
			frame->pending_units = 1;
			continue;
		}
		switch (node->as.field.as.text.encoding) {
		case VMF_TEXT_ASCII_7:
		case VMF_TEXT_ASCII_8:
			if (code > 127)
				return VMF_MALFORMED;
			frame->pending[0] = (uint8_t)code;
			frame->pending_size = 1;
			frame->pending_units = 1;
			break;
		case VMF_TEXT_LATIN_1:
			if (code < 128) {
				frame->pending[0] = (uint8_t)code;
				frame->pending_size = 1;
			} else {
				frame->pending[0] = 0xc0 | (code >> 6);
				frame->pending[1] = 0x80 | (code & 63);
				frame->pending_size = 2;
			}
			frame->pending_units = 1;
			break;
		case VMF_TEXT_UTF_8:
			status = append_utf8(frame, (uint8_t)code);
			if (status != VMF_OK)
				return status;
			break;
		default:
			return VMF_INVALID_SCHEMA;
		}
	}
}

static vmf_status utf8_character(const uint8_t *data, size_t size,
	size_t offset, uint32_t *character, size_t *length)
{
	if (offset >= size)
		return VMF_MALFORMED;
	uint8_t first = data[offset];
	uint32_t value;
	size_t count;

	if (first < 0x80) {
		*character = first;
		*length = 1;
		return VMF_OK;
	}
	if (first >= 0xc2 && first <= 0xdf) {
		count = 2;
		value = first & 31;
	} else if (first >= 0xe0 && first <= 0xef) {
		count = 3;
		value = first & 15;
	} else if (first >= 0xf0 && first <= 0xf4) {
		count = 4;
		value = first & 7;
	} else {
		return VMF_MALFORMED;
	}
	if (count > size - offset)
		return VMF_MALFORMED;
	for (size_t i = 1; i < count; ++i) {
		uint8_t code = data[offset + i];

		if ((code & 0xc0) != 0x80)
			return VMF_MALFORMED;
		value = (value << 6) | (code & 63);
	}
	if ((count == 2 && value < 0x80) || (count == 3 && value < 0x800) ||
		(count == 4 && value < 0x10000) ||
		(value >= 0xd800 && value <= 0xdfff) || value > 0x10ffff)
		return VMF_MALFORMED;
	*character = value;
	*length = count;
	return VMF_OK;
}

static vmf_status chunk_description(const vmf_node *node,
	const vmf_event *event, const uint8_t **data, size_t *size, bool *last,
	uint64_t *units)
{
	const vmf_field *value = &event->as.field.value;

	switch (node->as.field.kind) {
	case VMF_FIELD_TEXT: {
		*data = reinterpret_cast<const uint8_t *>(value->text.data);
		*size = value->text.size;
		*last = value->text.last;
		*units = 0;
		if (!*data && *size)
			return VMF_INVALID_ARGUMENT;
		for (size_t offset = 0; offset < *size;) {
			uint32_t character;
			size_t length;
			vmf_status status = utf8_character(
				*data, *size, offset, &character, &length);

			if (status != VMF_OK)
				return status;
			vmf_text_encoding encoding =
				node->as.field.as.text.encoding;

			if (((encoding == VMF_TEXT_ASCII_7 ||
				     encoding == VMF_TEXT_ASCII_8) &&
				    character > 127) ||
				(encoding == VMF_TEXT_LATIN_1 &&
					character > 255) ||
				(node->as.field.as.text.length.kind ==
						VMF_EXTENT_LITERAL &&
					character == 127))
				return VMF_OUT_OF_RANGE;
			*units += encoding == VMF_TEXT_UTF_8 ? length : 1;
			offset += length;
		}
		break;
	}
	case VMF_FIELD_OCTETS:
		*data = value->octets.data;
		*size = value->octets.size;
		*last = value->octets.last;
		*units = *size;
		break;
	case VMF_FIELD_BITS:
		*data = value->bits.data;
		*size = value->bits.bit_count;
		*last = value->bits.last;
		*units = *size;
		if (!*last && *size % 8)
			return VMF_OUT_OF_RANGE;
		if (!*data && *size)
			return VMF_INVALID_ARGUMENT;
		if (*size && *size % 8 && ((*data)[*size / 8] >> (*size % 8)))
			return VMF_OUT_OF_RANGE;
		break;
	default:
		return VMF_INVALID_SCHEMA;
	}
	if (!*data && *size)
		return VMF_INVALID_ARGUMENT;
	if (!*last && !*units)
		return VMF_OUT_OF_RANGE;
	return VMF_OK;
}

static vmf_status field_encode(payload_state *state, payload_frame *frame,
	vmf_output *output, const vmf_event *event)
{
	vmf_status status = check_event(state, frame, event, VMF_EVENT_FIELD);

	if (status != VMF_OK)
		return status;
	const vmf_node *node = frame->node;
	const vmf_extent_definition *extent = field_extent(node);
	const uint8_t *data;
	size_t size;
	bool last;
	uint64_t units;

	status = chunk_description(node, event, &data, &size, &last, &units);
	if (status != VMF_OK)
		return status;
	if (!frame->event_active) {
		if (frame->total_seen &&
			frame->declared_total != event->as.field.total_units)
			return VMF_OUT_OF_RANGE;
		frame->declared_total = event->as.field.total_units;
		frame->total_seen = true;
		if (event->as.field.offset_units != frame->units ||
			units >= UINT64_MAX - frame->units ||
			(!units && frame->units))
			return VMF_OUT_OF_RANGE;
		uint64_t total = frame->units + units;

		status = field_bounds(state, frame, total);
		if (status != VMF_OK)
			return status;
		if (!frame->unknown &&
			(event->as.field.total_units != frame->extent ||
				total > frame->extent ||
				last != (total == frame->extent)))
			return VMF_OUT_OF_RANGE;
		if (frame->unknown &&
			event->as.field.total_units != VMF_UNITS_UNKNOWN) {
			if (event->as.field.total_units < total ||
				last != (total ==
						event->as.field.total_units) ||
				(frame->extent != VMF_UNITS_UNKNOWN &&
					frame->extent !=
						event->as.field.total_units))
				return VMF_OUT_OF_RANGE;
			frame->extent = event->as.field.total_units;
		}
		if (last && total < extent->minimum)
			return VMF_OUT_OF_RANGE;
		frame->chunk_units = frame->units;
		frame->event_index = 0;
		frame->event_active = true;
	}
	if (event->as.field.offset_units != frame->chunk_units)
		return VMF_INVALID_STATE;
	while (frame->event_index < size) {
		uint64_t code;
		uint8_t width = unit_width(node);
		vmf_bit_order order;
		size_t consumed = 1;

		if (node->as.field.kind == VMF_FIELD_BITS) {
			code = (data[frame->event_index / 8] >>
				       (frame->event_index % 8)) &
			       1;
			order = VMF_BIT_LSB_FIRST;
		} else if (node->as.field.kind == VMF_FIELD_OCTETS) {
			code = data[frame->event_index];
			order = node->as.field.as.octets.order;
		} else {
			order = node->as.field.as.text.order;
			if (node->as.field.as.text.encoding == VMF_TEXT_UTF_8) {
				code = data[frame->event_index];
			} else {
				uint32_t character;

				status = utf8_character(data, size,
					frame->event_index, &character,
					&consumed);
				if (status != VMF_OK)
					return status;
				code = character;
			}
		}
		status = code_io(
			state, nullptr, output, width, order, &code, true);
		if (status != VMF_OK)
			return status;
		frame->event_index += consumed;
		++frame->units;
	}
	if (last && extent->kind == VMF_EXTENT_LITERAL &&
		frame->units < extent->maximum && !frame->literal_done) {
		uint64_t terminator = 127;

		status = code_io(state, nullptr, output, 7,
			node->as.field.as.text.order, &terminator, true);
		if (status != VMF_OK)
			return status;
		frame->literal_done = true;
	}
	frame->event_active = false;
	frame->phase = last ? POP : VALUE;
	return VMF_OK;
}

static vmf_status select_choice(payload_state *state, payload_frame *frame)
{
	const vmf_choice_schema *choice = &frame->node->as.choice;
	uint64_t value;

	if (!lookup(state, choice->selector, &value))
		return VMF_INVALID_SCHEMA;
	value &= choice->mask;
	frame->selected = choice->otherwise;
	for (size_t i = 0; i < choice->case_count; ++i) {
		const vmf_choice_case *item = &choice->cases[i];

		if (in_ranges(value, item->values, item->value_count)) {
			frame->selected = item->body;
			break;
		}
	}
	return frame->selected ? VMF_OK : VMF_OUT_OF_RANGE;
}

static vmf_status group_event(payload_state *state, payload_frame *frame,
	vmf_event *result, const vmf_event *source, bool encoding, bool begin)
{
	vmf_event_kind kind =
		begin ? VMF_EVENT_GROUP_BEGIN : VMF_EVENT_GROUP_END;
	uint64_t count = 1;

	if (frame->node->kind == VMF_NODE_REPEAT)
		count = begin ? frame->extent : frame->index;
	if (encoding) {
		vmf_status status = check_event(state, frame, source, kind);

		if (status != VMF_OK)
			return status;
		if (source->as.group.count != count &&
			!(begin && frame->unknown &&
				source->as.group.count == VMF_UNITS_UNKNOWN))
			return VMF_OUT_OF_RANGE;
		if (begin && frame->more_present &&
			source->as.group.more_present &&
			source->as.group.more != frame->more)
			return VMF_INVALID_STATE;
		if (source->as.group.more_present && !frame->more_present)
			return VMF_INVALID_STATE;
	} else {
		event_base(state, frame, result, kind);
		result->as.group.count = count;
		if (begin) {
			result->as.group.more_present = frame->more_present;
			result->as.group.more = frame->more;
		}
	}
	frame->phase = begin ? CHILDREN : POP;
	return VMF_OK;
}

static vmf_status repeat_child(payload_state *state, payload_frame *frame,
	vmf_input *input, vmf_output *output, const vmf_event *event,
	bool encoding)
{
	const vmf_repeat_schema *repeat = &frame->node->as.repeat;
	const vmf_extent_definition *extent = &repeat->count;
	bool body = false;
	bool more = false;
	bool leading = extent->kind == VMF_EXTENT_RECURRENCE_BEFORE;

	if (!frame->unknown) {
		body = frame->index < frame->extent;
	} else if (extent->kind == VMF_EXTENT_MORE_AFTER && !frame->index) {
		body = true;
	} else if (leading && frame->index && !frame->more) {
		body = false;
	} else {
		uint64_t flag = 0;

		if (encoding) {
			body = event->kind == VMF_EVENT_GROUP_BEGIN &&
			       event->node == repeat->body;
			if (!body && !(event->kind == VMF_EVENT_GROUP_END &&
					     event->node == frame->node))
				return VMF_INVALID_STATE;
			if (leading) {
				if (!body)
					return VMF_INVALID_STATE;
				if (frame->extent != VMF_UNITS_UNKNOWN) {
					if (frame->index >= frame->extent)
						return VMF_OUT_OF_RANGE;
					more = frame->index + 1 < frame->extent;
					if (event->as.group.more_present &&
						event->as.group.more != more)
						return VMF_INVALID_STATE;
				} else {
					if (!event->as.group.more_present)
						return VMF_INVALID_STATE;
					more = event->as.group.more;
				}
				flag = more == extent->more_bit;
			} else {
				flag = body == extent->more_bit;
			}
		}
		vmf_status status = code_io(state, input, output, 1,
			VMF_BIT_LSB_FIRST, &flag, encoding);

		if (status != VMF_OK)
			return status;
		if (!encoding) {
			more = (flag != 0) == extent->more_bit;
			body = leading || more;
		}
		if (leading)
			frame->more = more;
	}
	if (!body) {
		if (frame->index < extent->minimum ||
			frame->index > extent->maximum ||
			(frame->extent != VMF_UNITS_UNKNOWN &&
				frame->index != frame->extent))
			return VMF_OUT_OF_RANGE;
		frame->phase = END;
		return VMF_OK;
	}
	if (frame->index >= extent->maximum)
		return VMF_OUT_OF_RANGE;
	if (frame->index >= state->limits.max_repetitions ||
		state->stack > state->limits.max_depth)
		return VMF_LIMIT_EXCEEDED;
	push(state, repeat->body, frame->index++, leading, frame->more);
	return VMF_OK;
}

static vmf_status finish(payload_state *state, vmf_input *input,
	vmf_output *output, bool encoding)
{
	while (state->bits.offset % 8) {
		uint64_t padding = 0;
		vmf_status status = code_io(state, input, output, 1,
			VMF_BIT_LSB_FIRST, &padding, encoding);

		if (status != VMF_OK)
			return status;
		if (padding)
			return VMF_MALFORMED;
	}
	/* A final completed octet may still await output capacity. */
	if (encoding && state->bits.available == 8) {
		if (!state->counting && output->used == output->capacity)
			return VMF_NEED_OUTPUT;
		if (!state->counting)
			output->data[output->used++] = state->bits.octet;
		state->bits.available = 0;
		state->bits.completed = 0;
		state->bits.octet = 0;
	}
	if (!state->counting && state->bound != VMF_LENGTH_UNKNOWN &&
		state->bits.offset != state->bound)
		return VMF_MALFORMED;
	if (!encoding && state->bound == VMF_LENGTH_UNKNOWN) {
		if (input->used < input->size)
			return VMF_MALFORMED;
		if (!input->final)
			return VMF_NEED_INPUT;
	}
	state->finished = true;
	return encoding ? VMF_OK : VMF_DONE;
}

static vmf_status traverse(payload_state *state, vmf_input *input,
	vmf_output *output, vmf_event *result, const vmf_event *source,
	bool encoding)
{
	for (;;) {
		if (!state->stack) {
			if (encoding && source->kind != VMF_EVENT_MESSAGE_END)
				return VMF_INVALID_STATE;
			state->error_node = 0;
			return finish(state, input, output, encoding);
		}
		payload_frame *frame = &state->frames[state->stack - 1];
		const vmf_node *node = frame->node;
		vmf_status status;

		state->error_node = node->id;
		switch (frame->phase) {
		case PRESENCE: {
			bool present = true;

			if (node->presence.kind == VMF_PRESENCE_BIT) {
				uint64_t flag = 0;

				if (encoding) {
					present = source->kind !=
						  VMF_EVENT_ABSENT;
					flag = present ==
					       node->presence.present_bit;
				}
				status = code_io(state, input, output, 1,
					VMF_BIT_LSB_FIRST, &flag, encoding);
				if (status != VMF_OK)
					return status;
				present = (flag != 0) ==
					  node->presence.present_bit;
			} else if (node->presence.kind ==
				   VMF_PRESENCE_CONDITION) {
				status = condition(state,
					&node->presence.condition, &present);
				if (status != VMF_OK)
					return status;
			}
			frame->present = present;
			frame->phase = EXTENT;
			if (!present) {
				if (encoding)
					status = check_event(state, frame,
						source, VMF_EVENT_ABSENT);
				else {
					event_base(state, frame, result,
						VMF_EVENT_ABSENT);
					status = VMF_OK;
				}
				if (status != VMF_OK)
					return status;
				frame->phase = POP;
				return delivered(state);
			}
			break;
		}
		case EXTENT:
			if (node->kind == VMF_NODE_FIELD) {
				const vmf_extent_definition *extent =
					field_extent(node);

				if (extent) {
					if (encoding) {
						status = check_event(state,
							frame, source,
							VMF_EVENT_FIELD);
						if (status != VMF_OK)
							return status;
					}
					status = resolve_extent(state, frame,
						extent, input, output, source,
						encoding);
					if (status != VMF_OK)
						return status;
				}
				frame->phase = VALUE;
			} else {
				if (node->kind == VMF_NODE_REPEAT) {
					if (encoding) {
						status = check_event(state,
							frame, source,
							VMF_EVENT_GROUP_BEGIN);
						if (status != VMF_OK)
							return status;
					}
					status = resolve_extent(state, frame,
						&node->as.repeat.count, input,
						output, source, encoding);
					if (status != VMF_OK)
						return status;
				} else if (node->kind == VMF_NODE_CHOICE) {
					status = select_choice(state, frame);
					if (status != VMF_OK)
						return status;
				}
				clear_scope(state, state->stack - 1);
				frame->phase = BEGIN;
			}
			break;
		case BEGIN:
			status = group_event(
				state, frame, result, source, encoding, true);
			return status == VMF_OK ? delivered(state) : status;
		case VALUE:
			if (field_extent(node))
				status = encoding ? field_encode(state, frame,
							    output, source)
						  : field_decode(state, frame,
							    input, result);
			else
				status = encoding ? scalar_encode(state, frame,
							    output, source)
						  : scalar_decode(state, frame,
							    input, result);
			return status == VMF_OK ? delivered(state) : status;
		case CHILDREN:
			if (node->kind == VMF_NODE_REPEAT) {
				status = repeat_child(state, frame, input,
					output, source, encoding);
				if (status != VMF_OK)
					return status;
			} else if (node->kind == VMF_NODE_GROUP &&
				   frame->index < node->as.group.child_count) {
				if (state->stack > state->limits.max_depth)
					return VMF_LIMIT_EXCEEDED;
				push(state,
					&node->as.group
						.children[frame->index++],
					0);
			} else if (node->kind == VMF_NODE_CHOICE &&
				   !frame->index) {
				if (state->stack > state->limits.max_depth)
					return VMF_LIMIT_EXCEEDED;
				++frame->index;
				push(state, frame->selected, 0);
			} else {
				frame->phase = END;
			}
			break;
		case END:
			status = group_event(
				state, frame, result, source, encoding, false);
			return status == VMF_OK ? delivered(state) : status;
		case POP:
			if (node->kind != VMF_NODE_FIELD)
				clear_scope(state, state->stack - 1);
			--state->stack;
			break;
		}
	}
}

vmf_status payload_decode(
	payload_state *state, vmf_input *input, vmf_event *event)
{
	if (!state || !input || !event || input->used > input->size ||
		(!input->data && input->size))
		return VMF_INVALID_ARGUMENT;
	*event = {};
	if (state->begin_error != VMF_OK)
		return state->begin_error;
	if (state->finished)
		return VMF_DONE;
	vmf_status status =
		traverse(state, input, nullptr, event, nullptr, false);

	if (status != VMF_OK)
		*event = {};
	return status;
}

vmf_status payload_encode(payload_state *state, vmf_output *output,
	const vmf_event *event, bool counting)
{
	if (!state || !event || counting != state->counting ||
		(!counting && (!output || output->used > output->capacity ||
				      (!output->data && output->capacity))))
		return VMF_INVALID_ARGUMENT;
	if (state->begin_error != VMF_OK)
		return state->begin_error;
	if (state->finished)
		return VMF_INVALID_STATE;
	return traverse(state, nullptr, output, nullptr, event, true);
}

vmf_status payload_field_context(const payload_state *state,
	const vmf_node *node, size_t *depth, uint64_t *occurrence,
	uint64_t *offset)
{
	if (!state || !node || !depth || !occurrence || !offset)
		return VMF_INVALID_ARGUMENT;
	if (!state->stack)
		return VMF_INVALID_STATE;
	const payload_frame *frame = &state->frames[state->stack - 1];

	if (frame->node == node) {
		*depth = state->stack - 1;
		*occurrence = frame->occurrence;
		*offset =
			frame->event_active ? frame->chunk_units : frame->units;
		return VMF_OK;
	}
	if (frame->phase == CHILDREN && frame->node->kind == VMF_NODE_GROUP &&
		frame->index < frame->node->as.group.child_count &&
		&frame->node->as.group.children[frame->index] == node) {
		*depth = state->stack;
		*occurrence = 0;
		*offset = 0;
		return VMF_OK;
	}
	/* A completed sibling remains until the next event advances traversal.
	 */
	if (frame->phase == POP && state->stack > 1) {
		const payload_frame *parent = &state->frames[state->stack - 2];

		if (parent->node->kind == VMF_NODE_GROUP &&
			parent->index < parent->node->as.group.child_count &&
			&parent->node->as.group.children[parent->index] ==
				node) {
			*depth = state->stack - 1;
			*occurrence = 0;
			*offset = 0;
			return VMF_OK;
		}
	}
	return VMF_INVALID_STATE;
}

} /* namespace vmf_internal */
