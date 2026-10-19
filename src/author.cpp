#include <vmf/author.h>
#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace
{
bool message_member(const vmf_schema_set *set, const vmf_message_schema *m)
{
	if (!set || !m)
		return false;
	for (size_t i = 0; i < set->message_count; ++i)
		if (&set->messages[i] == m)
			return true;
	return false;
}

bool index_member(const vmf_message_schema *m, const vmf_index_schema *index)
{
	if (!m || !index)
		return false;
	for (size_t i = 0; i < m->index_count; ++i)
		if (&m->indexes[i] == index)
			return true;
	return false;
}

bool group_member(const vmf_index_schema *index, uint32_t id)
{
	for (size_t i = 0; i < index->group_code.count; ++i)
		if (index->group_code.parts[i] == id)
			return true;
	return false;
}

bool path_prefix(const vmf_repeat_occurrence *a, size_t a_count,
	const vmf_repeat_occurrence *b, size_t b_count)
{
	if (a_count > b_count)
		return false;
	for (size_t i = 0; i < a_count; ++i)
		if (a[i].id != b[i].id || a[i].occurrence != b[i].occurrence)
			return false;
	return true;
}

bool address_equal(const vmf_author_address *a, const vmf_author_address *b)
{
	return a->descriptor_index == b->descriptor_index &&
		a->schema == b->schema && a->index == b->index &&
		a->scope.kind == b->scope.kind && a->scope.id == b->scope.id &&
		a->path_count == b->path_count &&
		path_prefix(a->path, a->path_count, b->path, b->path_count);
}

vmf_status validate_path(const vmf_schema_set *set,
	const vmf_message_schema *m, vmf_repeat_path ancestry,
	const vmf_repeat_occurrence *path, size_t count)
{
	if (count != ancestry.count || (!path && count))
		return VMF_INVALID_ARGUMENT;
	for (size_t i = 0; i < count; ++i) {
		if (path[i].id != ancestry.parts[i].id)
			return VMF_INVALID_ARGUMENT;
		vmf_scope_info info = {};
		auto status = vmf_schema_scope_info(set, m,
			{VMF_SCOPE_REPEAT, path[i].id}, &info);

		if (status != VMF_OK)
			return status;
		if (path[i].occurrence >= info.maximum)
			return VMF_OUT_OF_RANGE;
	}
	return VMF_OK;
}

vmf_status utf8_character(const uint8_t *data, size_t size, size_t offset,
	uint32_t *character, size_t *length)
{
	uint8_t first = data[offset];
	uint32_t value;
	size_t count;

	if (first < 0x80) {
		*character = first;
		*length = 1;
		return VMF_OK;
	}
	if (first >= 0xc2 && first <= 0xdf) {
		count = 2;
		value = first & 31;
	} else if (first >= 0xe0 && first <= 0xef) {
		count = 3;
		value = first & 15;
	} else if (first >= 0xf0 && first <= 0xf4) {
		count = 4;
		value = first & 7;
	} else {
		return VMF_MALFORMED;
	}
	if (count > size - offset)
		return VMF_MALFORMED;
	for (size_t i = 1; i < count; ++i) {
		uint8_t code = data[offset + i];

		if ((code & 0xc0) != 0x80)
			return VMF_MALFORMED;
		value = (value << 6) | (code & 63);
	}
	if ((count == 2 && value < 0x80) || (count == 3 && value < 0x800) ||
		(count == 4 && value < 0x10000) ||
		(value >= 0xd800 && value <= 0xdfff) || value > 0x10ffff)
		return VMF_MALFORMED;
	*character = value;
	*length = count;
	return VMF_OK;
}

vmf_status scalar_code(const vmf_index_info *info, const vmf_field *value,
	uint64_t *code)
{
	uint8_t width = info->integer.bit_width;

	switch (info->kind) {
	case VMF_FIELD_BOOL:
		*code = value->boolean.value;
		break;
	case VMF_FIELD_UNSIGNED:
		*code = value->unsigned_integer.value;
		break;
	case VMF_FIELD_ENUM:
		*code = value->enumeration.code;
		break;
	case VMF_FIELD_SCALED:
		*code = value->scaled.code;
		break;
	case VMF_FIELD_SIGNED: {
		int64_t number = value->signed_integer.value;
		uint64_t sign = UINT64_C(1) << (width - 1);
		uint64_t magnitude = number < 0 ?
			(uint64_t)(-(number + 1)) + 1 : (uint64_t)number;
		bool twos = info->integer.representation ==
			VMF_INTEGER_TWOS_COMPLEMENT;

		if ((number >= 0 && magnitude >= sign) ||
			(number < 0 && magnitude > sign - !twos))
			return VMF_OUT_OF_RANGE;
		if (number >= 0)
			*code = magnitude;
		else if (twos)
			*code = UINT64_C(0) - magnitude;
		else if (info->integer.representation ==
			VMF_INTEGER_ONES_COMPLEMENT)
			*code = ~magnitude;
		else
			*code = sign | magnitude;
		if (width < 64)
			*code &= (UINT64_C(1) << width) - 1;
		break;
	}
	default:
		return VMF_TYPE_MISMATCH;
	}
	if (width < 64 && *code >> width)
		return VMF_OUT_OF_RANGE;
	if (info->constant.present && *code != info->constant.value)
		return VMF_OUT_OF_RANGE;
	if (!info->allowed_count)
		return VMF_OK;
	for (size_t i = 0; i < info->allowed_count; ++i)
		if (*code >= info->allowed[i].first &&
			*code <= info->allowed[i].last)
			return VMF_OK;
	return VMF_OUT_OF_RANGE;
}

vmf_status complete_units(const vmf_index_info *info, const vmf_field *value,
	uint64_t *units)
{
	*units = 0;
	switch (info->kind) {
	case VMF_FIELD_TEXT: {
		if (!value->text.last ||
			(!value->text.data && value->text.size))
			return VMF_INVALID_ARGUMENT;
		const uint8_t *data = reinterpret_cast<const uint8_t *>(
			value->text.data);

		for (size_t offset = 0; offset < value->text.size;) {
			uint32_t character;
			size_t length;
			auto status = utf8_character(data, value->text.size,
				offset, &character, &length);

			if (status != VMF_OK)
				return status;
			if (((info->text_encoding == VMF_TEXT_ASCII_7 ||
				info->text_encoding == VMF_TEXT_ASCII_8) &&
				character > 127) ||
				(info->text_encoding == VMF_TEXT_LATIN_1 &&
				character > 255) ||
				(info->literal && character == 127))
				return VMF_OUT_OF_RANGE;
			*units += info->text_encoding == VMF_TEXT_UTF_8 ?
				length : 1;
			offset += length;
		}
		break;
	}
	case VMF_FIELD_OCTETS:
		if (!value->octets.last ||
			(!value->octets.data && value->octets.size))
			return VMF_INVALID_ARGUMENT;
		*units = value->octets.size;
		break;
	case VMF_FIELD_BITS:
		if (!value->bits.last ||
			(!value->bits.data && value->bits.bit_count))
			return VMF_INVALID_ARGUMENT;
		*units = value->bits.bit_count;
		if (*units % 8 &&
			(value->bits.data[*units / 8] >> (*units % 8)))
			return VMF_OUT_OF_RANGE;
		break;
	default: {
		uint64_t code;
		auto status = scalar_code(info, value, &code);

		if (status != VMF_OK)
			return status;
		*units = 1;
		return VMF_OK;
	}
	}
	if ((!info->variable && !info->literal &&
		*units != info->capacity_units) ||
		((info->variable || info->literal) &&
		*units > info->capacity_units))
		return VMF_OUT_OF_RANGE;
	return VMF_OK;
}
} /* namespace */

