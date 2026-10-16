#include "cli.hpp"
#include <cstdlib>
#include <cstring>

namespace
{

static const char *const event_names[] = {
	"ENVELOPE", "MESSAGE_BEGIN", "FIELD", "GROUP_BEGIN", "GROUP_END",
	"ABSENT", "MESSAGE_END", "PDU_END"
};

static const char *const field_names[] = {
	"bool", "unsigned", "signed", "enum", "scaled", "text", "octets",
	"bits"
};

static bool equal(yyjson_val *value, const char *text)
{
	size_t size = std::strlen(text);

	return yyjson_is_str(value) && yyjson_get_len(value) == size &&
	       !std::memcmp(yyjson_get_str(value), text, size);
}

static bool members(yyjson_val *object, const char *const *names,
	size_t count)
{
	if (!yyjson_is_obj(object) || count > 64)
		return false;
	uint64_t seen = 0;
	yyjson_obj_iter iterator = yyjson_obj_iter_with(object);
	yyjson_val *key;

	while ((key = yyjson_obj_iter_next(&iterator))) {
		size_t i = 0;

		for (; i < count; ++i) {
			if (equal(key, names[i]))
				break;
		}
		if (i == count || (seen & (UINT64_C(1) << i)))
			return false;
		seen |= UINT64_C(1) << i;
	}
	return true;
}

static bool number(yyjson_val *value, uint64_t maximum, uint64_t *result)
{
	if (!yyjson_is_uint(value) || yyjson_get_uint(value) > maximum)
		return false;
	*result = yyjson_get_uint(value);
	return true;
}

static bool boolean(yyjson_val *value, bool *result)
{
	if (!yyjson_is_bool(value))
		return false;
	*result = yyjson_get_bool(value);
	return true;
}

static bool annotations(yyjson_val *object)
{
	const char *keys[] = {"name", "schema_name"};

	for (const char *key : keys) {
		yyjson_val *value = yyjson_obj_get(object, key);

		if (value && !yyjson_is_str(value))
			return false;
	}
	return true;
}

static bool add(yyjson_mut_doc *doc, yyjson_mut_val *object,
	const char *name, yyjson_mut_val *value)
{
	return value && yyjson_mut_obj_add_val(doc, object, name, value);
}

static yyjson_mut_val *hex(yyjson_mut_doc *document, const uint8_t *data,
	size_t size)
{
	if ((size && !data) || size > (16 * 1024 * 1024 - 1) / 2)
		return nullptr;
	char *text = static_cast<char *>(std::malloc(size * 2 + 1));

	if (!text)
		return nullptr;
	const char digits[] = "0123456789abcdef";

	for (size_t i = 0; i < size; ++i) {
		text[i * 2] = digits[data[i] >> 4];
		text[i * 2 + 1] = digits[data[i] & 15];
	}
	text[size * 2] = '\0';
	yyjson_mut_val *result =
		yyjson_mut_strncpy(document, text, size * 2);

	std::free(text);
	return result;
}

static int digit(char value)
{
	if (value >= '0' && value <= '9')
		return value - '0';
	if (value >= 'a' && value <= 'f')
		return value - 'a' + 10;
	if (value >= 'A' && value <= 'F')
		return value - 'A' + 10;
	return -1;
}

static bool bytes(yyjson_val *value, cli_arena *arena,
	const uint8_t **result, size_t *size)
{
	if (!yyjson_is_str(value) || (yyjson_get_len(value) & 1))
		return false;
	const char *text = yyjson_get_str(value);
	size_t length = yyjson_get_len(value) / 2;

	for (size_t i = 0; i < length * 2; ++i) {
		if (digit(text[i]) < 0)
			return false;
	}
	auto *data = length ? static_cast<uint8_t *>(
		cli_arena_alloc(arena, length)) : nullptr;

	if (length && !data)
		return false;
	for (size_t i = 0; i < length; ++i)
		data[i] = (digit(text[i * 2]) << 4) | digit(text[i * 2 + 1]);
	*result = data;
	*size = length;
	return true;
}

static yyjson_mut_val *field_to_json(yyjson_mut_doc *doc,
	const vmf_node *node, const vmf_field *field)
{
	unsigned int kind = node->as.field.kind;

	if (kind >= sizeof(field_names) / sizeof(*field_names))
		return nullptr;
	yyjson_mut_val *result = yyjson_mut_obj(doc);

	if (!result || !add(doc, result, "kind",
		yyjson_mut_str(doc, field_names[kind])))
		return nullptr;
	switch (kind) {
	case VMF_FIELD_BOOL:
		if (!add(doc, result, "value",
			yyjson_mut_bool(doc, field->boolean.value)))
			return nullptr;
		break;
	case VMF_FIELD_UNSIGNED:
		if (!add(doc, result, "value",
			cli_json_uint(doc, field->unsigned_integer.value, false)))
			return nullptr;
		break;
	case VMF_FIELD_SIGNED:
		if (!add(doc, result, "value",
			cli_json_int(doc, field->signed_integer.value)))
			return nullptr;
		break;
	case VMF_FIELD_ENUM:
	case VMF_FIELD_SCALED: {
		uint64_t code = kind == VMF_FIELD_ENUM ?
			field->enumeration.code : field->scaled.code;

		if (!add(doc, result, "code", cli_json_uint(doc, code, false)))
			return nullptr;
		break;
	}
	case VMF_FIELD_TEXT:
		if ((field->text.size && !field->text.data) ||
			!add(doc, result, "data", yyjson_mut_strncpy(doc,
				field->text.data ? field->text.data : "",
				field->text.size)) ||
			!add(doc, result, "last",
				yyjson_mut_bool(doc, field->text.last)))
			return nullptr;
		break;
	case VMF_FIELD_OCTETS:
		if (!add(doc, result, "hex", hex(doc, field->octets.data,
			field->octets.size)) ||
			!add(doc, result, "last",
				yyjson_mut_bool(doc, field->octets.last)))
			return nullptr;
		break;
	case VMF_FIELD_BITS:
		if (field->bits.bit_count > SIZE_MAX - 7 ||
			!add(doc, result, "hex", hex(doc, field->bits.data,
				(field->bits.bit_count + 7) / 8)) ||
			!add(doc, result, "bit_count",
				cli_json_uint(doc, field->bits.bit_count, false)) ||
			!add(doc, result, "last",
				yyjson_mut_bool(doc, field->bits.last)))
			return nullptr;
		break;
	}
	return result;
}

static vmf_status field_from_json(yyjson_val *object, const vmf_node *node,
	cli_arena *arena, vmf_field *field)
{
	const char *const scalar[] = {"kind", "value"};
	const char *const code[] = {"kind", "code"};
	const char *const text[] = {"kind", "data", "last"};
	const char *const octets[] = {"kind", "hex", "last"};
	const char *const bits[] = {"kind", "hex", "bit_count", "last"};
	unsigned int kind = node->as.field.kind;

	if (kind >= sizeof(field_names) / sizeof(*field_names))
		return VMF_INVALID_SCHEMA;
	if (!equal(yyjson_obj_get(object, "kind"), field_names[kind]))
		return VMF_TYPE_MISMATCH;
	yyjson_val *value = yyjson_obj_get(object, "value");
	bool valid = false;

	switch (kind) {
	case VMF_FIELD_BOOL:
		valid = members(object, scalar, 2) &&
			boolean(value, &field->boolean.value);
		break;
	case VMF_FIELD_UNSIGNED:
		valid = members(object, scalar, 2) &&
			cli_json_u64(value, &field->unsigned_integer.value, false);
		break;
	case VMF_FIELD_SIGNED:
		valid = members(object, scalar, 2) &&
			cli_json_i64(value, &field->signed_integer.value);
		break;
	case VMF_FIELD_ENUM:
	case VMF_FIELD_SCALED: {
		uint64_t raw;

		valid = members(object, code, 2) &&
			cli_json_u64(yyjson_obj_get(object, "code"), &raw, false);
		if (valid) {
			if (kind == VMF_FIELD_ENUM)
				field->enumeration.code = raw;
			else
				field->scaled.code = raw;
		}
		break;
	}
	case VMF_FIELD_TEXT: {
		value = yyjson_obj_get(object, "data");
		if (!members(object, text, 3) || !yyjson_is_str(value) ||
			!boolean(yyjson_obj_get(object, "last"), &field->text.last))
			break;
		size_t size = yyjson_get_len(value);
		auto *data = static_cast<char *>(cli_arena_alloc(arena, size + 1));

		if (!data)
			return VMF_OUT_OF_MEMORY;
		std::memcpy(data, yyjson_get_str(value), size);
		data[size] = '\0';
		field->text.data = data;
		field->text.size = size;
		valid = true;
		break;
	}
	case VMF_FIELD_OCTETS:
		valid = members(object, octets, 3) &&
			boolean(yyjson_obj_get(object, "last"),
				&field->octets.last) &&
			bytes(yyjson_obj_get(object, "hex"), arena,
				&field->octets.data, &field->octets.size);
		break;
	case VMF_FIELD_BITS: {
		uint64_t count;
		size_t size;

		valid = members(object, bits, 4) &&
			boolean(yyjson_obj_get(object, "last"), &field->bits.last) &&
			cli_json_u64(yyjson_obj_get(object, "bit_count"),
				&count, false) && count <= SIZE_MAX - 7 &&
			bytes(yyjson_obj_get(object, "hex"), arena,
				&field->bits.data, &size) && size == (count + 7) / 8;
		if (!valid)
			break;
		field->bits.bit_count = static_cast<size_t>(count);
		if (count % 8 && (field->bits.data[size - 1] >> (count % 8)))
			valid = false;
		break;
	}
	}
	return valid ? VMF_OK : VMF_MALFORMED;
}

} /* namespace */

