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
using string = ::vmf_string;
using bit_order = ::vmf_bit_order;
using integer_representation = ::vmf_integer_representation;
using integer_encoding = ::vmf_integer_encoding;
using code_range = ::vmf_code_range;
using rational = ::vmf_rational;
using scale_definition = ::vmf_scale_definition;
using condition_term = ::vmf_condition_term;
using condition_mode = ::vmf_condition_mode;
using condition = ::vmf_condition;
using text_encoding = ::vmf_text_encoding;
using index_number = ::vmf_index_number;
using index_schema = ::vmf_index_schema;
using index_refinements = ::vmf_index_refinements;
using index_info = ::vmf_index_info;
using group_id = ::vmf_group_id;
using repeat_id = ::vmf_repeat_id;
using group_path = ::vmf_group_path;
using repeat_path = ::vmf_repeat_path;
using repeat_component = ::vmf_repeat_component;
using repeat_definition = ::vmf_repeat_definition;
using repeat_count_kind = ::vmf_repeat_count_kind;
using scope_ref = ::vmf_scope_ref;
using scope_kind = ::vmf_scope_kind;
using indicator_role = ::vmf_indicator_role;
using control_target = ::vmf_control_target;
using target_kind = ::vmf_target_kind;
using encode_type = ::vmf_encode_type;
using message_number = ::vmf_message_number;
using message_standard = ::vmf_message_standard;
using standard_selection = ::vmf_standard_selection;
using standard_selection_kind = ::vmf_standard_selection_kind;
using schema_property = ::vmf_schema_property;
using optional_u64 = ::vmf_optional_u64;
using optional_i64 = ::vmf_optional_i64;
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
using encode_request_kind = ::vmf_encode_request_kind;
using repeat_occurrence = ::vmf_repeat_occurrence;
using encode_request = ::vmf_encode_request;
using encode_reply = ::vmf_encode_reply;
using encode_provider = ::vmf_encode_provider;
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

inline vmf_options options_for_schemas(const vmf_schema_set *set)
{
	return ::vmf_options_for_schemas(set);
}

inline vmf_status schema_validate(const vmf_schema_set *set,
	const vmf_message_schema *message, const vmf_limits *limits,
	const vmf_allocator *allocator, vmf_error *error)
{
	return ::vmf_schema_validate(set, message, limits, allocator, error);
}

inline vmf_status schema_set_validate(const vmf_schema_set *set,
	const vmf_limits *limits, const vmf_allocator *allocator,
	vmf_error *error)
{
	return ::vmf_schema_set_validate(set, limits, allocator, error);
}

inline vmf_status schema_message_id(const vmf_schema_set *set,
	const vmf_message_schema *message, vmf_message_id *result)
{
	return ::vmf_schema_message_id(set, message, result);
}

inline vmf_status schema_select(const vmf_schema_set *set,
	const vmf_message_id *id, const vmf_message_schema **result)
{
	return ::vmf_schema_select(set, id, result);
}

inline const vmf_index_schema *schema_find(
	const vmf_message_schema *message, vmf_index_number number)
{
	return ::vmf_schema_find(message, number);
}

inline const vmf_index_schema *schema_find_key(
	const vmf_message_schema *message, vmf_string key)
{
	return ::vmf_schema_find_key(message, key);
}

inline vmf_status schema_index_info(const vmf_schema_set *set,
	const vmf_message_schema *message, const vmf_index_schema *index,
	vmf_index_info *result)
{
	return ::vmf_schema_index_info(set, message, index, result);
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

inline vmf_status encoder_begin_pdu(
	vmf_encoder *encoder, const vmf_envelope_event *envelope)
{
	return ::vmf_encoder_begin_pdu(encoder, envelope);
}

inline vmf_status encoder_next_request(vmf_encoder *encoder,
	vmf_output *output, vmf_encode_request *request)
{
	return ::vmf_encoder_next_request(encoder, output, request);
}

inline vmf_status encoder_respond(vmf_encoder *encoder, vmf_output *output,
	const vmf_encode_reply *reply)
{
	return ::vmf_encoder_respond(encoder, output, reply);
}

inline vmf_status encoder_dispatch(vmf_encoder *encoder, vmf_output *output,
	vmf_encode_provider provider, void *user, size_t *delivered)
{
	return ::vmf_encoder_dispatch(
		encoder, output, provider, user, delivered);
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
	const vmf_index_schema *index, const void *value, size_t value_size,
	uint64_t total_units)
{
	return ::vmf_encoder_write_field(
		encoder, output, index, value, value_size, total_units);
}

inline vmf_status event_copy_field(
	const vmf_event *event, void *value, size_t value_size)
{
	return ::vmf_event_copy_field(event, value, value_size);
}

} /* namespace vmf */
#endif /* VMF_WIRE_HPP_INCLUDED */
