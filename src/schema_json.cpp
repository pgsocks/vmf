#include <vmf/schema_json.h>
#include <yyjson.h>
#include <cstddef>
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

struct conversion {
	vmf_schema_json_document *document;
	const vmf_limits *limits;
	vmf_schema_json_error *error;
	vmf_status status;
	size_t schema_index;
	size_t index_position;
	vmf_schema_property property;
};

static vmf_schema_json_error diagnostic(vmf_status status)
{
	return {status, vmf_status_string(status), SIZE_MAX, SIZE_MAX,
		SIZE_MAX, VMF_PROPERTY_NONE, SIZE_MAX, 0};
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
		if (i == count)
			return invalid(c, "unknown or inactive property");
		uint64_t bit = UINT64_C(1) << i;

		if (seen & bit)
			return invalid(c, "duplicate property");
		seen |= bit;
		members[i] = yyjson_obj_iter_get_val(key);
	}
	return (seen & required) == required ||
	       invalid(c, "missing required property");
}

static bool array(conversion *c, yyjson_val *value, size_t *count)
{
	if (!yyjson_is_arr(value))
		return invalid(c, "expected array");
	*count = yyjson_arr_size(value);
	return true;
}

static bool unsigned_integer(conversion *c, yyjson_val *value,
	uint64_t maximum, uint64_t *result)
{
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
	if (!yyjson_is_bool(value))
		return invalid(c, "expected boolean");
	*result = yyjson_get_bool(value);
	return true;
}

static bool enumeration(conversion *c, yyjson_val *value,
	const char *const *names, size_t count, int *result)
{
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
	if (!yyjson_is_str(value))
		return invalid(c, "expected notation string");
	*text = yyjson_get_str(value);
	*size = yyjson_get_len(value);
	return (*size && !std::memchr(*text, 0, *size)) ||
	       invalid(c, "empty or NUL-containing notation");
}

static bool index_number(conversion *c, yyjson_val *value,
	vmf_index_number *result)
{
	const char *text;
	size_t size;

	if (!notation(c, value, &text, &size))
		return false;
	size_t count = 1;

	for (size_t i = 0; i + 1 < size; ++i)
		if (text[i] == '.')
			++count;
	uint32_t *parts = static_cast<uint32_t *>(
		reserve(c, count, sizeof(*parts)));

	if (!parts)
		return false;
	size_t position = 0;

	for (size_t i = 0; i < count; ++i) {
		uint64_t component;

		if (!decimal(c, text, size, &position, UINT32_MAX, &component))
			return false;
		parts[i] = (uint32_t)component;
		if (position < size && text[position++] != '.')
			return invalid(c, "invalid index separator");
	}
	if (position != size)
		return invalid(c, "invalid index notation");
	*result = {parts, count};
	return true;
}

static bool scope_component(conversion *c, const char *text, size_t size,
	size_t *position, char prefix, uint32_t *result)
{
	uint64_t id;

	if (*position == size || text[(*position)++] != prefix ||
		!decimal(c, text, size, position, UINT32_MAX, &id))
		return invalid(c, "invalid scope identifier");
	*result = (uint32_t)id;
	return true;
}

static bool group_path(conversion *c, yyjson_val *value,
	vmf_group_path *result)
{
	const char *text;
	size_t size;

	if (!notation(c, value, &text, &size))
		return false;
	size_t count = 1;

	for (size_t i = 0; i < size; ++i)
		if (text[i] == '/')
			++count;
	vmf_group_id *parts = static_cast<vmf_group_id *>(
		reserve(c, count, sizeof(*parts)));

	if (!parts)
		return false;
	size_t position = 0;

	for (size_t i = 0; i < count; ++i) {
		if (!scope_component(c, text, size, &position, 'G', &parts[i]))
			return false;
		if (i + 1 < count &&
			(position == size || text[position++] != '/'))
			return invalid(c, "invalid group path");
	}
	if (position != size)
		return invalid(c, "invalid group path");
	*result = {parts, count};
	return true;
}

