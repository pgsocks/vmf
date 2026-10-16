#include "runtime_schema.hpp"
#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace
{

struct entry {
	const vmf_node *node;
	size_t parent;
	size_t depth;
	uint64_t minimum_bits;
	size_t child;
};

struct validation {
	entry *entries;
	size_t count;
	const vmf_limits *limits;
	vmf_error *error;
};

static vmf_status report(validation *v, vmf_status status, vmf_node_id id)
{
	if (v->error)
		*v->error = {status, 0, id, SIZE_MAX, SIZE_MAX,
			VMF_PROPERTY_NONE, SIZE_MAX, 0};
	return status;
}

static bool order_valid(vmf_bit_order order)
{
	return order == VMF_BIT_LSB_FIRST || order == VMF_BIT_MSB_FIRST;
}

static bool encoding_valid(const vmf_integer_encoding *encoding)
{
	return encoding->bit_width >= 1 && encoding->bit_width <= 64 &&
	order_valid(encoding->order) &&
	encoding->representation >= VMF_INTEGER_UNSIGNED &&
	encoding->representation <= VMF_INTEGER_SIGN_MAGNITUDE &&
	(encoding->representation == VMF_INTEGER_UNSIGNED ||
		encoding->bit_width >= 2);
}

static uint64_t maximum_code(unsigned width)
{
	return width == 64 ? UINT64_MAX : (UINT64_C(1) << width) - 1;
}

static bool ranges_valid(
	const vmf_code_range *ranges, size_t count, uint64_t maximum)
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

static bool ranges_overlap(const vmf_choice_case *a, const vmf_choice_case *b)
{
	for (size_t i = 0; i < a->value_count; ++i)
		for (size_t j = 0; j < b->value_count; ++j)
			if (a->values[i].first <= b->values[j].last &&
				b->values[j].first <= a->values[i].last)
				return true;
	return false;
}

static bool utf8_valid(const char *data, size_t size)
{
	for (size_t i = 0; i < size;) {
		uint8_t first = (uint8_t)data[i++];
		size_t following;

		if (first <= 0x7f)
			continue;
		if (first >= 0xc2 && first <= 0xdf)
			following = 1;
		else if (first >= 0xe0 && first <= 0xef)
			following = 2;
		else if (first >= 0xf0 && first <= 0xf4)
			following = 3;
		else
			return false;
		if (following > size - i)
			return false;
		uint8_t second = (uint8_t)data[i];

		if ((first == 0xe0 && second < 0xa0) ||
			(first == 0xed && second > 0x9f) ||
			(first == 0xf0 && second < 0x90) ||
			(first == 0xf4 && second > 0x8f))
			return false;
		for (size_t j = 0; j < following; ++j)
			if (((uint8_t)data[i++] & 0xc0) != 0x80)
				return false;
	}
	return true;
}

static bool labels_valid(
	const vmf_code_label *labels, size_t count, uint64_t maximum)
{
	if (count && !labels)
		return false;
	for (size_t i = 0; i < count; ++i) {
		const vmf_string *name = &labels[i].name;

		if (labels[i].code > maximum || (name->size && !name->data) ||
			!utf8_valid(name->data, name->size))
			return false;
		for (size_t j = 0; j < i; ++j) {
			if (labels[i].code == labels[j].code)
				return false;
			if (name->size && name->size == labels[j].name.size &&
				!std::memcmp(name->data, labels[j].name.data,
					name->size))
				return false;
		}
	}
	return true;
}

static bool scalar_kind(vmf_field_kind kind)
{
	return kind >= VMF_FIELD_BOOL && kind <= VMF_FIELD_SCALED;
}

static bool dependency(
	validation *v, size_t current, vmf_node_id id, bool extent)
{
	for (size_t i = 0; i < current; ++i) {
		const vmf_node *node = v->entries[i].node;

		if (node->id != id)
			continue;
		if (node->kind != VMF_NODE_FIELD ||
			node->presence.kind != VMF_PRESENCE_ALWAYS ||
			!scalar_kind(node->as.field.kind))
			return false;
		if (extent && node->as.field.kind != VMF_FIELD_BOOL &&
			node->as.field.kind != VMF_FIELD_UNSIGNED &&
			node->as.field.kind != VMF_FIELD_ENUM)
			return false;
		size_t scope = v->entries[current].parent;

		while (scope != SIZE_MAX) {
			if (scope == v->entries[i].parent)
				return v->entries[scope].node->kind ==
				VMF_NODE_GROUP;
			scope = v->entries[scope].parent;
		}
		return false;
	}
	return false;
}

static bool condition_valid(
	validation *v, size_t index, const vmf_runtime_condition *condition)
{
	if (!condition->term_count || !condition->terms ||
		(condition->mode != VMF_CONDITION_ALL &&
			condition->mode != VMF_CONDITION_ANY))
		return false;
	for (size_t i = 0; i < condition->term_count; ++i) {
		const vmf_runtime_condition_term *term = &condition->terms[i];

		if (!dependency(v, index, term->field, false) ||
			!term->value_count ||
			!ranges_valid(
				term->values, term->value_count, UINT64_MAX))
			return false;
	}
	return true;
}

static bool extent_valid(validation *v, size_t index,
	const vmf_extent_definition *extent, bool repeat, bool literal)
{
	if (extent->minimum > extent->maximum ||
		extent->minimum == VMF_UNITS_UNKNOWN)
		return false;
	if (extent->kind != VMF_EXTENT_PREFIX &&
		extent->kind != VMF_EXTENT_FIELD && extent->bias)
		return false;
	switch (extent->kind) {
	case VMF_EXTENT_FIXED:
		return extent->fixed != VMF_UNITS_UNKNOWN &&
		extent->fixed >= extent->minimum &&
		extent->fixed <= extent->maximum;
	case VMF_EXTENT_PREFIX:
		return encoding_valid(&extent->prefix) &&
		extent->prefix.representation == VMF_INTEGER_UNSIGNED;
	case VMF_EXTENT_FIELD:
		return dependency(v, index, extent->field, true);
	case VMF_EXTENT_REMAINING:
		return !repeat;
	case VMF_EXTENT_LITERAL:
		return !repeat && literal &&
		extent->maximum != VMF_UNITS_UNKNOWN;
	case VMF_EXTENT_MORE_BEFORE:
		return repeat;
	case VMF_EXTENT_MORE_AFTER:
	case VMF_EXTENT_RECURRENCE_BEFORE:
		return repeat && extent->minimum >= 1;
	default:
		return false;
	}
}

static vmf_status children(validation *v, const vmf_node *node, size_t index)
{
	if (node->kind == VMF_NODE_GROUP) {
		const vmf_group_schema *group = &node->as.group;

		if (group->child_count && !group->children)
			return report(v, VMF_INVALID_SCHEMA, node->id);
		if (group->child_count > v->limits->max_schema_nodes - v->count)
			return report(v, VMF_LIMIT_EXCEEDED, node->id);
	} else if (node->kind == VMF_NODE_REPEAT) {
		const vmf_repeat_schema *repeat = &node->as.repeat;

		if (!extent_valid(v, index, &repeat->count, true, false) ||
			!repeat->body || repeat->body->kind != VMF_NODE_GROUP ||
			repeat->body->presence.kind != VMF_PRESENCE_ALWAYS)
			return report(v, VMF_INVALID_SCHEMA, node->id);
		uint64_t minimum = repeat->count.kind == VMF_EXTENT_FIXED
					? repeat->count.fixed
					: repeat->count.minimum;

		if (minimum > v->limits->max_repetitions)
			return report(v, VMF_LIMIT_EXCEEDED, node->id);
	} else if (node->kind == VMF_NODE_CHOICE) {
		const vmf_choice_schema *choice = &node->as.choice;

		if (!dependency(v, index, choice->selector, false) ||
			(choice->case_count && !choice->cases) ||
			(!choice->case_count && !choice->otherwise))
			return report(v, VMF_INVALID_SCHEMA, node->id);
		if (choice->case_count > v->limits->max_schema_nodes - v->count)
			return report(v, VMF_LIMIT_EXCEEDED, node->id);
		for (size_t i = 0; i < choice->case_count; ++i) {
			const vmf_choice_case *arm = &choice->cases[i];

			if (!arm->value_count || !arm->body ||
				arm->body->presence.kind !=
					VMF_PRESENCE_ALWAYS ||
				!ranges_valid(arm->values, arm->value_count,
					UINT64_MAX))
				return report(v, VMF_INVALID_SCHEMA, node->id);
			for (size_t j = 0; j < i; ++j)
				if (ranges_overlap(arm, &choice->cases[j]))
					return report(v, VMF_INVALID_SCHEMA,
						node->id);
		}
		if (choice->otherwise) {
			if (choice->otherwise->presence.kind !=
				VMF_PRESENCE_ALWAYS)
				return report(v, VMF_INVALID_SCHEMA, node->id);
		}
	}
	return VMF_OK;
}

static const vmf_node *next_child(entry *item)
{
	const vmf_node *node = item->node;

	if (node->kind == VMF_NODE_GROUP) {
		if (item->child < node->as.group.child_count)
			return &node->as.group.children[item->child++];
	} else if (node->kind == VMF_NODE_REPEAT) {
		if (!item->child++)
			return node->as.repeat.body;
	} else if (node->kind == VMF_NODE_CHOICE) {
		if (item->child < node->as.choice.case_count)
			return node->as.choice.cases[item->child++].body;
		if (item->child++ == node->as.choice.case_count)
			return node->as.choice.otherwise;
	}
	return nullptr;
}

static vmf_status add(
	validation *v, const vmf_node *node, size_t parent, size_t depth)
{
	if (!node || !node->id)
		return report(v, VMF_INVALID_SCHEMA, node ? node->id : 0);
	for (size_t i = 0; i < v->count; ++i)
		if (v->entries[i].node == node ||
			v->entries[i].node->id == node->id)
			return report(v, VMF_INVALID_SCHEMA, node->id);
	if (v->count == v->limits->max_schema_nodes ||
		depth > v->limits->max_depth)
		return report(v, VMF_LIMIT_EXCEEDED, node->id);
	size_t index = v->count++;

	v->entries[index] = {node, parent, depth, 0, 0};
	if (node->presence.kind < VMF_PRESENCE_ALWAYS ||
		node->presence.kind > VMF_PRESENCE_CONDITION ||
		(node->presence.kind == VMF_PRESENCE_CONDITION &&
			!condition_valid(v, index, &node->presence.condition)))
		return report(v, VMF_INVALID_SCHEMA, node->id);
	if (node->kind < VMF_NODE_FIELD || node->kind > VMF_NODE_CHOICE)
		return report(v, VMF_INVALID_SCHEMA, node->id);
	if (node->kind != VMF_NODE_FIELD)
		return children(v, node, index);
	const vmf_field_schema *field = &node->as.field;

	if (field->kind < VMF_FIELD_BOOL || field->kind > VMF_FIELD_BITS)
		return report(v, VMF_INVALID_SCHEMA, node->id);
	if (scalar_kind(field->kind)) {
		const vmf_scalar_definition *scalar = &field->as.scalar;
		const vmf_integer_encoding *encoding = &scalar->encoding;
		vmf_integer_representation representation =
			encoding->representation;

		if (!encoding_valid(encoding) ||
			(field->kind == VMF_FIELD_BOOL &&
				(encoding->bit_width != 1 ||
				representation != VMF_INTEGER_UNSIGNED)) ||
			((field->kind == VMF_FIELD_UNSIGNED ||
				field->kind == VMF_FIELD_ENUM) &&
				representation != VMF_INTEGER_UNSIGNED) ||
			(field->kind == VMF_FIELD_SIGNED &&
				representation ==
					VMF_INTEGER_UNSIGNED))
			return report(v, VMF_INVALID_SCHEMA, node->id);
		uint64_t maximum = maximum_code(encoding->bit_width);

		if (!ranges_valid(
			scalar->allowed, scalar->allowed_count, maximum) ||
			!labels_valid(
				scalar->labels, scalar->label_count, maximum))
			return report(v, VMF_INVALID_SCHEMA, node->id);
		if (field->kind == VMF_FIELD_SCALED &&
			(!scalar->scale.factor.denominator ||
				!scalar->scale.bias.denominator ||
				!labels_valid(scalar->scale.sentinels,
					scalar->scale.sentinel_count, maximum)))
			return report(v, VMF_INVALID_SCHEMA, node->id);
		if (encoding->bit_width > v->limits->max_field_bits)
			return report(v, VMF_LIMIT_EXCEEDED, node->id);
		return VMF_OK;
	}
	const vmf_extent_definition *extent;
	bool literal = false;

	if (field->kind == VMF_FIELD_TEXT) {
		if (field->as.text.encoding < VMF_TEXT_ASCII_7 ||
			field->as.text.encoding > VMF_TEXT_UTF_8 ||
			!order_valid(field->as.text.order))
			return report(v, VMF_INVALID_SCHEMA, node->id);
		extent = &field->as.text.length;
		literal = field->as.text.encoding == VMF_TEXT_ASCII_7;
	} else if (field->kind == VMF_FIELD_OCTETS) {
		if (!order_valid(field->as.octets.order))
			return report(v, VMF_INVALID_SCHEMA, node->id);
		extent = &field->as.octets.length;
	} else {
		extent = &field->as.bits.length;
	}
	if (!extent_valid(v, index, extent, false, literal))
		return report(v, VMF_INVALID_SCHEMA, node->id);
	uint64_t minimum = extent->kind == VMF_EXTENT_FIXED ? extent->fixed
							: extent->minimum;
	unsigned width = field->kind == VMF_FIELD_BITS ? 1 : literal ? 7 : 8;

	if (minimum > v->limits->max_field_bits / width)
		return report(v, VMF_LIMIT_EXCEEDED, node->id);
	if (extent->kind == VMF_EXTENT_REMAINING) {
		if ((field->kind == VMF_FIELD_BITS || literal) &&
			extent->minimum != extent->maximum)
			return report(
				v, VMF_UNSUPPORTED_SCHEMA_FEATURE, node->id);
		size_t branch = index;

		while (v->entries[branch].parent != SIZE_MAX) {
			size_t ancestor = v->entries[branch].parent;
			const vmf_node *up = v->entries[ancestor].node;

			if (up->kind == VMF_NODE_REPEAT)
				return report(v, VMF_UNSUPPORTED_SCHEMA_FEATURE,
					node->id);
			if (up->kind == VMF_NODE_GROUP &&
				up->as.group.child_count &&
				&up->as.group.children[up->as.group
							.child_count -
						1] !=
					v->entries[branch].node)
				return report(v, VMF_UNSUPPORTED_SCHEMA_FEATURE,
					node->id);
			branch = ancestor;
		}
	}
	return VMF_OK;
}

static uint64_t multiply(uint64_t a, uint64_t b)
{
	return b && a > UINT64_MAX / b ? UINT64_MAX : a * b;
}

static uint64_t sum(uint64_t a, uint64_t b)
{
	return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}

static void minimums(validation *v)
{
	for (size_t i = v->count; i-- > 0;) {
		entry *item = &v->entries[i];
		const vmf_node *node = item->node;
		uint64_t bits = 0;

		if (node->kind == VMF_NODE_FIELD) {
			const vmf_field_schema *field = &node->as.field;

			if (scalar_kind(field->kind)) {
				bits = field->as.scalar.encoding.bit_width;
			} else {
				const vmf_extent_definition *extent =
					field->kind == VMF_FIELD_TEXT
						? &field->as.text.length
					: field->kind == VMF_FIELD_OCTETS
						? &field->as.octets.length
						: &field->as.bits.length;
				unsigned width = 8;
				bool ascii7 = field->kind == VMF_FIELD_TEXT &&
					field->as.text.encoding ==
						VMF_TEXT_ASCII_7;

				if (field->kind == VMF_FIELD_BITS)
					width = 1;
				else if (ascii7)
					width = 7;

				bits = multiply(extent->kind == VMF_EXTENT_FIXED
							? extent->fixed
							: extent->minimum,
					width);
				if (extent->kind == VMF_EXTENT_PREFIX)
					bits = sum(
						bits, extent->prefix.bit_width);
				if (extent->kind == VMF_EXTENT_LITERAL &&
					extent->minimum < extent->maximum)
					bits = sum(bits, 7);
			}
		} else if (node->kind == VMF_NODE_CHOICE) {
			bits = UINT64_MAX;
			for (size_t j = i + 1; j < v->count; ++j)
				if (v->entries[j].parent == i &&
					v->entries[j].minimum_bits < bits)
					bits = v->entries[j].minimum_bits;
		} else {
			for (size_t j = i + 1; j < v->count; ++j)
				if (v->entries[j].parent == i) {
					uint64_t child =
						v->entries[j].minimum_bits;

					bits = sum(bits, child);
				}
			if (node->kind == VMF_NODE_REPEAT) {
				const vmf_extent_definition *count =
					&node->as.repeat.count;

				uint64_t repetitions =
					count->kind == VMF_EXTENT_FIXED
						? count->fixed
						: count->minimum;

				if (count->kind >= VMF_EXTENT_MORE_BEFORE)
					bits = sum(bits, 1);
				bits = multiply(bits, repetitions);
				if (count->kind == VMF_EXTENT_PREFIX)
					bits = sum(
						bits, count->prefix.bit_width);
				if (count->kind == VMF_EXTENT_MORE_BEFORE)
					bits = sum(bits, 1);
			}
		}
		item->minimum_bits =
			node->presence.kind == VMF_PRESENCE_BIT		? 1
			: node->presence.kind == VMF_PRESENCE_CONDITION ? 0
									: bits;
	}
}

} /* namespace */

