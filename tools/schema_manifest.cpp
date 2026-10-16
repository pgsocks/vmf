#include "cli.hpp"
#include <cstring>

namespace
{

static bool add(yyjson_mut_doc *doc, yyjson_mut_val *object,
	const char *name, yyjson_mut_val *value)
{
	return value && yyjson_mut_obj_add_val(doc, object, name, value);
}

static bool append(yyjson_mut_val *array, yyjson_mut_val *value)
{
	return value && yyjson_mut_arr_append(array, value);
}

static yyjson_mut_val *components(yyjson_mut_doc *doc,
	const uint32_t *parts, size_t count)
{
	yyjson_mut_val *array = yyjson_mut_arr(doc);

	if (!array || (count && !parts))
		return nullptr;
	for (size_t i = 0; i < count; ++i)
		if (!append(array, yyjson_mut_uint(doc, parts[i])))
			return nullptr;
	return array;
}

static const char *scope_name(vmf_scope_kind kind)
{
	switch (kind) {
	case VMF_SCOPE_ROOT:
		return "root";
	case VMF_SCOPE_GROUP:
		return "group";
	case VMF_SCOPE_REPEAT:
		return "repeat";
	case VMF_SCOPE_REPEAT_BODY:
		return "repeat_body";
	default:
		return nullptr;
	}
}

static yyjson_mut_val *ranges(yyjson_mut_doc *doc,
	const vmf_code_range *values, size_t count)
{
	yyjson_mut_val *array = yyjson_mut_arr(doc);
	uint64_t previous = 0;
	bool started = false;

	if (!array)
		return nullptr;
	for (size_t emitted = 0; emitted < count; ++emitted) {
		const vmf_code_range *next = nullptr;

		for (size_t i = 0; i < count; ++i)
			if ((!started || values[i].first > previous) &&
				(!next || values[i].first < next->first))
				next = &values[i];
		if (!next)
			return nullptr;
		yyjson_mut_val *pair = yyjson_mut_arr(doc);

		if (!pair ||
			!append(pair, cli_json_uint(doc, next->first, false)) ||
			!append(pair, cli_json_uint(doc, next->last, false)) ||
			!append(array, pair))
			return nullptr;
		previous = next->first;
		started = true;
	}
	return array;
}

static yyjson_mut_val *condition(yyjson_mut_doc *doc,
	const vmf_condition *when)
{
	if (!when)
		return yyjson_mut_null(doc);
	yyjson_mut_val *object = yyjson_mut_obj(doc);
	yyjson_mut_val *terms = yyjson_mut_arr(doc);

	if (!object || !terms ||
		!add(doc, object, "mode", yyjson_mut_uint(doc, when->mode)) ||
		!add(doc, object, "negate", yyjson_mut_bool(doc, when->negate)))
		return nullptr;
	for (size_t i = 0; i < when->term_count; ++i) {
		const vmf_condition_term *term = &when->terms[i];
		yyjson_mut_val *item = yyjson_mut_obj(doc);

		if (!item || !add(doc, item, "field",
			cli_index_to_json(doc, term->field)) ||
			!add(doc, item, "mask", cli_json_uint(doc,
				term->mask.present ? term->mask.value :
					UINT64_MAX,
				false)) ||
			!add(doc, item, "values",
				ranges(doc, term->values, term->value_count)) ||
			!add(doc, item, "negate",
				yyjson_mut_bool(doc, term->negate)) ||
			!append(terms, item))
			return nullptr;
	}
	return add(doc, object, "terms", terms) ? object : nullptr;
}

static yyjson_mut_val *rational(yyjson_mut_doc *doc, vmf_rational value)
{
	yyjson_mut_val *array = yyjson_mut_arr(doc);

	if (!array || !append(array, cli_json_int(doc, value.numerator)) ||
		!append(array, cli_json_uint(doc, value.denominator, false)))
		return nullptr;
	return array;
}

static yyjson_mut_val *scale(yyjson_mut_doc *doc,
	const vmf_scale_definition *value)
{
	if (!value)
		return yyjson_mut_null(doc);
	yyjson_mut_val *object = yyjson_mut_obj(doc);
	yyjson_mut_val *sentinels = yyjson_mut_arr(doc);
	uint64_t previous = 0;
	bool started = false;

	if (!object || !sentinels)
		return nullptr;
	for (size_t j = 0; j < value->sentinel_count; ++j) {
		uint64_t next = UINT64_MAX;
		bool found = false;

		for (size_t i = 0; i < value->sentinel_count; ++i)
			if ((!started || value->sentinels[i] > previous) &&
				(!found || value->sentinels[i] < next)) {
				next = value->sentinels[i];
				found = true;
			}
		if (!found ||
			!append(sentinels, cli_json_uint(doc, next, false)))
			return nullptr;
		previous = next;
		started = true;
	}
	return add(doc, object, "factor", rational(doc, value->factor)) &&
	       add(doc, object, "bias", rational(doc, value->bias)) &&
	       add(doc, object, "sentinels", sentinels) ? object : nullptr;
}

static yyjson_mut_val *index_definition(yyjson_mut_doc *doc,
	const vmf_schema_set *set, const vmf_message_schema *message,
	const vmf_index_schema *index)
{
	vmf_index_info info = {};

	if (vmf_schema_index_info(set, message, index, &info) != VMF_OK)
		return nullptr;
	yyjson_mut_val *object = yyjson_mut_obj(doc);
	yyjson_mut_val *repeats = yyjson_mut_arr(doc);
	yyjson_mut_val *target = yyjson_mut_null(doc);
	vmf_optional_u64 maximum = {};

	if (!object || !repeats)
		return nullptr;
	for (size_t i = 0; i < index->repeat_code.count; ++i) {
		const vmf_repeat_component *part = &index->repeat_code.parts[i];

		if (!append(repeats, yyjson_mut_uint(doc, part->id)))
			return nullptr;
		if (part->maximum.present)
			maximum = part->maximum;
	}
	if (info.target.kind == VMF_TARGET_INDEX)
		target = cli_index_to_json(doc, info.target.as.index);
	else if (info.target.kind == VMF_TARGET_GROUP)
		target = yyjson_mut_uint(doc, info.target.as.group);
	vmf_code_range constant = {info.constant.value, info.constant.value};

	return add(doc, object, "index_number",
		       cli_index_to_json(doc, index->index_number)) &&
	       add(doc, object, "groups", components(doc,
		       index->group_code.parts, index->group_code.count)) &&
	       add(doc, object, "repeats", repeats) &&
	       add(doc, object, "repeat_maximum", maximum.present ?
		       cli_json_uint(doc, maximum.value, false) :
		       yyjson_mut_null(doc)) &&
	       add(doc, object, "indicator",
		       yyjson_mut_uint(doc, info.indicator)) &&
	       add(doc, object, "target_kind",
		       yyjson_mut_uint(doc, info.target.kind)) &&
	       add(doc, object, "target", target) &&
	       add(doc, object, "kind", yyjson_mut_uint(doc, info.kind)) &&
	       add(doc, object, "bits",
		       cli_json_uint(doc, info.number_of_bits, false)) &&
	       add(doc, object, "order",
		       yyjson_mut_uint(doc, info.integer.order)) &&
	       add(doc, object, "representation",
		       yyjson_mut_uint(doc, info.integer.representation)) &&
	       add(doc, object, "text_encoding",
		       yyjson_mut_uint(doc, info.text_encoding)) &&
	       add(doc, object, "unit_bits",
		       yyjson_mut_uint(doc, info.unit_bits)) &&
	       add(doc, object, "variable",
		       yyjson_mut_bool(doc, info.variable)) &&
	       add(doc, object, "literal",
		       yyjson_mut_bool(doc, info.literal)) &&
	       add(doc, object, "capacity",
		       cli_json_uint(doc, info.capacity_units, false)) &&
	       add(doc, object, "length_from", components(doc,
		       info.length_from.parts, info.length_from.count)) &&
	       add(doc, object, "length_bias",
		       cli_json_int(doc, info.length_bias)) &&
	       add(doc, object, "allowed", ranges(doc,
		       info.constant.present ? &constant : info.allowed,
		       info.constant.present ? 1 : info.allowed_count)) &&
	       add(doc, object, "when", condition(doc, info.when)) &&
	       add(doc, object, "scale", scale(doc, info.scale)) &&
	       add(doc, object, "key", yyjson_mut_strncpy(doc,
		       info.key.data ? info.key.data : "", info.key.size)) ?
	       object : nullptr;
}

static yyjson_mut_val *message_definition(yyjson_mut_doc *doc,
	const vmf_schema_set *set, const vmf_message_schema *message)
{
	vmf_message_id id = {};

	if (vmf_schema_message_id(set, message, &id) != VMF_OK)
		return nullptr;
	yyjson_mut_val *object = yyjson_mut_obj(doc);
	yyjson_mut_val *indexes = yyjson_mut_arr(doc);
	yyjson_mut_val *repeats = yyjson_mut_arr(doc);

	if (!object || !indexes || !repeats)
		return nullptr;
	for (size_t i = 0; i < message->index_count; ++i)
		if (!append(indexes, index_definition(doc, set, message,
			&message->indexes[i])))
			return nullptr;
	uint32_t previous = 0;

	for (size_t j = 0; j < message->repeat_count; ++j) {
		const vmf_repeat_definition *next = nullptr;

		for (size_t i = 0; i < message->repeat_count; ++i)
			if (message->repeats[i].id > previous &&
				(!next || message->repeats[i].id < next->id))
				next = &message->repeats[i];
		if (!next)
			return nullptr;
		yyjson_mut_val *item = yyjson_mut_obj(doc);
		bool fixed = next->kind == VMF_REPEAT_FIXED;

		if (!item ||
			!add(doc, item, "id", yyjson_mut_uint(doc, next->id)) ||
			!add(doc, item, "kind",
				yyjson_mut_uint(doc, next->kind)) ||
			!add(doc, item, "count", fixed ?
				cli_json_uint(doc, next->count, false) :
				yyjson_mut_null(doc)) ||
			!add(doc, item, "count_from",
				fixed ? yyjson_mut_arr(doc) :
				cli_index_to_json(doc, next->count_from)) ||
			!add(doc, item, "bias", cli_json_int(doc,
				!fixed && next->bias.present ?
					next->bias.value : 0)) ||
			!add(doc, item, "minimum", cli_json_uint(doc, fixed ?
				next->count : (next->minimum.present ?
					next->minimum.value : 0), false)) ||
			!add(doc, item, "maximum", cli_json_uint(doc,
				fixed ? next->count : next->maximum, false)) ||
			!append(repeats, item))
			return nullptr;
		previous = next->id;
	}
	vmf_bit_order packing = message->octet_order == VMF_BIT_DEFAULT ?
		VMF_BIT_LSB_FIRST : message->octet_order;

	return add(doc, object, "id", cli_id_to_json(doc, &id)) &&
	       add(doc, object, "packing", yyjson_mut_uint(doc, packing)) &&
	       add(doc, object, "indexes", indexes) &&
	       add(doc, object, "repeats", repeats) ? object : nullptr;
}

} /* namespace */

