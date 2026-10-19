#include <vmf/wire.h>

vmf_limits vmf_default_limits(void)
{
	return {256, 4096, 64, 4096, 262144, 256, 8388608, 1048575, 16777216,
		65536, 1048576};
}

bool vmf_message_id_equal(const vmf_message_id *a, const vmf_message_id *b)
{
	return a->user_message_format == b->user_message_format &&
	       a->standard_version.present == b->standard_version.present &&
	       (!a->standard_version.present ||
		       a->standard_version.value ==
			       b->standard_version.value) &&
	       a->identification_present == b->identification_present &&
	       (!a->identification_present ||
		       (a->functional_area == b->functional_area &&
			       a->message_number == b->message_number &&
			       a->subtype.present == b->subtype.present &&
			       (!a->subtype.present ||
				       a->subtype.value == b->subtype.value)));
}

const char *vmf_status_string(vmf_status status)
{
	switch (status) {
	case VMF_OK:
		return "ok";
	case VMF_NEED_INPUT:
		return "need_input";
	case VMF_NEED_OUTPUT:
		return "need_output";
	case VMF_DONE:
		return "done";
	case VMF_PAUSED:
		return "paused";
	case VMF_ABORTED:
		return "aborted";
	case VMF_INVALID_ARGUMENT:
		return "invalid_argument";
	case VMF_INVALID_STATE:
		return "invalid_state";
	case VMF_INVALID_SCHEMA:
		return "invalid_schema";
	case VMF_UNSUPPORTED_SCHEMA_FEATURE:
		return "unsupported_schema_feature";
	case VMF_UNSUPPORTED_ENVELOPE:
		return "unsupported_envelope";
	case VMF_UNSUPPORTED_ENVELOPE_FEATURE:
		return "unsupported_envelope_feature";
	case VMF_UNKNOWN_MESSAGE:
		return "unknown_message";
	case VMF_DUPLICATE_MESSAGE:
		return "duplicate_message";
	case VMF_LENGTH_REQUIRED:
		return "length_required";
	case VMF_TYPE_MISMATCH:
		return "type_mismatch";
	case VMF_OUT_OF_RANGE:
		return "out_of_range";
	case VMF_MALFORMED:
		return "malformed";
	case VMF_TRUNCATED:
		return "truncated";
	case VMF_BUFFER_TOO_SMALL:
		return "buffer_too_small";
	case VMF_LIMIT_EXCEEDED:
		return "limit_exceeded";
	case VMF_OUT_OF_MEMORY:
		return "out_of_memory";
	case VMF_NOT_IMPLEMENTED:
		return "not_implemented";
	default:
		return "unknown";
	}
}

vmf_layout vmf_field_layout(vmf_field_kind kind)
{
	switch (kind) {
	case VMF_FIELD_BOOL:
		return {sizeof(vmf_bool_field), alignof(vmf_bool_field)};
	case VMF_FIELD_UNSIGNED:
		return {sizeof(vmf_unsigned_field),
			alignof(vmf_unsigned_field)};
	case VMF_FIELD_SIGNED:
		return {sizeof(vmf_signed_field), alignof(vmf_signed_field)};
	case VMF_FIELD_ENUM:
		return {sizeof(vmf_enum_field), alignof(vmf_enum_field)};
	case VMF_FIELD_SCALED:
		return {sizeof(vmf_scaled_field), alignof(vmf_scaled_field)};
	case VMF_FIELD_TEXT:
		return {sizeof(vmf_text_field), alignof(vmf_text_field)};
	case VMF_FIELD_OCTETS:
		return {sizeof(vmf_octets_field), alignof(vmf_octets_field)};
	case VMF_FIELD_BITS:
		return {sizeof(vmf_bits_field), alignof(vmf_bits_field)};
	default:
		return {0, 0};
	}
}

vmf_layout vmf_header_layout(uint8_t profile)
{
	switch (profile) {
	case VMF_ENVELOPE_B:
		return {sizeof(vmf_header_b), alignof(vmf_header_b)};
	case VMF_ENVELOPE_C:
		return {sizeof(vmf_header_c), alignof(vmf_header_c)};
	case VMF_ENVELOPE_D:
		return {sizeof(vmf_header_d), alignof(vmf_header_d)};
	case VMF_ENVELOPE_D1:
		return {sizeof(vmf_header_d1), alignof(vmf_header_d1)};
	default:
		return {0, 0};
	}
}
