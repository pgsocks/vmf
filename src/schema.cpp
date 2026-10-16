#include "schema_validation.hpp"
#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace
{
static vmf_error clean_error(vmf_status status)
{
	return {status, 0, SIZE_MAX, SIZE_MAX, SIZE_MAX,
		VMF_PROPERTY_NONE, SIZE_MAX, 0};
}

static bool number_equal(vmf_index_number a, vmf_index_number b)
{
	return a.count == b.count && (!a.count || (a.parts && b.parts &&
		!std::memcmp(a.parts, b.parts, a.count * sizeof(*a.parts))));
}

static bool number_prefix(vmf_index_number a, vmf_index_number b)
{
	return a.count && a.count < b.count && a.parts && b.parts &&
		!std::memcmp(a.parts, b.parts, a.count * sizeof(*a.parts));
}

static bool number_valid(vmf_index_number number)
{
	return number.count && number.parts;
}

static bool string_equal(vmf_string a, vmf_string b)
{
	return a.size == b.size && (!a.size || (a.data && b.data &&
		!std::memcmp(a.data, b.data, a.size)));
}

static vmf_bit_order resolved_order(vmf_bit_order order)
{
	return order == VMF_BIT_DEFAULT ? VMF_BIT_LSB_FIRST : order;
}

static bool order_valid(vmf_bit_order order)
{
	return order >= VMF_BIT_DEFAULT && order <= VMF_BIT_LSB_FIRST;
}

static bool member(const vmf_schema_set *set,
	const vmf_message_schema *message)
{
	if (!set || !message || (set->message_count && !set->messages))
		return false;
	for (size_t i = 0; i < set->message_count; ++i)
		if (&set->messages[i] == message)
			return true;
	return false;
}

static bool ranges_valid(const vmf_code_range *ranges, size_t count,
	uint64_t maximum)
{
	if (count && !ranges)
		return false;
	for (size_t i = 0; i < count; ++i) {
		if (ranges[i].first > ranges[i].last ||
			ranges[i].last > maximum)
			return false;
		for (size_t j = 0; j < i; ++j)
			if (ranges[i].first <= ranges[j].last &&
				ranges[j].first <= ranges[i].last)
				return false;
	}
	return true;
}

static bool utf8_valid(vmf_string text)
{
	if (text.size && !text.data)
		return false;
	for (size_t i = 0; i < text.size;) {
		uint8_t c = (uint8_t)text.data[i++];
		size_t count;
		if (c < 0x80)
			continue;
		if (c >= 0xc2 && c <= 0xdf)
			count = 1;
		else if (c >= 0xe0 && c <= 0xef)
			count = 2;
		else if (c >= 0xf0 && c <= 0xf4)
			count = 3;
		else
			return false;
		if (count > text.size - i)
			return false;
		uint8_t next = (uint8_t)text.data[i];
		if ((c == 0xe0 && next < 0xa0) ||
			(c == 0xed && next > 0x9f) ||
			(c == 0xf0 && next < 0x90) ||
			(c == 0xf4 && next > 0x8f))
			return false;
		while (count--)
			if (((uint8_t)text.data[i++] & 0xc0) != 0x80)
				return false;
	}
	return true;
}

static vmf_status index_info(const vmf_index_schema *index,
	vmf_index_info *info)
{
	*info = {};
	if (!index || !number_valid(index->index_number) ||
		index->encode_type < VMF_ENCODE_DEFAULT ||
		index->encode_type > VMF_ENCODE_LITERAL ||
		index->indicator < VMF_INDICATOR_NONE ||
		index->indicator > VMF_INDICATOR_GRI)
		return VMF_INVALID_SCHEMA;
	info->key = index->key;
	info->indicator = index->indicator;
	info->target = index->target;
	info->number_of_bits = index->number_of_bits;
	if (!utf8_valid(index->key))
		return VMF_INVALID_SCHEMA;
	if (index->indicator != VMF_INDICATOR_NONE) {
		if (index->number_of_bits > 1 ||
			index->encode_type != VMF_ENCODE_DEFAULT ||
			index->refinements)
			return VMF_INVALID_SCHEMA;
		info->number_of_bits = 1;
		info->unit_bits = 1;
		return VMF_OK;
	}
	if (index->target.kind != VMF_TARGET_NONE ||
		index->number_of_bits == VMF_UNITS_UNKNOWN)
		return VMF_INVALID_SCHEMA;
	vmf_index_refinements empty = {};
	const vmf_index_refinements *r = index->refinements ?
		index->refinements : &empty;
	if (!order_valid(r->bit_order) ||
		r->representation < VMF_INTEGER_DEFAULT ||
		r->representation > VMF_INTEGER_SIGN_MAGNITUDE ||
		r->text_encoding < VMF_TEXT_DEFAULT ||
		r->text_encoding > VMF_TEXT_UTF_8 ||
		(r->length_from.count && !r->length_from.parts) ||
		(r->length_bias.present && !r->length_from.count))
		return VMF_INVALID_SCHEMA;
	vmf_encode_type type = index->encode_type == VMF_ENCODE_DEFAULT ?
		VMF_ENCODE_UNSIGNED : index->encode_type;
	switch (type) {
	case VMF_ENCODE_BOOL:
		info->kind = VMF_FIELD_BOOL;
		break;
	case VMF_ENCODE_UNSIGNED:
		info->kind = VMF_FIELD_UNSIGNED;
		break;
	case VMF_ENCODE_SIGNED:
		info->kind = VMF_FIELD_SIGNED;
		break;
	case VMF_ENCODE_ENUM:
		info->kind = VMF_FIELD_ENUM;
		break;
	case VMF_ENCODE_SCALED:
		info->kind = VMF_FIELD_SCALED;
		break;
	case VMF_ENCODE_BITS:
		info->kind = VMF_FIELD_BITS;
		break;
	case VMF_ENCODE_OCTETS:
		info->kind = VMF_FIELD_OCTETS;
		break;
	case VMF_ENCODE_TEXT:
	case VMF_ENCODE_LITERAL:
		info->kind = VMF_FIELD_TEXT;
		break;
	default: return VMF_INVALID_SCHEMA;
	}
	info->integer.order = resolved_order(r->bit_order);
	info->when = r->when;
	info->allowed = r->allowed;
	info->allowed_count = r->allowed_count;
	info->constant = r->constant;
	info->scale = r->scale;
	info->length_from = r->length_from;
	info->length_bias = r->length_bias.present ? r->length_bias.value : 0;
	if (info->kind <= VMF_FIELD_SCALED) {
		if (!index->number_of_bits || index->number_of_bits > 64 ||
			r->text_encoding ||
			r->length_from.count || r->length_bias.present ||
			(info->kind != VMF_FIELD_SCALED && r->scale))
			return VMF_INVALID_SCHEMA;
		info->integer.bit_width = (uint8_t)index->number_of_bits;
		info->integer.representation = r->representation ==
			VMF_INTEGER_DEFAULT ? VMF_INTEGER_UNSIGNED :
			r->representation;
		vmf_integer_representation rep = info->integer.representation;
		if ((info->kind == VMF_FIELD_SIGNED &&
			rep == VMF_INTEGER_UNSIGNED) ||
			((info->kind == VMF_FIELD_BOOL ||
			info->kind == VMF_FIELD_UNSIGNED ||
			info->kind == VMF_FIELD_ENUM) &&
				rep != VMF_INTEGER_UNSIGNED) ||
			(rep != VMF_INTEGER_UNSIGNED &&
				index->number_of_bits < 2) ||
			(info->kind == VMF_FIELD_BOOL &&
				index->number_of_bits != 1))
			return VMF_INVALID_SCHEMA;
		uint64_t maximum = index->number_of_bits == 64 ? UINT64_MAX :
			(UINT64_C(1) << index->number_of_bits) - 1;
		if (!ranges_valid(r->allowed, r->allowed_count, maximum) ||
			(r->constant.present && r->constant.value > maximum))
			return VMF_INVALID_SCHEMA;
		if (r->constant.present && r->allowed_count) {
			bool found = false;
			for (size_t i = 0; i < r->allowed_count; ++i)
				found |=
				r->constant.value >= r->allowed[i].first &&
					r->constant.value <= r->allowed[i].last;
			if (!found)
				return VMF_INVALID_SCHEMA;
		}
		if (info->kind == VMF_FIELD_SCALED) {
			if (!r->scale || !r->scale->factor.denominator ||
				!r->scale->bias.denominator ||
				(r->scale->sentinel_count &&
					!r->scale->sentinels))
				return VMF_INVALID_SCHEMA;
			for (size_t i = 0; i < r->scale->sentinel_count; ++i) {
				if (r->scale->sentinels[i] > maximum)
					return VMF_INVALID_SCHEMA;
				for (size_t j = 0; j < i; ++j)
					if (r->scale->sentinels[i] ==
						r->scale->sentinels[j])
						return VMF_INVALID_SCHEMA;
			}
		}
		info->unit_bits = info->integer.bit_width;
		info->capacity_units = 1;
		return VMF_OK;
	}
	if ((info->kind == VMF_FIELD_BITS && r->bit_order != VMF_BIT_DEFAULT) ||
		r->representation || r->allowed_count || r->constant.present ||
		r->scale || (info->kind != VMF_FIELD_TEXT && r->text_encoding))
		return VMF_INVALID_SCHEMA;
	info->text_encoding = info->kind == VMF_FIELD_TEXT ?
		(r->text_encoding == VMF_TEXT_DEFAULT ? VMF_TEXT_ASCII_7 :
		r->text_encoding) : VMF_TEXT_DEFAULT;
	info->unit_bits = info->kind == VMF_FIELD_BITS ? 1 :
		info->text_encoding == VMF_TEXT_ASCII_7 ? 7 : 8;
	info->literal = type == VMF_ENCODE_LITERAL;
	info->variable = info->literal || r->length_from.count;
	if ((!info->variable && !index->number_of_bits) ||
		index->number_of_bits % info->unit_bits ||
		(info->literal && (r->length_from.count ||
			info->text_encoding != VMF_TEXT_ASCII_7)))
		return VMF_INVALID_SCHEMA;
	info->capacity_units = index->number_of_bits / info->unit_bits;
	return VMF_OK;
}

struct allocation {
	allocation *next;
	std::max_align_t alignment;
};

struct scope {
	vmf_scope_kind kind;
	uint32_t id;
	uint32_t category_parent;
	size_t first, last, original_first;
	size_t control, presence, parent, rank;
	uint64_t maximum;
	const vmf_repeat_definition *declaration;
	vmf_node *node;
	vmf_node *children;
	size_t child_count;
	vmf_index_number anchor;
};

struct row {
	vmf_node *node;
	size_t parent;
	size_t presence;
	vmf_index_info info;
};

struct work {
	const vmf_schema_set *set;
	const vmf_message_schema *message;
	const vmf_limits *limits;
	const vmf_allocator *allocator;
	vmf_runtime_set *owner;
	vmf_error *error;
	size_t schema_index;
	scope *scopes;
	size_t scope_count;
	row *rows;
	vmf_status status;
};

static vmf_status fail(work *w, vmf_status status,
	vmf_schema_property property, size_t index = SIZE_MAX,
	size_t related = SIZE_MAX, uint32_t scope_id = 0)
{
	if (w->error) {
		*w->error = clean_error(status);
		w->error->schema_index = w->schema_index;
		w->error->index_position = index;
		w->error->property = property;
		w->error->related_index_position = related;
		w->error->related_scope_id = scope_id;
	}
	w->status = status;
	return status;
}

static void *allocate(work *w, size_t count, size_t size)
{
	if (!count)
		return nullptr;
	if (size && count > (SIZE_MAX - sizeof(allocation)) / size) {
		fail(w, VMF_LIMIT_EXCEEDED, VMF_PROPERTY_NONE);
		return nullptr;
	}
	size_t bytes = sizeof(allocation) + count * size;
	void *memory = w->allocator ? w->allocator->allocate(
		w->allocator->user, bytes) : std::malloc(bytes);
	if (!memory) {
		fail(w, VMF_OUT_OF_MEMORY, VMF_PROPERTY_NONE);
		return nullptr;
	}
	if (reinterpret_cast<uintptr_t>(memory) % alignof(std::max_align_t)) {
		if (w->allocator)
			w->allocator->deallocate(w->allocator->user, memory);
		else
			std::free(memory);
		fail(w, VMF_INVALID_ARGUMENT, VMF_PROPERTY_NONE);
		return nullptr;
	}
	auto *entry = static_cast<allocation *>(memory);
	entry->next = static_cast<allocation *>(w->owner->allocations);
	w->owner->allocations = entry;
	void *result = entry + 1;
	std::memset(result, 0, count * size);
	return result;
}

static size_t find_scope(work *w, vmf_scope_kind kind, uint32_t id)
{
	for (size_t i = 1; i < w->scope_count; ++i)
		if (w->scopes[i].kind == kind && w->scopes[i].id == id)
			return i;
	return SIZE_MAX;
}

static bool has_scope(const vmf_index_schema *index, const scope *s)
{
	if (s->kind == VMF_SCOPE_GROUP) {
		for (size_t i = 0; i < index->group_code.count; ++i)
			if (index->group_code.parts[i] == s->id)
				return true;
	} else {
		for (size_t i = 0; i < index->repeat_code.count; ++i)
			if (index->repeat_code.parts[i].id == s->id)
				return true;
	}
	return false;
}

static vmf_status add_scope(work *w, vmf_scope_kind kind, uint32_t id,
	uint32_t category_parent, size_t position)
{
	if (!id || id == category_parent)
		return fail(w, VMF_INVALID_SCHEMA, VMF_PROPERTY_GROUP,
			position);
	size_t slot = find_scope(w, kind, id);
	if (slot == SIZE_MAX) {
		if (w->scope_count == w->limits->max_schema_nodes)
			return fail(w, VMF_LIMIT_EXCEEDED, VMF_PROPERTY_GROUP,
				position);
		slot = w->scope_count++;
		w->scopes[slot] = {kind, id, category_parent, position,
			position,
			position, SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX, 0,
			nullptr, nullptr, nullptr, 0, {}};
	} else if (w->scopes[slot].category_parent != category_parent) {
		return fail(w, VMF_INVALID_SCHEMA, VMF_PROPERTY_GROUP,
			position, w->scopes[slot].original_first, id);
	}
	w->scopes[slot].last = position;
	return VMF_OK;
}

static size_t row_number(work *w, vmf_index_number number)
{
	for (size_t i = 0; i < w->message->index_count; ++i)
		if (number_equal(w->message->indexes[i].index_number, number))
			return i;
	return SIZE_MAX;
}

static vmf_status collect(work *w)
{
	const vmf_message_schema *m = w->message;
	size_t maximum = w->limits->max_schema_nodes;
	if ((m->index_count && !m->indexes) ||
		(m->repeat_count && !m->repeats) ||
			!order_valid(m->octet_order))
		return fail(w, VMF_INVALID_SCHEMA, VMF_PROPERTY_INDEX);
	if (m->index_count > maximum ||
		m->repeat_count > maximum ||
		m->index_count > UINT32_MAX / 2)
		return fail(w, VMF_LIMIT_EXCEEDED, VMF_PROPERTY_INDEX);
	w->rows = static_cast<row *>(allocate(w, m->index_count, sizeof(row)));
	w->scopes = static_cast<scope *>(allocate(w,
		maximum, sizeof(scope)));
	if ((!w->rows && m->index_count) || !w->scopes)
		return w->status;
	w->scope_count = 1;
	w->scopes[0].kind = VMF_SCOPE_ROOT;
	w->scopes[0].parent = SIZE_MAX;
	w->scopes[0].presence = SIZE_MAX;
	w->scopes[0].control = SIZE_MAX;
	for (size_t i = 0; i < m->index_count; ++i) {
		const vmf_index_schema *index = &m->indexes[i];
		w->rows[i].presence = SIZE_MAX;
		if (index->index_number.count > w->limits->max_depth ||
			index->index_number.count > maximum ||
			index->group_code.count > maximum ||
			index->repeat_code.count > maximum ||
			index->group_code.count > w->limits->max_depth ||
			index->repeat_code.count > w->limits->max_depth)
			return fail(w, VMF_LIMIT_EXCEEDED, VMF_PROPERTY_INDEX,
				i);
		if ((index->group_code.count && !index->group_code.parts) ||
			(index->repeat_code.count && !index->repeat_code.parts))
			return fail(w, VMF_INVALID_SCHEMA, VMF_PROPERTY_GROUP,
				i);
		const vmf_index_refinements *r = index->refinements;
		if (r && ((r->when && r->when->term_count >
			maximum) ||
			r->allowed_count > maximum ||
			(r->scale && r->scale->sentinel_count >
			maximum)))
			return fail(w, VMF_LIMIT_EXCEEDED,
				VMF_PROPERTY_CONSTRAINT, i);
		vmf_status status = index_info(index, &w->rows[i].info);
		if (status != VMF_OK)
			return fail(w, status, VMF_PROPERTY_ENCODING, i);
		if (index->number_of_bits > w->limits->max_field_bits)
			return fail(w, VMF_LIMIT_EXCEEDED,
				VMF_PROPERTY_ENCODING, i);
		for (size_t j = 0; j < i; ++j) {
			if (number_equal(index->index_number,
				m->indexes[j].index_number))
				return fail(w, VMF_INVALID_SCHEMA,
					VMF_PROPERTY_INDEX, i, j);
			if (index->key.size && string_equal(index->key,
				m->indexes[j].key))
				return fail(w, VMF_INVALID_SCHEMA,
					VMF_PROPERTY_KEY, i, j);
		}
		for (size_t j = 0; j < index->group_code.count; ++j) {
			status = add_scope(w, VMF_SCOPE_GROUP,
				index->group_code.parts[j], j ?
				index->group_code.parts[j - 1] : 0, i);
			if (status != VMF_OK)
				return status;
		}
		for (size_t j = 0; j < index->repeat_code.count; ++j) {
			const vmf_repeat_component *part =
				&index->repeat_code.parts[j];
			status = add_scope(w, VMF_SCOPE_REPEAT, part->id,
				j ? index->repeat_code.parts[j - 1].id : 0, i);
			if (status != VMF_OK)
				return status;
			if (part->maximum.present &&
				(j + 1 != index->repeat_code.count ||
				(index->indicator != VMF_INDICATOR_FRI &&
				index->indicator != VMF_INDICATOR_GRI)))
				return fail(w, VMF_INVALID_SCHEMA,
					VMF_PROPERTY_REPEAT, i);
		}
	}
	return VMF_OK;
}

static vmf_status controls(work *w)
{
	const vmf_message_schema *m = w->message;
	for (size_t i = 0; i < m->repeat_count; ++i) {
		const vmf_repeat_definition *d = &m->repeats[i];
		size_t s = find_scope(w, VMF_SCOPE_REPEAT, d->id);
		if (s == SIZE_MAX || w->scopes[s].declaration)
			return fail(w, VMF_INVALID_SCHEMA, VMF_PROPERTY_REPEAT,
				SIZE_MAX, SIZE_MAX, d->id);
		w->scopes[s].declaration = d;
		if (d->kind == VMF_REPEAT_FIXED) {
			if (d->count == VMF_UNITS_UNKNOWN ||
				d->count_from.count ||
				d->bias.present || d->minimum.present ||
					d->maximum)
				return fail(w, VMF_INVALID_SCHEMA,
					VMF_PROPERTY_REPEAT, SIZE_MAX, SIZE_MAX,
					d->id);
		} else if (d->kind == VMF_REPEAT_FIELD) {
			if (d->count || !number_valid(d->count_from) ||
				d->maximum == VMF_UNITS_UNKNOWN ||
				(d->minimum.present &&
				d->minimum.value > d->maximum))
				return fail(w, VMF_INVALID_SCHEMA,
					VMF_PROPERTY_REPEAT, SIZE_MAX, SIZE_MAX,
					d->id);
		} else {
			return fail(w, VMF_INVALID_SCHEMA, VMF_PROPERTY_REPEAT);
		}
	}
	for (size_t i = 0; i < m->index_count; ++i) {
		const vmf_index_schema *index = &m->indexes[i];
		if (!index->indicator)
			continue;
		if (index->indicator == VMF_INDICATOR_FPI) {
			if (index->target.kind == VMF_TARGET_INDEX &&
				(index->target.as.index.count >
				w->limits->max_depth ||
				index->target.as.index.count >
				w->limits->max_schema_nodes))
				return fail(w, VMF_LIMIT_EXCEEDED,
					VMF_PROPERTY_TARGET, i);
			size_t target = index->target.kind == VMF_TARGET_INDEX ?
				row_number(w,
					index->target.as.index) : SIZE_MAX;
			if (target == SIZE_MAX || target != i + 1 ||
				m->indexes[target].indicator ||
				w->rows[target].presence != SIZE_MAX ||
				w->rows[target].info.when)
				return fail(w, VMF_INVALID_SCHEMA,
					VMF_PROPERTY_TARGET, i, target);
			w->rows[target].presence = i;
		} else if (index->indicator == VMF_INDICATOR_GPI) {
			size_t target = index->target.kind == VMF_TARGET_GROUP ?
				find_scope(w, VMF_SCOPE_GROUP,
					index->target.as.group) : SIZE_MAX;
			if (target == SIZE_MAX || w->scopes[target].presence !=
				SIZE_MAX || w->scopes[target].first != i + 1 ||
				has_scope(index, &w->scopes[target]))
				return fail(w, VMF_INVALID_SCHEMA,
					VMF_PROPERTY_TARGET, i);
			w->scopes[target].presence = i;
			w->scopes[target].first = i;
		} else {
			if (index->target.kind != VMF_TARGET_NONE ||
				!index->repeat_code.count)
				return fail(w, VMF_INVALID_SCHEMA,
					VMF_PROPERTY_INDICATOR, i);
			size_t final = index->repeat_code.count - 1;
			const vmf_repeat_component *last =
				&index->repeat_code.parts[final];
			size_t target = find_scope(w, VMF_SCOPE_REPEAT,
				last->id);
			scope *s = &w->scopes[target];
			if (s->control != SIZE_MAX || s->declaration ||
				s->first != i || !last->maximum.present ||
				!last->maximum.value ||
				last->maximum.value == VMF_UNITS_UNKNOWN)
				return fail(w, VMF_INVALID_SCHEMA,
					VMF_PROPERTY_INDICATOR, i);
			s->control = i;
			s->maximum = last->maximum.value;
		}
	}
	for (size_t i = 1; i < w->scope_count; ++i) {
		scope *s = &w->scopes[i];
		if (s->kind == VMF_SCOPE_REPEAT && s->control == SIZE_MAX &&
			!s->declaration)
			return fail(w, VMF_INVALID_SCHEMA, VMF_PROPERTY_REPEAT,
				s->first, SIZE_MAX, s->id);
		for (size_t j = s->first; j <= s->last; ++j)
			if (j != s->presence &&
				!has_scope(&m->indexes[j], s))
				return fail(w, VMF_INVALID_SCHEMA,
					VMF_PROPERTY_GROUP, j, s->first, s->id);
	}
	return VMF_OK;
}

static void index_anchors(work *w)
{
	for (size_t i = 1; i < w->scope_count; ++i) {
		scope *s = &w->scopes[i];
		bool found = false;
		for (size_t j = s->first; j <= s->last; ++j) {
			const vmf_index_schema *index = &w->message->indexes[j];
			if (index->indicator || !has_scope(index, s))
				continue;
			if (!found) {
				s->anchor = index->index_number;
				found = true;
			} else {
				size_t common = 0;
				while (common < s->anchor.count &&
					common < index->index_number.count &&
					s->anchor.parts[common] ==
					index->index_number.parts[common])
					++common;
				s->anchor.count = common;
			}
		}
		size_t control = s->presence != SIZE_MAX ?
			s->presence : s->control;
		if (control != SIZE_MAX) {
			vmf_index_number address =
				w->message->indexes[control].index_number;
			if (number_prefix(address, s->anchor))
				s->anchor = address;
		}
	}
}

static bool before(work *w, size_t a, size_t b)
{
	const scope *outer = &w->scopes[a];
	const scope *inner = &w->scopes[b];
	if (number_prefix(outer->anchor, inner->anchor))
		return true;
	if (outer->kind == inner->kind) {
		uint32_t parent = inner->category_parent;
		for (size_t steps = 0; parent &&
			steps < w->scope_count; ++steps) {
			if (parent == outer->id)
				return true;
			size_t next = find_scope(w, inner->kind, parent);
			if (next == SIZE_MAX)
				break;
			parent = w->scopes[next].category_parent;
		}
	}
	return (inner->control != SIZE_MAX &&
		has_scope(&w->message->indexes[inner->control], outer)) ||
		(inner->presence != SIZE_MAX &&
		has_scope(&w->message->indexes[inner->presence], outer));
}

static bool encloses(const scope *a, const scope *b)
{
	return a->first <= b->first && a->last >= b->last;
}

static vmf_status nesting(work *w)
{
	index_anchors(w);
	for (size_t i = 1; i < w->scope_count; ++i) {
		scope *s = &w->scopes[i];
		for (size_t j = i + 1; j < w->scope_count; ++j) {
			scope *other = &w->scopes[j];
			if ((encloses(s, other) &&
				 number_prefix(other->anchor, s->anchor)) ||
				(encloses(other, s) &&
				 number_prefix(s->anchor, other->anchor))) {
				if (s->first != other->first ||
					s->last != other->last)
					return fail(w, VMF_INVALID_SCHEMA,
						VMF_PROPERTY_GROUP, s->first,
						other->first, other->id);
			}
			if (s->first <= other->last &&
				other->first <= s->last &&
				!encloses(s, other) && !encloses(other, s))
				return fail(w, VMF_INVALID_SCHEMA,
					VMF_PROPERTY_GROUP, s->first,
					other->first,
					other->id);
		}
	}
	for (size_t i = 1; i < w->scope_count; ++i) {
		scope *first = &w->scopes[i];
		if (first->rank != SIZE_MAX)
			continue;
		for (size_t rank = 0;; ++rank) {
			size_t candidate = SIZE_MAX;
			bool pending = false;
			for (size_t j = i; j < w->scope_count; ++j) {
				scope *s = &w->scopes[j];
				if (s->first != first->first ||
					s->last != first->last ||
					s->rank != SIZE_MAX)
					continue;
				pending = true;
				bool blocked = false;
				for (size_t k = i; k < w->scope_count; ++k)
					if (k != j &&
					w->scopes[k].first == s->first &&
						w->scopes[k].last == s->last &&
						w->scopes[k].rank == SIZE_MAX &&
						before(w, k, j))
						blocked = true;
				if (blocked)
					continue;
				if (candidate != SIZE_MAX)
					return fail(w, VMF_INVALID_SCHEMA,
						VMF_PROPERTY_GROUP, s->first,
						w->scopes[candidate].first,
						s->id);
				candidate = j;
			}
			if (!pending)
				break;
			if (candidate == SIZE_MAX)
				return fail(w, VMF_INVALID_SCHEMA,
					VMF_PROPERTY_GROUP, first->first);
			w->scopes[candidate].rank = rank;
		}
	}
	for (size_t i = 1; i < w->scope_count; ++i) {
		scope *s = &w->scopes[i];
		s->parent = 0;
		for (size_t j = 1; j < w->scope_count; ++j) {
			const scope *candidate = &w->scopes[j];
			if (i == j || !encloses(candidate, s) ||
				(candidate->first == s->first &&
				candidate->last == s->last &&
					candidate->rank >= s->rank))
				continue;
			const scope *parent = &w->scopes[s->parent];
			if (!s->parent || (encloses(parent, candidate) &&
				(parent->first != candidate->first ||
				parent->last != candidate->last ||
				parent->rank < candidate->rank)))
				s->parent = j;
		}
	}
	for (size_t i = 1; i < w->scope_count; ++i) {
		scope *s = &w->scopes[i];
		bool category_found = !s->category_parent;
		for (size_t p = s->parent; p; p = w->scopes[p].parent)
			if (w->scopes[p].kind == s->kind &&
				w->scopes[p].id == s->category_parent)
				category_found = true;
		if (!category_found)
			return fail(w, VMF_INVALID_SCHEMA,
				VMF_PROPERTY_GROUP, s->first);
	}
	for (size_t i = 0; i < w->message->index_count; ++i) {
		w->rows[i].parent = 0;
		for (size_t j = 1; j < w->scope_count; ++j)
			if (has_scope(&w->message->indexes[i], &w->scopes[j])) {
				size_t p = w->rows[i].parent;
				const scope *up = &w->scopes[p];
				const scope *current = &w->scopes[j];
				if (!p || (encloses(up, current) &&
					(up->first != current->first ||
					up->last != current->last ||
					up->rank < current->rank)))
					w->rows[i].parent = j;
			}
	}
	for (size_t i = 1; i < w->scope_count; ++i) {
		scope *s = &w->scopes[i];
		if (s->presence != SIZE_MAX &&
			w->rows[s->presence].parent != s->parent)
			return fail(w, VMF_INVALID_SCHEMA,
				VMF_PROPERTY_INDICATOR, s->presence);
		if (s->control != SIZE_MAX &&
			w->rows[s->control].parent != i)
			return fail(w, VMF_INVALID_SCHEMA,
				VMF_PROPERTY_INDICATOR, s->control);
	}
	for (size_t i = 0; i < w->message->index_count; ++i)
		if (w->rows[i].presence != SIZE_MAX &&
			w->rows[i].parent !=
			w->rows[w->rows[i].presence].parent)
			return fail(w, VMF_INVALID_SCHEMA,
				VMF_PROPERTY_INDICATOR, w->rows[i].presence, i);
	return VMF_OK;
}

static vmf_status make_nodes(work *w, vmf_runtime_schema *runtime)
{
	size_t count = 1;
	for (size_t i = 1; i < w->scope_count; ++i) {
		++w->scopes[w->scopes[i].parent].child_count;
		count += w->scopes[i].kind == VMF_SCOPE_REPEAT ? 2 : 1;
	}
	for (size_t i = 0; i < w->message->index_count; ++i)
		if (!w->message->indexes[i].indicator) {
			++w->scopes[w->rows[i].parent].child_count;
			++count;
		}
	if (count > w->limits->max_schema_nodes || count > UINT32_MAX)
		return fail(w, VMF_LIMIT_EXCEEDED, VMF_PROPERTY_GROUP);
	auto **bindings = static_cast<const vmf_node **>(
		allocate(w, count, sizeof(const vmf_node *)));
	auto *root = static_cast<vmf_node *>(allocate(w, 1, sizeof(vmf_node)));
	if (!bindings || !root)
		return w->status;
	runtime->bindings = bindings;
	runtime->binding_count = count;
	runtime->root = root;
	w->scopes[0].node = root;
	root->kind = VMF_NODE_GROUP;
	root->scope = {VMF_SCOPE_ROOT, 0};
	root->id = 1;
	bindings[0] = root;
	size_t next = 1;
	for (size_t i = 0; i < w->scope_count; ++i) {
		scope *s = &w->scopes[i];
		s->children = static_cast<vmf_node *>(allocate(w,
			s->child_count, sizeof(vmf_node)));
		if (s->child_count && !s->children)
			return w->status;
		size_t child = 0;
		for (size_t j = 0; j < w->message->index_count; ++j) {
			for (size_t k = 1; k < w->scope_count; ++k)
				if (w->scopes[k].parent == i &&
					w->scopes[k].first == j) {
					vmf_node *node = &s->children[child++];
					w->scopes[k].node = node;
					node->id = (uint32_t)next + 1;
					bindings[next++] = node;
					node->scope = {w->scopes[k].kind,
						w->scopes[k].id};
				}
			if (!w->message->indexes[j].indicator &&
				w->rows[j].parent == i) {
				vmf_node *node = &s->children[child++];
				w->rows[j].node = node;
				node->id = (uint32_t)next + 1;
				bindings[next++] = node;
				node->source = &w->message->indexes[j];
			}
		}
	}
	for (size_t i = 0; i < w->scope_count; ++i) {
		scope *s = &w->scopes[i];
		vmf_node *body = s->node;
		if (s->kind == VMF_SCOPE_REPEAT) {
			body = static_cast<vmf_node *>(allocate(w, 1,
				sizeof(vmf_node)));
			if (!body)
				return w->status;
			body->id = (uint32_t)next + 1;
			bindings[next++] = body;
			body->scope = {VMF_SCOPE_REPEAT_BODY, s->id};
			s->node->kind = VMF_NODE_REPEAT;
			s->node->as.repeat.body = body;
		} else {
			s->node->kind = VMF_NODE_GROUP;
		}
		body->kind = VMF_NODE_GROUP;
		body->as.group = {s->children, s->child_count};
		if (s->presence != SIZE_MAX) {
			s->node->presence.kind = VMF_PRESENCE_BIT;
			s->node->presence.present_bit = true;
		}
		if (i)
			s->node->source = &w->message->indexes[
				s->presence != SIZE_MAX ?
					s->presence : s->original_first];
		if (s->kind == VMF_SCOPE_REPEAT)
			body->source = s->node->source;
	}
	return VMF_OK;
}

static vmf_status dependency_id(work *w, vmf_index_number reference,
	vmf_node_id *id, size_t position)
{
	if (reference.count > w->limits->max_depth ||
		reference.count > w->limits->max_schema_nodes)
		return fail(w, VMF_LIMIT_EXCEEDED, VMF_PROPERTY_DEPENDENCY,
			position);
	if (!number_valid(reference))
		return fail(w, VMF_INVALID_SCHEMA, VMF_PROPERTY_DEPENDENCY,
			position);
	size_t source = row_number(w, reference);
	if (source == SIZE_MAX || !w->rows[source].node)
		return fail(w, VMF_INVALID_SCHEMA, VMF_PROPERTY_DEPENDENCY,
			position, source);
	*id = w->rows[source].node->id;
	return VMF_OK;
}

static vmf_status fill_nodes(work *w)
{
	for (size_t i = 0; i < w->message->index_count; ++i) {
		row *r = &w->rows[i];
		vmf_node *node = r->node;
		if (!node)
			continue;
		const vmf_index_info *info = &r->info;
		node->kind = VMF_NODE_FIELD;
		node->as.field.kind = info->kind;
		if (r->presence != SIZE_MAX) {
			node->presence.kind = VMF_PRESENCE_BIT;
			node->presence.present_bit = true;
		}
		if (info->when) {
			const vmf_condition *condition = info->when;
			if (!condition->term_count || !condition->terms)
				return fail(w, VMF_INVALID_SCHEMA,
					VMF_PROPERTY_DEPENDENCY, i);
			auto *terms = static_cast<vmf_runtime_condition_term *>(
				allocate(w, condition->term_count,
					sizeof(vmf_runtime_condition_term)));
			if (!terms)
				return w->status;
			node->presence.kind = VMF_PRESENCE_CONDITION;
			node->presence.condition = {condition->mode, terms,
				condition->term_count, condition->negate};
			for (size_t j = 0; j < condition->term_count; ++j) {
				const vmf_condition_term *term =
					&condition->terms[j];
				if (term->value_count >
					w->limits->max_schema_nodes)
					return fail(w, VMF_LIMIT_EXCEEDED,
						VMF_PROPERTY_CONSTRAINT, i);
				if (dependency_id(w, term->field,
					&terms[j].field, i)
					!= VMF_OK)
					return w->status;
				terms[j].mask = term->mask.present ?
					term->mask.value : UINT64_MAX;
				terms[j].values = term->values;
				terms[j].value_count = term->value_count;
				terms[j].negate = term->negate;
			}
		}
		if (info->kind <= VMF_FIELD_SCALED) {
			vmf_scalar_definition *scalar =
				&node->as.field.as.scalar;
			scalar->encoding = info->integer;
			scalar->allowed = info->allowed;
			scalar->allowed_count = info->allowed_count;
			if (info->constant.present) {
				auto *range = static_cast<vmf_code_range *>(
					allocate(w, 1, sizeof(vmf_code_range)));
				if (!range)
					return w->status;
				*range = {info->constant.value,
					info->constant.value};
				scalar->allowed = range;
				scalar->allowed_count = 1;
			}
			if (info->scale) {
				const vmf_scale_definition *scale = info->scale;
				auto *labels = static_cast<vmf_code_label *>(
					allocate(w,
					scale->sentinel_count,
					sizeof(vmf_code_label)));
				if (scale->sentinel_count && !labels)
					return w->status;
				for (size_t j =
					0; j < scale->sentinel_count; ++j)
					labels[j].code = scale->sentinels[j];
				scalar->scale = {scale->factor, scale->bias,
					labels, scale->sentinel_count};
			}
			continue;
		}
		vmf_extent_definition length = {};
		length.maximum = info->capacity_units;
		if (info->literal) {
			length.kind = VMF_EXTENT_LITERAL;
		} else if (info->length_from.count) {
			length.kind = VMF_EXTENT_FIELD;
			length.bias = info->length_bias;
			if (dependency_id(w, info->length_from, &length.field,
				i)
				!= VMF_OK)
				return w->status;
		} else {
			length.kind = VMF_EXTENT_FIXED;
			length.fixed = length.minimum = info->capacity_units;
		}
		if (info->kind == VMF_FIELD_TEXT)
			node->as.field.as.text = {info->text_encoding,
				info->integer.order, length};
		else if (info->kind == VMF_FIELD_OCTETS)
			node->as.field.as.octets = {info->integer.order,
				length};
		else
			node->as.field.as.bits = {length};
	}
	for (size_t i = 1; i < w->scope_count; ++i) {
		scope *s = &w->scopes[i];
		if (s->kind != VMF_SCOPE_REPEAT)
			continue;
		vmf_extent_definition *count = &s->node->as.repeat.count;
		if (s->control != SIZE_MAX) {
			count->kind = VMF_EXTENT_RECURRENCE_BEFORE;
			count->minimum = 1;
			count->maximum = s->maximum;
			count->more_bit = true;
			if (w->message->indexes[s->control].indicator ==
				VMF_INDICATOR_FRI && (s->child_count != 1 ||
				s->children[0].kind != VMF_NODE_FIELD))
				return fail(w, VMF_INVALID_SCHEMA,
					VMF_PROPERTY_INDICATOR, s->control);
		} else {
			const vmf_repeat_definition *d = s->declaration;
			if (d->kind == VMF_REPEAT_FIXED) {
				count->fixed = count->minimum = count->maximum =
					d->count;
			} else {
				count->kind = VMF_EXTENT_FIELD;
				count->minimum = d->minimum.present ?
					d->minimum.value : 0;
				count->maximum = d->maximum;
				count->bias = d->bias.present ?
					d->bias.value : 0;
				if (dependency_id(w, d->count_from,
					&count->field,
					s->first) != VMF_OK)
					return w->status;
			}
		}
	}
	return VMF_OK;
}

} /* namespace */
bool vmf_internal::message_version_valid(vmf_envelope_version profile,
	const vmf_message_id *id)
{
	if (!id || profile < VMF_ENVELOPE_B || profile > VMF_ENVELOPE_D1)
		return false;
	if (profile == VMF_ENVELOPE_B)
		return !id->standard_version.present;
	if (!id->standard_version.present)
		return profile == VMF_ENVELOPE_C ||
			id->user_message_format != 2;
	uint8_t code = id->standard_version.value;
	bool c = profile == VMF_ENVELOPE_C;
	switch (id->user_message_format) {
	case 0:
	case 5:
		return code <= (c ? 9 : 15);
	case 2:
		return code <= (c ? 9 : 10);
	case 3:
		return !c && code == 0;
	case 7:
		return code >= 6 && code <= (c ? 9 : 15);
	case 8:
		return code >= 3 && code <= (c ? 9 : 10);
	default: return false;
	}
}