vmf_status vmf_schema_scope_info(const vmf_schema_set *set,
	const vmf_message_schema *message, vmf_scope_ref scope,
	vmf_scope_info *result)
{
	if (result)
		*result = {};
	if (!result || !message_member(set, message) ||
		(scope.kind != VMF_SCOPE_GROUP &&
		 scope.kind != VMF_SCOPE_REPEAT))
		return VMF_INVALID_ARGUMENT;
	const vmf_index_schema *member = nullptr;
	size_t repeat_position = 0;

	for (size_t i = 0; i < message->index_count; ++i) {
		const vmf_index_schema *index = &message->indexes[i];

		if (scope.kind == VMF_SCOPE_GROUP) {
			if (index->indicator == VMF_INDICATOR_GPI &&
				index->target.kind == VMF_TARGET_GROUP &&
				index->target.as.group == scope.id) {
				result->controller = index;
				result->ancestry = index->repeat_code;
				result->maximum = 1;
				return VMF_OK;
			}
			if (!member && group_member(index, scope.id))
				member = index;
		} else {
			for (size_t j = 0; j < index->repeat_code.count; ++j) {
				if (index->repeat_code.parts[j].id != scope.id)
					continue;
				if (!member) {
					member = index;
					repeat_position = j;
				}
				bool recurrence =
					index->indicator == VMF_INDICATOR_FRI ||
					index->indicator == VMF_INDICATOR_GRI;

				if (j + 1 != index->repeat_code.count ||
					!recurrence)
					continue;
				const auto *parts = index->repeat_code.parts;

				result->controller = index;
				result->ancestry = {parts, j};
				result->minimum = 1;
				result->maximum = parts[j].maximum.value;
				return VMF_OK;
			}
		}
	}
	if (!member)
		return VMF_INVALID_ARGUMENT;
	if (scope.kind == VMF_SCOPE_GROUP) {
		result->maximum = 1;
		return VMF_OK;
	}
	for (size_t i = 0; i < message->repeat_count; ++i) {
		const vmf_repeat_definition *repeat = &message->repeats[i];

		if (repeat->id != scope.id)
			continue;
		result->ancestry = {member->repeat_code.parts, repeat_position};
		result->minimum = repeat->kind == VMF_REPEAT_FIXED ?
			repeat->count : (repeat->minimum.present ?
				repeat->minimum.value : 0);
		result->maximum = repeat->kind == VMF_REPEAT_FIXED ?
			repeat->count : repeat->maximum;
		return VMF_OK;
	}
	return VMF_INVALID_SCHEMA;
}