static bool repeat_path(conversion *c, yyjson_val *value,
	vmf_repeat_path *result)
{
	const char *text;
	size_t size;

	if (!notation(c, value, &text, &size))
		return false;
	size_t count = 1;

	for (size_t i = 0; i < size; ++i)
		if (text[i] == '/')
			++count;
	vmf_repeat_component *parts = static_cast<vmf_repeat_component *>(
		reserve(c, count, sizeof(*parts)));

	if (!parts)
		return false;
	size_t position = 0;

	for (size_t i = 0; i < count; ++i) {
		if (!scope_component(c, text, size, &position, 'R',
			&parts[i].id))
			return false;
		if (position < size && text[position] == '(') {
			++position;
			parts[i].maximum.present = true;
			if (!decimal(c, text, size, &position, UINT64_MAX,
				&parts[i].maximum.value) || position == size ||
				text[position++] != ')')
				return invalid(c, "invalid repeat bound");
		}
		if (i + 1 < count &&
			(position == size || text[position++] != '/'))
			return invalid(c, "invalid repeat path");
	}
	if (position != size)
		return invalid(c, "invalid repeat path");
	*result = {parts, count};
	return true;
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

static bool condition(conversion *c, yyjson_val *value, vmf_condition *result)
{
	static const char *const keys[] = {"mode", "terms", "negate"};
	static const char *const names[] = {"all", "any"};
	yyjson_val *m[3];
	int mode;

	if (!object(c, value, keys, 3, 3, m) ||
		!enumeration(c, m[0], names, 2, &mode) ||
		!array(c, m[1], &result->term_count) ||
		(m[2] && !boolean(c, m[2], &result->negate)))
		return false;
	result->mode = (vmf_condition_mode)mode;
	vmf_condition_term *terms = static_cast<vmf_condition_term *>(
		reserve(c, result->term_count, sizeof(*terms)));

	if (result->term_count && !terms)
		return false;
	result->terms = terms;
	yyjson_arr_iter iter = yyjson_arr_iter_with(m[1]);
	yyjson_val *item;
	size_t index = 0;
	static const char *const term_keys[] = {
		"field", "mask", "values", "negate"};

	while ((item = yyjson_arr_iter_next(&iter))) {
		yyjson_val *t[4];
		vmf_condition_term *term = &terms[index++];

		if (!object(c, item, term_keys, 4, 5, t) ||
			!index_number(c, t[0], &term->field) ||
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

static bool target(conversion *c, yyjson_val *value,
	vmf_control_target *result)
{
	const char *text;
	size_t size;

	if (!notation(c, value, &text, &size))
		return false;
	if (text[0] != 'G') {
		result->kind = VMF_TARGET_INDEX;
		return index_number(c, value, &result->as.index);
	}
	size_t position = 0;

	result->kind = VMF_TARGET_GROUP;
	return scope_component(c, text, size, &position, 'G',
		&result->as.group) &&
	       (position == size || invalid(c, "invalid group target"));
}

static bool refinements(conversion *c, yyjson_val **m,
	vmf_index_schema *result)
{
	bool present = false;

	for (size_t i = 7; i < 16; ++i)
		present = present || m[i];
	if (!present)
		return true;
	vmf_index_refinements *r = static_cast<vmf_index_refinements *>(
		reserve(c, 1, sizeof(*r)));

	if (!r)
		return false;
	result->refinements = r;
	int code;
	static const char *const representations[] = {"default", "unsigned",
		"twos_complement", "ones_complement", "sign_magnitude"};
	static const char *const orders[] = {"default", "msb_first",
		"lsb_first"};
	static const char *const encodings[] = {"default", "ascii_7", "ascii_8",
		"latin_1", "utf_8"};

	if (m[7]) {
		if (!enumeration(c, m[7], representations, 5, &code))
			return false;
		r->representation = (vmf_integer_representation)code;
	}
	if (m[8]) {
		if (!enumeration(c, m[8], orders, 3, &code))
			return false;
		r->bit_order = (vmf_bit_order)code;
	}
	if (m[9]) {
		if (!enumeration(c, m[9], encodings, 5, &code))
			return false;
		r->text_encoding = (vmf_text_encoding)code;
	}
	if (m[10] && !index_number(c, m[10], &r->length_from))
		return false;
	if (m[11]) {
		r->length_bias.present = true;
		if (!signed_integer(c, m[11], &r->length_bias.value))
			return false;
	}
	if (m[12]) {
		vmf_condition *when = static_cast<vmf_condition *>(
			reserve(c, 1, sizeof(*when)));

		if (!when || !condition(c, m[12], when))
			return false;
		r->when = when;
	}
	if (m[13] && !ranges(c, m[13], &r->allowed, &r->allowed_count))
		return false;
	if (m[14]) {
		r->constant.present = true;
		if (!unsigned_integer(c, m[14], UINT64_MAX, &r->constant.value))
			return false;
	}
	if (m[15]) {
		vmf_scale_definition *value =
			static_cast<vmf_scale_definition *>(
				reserve(c, 1, sizeof(*value)));

		if (!value || !scale(c, m[15], value))
			return false;
		r->scale = value;
	}
	return true;
}

static bool index(conversion *c, yyjson_val *value, vmf_index_schema *result)
{
	static const char *const keys[] = {"index_number", "number_of_bits",
		"group_code", "repeat_code", "encode_type", "indicator",
		"target",
		"representation", "bit_order", "text_encoding", "length_from",
		"length_bias", "when", "allowed", "constant", "scale", "key"};
	static const char *const types[] = {"default", "bool", "unsigned",
		"signed", "enum", "scaled", "bits", "octets", "text",
		"literal"};
	static const char *const indicators[] = {"none", "fpi", "gpi", "fri",
		"gri"};
	yyjson_val *m[17];
	int code;

	if (!object(c, value, keys, 17, 1, m) ||
		!index_number(c, m[0], &result->index_number) ||
		(m[1] && !unsigned_integer(c, m[1], UINT64_MAX,
			&result->number_of_bits)) ||
		(m[2] && !group_path(c, m[2], &result->group_code)) ||
		(m[3] && !repeat_path(c, m[3], &result->repeat_code)))
		return false;
	if (m[4]) {
		if (!enumeration(c, m[4], types, 10, &code))
			return false;
		result->encode_type = (vmf_encode_type)code;
	}
	if (m[5]) {
		if (!enumeration(c, m[5], indicators, 5, &code))
			return false;
		result->indicator = (vmf_indicator_role)code;
	}
	return (!m[6] || target(c, m[6], &result->target)) &&
	       refinements(c, m, result) &&
	       (!m[16] || string(c, m[16], false,
			 &result->key.data, &result->key.size));
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

static bool repeats(conversion *c, yyjson_val *value,
	vmf_message_schema *result)
{
	if (!array(c, value, &result->repeat_count))
		return false;
	vmf_repeat_definition *items = static_cast<vmf_repeat_definition *>(
		reserve(c, result->repeat_count, sizeof(*items)));

	if (result->repeat_count && !items)
		return false;
	result->repeats = items;
	yyjson_arr_iter iter = yyjson_arr_iter_with(value);
	yyjson_val *item;
	size_t i = 0;
	static const char *const keys[] = {"repeat_code", "count", "count_from",
		"maximum", "minimum", "bias"};

	while ((item = yyjson_arr_iter_next(&iter))) {
		yyjson_val *m[6];
		vmf_repeat_definition *r = &items[i++];
		const char *text;
		size_t size, position = 0;

		if (!object(c, item, keys, 6, 1, m) ||
			!notation(c, m[0], &text, &size) ||
			!scope_component(c, text, size, &position, 'R', &r->id))
			return false;
		if (position != size)
			return invalid(c,
				"repeat declaration requires one identifier");
		if (!!m[1] == !!m[2])
			return invalid(c,
				"repeat requires one count mechanism");
		if (m[1]) {
			if (m[3] || m[4] || m[5])
				return invalid(c,
					"inactive fixed repeat property");
			if (!unsigned_integer(c, m[1], UINT64_MAX, &r->count))
				return false;
		} else {
			r->kind = VMF_REPEAT_FIELD;
			if (!index_number(c, m[2], &r->count_from) ||
				!unsigned_integer(c, m[3], UINT64_MAX,
					&r->maximum))
				return false;
			if (m[4]) {
				r->minimum.present = true;
				if (!unsigned_integer(c, m[4], UINT64_MAX,
					&r->minimum.value))
					return false;
			}
			if (m[5]) {
				r->bias.present = true;
				if (!signed_integer(c, m[5], &r->bias.value))
					return false;
			}
		}
	}
	return true;
}

static bool message(conversion *c, yyjson_val *value,
	vmf_message_schema *result)
{
	static const char *const keys[] = {"message_number", "indexes",
		"message_standard", "standard_version", "subtype", "repeats",
		"octet_order"};
	yyjson_val *m[7];
	const char *text;
	size_t size, position = 1;
	uint64_t area, number;

	if (!object(c, value, keys, 7, 3, m) ||
		!notation(c, m[0], &text, &size))
		return false;
	if (text[0] != 'K' ||
		!decimal(c, text, size, &position, UINT8_MAX, &area) ||
		position == size || text[position++] != '.' ||
		!decimal(c, text, size, &position, UINT8_MAX, &number) ||
		position != size)
		return invalid(c, "invalid K-series message number");
	result->message_number = {(uint8_t)area, (uint8_t)number};
	if (!standard(c, m[2], m[3], &result->standard) ||
		!array(c, m[1], &result->index_count))
		return false;
	if (result->index_count > c->limits->max_schema_nodes)
		return fail(c, VMF_LIMIT_EXCEEDED,
			"schema index limit exceeded");
	if (m[4]) {
		uint64_t subtype;

		if (!unsigned_integer(c, m[4], UINT8_MAX, &subtype))
			return false;
		result->subtype = {true, (uint8_t)subtype};
	}
	if (m[5] && !repeats(c, m[5], result))
		return false;
	if (m[6]) {
		static const char *const names[] = {"default", "msb_first",
			"lsb_first"};
		int code;

		if (!enumeration(c, m[6], names, 3, &code))
			return false;
		result->octet_order = (vmf_bit_order)code;
	}
	vmf_index_schema *items = static_cast<vmf_index_schema *>(
		reserve(c, result->index_count, sizeof(*items)));

	if (result->index_count && !items)
		return false;
	result->indexes = items;
	yyjson_arr_iter iter = yyjson_arr_iter_with(m[1]);
	yyjson_val *item;
	size_t i = 0;

	while ((item = yyjson_arr_iter_next(&iter))) {
		c->index_position = i;
		if (!index(c, item, &items[i++]))
			return false;
	}
	c->index_position = SIZE_MAX;
	return true;
}

static bool convert(conversion *c, yyjson_val *value)
{
	if (!yyjson_is_obj(value))
		return invalid(c, "expected object");
	yyjson_val *format = nullptr;
	yyjson_obj_iter properties = yyjson_obj_iter_with(value);
	yyjson_val *key;

	while ((key = yyjson_obj_iter_next(&properties)))
		if (string_equal(key, "format")) {
			if (format)
				return invalid(c, "duplicate property");
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
	int profile;
	size_t count;

	if (!object(c, value, keys, 5, 7, m) ||
		!string_equal(m[0], "vmf-schema/1") ||
		!enumeration(c, m[1], profiles, 4, &profile) ||
		!array(c, m[2], &count) ||
		!standard(c, m[3], m[4], &c->document->schemas.standard))
		return invalid(c, "invalid schema document");
	if (count > c->limits->max_schemas)
		return fail(c, VMF_LIMIT_EXCEEDED,
			"schema count limit exceeded");
	vmf_message_schema *items = static_cast<vmf_message_schema *>(
		reserve(c, count, sizeof(*items)));

	if (count && !items)
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
	fail(c, status, vmf_status_string(status));
	if (c->error) {
		c->error->related_index_position = error.related_index_position;
		c->error->related_scope_id = error.related_scope_id;
	}
	return false;
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
		conversion c = {document, &options->limits, error, VMF_OK,
			SIZE_MAX, SIZE_MAX, VMF_PROPERTY_NONE};
		bool converted = convert(&c, yyjson_doc_get_root(json));

		yyjson_doc_free(json);
		if (converted)
			validate(&c);
		status = c.status;
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