vmf_status vmf_schema_message_id(const vmf_schema_set *set,
	const vmf_message_schema *message, vmf_message_id *result)
{
	if (result)
		*result = {};
	if (!result || !member(set, message))
		return VMF_INVALID_ARGUMENT;
	if (set->envelope < VMF_ENVELOPE_B || set->envelope > VMF_ENVELOPE_D1 ||
		message->message_number.functional_area > 10 ||
		!message->message_number.number ||
			message->message_number.number > 127 ||
		(message->subtype.present && message->subtype.value > 127))
		return VMF_INVALID_SCHEMA;
	vmf_standard_selection selection = message->standard.kind ==
		VMF_STANDARD_INHERIT ? set->standard : message->standard;
	vmf_message_id id = {2, {}, true,
		message->message_number.functional_area,
		message->message_number.number, message->subtype};
	if (selection.kind == VMF_STANDARD_RAW_CODE) {
		id.standard_version = {true, selection.as.raw_code};
	} else if (selection.kind == VMF_STANDARD_EDITION) {
		bool c = set->envelope == VMF_ENVELOPE_C;
		int code = -1;
		switch (selection.as.edition) {
		case VMF_STANDARD_6017:
			code = c ? 6 : 5;
			break;
		case VMF_STANDARD_6017A:
			code = c ? 7 : 6;
			break;
		case VMF_STANDARD_6017B:
			code = c ? 8 : 7;
			break;
		case VMF_STANDARD_6017C:
			code = c ? 9 : 8;
			break;
		case VMF_STANDARD_6017D:
			if (!c) code = 9;
			break;
		case VMF_STANDARD_6017E:
			if (!c) code = 10;
			break;
		case VMF_STANDARD_TIDP_TE_R2:
			code = 0;
			break;
		case VMF_STANDARD_TIDP_TE_R3:
			code = 1;
			break;
		case VMF_STANDARD_TIDP_TE_R4:
			code = 2;
			break;
		case VMF_STANDARD_TIDP_TE_R5:
			code = 3;
			break;
		case VMF_STANDARD_TIDP_TE_R6:
			if (c) code = 4;
			break;
		case VMF_STANDARD_TIDP_TE_R7:
			if (c) code = 5;
			break;
		case VMF_STANDARD_TIDP_FTE_R6:
			if (!c) code = 4;
			break;
		}
		if (code < 0)
			return VMF_INVALID_SCHEMA;
		id.standard_version = {true, (uint8_t)code};
	} else if (selection.kind != VMF_STANDARD_INHERIT &&
		selection.kind != VMF_STANDARD_ABSENT) {
		return VMF_INVALID_SCHEMA;
	}
	if (!vmf_internal::message_version_valid(set->envelope, &id))
		return VMF_INVALID_SCHEMA;
	*result = id;
	return VMF_OK;
}

