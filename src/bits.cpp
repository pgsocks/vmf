#include "bits.hpp"

namespace vmf_internal
{

vmf_status read_bits(bit_cursor *cursor, vmf_input *input, uint8_t width,
	vmf_bit_order order, uint64_t bound, uint64_t *result)
{
	if (!cursor || !input || !result || !width || width > 64 ||
		order > VMF_BIT_LSB_FIRST || input->used > input->size ||
		(!input->data && input->size))
		return VMF_INVALID_ARGUMENT;
	while (cursor->completed < width) {
		if (cursor->offset == UINT64_MAX)
			return VMF_LIMIT_EXCEEDED;
		if (cursor->offset >= bound)
			return VMF_MALFORMED;
		if (!cursor->available) {
			if (input->used == input->size)
				return input->final ? VMF_TRUNCATED
						    : VMF_NEED_INPUT;
			cursor->octet = input->data[input->used++];
			cursor->available = 8;
		}
		unsigned int position = 8 - cursor->available;

		if (cursor->msb_octets)
			position = 7 - position;
		uint64_t bit = (cursor->octet >> position) & 1;

		if (order == VMF_BIT_MSB_FIRST)
			cursor->value = (cursor->value << 1) | bit;
		else
			cursor->value |= bit << cursor->completed;
		--cursor->available;
		++cursor->completed;
		++cursor->offset;
	}
	*result = cursor->value;
	cursor->value = 0;
	cursor->completed = 0;
	return VMF_OK;
}

vmf_status write_bits(bit_cursor *cursor, vmf_output *output, uint8_t width,
	vmf_bit_order order, uint64_t value, bool counting)
{
	if (!cursor || !width || width > 64 || order > VMF_BIT_LSB_FIRST ||
		(!counting && (!output || output->used > output->capacity ||
				      (!output->data && output->capacity))) ||
		(width < 64 && value >= (UINT64_C(1) << width)))
		return VMF_INVALID_ARGUMENT;
	for (;;) {
		if (cursor->available == 8) {
			if (!counting && output->used == output->capacity)
				return VMF_NEED_OUTPUT;
			if (!counting)
				output->data[output->used++] = cursor->octet;
			cursor->available = 0;
			cursor->octet = 0;
		}
		if (cursor->completed == width) {
			cursor->completed = 0;
			return VMF_OK;
		}
		if (cursor->offset == UINT64_MAX)
			return VMF_LIMIT_EXCEEDED;
		unsigned int shift = order == VMF_BIT_MSB_FIRST
					     ? width - cursor->completed - 1
					     : cursor->completed;
		unsigned int position = cursor->msb_octets
						? 7 - cursor->available
						: cursor->available;

		cursor->octet |= ((value >> shift) & 1) << position;
		++cursor->available;
		++cursor->completed;
		++cursor->offset;
	}
}

} /* namespace vmf_internal */
