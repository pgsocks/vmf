#include "envelope.hpp"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vmf/header_json.h>
#include <yyjson.h>

namespace
{

/* The prefix preserves max_align_t alignment for returned allocations. */
struct alignas(std::max_align_t) allocation {
	allocation *next;
	allocation *previous;
	size_t size;
};

struct memory_state {
	vmf_allocator allocator;
	allocation *head;
	size_t live;
	size_t maximum;
	vmf_status failure;
};

static void *default_allocate(void *, size_t size)
{
	return std::malloc(size);
}

static void default_deallocate(void *, void *memory)
{
	std::free(memory);
}

static void *memory_allocate(void *user, size_t size)
{
	memory_state *state = static_cast<memory_state *>(user);

	if (size > SIZE_MAX - sizeof(allocation) ||
	    size + sizeof(allocation) > state->maximum - state->live) {
		state->failure = VMF_LIMIT_EXCEEDED;
		return nullptr;
	}
	size_t total = sizeof(allocation) + size;
	void *storage = state->allocator.allocate(state->allocator.user, total);

	if (!storage) {
		state->failure = VMF_OUT_OF_MEMORY;
		return nullptr;
	}
	if (reinterpret_cast<uintptr_t>(storage) % alignof(std::max_align_t)) {
		state->allocator.deallocate(state->allocator.user, storage);
		state->failure = VMF_INVALID_ARGUMENT;
		return nullptr;
	}
	allocation *block =
	    new (storage) allocation{state->head, nullptr, size};

	if (state->head)
		state->head->previous = block;
	state->head = block;
	state->live += total;
	return block + 1;
}

static void memory_deallocate(void *user, void *memory)
{
	if (!memory)
		return;
	memory_state *state = static_cast<memory_state *>(user);
	allocation *block = static_cast<allocation *>(memory) - 1;

	if (block->previous)
		block->previous->next = block->next;
	else
		state->head = block->next;
	if (block->next)
		block->next->previous = block->previous;
	state->live -= sizeof(allocation) + block->size;
	state->allocator.deallocate(state->allocator.user, block);
}

static void *memory_reallocate(void *user, void *memory, size_t, size_t size)
{
	if (!memory)
		return memory_allocate(user, size);
	if (!size) {
		memory_deallocate(user, memory);
		return nullptr;
	}
	/* Count the old and replacement blocks until copying has completed. */
	void *replacement = memory_allocate(user, size);

	if (!replacement)
		return nullptr;
	allocation *block = static_cast<allocation *>(memory) - 1;
	size_t copy = block->size < size ? block->size : size;

	std::memcpy(replacement, memory, copy);
	memory_deallocate(user, memory);
	return replacement;
}

constexpr size_t line_limit = 16u * 1024 * 1024;

struct conversion {
	memory_state *memory;
	yyjson_val *root;
	vmf_header_json_error *error;
	bool failed;
};

static vmf_header_json_error diagnostic(vmf_status status,
					vmf_header_json_reason reason)
{
	return {status, reason, vmf_status_string(status), SIZE_MAX, {}, false};
}

static void append_path(vmf_header_json_error *error, const char *text,
			size_t size)
{
	size_t used = std::strlen(error->path);

	if (used + 1 >= sizeof(error->path)) {
		error->path_truncated = true;
		return;
	}
	error->path[used++] = '/';
	for (size_t i = 0; i < size;) {
		const char *piece = text + i;
		size_t length = 1;
		size_t consumed = 1;
		unsigned char code = static_cast<unsigned char>(text[i]);

		/* Parser keys are valid UTF-8. Keep whole characters at the
		 * bound. NUL cannot be represented by this C string API.
		 */
		if (!code) {
			error->path_truncated = true;
			break;
		}
		if (code >= 0x80) {
			consumed = code < 0xe0 ? 2 : (code < 0xf0 ? 3 : 4);
			length = consumed;
		} else if (code == '~' || code == '/') {
			piece = code == '~' ? "~0" : "~1";
			length = 2;
		}
		if (length >= sizeof(error->path) - used ||
			consumed > size - i) {
			error->path_truncated = true;
			break;
		}
		std::memcpy(error->path + used, piece, length);
		used += length;
		i += consumed;
	}
	error->path[used] = 0;
}

static bool locate(yyjson_val *value, yyjson_val *target,
		   vmf_header_json_error *error, unsigned depth)
{
	if (value == target)
		return true;
	if (!value || depth == 64)
		return false;
	size_t saved = std::strlen(error->path);
	bool truncated = error->path_truncated;

	if (yyjson_is_obj(value)) {
		yyjson_obj_iter iterator = yyjson_obj_iter_with(value);
		yyjson_val *key;

		while ((key = yyjson_obj_iter_next(&iterator))) {
			append_path(error, yyjson_get_str(key),
				    yyjson_get_len(key));
			if (key == target ||
			    locate(yyjson_obj_iter_get_val(key), target, error,
				   depth + 1))
				return true;
			error->path[saved] = 0;
			error->path_truncated = truncated;
		}
	} else if (yyjson_is_arr(value)) {
		for (size_t i = 0; i < yyjson_arr_size(value); ++i) {
			char index[32];

			std::snprintf(index, sizeof(index), "%zu", i);
			append_path(error, index, std::strlen(index));
			if (locate(yyjson_arr_get(value, i), target, error,
				   depth + 1))
				return true;
			error->path[saved] = 0;
			error->path_truncated = truncated;
		}
	}
	return false;
}

static bool fail(conversion *c, yyjson_val *value,
		 vmf_header_json_reason reason, const char *message)
{
	if (!c->failed) {
		c->failed = true;
		*c->error = diagnostic(VMF_MALFORMED, reason);
		c->error->message = message;
		if (value)
			locate(c->root, value, c->error, 0);
	}
	return false;
}

static void *reserve(conversion *c, size_t size)
{
	void *memory = memory_allocate(c->memory, size ? size : 1);

	if (memory)
		std::memset(memory, 0, size ? size : 1);
	return memory;
}

static bool key_equal(yyjson_val *value, const char *name)
{
	size_t size = std::strlen(name);

	return yyjson_is_str(value) && yyjson_get_len(value) == size &&
	       !std::memcmp(yyjson_get_str(value), name, size);
}

/* Explicit member lists reject decoded duplicate and inactive keys too. */
static bool object(conversion *c, yyjson_val *value, const char *const *names,
		   size_t count, uint64_t required, yyjson_val **members)
{
	if (!yyjson_is_obj(value))
		return fail(c, value, VMF_HEADER_JSON_TYPE, "expected object");
	std::memset(members, 0, count * sizeof(*members));
	uint64_t seen = 0;
	yyjson_obj_iter iterator = yyjson_obj_iter_with(value);
	yyjson_val *key;

	while ((key = yyjson_obj_iter_next(&iterator))) {
		size_t index;

		for (index = 0; index < count; ++index)
			if (key_equal(key, names[index]))
				break;
		if (index == count)
			return fail(c, key, VMF_HEADER_JSON_PROPERTY,
				    "unknown or profile-incompatible property");
		if (seen & (UINT64_C(1) << index))
			return fail(c, key, VMF_HEADER_JSON_DUPLICATE,
				    "duplicate property");
		seen |= UINT64_C(1) << index;
		members[index] = yyjson_obj_iter_get_val(key);
	}
	for (size_t i = 0; i < count; ++i) {
		if ((required & (UINT64_C(1) << i)) && !members[i]) {
			fail(c, value, VMF_HEADER_JSON_MISSING,
			     "missing required property");
			append_path(c->error, names[i], std::strlen(names[i]));
			return false;
		}
	}
	return true;
}

static bool complete_object(conversion *c, yyjson_val *value,
			    const char *const *names, size_t count,
			    yyjson_val **members)
{
	return object(c, value, names, count, 0, members);
}

static bool put(yyjson_mut_doc *document, yyjson_mut_val *object,
		const char *name, yyjson_mut_val *value)
{
	return value && yyjson_mut_obj_add_val(document, object, name, value);
}

static bool number(conversion *c, yyjson_val *value, uint64_t maximum,
		   uint64_t *result)
{
	if (!yyjson_is_uint(value))
		return fail(c, value, VMF_HEADER_JSON_TYPE,
			    "expected unsigned integer");
	if (yyjson_get_uint(value) > maximum)
		return fail(c, value, VMF_HEADER_JSON_RANGE,
			    "integer out of range");
	*result = yyjson_get_uint(value);
	return true;
}

static bool byte(conversion *c, yyjson_val *value, uint8_t *result)
{
	if (!value)
		return true;
	uint64_t parsed;

	if (!number(c, value, UINT8_MAX, &parsed))
		return false;
	*result = static_cast<uint8_t>(parsed);
	return true;
}

static bool boolean(conversion *c, yyjson_val *value, bool *result)
{
	if (!value)
		return true;
	if (!yyjson_is_bool(value))
		return fail(c, value, VMF_HEADER_JSON_TYPE, "expected boolean");
	*result = yyjson_get_bool(value);
	return true;
}

static yyjson_mut_val *optional_byte(conversion *, yyjson_mut_doc *document,
				     const vmf_optional_u8 *value)
{
	return value->present ? yyjson_mut_uint(document, value->value)
			      : yyjson_mut_null(document);
}

static bool read_optional_byte(conversion *c, yyjson_val *value,
			       vmf_optional_u8 *result)
{
	if (!value || yyjson_is_null(value))
		return true;
	result->present = true;
	return byte(c, value, &result->value);
}

static yyjson_mut_val *optional_word(conversion *, yyjson_mut_doc *document,
				     const vmf_optional_u32 *value)
{
	return value->present ? yyjson_mut_uint(document, value->value)
			      : yyjson_mut_null(document);
}

static bool read_optional_word(conversion *c, yyjson_val *value,
			       vmf_optional_u32 *result)
{
	if (!value || yyjson_is_null(value))
		return true;
	uint64_t parsed;

	if (!number(c, value, UINT32_MAX, &parsed))
		return false;
	result->present = true;
	result->value = static_cast<uint32_t>(parsed);
	return true;
}

static yyjson_mut_val *text(conversion *, yyjson_mut_doc *document,
			    const vmf_header_text *value)
{
	if (!value->present)
		return yyjson_mut_null(document);
	if ((value->value.size && !value->value.data) ||
	    value->value.size > line_limit)
		return nullptr;
	const char *data = value->value.data ? value->value.data : "";

	return yyjson_mut_strncpy(document, data, value->value.size);
}

static bool read_text(conversion *c, yyjson_val *value, vmf_header_text *result)
{
	if (!value || yyjson_is_null(value))
		return true;
	if (!yyjson_is_str(value))
		return fail(c, value, VMF_HEADER_JSON_TYPE, "expected string");
	size_t size = yyjson_get_len(value);
	char *data = static_cast<char *>(reserve(c, size + 1));

	if (!data)
		return false;
	std::memcpy(data, yyjson_get_str(value), size);
	result->present = true;
	result->value = {data, size};
	return true;
}

static yyjson_mut_val *bytes(conversion *c, yyjson_mut_doc *document,
			     const vmf_header_bytes *value)
{
	if (!value->present)
		return yyjson_mut_null(document);
	if ((value->size && !value->data) || value->size > (line_limit - 1) / 2)
		return nullptr;
	char *buffer = static_cast<char *>(reserve(c, value->size * 2 + 1));

	if (!buffer)
		return nullptr;
	static const char digits[] = "0123456789abcdef";

	for (size_t index = 0; index < value->size; ++index) {
		buffer[index * 2] = digits[value->data[index] >> 4];
		buffer[index * 2 + 1] = digits[value->data[index] & 15];
	}
	yyjson_mut_val *result =
	    yyjson_mut_strncpy(document, buffer, value->size * 2);

	memory_deallocate(c->memory, buffer);
	return result;
}

static int hex_digit(char value)
{
	if (value >= '0' && value <= '9')
		return value - '0';
	if (value >= 'a' && value <= 'f')
		return value - 'a' + 10;
	if (value >= 'A' && value <= 'F')
		return value - 'A' + 10;
	return -1;
}

static bool read_bytes(conversion *c, yyjson_val *value,
		       vmf_header_bytes *result)
{
	if (!value || yyjson_is_null(value))
		return true;
	if (!yyjson_is_str(value) || yyjson_get_len(value) % 2)
		return fail(c, value, VMF_HEADER_JSON_TYPE,
			    "expected even-length hexadecimal string");
	size_t size = yyjson_get_len(value) / 2;
	uint8_t *data = static_cast<uint8_t *>(reserve(c, size ? size : 1));

	if (!data)
		return false;
	const char *encoded = yyjson_get_str(value);

	for (size_t index = 0; index < size; ++index) {
		int high = hex_digit(encoded[index * 2]);
		int low = hex_digit(encoded[index * 2 + 1]);

		if (high < 0 || low < 0)
			return fail(c, value, VMF_HEADER_JSON_TYPE,
				    "invalid hexadecimal digit");
		data[index] = static_cast<uint8_t>((high << 4) | low);
	}
	*result = {true, data, size};
	return true;
}

static yyjson_mut_val *id_to_json(conversion *, yyjson_mut_doc *document,
				  const vmf_message_id *id)
{
	if (!document || !id)
		return nullptr;
	yyjson_mut_val *result = yyjson_mut_obj(document);

	if (!put(document, result, "user_message_format",
		 yyjson_mut_uint(document, id->user_message_format)))
		return nullptr;
	if (id->standard_version.present &&
	    !put(document, result, "standard_version",
		 yyjson_mut_uint(document, id->standard_version.value)))
		return nullptr;
	if (id->identification_present) {
		yyjson_mut_val *identification = yyjson_mut_obj(document);

		if (!put(document, identification, "functional_area",
			 yyjson_mut_uint(document, id->functional_area)) ||
		    !put(document, identification, "message_number",
			 yyjson_mut_uint(document, id->message_number)))
			return nullptr;
		if (id->subtype.present &&
		    !put(document, identification, "subtype",
			 yyjson_mut_uint(document, id->subtype.value)))
			return nullptr;
		if (!put(document, result, "identification", identification))
			return nullptr;
	}
	return result;
}

static bool id_from_json(conversion *c, yyjson_val *value,
			 vmf_envelope_version profile, vmf_message_id *id)
{
	static const char *const keys[] = {
	    "user_message_format", "identification", "standard_version"};
	yyjson_val *members[3] = {};
	vmf_message_id parsed = {};
	size_t count = profile == VMF_ENVELOPE_B ? 2 : 3;

	if (!object(c, value, keys, count, 1, members) ||
	    !byte(c, members[0], &parsed.user_message_format))
		return false;
	if (members[2]) {
		parsed.standard_version.present = true;
		if (!byte(c, members[2], &parsed.standard_version.value))
			return false;
	}
	if (members[1]) {
		static const char *const names[] = {
		    "functional_area", "message_number", "subtype"};
		yyjson_val *parts[3];

		if (!object(c, members[1], names, 3, 3, parts) ||
		    !byte(c, parts[0], &parsed.functional_area) ||
		    !byte(c, parts[1], &parsed.message_number))
			return false;
		parsed.identification_present = true;
		if (parts[2]) {
			parsed.subtype.present = true;
			if (!byte(c, parts[2], &parsed.subtype.value))
				return false;
		}
	}
	*id = parsed;
	return true;
}

static yyjson_mut_val *address(conversion *c, yyjson_mut_doc *document,
			       const vmf_address *value)
{
	if (!value->present)
		return yyjson_mut_null(document);
	yyjson_mut_val *result = yyjson_mut_obj(document);

	if (!put(document, result, "urn",
		 optional_word(c, document, &value->urn)) ||
	    !put(document, result, "name", text(c, document, &value->name)))
		return nullptr;
	return result;
}

static bool read_address(conversion *c, yyjson_val *value, vmf_address *result)
{
	if (!value || yyjson_is_null(value))
		return true;
	static const char *const keys[] = {"urn", "name"};
	yyjson_val *members[2];

	if (!complete_object(c, value, keys, 2, members) ||
	    !read_optional_word(c, members[0], &result->urn) ||
	    !read_text(c, members[1], &result->name))
		return false;
	result->present = true;
	return true;
}

static yyjson_mut_val *dtg(conversion *c, yyjson_mut_doc *document,
			   const vmf_dtg *value)
{
	if (!value->present)
		return yyjson_mut_null(document);
	yyjson_mut_val *result = yyjson_mut_obj(document);

	if (!put(document, result, "year",
		 yyjson_mut_uint(document, value->year)) ||
	    !put(document, result, "month",
		 yyjson_mut_uint(document, value->month)) ||
	    !put(document, result, "day",
		 yyjson_mut_uint(document, value->day)) ||
	    !put(document, result, "hour",
		 yyjson_mut_uint(document, value->hour)) ||
	    !put(document, result, "minute",
		 yyjson_mut_uint(document, value->minute)) ||
	    !put(document, result, "second",
		 yyjson_mut_uint(document, value->second)) ||
	    !put(document, result, "extension",
		 optional_word(c, document, &value->extension)))
		return nullptr;
	return result;
}

static bool read_dtg(conversion *c, yyjson_val *value, vmf_dtg *result)
{
	if (!value || yyjson_is_null(value))
		return true;
	static const char *const keys[] = {
	    "year", "month", "day", "hour", "minute", "second", "extension"};
	yyjson_val *members[7];

	if (!object(c, value, keys, 7, 63, members) ||
	    !byte(c, members[0], &result->year) ||
	    !byte(c, members[1], &result->month) ||
	    !byte(c, members[2], &result->day) ||
	    !byte(c, members[3], &result->hour) ||
	    !byte(c, members[4], &result->minute) ||
	    !byte(c, members[5], &result->second) ||
	    !read_optional_word(c, members[6], &result->extension))
		return false;
	result->present = true;
	return true;
}

static yyjson_mut_val *acknowledgment(conversion *, yyjson_mut_doc *document,
				      const vmf_ack_request *value)
{
	if (!value->present)
		return yyjson_mut_null(document);
	yyjson_mut_val *result = yyjson_mut_obj(document);

	if (!put(document, result, "machine",
		 yyjson_mut_bool(document, value->machine)) ||
	    !put(document, result, "operator_ack",
		 yyjson_mut_bool(document, value->operator_ack)) ||
	    !put(document, result, "operator_reply",
		 yyjson_mut_bool(document, value->operator_reply)))
		return nullptr;
	return result;
}

static bool read_acknowledgment(conversion *c, yyjson_val *value,
				vmf_ack_request *result)
{
	if (!value || yyjson_is_null(value))
		return true;
	static const char *const keys[] = {"machine", "operator_ack",
					   "operator_reply"};
	yyjson_val *members[3];

	if (!complete_object(c, value, keys, 3, members) ||
	    !boolean(c, members[0], &result->machine) ||
	    !boolean(c, members[1], &result->operator_ack) ||
	    !boolean(c, members[2], &result->operator_reply))
		return false;
	result->present = true;
	return true;
}

static yyjson_mut_val *response(conversion *c, yyjson_mut_doc *document,
				const vmf_response *value)
{
	if (!value->present)
		return yyjson_mut_null(document);
	yyjson_mut_val *result = yyjson_mut_obj(document);

	if (!put(document, result, "acknowledged",
		 dtg(c, document, &value->acknowledged)) ||
	    !put(document, result, "receipt_compliance",
		 yyjson_mut_uint(document, value->receipt_compliance)) ||
	    !put(document, result, "cantco_reason",
		 optional_byte(c, document, &value->cantco_reason)) ||
	    !put(document, result, "cantpro_reason",
		 optional_byte(c, document, &value->cantpro_reason)) ||
	    !put(document, result, "amplification",
		 text(c, document, &value->amplification)))
		return nullptr;
	return result;
}

static bool read_response(conversion *c, yyjson_val *value,
			  vmf_response *result)
{
	if (!value || yyjson_is_null(value))
		return true;
	static const char *const keys[] = {"acknowledged", "receipt_compliance",
					   "cantco_reason", "cantpro_reason",
					   "amplification"};
	yyjson_val *members[5];

	if (!object(c, value, keys, 5, 2, members) ||
	    !read_dtg(c, members[0], &result->acknowledged) ||
	    !byte(c, members[1], &result->receipt_compliance) ||
	    !read_optional_byte(c, members[2], &result->cantco_reason) ||
	    !read_optional_byte(c, members[3], &result->cantpro_reason) ||
	    !read_text(c, members[4], &result->amplification))
		return false;
	result->present = true;
	return true;
}

static bool array(conversion *c, yyjson_val *value, size_t element_size,
		  size_t maximum, void **storage, size_t *count)
{
	if (!value) {
		*count = 0;
		*storage = nullptr;
		return true;
	}
	if (!yyjson_is_arr(value))
		return fail(c, value, VMF_HEADER_JSON_TYPE, "expected array");
	*count = yyjson_arr_size(value);
	*storage = nullptr;
	if (*count > maximum || *count > SIZE_MAX / element_size)
		return fail(c, value, VMF_HEADER_JSON_RANGE,
			    "array exceeds native capacity");
	if (!*count)
		return true;
	*storage = reserve(c, *count * element_size);
	return *storage != nullptr;
}

static yyjson_mut_val *reference(conversion *c, yyjson_mut_doc *document,
				 vmf_envelope_version profile,
				 const vmf_reference *value)
{
	yyjson_mut_val *result = yyjson_mut_obj(document);

	if (!put(document, result, "originator",
		 address(c, document, &value->originator)) ||
	    !put(document, result, "dtg", dtg(c, document, &value->dtg)))
		return nullptr;
	if (profile == VMF_ENVELOPE_B &&
	    (!put(document, result, "functional_area",
		  yyjson_mut_uint(document, value->functional_area)) ||
	     !put(document, result, "message_number",
		  yyjson_mut_uint(document, value->message_number))))
		return nullptr;
	return result;
}

static bool read_reference(conversion *c, yyjson_val *value,
			   vmf_envelope_version profile, vmf_reference *result)
{
	static const char *const keys[] = {"originator", "dtg",
					   "functional_area", "message_number"};
	yyjson_val *members[4];
	size_t count = profile == VMF_ENVELOPE_B ? 4 : 2;

	if (!complete_object(c, value, keys, count, members) ||
	    !read_address(c, members[0], &result->originator) ||
	    !read_dtg(c, members[1], &result->dtg))
		return false;
	return profile != VMF_ENVELOPE_B ||
	       (byte(c, members[2], &result->functional_area) &&
		byte(c, members[3], &result->message_number));
}

static yyjson_mut_val *security(conversion *c, yyjson_mut_doc *document,
				const vmf_security *value)
{
	if (!value->present)
		return yyjson_mut_null(document);
	yyjson_mut_val *result = yyjson_mut_obj(document);
	yyjson_mut_val *tokens = yyjson_mut_arr(document);

	if (!tokens)
		return nullptr;
	for (size_t index = 0; index < value->token_count; ++index) {
		yyjson_mut_val *token =
		    bytes(c, document, &value->tokens[index]);

		if (!token || !yyjson_mut_arr_add_val(tokens, token))
			return nullptr;
	}
	if (!put(document, result, "spi",
		 yyjson_mut_uint(document, value->spi)) ||
	    !put(document, result, "keying_material",
		 bytes(c, document, &value->keying_material)) ||
	    !put(document, result, "initialization",
		 bytes(c, document, &value->initialization)) ||
	    !put(document, result, "tokens_present",
		 yyjson_mut_bool(document, value->tokens_present)) ||
	    !put(document, result, "tokens", tokens) ||
	    !put(document, result, "authentication_a",
		 bytes(c, document, &value->authentication_a)) ||
	    !put(document, result, "authentication_b",
		 bytes(c, document, &value->authentication_b)) ||
	    !put(document, result, "signed_ack_request",
		 yyjson_mut_bool(document, value->signed_ack_request)) ||
	    !put(document, result, "padding_present",
		 yyjson_mut_bool(document, value->padding_present)) ||
	    !put(document, result, "padding_length",
		 yyjson_mut_uint(document, value->padding_length)) ||
	    !put(document, result, "padding",
		 bytes(c, document, &value->padding)))
		return nullptr;
	return result;
}

static bool read_security(conversion *c, yyjson_val *value,
			  vmf_security *result)
{
	if (!value || yyjson_is_null(value))
		return true;
	static const char *const keys[] = {"spi",
					   "keying_material",
					   "initialization",
					   "tokens_present",
					   "tokens",
					   "authentication_a",
					   "authentication_b",
					   "signed_ack_request",
					   "padding_present",
					   "padding_length",
					   "padding"};
	yyjson_val *members[11];
	void *storage;

	if (!object(c, value, keys, 11, 1, members) ||
	    !byte(c, members[0], &result->spi) ||
	    !read_bytes(c, members[1], &result->keying_material) ||
	    !read_bytes(c, members[2], &result->initialization) ||
	    !boolean(c, members[3], &result->tokens_present) ||
	    !array(c, members[4], sizeof(vmf_header_bytes), 17, &storage,
		   &result->token_count))
		return false;
	auto *tokens = static_cast<vmf_header_bytes *>(storage);

	result->tokens = tokens;
	for (size_t index = 0; index < result->token_count; ++index)
		if (!read_bytes(c, yyjson_arr_get(members[4], index),
				&tokens[index]))
			return false;
	if (!read_bytes(c, members[5], &result->authentication_a) ||
	    !read_bytes(c, members[6], &result->authentication_b) ||
	    !boolean(c, members[7], &result->signed_ack_request) ||
	    !boolean(c, members[8], &result->padding_present) ||
	    !byte(c, members[9], &result->padding_length) ||
	    !read_bytes(c, members[10], &result->padding))
		return false;
	result->present = true;
	return true;
}

static yyjson_mut_val *message(conversion *c, yyjson_mut_doc *document,
			       vmf_envelope_version profile,
			       const vmf_message_header *value)
{
	yyjson_mut_val *result = yyjson_mut_obj(document);
	yyjson_mut_val *references = yyjson_mut_arr(document);

	if (!references)
		return nullptr;
	for (size_t index = 0; index < value->reference_count; ++index) {
		yyjson_mut_val *entry =
		    reference(c, document, profile, &value->references[index]);

		if (!entry || !yyjson_mut_arr_add_val(references, entry))
			return nullptr;
	}
	if (!put(document, result, "id", id_to_json(c, document, &value->id)) ||
	    !put(document, result, "file_name",
		 text(c, document, &value->file_name)) ||
	    !put(document, result, "message_size_octets",
		 optional_word(c, document, &value->message_size_octets)) ||
	    !put(document, result, "operation",
		 yyjson_mut_uint(document, value->operation)) ||
	    !put(document, result, "retransmit",
		 yyjson_mut_bool(document, value->retransmit)) ||
	    !put(document, result, "precedence",
		 yyjson_mut_uint(document, value->precedence)) ||
	    !put(document, result, "classification",
		 yyjson_mut_uint(document, value->classification)) ||
	    !put(document, result, "originator_dtg",
		 dtg(c, document, &value->originator_dtg)) ||
	    !put(document, result, "perishability",
		 dtg(c, document, &value->perishability)) ||
	    !put(document, result, "acknowledgment",
		 acknowledgment(c, document, &value->acknowledgment)) ||
	    !put(document, result, "response",
		 response(c, document, &value->response)) ||
	    !put(document, result, "references", references))
		return nullptr;
	if (profile == VMF_ENVELOPE_B &&
	    !put(document, result, "marking_b",
		 optional_word(c, document, &value->marking_b)))
		return nullptr;
	if (profile == VMF_ENVELOPE_C &&
	    !put(document, result, "marking_c",
		 text(c, document, &value->marking_c)))
		return nullptr;
	if (profile == VMF_ENVELOPE_D || profile == VMF_ENVELOPE_D1) {
		yyjson_mut_val *markings = yyjson_mut_arr(document);

		if (!markings)
			return nullptr;
		for (size_t index = 0; index < value->marking_count; ++index) {
			yyjson_mut_val *entry =
			    yyjson_mut_uint(document, value->markings[index]);

			if (!entry || !yyjson_mut_arr_add_val(markings, entry))
				return nullptr;
		}
		if (!put(document, result, "markings", markings))
			return nullptr;
	}
	if (profile != VMF_ENVELOPE_B &&
	    !put(document, result, "security",
		 security(c, document, &value->security)))
		return nullptr;
	return result;
}

static bool read_message(conversion *c, yyjson_val *value,
			 vmf_envelope_version profile,
			 vmf_message_header *result)
{
	const char *keys[] = {"id",
			      "file_name",
			      "message_size_octets",
			      "operation",
			      "retransmit",
			      "precedence",
			      "classification",
			      "originator_dtg",
			      "perishability",
			      "acknowledgment",
			      "response",
			      "references",
			      "marking_b",
			      "security"};
	yyjson_val *members[14];
	size_t count = profile == VMF_ENVELOPE_B ? 13 : 14;

	result->precedence = profile <= VMF_ENVELOPE_C ? 2 : 0;
	if (profile == VMF_ENVELOPE_C)
		keys[12] = "marking_c";
	else if (profile == VMF_ENVELOPE_D || profile == VMF_ENVELOPE_D1)
		keys[12] = "markings";
	if (!object(c, value, keys, count, 1, members) ||
	    !id_from_json(c, members[0], profile, &result->id) ||
	    !read_text(c, members[1], &result->file_name) ||
	    !read_optional_word(c, members[2], &result->message_size_octets) ||
	    !byte(c, members[3], &result->operation) ||
	    !boolean(c, members[4], &result->retransmit) ||
	    !byte(c, members[5], &result->precedence) ||
	    !byte(c, members[6], &result->classification) ||
	    !read_dtg(c, members[7], &result->originator_dtg) ||
	    !read_dtg(c, members[8], &result->perishability) ||
	    !read_acknowledgment(c, members[9], &result->acknowledgment) ||
	    !read_response(c, members[10], &result->response))
		return false;
	void *storage;

	if (!array(c, members[11], sizeof(vmf_reference), 4, &storage,
		   &result->reference_count))
		return false;
	auto *references = static_cast<vmf_reference *>(storage);

	result->references = references;
	for (size_t index = 0; index < result->reference_count; ++index)
		if (!read_reference(c, yyjson_arr_get(members[11], index),
				    profile, &references[index]))
			return false;
	if (profile == VMF_ENVELOPE_B) {
		if (!read_optional_word(c, members[12], &result->marking_b))
			return false;
	} else if (profile == VMF_ENVELOPE_C) {
		if (!read_text(c, members[12], &result->marking_c))
			return false;
	} else {
		if (!array(c, members[12], sizeof(uint16_t), 16, &storage,
			   &result->marking_count))
			return false;
		auto *markings = static_cast<uint16_t *>(storage);

		result->markings = markings;
		for (size_t index = 0; index < result->marking_count; ++index) {
			uint64_t parsed;

			if (!number(c, yyjson_arr_get(members[12], index),
				    UINT16_MAX, &parsed))
				return false;
			markings[index] = static_cast<uint16_t>(parsed);
		}
	}
	if (profile != VMF_ENVELOPE_B &&
	    !read_security(c, members[13], &result->security))
		return false;
	if (!members[5] && profile >= VMF_ENVELOPE_D &&
	    result->id.user_message_format == 6)
		result->precedence = 5;
	result->has_body = !result->response.present;
	return true;
}

static yyjson_mut_val *addresses(conversion *c, yyjson_mut_doc *document,
				 const vmf_address *values, size_t count)
{
	yyjson_mut_val *result = yyjson_mut_arr(document);

	if (!result)
		return nullptr;
	for (size_t index = 0; index < count; ++index) {
		yyjson_mut_val *entry = address(c, document, &values[index]);

		if (!entry || !yyjson_mut_arr_add_val(result, entry))
			return nullptr;
	}
	return result;
}

static bool read_addresses(conversion *c, yyjson_val *value,
			   const vmf_address **result, size_t *count)
{
	void *storage;

	if (!array(c, value, sizeof(vmf_address), 16, &storage, count))
		return false;
	auto *values = static_cast<vmf_address *>(storage);

	*result = values;
	for (size_t index = 0; index < *count; ++index) {
		yyjson_val *entry = yyjson_arr_get(value, index);

		if (!read_address(c, entry, &values[index]))
			return false;
		if (!values[index].present)
			return fail(c, entry, VMF_HEADER_JSON_TYPE,
				    "expected present address object");
	}
	return true;
}

static yyjson_mut_val *fields(conversion *c, yyjson_mut_doc *document,
			      vmf_envelope_version profile,
			      const vmf_header_fields *value)
{
	yyjson_mut_val *result = yyjson_mut_obj(document);
	yyjson_mut_val *messages = yyjson_mut_arr(document);

	if (!messages)
		return nullptr;
	for (size_t index = 0; index < value->message_count; ++index) {
		yyjson_mut_val *entry =
		    message(c, document, profile, &value->messages[index]);

		if (!entry || !yyjson_mut_arr_add_val(messages, entry))
			return nullptr;
	}
	if (!put(document, result, "compression",
		 optional_byte(c, document, &value->compression)) ||
	    !put(document, result, "originator",
		 address(c, document, &value->originator)) ||
	    !put(document, result, "recipients",
		 addresses(c, document, value->recipients,
			   value->recipient_count)) ||
	    !put(document, result, "information_addressees",
		 addresses(c, document, value->information_addressees,
			   value->information_addressee_count)) ||
	    !put(document, result, "messages", messages))
		return nullptr;
	if (profile != VMF_ENVELOPE_B &&
	    !put(document, result, "header_size_octets",
		 optional_word(c, document, &value->header_size_octets)))
		return nullptr;
	return result;
}

static bool read_fields(conversion *c, yyjson_val *value,
			vmf_envelope_version profile, bool shortened,
			vmf_header_fields *result)
{
	static const char *const keys[] = {
	    "compression", "originator",
	    "recipients",  "information_addressees",
	    "messages",	   "header_size_octets"};
	yyjson_val *members[6];
	size_t count = profile == VMF_ENVELOPE_B ? 5 : 6;

	if (!object(c, value, keys, count, shortened ? 0 : 16, members) ||
	    !read_optional_byte(c, members[0], &result->compression) ||
	    !read_address(c, members[1], &result->originator) ||
	    !read_addresses(c, members[2], &result->recipients,
			    &result->recipient_count) ||
	    !read_addresses(c, members[3], &result->information_addressees,
			    &result->information_addressee_count))
		return false;
	if (result->recipient_count + result->information_addressee_count > 16)
		return fail(
		    c, members[3], VMF_HEADER_JSON_RANGE,
		    "recipient and information address total exceeds 16");
	void *storage;

	if (!array(c, members[4], sizeof(vmf_message_header), 16, &storage,
		   &result->message_count))
		return false;
	auto *messages = static_cast<vmf_message_header *>(storage);

	result->messages = messages;
	for (size_t index = 0; index < result->message_count; ++index)
		if (!read_message(c, yyjson_arr_get(members[4], index), profile,
				  &messages[index]))
			return false;
	return profile == VMF_ENVELOPE_B ||
	       read_optional_word(c, members[5], &result->header_size_octets);
}
static yyjson_mut_val *header_to_json(conversion *c, yyjson_mut_doc *document,
				      vmf_envelope_version profile,
				      const void *header)
{
	if (!document || !header)
		return nullptr;
	const vmf_header_fields *value;
	bool shortened = false;

	switch (profile) {
	case VMF_ENVELOPE_B:
		value = &static_cast<const vmf_header_b *>(header)->fields;
		break;
	case VMF_ENVELOPE_C: {
		auto *source = static_cast<const vmf_header_c *>(header);

		value = &source->fields;
		shortened = source->shortened;
		break;
	}
	case VMF_ENVELOPE_D: {
		auto *source = static_cast<const vmf_header_d *>(header);

		value = &source->fields;
		shortened = source->shortened;
		break;
	}
	case VMF_ENVELOPE_D1: {
		auto *source = static_cast<const vmf_header_d1 *>(header);

		value = &source->fields;
		shortened = source->shortened;
		break;
	}
	default:
		return nullptr;
	}
	yyjson_mut_val *result = yyjson_mut_obj(document);

	if (!put(document, result, "fields",
		 fields(c, document, profile, value)))
		return nullptr;
	if (profile != VMF_ENVELOPE_B &&
	    !put(document, result, "shortened",
		 yyjson_mut_bool(document, shortened)))
		return nullptr;
	return result;
}

static bool header_from_json(conversion *c, yyjson_val *value,
			     vmf_envelope_version profile, vmf_header *header)
{
	if (!header)
		return false;
	static const char *const keys[] = {"fields", "shortened"};
	yyjson_val *members[2];
	size_t count = profile == VMF_ENVELOPE_B ? 1 : 2;
	vmf_header parsed = {};
	vmf_header_fields *target;
	bool *shortened = nullptr;

	switch (profile) {
	case VMF_ENVELOPE_B:
		target = &parsed.b.fields;
		break;
	case VMF_ENVELOPE_C:
		target = &parsed.c.fields;
		shortened = &parsed.c.shortened;
		break;
	case VMF_ENVELOPE_D:
		target = &parsed.d.fields;
		shortened = &parsed.d.shortened;
		break;
	case VMF_ENVELOPE_D1:
		target = &parsed.d1.fields;
		shortened = &parsed.d1.shortened;
		break;
	default:
		return false;
	}
	if (!object(c, value, keys, count, 1, members) ||
	    (shortened && !boolean(c, members[1], shortened)) ||
	    !read_fields(c, members[0], profile, shortened && *shortened,
			 target))
		return false;
	*header = parsed;
	return true;
}

static vmf_header_fields *native_fields(vmf_envelope_version profile,
					vmf_header *header)
{
	switch (profile) {
	case VMF_ENVELOPE_B:
		return &header->b.fields;
	case VMF_ENVELOPE_C:
		return &header->c.fields;
	case VMF_ENVELOPE_D:
		return &header->d.fields;
	case VMF_ENVELOPE_D1:
		return &header->d1.fields;
	}
	return nullptr;
}

static bool shortened(vmf_envelope_version profile, const void *header)
{
	switch (profile) {
	case VMF_ENVELOPE_B:
		return false;
	case VMF_ENVELOPE_C:
		return static_cast<const vmf_header_c *>(header)->shortened;
	case VMF_ENVELOPE_D:
		return static_cast<const vmf_header_d *>(header)->shortened;
	case VMF_ENVELOPE_D1:
		return static_cast<const vmf_header_d1 *>(header)->shortened;
	}
	return false;
}

static vmf_header_json_reason status_reason(vmf_status status)
{
	if (status == VMF_OUT_OF_MEMORY)
		return VMF_HEADER_JSON_ALLOCATION;
	if (status == VMF_LIMIT_EXCEEDED)
		return VMF_HEADER_JSON_LIMIT;
	if (status == VMF_INVALID_ARGUMENT)
		return VMF_HEADER_JSON_ARGUMENT;
	return VMF_HEADER_JSON_NATIVE;
}

static vmf_status validate(conversion *c, vmf_envelope_version profile,
			   const void *header,
			   const vmf_header_json_options *options)
{
	void *scratch = reserve(c, options->limits.max_header_bytes);

	if (!scratch)
		return c->memory->failure;
	vmf_envelope_event event = {
	    static_cast<uint8_t>(shortened(profile, header) ? 15 : profile),
	    header, vmf_header_layout(profile).size, 0, VMF_LENGTH_UNKNOWN};
	size_t size = 0;
	vmf_error error;
	vmf_status status = vmf_internal::serialize_header(
	    profile, &event, true, static_cast<uint8_t *>(scratch),
	    options->limits.max_header_bytes, &size, &options->limits, &error);

	memory_deallocate(c->memory, scratch);
	if (status != VMF_OK) {
		*c->error = diagnostic(status, status_reason(status));
		std::strcpy(c->error->path, "/fields");
		if (error.message_index != SIZE_MAX)
			std::snprintf(c->error->path, sizeof(c->error->path),
				      "/fields/messages/%zu",
				      error.message_index);
	}
	return status;
}

static vmf_status initialize(vmf_envelope_version profile,
			     const vmf_header_json_options *options,
			     memory_state *memory, vmf_header_json_error *error)
{
	*error = diagnostic(VMF_OK, VMF_HEADER_JSON_NONE);
	if (!options || !vmf_header_layout(profile).size) {
		*error =
		    diagnostic(VMF_INVALID_ARGUMENT, VMF_HEADER_JSON_ARGUMENT);
		return error->status;
	}
	vmf_allocator allocator = {default_allocate, default_deallocate,
				   nullptr};

	if (options->allocator) {
		if (!!options->allocator->allocate !=
		    !!options->allocator->deallocate) {
			*error = diagnostic(VMF_INVALID_ARGUMENT,
					    VMF_HEADER_JSON_ARGUMENT);
			return error->status;
		}
		if (options->allocator->allocate)
			allocator = *options->allocator;
	}
	*memory = {allocator, nullptr, 0, options->max_memory_bytes, VMF_OK};
	return VMF_OK;
}

static void release_all(memory_state memory)
{
	allocation *block = memory.head;

	while (block) {
		allocation *next = block->next;

		memory.allocator.deallocate(memory.allocator.user, block);
		block = next;
	}
}

static vmf_status memory_error(memory_state *memory,
			       vmf_header_json_error *error)
{
	vmf_status status = memory->failure;

	if (status == VMF_OK)
		status = VMF_OUT_OF_MEMORY;
	*error = diagnostic(status, status_reason(status));
	return status;
}

} /* namespace */