yyjson_mut_val *cli_index_to_json(yyjson_mut_doc *doc,
	vmf_index_number number)
{
	return number.count ? components(doc, number.parts, number.count) :
		nullptr;
}

bool cli_index_from_json(yyjson_val *value, cli_arena *arena,
	vmf_index_number *number)
{
	if (!yyjson_is_arr(value) || !yyjson_arr_size(value) ||
		yyjson_arr_size(value) > vmf_default_limits().max_depth)
		return false;
	size_t count = yyjson_arr_size(value);
	auto *parts = static_cast<uint32_t *>(
		cli_arena_alloc(arena, count * sizeof(uint32_t)));

	if (!parts)
		return false;
	for (size_t i = 0; i < count; ++i) {
		yyjson_val *part = yyjson_arr_get(value, i);

		if (!yyjson_is_uint(part) || !yyjson_get_uint(part) ||
			yyjson_get_uint(part) > UINT32_MAX)
			return false;
		parts[i] = static_cast<uint32_t>(yyjson_get_uint(part));
	}
	*number = {parts, count};
	return true;
}

yyjson_mut_val *cli_scope_to_json(yyjson_mut_doc *doc, vmf_scope_ref scope)
{
	const char *name = scope_name(scope.kind);
	yyjson_mut_val *object = yyjson_mut_obj(doc);

	return name && object &&
	       add(doc, object, "kind", yyjson_mut_str(doc, name)) &&
	       add(doc, object, "id", yyjson_mut_uint(doc, scope.id)) ?
	       object : nullptr;
}

