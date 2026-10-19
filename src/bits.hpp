#ifndef VMF_PRIVATE_BITS_HPP
#define VMF_PRIVATE_BITS_HPP

#include <vmf/wire.h>

namespace vmf_internal
{

/* No pointer into a caller span survives a call. Fields default to zero.
 * available is unread input bits or occupied output bits. Physical packing
 * is LSB first unless msb_octets is set; order selects code significance.
 */
struct bit_cursor {
	uint64_t offset;
	uint64_t value;
	uint8_t octet;
	uint8_t available;
	uint8_t completed;
	bool msb_octets;
};

/* Resume a bounded code. UNKNOWN disables the absolute logical bit bound. */
vmf_status read_bits(bit_cursor *cursor, vmf_input *input, uint8_t width,
	vmf_bit_order order, uint64_t bound, uint64_t *result);

/* Resume the identical code on NEED_OUTPUT. Only complete octets publish.
 * Counting uses NULL output and follows the identical logical bit path.
 */
vmf_status write_bits(bit_cursor *cursor, vmf_output *output, uint8_t width,
	vmf_bit_order order, uint64_t value, bool counting);

} /* namespace vmf_internal */
#endif