struct vmf_header_json_document {
	memory_state memory;
	vmf_envelope_version profile;
	vmf_header header;
};

struct vmf_header_json_text {
	memory_state memory;
	char *data;
	size_t size;
};

vmf_header_json_options vmf_header_json_default_options(void)
{
	return {vmf_default_limits(), 16u * 1024 * 1024, 64u * 1024 * 1024,
		nullptr};
}

vmf_status vmf_header_json_load(vmf_envelope_version profile, const char *data,
				size_t size,
				const vmf_header_json_options *options,
				vmf_header_json_document **result,
				vmf_header_json_error *error)
{
	vmf_header_json_error local;

	if (!error)
		error = &local;
	if (result)
		*result = nullptr;
	memory_state memory = {};
	vmf_status status = initialize(profile, options, &memory, error);

	if (status != VMF_OK)
		return status;
	if (!data || !result) {
		*error =
		    diagnostic(VMF_INVALID_ARGUMENT, VMF_HEADER_JSON_ARGUMENT);
		return error->status;
	}
	if (size > options->max_json_bytes) {
		*error = diagnostic(VMF_LIMIT_EXCEEDED, VMF_HEADER_JSON_LIMIT);
		return error->status;
	}
	if (!size) {
		*error = diagnostic(VMF_TRUNCATED, VMF_HEADER_JSON_SYNTAX);
		error->byte_offset = 0;
		return error->status;
	}
	void *storage =
	    memory_allocate(&memory, sizeof(vmf_header_json_document));

	if (!storage)
		return memory_error(&memory, error);
	auto *document =
	    new (storage) vmf_header_json_document{memory, profile, {}};
	yyjson_alc allocator = {memory_allocate, memory_reallocate,
				memory_deallocate, &document->memory};
	yyjson_read_err syntax;
	yyjson_doc *json = yyjson_read_opts(const_cast<char *>(data), size, 0,
					    &allocator, &syntax);

	if (!json) {
		if (document->memory.failure != VMF_OK)
			status = memory_error(&document->memory, error);
		else {
			status =
			    syntax.code == YYJSON_READ_ERROR_UNEXPECTED_END ||
				    syntax.code ==
					YYJSON_READ_ERROR_EMPTY_CONTENT
				? VMF_TRUNCATED
				: VMF_MALFORMED;
			*error = diagnostic(status, VMF_HEADER_JSON_SYNTAX);
			error->byte_offset = syntax.pos;
		}
	} else {
		conversion c = {&document->memory, yyjson_doc_get_root(json),
				error, false};
		bool converted =
		    header_from_json(&c, c.root, profile, &document->header);

		if (document->memory.failure != VMF_OK)
			status = memory_error(&document->memory, error);
		else if (!converted) {
			if (!c.failed)
				fail(&c, c.root, VMF_HEADER_JSON_NATIVE,
				     "invalid header definition");
			status = error->status;
		} else
			status =
			    validate(&c, profile, &document->header, options);
		yyjson_doc_free(json);
	}
	if (status != VMF_OK) {
		if (document->memory.failure != VMF_OK)
			status = memory_error(&document->memory, error);
		vmf_header_json_destroy(document);
		return status;
	}
	*result = document;
	return VMF_OK;
}