vmf_status vmf_schema_select(const vmf_schema_set *set,
	const vmf_message_id *id, const vmf_message_schema **result)
{
	if (result)
		*result = nullptr;
	if (!set || !id || !result || (set->message_count && !set->messages))
		return VMF_INVALID_ARGUMENT;
	const vmf_message_schema *match = nullptr;
	for (size_t i = 0; i < set->message_count; ++i) {
		vmf_message_id candidate;
		vmf_status status = vmf_schema_message_id(set,
			&set->messages[i],
			&candidate);
		if (status != VMF_OK)
			return status;
		if (vmf_message_id_equal(&candidate, id)) {
			if (match)
				return VMF_DUPLICATE_MESSAGE;
			match = &set->messages[i];
		}
	}
	*result = match;
	return match ? VMF_OK : VMF_UNKNOWN_MESSAGE;
}

const vmf_index_schema *vmf_schema_find(const vmf_message_schema *message,
	vmf_index_number number)
{
	if (!message || !number_valid(number))
		return nullptr;
	for (size_t i = 0; i < message->index_count; ++i)
		if (number_equal(message->indexes[i].index_number, number))
			return &message->indexes[i];
	return nullptr;
}

const vmf_index_schema *vmf_schema_find_key(const vmf_message_schema *message,
	vmf_string key)
{
	if (!message || !key.size || !key.data)
		return nullptr;
	for (size_t i = 0; i < message->index_count; ++i)
		if (string_equal(message->indexes[i].key, key))
			return &message->indexes[i];
	return nullptr;
}

