#ifndef VMF_UNIT_FIXTURES_HPP
#define VMF_UNIT_FIXTURES_HPP

#include <vmf/wire.h>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace fixtures
{

/* Public DFDL fixture; attribution and provenance are in NOTICE. */
inline constexpr uint8_t public_c_header[] = {0xe2, 0x9b, 0x61, 0x09, 0x65,
	0x31, 0x51, 0x20, 0x06, 0x00, 0x79, 0x25, 0x40, 0x00, 0x00, 0x08, 0x51,
	0xa2, 0x8c, 0xc2, 0x41, 0x0f, 0x10, 0x00};

/* Synthetic binary envelopes, previously checked with DFDL. */
inline constexpr uint8_t binary_c[] = {
	0x02, 0x17, 0x00, 0x04, 0xf1, 0xff, 0x01, 0x00, 0x80, 0x03, 0x00, 0x5a};
inline constexpr uint8_t binary_d1[] = {0x04, 0x1b, 0x00, 0x80, 0x20, 0xfe,
	0x3f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5a};

inline void require(vmf_status expected, vmf_status actual)
{
	if (expected != actual)
		throw std::runtime_error(vmf_status_string(actual));
}

inline vmf_options options(
	vmf_envelope_version profile, const vmf_schema_set *schemas = nullptr)
{
	vmf_options config = {};

	config.profile = profile;
	config.limits = vmf_default_limits();
	config.schemas = schemas;
	return config;
}

/* Independently assemble the supported minimal Table I grammar. These VMF
 * identities are synthetic; no assigned payload layout is claimed.
 */
inline std::vector<uint8_t> packet(vmf_envelope_version profile,
	const std::vector<uint8_t> &body, bool sized = true, size_t count = 1)
{
	std::vector<unsigned> bits;
	auto put = [&](uint64_t value, unsigned width) {
		for (unsigned i = 0; i < width; ++i)
			bits.push_back((value >> i) & 1);
	};

	put(profile, 4);
	put(0, 4);
	size_t header_size = 0;

	if (profile != VMF_ENVELOPE_B) {
		put(1, 1);
		header_size = bits.size();
		put(0, 16);
	}
	if (profile >= VMF_ENVELOPE_D)
		put(0, 5);
	for (size_t item = 0; item < count; ++item) {
		put(item + 1 < count, 1);
		put(2, 4);
		if (profile != VMF_ENVELOPE_B) {
			put(1, 1);
			put(0, 4);
		}
		put(1, 1);
		put(1, 4);
		put(1, 7);
		put(0, 1);
		put(1, 1);
		put('x', 7);
		put(127, 7);
		put(sized, 1);
		if (sized)
			put(body.size(), 20);
		put(0, 3);
		put(profile <= VMF_ENVELOPE_C ? 7 : 0, 3);
		put(0, 8);
		if (profile >= VMF_ENVELOPE_D)
			put(0, 5);
		if (profile != VMF_ENVELOPE_B)
			put(0, 1);
	}
	if (profile >= VMF_ENVELOPE_D)
		put(0, 5);
	while (bits.size() % 8)
		put(0, 1);
	if (header_size)
		for (unsigned i = 0; i < 16; ++i)
			bits[header_size + i] = (bits.size() / 8 >> i) & 1;
	std::vector<uint8_t> result(bits.size() / 8, 0);

	for (size_t i = 0; i < bits.size(); ++i)
		result[i / 8] |= bits[i] << (i % 8);
	for (size_t i = 0; i < count; ++i)
		result.insert(result.end(), body.begin(), body.end());
	return result;
}

struct example_schema {
	vmf_index_schema indexes[32] = {};
	uint32_t parts[32][2] = {};
	vmf_group_id groups[32] = {};
	vmf_message_schema schema = {};
	vmf_schema_set set = {};
};

inline vmf_index_schema *add_index(example_schema *s, uint32_t major,
	uint32_t minor, uint64_t width, vmf_group_id group = 0)
{
	size_t row = s->schema.index_count++;
	auto *index = &s->indexes[row];

	s->parts[row][0] = major;
	s->parts[row][1] = minor;
	index->index_number = {s->parts[row], minor ? 2u : 1u};
	index->number_of_bits = width;
	if (group) {
		s->groups[row] = group;
		index->group_code = {&s->groups[row], 1};
	}
	return index;
}

inline vmf_index_schema *optional_index(example_schema *s,
	uint32_t major, uint64_t width)
{
	auto *control = add_index(s, major, 0, 0);
	auto *field = add_index(s, major, 1, width);

	control->indicator = VMF_INDICATOR_FPI;
	control->target.kind = VMF_TARGET_INDEX;
	control->target.as.index = field->index_number;
	return field;
}

inline void optional_group(example_schema *s, uint32_t major,
	vmf_group_id group, const uint8_t *widths, size_t count,
	size_t text_count = 0)
{
	auto *control = add_index(s, major, 0, 0);

	control->indicator = VMF_INDICATOR_GPI;
	control->target.kind = VMF_TARGET_GROUP;
	control->target.as.group = group;
	for (size_t i = 0; i < count; ++i) {
		auto *field = add_index(s, major, i + 1, widths[i], group);

		if (i < text_count)
			field->encode_type = VMF_ENCODE_TEXT;
	}
}

inline void initialize(example_schema *s, bool d1)
{
	s->schema.message_number = {1, 1};
	s->schema.indexes = s->indexes;
	s->set = {d1 ? VMF_ENVELOPE_D1 : VMF_ENVELOPE_C,
		&s->schema, 1, {}};
	s->set.standard.kind = VMF_STANDARD_RAW_CODE;
	s->set.standard.as.raw_code = 0;
	add_index(s, 1, 0, d1 ? 5 : 3);
	if (d1)
		optional_index(s, 2, 7)->encode_type = VMF_ENCODE_TEXT;
	else
		add_index(s, 2, 0, 3);
	uint8_t d_target[] = {7, 14};
	uint8_t c_target[] = {7, 7, 14};
	uint8_t effective[] = {5, 6, 6};
	uint8_t entity[] = {24, 32, 5, 5, 6, 6};

	optional_group(s, 3, 1, d1 ? d_target : c_target, d1 ? 2 : 3,
		d1 ? 1 : 2);
	optional_index(s, 4, d1 ? 21 : 24);
	optional_group(s, 5, 2, effective, 3);
	optional_index(s, 6, 7);
	optional_group(s, 7, 3, entity, 6);
}

inline void write(vmf_encoder *encoder, const vmf_event *event,
	std::vector<uint8_t> *bytes)
{
	vmf_status status;
	size_t attempts = 0;

	do {
		uint8_t byte;
		vmf_output output = {&byte, 1, 0};

		status = vmf_encoder_write(encoder, &output, event);
		if (output.used)
			bytes->push_back(byte);
		if (++attempts > 1048576)
			throw std::runtime_error(
				"encoder did not make progress");
	} while (status == VMF_NEED_OUTPUT);
	require(VMF_OK, status);
}

} /* namespace fixtures */

#endif