const vmf_header *
vmf_header_json_header(const vmf_header_json_document *document)
{
	return document ? &document->header : nullptr;
}

vmf_envelope_version
vmf_header_json_profile(const vmf_header_json_document *document)
{
	return document ? document->profile
			: static_cast<vmf_envelope_version>(0);
}

void vmf_header_json_destroy(vmf_header_json_document *document)
{
	if (document)
		release_all(document->memory);
}

namespace
{

static vmf_status emit(vmf_envelope_version profile, const void *header,
		       const vmf_header_json_options *options,
		       vmf_header_json_text **result,
		       vmf_header_json_error *error, bool minimal)
{
	vmf_header_json_error local;

	if (!error)
		error = &local;
	if (result)
		*result = nullptr;
	memory_state memory = {};
	vmf_status status = initialize(profile, options, &memory, error);

	if (status != VMF_OK)
		return status;
	if (!header || !result) {
		*error =
		    diagnostic(VMF_INVALID_ARGUMENT, VMF_HEADER_JSON_ARGUMENT);
		return error->status;
	}
	void *storage = memory_allocate(&memory, sizeof(vmf_header_json_text));

	if (!storage)
		return memory_error(&memory, error);
	auto *text = new (storage) vmf_header_json_text{memory, nullptr, 0};
	conversion c = {&text->memory, nullptr, error, false};

	status = validate(&c, profile, header, options);
	yyjson_alc allocator = {memory_allocate, memory_reallocate,
				memory_deallocate, &text->memory};
	yyjson_mut_doc *json =
	    status == VMF_OK ? yyjson_mut_doc_new(&allocator) : nullptr;

	if (status == VMF_OK && !json)
		status = memory_error(&text->memory, error);
	if (json) {
		yyjson_mut_val *root = nullptr;

		if (!minimal)
			root = header_to_json(&c, json, profile, header);
		else {
			auto *source = static_cast<const vmf_header *>(header);
			vmf_header copy = *source;
			auto *fields = native_fields(profile, &copy);
			auto *body = yyjson_mut_obj(json);
			auto *messages = yyjson_mut_arr(json);

			root = yyjson_mut_obj(json);
			bool valid = root && body && messages;

			for (size_t i = 0; valid && i < fields->message_count;
			     ++i) {
				auto *message = yyjson_mut_obj(json);

				valid =
				    message &&
				    put(json, message, "id",
					id_to_json(&c, json,
						   &fields->messages[i].id));
				if (valid && fields->message_count > 1)
					valid = put(json, message,
						    "message_size_octets",
						    yyjson_mut_uint(json, 0));
				valid = valid && yyjson_mut_arr_add_val(
						     messages, message);
			}
			if (valid && fields->header_size_octets.present)
				valid = put(json, body, "header_size_octets",
					    yyjson_mut_uint(json, 0));
			valid = valid &&
				put(json, body, "messages", messages) &&
				put(json, root, "fields", body);
			if (!valid)
				root = nullptr;
		}
		if (!root)
			status = memory_error(&text->memory, error);
		else {
			yyjson_mut_doc_set_root(json, root);
			yyjson_write_err syntax;

			text->data = yyjson_mut_write_opts(
			    json, 0, &allocator, &text->size, &syntax);
			if (!text->data)
				status = memory_error(&text->memory, error);
			else if (text->size > options->max_json_bytes) {
				status = VMF_LIMIT_EXCEEDED;
				*error =
				    diagnostic(status, VMF_HEADER_JSON_LIMIT);
			}
		}
		yyjson_mut_doc_free(json);
	}
	if (status != VMF_OK) {
		if (text->memory.failure != VMF_OK)
			status = memory_error(&text->memory, error);
		vmf_header_json_text_destroy(text);
		return status;
	}
	*result = text;
	return VMF_OK;
}

} /* namespace */