const char *cli_event_name(vmf_event_kind kind)
{
	unsigned int index = kind;

	return index < sizeof(event_names) / sizeof(*event_names) ?
		event_names[index] : nullptr;
}

const char *cli_profile_name(vmf_envelope_version profile)
{
	switch (profile) {
	case VMF_ENVELOPE_B:
		return "B";
	case VMF_ENVELOPE_C:
		return "C";
	case VMF_ENVELOPE_D:
		return "D";
	case VMF_ENVELOPE_D1:
		return "D1";
	}
	return nullptr;
}

bool cli_profile_parse(const char *text, vmf_envelope_version *profile)
{
	if (!text || !profile)
		return false;
	for (unsigned int i = VMF_ENVELOPE_B; i <= VMF_ENVELOPE_D1; ++i) {
		auto candidate = static_cast<vmf_envelope_version>(i);

		if (!std::strcmp(text, cli_profile_name(candidate))) {
			*profile = candidate;
			return true;
		}
	}
	return false;
}

yyjson_mut_doc *cli_event_to_json(cli_json *context,
	const vmf_event *event, vmf_envelope_version profile, uint64_t time_ns)
{
	if (!context || !event || !cli_profile_name(profile) ||
		!cli_event_name(event->kind))
		return nullptr;
	yyjson_mut_doc *doc = cli_json_new(context);

	if (!doc)
		return nullptr;
	yyjson_mut_val *root = yyjson_mut_obj(doc);
	bool valid = root && add(doc, root, "event",
		yyjson_mut_str(doc, cli_event_name(event->kind)));
	bool body = event->kind == VMF_EVENT_FIELD ||
		event->kind == VMF_EVENT_GROUP_BEGIN ||
		event->kind == VMF_EVENT_GROUP_END ||
		event->kind == VMF_EVENT_ABSENT;

	if (body) {
		valid = valid && event->node &&
			add(doc, root, "node_id",
				yyjson_mut_uint(doc, event->node->id)) &&
			add(doc, root, "depth", yyjson_mut_uint(doc, event->depth)) &&
			add(doc, root, "occurrence",
				cli_json_uint(doc, event->occurrence, false));
		if (valid && event->node->name)
			valid = add(doc, root, "name",
				yyjson_mut_strcpy(doc, event->node->name));
	}
	if (valid && event->schema && event->schema->name)
		valid = add(doc, root, "schema_name",
			yyjson_mut_strcpy(doc, event->schema->name));
	if (valid) {
		switch (event->kind) {
		case VMF_EVENT_ENVELOPE: {
			const auto *envelope = &event->as.envelope;

			valid = add(doc, root, "time_ns",
				cli_json_uint(doc, time_ns, false)) &&
				add(doc, root, "wire_version",
					yyjson_mut_uint(doc, envelope->wire_version)) &&
				add(doc, root, "header_octets", cli_json_uint(doc,
					envelope->header_octets, true)) &&
				add(doc, root, "pdu_octets", cli_json_uint(doc,
					envelope->pdu_octets, true)) &&
				add(doc, root, "header", cli_header_to_json(doc,
					profile, envelope->header));
			break;
		}
		case VMF_EVENT_MESSAGE_BEGIN:
		case VMF_EVENT_MESSAGE_END:
			valid = add(doc, root, "id",
				cli_id_to_json(doc, &event->as.message.id)) &&
				add(doc, root, "index",
					yyjson_mut_uint(doc, event->as.message.index)) &&
				add(doc, root, "payload_octets", cli_json_uint(doc,
					event->as.message.payload_octets, true));
			break;
		case VMF_EVENT_FIELD:
			valid = event->node->kind == VMF_NODE_FIELD &&
				add(doc, root, "offset_units", cli_json_uint(doc,
					event->as.field.offset_units, true)) &&
				add(doc, root, "total_units", cli_json_uint(doc,
					event->as.field.total_units, true)) &&
				add(doc, root, "value", field_to_json(doc,
					event->node, &event->as.field.value));
			break;
		case VMF_EVENT_GROUP_BEGIN:
		case VMF_EVENT_GROUP_END:
			valid = event->node->kind != VMF_NODE_FIELD &&
				add(doc, root, "count", cli_json_uint(doc,
					event->as.group.count, true)) &&
				add(doc, root, "more_present", yyjson_mut_bool(doc,
					event->as.group.more_present)) &&
				add(doc, root, "more",
					yyjson_mut_bool(doc, event->as.group.more));
			break;
		case VMF_EVENT_ABSENT:
		case VMF_EVENT_PDU_END:
			break;
		}
	}
	if (!valid) {
		yyjson_mut_doc_free(doc);
		return nullptr;
	}
	yyjson_mut_doc_set_root(doc, root);
	return doc;
}

