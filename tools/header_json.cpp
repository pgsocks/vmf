#include "cli.hpp"
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vmf/header_json.h>

namespace
{

constexpr size_t memory_limit = 64 * 1024 * 1024;
constexpr size_t line_limit = 16 * 1024 * 1024;

struct alignas(std::max_align_t) json_block {
	size_t size;
};

struct header_owner {
	header_owner *next;
	vmf_header_json_document *document;
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
static bool object(yyjson_val *value, const char *const *names, size_t count,
		   uint64_t required, yyjson_val **members)
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

} /* namespace */

void cli_json_init(cli_json *context)
{
	if (!context)
		return;
	*context = {};
	context->allocator = {json_allocate, json_reallocate, json_release,
			      context};
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
	auto *block = new (memory)
	    arena_block{static_cast<arena_block *>(arena->blocks), total};

	arena->blocks = block;
	arena->used += total;
	std::memset(block + 1, 0, size);
	return block + 1;
}

void cli_arena_destroy(cli_arena *arena)
{
	if (!arena)
		return;
	auto *owner = static_cast<header_owner *>(arena->header_documents);

	while (owner) {
		vmf_header_json_destroy(owner->document);
		owner = owner->next;
	}
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

yyjson_mut_val *cli_header_to_json(yyjson_mut_doc *document,
				   vmf_envelope_version profile,
				   const void *header)
{
	if (!document || !header)
		return nullptr;
	auto options = vmf_header_json_default_options();
	vmf_header_json_text *text = nullptr;

	if (vmf_header_json_serialize(profile, header, &options, &text,
				      nullptr) != VMF_OK)
		return nullptr;
	cli_json context;

	cli_json_init(&context);
	yyjson_doc *parsed =
	    cli_json_read(&context, vmf_header_json_text_data(text),
			  vmf_header_json_text_size(text));
	yyjson_mut_val *result =
	    parsed ? yyjson_val_mut_copy(document, yyjson_doc_get_root(parsed))
		   : nullptr;

	yyjson_doc_free(parsed);
	vmf_header_json_text_destroy(text);
	return result;
}

bool cli_header_from_json(yyjson_val *value, vmf_envelope_version profile,
			  cli_arena *arena, vmf_header *header)
{
	if (!value || !arena || !header)
		return false;
	cli_json context;

	cli_json_init(&context);
	size_t size = 0;
	char *bytes =
	    yyjson_val_write_opts(value, 0, &context.allocator, &size, nullptr);

	if (!bytes)
		return false;
	auto options = vmf_header_json_default_options();
	vmf_header_json_document *document = nullptr;
	vmf_status status = vmf_header_json_load(profile, bytes, size, &options,
						 &document, nullptr);

	context.allocator.free(context.allocator.ctx, bytes);
	if (status != VMF_OK)
		return false;
	auto *owner = static_cast<header_owner *>(
	    cli_arena_alloc(arena, sizeof(header_owner)));

	if (!owner) {
		vmf_header_json_destroy(document);
		return false;
	}
	*owner = {static_cast<header_owner *>(arena->header_documents),
		  document};
	arena->header_documents = owner;
	*header = *vmf_header_json_header(document);
	return true;
}
