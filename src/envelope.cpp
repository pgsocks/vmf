#include "envelope.hpp"

#include <cstring>

namespace vmf_internal
{

struct header_io {
	bool reading;
	bool final;
	bool counting;
	vmf_envelope_version profile;
	const uint8_t *input;
	uint8_t *output;
	size_t size;
	size_t bit;
	size_t needed;
	size_t header_bound;
	uint8_t *arena;
	size_t arena_capacity;
	size_t arena_used;
	size_t items;
	size_t message_index;
	const vmf_limits *limits;
	vmf_error error;
};

static bool fail(header_io *io, vmf_status status)
{
	if (io->error.status == VMF_OK)
		io->error = {status, io->bit, 0, io->message_index};
	return false;
}

static bool require(header_io *io, bool valid)
{
	return valid ||
	       fail(io, io->reading ? VMF_MALFORMED : VMF_OUT_OF_RANGE);
}

static bool items(header_io *io, size_t count)
{
	if (count > io->limits->max_header_items - io->items)
		return fail(io, VMF_LIMIT_EXCEEDED);
	io->items += count;
	return true;
}

static void *take(header_io *io, size_t size, size_t alignment)
{
	uintptr_t address = reinterpret_cast<uintptr_t>(io->arena);
	size_t padding = (alignment - (address + io->arena_used) % alignment) %
			 alignment;

	if (padding > io->arena_capacity - io->arena_used ||
		size > io->arena_capacity - io->arena_used - padding) {
		fail(io, VMF_LIMIT_EXCEEDED);
		return nullptr;
	}
	void *result = io->arena + io->arena_used + padding;

	io->arena_used += padding + size;
	return result;
}

static bool available(header_io *io, size_t bits)
{
	if (bits > SIZE_MAX - io->bit)
		return fail(io, VMF_LIMIT_EXCEEDED);
	size_t end = io->bit + bits;
	size_t bytes = end / 8 + (end % 8 != 0);

	if (bytes > io->limits->max_header_bytes)
		return fail(io, VMF_LIMIT_EXCEEDED);
	if (io->header_bound && !require(io, bytes <= io->header_bound))
		return false;
	if (bytes <= io->size)
		return true;
	if (!io->reading)
		return fail(io, VMF_BUFFER_TOO_SMALL);
	io->needed = bytes;
	return fail(io, io->final ? VMF_TRUNCATED : VMF_NEED_INPUT);
}

static bool number(header_io *io, uint32_t *value, unsigned width)
{
	if (!io->reading && !require(io, *value < (UINT32_C(1) << width)))
		return false;
	if (!available(io, width))
		return false;
	if (io->reading)
		*value = 0;
	for (unsigned i = 0; i < width; ++i) {
		size_t position = io->bit++;
		uint8_t mask = (uint8_t)(1u << (position % 8));

		if (io->reading) {
			if (io->input[position / 8] & mask)
				*value |= UINT32_C(1) << i;
		} else if ((*value >> i) & 1u) {
			io->output[position / 8] |= mask;
		} else {
			io->output[position / 8] &= (uint8_t)~mask;
		}
	}
	return true;
}

static bool small(header_io *io, uint8_t *value, unsigned width)
{
	uint32_t temporary = *value;

	if (!number(io, &temporary, width))
		return false;
	*value = (uint8_t)temporary;
	return true;
}

static bool flag(header_io *io, bool *value)
{
	uint32_t temporary = *value;

	if (!number(io, &temporary, 1))
		return false;
	*value = temporary != 0;
	return true;
}

static bool optional(header_io *io, vmf_optional_u32 *value, unsigned width,
	bool measured = false)
{
	if (!flag(io, &value->present))
		return false;
	if (!value->present)
		return true;
	if (!io->reading && io->counting && measured)
		value->value = 0;
	return number(io, &value->value, width);
}

static bool optional_small(
	header_io *io, vmf_optional_u8 *value, unsigned width)
{
	return flag(io, &value->present) &&
	       (!value->present || small(io, &value->value, width));
}

static bool literal(header_io *io, vmf_header_text *text, size_t maximum)
{
	if (!flag(io, &text->present) || !text->present)
		return io->error.status == VMF_OK;
	if (!items(io, 1))
		return false;
	if (!io->reading) {
		if (!require(io,
			    text->value.size <= maximum &&
				    (text->value.data || !text->value.size)))
			return false;
		for (size_t i = 0; i < text->value.size; ++i) {
			uint32_t code = (unsigned char)text->value.data[i];

			if (!require(io, code < 127) || !number(io, &code, 7))
				return false;
		}
		uint32_t terminator = 127;

		return text->value.size == maximum ||
		       number(io, &terminator, 7);
	}
	char temporary[64];
	size_t count = 0;

	for (; count < maximum; ++count) {
		uint32_t code = 0;

		if (!number(io, &code, 7))
			return false;
		if (code == 127)
			break;
		temporary[count] = (char)code;
	}
	char *copy = static_cast<char *>(take(io, count, 1));

	if (!copy)
		return false;
	std::memcpy(copy, temporary, count);
	text->value = {copy, count};
	return true;
}

static bool address(header_io *io, vmf_address *value, bool grouped)
{
	if (grouped) {
		if (!flag(io, &value->present) || !value->present)
			return io->error.status == VMF_OK;
	} else if (io->reading) {
		value->present = true;
	} else if (!require(io, value->present)) {
		return false;
	}
	return optional(io, &value->urn, 24) && literal(io, &value->name, 64) &&
	       require(io, value->urn.present != value->name.present);
}

static bool addresses(header_io *io, const vmf_address **values, size_t *count,
	size_t maximum)
{
	bool present = *count != 0;

	if (!flag(io, &present))
		return false;
	if (!present) {
		if (io->reading)
			*count = 0;
		return true;
	}
	if (!io->reading && !require(io, *values && *count <= maximum))
		return false;
	vmf_address temporary[16] = {};
	size_t length = 0;
	bool more;

	do {
		if (!require(io, length < maximum) || !items(io, 1))
			return false;
		more = !io->reading && length + 1 < *count;
		vmf_address item =
			io->reading ? vmf_address{} : (*values)[length];

		if (!flag(io, &more) || !address(io, &item, false))
			return false;
		temporary[length++] = item;
	} while (more);
	if (io->reading) {
		auto *copy = static_cast<vmf_address *>(take(io,
			length * sizeof(vmf_address), alignof(vmf_address)));

		if (!copy)
			return false;
		std::memcpy(copy, temporary, length * sizeof(vmf_address));
		*values = copy;
		*count = length;
	}
	return true;
}

static bool dtg(header_io *io, vmf_dtg *value, bool grouped, bool extended)
{
	if (grouped) {
		if (!flag(io, &value->present) || !value->present)
			return io->error.status == VMF_OK;
	} else if (io->reading) {
		value->present = true;
	} else if (!require(io, value->present)) {
		return false;
	}
	if (!small(io, &value->year, 7) || !small(io, &value->month, 4) ||
		!small(io, &value->day, 5) || !small(io, &value->hour, 5) ||
		!small(io, &value->minute, 6) || !small(io, &value->second, 6))
		return false;
	if (extended) {
		if (!optional(io, &value->extension, 12))
			return false;
	} else if (!require(io, !value->extension.present)) {
		return false;
	}
	return require(io, value->year <= 99 && value->month >= 1 &&
				   value->month <= 12 && value->day >= 1 &&
				   value->day <= 31 && value->hour <= 23 &&
				   value->minute <= 59 &&
				   (value->second <= 59 ||
					   (io->profile >= VMF_ENVELOPE_D &&
						   value->second == 63)));
}

static bool response(header_io *io, vmf_response *value)
{
	if (!flag(io, &value->present) || !value->present)
		return io->error.status == VMF_OK;
	if (!dtg(io, &value->acknowledged, false, true) ||
		!small(io, &value->receipt_compliance, 3) ||
		!optional_small(io, &value->cantco_reason, 3) ||
		!optional_small(io, &value->cantpro_reason, 6) ||
		!literal(io, &value->amplification, 50))
		return false;
	return require(io, value->receipt_compliance >= 1 &&
				   value->receipt_compliance <= 6 &&
				   (value->receipt_compliance == 6 ||
					   !value->cantco_reason.present) &&
				   (value->receipt_compliance == 2 ||
					   !value->cantpro_reason.present) &&
				   (io->profile != VMF_ENVELOPE_B ||
					   value->receipt_compliance >= 4 ||
					   !value->amplification.present));
}

static bool references(header_io *io, vmf_message_header *message)
{
	bool present = message->reference_count != 0;

	if (!flag(io, &present) || !present)
		return io->error.status == VMF_OK;
	if (!io->reading && !require(io, message->references &&
						 message->reference_count <= 4))
		return false;
	vmf_reference temporary[4] = {};
	size_t count = 0;
	bool more;

	do {
		if (!require(io, count < 4) || !items(io, 1))
			return false;
		more = !io->reading && count + 1 < message->reference_count;
		vmf_reference item = io->reading ? vmf_reference{}
						 : message->references[count];

		if (!flag(io, &more) || !address(io, &item.originator, false) ||
			!dtg(io, &item.dtg, false, true))
			return false;
		if (io->profile == VMF_ENVELOPE_B) {
			if (!small(io, &item.functional_area, 4) ||
				!small(io, &item.message_number, 7))
				return false;
			bool valid_fad = message->id.user_message_format != 2 ||
				item.functional_area <= 10;

			if (!require(io, item.message_number >= 1 && valid_fad))
				return false;
		} else if (!require(io, item.functional_area == 0 &&
						item.message_number == 0)) {
			return false;
		}
		temporary[count++] = item;
	} while (more);
	if (io->reading) {
		auto *copy = static_cast<vmf_reference *>(take(io,
			count * sizeof(vmf_reference), alignof(vmf_reference)));

		if (!copy)
			return false;
		std::memcpy(copy, temporary, count * sizeof(vmf_reference));
		message->references = copy;
		message->reference_count = count;
	}
	return true;
}

static bool span(header_io *io, vmf_header_bytes *value, size_t length)
{
	if (!items(io, 1) || !available(io, length * 8))
		return false;
	uint8_t *copy = nullptr;

	if (io->reading) {
		copy = static_cast<uint8_t *>(take(io, length, 1));
		if (!copy)
			return false;
		*value = {true, copy, length};
	} else if (!require(io, value->present && value->size == length &&
					(value->data || length == 0))) {
		return false;
	}
	for (size_t i = 0; i < length; ++i) {
		uint32_t code = io->reading ? 0 : value->data[i];

		if (!number(io, &code, 8))
			return false;
		if (io->reading)
			copy[i] = (uint8_t)code;
	}
	return true;
}

static bool security_span(
	header_io *io, vmf_header_bytes *value, unsigned width, size_t unit)
{
	if (!flag(io, &value->present) || !value->present)
		return io->error.status == VMF_OK;
	if (!io->reading &&
		!require(
			io, value->size >= unit && value->size % unit == 0 &&
				    value->size / unit <= (size_t(1) << width)))
		return false;
	uint32_t code = io->reading ? 0 : (uint32_t)(value->size / unit - 1);

	return number(io, &code, width) && span(io, value, (code + 1) * unit);
}

static bool security(header_io *io, vmf_security *value)
{
	if (!flag(io, &value->present) || !value->present)
		return io->error.status == VMF_OK;
	if (!small(io, &value->spi, 4) ||
		!security_span(io, &value->keying_material, 3, 1) ||
		!security_span(io, &value->initialization, 4, 8) ||
		!flag(io, &value->tokens_present))
		return false;
	if (value->tokens_present) {
		if (!io->reading &&
			!require(io, value->tokens && value->token_count >= 1 &&
					     value->token_count <= 17))
			return false;
		uint32_t length = 0;

		if (!io->reading) {
			size_t size = value->tokens[0].size;

			if (!require(io,
				    size >= 8 && size <= 2048 && size % 8 == 0))
				return false;
			length = (uint32_t)(size / 8 - 1);
		}
		if (!number(io, &length, 8))
			return false;
		vmf_header_bytes temporary[17] = {};
		size_t count = 0;
		bool more;

		do {
			if (!require(io, count < 17) || !items(io, 1))
				return false;
			more = !io->reading && count + 1 < value->token_count;
			vmf_header_bytes token = io->reading
							 ? vmf_header_bytes{}
							 : value->tokens[count];

			if (!flag(io, &more) ||
				!span(io, &token, (length + 1) * 8))
				return false;
			temporary[count++] = token;
		} while (more);
		if (io->reading) {
			auto *copy = static_cast<vmf_header_bytes *>(
				take(io, count * sizeof(vmf_header_bytes),
					alignof(vmf_header_bytes)));

			if (!copy)
				return false;
			std::memcpy(copy, temporary,
				count * sizeof(vmf_header_bytes));
			value->tokens = copy;
			value->token_count = count;
		}
	} else if (!require(io, value->token_count == 0)) {
		return false;
	}
	if (!security_span(io, &value->authentication_a, 7, 8) ||
		!security_span(io, &value->authentication_b, 7, 8) ||
		!flag(io, &value->signed_ack_request) ||
		!flag(io, &value->padding_present))
		return false;
	if (value->padding_present) {
		if (!small(io, &value->padding_length, 8) ||
			!flag(io, &value->padding.present))
			return false;
		if (value->padding.present &&
			!span(io, &value->padding, value->padding_length))
			return false;
	} else if (!require(io, !value->padding.present)) {
		return false;
	}
	if (value->spi == 0 &&
		!require(io,
			!value->keying_material.present &&
				!value->initialization.present &&
				!value->tokens_present &&
				!value->padding_present &&
				value->authentication_a.present &&
				value->authentication_a.size == 40 &&
				(!value->authentication_b.present ||
					value->authentication_b.size == 40)))
		return false;
	return true;
}

static bool future(header_io *io)
{
	for (unsigned i = 0; i < 5; ++i) {
		bool present = false;

		if (!flag(io, &present) || !require(io, !present))
			return false;
	}
	return true;
}

static bool markings(header_io *io, vmf_message_header *message)
{
	if (io->profile == VMF_ENVELOPE_B)
		return optional(io, &message->marking_b, 14);
	if (io->profile == VMF_ENVELOPE_C)
		return literal(io, &message->marking_c, 32);
	bool present = message->marking_count != 0;

	if (!flag(io, &present) || !present)
		return io->error.status == VMF_OK;
	if (!io->reading &&
		!require(io, message->markings && message->marking_count <= 16))
		return false;
	uint16_t temporary[16];
	size_t count = 0;
	bool more;

	do {
		if (!require(io, count < 16) || !items(io, 1))
			return false;
		more = !io->reading && count + 1 < message->marking_count;
		uint32_t code = io->reading ? 0 : message->markings[count];

		if (!flag(io, &more) || !number(io, &code, 9))
			return false;
		temporary[count++] = (uint16_t)code;
	} while (more);
	if (io->reading) {
		auto *copy = static_cast<uint16_t *>(
			take(io, count * sizeof(uint16_t), alignof(uint16_t)));

		if (!copy)
			return false;
		std::memcpy(copy, temporary, count * sizeof(uint16_t));
		message->markings = copy;
		message->marking_count = count;
	}
	return true;
}

static bool version_allowed(header_io *io, const vmf_message_id *id)
{
	if (!id->standard_version.present)
		return true;
	uint8_t code = id->standard_version.value;
	uint8_t umf = id->user_message_format;
	bool c = io->profile == VMF_ENVELOPE_C;
	bool valid = false;

	switch (umf) {
	case 0:
	case 5:
		valid = code <= (c ? 9 : 15);
		break;
	case 2:
		valid = code <= (c ? 9 : 10);
		break;
	case 3:
		valid = !c && code == 0;
		break;
	case 7:
		valid = code >= 6 && code <= (c ? 9 : 15);
		break;
	case 8:
		valid = code >= 3 && code <= (c ? 9 : 10);
		break;
	}
	return require(io, valid);
}

static bool message_rules(header_io *io, const vmf_message_header *value,
	const vmf_header_fields *header)
{
	const vmf_message_id *id = &value->id;
	uint8_t umf = id->user_message_format;
	bool b = io->profile == VMF_ENVELOPE_B;
	bool d = io->profile >= VMF_ENVELOPE_D;
	bool identified = umf == 2 || (!b && umf == 8);

	if (!require(io, umf <= (b ? 6 : 8)) || !version_allowed(io, id))
		return false;
	/* B does not impose C's enumerated non-VMF case on Link16/NITFS. */
	if (!require(io,
		    (!identified || id->identification_present) &&
			    (!(umf == 1 || umf == 4 || umf == 5 || umf == 6 ||
				     (!b && !identified)) ||
				    !id->identification_present) &&
			    (!id->subtype.present ||
				    id->identification_present) &&
			    (!d || umf != 2 || id->standard_version.present)))
		return false;
	if (id->identification_present &&
		!require(
			io, id->message_number >= 1 &&
				    (!identified || id->functional_area <= 10)))
		return false;
	if (!require(io, value->operation <= (b ? 2 : 3) &&
				 value->classification <= 3 &&
				 (d ? value->precedence <= 5
				    : (value->precedence == 2 ||
					      value->precedence >= 4)) &&
				 (!d || umf != 6 || value->precedence == 5)))
		return false;
	if (!require(io, umf != 1 || value->response.present ||
				 (value->file_name.present &&
					 value->message_size_octets.present)))
		return false;
	if (!require(io, !value->acknowledgment.present ||
				 header->originator.present))
		return false;
	bool requested = value->acknowledgment.present &&
			 (value->acknowledgment.machine ||
				 value->acknowledgment.operator_ack ||
				 value->acknowledgment.operator_reply);

	if (!require(io, !(requested || (d && value->retransmit)) ||
				 value->originator_dtg.present))
		return false;
	if (value->response.present &&
		!require(io, header->originator.present &&
				     header->recipient_count &&
				     !value->perishability.present &&
				     !value->acknowledgment.present &&
				     !header->compression.present))
		return false;
	if (value->security.present) {
		if (!require(io, !value->security.signed_ack_request ||
					 value->acknowledgment.present))
			return false;
		if (value->security.authentication_b.present &&
			!require(io,
				value->security.authentication_a.present &&
					value->response.present &&
					!value->security.signed_ack_request))
			return false;
	}
	if ((umf == 2 || umf == 1 || umf == 4 || umf == 8) &&
		value->response.cantpro_reason.present &&
		!require(io, value->response.cantpro_reason.value >= 1 &&
				     value->response.cantpro_reason.value <=
					     (b ? 25 : (d ? 32 : 31))))
		return false;
	if (!io->reading &&
		!require(io, value->has_body == !value->response.present))
		return false;
	return true;
}

static bool message(header_io *io, vmf_message_header *value,
	const vmf_header_fields *header)
{
	bool b = io->profile == VMF_ENVELOPE_B;
	bool d = io->profile >= VMF_ENVELOPE_D;

	if (!require(io, (!b || (!value->id.standard_version.present &&
					!value->security.present)) &&
				 (io->profile == VMF_ENVELOPE_B ||
					 !value->marking_b.present) &&
				 (io->profile == VMF_ENVELOPE_C ||
					 !value->marking_c.present) &&
				 (d || value->marking_count == 0)))
		return false;
	if (!small(io, &value->id.user_message_format, 4) ||
		(!b && !optional_small(io, &value->id.standard_version, 4)) ||
		!flag(io, &value->id.identification_present))
		return false;
	if (value->id.identification_present) {
		if (!small(io, &value->id.functional_area, 4) ||
			!small(io, &value->id.message_number, 7) ||
			!optional_small(io, &value->id.subtype, 7))
			return false;
	}
	if (!literal(io, &value->file_name, 64) ||
		!optional(io, &value->message_size_octets, 20, true) ||
		!small(io, &value->operation, 2) ||
		!flag(io, &value->retransmit) ||
		!small(io, &value->precedence, 3) ||
		!small(io, &value->classification, 2) || !markings(io, value) ||
		!dtg(io, &value->originator_dtg, true, true) ||
		!dtg(io, &value->perishability, true, false) ||
		!flag(io, &value->acknowledgment.present))
		return false;
	if (value->acknowledgment.present &&
		(!flag(io, &value->acknowledgment.machine) ||
			!flag(io, &value->acknowledgment.operator_ack) ||
			!flag(io, &value->acknowledgment.operator_reply)))
		return false;
	if (!response(io, &value->response) || !references(io, value) ||
		(d && !future(io)) || (!b && !security(io, &value->security)))
		return false;
	if (io->reading)
		value->has_body = !value->response.present;
	return message_rules(io, value, header);
}

static bool messages(header_io *io, vmf_header_fields *header)
{
	if (!io->reading &&
		!require(io, header->messages && header->message_count >= 1 &&
				     header->message_count <= 16))
		return false;
	vmf_message_header temporary[16] = {};
	size_t count = 0;
	bool more;

	do {
		io->message_index = count;
		if (!require(io, count < 16) || !items(io, 1))
			return false;
		more = !io->reading && count + 1 < header->message_count;
		vmf_message_header item = io->reading ? vmf_message_header{}
						      : header->messages[count];

		if (!flag(io, &more) || !message(io, &item, header))
			return false;
		temporary[count++] = item;
	} while (more);
	if (count > 1) {
		if (!require(io, io->profile == VMF_ENVELOPE_B ||
					 header->header_size_octets.present))
			return false;
		for (size_t i = 0; i < count; ++i) {
			io->message_index = i;
			if (!require(io,
				    temporary[i].message_size_octets.present))
				return false;
		}
	}
	io->message_index = SIZE_MAX;
	if (io->reading) {
		auto *copy = static_cast<vmf_message_header *>(
			take(io, count * sizeof(vmf_message_header),
				alignof(vmf_message_header)));

		if (!copy)
			return false;
		std::memcpy(
			copy, temporary, count * sizeof(vmf_message_header));
		header->messages = copy;
		header->message_count = count;
	}
	return true;
}

static bool unique_addresses(header_io *io, const vmf_header_fields *header)
{
	if (io->profile == VMF_ENVELOPE_B)
		return true;
	uint32_t seen[16];
	size_t count = 0;

	for (unsigned group = 0; group < 2; ++group) {
		const vmf_address *addresses =
			group ? header->information_addressees
			      : header->recipients;
		size_t length = group ? header->information_addressee_count
				      : header->recipient_count;

		for (size_t i = 0; i < length; ++i) {
			if (!addresses[i].urn.present)
				continue;
			for (size_t j = 0; j < count; ++j)
				if (!require(io,
					    seen[j] != addresses[i].urn.value))
					return false;
			seen[count++] = addresses[i].urn.value;
		}
	}
	return true;
}

static bool process_header(header_io *io, vmf_header_fields *header,
	bool *shortened, uint8_t *version)
{
	if (!small(io, version, 4))
		return false;
	if (*version != io->profile &&
		!(*version == 15 && io->profile != VMF_ENVELOPE_B))
		return fail(io, VMF_UNSUPPORTED_ENVELOPE);
	if (io->reading)
		*shortened = *version == 15;
	else if (!require(io, *shortened == (*version == 15)))
		return false;
	if (!optional_small(io, &header->compression, 2) ||
		!require(io, !header->compression.present ||
				     header->compression.value <= 1) ||
		!address(io, &header->originator, true) ||
		!addresses(
			io, &header->recipients, &header->recipient_count, 16))
		return false;
	if (*shortened) {
		if (!require(io,
			    !header->compression.present &&
				    header->originator.present &&
				    header->recipient_count >= 1 &&
				    header->information_addressee_count == 0 &&
				    header->message_count == 0 &&
				    !header->header_size_octets.present))
			return false;
	} else {
		if (!addresses(io, &header->information_addressees,
			    &header->information_addressee_count,
			    16 - header->recipient_count) ||
			!require(io,
				io->profile != VMF_ENVELOPE_B ||
					header->recipient_count ||
					!header->information_addressee_count))
			return false;
		if (io->profile == VMF_ENVELOPE_B) {
			if (!require(io, !header->header_size_octets.present))
				return false;
		} else if (!optional(
				   io, &header->header_size_octets, 16, true)) {
			return false;
		}
		if (header->header_size_octets.present && !io->counting) {
			io->header_bound = header->header_size_octets.value;
			if (!require(io,
				    io->header_bound >=
					    io->bit / 8 + (io->bit % 8 != 0)))
				return false;
			if (io->header_bound > io->limits->max_header_bytes)
				return fail(io, VMF_LIMIT_EXCEEDED);
		}
		if ((io->profile >= VMF_ENVELOPE_D && !future(io)) ||
			!messages(io, header) ||
			(io->profile >= VMF_ENVELOPE_D && !future(io)))
			return false;
	}
	if (!unique_addresses(io, header))
		return false;
	while (io->bit % 8) {
		uint32_t zero = 0;

		if (!number(io, &zero, 1) || !require(io, zero == 0))
			return false;
	}
	return require(
		io, !header->header_size_octets.present ||
			    (io->bit / 8 <= 65535 &&
				    (io->counting ||
					    header->header_size_octets.value ==
						    io->bit / 8)));
}

static vmf_header_fields *fields(
	vmf_envelope_version profile, void *header, bool **shortened)
{
	*shortened = nullptr;
	switch (profile) {
	case VMF_ENVELOPE_B:
		return &static_cast<vmf_header_b *>(header)->fields;
	case VMF_ENVELOPE_C:
		*shortened = &static_cast<vmf_header_c *>(header)->shortened;
		return &static_cast<vmf_header_c *>(header)->fields;
	case VMF_ENVELOPE_D:
		*shortened = &static_cast<vmf_header_d *>(header)->shortened;
		return &static_cast<vmf_header_d *>(header)->fields;
	case VMF_ENVELOPE_D1:
		*shortened = &static_cast<vmf_header_d1 *>(header)->shortened;
		return &static_cast<vmf_header_d1 *>(header)->fields;
	}
	return nullptr;
}

static bool pdu_size(
	header_io *io, const vmf_header_fields *header, uint64_t *result)
{
	uint64_t total = io->bit / 8;
	bool known = true;

	for (size_t i = 0; i < header->message_count; ++i) {
		const vmf_message_header *message = &header->messages[i];

		if (!message->has_body)
			continue;
		if (io->counting) {
			known = false;
			continue;
		}
		if (!message->message_size_octets.present) {
			known = false;
			continue;
		}
		uint32_t size = message->message_size_octets.value;

		io->message_index = i;
		if (!io->counting && size > io->limits->max_message_octets)
			return fail(io, VMF_LIMIT_EXCEEDED);
		total += size;
	}
	io->message_index = SIZE_MAX;
	if (total > io->limits->max_pdu_octets)
		return fail(io, VMF_LIMIT_EXCEEDED);
	if (!io->counting &&
		!require(io, io->profile > VMF_ENVELOPE_C || total <= 1048575))
		return false;
	*result = known ? total : VMF_LENGTH_UNKNOWN;
	return true;
}

vmf_status parse_header(vmf_envelope_version profile, const uint8_t *data,
	size_t size, bool final, void *arena, size_t capacity,
	const vmf_limits *limits, bool counting, vmf_envelope_event *event,
	size_t *needed, vmf_error *error)
{
	vmf_error result = {VMF_INVALID_ARGUMENT, 0, 0, SIZE_MAX};

	if (event)
		*event = {};
	if (needed)
		*needed = 0;
	vmf_layout layout = vmf_header_layout((uint8_t)profile);

	if (!layout.size || (!data && size) || !arena || !limits || !event ||
		!needed) {
		if (error)
			*error = result;
		return result.status;
	}
	header_io io = {};

	io.reading = true;
	io.final = final;
	io.counting = counting;
	io.profile = profile;
	io.input = data;
	io.size = size;
	io.arena = static_cast<uint8_t *>(arena);
	io.arena_capacity = capacity < limits->max_header_bytes
				    ? capacity
				    : limits->max_header_bytes;
	io.message_index = SIZE_MAX;
	io.limits = limits;
	io.error = {VMF_OK, 0, 0, SIZE_MAX};
	void *object = take(&io, layout.size, layout.alignment);
	bool shortened = false;
	bool *short_pointer = nullptr;
	uint8_t version = 0;
	uint64_t pdu = VMF_LENGTH_UNKNOWN;

	if (object) {
		std::memset(object, 0, layout.size);
		vmf_header_fields *header =
			fields(profile, object, &short_pointer);

		if (process_header(&io, header, &shortened, &version) &&
			pdu_size(&io, header, &pdu)) {
			if (short_pointer)
				*short_pointer = shortened;
			*event = {
				version, object, layout.size, io.bit / 8, pdu};
		}
	}
	if (io.error.status == VMF_NEED_INPUT)
		*needed = io.needed > size ? io.needed : size + 1;
	if (error)
		*error = io.error;
	return io.error.status;
}

vmf_status serialize_header(vmf_envelope_version profile,
	const vmf_envelope_event *event, bool counting, uint8_t *data,
	size_t capacity, size_t *size, const vmf_limits *limits,
	vmf_error *error)
{
	vmf_error result = {VMF_INVALID_ARGUMENT, 0, 0, SIZE_MAX};

	if (size)
		*size = 0;
	vmf_layout layout = vmf_header_layout((uint8_t)profile);

	if (!layout.size || !event || !event->header || !data || !size ||
		!limits || event->object_size != layout.size) {
		if (error)
			*error = result;
		return result.status;
	}
	vmf_header copy = {};

	std::memcpy(&copy, event->header, layout.size);
	bool shortened = false;
	bool *short_pointer = nullptr;
	vmf_header_fields *header = fields(profile, &copy, &short_pointer);

	if (short_pointer)
		shortened = *short_pointer;
	header_io io = {};

	io.counting = counting;
	io.profile = profile;
	io.output = data;
	io.size = capacity;
	io.limits = limits;
	io.message_index = SIZE_MAX;
	io.error = {VMF_OK, 0, 0, SIZE_MAX};
	uint8_t version = event->wire_version;
	uint64_t pdu = VMF_LENGTH_UNKNOWN;

	if (process_header(&io, header, &shortened, &version) &&
		pdu_size(&io, header, &pdu))
		*size = io.bit / 8;
	if (error)
		*error = io.error;
	return io.error.status;
}

} /* namespace vmf_internal */