vmf_status vmf_author_validate_address(const vmf_schema_set *set,
	const vmf_author_address *address)
{
	if (!address || !message_member(set, address->schema))
		return VMF_INVALID_ARGUMENT;
	vmf_repeat_path ancestry = {};

	if (address->index) {
		if (address->scope.kind != VMF_SCOPE_NONE ||
			address->scope.id ||
			!index_member(address->schema, address->index))
			return VMF_INVALID_ARGUMENT;
		if (address->index->indicator != VMF_INDICATOR_NONE)
			return VMF_UNSUPPORTED_SCHEMA_FEATURE;
		ancestry = address->index->repeat_code;
	} else {
		vmf_scope_info info = {};
		auto status = vmf_schema_scope_info(set, address->schema,
			address->scope, &info);

		if (status != VMF_OK)
			return status;
		if (!info.controller)
			return VMF_UNSUPPORTED_SCHEMA_FEATURE;
		ancestry = info.ancestry;
	}
	return validate_path(set, address->schema, ancestry,
		address->path, address->path_count);
}

vmf_status vmf_author_validate_field(const vmf_schema_set *set,
	const vmf_author_field *field, uint64_t *units)
{
	if (units)
		*units = 0;
	if (!field || !field->address.index)
		return VMF_INVALID_ARGUMENT;
	auto status = vmf_author_validate_address(set, &field->address);

	if (status != VMF_OK)
		return status;
	vmf_index_info info = {};

	status = vmf_schema_index_info(set, field->address.schema,
		field->address.index, &info);
	if (status != VMF_OK)
		return status;
	if (field->kind != info.kind)
		return VMF_TYPE_MISMATCH;
	uint64_t actual = 0;

	status = complete_units(&info, &field->value, &actual);
	if (status == VMF_OK && units)
		*units = actual;
	return status;
}

vmf_status vmf_author_validate_control(const vmf_schema_set *set,
	const vmf_author_control *control)
{
	if (!control || control->address.index ||
		(control->kind != VMF_ENCODE_REQUEST_PRESENCE &&
		 control->kind != VMF_ENCODE_REQUEST_REPEAT_COUNT))
		return VMF_INVALID_ARGUMENT;
	auto status = vmf_author_validate_address(set, &control->address);

	if (status != VMF_OK)
		return status;
	if (control->kind == VMF_ENCODE_REQUEST_PRESENCE)
		return control->address.scope.kind == VMF_SCOPE_GROUP ?
			VMF_OK : VMF_TYPE_MISMATCH;
	if (control->address.scope.kind != VMF_SCOPE_REPEAT)
		return VMF_TYPE_MISMATCH;
	vmf_scope_info info = {};

	status = vmf_schema_scope_info(set, control->address.schema,
		control->address.scope, &info);
	if (status != VMF_OK)
		return status;
	uint64_t count = control->as.repeat_count;

	if (count == VMF_UNITS_UNKNOWN)
		return VMF_LENGTH_REQUIRED;
	return count >= info.minimum && count <= info.maximum ?
		VMF_OK : VMF_OUT_OF_RANGE;
}