static vmf_status validate_allocated(const vmf_runtime_schema *schema,
	const vmf_limits *limits, const vmf_allocator *allocator,
	vmf_error *error)
{
	if (error)
		*error = {VMF_OK, 0, 0, SIZE_MAX, SIZE_MAX,
			VMF_PROPERTY_NONE, SIZE_MAX, 0};
	if (!schema || !limits) {
		if (error)
			error->status = VMF_INVALID_ARGUMENT;
		return VMF_INVALID_ARGUMENT;
	}
	validation v = {nullptr, 0, limits, error};

	if (!schema->root || schema->root->kind != VMF_NODE_GROUP ||
		schema->root->presence.kind != VMF_PRESENCE_ALWAYS ||
		schema->padding_bit != 0 || !order_valid(schema->octet_order) ||
		schema->id.user_message_format > 15 ||
		(schema->id.standard_version.present &&
			schema->id.standard_version.value > 15) ||
		(!schema->id.identification_present &&
			schema->id.subtype.present) ||
		(schema->id.identification_present &&
			(schema->id.functional_area > 15 ||
				!schema->id.message_number ||
				schema->id.message_number > 127 ||
				(schema->id.subtype.present &&
					schema->id.subtype.value > 127))))
		return report(&v, VMF_INVALID_SCHEMA, 0);
	if (!limits->max_schema_nodes ||
		limits->max_schema_nodes > SIZE_MAX / sizeof(entry))
		return report(&v, VMF_LIMIT_EXCEEDED, 0);
	v.entries = static_cast<entry *>(
		allocator ? allocator->allocate(allocator->user,
				limits->max_schema_nodes * sizeof(entry))
			: std::malloc(
				limits->max_schema_nodes * sizeof(entry)));
	if (!v.entries)
		return report(&v, VMF_OUT_OF_MEMORY, 0);
	vmf_status status;
	size_t alignment = alignof(std::max_align_t);
	if (reinterpret_cast<uintptr_t>(v.entries) % alignment) {
		status = report(&v, VMF_INVALID_ARGUMENT, 0);
	} else {
		status = add(&v, schema->root, SIZE_MAX, 0);
		size_t index = 0;

		while (status == VMF_OK) {
			const vmf_node *child = next_child(&v.entries[index]);

			if (child) {
				status = add(&v, child, index,
					v.entries[index].depth + 1);
				index = v.count - 1;
			} else {
				index = v.entries[index].parent;
				if (index == SIZE_MAX)
					break;
			}
		}
	}

	if (status == VMF_OK) {
		minimums(&v);
		for (size_t i = 0; i < v.count; ++i) {
			const vmf_node *node = v.entries[i].node;

			if (node->kind != VMF_NODE_REPEAT)
				continue;
			const vmf_extent_definition *count =
				&node->as.repeat.count;
			bool nonempty = count->kind == VMF_EXTENT_FIXED
						? count->fixed != 0
						: count->maximum != 0;
			if (nonempty && count->kind < VMF_EXTENT_MORE_BEFORE &&
				!v.entries[i + 1].minimum_bits) {
				status = report(
					&v, VMF_INVALID_SCHEMA, node->id);
				break;
			}
		}
	}
	if (allocator)
		allocator->deallocate(allocator->user, v.entries);
	else
		std::free(v.entries);
	return status;
}

