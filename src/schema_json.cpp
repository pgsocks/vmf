#include <vmf/schema_json.h>
#include <yyjson.h>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

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

} /* namespace */

struct vmf_schema_json_document {
	memory_state memory;
	vmf_schema_set schemas;
};

namespace
{

struct source {
	yyjson_val *value;
	yyjson_val *key;
	size_t parent;
	size_t position;
	size_t depth;
};

struct reference;

struct message_source {
	yyjson_val *value;
	yyjson_val **indexes;
	yyjson_val **repeats;
	reference *references;
};

struct reference {
	reference *next;
	vmf_index_number *target;
	yyjson_val *value;
	size_t index_position;
};

struct conversion {
	vmf_schema_json_document *document;
	const vmf_limits *limits;
	vmf_schema_json_error *error;
	vmf_status status;
	size_t schema_index;
	size_t index_position;
	vmf_schema_property property;
	yyjson_val *current;
	yyjson_val *related;
	source *sources;
	size_t source_count;
	message_source *messages;
	reference *references;
};

static vmf_schema_json_error diagnostic(vmf_status status)
{
	vmf_schema_json_error result = {};

	result.status = status;
	result.message = vmf_status_string(status);
	result.byte_offset = SIZE_MAX;
	result.schema_index = SIZE_MAX;
	result.index_position = SIZE_MAX;
	result.related_index_position = SIZE_MAX;
	return result;
}

static void append_path(char *path, bool *truncated, const char *text,
	size_t size)
{
	if (*truncated)
		return;
	size_t used = std::strlen(path);

	if (used + 1 >= 512) {
		*truncated = true;
		return;
	}
	path[used++] = '/';
	for (size_t i = 0; i < size;) {
		const char *piece = text + i;
		size_t consumed = 1, length = 1;
		unsigned char code = static_cast<unsigned char>(text[i]);

		if (!code) {
			*truncated = true;
			break;
		}
		if (code >= 0x80) {
			consumed = code < 0xe0 ? 2 : (code < 0xf0 ? 3 : 4);
			length = consumed;
		} else if (code == '~' || code == '/') {
			piece = code == '~' ? "~0" : "~1";
			length = 2;
		}
		if (length >= 512 - used || consumed > size - i) {
			*truncated = true;
			break;
		}
		std::memcpy(path + used, piece, length);
		used += length;
		i += consumed;
	}
	path[used] = 0;
}

/* Source parents form an explicit tree. Diagnostics need only the first
 * 512 edges: every later edge would exceed the fixed pointer buffer.
 */
static void source_path(conversion *c, yyjson_val *value, char *path,
	bool *truncated)
{
	if (!value)
		return;
	size_t found = SIZE_MAX;

	for (size_t i = 0; i < c->source_count; ++i)
		if (c->sources[i].value == value || c->sources[i].key == value) {
			found = i;
			break;
		}
	if (found == SIZE_MAX)
		return;
	size_t depth = c->sources[found].depth;
	bool longer = depth > 512;
	size_t edges[512];

	while (depth > 512) {
		found = c->sources[found].parent;
		--depth;
	}
	size_t count = depth;

	while (depth) {
		edges[--depth] = found;
		found = c->sources[found].parent;
	}
	for (size_t i = 0; i < count && !*truncated; ++i) {
		const source *entry = &c->sources[edges[i]];

		if (entry->key) {
			append_path(path, truncated, yyjson_get_str(entry->key),
				yyjson_get_len(entry->key));
		} else {
			char index[32];

			std::snprintf(index, sizeof(index), "%zu", entry->position);
			append_path(path, truncated, index, std::strlen(index));
		}
	}
	*truncated = *truncated || longer;
}

static bool fail(conversion *c, vmf_status status, const char *message)
{
	if (c->status == VMF_OK) {
		c->status = status;
		if (c->error) {
			*c->error = diagnostic(status);
			c->error->message = message;
			c->error->schema_index = c->schema_index;
			c->error->index_position = c->index_position;
			c->error->property = c->property;
			source_path(c, c->current, c->error->path,
				&c->error->path_truncated);
			source_path(c, c->related, c->error->related_path,
				&c->error->related_path_truncated);
		}
	}
	return false;
}

static bool invalid(conversion *c, const char *message)
{
	return fail(c, VMF_INVALID_SCHEMA, message);
}

static void *reserve(conversion *c, size_t count, size_t size)
{
	if (!count)
		return nullptr;
	if (count > SIZE_MAX / size) {
		fail(c, VMF_LIMIT_EXCEEDED, "allocation size overflow");
		return nullptr;
	}
	void *memory = memory_allocate(&c->document->memory, count * size);

	if (!memory) {
		vmf_status status = c->document->memory.failure;

		fail(c, status, vmf_status_string(status));
		return nullptr;
	}
	std::memset(memory, 0, count * size);
	return memory;
}

/* Breadth-first indexing visits every parser value without recursion and
 * retains only borrowed parser pointers until native validation finishes.
 */
static bool source_tree(conversion *c, yyjson_doc *document)
{
	size_t capacity = yyjson_doc_get_val_count(document);

	c->sources = static_cast<source *>(reserve(c, capacity, sizeof(source)));
	if (!c->sources)
		return false;
	c->source_count = 1;
	c->sources[0] = {yyjson_doc_get_root(document), nullptr, SIZE_MAX, 0, 0};
	for (size_t i = 0; i < c->source_count; ++i) {
		yyjson_val *value = c->sources[i].value;
		size_t depth = c->sources[i].depth + 1;

		if (yyjson_is_obj(value)) {
			yyjson_obj_iter iter = yyjson_obj_iter_with(value);
			yyjson_val *key;

			while ((key = yyjson_obj_iter_next(&iter))) {
				c->sources[c->source_count++] = {
					yyjson_obj_iter_get_val(key), key, i, 0, depth};
			}
		} else if (yyjson_is_arr(value)) {
			yyjson_arr_iter iter = yyjson_arr_iter_with(value);
			yyjson_val *child;
			size_t position = 0;

			while ((child = yyjson_arr_iter_next(&iter)))
				c->sources[c->source_count++] = {
					child, nullptr, i, position++, depth};
		}
	}
	return true;
}

static bool string_equal(yyjson_val *value, const char *text)
{
	return yyjson_is_str(value) &&
	       yyjson_get_len(value) == std::strlen(text) &&
	       !std::memcmp(yyjson_get_str(value), text, std::strlen(text));
}

/* Each object has a fixed vocabulary. A bitset detects decoded-key duplicates
 * without allocating a hash table or accepting escaped aliases.
 */
static bool object(conversion *c, yyjson_val *value,
	const char *const *keys, size_t count, uint64_t required,
	yyjson_val **members)
{
	c->current = value;
	if (!yyjson_is_obj(value))
		return invalid(c, "expected object");
	std::memset(members, 0, count * sizeof(*members));
	uint64_t seen = 0;
	yyjson_obj_iter iter = yyjson_obj_iter_with(value);
	yyjson_val *key;

	while ((key = yyjson_obj_iter_next(&iter))) {
		size_t i;

		for (i = 0; i < count; ++i)
			if (string_equal(key, keys[i]))
				break;
		if (i == count) {
			c->current = key;
			return invalid(c, "unknown or inactive property");
		}
		uint64_t bit = UINT64_C(1) << i;

		if (seen & bit) {
			c->current = key;
			return invalid(c, "duplicate property");
		}
		seen |= bit;
		members[i] = yyjson_obj_iter_get_val(key);
	}
	for (size_t i = 0; i < count; ++i) {
		if (!(required & (UINT64_C(1) << i)) || members[i])
			continue;
		invalid(c, "missing required property");
		if (c->error)
			append_path(c->error->path, &c->error->path_truncated,
				keys[i], std::strlen(keys[i]));
		return false;
	}
	return true;
}

static bool array(conversion *c, yyjson_val *value, size_t *count)
{
	c->current = value;
	if (!yyjson_is_arr(value))
		return invalid(c, "expected array");
	*count = yyjson_arr_size(value);
	return true;
}

static bool unsigned_integer(conversion *c, yyjson_val *value,
	uint64_t maximum, uint64_t *result)
{
	c->current = value;
	uint64_t number;

	if (yyjson_is_uint(value))
		number = yyjson_get_uint(value);
	else if (yyjson_is_sint(value) && yyjson_get_sint(value) >= 0)
		number = (uint64_t)yyjson_get_sint(value);
	else
		return invalid(c, "expected unsigned integer");
	if (number > maximum)
		return invalid(c, "integer out of range");
	*result = number;
	return true;
}

static bool signed_integer(conversion *c, yyjson_val *value, int64_t *result)
{
	c->current = value;
	if (yyjson_is_sint(value)) {
		*result = yyjson_get_sint(value);
		return true;
	}
	if (yyjson_is_uint(value) && yyjson_get_uint(value) <= INT64_MAX) {
		*result = (int64_t)yyjson_get_uint(value);
		return true;
	}
	return invalid(c, "expected signed 64-bit integer");
}

static bool boolean(conversion *c, yyjson_val *value, bool *result)
{
	c->current = value;
	if (!yyjson_is_bool(value))
		return invalid(c, "expected boolean");
	*result = yyjson_get_bool(value);
	return true;
}

static bool enumeration(conversion *c, yyjson_val *value,
	const char *const *names, size_t count, int *result)
{
	c->current = value;
	for (size_t i = 0; i < count; ++i)
		if (string_equal(value, names[i])) {
			*result = (int)i;
			return true;
		}
	return invalid(c, "unknown enumeration value");
}

static bool string(conversion *c, yyjson_val *value, bool terminated,
	const char **result, size_t *length)
{
	c->current = value;
	if (!yyjson_is_str(value))
		return invalid(c, "expected string");
	const char *source = yyjson_get_str(value);
	size_t size = yyjson_get_len(value);

	if (terminated && std::memchr(source, 0, size))
		return invalid(c, "NUL in terminated name");
	if (size == SIZE_MAX)
		return fail(c, VMF_LIMIT_EXCEEDED, "string size overflow");
	char *copy = static_cast<char *>(reserve(c, size + 1, 1));

	if (!copy)
		return false;
	std::memcpy(copy, source, size);
	*result = copy;
	if (length)
		*length = size;
	return true;
}

/* Only notation is interpreted here; ownership and defaults belong to core. */
static bool decimal(conversion *c, const char *text, size_t size,
	size_t *position, uint64_t maximum, uint64_t *result)
{
	size_t start = *position;
	uint64_t value = 0;

	while (*position < size && text[*position] >= '0' &&
		text[*position] <= '9') {
		unsigned digit = text[(*position)++] - '0';

		if (value > (maximum - digit) / 10)
			return invalid(c, "notation integer out of range");
		value = value * 10 + digit;
	}
	if (*position == start)
		return invalid(c, "expected notation integer");
	*result = value;
	return true;
}

static bool notation(conversion *c, yyjson_val *value,
	const char **text, size_t *size)
{
	c->current = value;
	if (!yyjson_is_str(value))
		return invalid(c, "expected notation string");
	*text = yyjson_get_str(value);
	*size = yyjson_get_len(value);
	return (*size && !std::memchr(*text, 0, *size)) ||
	       invalid(c, "empty or NUL-containing notation");
}

static bool ranges(conversion *c, yyjson_val *value,
	const vmf_code_range **result, size_t *count)
{
	if (!array(c, value, count))
		return false;
	vmf_code_range *items = static_cast<vmf_code_range *>(
		reserve(c, *count, sizeof(*items)));

	if (*count && !items)
		return false;
	yyjson_arr_iter iter = yyjson_arr_iter_with(value);
	yyjson_val *item;
	size_t index = 0;
	static const char *const keys[] = {"first", "last"};

	while ((item = yyjson_arr_iter_next(&iter))) {
		yyjson_val *m[2];

		if (!object(c, item, keys, 2, 3, m) ||
			!unsigned_integer(c, m[0], UINT64_MAX,
				&items[index].first) ||
			!unsigned_integer(c, m[1], UINT64_MAX,
				&items[index].last))
			return false;
		++index;
	}
	*result = items;
	return true;
}

static bool rational(conversion *c, yyjson_val *value, vmf_rational *result)
{
	static const char *const keys[] = {"numerator", "denominator"};
	yyjson_val *m[2];

	return object(c, value, keys, 2, 3, m) &&
	       signed_integer(c, m[0], &result->numerator) &&
	       unsigned_integer(c, m[1], UINT64_MAX, &result->denominator);
}

static bool scale(conversion *c, yyjson_val *value,
	vmf_scale_definition *result)
{
	static const char *const keys[] = {"factor", "bias", "sentinels"};
	yyjson_val *m[3];

	if (!object(c, value, keys, 3, 3, m) ||
		!rational(c, m[0], &result->factor) ||
		!rational(c, m[1], &result->bias))
		return false;
	if (!m[2])
		return true;
	if (!array(c, m[2], &result->sentinel_count))
		return false;
	uint64_t *codes = static_cast<uint64_t *>(
		reserve(c, result->sentinel_count, sizeof(*codes)));

	if (result->sentinel_count && !codes)
		return false;
	result->sentinels = codes;
	yyjson_arr_iter iter = yyjson_arr_iter_with(m[2]);
	yyjson_val *item;
	size_t i = 0;

	while ((item = yyjson_arr_iter_next(&iter)))
		if (!unsigned_integer(c, item, UINT64_MAX, &codes[i++]))
			return false;
	return true;
}

static bool field_reference(conversion *c, yyjson_val *value,
	vmf_index_number *target)
{
	const char *text;
	size_t size;

	c->property = VMF_PROPERTY_DEPENDENCY;
	if (!notation(c, value, &text, &size))
		return false;
	reference *entry = static_cast<reference *>(
		reserve(c, 1, sizeof(reference)));

	if (!entry)
		return false;
	*entry = {c->references, target, value, c->index_position};
	c->references = entry;
	return true;
}

static bool condition(conversion *c, yyjson_val *value, vmf_condition *result)
{
	static const char *const keys[] = {"all", "any", "negate"};
	yyjson_val *m[3];

	if (!object(c, value, keys, 3, 0, m))
		return false;
	if (!!m[0] == !!m[1])
		return invalid(c, "condition requires either all or any");
	result->mode = m[0] ? VMF_CONDITION_ALL : VMF_CONDITION_ANY;
	if (!array(c, m[0] ? m[0] : m[1], &result->term_count) ||
		(m[2] && !boolean(c, m[2], &result->negate)))
		return false;
	if (!result->term_count)
		return invalid(c, "empty condition");
	if (result->term_count > c->limits->max_schema_nodes)
		return fail(c, VMF_LIMIT_EXCEEDED, "condition term limit exceeded");
	vmf_condition_term *terms = static_cast<vmf_condition_term *>(
		reserve(c, result->term_count, sizeof(*terms)));

	if (!terms)
		return false;
	result->terms = terms;
	yyjson_arr_iter iter = yyjson_arr_iter_with(m[0] ? m[0] : m[1]);
	yyjson_val *item;
	size_t index = 0;
	static const char *const term_keys[] = {
		"field", "mask", "values", "negate"};

	while ((item = yyjson_arr_iter_next(&iter))) {
		yyjson_val *t[4];
		vmf_condition_term *term = &terms[index++];

		if (!object(c, item, term_keys, 4, 5, t) ||
			!field_reference(c, t[0], &term->field) ||
			!ranges(c, t[2], &term->values, &term->value_count) ||
			(t[3] && !boolean(c, t[3], &term->negate)))
			return false;
		if (t[1]) {
			term->mask.present = true;
			if (!unsigned_integer(c, t[1], UINT64_MAX,
				&term->mask.value))
				return false;
		}
	}
	return true;
}

static bool field(conversion *c, yyjson_val *value,
	vmf_index_schema *result)
{
	static const char *const keys[] = {"key", "number_of_bits", "type",
		"bit_order", "text_encoding", "length_from", "length_bias",
		"when", "allowed", "constant", "scale"};
	static const char *const types[] = {"unsigned", "bool", "enum",
		"twos_complement", "ones_complement", "sign_magnitude", "bits",
		"octets", "text", "literal"};
	yyjson_val *m[11];
	int type = 0;
	const char *name;
	size_t name_size;

	c->property = VMF_PROPERTY_KEY;
	if (!object(c, value, keys, 11, 1, m) ||
		!notation(c, m[0], &name, &name_size) ||
		!string(c, m[0], true, &result->key.data, &result->key.size))
		return false;
	c->property = VMF_PROPERTY_ENCODING;
	if (m[2] && !enumeration(c, m[2], types, 10, &type))
		return false;
	if (m[1]) {
		if (!unsigned_integer(c, m[1], UINT64_MAX,
			&result->number_of_bits))
			return false;
	} else if (type == 1) {
		result->number_of_bits = 1;
	} else {
		c->current = value;
		invalid(c, "missing required property");
		if (c->error)
			append_path(c->error->path, &c->error->path_truncated,
				"number_of_bits", 14);
		return false;
	}
	static const vmf_encode_type encodings[] = {VMF_ENCODE_UNSIGNED,
		VMF_ENCODE_BOOL, VMF_ENCODE_ENUM, VMF_ENCODE_SIGNED,
		VMF_ENCODE_SIGNED, VMF_ENCODE_SIGNED, VMF_ENCODE_BITS,
		VMF_ENCODE_OCTETS, VMF_ENCODE_TEXT, VMF_ENCODE_LITERAL};

	result->encode_type = encodings[type];
	bool refined = type >= 3 && type <= 5;

	for (size_t i = 3; i < 11; ++i)
		refined = refined || m[i];
	if (!refined)
		return true;
	vmf_index_refinements *r = static_cast<vmf_index_refinements *>(
		reserve(c, 1, sizeof(*r)));

	if (!r)
		return false;
	result->refinements = r;
	if (type >= 3 && type <= 5)
		r->representation = static_cast<vmf_integer_representation>(
			VMF_INTEGER_TWOS_COMPLEMENT + type - 3);
	int code;
	static const char *const orders[] = {"default", "msb_first", "lsb_first"};
	static const char *const text_encodings[] = {"default", "ascii_7",
		"ascii_8", "latin_1", "utf_8"};

	if (m[3]) {
		if (!enumeration(c, m[3], orders, 3, &code))
			return false;
		r->bit_order = static_cast<vmf_bit_order>(code);
	}
	if (m[4]) {
		if (!enumeration(c, m[4], text_encodings, 5, &code))
			return false;
		r->text_encoding = static_cast<vmf_text_encoding>(code);
	}
	if (m[5] && !field_reference(c, m[5], &r->length_from))
		return false;
	if (m[6]) {
		r->length_bias.present = true;
		if (!signed_integer(c, m[6], &r->length_bias.value))
			return false;
	}
	if (m[7]) {
		vmf_condition *when = static_cast<vmf_condition *>(
			reserve(c, 1, sizeof(*when)));

		if (!when || !condition(c, m[7], when))
			return false;
		r->when = when;
	}
	if (m[8] && !ranges(c, m[8], &r->allowed, &r->allowed_count))
		return false;
	if (m[9]) {
		r->constant.present = true;
		if (!unsigned_integer(c, m[9], UINT64_MAX, &r->constant.value))
			return false;
	}
	if (m[10]) {
		c->current = m[10];
		if (type != 0 && (type < 3 || type > 5))
			return invalid(c, "scale requires an integer representation");
		vmf_scale_definition *value =
			static_cast<vmf_scale_definition *>(
				reserve(c, 1, sizeof(*value)));

		if (!value || !scale(c, m[10], value))
			return false;
		r->scale = value;
		result->encode_type = VMF_ENCODE_SCALED;
	}
	return true;
}

static bool standard(conversion *c, yyjson_val *edition,
	yyjson_val *raw, vmf_standard_selection *result)
{
	if (edition && raw)
		return invalid(c, "conflicting message standard selections");
	if (edition) {
		static const char *const names[] = {"MIL-STD-6017",
			"MIL-STD-6017A", "MIL-STD-6017B", "MIL-STD-6017C",
			"MIL-STD-6017D", "MIL-STD-6017E", "TIDP-TE-R2",
			"TIDP-TE-R3", "TIDP-TE-R4", "TIDP-TE-R5", "TIDP-TE-R6",
			"TIDP-TE-R7", "TIDP-FTE-R6"};
		int code;

		if (!enumeration(c, edition, names, 13, &code))
			return false;
		result->kind = VMF_STANDARD_EDITION;
		result->as.edition = (vmf_message_standard)code;
	} else if (raw) {
		if (string_equal(raw, "absent")) {
			result->kind = VMF_STANDARD_ABSENT;
			return true;
		}
		uint64_t code;

		if (!unsigned_integer(c, raw, UINT8_MAX, &code))
			return false;
		result->kind = VMF_STANDARD_RAW_CODE;
		result->as.raw_code = (uint8_t)code;
	}
	return true;
}

struct node_frame {
	yyjson_val *value;
	yyjson_val *children;
	yyjson_arr_iter iterator;
	size_t next;
	uint32_t component;
	uint32_t group;
	uint32_t repeat;
	uint64_t maximum;
	bool entered;
	bool recurrence;
};

struct lowering {
	conversion *context;
	vmf_message_schema *message;
	message_source *source;
	node_frame *frames;
	size_t stack;
	size_t capacity;
	size_t indexes;
	size_t repeats;
	size_t nodes;
	uint32_t groups;
	uint32_t repeat_ids;
	bool emit;
};

static bool direct_field(yyjson_val *value)
{
	return yyjson_is_obj(value) && !yyjson_obj_get(value, "optional") &&
	       !yyjson_obj_get(value, "recurrence") &&
	       !yyjson_obj_get(value, "repeat");
}

static bool add_nodes(lowering *l, size_t count)
{
	if (count > l->context->limits->max_schema_nodes - l->nodes)
		return fail(l->context, VMF_LIMIT_EXCEEDED,
			"schema node limit exceeded");
	l->nodes += count;
	return true;
}

static bool address(lowering *l, vmf_index_number *result, bool child)
{
	conversion *c = l->context;
	size_t count = l->stack - 1;

	if (child && count == c->limits->max_depth)
		return fail(c, VMF_LIMIT_EXCEEDED, "schema depth limit exceeded");
	uint32_t *parts = static_cast<uint32_t *>(
		reserve(c, count + (child ? 1 : 0), sizeof(*parts)));

	if (!parts)
		return false;
	for (size_t i = 0; i < count; ++i)
		parts[i] = l->frames[i + 1].component;
	if (child)
		parts[count++] = 1;
	*result = {parts, count};
	return true;
}

static bool emit_index(lowering *l, vmf_indicator_role indicator)
{
	conversion *c = l->context;
	size_t position = l->indexes++;

	c->index_position = position;
	if (l->indexes > c->limits->max_schema_nodes)
		return fail(c, VMF_LIMIT_EXCEEDED, "schema index limit exceeded");
	if (!l->emit)
		return true;
	node_frame *frame = &l->frames[l->stack - 1];
	vmf_index_schema *index = const_cast<vmf_index_schema *>(
		&l->message->indexes[position]);

	c->current = frame->value;
	l->source->indexes[position] = frame->value;
	if (!address(l, &index->index_number, false))
		return false;
	index->indicator = indicator;
	if (indicator != VMF_INDICATOR_NONE)
		index->number_of_bits = 1;
	size_t groups = 0, repeats = 0;

	for (size_t i = 1; i < l->stack; ++i) {
		groups += l->frames[i].group &&
			!(i + 1 == l->stack && indicator == VMF_INDICATOR_GPI);
		repeats += l->frames[i].repeat != 0;
	}
	vmf_group_id *group_parts = static_cast<vmf_group_id *>(
		reserve(c, groups, sizeof(*group_parts)));
	vmf_repeat_component *repeat_parts =
		static_cast<vmf_repeat_component *>(
			reserve(c, repeats, sizeof(*repeat_parts)));

	if ((groups && !group_parts) || (repeats && !repeat_parts))
		return false;
	index->group_code = {group_parts, groups};
	index->repeat_code = {repeat_parts, repeats};
	groups = 0;
	repeats = 0;
	for (size_t i = 1; i < l->stack; ++i) {
		const node_frame *part = &l->frames[i];

		if (part->group &&
			!(i + 1 == l->stack && indicator == VMF_INDICATOR_GPI))
			group_parts[groups++] = part->group;
		if (!part->repeat)
			continue;
		repeat_parts[repeats].id = part->repeat;
		if (i + 1 == l->stack && frame->recurrence) {
			repeat_parts[repeats].maximum = {true, frame->maximum};
		}
		++repeats;
	}
	if (indicator == VMF_INDICATOR_FPI) {
		index->target.kind = VMF_TARGET_INDEX;
		return address(l, &index->target.as.index, true);
	}
	if (indicator == VMF_INDICATOR_GPI) {
		index->target.kind = VMF_TARGET_GROUP;
		index->target.as.group = frame->group;
	}
	if (indicator != VMF_INDICATOR_NONE)
		return true;
	if (!field(c, frame->value, index))
		return false;
	for (size_t i = 0; i < position; ++i) {
		const vmf_index_schema *previous = &l->message->indexes[i];

		if (previous->key.size != index->key.size ||
			!previous->key.size || std::memcmp(previous->key.data,
				index->key.data, index->key.size))
			continue;
		c->current = yyjson_obj_get(frame->value, "key");
		c->related = yyjson_obj_get(l->source->indexes[i], "key");
		c->property = VMF_PROPERTY_KEY;
		invalid(c, "duplicate field key");
		if (c->error)
			c->error->related_index_position = i;
		return false;
	}
	return true;
}

static bool repeat_definition(lowering *l, yyjson_val *value,
	node_frame *frame)
{
	conversion *c = l->context;
	static const char *const keys[] = {"body", "count", "count_from",
		"maximum", "minimum", "bias"};
	yyjson_val *m[6];
	vmf_repeat_definition definition = {};

	if (!object(c, value, keys, 6, 1, m))
		return false;
	if (!!m[1] == !!m[2])
		return invalid(c, "repeat requires one count mechanism");
	definition.id = frame->repeat;
	frame->children = m[0];
	if (m[1]) {
		if (m[3] || m[4] || m[5]) {
			c->current = m[3] ? m[3] : (m[4] ? m[4] : m[5]);
			return invalid(c, "inactive fixed repeat property");
		}
		if (!unsigned_integer(c, m[1], UINT64_MAX - 1,
			&definition.count))
			return false;
	} else {
		definition.kind = VMF_REPEAT_FIELD;
		if (!m[3]) {
			c->current = value;
			invalid(c, "missing required property");
			if (c->error)
				append_path(c->error->path,
					&c->error->path_truncated, "maximum", 7);
			return false;
		}
		const char *key;
		size_t size;

		if (!notation(c, m[2], &key, &size) ||
			!unsigned_integer(c, m[3], UINT64_MAX - 1,
				&definition.maximum))
			return false;
		if (m[4]) {
			definition.minimum.present = true;
			if (!unsigned_integer(c, m[4], UINT64_MAX,
				&definition.minimum.value))
				return false;
		}
		if (m[5]) {
			definition.bias.present = true;
			if (!signed_integer(c, m[5], &definition.bias.value))
				return false;
		}
	}
	size_t position = l->repeats++;

	if (!l->emit)
		return true;
	vmf_repeat_definition *result = const_cast<vmf_repeat_definition *>(
		&l->message->repeats[position]);

	*result = definition;
	l->source->repeats[position] = frame->value;
	c->index_position = l->indexes;
	return !m[2] || field_reference(c, m[2], &result->count_from);
}

static bool enter_node(lowering *l)
{
	conversion *c = l->context;
	node_frame *frame = &l->frames[l->stack - 1];
	yyjson_val *value = frame->value;

	c->current = value;
	c->property = VMF_PROPERTY_INDEX;
	frame->entered = true;
	if (yyjson_is_arr(value)) {
		frame->children = value;
	} else if (yyjson_is_obj(value)) {
		yyjson_val *optional = yyjson_obj_get(value, "optional");
		yyjson_val *recurrence = yyjson_obj_get(value, "recurrence");
		yyjson_val *repeat = yyjson_obj_get(value, "repeat");

		if (!optional && !recurrence && !repeat) {
			return add_nodes(l, 1) &&
			       emit_index(l, VMF_INDICATOR_NONE);
		}
		const char *name = optional ? "optional" :
			(recurrence ? "recurrence" : "repeat");
		const char *keys[] = {name};
		yyjson_val *m[1];

		if (!object(c, value, keys, 1, 1, m))
			return false;
		if (optional) {
			frame->children = optional;
			bool leaf = direct_field(optional);

			if (!leaf) {
				if (!add_nodes(l, 1) || l->groups == UINT32_MAX)
					return fail(c, VMF_LIMIT_EXCEEDED,
						"group identifier limit exceeded");
				frame->group = ++l->groups;
			}
			if (!emit_index(l,
				leaf ? VMF_INDICATOR_FPI : VMF_INDICATOR_GPI))
				return false;
		} else {
			if (!add_nodes(l, 2) || l->repeat_ids == UINT32_MAX)
				return fail(c, VMF_LIMIT_EXCEEDED,
					"repeat identifier limit exceeded");
			frame->repeat = ++l->repeat_ids;
			if (recurrence) {
				static const char *const names[] = {"maximum", "body"};
				yyjson_val *parts[2];

				if (!object(c, recurrence, names, 2, 3, parts) ||
					!unsigned_integer(c, parts[0], UINT64_MAX - 1,
						&frame->maximum))
					return false;
				if (!frame->maximum)
					return invalid(c, "recurrence maximum must be positive");
				frame->children = parts[1];
				frame->recurrence = true;
				if (!emit_index(l, direct_field(parts[1]) ?
					VMF_INDICATOR_FRI : VMF_INDICATOR_GRI))
					return false;
			} else if (!repeat_definition(l, repeat, frame)) {
				return false;
			}
		}
	} else {
		return invalid(c, "expected field, group or wrapper");
	}
	if (yyjson_is_arr(frame->children)) {
		if (!yyjson_arr_size(frame->children)) {
			c->current = frame->children;
			return invalid(c, "empty nested group");
		}
		frame->iterator = yyjson_arr_iter_with(frame->children);
	}
	return true;
}

/* Root lists and wrapper body lists supply sibling positions directly.
 * Plain nested lists add an address level but no native group membership.
 * Each active frame owns the membership introduced by that wrapper.
 */
static bool walk(lowering *l, yyjson_val *elements)
{
	conversion *c = l->context;

	l->stack = 1;
	l->nodes = 1;
	if (!c->limits->max_schema_nodes)
		return fail(c, VMF_LIMIT_EXCEEDED, "schema node limit exceeded");
	l->frames[0] = {};
	l->frames[0].value = elements;
	l->frames[0].children = elements;
	l->frames[0].iterator = yyjson_arr_iter_with(elements);
	l->frames[0].entered = true;
	while (l->stack) {
		node_frame *frame = &l->frames[l->stack - 1];

		if (!frame->entered && !enter_node(l))
			return false;
		yyjson_val *child = nullptr;

		if (yyjson_is_arr(frame->children))
			child = yyjson_arr_iter_next(&frame->iterator);
		else if (frame->children && !frame->next)
			child = frame->children;
		if (!child) {
			--l->stack;
			continue;
		}
		c->current = child;
		if (l->stack == l->capacity ||
			l->stack > c->limits->max_depth || frame->next == UINT32_MAX)
			return fail(c, VMF_LIMIT_EXCEEDED,
				"schema depth or position limit exceeded");
		uint32_t component = static_cast<uint32_t>(++frame->next);

		l->frames[l->stack++] = {};
		l->frames[l->stack - 1].value = child;
		l->frames[l->stack - 1].component = component;
	}
	c->index_position = SIZE_MAX;
	return true;
}

static bool resolve_references(conversion *c,
	const vmf_message_schema *message)
{
	while (c->references) {
		reference *entry = c->references;
		const char *key = yyjson_get_str(entry->value);
		size_t size = yyjson_get_len(entry->value);
		const vmf_index_schema *found = nullptr;

		for (size_t i = 0; i < message->index_count; ++i) {
			const vmf_index_schema *candidate = &message->indexes[i];

			if (size == candidate->key.size &&
				!std::memcmp(key, candidate->key.data, size)) {
				found = candidate;
				break;
			}
		}
		if (!found) {
			c->current = entry->value;
			c->index_position = entry->index_position;
			c->property = VMF_PROPERTY_DEPENDENCY;
			return invalid(c, "unknown field reference");
		}
		*entry->target = found->index_number;
		c->references = entry->next;
		message_source *source = &c->messages[c->schema_index];

		entry->next = source->references;
		source->references = entry;
	}
	c->index_position = SIZE_MAX;
	return true;
}

static bool message(conversion *c, yyjson_val *value,
	vmf_message_schema *result)
{
	static const char *const keys[] = {"message_number", "elements",
		"message_standard", "standard_version", "subtype", "octet_order"};
	yyjson_val *m[6];
	const char *text;
	size_t size, position = 1;
	uint64_t area, number;

	c->property = VMF_PROPERTY_IDENTITY;
	if (!object(c, value, keys, 6, 3, m) ||
		!notation(c, m[0], &text, &size))
		return false;
	if (text[0] != 'K' ||
		!decimal(c, text, size, &position, UINT8_MAX, &area) ||
		position == size || text[position++] != '.' ||
		!decimal(c, text, size, &position, UINT8_MAX, &number) ||
		position != size)
		return invalid(c, "invalid K-series message number");
	result->message_number = {(uint8_t)area, (uint8_t)number};
	size_t count;

	if (!standard(c, m[2], m[3], &result->standard) ||
		!array(c, m[1], &count))
		return false;
	if (m[4]) {
		uint64_t subtype;

		if (!unsigned_integer(c, m[4], UINT8_MAX, &subtype))
			return false;
		result->subtype = {true, (uint8_t)subtype};
	}
	if (m[5]) {
		static const char *const names[] = {"default", "msb_first", "lsb_first"};
		int code;

		if (!enumeration(c, m[5], names, 3, &code))
			return false;
		result->octet_order = static_cast<vmf_bit_order>(code);
	}
	size_t capacity = c->source_count;

	if (c->limits->max_depth < capacity)
		capacity = c->limits->max_depth + 1;
	node_frame *frames = static_cast<node_frame *>(
		reserve(c, capacity, sizeof(*frames)));

	if (!frames)
		return false;
	message_source *source = &c->messages[c->schema_index];
	lowering l = {c, result, source, frames, 0, capacity, 0, 0, 0, 0, 0,
		false};

	source->value = value;
	if (!walk(&l, m[1]))
		return false;
	result->index_count = l.indexes;
	result->repeat_count = l.repeats;
	result->indexes = static_cast<vmf_index_schema *>(
		reserve(c, l.indexes, sizeof(vmf_index_schema)));
	result->repeats = static_cast<vmf_repeat_definition *>(
		reserve(c, l.repeats, sizeof(vmf_repeat_definition)));
	source->indexes = static_cast<yyjson_val **>(
		reserve(c, l.indexes, sizeof(yyjson_val *)));
	source->repeats = static_cast<yyjson_val **>(
		reserve(c, l.repeats, sizeof(yyjson_val *)));
	if ((l.indexes && (!result->indexes || !source->indexes)) ||
		(l.repeats && (!result->repeats || !source->repeats)))
		return false;
	l.indexes = 0;
	l.repeats = 0;
	l.groups = 0;
	l.repeat_ids = 0;
	l.emit = true;
	bool converted = walk(&l, m[1]) && resolve_references(c, result);

	memory_deallocate(&c->document->memory, frames);
	return converted;
}

static bool convert(conversion *c, yyjson_val *value)
{
	c->current = value;
	if (!yyjson_is_obj(value))
		return invalid(c, "expected object");
	yyjson_val *format = nullptr;
	yyjson_obj_iter properties = yyjson_obj_iter_with(value);
	yyjson_val *key;

	while ((key = yyjson_obj_iter_next(&properties)))
		if (string_equal(key, "format")) {
			if (format) {
				c->current = key;
				return invalid(c, "duplicate property");
			}
			format = yyjson_obj_iter_get_val(key);
		}

	if ((!format && yyjson_obj_get(value, "schemas")) ||
		(format && yyjson_is_str(format) &&
		 !string_equal(format, "vmf-schema/1")))
		return fail(c, VMF_UNSUPPORTED_SCHEMA_FEATURE,
			"unsupported schema format");
	static const char *const keys[] = {"format", "envelope", "messages",
		"message_standard", "standard_version"};
	static const char *const profiles[] = {"B", "C", "D", "D1"};
	yyjson_val *m[5];
	int profile = -1;
	size_t count;

	if (!object(c, value, keys, 5, 5, m) ||
		!string_equal(m[0], "vmf-schema/1") ||
		(m[1] && !enumeration(c, m[1], profiles, 4, &profile)) ||
		!array(c, m[2], &count) ||
		!standard(c, m[3], m[4], &c->document->schemas.standard))
		return invalid(c, "invalid schema document");
	if (count > c->limits->max_schemas)
		return fail(c, VMF_LIMIT_EXCEEDED,
			"schema count limit exceeded");
	vmf_message_schema *items = static_cast<vmf_message_schema *>(
		reserve(c, count, sizeof(*items)));

	c->messages = static_cast<message_source *>(
		reserve(c, count, sizeof(message_source)));
	if (count && (!items || !c->messages))
		return false;
	c->document->schemas.envelope = (vmf_envelope_version)(profile + 1);
	c->document->schemas.messages = items;
	c->document->schemas.message_count = count;
	yyjson_arr_iter iter = yyjson_arr_iter_with(m[2]);
	yyjson_val *item;
	size_t i = 0;

	while ((item = yyjson_arr_iter_next(&iter))) {
		c->schema_index = i;
		if (!message(c, item, &items[i++]))
			return false;
	}
	c->schema_index = SIZE_MAX;
	return true;
}

static size_t source_position(conversion *c, yyjson_val *value)
{
	for (size_t i = 0; i < c->source_count; ++i)
		if (c->sources[i].value == value)
			return i;
	return SIZE_MAX;
}

static yyjson_val *common_source(conversion *c, yyjson_val *a,
	yyjson_val *b)
{
	size_t first = source_position(c, a);
	size_t second = source_position(c, b);

	if (first == SIZE_MAX || second == SIZE_MAX)
		return nullptr;
	while (c->sources[first].depth > c->sources[second].depth)
		first = c->sources[first].parent;
	while (c->sources[second].depth > c->sources[first].depth)
		second = c->sources[second].parent;
	while (first != second) {
		first = c->sources[first].parent;
		second = c->sources[second].parent;
	}
	return c->sources[first].value;
}

/* Width is a precise source location only when the width itself violates an
 * intrinsic bound. Otherwise retain the whole field: native diagnostics do
 * not identify which of several encoding refinements caused the failure.
 */
static bool invalid_width(conversion *c, const vmf_index_schema *index)
{
	uint64_t width = index->number_of_bits;

	if (width == VMF_UNITS_UNKNOWN || width > c->limits->max_field_bits)
		return true;
	if (index->indicator)
		return width > 1;
	if (index->encode_type <= VMF_ENCODE_SCALED) {
		if (!width || width > 64)
			return true;
		if (index->encode_type == VMF_ENCODE_BOOL && width != 1)
			return true;
		return index->refinements &&
			index->refinements->representation > VMF_INTEGER_UNSIGNED &&
			width < 2;
	}
	bool literal = index->encode_type == VMF_ENCODE_LITERAL;
	bool variable = literal || (index->refinements &&
		index->refinements->length_from.count);
	unsigned unit = index->encode_type == VMF_ENCODE_BITS ? 1 : 8;

	if (index->encode_type == VMF_ENCODE_TEXT || literal) {
		vmf_text_encoding encoding = index->refinements ?
			index->refinements->text_encoding : VMF_TEXT_DEFAULT;

		if (encoding == VMF_TEXT_DEFAULT || encoding == VMF_TEXT_ASCII_7)
			unit = 7;
	}
	return (!variable && !width) || width % unit;
}

static yyjson_val *native_source(conversion *c,
	const vmf_error *error, size_t position, bool related)
{
	if (error->schema_index >= c->document->schemas.message_count)
		return c->current;
	const vmf_message_schema *message =
		&c->document->schemas.messages[error->schema_index];
	const message_source *sources = &c->messages[error->schema_index];

	if (!related && error->property == VMF_PROPERTY_REPEAT &&
		error->related_scope_id) {
		for (size_t i = 0; i < message->repeat_count; ++i)
			if (message->repeats[i].id == error->related_scope_id)
				return yyjson_obj_get(sources->repeats[i], "repeat");
	}
	if (!related && error->property == VMF_PROPERTY_DEPENDENCY) {
		yyjson_val *reference_source = nullptr;

		/* Core identifies the consuming native row, not a condition term.
		 * One candidate identifies the exact key. Several candidates keep
		 * their common source container rather than guessing a culprit.
		 */
		for (reference *entry = sources->references; entry;
			entry = entry->next) {
			if (entry->index_position != position)
				continue;
			reference_source = reference_source ?
				common_source(c, reference_source, entry->value) :
				entry->value;
		}
		if (reference_source)
			return reference_source;
	}
	if (position < message->index_count) {
		yyjson_val *value = sources->indexes[position];
		const char *property = nullptr;

		if (related || error->property == VMF_PROPERTY_KEY)
			property = "key";
		else if (error->property == VMF_PROPERTY_ENCODING &&
			invalid_width(c, &message->indexes[position]))
			property = "number_of_bits";
		yyjson_val *member = property ? yyjson_obj_get(value, property) :
			nullptr;

		return member ? member : value;
	}
	return sources->value;
}

static bool validate(conversion *c)
{
	vmf_allocator allocator = {
		memory_allocate, memory_deallocate, &c->document->memory};
	vmf_error error;
	vmf_status status = vmf_schema_set_validate(&c->document->schemas,
		c->limits, &allocator, &error);

	if (status == VMF_OK)
		return true;
	if (c->document->memory.failure != VMF_OK)
		status = c->document->memory.failure;
	c->schema_index = error.schema_index;
	c->index_position = error.index_position;
	c->property = error.property;
	c->current = native_source(c, &error, error.index_position, false);
	c->related = error.related_index_position == SIZE_MAX ? nullptr :
		native_source(c, &error, error.related_index_position, true);
	fail(c, status, vmf_status_string(status));
	if (c->error) {
		c->error->related_index_position = error.related_index_position;
		c->error->related_scope_id = error.related_scope_id;
	}
	return false;
}

static void release_sources(conversion *c)
{
	for (size_t i = 0; i < c->document->schemas.message_count; ++i) {
		memory_deallocate(&c->document->memory, c->messages[i].indexes);
		memory_deallocate(&c->document->memory, c->messages[i].repeats);
		reference *entry = c->messages[i].references;

		while (entry) {
			reference *next = entry->next;

			memory_deallocate(&c->document->memory, entry);
			entry = next;
		}
	}
	memory_deallocate(&c->document->memory, c->messages);
	memory_deallocate(&c->document->memory, c->sources);
}

} /* namespace */