namespace
{
struct budget {
	vmf_allocator allocator;
	size_t live;
	size_t maximum;
	bool exceeded;
};

/* The aligned prefix keeps custom allocators' original pointers intact. */
union allocation_prefix {
	std::max_align_t alignment;
	size_t size;
};

void *default_allocate(void *, size_t size)
{
	return std::malloc(size);
}
void default_deallocate(void *, void *memory)
{
	std::free(memory);
}

void *budget_allocate(void *user, size_t size)
{
	auto *b = static_cast<budget *>(user);

	if (size > SIZE_MAX - sizeof(allocation_prefix) ||
		size + sizeof(allocation_prefix) > b->maximum - b->live) {
		b->exceeded = true;
		return nullptr;
	}
	size_t total = size + sizeof(allocation_prefix);
	void *memory = b->allocator.allocate(b->allocator.user, total);

	if (!memory)
		return nullptr;
	if (reinterpret_cast<uintptr_t>(memory) %
		alignof(std::max_align_t)) {
		b->allocator.deallocate(b->allocator.user, memory);
		return nullptr;
	}
	auto *prefix = static_cast<allocation_prefix *>(memory);

	prefix->size = total;
	b->live += total;
	return prefix + 1;
}
void budget_deallocate(void *user, void *memory)
{
	if (!memory)
		return;
	auto *b = static_cast<budget *>(user);
	auto *prefix = static_cast<allocation_prefix *>(memory) - 1;

	b->live -= prefix->size;
	b->allocator.deallocate(b->allocator.user, prefix);
}

vmf_error empty_codec_error()
{
	return {VMF_OK, 0, SIZE_MAX, SIZE_MAX, SIZE_MAX,
		VMF_PROPERTY_NONE, SIZE_MAX, 0};
}
void initialize_error(vmf_author_error *error)
{
	*error = {};
	error->entry_index = SIZE_MAX;
	error->descriptor_index = SIZE_MAX;
	error->codec = empty_codec_error();
}
void address_error(vmf_author_error *error, const vmf_author_address *address,
	bool control, size_t entry, vmf_status status)
{
	error->status = status;
	error->entry_index = entry;
	error->entry_is_control = control;
	error->descriptor_index = address->descriptor_index;
	error->schema = address->schema;
	error->index = address->index;
	error->scope = address->scope;
	error->path_count = address->path_count;
	size_t count = address->path_count;

	if (count > VMF_AUTHOR_ERROR_PATH_CAPACITY)
		count = VMF_AUTHOR_ERROR_PATH_CAPACITY;
	if (address->path)
		for (size_t i = 0; i < count; ++i)
			error->path[i] = address->path[i];
}

vmf_header_fields *header_fields(vmf_header *header,
	vmf_envelope_version profile)
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
	default:
		return nullptr;
	}
}

bool security_material(const vmf_security *security)
{
	return security->present || security->spi ||
		security->keying_material.present ||
		security->keying_material.size ||
		security->initialization.present ||
		security->initialization.size || security->tokens_present ||
		security->token_count || security->authentication_a.present ||
		security->authentication_a.size ||
		security->authentication_b.present ||
		security->authentication_b.size ||
		security->signed_ack_request || security->padding_present ||
		security->padding_length || security->padding.present ||
		security->padding.size;
}

vmf_status validate_envelope(const vmf_author_options *options,
	const vmf_envelope_event *envelope, vmf_header *copy,
	vmf_author_error *error)
{
	if (!options || !envelope || !options->codec.schemas ||
		!envelope->header || envelope->wire_version !=
			options->codec.profile ||
		options->codec.profile < VMF_ENVELOPE_B ||
		options->codec.profile > VMF_ENVELOPE_D1)
		return VMF_INVALID_ARGUMENT;
	auto layout = vmf_header_layout(options->codec.profile);

	if (envelope->object_size != layout.size)
		return VMF_INVALID_ARGUMENT;
	*copy = {};
	std::memcpy(copy, envelope->header, layout.size);
	auto *fields = header_fields(copy, options->codec.profile);

	if (!fields->messages || !fields->message_count ||
		fields->message_count > 16)
		return VMF_INVALID_ARGUMENT;
	if (fields->compression.present)
		return VMF_UNSUPPORTED_ENVELOPE_FEATURE;
	for (size_t i = 0; i < fields->message_count; ++i) {
		const vmf_message_header *message = &fields->messages[i];

		if (security_material(&message->security)) {
			error->descriptor_index = i;
			return VMF_UNSUPPORTED_ENVELOPE_FEATURE;
		}
	}
	return VMF_OK;
}

vmf_status validate_descriptors(const vmf_schema_set *set,
	const vmf_header_fields *header, vmf_author_error *error)
{
	for (size_t i = 0; i < header->message_count; ++i) {
		const auto *message = &header->messages[i];

		if (!message->has_body)
			continue;
		const vmf_message_schema *schema = nullptr;
		auto status = vmf_schema_select(set, &message->id, &schema);

		if (status != VMF_OK) {
			error->descriptor_index = i;
			return status;
		}
	}
	return VMF_OK;
}
} /* namespace */

