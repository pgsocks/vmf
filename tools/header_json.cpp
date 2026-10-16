#include "cli.hpp"
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>

namespace
{

constexpr size_t memory_limit = 64 * 1024 * 1024;
constexpr size_t line_limit = 16 * 1024 * 1024;

struct alignas(std::max_align_t) json_block {
	size_t size;
};

struct alignas(std::max_align_t) arena_block {
	arena_block *next;
	size_t size;
};

static void *json_allocate(void *user, size_t size)
{
	auto *context = static_cast<cli_json *>(user);

	if (size > SIZE_MAX - sizeof(json_block))
		return nullptr;
	size_t total = sizeof(json_block) + size;

	if (total > memory_limit - context->used)
		return nullptr;
	void *memory = std::malloc(total);

	if (!memory)
		return nullptr;
	auto *block = new (memory) json_block{total};

	context->used += total;
	return block + 1;
}

static void json_release(void *user, void *memory)
{
	if (!memory)
		return;
	auto *context = static_cast<cli_json *>(user);
	auto *block = static_cast<json_block *>(memory) - 1;

	context->used -= block->size;
	std::free(block);
}

static void *json_reallocate(void *user, void *memory, size_t, size_t size)
{
	if (!memory)
		return json_allocate(user, size);
	if (!size) {
		json_release(user, memory);
		return nullptr;
	}
	void *replacement = json_allocate(user, size);

	if (!replacement)
		return nullptr;
	auto *block = static_cast<json_block *>(memory) - 1;
	size_t old = block->size - sizeof(json_block);

	std::memcpy(replacement, memory, old < size ? old : size);
	json_release(user, memory);
	return replacement;
}

static bool key_equal(yyjson_val *value, const char *name)
{
	size_t size = std::strlen(name);

	return yyjson_is_str(value) && yyjson_get_len(value) == size &&
	       !std::memcmp(yyjson_get_str(value), name, size);
}

/* Explicit member lists reject decoded duplicate and inactive keys too. */
static bool object(yyjson_val *value, const char *const *names,
	size_t count, uint64_t required, yyjson_val **members)
{
	if (!yyjson_is_obj(value))
		return false;
	std::memset(members, 0, count * sizeof(*members));
	uint64_t seen = 0;
	yyjson_obj_iter iterator = yyjson_obj_iter_with(value);
	yyjson_val *key;

	while ((key = yyjson_obj_iter_next(&iterator))) {
		size_t index;

		for (index = 0; index < count; ++index)
			if (key_equal(key, names[index]))
				break;
		if (index == count || (seen & (UINT64_C(1) << index)))
			return false;
		seen |= UINT64_C(1) << index;
		members[index] = yyjson_obj_iter_get_val(key);
	}
	return (seen & required) == required;
}

static bool complete_object(yyjson_val *value, const char *const *names,
	size_t count, yyjson_val **members)
{
	return object(value, names, count,
		(UINT64_C(1) << count) - 1, members);
}

static bool put(yyjson_mut_doc *document, yyjson_mut_val *object,
	const char *name, yyjson_mut_val *value)
{
	return value && yyjson_mut_obj_add_val(document, object, name, value);
}

static bool number(yyjson_val *value, uint64_t maximum, uint64_t *result)
{
	if (!yyjson_is_uint(value) || yyjson_get_uint(value) > maximum)
		return false;
	*result = yyjson_get_uint(value);
	return true;
}

static bool byte(yyjson_val *value, uint8_t *result)
{
	uint64_t parsed;

	if (!number(value, UINT8_MAX, &parsed))
		return false;
	*result = static_cast<uint8_t>(parsed);
	return true;
}

static bool boolean(yyjson_val *value, bool *result)
{
	if (!yyjson_is_bool(value))
		return false;
	*result = yyjson_get_bool(value);
	return true;
}

static yyjson_mut_val *optional_byte(yyjson_mut_doc *document,
	const vmf_optional_u8 *value)
{
	return value->present ? yyjson_mut_uint(document, value->value)
			      : yyjson_mut_null(document);
}

static bool read_optional_byte(yyjson_val *value, vmf_optional_u8 *result)
{
	if (yyjson_is_null(value))
		return true;
	result->present = true;
	return byte(value, &result->value);
}

static yyjson_mut_val *optional_word(yyjson_mut_doc *document,
	const vmf_optional_u32 *value)
{
	return value->present ? yyjson_mut_uint(document, value->value)
			      : yyjson_mut_null(document);
}

static bool read_optional_word(yyjson_val *value, vmf_optional_u32 *result)
{
	if (yyjson_is_null(value))
		return true;
	uint64_t parsed;

	if (!number(value, UINT32_MAX, &parsed))
		return false;
	result->present = true;
	result->value = static_cast<uint32_t>(parsed);
	return true;
}

static yyjson_mut_val *text(yyjson_mut_doc *document,
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

static bool read_text(yyjson_val *value, cli_arena *arena,
	vmf_header_text *result)
{
	if (yyjson_is_null(value))
		return true;
	if (!yyjson_is_str(value))
		return false;
	size_t size = yyjson_get_len(value);
	char *data = static_cast<char *>(cli_arena_alloc(arena, size + 1));

	if (!data)
		return false;
	std::memcpy(data, yyjson_get_str(value), size);
	result->present = true;
	result->value = {data, size};
	return true;
}

static yyjson_mut_val *bytes(yyjson_mut_doc *document,
	const vmf_header_bytes *value)
{
	if (!value->present)
		return yyjson_mut_null(document);
	if ((value->size && !value->data) ||
		value->size > (line_limit - 1) / 2)
		return nullptr;
	char *buffer = static_cast<char *>(
		std::malloc(value->size * 2 + 1));

	if (!buffer)
		return nullptr;
	static const char digits[] = "0123456789abcdef";

	for (size_t index = 0; index < value->size; ++index) {
		buffer[index * 2] = digits[value->data[index] >> 4];
		buffer[index * 2 + 1] = digits[value->data[index] & 15];
	}
	yyjson_mut_val *result = yyjson_mut_strncpy(document, buffer,
		value->size * 2);

	std::free(buffer);
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

static bool read_bytes(yyjson_val *value, cli_arena *arena,
	vmf_header_bytes *result)
{
	if (yyjson_is_null(value))
		return true;
	if (!yyjson_is_str(value) || yyjson_get_len(value) % 2)
		return false;
	size_t size = yyjson_get_len(value) / 2;
	uint8_t *data = static_cast<uint8_t *>(
		cli_arena_alloc(arena, size ? size : 1));

	if (!data)
		return false;
	const char *encoded = yyjson_get_str(value);

	for (size_t index = 0; index < size; ++index) {
		int high = hex_digit(encoded[index * 2]);
		int low = hex_digit(encoded[index * 2 + 1]);

		if (high < 0 || low < 0)
			return false;
		data[index] = static_cast<uint8_t>((high << 4) | low);
	}
	*result = {true, data, size};
	return true;
}

} /* namespace */

void cli_json_init(cli_json *context)
{
	if (!context)
		return;
	*context = {};
	context->allocator = {
		json_allocate, json_reallocate, json_release, context};
}

yyjson_doc *cli_json_read(cli_json *context, const char *data, size_t size)
{
	if (!context || !data || size > line_limit)
		return nullptr;
	return yyjson_read_opts(const_cast<char *>(data), size, 0,
		&context->allocator, nullptr);
}

yyjson_mut_doc *cli_json_new(cli_json *context)
{
	return context ? yyjson_mut_doc_new(&context->allocator) : nullptr;
}

void *cli_arena_alloc(cli_arena *arena, size_t size)
{
	if (!arena || !size || size > SIZE_MAX - sizeof(arena_block))
		return nullptr;
	size_t total = sizeof(arena_block) + size;

	if (total > memory_limit - arena->used)
		return nullptr;
	void *memory = std::malloc(total);

	if (!memory)
		return nullptr;
	auto *block = new (memory) arena_block{
		static_cast<arena_block *>(arena->blocks), total};

	arena->blocks = block;
	arena->used += total;
	std::memset(block + 1, 0, size);
	return block + 1;
}

void cli_arena_destroy(cli_arena *arena)
{
	if (!arena)
		return;
	auto *block = static_cast<arena_block *>(arena->blocks);

	while (block) {
		arena_block *next = block->next;

		std::free(block);
		block = next;
	}
	*arena = {};
}

bool cli_json_u64(yyjson_val *value, uint64_t *result, bool unknown)
{
	if (!result)
		return false;
	if (unknown && yyjson_is_null(value)) {
		*result = UINT64_MAX;
		return true;
	}
	if (!yyjson_is_str(value))
		return false;
	size_t size = yyjson_get_len(value);
	const char *data = yyjson_get_str(value);

	if (!size || (size > 1 && data[0] == '0'))
		return false;
	uint64_t parsed = 0;

	for (size_t index = 0; index < size; ++index) {
		if (data[index] < '0' || data[index] > '9')
			return false;
		unsigned digit = static_cast<unsigned>(data[index] - '0');

		if (parsed > (UINT64_MAX - digit) / 10)
			return false;
		parsed = parsed * 10 + digit;
	}
	if (unknown && parsed == UINT64_MAX)
		return false;
	*result = parsed;
	return true;
}

bool cli_json_i64(yyjson_val *value, int64_t *result)
{
	if (!result || !yyjson_is_str(value))
		return false;
	size_t size = yyjson_get_len(value);
	const char *data = yyjson_get_str(value);
	bool negative = size && data[0] == '-';
	size_t first = negative ? 1 : 0;

	if (size == first || (size - first > 1 && data[first] == '0'))
		return false;
	uint64_t maximum = static_cast<uint64_t>(INT64_MAX) + negative;
	uint64_t parsed = 0;

	for (size_t index = first; index < size; ++index) {
		if (data[index] < '0' || data[index] > '9')
			return false;
		unsigned digit = static_cast<unsigned>(data[index] - '0');

		if (parsed > (maximum - digit) / 10)
			return false;
		parsed = parsed * 10 + digit;
	}
	if (negative && !parsed)
		return false;
	*result = negative ? -static_cast<int64_t>(parsed - 1) - 1
			   : static_cast<int64_t>(parsed);
	return true;
}

yyjson_mut_val *cli_json_uint(yyjson_mut_doc *document, uint64_t value,
	bool unknown)
{
	if (unknown && value == UINT64_MAX)
		return yyjson_mut_null(document);
	char buffer[21];
	size_t index = sizeof(buffer);

	do {
		buffer[--index] = static_cast<char>('0' + value % 10);
		value /= 10;
	} while (value);
	return yyjson_mut_strncpy(document, buffer + index,
		sizeof(buffer) - index);
}

yyjson_mut_val *cli_json_int(yyjson_mut_doc *document, int64_t value)
{
	bool negative = value < 0;
	uint64_t magnitude = negative ? static_cast<uint64_t>(-(value + 1)) + 1
				      : static_cast<uint64_t>(value);
	char buffer[21];
	size_t index = sizeof(buffer);

	do {
		buffer[--index] = static_cast<char>('0' + magnitude % 10);
		magnitude /= 10;
	} while (magnitude);
	if (negative)
		buffer[--index] = '-';
	return yyjson_mut_strncpy(document, buffer + index,
		sizeof(buffer) - index);
}

yyjson_mut_val *cli_id_to_json(yyjson_mut_doc *document,
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

bool cli_id_from_json(yyjson_val *value, vmf_message_id *id)
{
	if (!id)
		return false;
	static const char *const keys[] = {
		"user_message_format", "standard_version", "identification"};
	yyjson_val *members[3];
	vmf_message_id parsed = {};

	if (!object(value, keys, 3, 1, members) ||
		!byte(members[0], &parsed.user_message_format))
		return false;
	if (members[1]) {
		parsed.standard_version.present = true;
		if (!byte(members[1], &parsed.standard_version.value))
			return false;
	}
	if (members[2]) {
		static const char *const names[] = {
			"functional_area", "message_number", "subtype"};
		yyjson_val *parts[3];

		if (!object(members[2], names, 3, 3, parts) ||
			!byte(parts[0], &parsed.functional_area) ||
			!byte(parts[1], &parsed.message_number))
			return false;
		parsed.identification_present = true;
		if (parts[2]) {
			parsed.subtype.present = true;
			if (!byte(parts[2], &parsed.subtype.value))
				return false;
		}
	}
	*id = parsed;
	return true;
}

namespace
{

static yyjson_mut_val *address(yyjson_mut_doc *document,
	const vmf_address *value)
{
	if (!value->present)
		return yyjson_mut_null(document);
	yyjson_mut_val *result = yyjson_mut_obj(document);

	if (!put(document, result, "urn", optional_word(document, &value->urn)) ||
		!put(document, result, "name", text(document, &value->name)))
		return nullptr;
	return result;
}

static bool read_address(yyjson_val *value, cli_arena *arena,
	vmf_address *result)
{
	if (yyjson_is_null(value))
		return true;
	static const char *const keys[] = {"urn", "name"};
	yyjson_val *members[2];

	if (!complete_object(value, keys, 2, members) ||
		!read_optional_word(members[0], &result->urn) ||
		!read_text(members[1], arena, &result->name))
		return false;
	result->present = true;
	return true;
}

static yyjson_mut_val *dtg(yyjson_mut_doc *document, const vmf_dtg *value)
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
			optional_word(document, &value->extension)))
		return nullptr;
	return result;
}

static bool read_dtg(yyjson_val *value, vmf_dtg *result)
{
	if (yyjson_is_null(value))
		return true;
	static const char *const keys[] = {
		"year", "month", "day", "hour", "minute", "second", "extension"};
	yyjson_val *members[7];

	if (!complete_object(value, keys, 7, members) ||
		!byte(members[0], &result->year) ||
		!byte(members[1], &result->month) ||
		!byte(members[2], &result->day) ||
		!byte(members[3], &result->hour) ||
		!byte(members[4], &result->minute) ||
		!byte(members[5], &result->second) ||
		!read_optional_word(members[6], &result->extension))
		return false;
	result->present = true;
	return true;
}

static yyjson_mut_val *acknowledgment(yyjson_mut_doc *document,
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

static bool read_acknowledgment(yyjson_val *value, vmf_ack_request *result)
{
	if (yyjson_is_null(value))
		return true;
	static const char *const keys[] = {
		"machine", "operator_ack", "operator_reply"};
	yyjson_val *members[3];

	if (!complete_object(value, keys, 3, members) ||
		!boolean(members[0], &result->machine) ||
		!boolean(members[1], &result->operator_ack) ||
		!boolean(members[2], &result->operator_reply))
		return false;
	result->present = true;
	return true;
}

static yyjson_mut_val *response(yyjson_mut_doc *document,
	const vmf_response *value)
{
	if (!value->present)
		return yyjson_mut_null(document);
	yyjson_mut_val *result = yyjson_mut_obj(document);

	if (!put(document, result, "acknowledged",
			dtg(document, &value->acknowledged)) ||
		!put(document, result, "receipt_compliance",
			yyjson_mut_uint(document, value->receipt_compliance)) ||
		!put(document, result, "cantco_reason",
			optional_byte(document, &value->cantco_reason)) ||
		!put(document, result, "cantpro_reason",
			optional_byte(document, &value->cantpro_reason)) ||
		!put(document, result, "amplification",
			text(document, &value->amplification)))
		return nullptr;
	return result;
}

static bool read_response(yyjson_val *value, cli_arena *arena,
	vmf_response *result)
{
	if (yyjson_is_null(value))
		return true;
	static const char *const keys[] = {"acknowledged", "receipt_compliance",
		"cantco_reason", "cantpro_reason", "amplification"};
	yyjson_val *members[5];

	if (!complete_object(value, keys, 5, members) ||
		!read_dtg(members[0], &result->acknowledged) ||
		!byte(members[1], &result->receipt_compliance) ||
		!read_optional_byte(members[2], &result->cantco_reason) ||
		!read_optional_byte(members[3], &result->cantpro_reason) ||
		!read_text(members[4], arena, &result->amplification))
		return false;
	result->present = true;
	return true;
}

static bool array(yyjson_val *value, cli_arena *arena, size_t element_size,
	size_t maximum, void **storage, size_t *count)
{
	if (!yyjson_is_arr(value))
		return false;
	*count = yyjson_arr_size(value);
	*storage = nullptr;
	if (*count > maximum || *count > SIZE_MAX / element_size)
		return false;
	if (!*count)
		return true;
	*storage = cli_arena_alloc(arena, *count * element_size);
	return *storage != nullptr;
}

static yyjson_mut_val *reference(yyjson_mut_doc *document,
	vmf_envelope_version profile, const vmf_reference *value)
{
	yyjson_mut_val *result = yyjson_mut_obj(document);

	if (!put(document, result, "originator",
			address(document, &value->originator)) ||
		!put(document, result, "dtg", dtg(document, &value->dtg)))
		return nullptr;
	if (profile == VMF_ENVELOPE_B &&
		(!put(document, result, "functional_area",
			yyjson_mut_uint(document, value->functional_area)) ||
		!put(document, result, "message_number",
			yyjson_mut_uint(document, value->message_number))))
		return nullptr;
	return result;
}

static bool read_reference(yyjson_val *value, vmf_envelope_version profile,
	cli_arena *arena, vmf_reference *result)
{
	static const char *const keys[] = {
		"originator", "dtg", "functional_area", "message_number"};
	yyjson_val *members[4];
	size_t count = profile == VMF_ENVELOPE_B ? 4 : 2;

	if (!complete_object(value, keys, count, members) ||
		!read_address(members[0], arena, &result->originator) ||
		!read_dtg(members[1], &result->dtg))
		return false;
	return profile != VMF_ENVELOPE_B ||
	       (byte(members[2], &result->functional_area) &&
		       byte(members[3], &result->message_number));
}

static yyjson_mut_val *security(yyjson_mut_doc *document,
	const vmf_security *value)
{
	if (!value->present)
		return yyjson_mut_null(document);
	yyjson_mut_val *result = yyjson_mut_obj(document);
	yyjson_mut_val *tokens = yyjson_mut_arr(document);

	if (!tokens)
		return nullptr;
	for (size_t index = 0; index < value->token_count; ++index) {
		yyjson_mut_val *token = bytes(document, &value->tokens[index]);

		if (!token || !yyjson_mut_arr_add_val(tokens, token))
			return nullptr;
	}
	if (!put(document, result, "spi",
			yyjson_mut_uint(document, value->spi)) ||
		!put(document, result, "keying_material",
			bytes(document, &value->keying_material)) ||
		!put(document, result, "initialization",
			bytes(document, &value->initialization)) ||
		!put(document, result, "tokens_present",
			yyjson_mut_bool(document, value->tokens_present)) ||
		!put(document, result, "tokens", tokens) ||
		!put(document, result, "authentication_a",
			bytes(document, &value->authentication_a)) ||
		!put(document, result, "authentication_b",
			bytes(document, &value->authentication_b)) ||
		!put(document, result, "signed_ack_request",
			yyjson_mut_bool(document, value->signed_ack_request)) ||
		!put(document, result, "padding_present",
			yyjson_mut_bool(document, value->padding_present)) ||
		!put(document, result, "padding_length",
			yyjson_mut_uint(document, value->padding_length)) ||
		!put(document, result, "padding", bytes(document, &value->padding)))
		return nullptr;
	return result;
}

static bool read_security(yyjson_val *value, cli_arena *arena,
	vmf_security *result)
{
	if (yyjson_is_null(value))
		return true;
	static const char *const keys[] = {
		"spi", "keying_material", "initialization", "tokens_present",
		"tokens", "authentication_a", "authentication_b",
		"signed_ack_request", "padding_present", "padding_length",
		"padding"};
	yyjson_val *members[11];
	void *storage;

	if (!complete_object(value, keys, 11, members) ||
		!byte(members[0], &result->spi) ||
		!read_bytes(members[1], arena, &result->keying_material) ||
		!read_bytes(members[2], arena, &result->initialization) ||
		!boolean(members[3], &result->tokens_present) ||
		!array(members[4], arena, sizeof(vmf_header_bytes), 17,
			&storage, &result->token_count))
		return false;
	auto *tokens = static_cast<vmf_header_bytes *>(storage);

	result->tokens = tokens;
	for (size_t index = 0; index < result->token_count; ++index)
		if (!read_bytes(yyjson_arr_get(members[4], index), arena,
				&tokens[index]))
			return false;
	if (!read_bytes(members[5], arena, &result->authentication_a) ||
		!read_bytes(members[6], arena, &result->authentication_b) ||
		!boolean(members[7], &result->signed_ack_request) ||
		!boolean(members[8], &result->padding_present) ||
		!byte(members[9], &result->padding_length) ||
		!read_bytes(members[10], arena, &result->padding))
		return false;
	result->present = true;
	return true;
}

static yyjson_mut_val *message(yyjson_mut_doc *document,
	vmf_envelope_version profile, const vmf_message_header *value)
{
	yyjson_mut_val *result = yyjson_mut_obj(document);
	yyjson_mut_val *references = yyjson_mut_arr(document);

	if (!references)
		return nullptr;
	for (size_t index = 0; index < value->reference_count; ++index) {
		yyjson_mut_val *entry = reference(document, profile,
			&value->references[index]);

		if (!entry || !yyjson_mut_arr_add_val(references, entry))
			return nullptr;
	}
	if (!put(document, result, "id", cli_id_to_json(document, &value->id)) ||
		!put(document, result, "file_name",
			text(document, &value->file_name)) ||
		!put(document, result, "message_size_octets",
			optional_word(document, &value->message_size_octets)) ||
		!put(document, result, "operation",
			yyjson_mut_uint(document, value->operation)) ||
		!put(document, result, "retransmit",
			yyjson_mut_bool(document, value->retransmit)) ||
		!put(document, result, "precedence",
			yyjson_mut_uint(document, value->precedence)) ||
		!put(document, result, "classification",
			yyjson_mut_uint(document, value->classification)) ||
		!put(document, result, "originator_dtg",
			dtg(document, &value->originator_dtg)) ||
		!put(document, result, "perishability",
			dtg(document, &value->perishability)) ||
		!put(document, result, "acknowledgment",
			acknowledgment(document, &value->acknowledgment)) ||
		!put(document, result, "response",
			response(document, &value->response)) ||
		!put(document, result, "references", references))
		return nullptr;
	if (profile == VMF_ENVELOPE_B &&
		!put(document, result, "marking_b",
			optional_word(document, &value->marking_b)))
		return nullptr;
	if (profile == VMF_ENVELOPE_C &&
		!put(document, result, "marking_c", text(document, &value->marking_c)))
		return nullptr;
	if (profile == VMF_ENVELOPE_D || profile == VMF_ENVELOPE_D1) {
		yyjson_mut_val *markings = yyjson_mut_arr(document);

		if (!markings)
			return nullptr;
		for (size_t index = 0; index < value->marking_count; ++index) {
			yyjson_mut_val *entry = yyjson_mut_uint(document,
				value->markings[index]);

			if (!entry || !yyjson_mut_arr_add_val(markings, entry))
				return nullptr;
		}
		if (!put(document, result, "markings", markings))
			return nullptr;
	}
	if (profile != VMF_ENVELOPE_B &&
		!put(document, result, "security", security(document, &value->security)))
		return nullptr;
	return result;
}

static bool read_message(yyjson_val *value, vmf_envelope_version profile,
	cli_arena *arena, vmf_message_header *result)
{
	const char *keys[] = {
		"id", "file_name", "message_size_octets", "operation",
		"retransmit", "precedence", "classification", "originator_dtg",
		"perishability", "acknowledgment", "response", "references",
		"marking_b", "security"};
	yyjson_val *members[14];
	size_t count = profile == VMF_ENVELOPE_B ? 13 : 14;

	if (profile == VMF_ENVELOPE_C)
		keys[12] = "marking_c";
	else if (profile == VMF_ENVELOPE_D || profile == VMF_ENVELOPE_D1)
		keys[12] = "markings";
	if (!complete_object(value, keys, count, members) ||
		!cli_id_from_json(members[0], &result->id) ||
		!read_text(members[1], arena, &result->file_name) ||
		!read_optional_word(members[2], &result->message_size_octets) ||
		!byte(members[3], &result->operation) ||
		!boolean(members[4], &result->retransmit) ||
		!byte(members[5], &result->precedence) ||
		!byte(members[6], &result->classification) ||
		!read_dtg(members[7], &result->originator_dtg) ||
		!read_dtg(members[8], &result->perishability) ||
		!read_acknowledgment(members[9], &result->acknowledgment) ||
		!read_response(members[10], arena, &result->response))
		return false;
	void *storage;

	if (!array(members[11], arena, sizeof(vmf_reference), 4,
			&storage, &result->reference_count))
		return false;
	auto *references = static_cast<vmf_reference *>(storage);

	result->references = references;
	for (size_t index = 0; index < result->reference_count; ++index)
		if (!read_reference(yyjson_arr_get(members[11], index), profile,
				arena, &references[index]))
			return false;
	if (profile == VMF_ENVELOPE_B) {
		if (!read_optional_word(members[12], &result->marking_b) ||
			result->id.standard_version.present)
			return false;
	} else if (profile == VMF_ENVELOPE_C) {
		if (!read_text(members[12], arena, &result->marking_c))
			return false;
	} else {
		if (!array(members[12], arena, sizeof(uint16_t), 16,
				&storage, &result->marking_count))
			return false;
		auto *markings = static_cast<uint16_t *>(storage);

		result->markings = markings;
		for (size_t index = 0; index < result->marking_count; ++index) {
			uint64_t parsed;

			if (!number(yyjson_arr_get(members[12], index),
					UINT16_MAX, &parsed))
				return false;
			markings[index] = static_cast<uint16_t>(parsed);
		}
	}
	if (profile != VMF_ENVELOPE_B &&
		!read_security(members[13], arena, &result->security))
		return false;
	result->has_body = !result->response.present;
	return true;
}

static yyjson_mut_val *addresses(yyjson_mut_doc *document,
	const vmf_address *values, size_t count)
{
	yyjson_mut_val *result = yyjson_mut_arr(document);

	if (!result)
		return nullptr;
	for (size_t index = 0; index < count; ++index) {
		yyjson_mut_val *entry = address(document, &values[index]);

		if (!entry || !yyjson_mut_arr_add_val(result, entry))
			return nullptr;
	}
	return result;
}

static bool read_addresses(yyjson_val *value, cli_arena *arena,
	const vmf_address **result, size_t *count)
{
	void *storage;

	if (!array(value, arena, sizeof(vmf_address), 16, &storage, count))
		return false;
	auto *values = static_cast<vmf_address *>(storage);

	*result = values;
	for (size_t index = 0; index < *count; ++index)
		if (!read_address(yyjson_arr_get(value, index), arena,
				&values[index]) || !values[index].present)
			return false;
	return true;
}

static yyjson_mut_val *fields(yyjson_mut_doc *document,
	vmf_envelope_version profile, const vmf_header_fields *value)
{
	yyjson_mut_val *result = yyjson_mut_obj(document);
	yyjson_mut_val *messages = yyjson_mut_arr(document);

	if (!messages)
		return nullptr;
	for (size_t index = 0; index < value->message_count; ++index) {
		yyjson_mut_val *entry = message(document, profile,
			&value->messages[index]);

		if (!entry || !yyjson_mut_arr_add_val(messages, entry))
			return nullptr;
	}
	if (!put(document, result, "compression",
			optional_byte(document, &value->compression)) ||
		!put(document, result, "originator",
			address(document, &value->originator)) ||
		!put(document, result, "recipients",
			addresses(document, value->recipients, value->recipient_count)) ||
		!put(document, result, "information_addressees",
			addresses(document, value->information_addressees,
				value->information_addressee_count)) ||
		!put(document, result, "messages", messages))
		return nullptr;
	if (profile != VMF_ENVELOPE_B &&
		!put(document, result, "header_size_octets",
			optional_word(document, &value->header_size_octets)))
		return nullptr;
	return result;
}

static bool read_fields(yyjson_val *value, vmf_envelope_version profile,
	cli_arena *arena, vmf_header_fields *result)
{
	static const char *const keys[] = {"compression", "originator",
		"recipients", "information_addressees", "messages",
		"header_size_octets"};
	yyjson_val *members[6];
	size_t count = profile == VMF_ENVELOPE_B ? 5 : 6;

	if (!complete_object(value, keys, count, members) ||
		!read_optional_byte(members[0], &result->compression) ||
		!read_address(members[1], arena, &result->originator) ||
		!read_addresses(members[2], arena, &result->recipients,
			&result->recipient_count) ||
		!read_addresses(members[3], arena, &result->information_addressees,
			&result->information_addressee_count))
		return false;
	if (result->recipient_count + result->information_addressee_count > 16)
		return false;
	void *storage;

	if (!array(members[4], arena, sizeof(vmf_message_header), 16,
			&storage, &result->message_count))
		return false;
	auto *messages = static_cast<vmf_message_header *>(storage);

	result->messages = messages;
	for (size_t index = 0; index < result->message_count; ++index)
		if (!read_message(yyjson_arr_get(members[4], index), profile,
				arena, &messages[index]))
			return false;
	return profile == VMF_ENVELOPE_B ||
	       read_optional_word(members[5], &result->header_size_octets);
}

} /* namespace */

yyjson_mut_val *cli_header_to_json(yyjson_mut_doc *document,
	vmf_envelope_version profile, const void *header)
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

	if (!put(document, result, "fields", fields(document, profile, value)))
		return nullptr;
	if (profile != VMF_ENVELOPE_B &&
		!put(document, result, "shortened",
			yyjson_mut_bool(document, shortened)))
		return nullptr;
	return result;
}

bool cli_header_from_json(yyjson_val *value, vmf_envelope_version profile,
	cli_arena *arena, vmf_header *header)
{
	if (!arena || !header)
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
	if (!complete_object(value, keys, count, members) ||
		!read_fields(members[0], profile, arena, target) ||
		(shortened && !boolean(members[1], shortened)))
		return false;
	*header = parsed;
	return true;
}