vmf_status vmf_schema_index_info(const vmf_schema_set *set,
	const vmf_message_schema *message, const vmf_index_schema *index,
	vmf_index_info *result)
{
	if (result)
		*result = {};
	if (!result || !index || !member(set, message))
		return VMF_INVALID_ARGUMENT;
	for (size_t i = 0; i < message->index_count; ++i)
		if (&message->indexes[i] == index) {
			vmf_index_info info;
			vmf_status status = index_info(index, &info);
			if (status == VMF_OK)
				*result = info;
			return status;
		}
	return VMF_INVALID_ARGUMENT;
}

void vmf_internal::destroy_normalized_set(vmf_runtime_set *set,
	const vmf_allocator *allocator)
{
	if (!set)
		return;
	auto *entry = static_cast<allocation *>(set->allocations);
	while (entry) {
		allocation *next = entry->next;
		if (allocator)
			allocator->deallocate(allocator->user, entry);
		else
			std::free(entry);
		entry = next;
	}
	*set = {};
}

vmf_status vmf_internal::normalize_schemas(const vmf_schema_set *set,
	const vmf_limits *limits, const vmf_allocator *allocator,
	vmf_runtime_set *result, vmf_error *error)
{
	if (error)
		*error = clean_error(VMF_OK);
	if (result)
		*result = {};
	if (!set || !limits || !result ||
		(set->message_count && !set->messages) ||
		(allocator &&
			(!!allocator->allocate != !!allocator->deallocate))) {
		if (error)
			error->status = VMF_INVALID_ARGUMENT;
		return VMF_INVALID_ARGUMENT;
	}
	if (allocator && !allocator->allocate)
		allocator = nullptr;
	work w = {set, nullptr, limits, allocator, result, error, SIZE_MAX,
		nullptr, 0, nullptr, VMF_OK};
	if (set->envelope < VMF_ENVELOPE_B || set->envelope > VMF_ENVELOPE_D1)
		return fail(&w, VMF_INVALID_SCHEMA, VMF_PROPERTY_PROFILE);
	if ((!limits->max_schema_nodes && set->message_count) ||
		set->message_count > limits->max_schemas)
		return fail(&w, VMF_LIMIT_EXCEEDED, VMF_PROPERTY_NONE);
	if (set->standard.kind < VMF_STANDARD_INHERIT ||
		set->standard.kind > VMF_STANDARD_RAW_CODE)
		return fail(&w, VMF_INVALID_SCHEMA, VMF_PROPERTY_IDENTITY);
	if (set->standard.kind != VMF_STANDARD_INHERIT ||
		!set->message_count) {
		vmf_message_schema root = {};
		root.message_number = {0, 1};
		vmf_schema_set context = *set;
		context.messages = &root;
		context.message_count = 1;
		vmf_message_id identity;
		if (vmf_schema_message_id(&context, &root, &identity) != VMF_OK)
			return fail(&w, VMF_INVALID_SCHEMA,
				VMF_PROPERTY_IDENTITY);
	}
	auto **schemas = static_cast<const vmf_runtime_schema **>(
		allocate(&w, set->message_count,
			sizeof(const vmf_runtime_schema *)));
	if (set->message_count && !schemas)
		return w.status;
	result->schemas = schemas;
	result->count = set->message_count;
	for (size_t i = 0; i < set->message_count; ++i) {
		w.message = &set->messages[i];
		w.schema_index = i;
		auto *runtime = static_cast<vmf_runtime_schema *>(
			allocate(&w, 1, sizeof(vmf_runtime_schema)));
		if (!runtime)
			break;
		schemas[i] = runtime;
		runtime->source = w.message;
		runtime->octet_order = resolved_order(w.message->octet_order);
		vmf_status status = vmf_schema_message_id(set, w.message,
			&runtime->id);
		if (status != VMF_OK) {
			fail(&w, status, VMF_PROPERTY_IDENTITY);
			break;
		}
		for (size_t j = 0; j < i; ++j)
			if (vmf_message_id_equal(&runtime->id,
				&schemas[j]->id)) {
				fail(&w, VMF_DUPLICATE_MESSAGE,
					VMF_PROPERTY_IDENTITY);
				break;
			}
		if (w.status != VMF_OK || collect(&w) != VMF_OK ||
			controls(&w) != VMF_OK || nesting(&w) != VMF_OK ||
			make_nodes(&w, runtime) != VMF_OK ||
				fill_nodes(&w) != VMF_OK)
			break;
		status = runtime_schema_validate(runtime, limits, allocator,
			error);
		if (status != VMF_OK) {
			size_t id = error ? error->index_position : 0;
			const vmf_node *node = nullptr;

			for (size_t j = 0; j < runtime->binding_count; ++j)
				if (runtime->bindings[j]->id == id)
					node = runtime->bindings[j];
			size_t position = SIZE_MAX;
			size_t count = w.message->index_count;

			if (node && node->source)
				for (size_t j = 0; j < count; ++j)
					if (&w.message->indexes[j] ==
						node->source)
						position = j;
			fail(&w, status, VMF_PROPERTY_DEPENDENCY, position);
			break;
		}
	}
	if (w.status != VMF_OK) {
		destroy_normalized_set(result, allocator);
		return w.status;
	}
	if (error)
		*error = clean_error(VMF_OK);
	return VMF_OK;
}