vmf_status cli_event_from_json(yyjson_val *value,
	vmf_envelope_version profile, const vmf_schema_set *schemas,
	const vmf_message_schema **active, cli_arena *arena,
	vmf_event *event, uint64_t *time_ns)
{
	if (!value || !schemas || !active || !arena || !event || !time_ns ||
		!cli_profile_name(profile))
		return VMF_INVALID_ARGUMENT;
	vmf_event candidate = {};
	unsigned int kind = 0;

	for (; kind < sizeof(event_names) / sizeof(*event_names); ++kind) {
		if (equal(yyjson_obj_get(value, "event"), event_names[kind]))
			break;
	}
	if (kind == sizeof(event_names) / sizeof(*event_names) ||
		!annotations(value))
		return VMF_MALFORMED;
	candidate.kind = static_cast<vmf_event_kind>(kind);
	const char *const envelope[] = {"event", "time_ns", "wire_version",
		"header_octets", "pdu_octets", "header"};
	const char *const message[] = {"event", "id", "index",
		"payload_octets", "schema_name"};
	const char *const field[] = {"event", "node_id", "depth",
		"occurrence", "offset_units", "total_units", "value", "name",
		"schema_name"};
	const char *const group[] = {"event", "node_id", "depth",
		"occurrence", "count", "more_present", "more", "name",
		"schema_name"};
	const char *const absent[] = {"event", "node_id", "depth",
		"occurrence", "name", "schema_name"};
	const char *const end[] = {"event"};
	const char *const *keys = nullptr;
	size_t key_count = 0;

	switch (candidate.kind) {
	case VMF_EVENT_ENVELOPE:
		keys = envelope;
		key_count = sizeof(envelope) / sizeof(*envelope);
		break;
	case VMF_EVENT_MESSAGE_BEGIN:
	case VMF_EVENT_MESSAGE_END:
		keys = message;
		key_count = sizeof(message) / sizeof(*message);
		break;
	case VMF_EVENT_FIELD:
		keys = field;
		key_count = sizeof(field) / sizeof(*field);
		break;
	case VMF_EVENT_GROUP_BEGIN:
	case VMF_EVENT_GROUP_END:
		keys = group;
		key_count = sizeof(group) / sizeof(*group);
		break;
	case VMF_EVENT_ABSENT:
		keys = absent;
		key_count = sizeof(absent) / sizeof(*absent);
		break;
	case VMF_EVENT_PDU_END:
		keys = end;
		key_count = sizeof(end) / sizeof(*end);
		break;
	}
	if (!members(value, keys, key_count))
		return VMF_MALFORMED;
	bool body = candidate.kind == VMF_EVENT_FIELD ||
		candidate.kind == VMF_EVENT_GROUP_BEGIN ||
		candidate.kind == VMF_EVENT_GROUP_END ||
		candidate.kind == VMF_EVENT_ABSENT;
	uint64_t numeric = 0;

	if (body) {
		if (!number(yyjson_obj_get(value, "node_id"), UINT32_MAX,
			&numeric) || !numeric)
			return VMF_MALFORMED;
		candidate.node = vmf_schema_find(*active,
			static_cast<vmf_node_id>(numeric));
		if (!candidate.node)
			return VMF_INVALID_SCHEMA;
		candidate.schema = *active;
		if (!number(yyjson_obj_get(value, "depth"), SIZE_MAX, &numeric) ||
			!cli_json_u64(yyjson_obj_get(value, "occurrence"),
				&candidate.occurrence, false))
			return VMF_MALFORMED;
		candidate.depth = static_cast<size_t>(numeric);
	}
	uint64_t timestamp = 0;
	bool valid = true;

	switch (candidate.kind) {
	case VMF_EVENT_ENVELOPE: {
		if (!number(yyjson_obj_get(value, "wire_version"), UINT8_MAX,
			&numeric) || !cli_json_u64(yyjson_obj_get(value, "time_ns"),
				&timestamp, false) ||
			!cli_json_u64(yyjson_obj_get(value, "header_octets"),
				&candidate.as.envelope.header_octets, true) ||
			!cli_json_u64(yyjson_obj_get(value, "pdu_octets"),
				&candidate.as.envelope.pdu_octets, true))
			return VMF_MALFORMED;
		auto *header = static_cast<vmf_header *>(
			cli_arena_alloc(arena, sizeof(vmf_header)));

		if (!header)
			return VMF_OUT_OF_MEMORY;
		std::memset(header, 0, sizeof(*header));
		if (!cli_header_from_json(yyjson_obj_get(value, "header"),
			profile, arena, header))
			return VMF_MALFORMED;
		candidate.as.envelope.wire_version = static_cast<uint8_t>(numeric);
		candidate.as.envelope.header = header;
		candidate.as.envelope.object_size = vmf_header_layout(profile).size;
		break;
	}
	case VMF_EVENT_MESSAGE_BEGIN:
	case VMF_EVENT_MESSAGE_END: {
		if (!cli_id_from_json(yyjson_obj_get(value, "id"),
			&candidate.as.message.id) ||
			!number(yyjson_obj_get(value, "index"), SIZE_MAX, &numeric) ||
			!cli_json_u64(yyjson_obj_get(value, "payload_octets"),
				&candidate.as.message.payload_octets, true))
			return VMF_MALFORMED;
		candidate.as.message.index = static_cast<size_t>(numeric);
		vmf_status status = vmf_schema_select(schemas,
			&candidate.as.message.id, &candidate.schema);

		if (status != VMF_OK)
			return status;
		break;
	}
	case VMF_EVENT_FIELD: {
		if (candidate.node->kind != VMF_NODE_FIELD)
			return VMF_TYPE_MISMATCH;
		if (!cli_json_u64(yyjson_obj_get(value, "offset_units"),
			&candidate.as.field.offset_units, true) ||
			!cli_json_u64(yyjson_obj_get(value, "total_units"),
				&candidate.as.field.total_units, true))
			return VMF_MALFORMED;
		vmf_status status = field_from_json(yyjson_obj_get(value, "value"),
			candidate.node, arena, &candidate.as.field.value);

		if (status != VMF_OK)
			return status;
		break;
	}
	case VMF_EVENT_GROUP_BEGIN:
	case VMF_EVENT_GROUP_END:
		if (candidate.node->kind == VMF_NODE_FIELD)
			return VMF_TYPE_MISMATCH;
		valid = cli_json_u64(yyjson_obj_get(value, "count"),
			&candidate.as.group.count, true) &&
			boolean(yyjson_obj_get(value, "more_present"),
				&candidate.as.group.more_present) &&
			boolean(yyjson_obj_get(value, "more"),
				&candidate.as.group.more);
		break;
	case VMF_EVENT_ABSENT:
	case VMF_EVENT_PDU_END:
		break;
	}
	if (!valid)
		return VMF_MALFORMED;
	if (candidate.kind == VMF_EVENT_MESSAGE_BEGIN)
		*active = candidate.schema;
	if (candidate.kind == VMF_EVENT_ENVELOPE)
		*time_ns = timestamp;
	*event = candidate;
	return VMF_OK;
}

bool cli_schedule(cli_clock *clock, uint64_t timestamp, bool timed)
{
	if (!clock || (timed && (!clock->now || !clock->sleep_until)))
		return false;
	if (!clock->started) {
		clock->origin = clock->now ? clock->now(clock->user) : 0;
		clock->first = timestamp;
		clock->previous = timestamp;
		clock->started = true;
		return true;
	}
	if (timestamp < clock->previous)
		return false;
	uint64_t difference = timestamp - clock->first;

	if (timed && difference > UINT64_MAX - clock->origin)
		return false;
	if (timed && !clock->sleep_until(clock->user,
		clock->origin + difference))
		return false;
	clock->previous = timestamp;
	return true;
}
