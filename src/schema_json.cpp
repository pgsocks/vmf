#include <vmf/schema_json.h>
#include "schema_validation.hpp"
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

struct node_task {
	yyjson_val *source;
	vmf_node *target;
	size_t depth;
};

struct conversion {
	vmf_schema_json_document *document;
	const vmf_limits *limits;
	vmf_schema_json_error *error;
	vmf_status status;
	size_t schema_index;
	vmf_node_id node_id;
	node_task *tasks;
	size_t task_count;
	size_t node_count;
};

static bool fail(conversion *c, vmf_status status, const char *message)
{
	if (c->status == VMF_OK) {
		c->status = status;
		if (c->error)
			*c->error = {status, message, SIZE_MAX,
				c->schema_index, c->node_id};
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

static bool order(conversion *c, yyjson_val *value, vmf_bit_order *result)
{
	static const char *const names[] = {"msb_first", "lsb_first"};
	int code;

	if (!enumeration(c, value, names, 2, &code))
		return false;
	*result = (vmf_bit_order)code;
	return true;
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

static bool encoding(conversion *c, yyjson_val *value,
	vmf_integer_encoding *result)
{
	static const char *const keys[] = {
		"bit_width", "order", "representation"};
	static const char *const names[] = {"unsigned", "twos_complement",
		"ones_complement", "sign_magnitude"};
	yyjson_val *m[3];
	uint64_t width;
	int representation;

	if (!object(c, value, keys, 3, 7, m) ||
		!unsigned_integer(c, m[0], UINT8_MAX, &width) ||
		!order(c, m[1], &result->order) ||
		!enumeration(c, m[2], names, 4, &representation))
		return false;
	result->bit_width = (uint8_t)width;
	result->representation = (vmf_integer_representation)representation;
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

static bool labels(conversion *c, yyjson_val *value,
	const vmf_code_label **result, size_t *count)
{
	if (!array(c, value, count))
		return false;
	vmf_code_label *items = static_cast<vmf_code_label *>(
		reserve(c, *count, sizeof(*items)));

	if (*count && !items)
		return false;
	yyjson_arr_iter iter = yyjson_arr_iter_with(value);
	yyjson_val *item;
	size_t index = 0;
	static const char *const keys[] = {"code", "name"};

	while ((item = yyjson_arr_iter_next(&iter))) {
		yyjson_val *m[2];

		if (!object(c, item, keys, 2, 3, m) ||
			!unsigned_integer(c, m[0], UINT64_MAX,
				&items[index].code) ||
			!string(c, m[1], false, &items[index].name.data,
				&items[index].name.size))
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

	return object(c, value, keys, 3, 3, m) &&
	       rational(c, m[0], &result->factor) &&
	       rational(c, m[1], &result->bias) &&
	       (!m[2] || labels(c, m[2], &result->sentinels,
			 &result->sentinel_count));
}

static bool scalar(conversion *c, yyjson_val *value,
	vmf_scalar_definition *result, bool scaled)
{
	static const char *const keys[] = {
		"encoding", "allowed", "labels", "scale"};
	yyjson_val *m[4];

	if (!object(c, value, keys, scaled ? 4 : 3, scaled ? 9 : 1, m) ||
		!encoding(c, m[0], &result->encoding))
		return false;
	if (m[1] && !ranges(c, m[1], &result->allowed, &result->allowed_count))
		return false;
	return (!m[2] || labels(c, m[2], &result->labels,
			 &result->label_count)) &&
	       (!scaled || scale(c, m[3], &result->scale));
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
		uint64_t id;
		vmf_condition_term *term = &terms[index++];

		term->mask = UINT64_MAX;
		if (!object(c, item, term_keys, 4, 5, t) ||
			!unsigned_integer(c, t[0], UINT32_MAX, &id) ||
			(t[1] && !unsigned_integer(c, t[1], UINT64_MAX,
					 &term->mask)) ||
			!ranges(c, t[2], &term->values, &term->value_count) ||
			(t[3] && !boolean(c, t[3], &term->negate)))
			return false;
		term->field = (vmf_node_id)id;
	}
	return true;
}

static bool presence(conversion *c, yyjson_val *value,
	vmf_presence_definition *result)
{
	static const char *const keys[] = {"kind", "present_bit", "condition"};
	static const char *const names[] = {"always", "bit", "condition"};
	yyjson_val *m[3];
	int kind;

	if (!object(c, value, keys, 3, 1, m) ||
		!enumeration(c, m[0], names, 3, &kind))
		return false;
	result->kind = (vmf_presence_kind)kind;
	if ((kind != VMF_PRESENCE_BIT && m[1]) ||
		(kind != VMF_PRESENCE_CONDITION && m[2]))
		return invalid(c, "inactive presence property");
	if (kind == VMF_PRESENCE_BIT)
		return boolean(c, m[1], &result->present_bit);
	if (kind == VMF_PRESENCE_CONDITION)
		return condition(c, m[2], &result->condition);
	return true;
}

static bool extent(conversion *c, yyjson_val *value,
	vmf_extent_definition *result)
{
	static const char *const keys[] = {"kind", "minimum", "maximum",
		"fixed", "prefix", "field", "bias", "more_bit"};
	static const char *const names[] = {"fixed", "prefix", "field",
		"remaining", "more_before", "more_after", "recurrence_before",
		"literal"};
	yyjson_val *m[8];
	int kind;

	if (!object(c, value, keys, 8, 7, m) ||
		!enumeration(c, m[0], names, 8, &kind) ||
		!unsigned_integer(c, m[1], UINT64_MAX, &result->minimum) ||
		!unsigned_integer(c, m[2], UINT64_MAX, &result->maximum))
		return false;
	result->kind = (vmf_extent_kind)kind;
	unsigned allowed = 7;

	switch (result->kind) {
	case VMF_EXTENT_FIXED:
		allowed |= 1u << 3;
		break;
	case VMF_EXTENT_PREFIX:
		allowed |= (1u << 4) | (1u << 6);
		break;
	case VMF_EXTENT_FIELD:
		allowed |= (1u << 5) | (1u << 6);
		break;
	case VMF_EXTENT_MORE_BEFORE:
	case VMF_EXTENT_MORE_AFTER:
	case VMF_EXTENT_RECURRENCE_BEFORE:
		allowed |= 1u << 7;
		break;
	default:
		break;
	}
	for (size_t i = 3; i < 8; ++i)
		if (m[i] && !(allowed & (1u << i)))
			return invalid(c, "inactive extent property");
	if (result->kind == VMF_EXTENT_FIXED)
		return unsigned_integer(c, m[3], UINT64_MAX, &result->fixed);
	if (result->kind == VMF_EXTENT_PREFIX &&
		!encoding(c, m[4], &result->prefix))
		return false;
	if (result->kind == VMF_EXTENT_FIELD) {
		uint64_t id;

		if (!unsigned_integer(c, m[5], UINT32_MAX, &id))
			return false;
		result->field = (vmf_node_id)id;
	}
	if (m[6] && !signed_integer(c, m[6], &result->bias))
		return false;
	if (allowed & (1u << 7))
		return boolean(c, m[7], &result->more_bit);
	return true;
}

static bool field(conversion *c, yyjson_val *value, vmf_field_schema *result)
{
	static const char *const keys[] = {
		"kind", "scalar", "text", "octets", "bits"};
	static const char *const names[] = {"bool", "unsigned", "signed",
		"enum", "scaled", "text", "octets", "bits"};
	yyjson_val *m[5];
	int kind;

	if (!object(c, value, keys, 5, 1, m) ||
		!enumeration(c, m[0], names, 8, &kind))
		return false;
	result->kind = (vmf_field_kind)kind;
	size_t active = kind <= VMF_FIELD_SCALED ? 1 : (size_t)kind - 3;

	for (size_t i = 1; i < 5; ++i)
		if (i != active && m[i])
			return invalid(c, "inactive field property");
	if (kind <= VMF_FIELD_SCALED)
		return scalar(c, m[1], &result->as.scalar,
			kind == VMF_FIELD_SCALED);
	yyjson_val *body = m[active];
	yyjson_val *f[3];

	if (kind == VMF_FIELD_TEXT) {
		static const char *const text_keys[] = {
			"encoding", "order", "length"};
		static const char *const text_names[] = {
			"ascii_7", "ascii_8", "latin_1", "utf_8"};
		int text_encoding;

		if (!object(c, body, text_keys, 3, 7, f) ||
			!enumeration(c, f[0], text_names, 4, &text_encoding) ||
			!order(c, f[1], &result->as.text.order) ||
			!extent(c, f[2], &result->as.text.length))
			return false;
		result->as.text.encoding = (vmf_text_encoding)text_encoding;
		return true;
	}
	if (kind == VMF_FIELD_OCTETS) {
		static const char *const octets_keys[] = {"order", "length"};

		return object(c, body, octets_keys, 2, 3, f) &&
		       order(c, f[0], &result->as.octets.order) &&
		       extent(c, f[1], &result->as.octets.length);
	}
	static const char *const bits_keys[] = {"length"};

	return object(c, body, bits_keys, 1, 1, f) &&
	       extent(c, f[0], &result->as.bits.length);
}

static vmf_node *nodes(conversion *c, size_t count)
{
	if (count > c->limits->max_schema_nodes - c->node_count) {
		fail(c, VMF_LIMIT_EXCEEDED, "schema node limit exceeded");
		return nullptr;
	}
	vmf_node *items = static_cast<vmf_node *>(
		reserve(c, count, sizeof(*items)));

	if (items)
		c->node_count += count;
	return items;
}

static bool schedule(conversion *c, yyjson_val *source, vmf_node *target,
	size_t parent_depth)
{
	if (parent_depth >= c->limits->max_depth)
		return fail(c, VMF_LIMIT_EXCEEDED,
			"schema depth limit exceeded");
	c->tasks[c->task_count++] = {source, target, parent_depth + 1};
	return true;
}

static bool group(conversion *c, yyjson_val *value, vmf_group_schema *result,
	size_t depth)
{
	static const char *const keys[] = {"children"};
	yyjson_val *m[1];

	if (!object(c, value, keys, 1, 1, m) ||
		!array(c, m[0], &result->child_count))
		return false;
	vmf_node *children = nodes(c, result->child_count);

	if (result->child_count && !children)
		return false;
	result->children = children;
	yyjson_arr_iter iter = yyjson_arr_iter_with(m[0]);
	yyjson_val *child;
	size_t start = c->task_count;
	size_t i = 0;

	while ((child = yyjson_arr_iter_next(&iter)))
		if (!schedule(c, child, &children[i++], depth))
			return false;
	/* Reverse pending siblings so conversion follows logical wire order. */
	for (size_t a = start, b = c->task_count; a < b && a < --b; ++a) {
		node_task temporary = c->tasks[a];

		c->tasks[a] = c->tasks[b];
		c->tasks[b] = temporary;
	}
	return true;
}

static bool repeat(conversion *c, yyjson_val *value, vmf_repeat_schema *result,
	size_t depth)
{
	static const char *const keys[] = {"count", "body"};
	yyjson_val *m[2];

	if (!object(c, value, keys, 2, 3, m) ||
		!extent(c, m[0], &result->count))
		return false;
	vmf_node *body = nodes(c, 1);

	if (!body)
		return false;
	result->body = body;
	return schedule(c, m[1], body, depth);
}

static bool choice(conversion *c, yyjson_val *value, vmf_choice_schema *result,
	size_t depth)
{
	static const char *const keys[] = {
		"selector", "mask", "cases", "otherwise"};
	yyjson_val *m[4];
	uint64_t selector;

	result->mask = UINT64_MAX;
	if (!object(c, value, keys, 4, 5, m) ||
		!unsigned_integer(c, m[0], UINT32_MAX, &selector) ||
		(m[1] && !unsigned_integer(c, m[1], UINT64_MAX,
				 &result->mask)) ||
		!array(c, m[2], &result->case_count))
		return false;
	result->selector = (vmf_node_id)selector;
	vmf_choice_case *cases = static_cast<vmf_choice_case *>(
		reserve(c, result->case_count, sizeof(*cases)));

	if (result->case_count && !cases)
		return false;
	result->cases = cases;
	if (m[3]) {
		vmf_node *body = nodes(c, 1);

		if (!body)
			return false;
		result->otherwise = body;
		if (!schedule(c, m[3], body, depth))
			return false;
	}
	yyjson_arr_iter iter = yyjson_arr_iter_with(m[2]);
	yyjson_val *item;
	size_t index = 0;
	static const char *const case_keys[] = {"values", "body"};

	while ((item = yyjson_arr_iter_next(&iter))) {
		yyjson_val *arm[2];
		vmf_choice_case *entry = &cases[index++];

		if (!object(c, item, case_keys, 2, 3, arm) ||
			!ranges(c, arm[0], &entry->values, &entry->value_count))
			return false;
		vmf_node *body = nodes(c, 1);

		if (!body)
			return false;
		entry->body = body;
		if (!schedule(c, arm[1], body, depth))
			return false;
	}
	return true;
}

static bool node(conversion *c, const node_task *task)
{
	static const char *const keys[] = {"id", "name", "presence", "kind",
		"field", "group", "repeat", "choice"};
	static const char *const names[] = {
		"field", "group", "repeat", "choice"};
	yyjson_val *m[8];
	uint64_t id;
	int kind;
	vmf_node *result = task->target;

	c->node_id = 0;
	if (!object(c, task->source, keys, 8, 9, m) ||
		!unsigned_integer(c, m[0], UINT32_MAX, &id))
		return false;
	result->id = (vmf_node_id)id;
	c->node_id = result->id;
	if (!enumeration(c, m[3], names, 4, &kind) ||
		(m[1] && !string(c, m[1], true, &result->name, nullptr)) ||
		(m[2] && !presence(c, m[2], &result->presence)))
		return false;
	result->kind = (vmf_node_kind)kind;
	for (size_t i = 4; i < 8; ++i)
		if (i != 4u + kind && m[i])
			return invalid(c, "inactive node property");
	switch (result->kind) {
	case VMF_NODE_FIELD:
		return field(c, m[4], &result->as.field);
	case VMF_NODE_GROUP:
		return group(c, m[5], &result->as.group, task->depth);
	case VMF_NODE_REPEAT:
		return repeat(c, m[6], &result->as.repeat, task->depth);
	case VMF_NODE_CHOICE:
		return choice(c, m[7], &result->as.choice, task->depth);
	default:
		return invalid(c, "unknown node kind");
	}
}

static bool identity(conversion *c, yyjson_val *value, vmf_message_id *result)
{
	static const char *const keys[] = {
		"user_message_format", "standard_version", "identification"};
	yyjson_val *m[3];
	uint64_t number;

	if (!object(c, value, keys, 3, 1, m) ||
		!unsigned_integer(c, m[0], UINT8_MAX, &number))
		return false;
	result->user_message_format = (uint8_t)number;
	if (m[1]) {
		if (!unsigned_integer(c, m[1], UINT8_MAX, &number))
			return false;
		result->standard_version = {true, (uint8_t)number};
	}
	if (m[2]) {
		static const char *const id_keys[] = {
			"functional_area", "message_number", "subtype"};
		yyjson_val *id[3];

		if (!object(c, m[2], id_keys, 3, 3, id) ||
			!unsigned_integer(c, id[0], UINT8_MAX, &number))
			return false;
		result->identification_present = true;
		result->functional_area = (uint8_t)number;
		if (!unsigned_integer(c, id[1], UINT8_MAX, &number))
			return false;
		result->message_number = (uint8_t)number;
		if (id[2]) {
			if (!unsigned_integer(c, id[2], UINT8_MAX, &number))
				return false;
			result->subtype = {true, (uint8_t)number};
		}
	}
	return true;
}

static bool schema(conversion *c, yyjson_val *value,
	vmf_message_schema *result)
{
	static const char *const keys[] = {
		"id", "name", "root", "octet_order", "padding_bit"};
	yyjson_val *m[5];
	uint64_t padding;

	if (!object(c, value, keys, 5, 13, m) ||
		!identity(c, m[0], &result->id) ||
		!order(c, m[3], &result->octet_order) ||
		(m[1] && !string(c, m[1], true, &result->name, nullptr)))
		return false;
	if (m[4]) {
		if (!unsigned_integer(c, m[4], UINT8_MAX, &padding))
			return false;
		result->padding_bit = (uint8_t)padding;
	}
	c->node_count = 0;
	c->task_count = 0;
	vmf_node *root = nodes(c, 1);

	if (!root)
		return false;
	result->root = root;
	c->tasks = static_cast<node_task *>(reserve(c,
		c->limits->max_schema_nodes, sizeof(*c->tasks)));
	if (!c->tasks)
		return false;
	c->tasks[c->task_count++] = {m[2], root, 0};
	while (c->task_count) {
		node_task task = c->tasks[--c->task_count];

		if (!node(c, &task))
			return false;
	}
	memory_deallocate(&c->document->memory, c->tasks);
	c->tasks = nullptr;
	c->node_id = 0;
	return true;
}

static bool convert(conversion *c, yyjson_val *value)
{
	static const char *const keys[] = {"schemas"};
	yyjson_val *m[1];
	size_t count;

	if (!object(c, value, keys, 1, 1, m) || !array(c, m[0], &count))
		return false;
	if (count > c->limits->max_schemas)
		return fail(c, VMF_LIMIT_EXCEEDED,
			"schema count limit exceeded");
	vmf_message_schema *schemas = static_cast<vmf_message_schema *>(
		reserve(c, count, sizeof(*schemas)));
	if (count && !schemas)
		return false;
	const vmf_message_schema **entries =
		static_cast<const vmf_message_schema **>(
			reserve(c, count, sizeof(*entries)));
	if (count && !entries)
		return false;
	c->document->schemas = {entries, count};
	yyjson_arr_iter iter = yyjson_arr_iter_with(m[0]);
	yyjson_val *item;
	size_t i = 0;

	while ((item = yyjson_arr_iter_next(&iter))) {
		c->schema_index = i;
		if (!schema(c, item, &schemas[i]))
			return false;
		entries[i] = &schemas[i];
		++i;
	}
	return true;
}

static bool validate(conversion *c)
{
	const vmf_schema_set *schemas = &c->document->schemas;
	vmf_allocator allocator = {
		memory_allocate, memory_deallocate, &c->document->memory};

	for (size_t i = 0; i < schemas->count; ++i) {
		c->schema_index = i;
		vmf_schema_set single = {&schemas->schemas[i], 1};
		vmf_error error;
		vmf_status status = vmf_internal::validate_schemas_allocated(
			&single, c->limits, &allocator, &error);

		if (status != VMF_OK) {
			c->node_id = error.node_id;
			if (c->document->memory.failure != VMF_OK)
				status = c->document->memory.failure;
			return fail(c, status, vmf_status_string(status));
		}
		for (size_t j = 0; j < i; ++j)
			if (vmf_message_id_equal(&schemas->schemas[i]->id,
				    &schemas->schemas[j]->id))
				return fail(c, VMF_DUPLICATE_MESSAGE,
					"duplicate message identity");
	}
	return true;
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
		*error = {VMF_OK, "ok", SIZE_MAX, SIZE_MAX, 0};
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
			error->status = status;
			error->message = vmf_status_string(status);
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
		if (error) {
			error->status = memory.failure;
			error->message = vmf_status_string(memory.failure);
		}
		return memory.failure;
	}
	vmf_schema_json_document *document =
		new (storage) vmf_schema_json_document{memory, {nullptr, 0}};
	yyjson_alc json_allocator = {memory_allocate, memory_reallocate,
		memory_deallocate, &document->memory};
	yyjson_read_err read_error;
	yyjson_doc *json = yyjson_read_opts(const_cast<char *>(data), size,
		YYJSON_READ_BIGNUM_AS_RAW, &json_allocator, &read_error);

	if (!json) {
		status = document->memory.failure;
		if (status == VMF_OK) {
			yyjson_read_code code = read_error.code;

			if (code == YYJSON_READ_ERROR_UNEXPECTED_END ||
				code == YYJSON_READ_ERROR_EMPTY_CONTENT)
				status = VMF_TRUNCATED;
			else
				status = VMF_MALFORMED;
		}
		if (error)
			*error = {status, vmf_status_string(status),
				document->memory.failure == VMF_OK
					? read_error.pos : SIZE_MAX,
				SIZE_MAX, 0};
	} else {
		conversion c = {document, &options->limits, error, VMF_OK,
			SIZE_MAX, 0, nullptr, 0, 0};
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

	/* The owner was allocated first and is the last block released. */
	while (block) {
		allocation *next = block->next;

		allocator.deallocate(allocator.user, block);
		block = next;
	}
}