vmf_status vmf_internal::validate_schemas_allocated(const vmf_schema_set *set,
	const vmf_limits *limits, const vmf_allocator *allocator,
		vmf_error *error)
{
	vmf_runtime_set normalized = {};
	vmf_status status = normalize_schemas(set, limits, allocator,
		&normalized, error);
	if (status == VMF_OK)
		destroy_normalized_set(&normalized,
			allocator && allocator->allocate ? allocator : nullptr);
	return status;
}

vmf_status vmf_schema_set_validate(const vmf_schema_set *set,
	const vmf_limits *limits, const vmf_allocator *allocator,
		vmf_error *error)
{
	return vmf_internal::validate_schemas_allocated(set, limits, allocator,
		error);
}

vmf_status vmf_schema_validate(const vmf_schema_set *set,
	const vmf_message_schema *message, const vmf_limits *limits,
	const vmf_allocator *allocator, vmf_error *error)
{
	if (!member(set, message)) {
		if (error)
			*error = clean_error(VMF_INVALID_ARGUMENT);
		return VMF_INVALID_ARGUMENT;
	}
	vmf_schema_set single = *set;
	single.messages = message;
	single.message_count = 1;
	vmf_status status = vmf_schema_set_validate(&single, limits,
		allocator, error);
	if (error && error->schema_index == 0)
		for (size_t i = 0; i < set->message_count; ++i)
			if (&set->messages[i] == message)
				error->schema_index = i;
	return status;
}