bool cli_scope_from_json(yyjson_val *value,
	const vmf_message_schema *message, vmf_scope_ref *scope)
{
	if (!message || !yyjson_is_obj(value) || yyjson_obj_size(value) != 2)
		return false;
	yyjson_val *kind = yyjson_obj_get(value, "kind");
	yyjson_val *id = yyjson_obj_get(value, "id");

	if (!yyjson_is_uint(id) || yyjson_get_uint(id) > UINT32_MAX)
		return false;
	for (unsigned i = VMF_SCOPE_ROOT; i <= VMF_SCOPE_REPEAT_BODY; ++i) {
		auto candidate = static_cast<vmf_scope_kind>(i);

		if (!yyjson_equals_str(kind, scope_name(candidate)))
			continue;
		uint32_t number = static_cast<uint32_t>(yyjson_get_uint(id));
		bool found = candidate == VMF_SCOPE_ROOT && number == 0;

		for (size_t j = 0; j < message->index_count && !found; ++j) {
			const vmf_index_schema *index = &message->indexes[j];

			if (candidate == VMF_SCOPE_GROUP) {
				const vmf_group_path *path = &index->group_code;

				for (size_t k = 0; k < path->count; ++k)
					found |= path->parts[k] == number;
			} else if (candidate != VMF_SCOPE_ROOT) {
				const auto *path = &index->repeat_code;

				for (size_t k = 0; k < path->count; ++k)
					found |= path->parts[k].id == number;
			}
		}
		if (!found)
			return false;
		*scope = {candidate, number};
		return true;
	}
	return false;
}

