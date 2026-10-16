#ifndef VMF_WIRE_HPP_INCLUDED
#define VMF_WIRE_HPP_INCLUDED

#include <vmf/wire.h>

/* Procedural aliases and forwarding functions; ownership, status, lifetime
 * and retry rules are exactly those of wire.h. No RAII or exceptions.
 */
namespace vmf
{

using status = ::vmf_status;
using layout = ::vmf_layout;
using input = ::vmf_input;
using output = ::vmf_output;
using limits = ::vmf_limits;
using error = ::vmf_error;
using field_kind = ::vmf_field_kind;
using bool_field = ::vmf_bool_field;
using unsigned_field = ::vmf_unsigned_field;
using signed_field = ::vmf_signed_field;
using enum_field = ::vmf_enum_field;
using scaled_field = ::vmf_scaled_field;
using text_field = ::vmf_text_field;
using octets_field = ::vmf_octets_field;
using bits_field = ::vmf_bits_field;
using field = ::vmf_field;
using node_id = ::vmf_node_id;
using string = ::vmf_string;
using bit_order = ::vmf_bit_order;
using integer_representation = ::vmf_integer_representation;
using integer_encoding = ::vmf_integer_encoding;
using code_range = ::vmf_code_range;
using code_label = ::vmf_code_label;
using rational = ::vmf_rational;
using scale_definition = ::vmf_scale_definition;
using scalar_definition = ::vmf_scalar_definition;
using condition_term = ::vmf_condition_term;
using condition_mode = ::vmf_condition_mode;
using condition = ::vmf_condition;
using presence_kind = ::vmf_presence_kind;
using presence_definition = ::vmf_presence_definition;
using extent_kind = ::vmf_extent_kind;
using extent_definition = ::vmf_extent_definition;
using text_encoding = ::vmf_text_encoding;
using text_definition = ::vmf_text_definition;
using octets_definition = ::vmf_octets_definition;
using bits_definition = ::vmf_bits_definition;
using field_schema = ::vmf_field_schema;
using node = ::vmf_node;
using group_schema = ::vmf_group_schema;
using repeat_schema = ::vmf_repeat_schema;
using choice_case = ::vmf_choice_case;
using choice_schema = ::vmf_choice_schema;
using node_kind = ::vmf_node_kind;
using optional_u8 = ::vmf_optional_u8;
using optional_u32 = ::vmf_optional_u32;
using message_id = ::vmf_message_id;
using message_schema = ::vmf_message_schema;
using schema_set = ::vmf_schema_set;
using envelope_version = ::vmf_envelope_version;
using header_text = ::vmf_header_text;
using header_bytes = ::vmf_header_bytes;
using address = ::vmf_address;
using dtg = ::vmf_dtg;
using ack_request = ::vmf_ack_request;
using response = ::vmf_response;
using reference = ::vmf_reference;
using security = ::vmf_security;
using message_header = ::vmf_message_header;
using header_fields = ::vmf_header_fields;
using header_b = ::vmf_header_b;
using header_c = ::vmf_header_c;
using header_d = ::vmf_header_d;
using header_d1 = ::vmf_header_d1;
using header = ::vmf_header;
using event_kind = ::vmf_event_kind;
using envelope_event = ::vmf_envelope_event;
using message_event = ::vmf_message_event;
using group_event = ::vmf_group_event;
using field_event = ::vmf_field_event;
using event = ::vmf_event;
using decoder = ::vmf_decoder;
using encoder = ::vmf_encoder;
using allocate = ::vmf_allocate;
using deallocate = ::vmf_deallocate;
using event_handler = ::vmf_event_handler;
using allocator = ::vmf_allocator;
using options = ::vmf_options;
using event_action = ::vmf_event_action;
using measurement = ::vmf_measurement;

inline vmf_limits default_limits(void)
{
	return ::vmf_default_limits();
}

inline const char *status_string(vmf_status status)
{
	return ::vmf_status_string(status);
}

inline vmf_layout field_layout(vmf_field_kind kind)
{
	return ::vmf_field_layout(kind);
}

inline bool message_id_equal(const vmf_message_id *a, const vmf_message_id *b)
{
	return ::vmf_message_id_equal(a, b);
}

inline vmf_status schema_validate(const vmf_message_schema *schema,
	const vmf_limits *limits, vmf_error *error)
{
	return ::vmf_schema_validate(schema, limits, error);
}

inline vmf_status schema_set_validate(
	const vmf_schema_set *set, const vmf_limits *limits, vmf_error *error)
{
	return ::vmf_schema_set_validate(set, limits, error);
}

inline vmf_status schema_select(const vmf_schema_set *set,
	const vmf_message_id *id, const vmf_message_schema **result)
{
	return ::vmf_schema_select(set, id, result);
}

inline const vmf_node *schema_find(
	const vmf_message_schema *schema, vmf_node_id id)
{
	return ::vmf_schema_find(schema, id);
}

inline vmf_layout header_layout(uint8_t profile)
{
	return ::vmf_header_layout(profile);
}

inline vmf_status decoder_create(
	const vmf_options *options, vmf_decoder **result, vmf_error *error)
{
	return ::vmf_decoder_create(options, result, error);
}

inline vmf_status encoder_create(const vmf_options *options, bool counting_only,
	vmf_encoder **result, vmf_error *error)
{
	return ::vmf_encoder_create(options, counting_only, result, error);
}

inline void decoder_destroy(vmf_decoder *decoder)
{
	::vmf_decoder_destroy(decoder);
}

inline void encoder_destroy(vmf_encoder *encoder)
{
	::vmf_encoder_destroy(encoder);
}

inline void decoder_reset(vmf_decoder *decoder)
{
	::vmf_decoder_reset(decoder);
}

inline void encoder_reset(vmf_encoder *encoder)
{
	::vmf_encoder_reset(encoder);
}

inline vmf_error decoder_error(const vmf_decoder *decoder)
{
	return ::vmf_decoder_error(decoder);
}

inline vmf_error encoder_error(const vmf_encoder *encoder)
{
	return ::vmf_encoder_error(encoder);
}

inline uint64_t encoder_bit_count(const vmf_encoder *encoder)
{
	return ::vmf_encoder_bit_count(encoder);
}

inline vmf_status decoder_begin_pdu(vmf_decoder *decoder)
{
	return ::vmf_decoder_begin_pdu(decoder);
}

inline vmf_status decoder_begin_envelope(vmf_decoder *decoder)
{
	return ::vmf_decoder_begin_envelope(decoder);
}

inline vmf_status decoder_next(
	vmf_decoder *decoder, vmf_input *input, vmf_event *event)
{
	return ::vmf_decoder_next(decoder, input, event);
}

inline vmf_status decoder_dispatch(vmf_decoder *decoder, vmf_input *input,
	vmf_event_handler handler, void *user, size_t *delivered)
{
	return ::vmf_decoder_dispatch(decoder, input, handler, user, delivered);
}

inline vmf_status encoder_begin_envelope(vmf_encoder *encoder)
{
	return ::vmf_encoder_begin_envelope(encoder);
}

inline vmf_status encoder_write(
	vmf_encoder *encoder, vmf_output *output, const vmf_event *event)
{
	return ::vmf_encoder_write(encoder, output, event);
}

inline vmf_status encoder_measurement(
	const vmf_encoder *encoder, size_t descriptor, vmf_measurement *result)
{
	return ::vmf_encoder_measurement(encoder, descriptor, result);
}

inline vmf_status encoder_write_field(vmf_encoder *encoder, vmf_output *output,
	const vmf_node *node, const void *value, size_t value_size,
	uint64_t total_units)
{
	return ::vmf_encoder_write_field(
		encoder, output, node, value, value_size, total_units);
}

inline vmf_status event_copy_field(
	const vmf_event *event, void *value, size_t value_size)
{
	return ::vmf_event_copy_field(event, value, value_size);
}

} /* namespace vmf */
#endif /* VMF_WIRE_HPP_INCLUDED */