struct vmf_author_result {
	budget memory;
	uint8_t *data;
	size_t size;
	vmf_measurement *measurements;
	size_t descriptor_count;
	bool *field_active;
	size_t field_count;
	bool *control_active;
	size_t control_count;
};

namespace
{
struct author_run {
	const vmf_author_options *options;
	const vmf_author_field *fields;
	size_t field_count;
	const vmf_author_control *controls;
	size_t control_count;
	vmf_author_result *result;
	vmf_author_error *error;
};

vmf_author_address request_address(const vmf_encode_request *request)
{
	return {request->descriptor_index, request->schema, request->index,
		request->scope, request->path, request->path_count};
}

bool same_message_prefix(const vmf_author_address *request,
	const vmf_author_address *entry)
{
	return request->schema == entry->schema &&
		request->descriptor_index == entry->descriptor_index &&
		path_prefix(request->path, request->path_count,
			entry->path, entry->path_count);
}

bool infer_group(const author_run *run, const vmf_author_address *request)
{
	for (size_t i = 0; i < run->field_count; ++i) {
		const auto *address = &run->fields[i].address;

		if (same_message_prefix(request, address) &&
			group_member(address->index, request->scope.id))
			return true;
	}
	for (size_t i = 0; i < run->control_count; ++i) {
		const auto *control = &run->controls[i];

		if (!same_message_prefix(request, &control->address) ||
			(control->kind == VMF_ENCODE_REQUEST_PRESENCE &&
			 !control->as.present))
			continue;
		vmf_scope_info info = {};

		if (vmf_schema_scope_info(run->options->codec.schemas,
			control->address.schema, control->address.scope,
			&info) == VMF_OK && info.controller &&
			group_member(info.controller, request->scope.id))
			return true;
	}
	return false;
}

vmf_event_action provide(void *user, const vmf_encode_request *request,
	vmf_encode_reply *reply)
{
	auto *run = static_cast<author_run *>(user);
	vmf_author_address address = request_address(request);

	*reply = {};
	reply->kind = request->kind;
	if (request->index) {
		for (size_t i = 0; i < run->field_count; ++i) {
			if (!address_equal(&address, &run->fields[i].address))
				continue;
			address_error(run->error, &address, false, i, VMF_OK);
			if (request->kind == VMF_ENCODE_REQUEST_PRESENCE)
				reply->as.present = true;
			else {
				reply->as.field = run->fields[i].value;
				run->result->field_active[i] = true;
			}
			return VMF_EVENT_CONTINUE;
		}
		if (request->kind == VMF_ENCODE_REQUEST_PRESENCE) {
			reply->as.present = false;
			return VMF_EVENT_CONTINUE;
		}
	} else {
		for (size_t i = 0; i < run->control_count; ++i) {
			const auto *control = &run->controls[i];

			if (!address_equal(&address, &control->address))
				continue;
			address_error(run->error, &address, true, i, VMF_OK);
			if (request->kind == VMF_ENCODE_REQUEST_PRESENCE)
				reply->as.present = control->as.present;
			else
				reply->as.repeat_count =
					control->as.repeat_count;
			run->result->control_active[i] = true;
			return VMF_EVENT_CONTINUE;
		}
		if (request->kind == VMF_ENCODE_REQUEST_PRESENCE) {
			reply->as.present = infer_group(run, &address);
			return VMF_EVENT_CONTINUE;
		}
	}
	address_error(run->error, &address, !request->index, SIZE_MAX,
		VMF_LENGTH_REQUIRED);
	return VMF_EVENT_ABORT;
}

vmf_status validate_entries(author_run *run, const vmf_header_fields *header)
{
	const auto *set = run->options->codec.schemas;

	for (size_t i = 0; i < run->field_count; ++i) {
		const auto *field = &run->fields[i];
		const auto *address = &field->address;
		uint64_t units = 0;
		auto status = vmf_author_validate_field(set, field, &units);

		if (status == VMF_OK) {
			vmf_index_info info = {};

			vmf_schema_index_info(set, address->schema,
				address->index, &info);
			if (info.number_of_bits >
				run->options->codec.limits.max_field_bits)
				status = VMF_LIMIT_EXCEEDED;
		}
		if (status == VMF_OK &&
			(address->descriptor_index >= header->message_count ||
			 !header->messages[address->descriptor_index].has_body))
			status = VMF_INVALID_ARGUMENT;
		if (status == VMF_OK) {
			vmf_message_id id = {};

			status = vmf_schema_message_id(set,
				address->schema, &id);
			const auto *message =
				&header->messages[address->descriptor_index];

			if (status == VMF_OK &&
				!vmf_message_id_equal(&id, &message->id))
				status = VMF_INVALID_ARGUMENT;
		}
		if (status == VMF_OK)
			for (size_t j = 0; j < i; ++j)
				if (address_equal(address,
					&run->fields[j].address)) {
					status = VMF_INVALID_ARGUMENT;
					break;
				}
		if (status != VMF_OK) {
			address_error(run->error, address, false, i, status);
			return status;
		}
	}
	for (size_t i = 0; i < run->control_count; ++i) {
		const auto *control = &run->controls[i];
		const auto *address = &control->address;
		auto status = vmf_author_validate_control(set, control);

		if (status == VMF_OK &&
			control->kind == VMF_ENCODE_REQUEST_REPEAT_COUNT &&
			control->as.repeat_count >
				run->options->codec.limits.max_repetitions)
			status = VMF_LIMIT_EXCEEDED;
		if (status == VMF_OK &&
			(address->descriptor_index >= header->message_count ||
			 !header->messages[address->descriptor_index].has_body))
			status = VMF_INVALID_ARGUMENT;
		if (status == VMF_OK) {
			vmf_message_id id = {};

			status = vmf_schema_message_id(set,
				address->schema, &id);
			const auto *message =
				&header->messages[address->descriptor_index];

			if (status == VMF_OK &&
				!vmf_message_id_equal(&id, &message->id))
				status = VMF_INVALID_ARGUMENT;
		}
		if (status == VMF_OK)
			for (size_t j = 0; j < i; ++j)
				if (address_equal(address,
					&run->controls[j].address)) {
					status = VMF_INVALID_ARGUMENT;
					break;
				}
		if (status != VMF_OK) {
			address_error(run->error, address, true, i, status);
			return status;
		}
	}
	return VMF_OK;
}

vmf_status run_pass(author_run *run, const vmf_options *options,
	const vmf_envelope_event *envelope, bool counting)
{
	vmf_encoder *encoder = nullptr;
	auto status = vmf_encoder_create(options, counting, &encoder,
		&run->error->codec);

	if (status == VMF_OK)
		status = vmf_encoder_begin_pdu(encoder, envelope);
	size_t size = 0;

	while (status == VMF_OK || status == VMF_NEED_OUTPUT) {
		size_t available = run->result->size - size;

		if (available > run->options->output_slice_bytes)
			available = run->options->output_slice_bytes;
		vmf_output output = {counting ? nullptr :
			run->result->data + size, available, 0};
		size_t delivered = 0;

		status = vmf_encoder_dispatch(encoder,
			counting ? nullptr : &output, provide, run, &delivered);
		size += output.used;
		if (!counting && status == VMF_NEED_OUTPUT &&
			size == run->result->size) {
			status = VMF_INVALID_STATE;
			break;
		}
	}
	if (status == VMF_DONE) {
		for (size_t i = 0; i < run->result->descriptor_count; ++i) {
			vmf_measurement measured = {};

			status = vmf_encoder_measurement(encoder, i, &measured);
			if (status != VMF_OK)
				break;
			if (counting)
				run->result->measurements[i] = measured;
			else if (std::memcmp(&measured,
				&run->result->measurements[i],
				sizeof(measured))) {
				status = VMF_INVALID_STATE;
				break;
			}
		}
		if (status == VMF_OK && !counting &&
			size != run->result->size)
			status = VMF_INVALID_STATE;
	} else if (run->error->status != VMF_OK) {
		status = run->error->status;
	} else if (encoder) {
		run->error->codec = vmf_encoder_error(encoder);
	}
	vmf_encoder_destroy(encoder);
	if (run->result->memory.exceeded)
		status = VMF_LIMIT_EXCEEDED;
	return status;
}

vmf_status initialize_result(const vmf_author_options *options,
	vmf_author_result **result)
{
	vmf_allocator allocator = {
		default_allocate, default_deallocate, nullptr};

	if (options->codec.allocator) {
		if (!!options->codec.allocator->allocate !=
			!!options->codec.allocator->deallocate)
			return VMF_INVALID_ARGUMENT;
		if (options->codec.allocator->allocate)
			allocator = *options->codec.allocator;
	}
	if (options->max_working_bytes < sizeof(vmf_author_result))
		return VMF_LIMIT_EXCEEDED;
	void *memory = allocator.allocate(allocator.user,
		sizeof(vmf_author_result));

	if (!memory)
		return VMF_OUT_OF_MEMORY;
	if (reinterpret_cast<uintptr_t>(memory) % alignof(std::max_align_t)) {
		allocator.deallocate(allocator.user, memory);
		return VMF_INVALID_ARGUMENT;
	}
	auto *created = static_cast<vmf_author_result *>(memory);

	*created = {};
	created->memory = {allocator, sizeof(*created),
		options->max_working_bytes, false};
	*result = created;
	return VMF_OK;
}

vmf_status encode_pdu(author_run *run, const vmf_envelope_event *envelope,
	vmf_header *header)
{
	auto *result = run->result;
	auto *fields = header_fields(header, run->options->codec.profile);
	vmf_message_header messages[16] = {};

	for (size_t i = 0; i < fields->message_count; ++i) {
		messages[i] = fields->messages[i];
		messages[i].message_size_octets = {true, 0};
	}
	fields->messages = messages;
	if (run->options->codec.profile != VMF_ENVELOPE_B)
		fields->header_size_octets = {true, 0};
	vmf_envelope_event adjusted = *envelope;

	adjusted.header = header;
	adjusted.header_octets = VMF_LENGTH_UNKNOWN;
	adjusted.pdu_octets = VMF_LENGTH_UNKNOWN;
	result->descriptor_count = fields->message_count;
	result->field_count = run->field_count;
	result->control_count = run->control_count;
	size_t measurement_bytes =
		fields->message_count * sizeof(vmf_measurement);

	result->measurements = static_cast<vmf_measurement *>(budget_allocate(
		&result->memory, measurement_bytes));
	if (run->field_count)
		result->field_active = static_cast<bool *>(budget_allocate(
			&result->memory, run->field_count * sizeof(bool)));
	if (run->control_count)
		result->control_active = static_cast<bool *>(budget_allocate(
			&result->memory, run->control_count * sizeof(bool)));
	if (!result->measurements ||
		(!result->field_active && run->field_count) ||
		(!result->control_active && run->control_count))
		return result->memory.exceeded ? VMF_LIMIT_EXCEEDED :
			VMF_OUT_OF_MEMORY;
	std::memset(result->measurements, 0,
		fields->message_count * sizeof(vmf_measurement));
	if (run->field_count)
		std::memset(result->field_active, 0, run->field_count);
	if (run->control_count)
		std::memset(result->control_active, 0, run->control_count);
	vmf_allocator bounded = {budget_allocate, budget_deallocate,
		&result->memory};
	vmf_options options = run->options->codec;

	options.allocator = &bounded;
	auto status = run_pass(run, &options, &adjusted, true);

	if (status != VMF_OK)
		return status;
	const auto *first = &result->measurements[0];

	if (first->header_octets > UINT32_MAX || first->pdu_octets > SIZE_MAX)
		return VMF_LIMIT_EXCEEDED;
	fields->header_size_octets.value =
		static_cast<uint32_t>(first->header_octets);
	for (size_t i = 0; i < fields->message_count; ++i) {
		uint64_t size = result->measurements[i].body_octets;

		if (size > UINT32_MAX)
			return VMF_LIMIT_EXCEEDED;
		messages[i].message_size_octets.value =
			static_cast<uint32_t>(size);
	}
	result->size = static_cast<size_t>(first->pdu_octets);
	result->data = static_cast<uint8_t *>(budget_allocate(
		&result->memory, result->size));
	if (!result->data)
		return result->memory.exceeded ? VMF_LIMIT_EXCEEDED :
			VMF_OUT_OF_MEMORY;
	return run_pass(run, &options, &adjusted, false);
}
} /* namespace */