vmf_schema_json_options vmf_schema_json_default_options(void)
{
	return {vmf_default_limits(), 16u * 1024 * 1024, 64u * 1024 * 1024,
		nullptr};
}

vmf_status vmf_schema_json_load(const char *data, size_t size,
	const vmf_schema_json_options *options,
	vmf_schema_json_document **result, vmf_schema_json_error *error)
{
	if (result)
		*result = nullptr;
	if (error)
		*error = diagnostic(VMF_OK);
	vmf_status status = VMF_OK;
	vmf_allocator allocator = {
		default_allocate, default_deallocate, nullptr};

	if (!data || !options || !result)
		status = VMF_INVALID_ARGUMENT;
	else if (options->allocator) {
		if (!!options->allocator->allocate !=
			!!options->allocator->deallocate)
			status = VMF_INVALID_ARGUMENT;
		else if (options->allocator->allocate)
			allocator = *options->allocator;
	}
	if (status == VMF_OK && size > options->max_json_bytes)
		status = VMF_LIMIT_EXCEEDED;
	if (status == VMF_OK && !size)
		status = VMF_TRUNCATED;
	if (status != VMF_OK) {
		if (error) {
			*error = diagnostic(status);
			if (status == VMF_TRUNCATED)
				error->byte_offset = 0;
		}
		return status;
	}
	memory_state memory = {allocator, nullptr, 0,
		options->max_memory_bytes, VMF_OK};
	void *storage =
		memory_allocate(&memory, sizeof(vmf_schema_json_document));

	if (!storage) {
		if (error)
			*error = diagnostic(memory.failure);
		return memory.failure;
	}
	vmf_schema_json_document *document =
		new (storage) vmf_schema_json_document{memory, {}};
	yyjson_alc json_allocator = {memory_allocate, memory_reallocate,
		memory_deallocate, &document->memory};
	yyjson_read_err read_error;
	yyjson_doc *json = yyjson_read_opts(const_cast<char *>(data), size,
		YYJSON_READ_BIGNUM_AS_RAW, &json_allocator, &read_error);

	if (!json) {
		status = document->memory.failure;
		if (status == VMF_OK) {
			yyjson_read_code code = read_error.code;

			status = code == YYJSON_READ_ERROR_UNEXPECTED_END ||
				 code == YYJSON_READ_ERROR_EMPTY_CONTENT
				? VMF_TRUNCATED : VMF_MALFORMED;
		}
		if (error) {
			*error = diagnostic(status);
			if (document->memory.failure == VMF_OK)
				error->byte_offset = read_error.pos;
		}
	} else {
		conversion c = {};

		c.document = document;
		c.limits = &options->limits;
		c.error = error;
		c.schema_index = SIZE_MAX;
		c.index_position = SIZE_MAX;
		c.current = yyjson_doc_get_root(json);
		bool converted = source_tree(&c, json) && convert(&c, c.current);

		if (converted)
			validate(&c);
		status = c.status;
		if (status == VMF_OK)
			release_sources(&c);
		yyjson_doc_free(json);
	}
	if (status != VMF_OK) {
		vmf_schema_json_destroy(document);
		return status;
	}
	*result = document;
	return VMF_OK;
}

const vmf_schema_set *vmf_schema_json_schemas(
	const vmf_schema_json_document *document)
{
	return document ? &document->schemas : nullptr;
}

void vmf_schema_json_destroy(vmf_schema_json_document *document)
{
	if (!document)
		return;
	vmf_allocator allocator = document->memory.allocator;
	allocation *block = document->memory.head;

	while (block) {
		allocation *next = block->next;

		allocator.deallocate(allocator.user, block);
		block = next;
	}
}