vmf_status vmf_header_json_serialize(vmf_envelope_version profile,
				     const void *header,
				     const vmf_header_json_options *options,
				     vmf_header_json_text **result,
				     vmf_header_json_error *error)
{
	return emit(profile, header, options, result, error, false);
}

vmf_status vmf_header_json_template(vmf_envelope_version profile,
				    const vmf_message_id *ids, size_t count,
				    const vmf_header_json_options *options,
				    vmf_header_json_text **result,
				    vmf_header_json_error *error)
{
	if (result)
		*result = nullptr;
	if (error)
		*error = diagnostic(VMF_OK, VMF_HEADER_JSON_NONE);
	if (!ids || !count || count > 16) {
		if (error)
			*error = diagnostic(VMF_INVALID_ARGUMENT,
					    VMF_HEADER_JSON_ARGUMENT);
		return VMF_INVALID_ARGUMENT;
	}
	vmf_header header = {};
	vmf_header_fields *fields = native_fields(profile, &header);

	if (!fields) {
		if (error)
			*error = diagnostic(VMF_INVALID_ARGUMENT,
					    VMF_HEADER_JSON_ARGUMENT);
		return VMF_INVALID_ARGUMENT;
	}
	vmf_message_header messages[16] = {};

	fields->messages = messages;
	fields->message_count = count;
	fields->header_size_octets = {count > 1 && profile != VMF_ENVELOPE_B,
				      0};
	for (size_t i = 0; i < count; ++i) {
		messages[i].id = ids[i];
		messages[i].precedence =
		    profile <= VMF_ENVELOPE_C
			? 2
			: (ids[i].user_message_format == 6 ? 5 : 0);
		messages[i].has_body = true;
		messages[i].message_size_octets = {count > 1, 0};
	}
	return emit(profile, &header, options, result, error, true);
}

const char *vmf_header_json_text_data(const vmf_header_json_text *text)
{
	return text ? text->data : nullptr;
}

size_t vmf_header_json_text_size(const vmf_header_json_text *text)
{
	return text ? text->size : 0;
}

void vmf_header_json_text_destroy(vmf_header_json_text *text)
{
	if (text)
		release_all(text->memory);
}