vmf_author_options vmf_author_options_for_schemas(const vmf_schema_set *set)
{
	return {vmf_options_for_schemas(set), 64 * 1024 * 1024, 65536, 65536};
}

vmf_status vmf_author_validate_header(const vmf_author_options *options,
	const vmf_envelope_event *envelope, vmf_author_error *error)
{
	vmf_author_error local;

	if (!error)
		error = &local;
	initialize_error(error);
	vmf_header header = {};
	auto status = validate_envelope(options, envelope, &header, error);
	vmf_author_result *result = nullptr;

	if (status == VMF_OK)
		status = initialize_result(options, &result);
	if (status == VMF_OK) {
		vmf_allocator allocator = {budget_allocate, budget_deallocate,
			&result->memory};
		vmf_options codec = options->codec;
		vmf_encoder *encoder = nullptr;

		auto *fields = header_fields(&header, codec.profile);
		vmf_message_header messages[16] = {};

		for (size_t i = 0; i < fields->message_count; ++i) {
			messages[i] = fields->messages[i];
			messages[i].message_size_octets = {true, 0};
		}
		fields->messages = messages;
		if (codec.profile != VMF_ENVELOPE_B)
			fields->header_size_octets = {true, 0};
		vmf_envelope_event adjusted = *envelope;

		adjusted.header = &header;
		adjusted.header_octets = VMF_LENGTH_UNKNOWN;
		adjusted.pdu_octets = VMF_LENGTH_UNKNOWN;
		codec.allocator = &allocator;
		status = vmf_encoder_create(&codec, true, &encoder,
			&error->codec);
		if (status == VMF_OK)
			status = validate_descriptors(codec.schemas,
				fields, error);
		if (status == VMF_OK)
			status = vmf_encoder_begin_pdu(encoder, &adjusted);
		if (status != VMF_OK && encoder)
			error->codec = vmf_encoder_error(encoder);
		vmf_encoder_destroy(encoder);
		if (result->memory.exceeded)
			status = VMF_LIMIT_EXCEEDED;
	}
	vmf_author_result_destroy(result);
	error->status = status;
	return status;
}