vmf_status runtime_set_validate(const vmf_runtime_set *set,
	const vmf_limits *limits, const vmf_allocator *allocator,
	vmf_error *error)
{
	if (error)
		*error = {VMF_OK, 0, 0, SIZE_MAX, SIZE_MAX,
			VMF_PROPERTY_NONE, SIZE_MAX, 0};
	if (!set || !limits || (set->count && !set->schemas)) {
		if (error)
			error->status = VMF_INVALID_ARGUMENT;
		return VMF_INVALID_ARGUMENT;
	}
	if (set->count > limits->max_schemas) {
		if (error)
			error->status = VMF_LIMIT_EXCEEDED;
		return VMF_LIMIT_EXCEEDED;
	}
	for (size_t i = 0; i < set->count; ++i) {
		vmf_status status = validate_allocated(
			set->schemas[i], limits, allocator, error);

		if (status != VMF_OK)
			return status;
		for (size_t j = 0; j < i; ++j)
			if (vmf_message_id_equal(&set->schemas[i]->id,
				&set->schemas[j]->id)) {
				if (error)
					error->status = VMF_DUPLICATE_MESSAGE;
				return VMF_DUPLICATE_MESSAGE;
			}
	}
	return VMF_OK;
}

static const vmf_node *find_node(const vmf_node *node, vmf_node_id id)
{
	/* Walk with explicit pending branches rather than recursive
	* descent, releasing temporary traversal storage on every return.
	*/
	entry local[64];
	entry *stack = local;
	size_t count = 0, capacity = sizeof(local) / sizeof(*local);
	const vmf_node *found = nullptr;
	size_t maximum_capacity = SIZE_MAX / sizeof(entry) / 2;

	while (node) {
		if (node->id == id) {
			found = node;
			break;
		}
		entry item = {node, 0, 0, 0, 0};
		const vmf_node *first = next_child(&item);

		if (first) {
			/* Single-child paths need no pending parent frame. */
			if (next_child(&item)) {
				if (count == capacity) {
					if (capacity > maximum_capacity)
						break;
					auto *grown = static_cast<entry *>(
						std::malloc(capacity * 2 *
							sizeof(entry)));

					if (!grown)
						break;
					std::memcpy(grown, stack,
						count * sizeof(entry));
					if (stack != local)
						std::free(stack);
					stack = grown;
					capacity *= 2;
				}
				item.child = 1;
				stack[count++] = item;
			}
			node = first;
		} else {
			node = nullptr;
			while (count && !node) {
				node = next_child(&stack[count - 1]);
				if (!node)
					--count;
			}
		}
	}
	if (stack != local)
		std::free(stack);
	return found;
}

const vmf_node *runtime_schema_find(
	const vmf_runtime_schema *schema, vmf_node_id id)
{
	return !schema || !schema->root || !id ? nullptr
					: find_node(schema->root, id);
}

vmf_status runtime_schema_validate(const vmf_runtime_schema *schema,
	const vmf_limits *limits, const vmf_allocator *allocator,
		vmf_error *error)
{
	return validate_allocated(schema, limits, allocator, error);
}
