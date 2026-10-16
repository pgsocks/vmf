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

inline std::vector<uint8_t> packet(
	vmf_envelope_version profile, const std::vector<uint8_t> &body)
{
	std::vector<uint8_t> bytes;

	if (profile == VMF_ENVELOPE_C) {
		bytes.assign(binary_c, binary_c + sizeof(binary_c) - 1);
		bytes[6] = static_cast<uint8_t>(body.size());
	} else {
		bytes.assign(binary_d1, binary_d1 + sizeof(binary_d1) - 1);
		bytes[6] = (bytes[6] & 0x1f) |
			   static_cast<uint8_t>((body.size() & 7) << 5);
		bytes[7] = static_cast<uint8_t>(body.size() >> 3);
	}
	bytes.insert(bytes.end(), body.begin(), body.end());
	return bytes;
}

inline void scalar(vmf_node *node, uint32_t id, uint8_t width)
{
	*node = {};
	node->id = id;
	node->kind = VMF_NODE_FIELD;
	node->as.field.kind = VMF_FIELD_UNSIGNED;
	node->as.field.as.scalar.encoding = {
		width, VMF_BIT_LSB_FIRST, VMF_INTEGER_UNSIGNED};
}

inline void group(
	vmf_node *node, uint32_t id, const vmf_node *children, size_t count)
{
	*node = {};
	node->id = id;
	node->kind = VMF_NODE_GROUP;
	node->as.group = {children, count};
}

inline void optional(vmf_node *node)
{
	node->presence.kind = VMF_PRESENCE_BIT;
	node->presence.present_bit = true;
}

inline void character(vmf_node *node)
{
	node->as.field = {};
	node->as.field.kind = VMF_FIELD_TEXT;
	auto *text = &node->as.field.as.text;

	text->encoding = VMF_TEXT_ASCII_7;
	text->order = VMF_BIT_LSB_FIRST;
	text->length.kind = VMF_EXTENT_FIXED;
	text->length.fixed = 1;
	text->length.minimum = 1;
	text->length.maximum = 1;
}

struct example_schema {
	vmf_node root = {};
	vmf_node fields[7] = {};
	vmf_node target[3] = {};
	vmf_node effective[3] = {};
	vmf_node entity[6] = {};
	vmf_message_schema schema = {};
	const vmf_message_schema *entry = &schema;
	vmf_schema_set set = {&entry, 1};
};

inline void initialize(example_schema *s, bool d1)
{
	uint32_t id = 2;

	for (auto &node : s->fields)
		scalar(&node, id++, 7);
	for (auto &node : s->target)
		scalar(&node, id++, 7);
	uint8_t effective_widths[] = {5, 6, 6};
	uint8_t entity_widths[] = {24, 32, 5, 5, 6, 6};

	for (size_t i = 0; i < 3; ++i)
		scalar(&s->effective[i], id++, effective_widths[i]);
	for (size_t i = 0; i < 6; ++i)
		scalar(&s->entity[i], id++, entity_widths[i]);
	group(&s->root, 1, s->fields, 7);
	s->fields[0].as.field.as.scalar.encoding.bit_width = d1 ? 5 : 3;
	if (d1) {
		character(&s->fields[1]);
		optional(&s->fields[1]);
		character(&s->target[0]);
		s->target[1].as.field.as.scalar.encoding.bit_width = 14;
		group(&s->fields[2], s->fields[2].id, s->target, 2);
	} else {
		s->fields[1].as.field.as.scalar.encoding.bit_width = 3;
		character(&s->target[0]);
		character(&s->target[1]);
		s->target[2].as.field.as.scalar.encoding.bit_width = 14;
		group(&s->fields[2], s->fields[2].id, s->target, 3);
	}
	optional(&s->fields[2]);
	s->fields[3].as.field.as.scalar.encoding.bit_width = d1 ? 21 : 24;
	optional(&s->fields[3]);
	group(&s->fields[4], s->fields[4].id, s->effective, 3);
	optional(&s->fields[4]);
	optional(&s->fields[5]);
	group(&s->fields[6], s->fields[6].id, s->entity, 6);
	optional(&s->fields[6]);
	s->schema.id.user_message_format = 1;
	s->schema.root = &s->root;
	s->schema.octet_order = VMF_BIT_LSB_FIRST;
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