vmf_status vmf_author_encode(const vmf_author_options *options,
	const vmf_envelope_event *envelope, const vmf_author_field *fields,
	size_t field_count, const vmf_author_control *controls,
	size_t control_count, vmf_author_result **result,
	vmf_author_error *error)
{
	vmf_author_error local;

	if (!error)
		error = &local;
	initialize_error(error);
	if (result)
		*result = nullptr;
	if (!result || !options || (!fields && field_count) ||
		(!controls && control_count) || !options->output_slice_bytes) {
		error->status = VMF_INVALID_ARGUMENT;
		return error->status;
	}
	if (field_count > options->max_entries ||
		control_count > options->max_entries) {
		error->status = VMF_LIMIT_EXCEEDED;
		return error->status;
	}
	vmf_header header = {};
	auto status = validate_envelope(options, envelope, &header, error);
	vmf_author_result *created = nullptr;

	if (status == VMF_OK)
		status = initialize_result(options, &created);
	if (status == VMF_OK) {
		/* Metadata helpers require a codec-validated schema. */
		vmf_allocator allocator = {budget_allocate, budget_deallocate,
			&created->memory};
		vmf_options codec = options->codec;
		vmf_encoder *validator = nullptr;

		codec.allocator = &allocator;
		status = vmf_encoder_create(&codec, true, &validator,
			&error->codec);
		vmf_encoder_destroy(validator);
		if (created->memory.exceeded)
			status = VMF_LIMIT_EXCEEDED;
	}
	if (status == VMF_OK) {
		author_run run = {options, fields, field_count, controls,
			control_count, created, error};

		const auto *native = header_fields(&header,
			options->codec.profile);

		status = validate_descriptors(options->codec.schemas,
			native, error);
		if (status == VMF_OK)
			status = validate_entries(&run, native);
		if (status == VMF_OK)
			status = encode_pdu(&run, envelope, &header);
	}
	if (status == VMF_OK) {
		*result = created;
		initialize_error(error);
	} else {
		vmf_author_result_destroy(created);
	}
	error->status = status;
	return status;
}