yyjson_mut_doc *cli_metadata_to_json(cli_json *context,
	vmf_envelope_version profile, const vmf_schema_set *schemas)
{
	const char *name = cli_profile_name(profile);
	yyjson_mut_doc *doc = cli_json_new(context);

	if (!doc || !name)
		return nullptr;
	yyjson_mut_val *root = yyjson_mut_obj(doc);
	yyjson_mut_val *definitions = yyjson_mut_arr(doc);
	bool valid = root && definitions &&
		add(doc, root, "format", yyjson_mut_str(doc, "vmf-events")) &&
		add(doc, root, "version", yyjson_mut_uint(doc, 1)) &&
		add(doc, root, "profile", yyjson_mut_str(doc, name));

	for (size_t i = 0; valid && schemas && i < schemas->message_count; ++i)
		valid = append(definitions,
			message_definition(doc, schemas,
				&schemas->messages[i]));
	if (!valid || !add(doc, root, "schemas", definitions)) {
		yyjson_mut_doc_free(doc);
		return nullptr;
	}
	yyjson_mut_doc_set_root(doc, root);
	return doc;
}

vmf_status cli_metadata_from_json(yyjson_val *value,
	const vmf_schema_set *schemas, vmf_envelope_version *profile)
{
	if (!yyjson_is_obj(value) || yyjson_obj_size(value) != 4 ||
		!yyjson_equals_str(yyjson_obj_get(value, "format"),
			"vmf-events") ||
		!yyjson_is_uint(yyjson_obj_get(value, "version")) ||
		yyjson_get_uint(yyjson_obj_get(value, "version")) != 1)
		return VMF_MALFORMED;
	yyjson_val *edition = yyjson_obj_get(value, "profile");
	yyjson_val *definitions = yyjson_obj_get(value, "schemas");
	vmf_envelope_version selected;

	if (!yyjson_is_str(edition) ||
		std::strlen(yyjson_get_str(edition)) !=
			yyjson_get_len(edition) ||
		!cli_profile_parse(yyjson_get_str(edition), &selected) ||
		!yyjson_is_arr(definitions))
		return VMF_MALFORMED;
	if (schemas && schemas->envelope && schemas->envelope != selected)
		return VMF_INVALID_SCHEMA;
	if (yyjson_arr_size(definitions) > vmf_default_limits().max_schemas)
		return VMF_LIMIT_EXCEEDED;
	cli_json context;

	cli_json_init(&context);
	yyjson_mut_doc *expected = cli_json_new(&context);

	if (!expected)
		return VMF_OUT_OF_MEMORY;
	vmf_status status = VMF_OK;

	for (size_t i = 0; i < yyjson_arr_size(definitions); ++i) {
		yyjson_val *definition = yyjson_arr_get(definitions, i);
		vmf_message_id id = {};
		const vmf_message_schema *message = nullptr;

		if (!cli_id_from_json(yyjson_obj_get(definition, "id"), &id)) {
			status = VMF_MALFORMED;
			break;
		}
		for (size_t j = 0; j < i; ++j) {
			vmf_message_id previous = {};

			if (!cli_id_from_json(yyjson_obj_get(
				yyjson_arr_get(definitions, j), "id"),
				&previous) ||
				vmf_message_id_equal(&id, &previous)) {
				status = VMF_MALFORMED;
				break;
			}
		}
		if (status != VMF_OK)
			break;
		status = schemas ? vmf_schema_select(schemas, &id, &message) :
			VMF_UNKNOWN_MESSAGE;
		if (status != VMF_OK)
			break;
		yyjson_mut_val *canonical = message_definition(expected,
			schemas,
			message);
		yyjson_mut_val *recorded =
			yyjson_val_mut_copy(expected, definition);

		if (!canonical || !recorded) {
			status = VMF_OUT_OF_MEMORY;
			break;
		}
		if (!yyjson_mut_equals(canonical, recorded)) {
			status = VMF_INVALID_SCHEMA;
			break;
		}
	}
	yyjson_mut_doc_free(expected);
	if (status == VMF_OK)
		*profile = selected;
	return status;
}