const uint8_t *vmf_author_result_data(const vmf_author_result *result)
{
	return result ? result->data : nullptr;
}
size_t vmf_author_result_size(const vmf_author_result *result)
{
	return result ? result->size : 0;
}
size_t vmf_author_result_descriptor_count(const vmf_author_result *result)
{
	return result ? result->descriptor_count : 0;
}
vmf_status vmf_author_result_measurement(const vmf_author_result *result,
	size_t descriptor, vmf_measurement *measurement)
{
	if (measurement)
		*measurement = {};
	if (!result || !measurement || descriptor >= result->descriptor_count)
		return VMF_INVALID_ARGUMENT;
	*measurement = result->measurements[descriptor];
	return VMF_OK;
}
bool vmf_author_result_field_active(const vmf_author_result *result,
	size_t entry)
{
	return result && entry < result->field_count &&
		result->field_active[entry];
}
bool vmf_author_result_control_active(const vmf_author_result *result,
	size_t entry)
{
	return result && entry < result->control_count &&
		result->control_active[entry];
}
void vmf_author_result_destroy(vmf_author_result *result)
{
	if (!result)
		return;
	budget_deallocate(&result->memory, result->data);
	budget_deallocate(&result->memory, result->measurements);
	budget_deallocate(&result->memory, result->field_active);
	budget_deallocate(&result->memory, result->control_active);
	vmf_allocator allocator = result->memory.allocator;

	allocator.deallocate(allocator.user, result);
}
