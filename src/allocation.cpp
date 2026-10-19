#include "codec.hpp"
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>

using namespace vmf_internal;

namespace vmf_internal
{

vmf_error make_error(vmf_status status)
{
	return {status, 0, SIZE_MAX, SIZE_MAX, SIZE_MAX,
		VMF_PROPERTY_NONE, SIZE_MAX, 0};
}

const vmf_header_fields *fields(const vmf_envelope_event *event)
{
	/* All public revision objects have fields as their first member. */
	return static_cast<const vmf_header_fields *>(event->header);
}

} /* namespace vmf_internal */

static void *default_allocate(void *, size_t size)
{
	return std::malloc(size);
}

static void default_deallocate(void *, void *memory)
{
	std::free(memory);
}

static vmf_status configure(const vmf_options *options, configuration *config,
	size_t *payload_size, size_t *storage_size, vmf_error *error)
{
	if (!options || options->profile < VMF_ENVELOPE_B ||
		options->profile > VMF_ENVELOPE_D1)
		return VMF_INVALID_ARGUMENT;
	*config = {options->profile, options->limits,
		{default_allocate, default_deallocate, nullptr}, nullptr, {}};
	if (options->allocator) {
		if (!!options->allocator->allocate !=
			!!options->allocator->deallocate)
			return VMF_INVALID_ARGUMENT;
		if (options->allocator->allocate)
			config->allocator = *options->allocator;
	}
	if (options->schemas)
		config->schemas = options->schemas;
	const vmf_limits *limits = &config->limits;

	if (limits->field_chunk_bytes < 4 || !limits->max_header_bytes ||
		!limits->max_header_items || !limits->max_field_bits ||
		!limits->max_message_octets || !limits->max_pdu_octets ||
		!limits->max_events_per_message ||
		limits->max_message_octets > UINT64_MAX / 8 ||
		limits->max_pdu_octets > UINT64_MAX / 8)
		return VMF_INVALID_ARGUMENT;
	vmf_status status = payload_storage_size(limits, payload_size);

	if (status != VMF_OK)
		return status;
	size_t alignment = alignof(std::max_align_t);

	if (limits->max_header_bytes > (SIZE_MAX - alignment) / 2 ||
		*payload_size >
			SIZE_MAX - limits->max_header_bytes * 2 - alignment)
		return VMF_LIMIT_EXCEEDED;
	*storage_size =
		limits->max_header_bytes * 2 + *payload_size + alignment;
	if (config->schemas && config->schemas->envelope != config->profile) {
		if (error)
			error->property = VMF_PROPERTY_PROFILE;
		return VMF_INVALID_SCHEMA;
	}
	if (!config->schemas)
		return VMF_OK;
	return normalize_schemas(config->schemas, limits, &config->allocator,
		&config->resolved, error);
}

static bool aligned(void *memory)
{
	return reinterpret_cast<uintptr_t>(memory) %
		alignof(std::max_align_t) == 0;
}

static vmf_status allocate_storage(const configuration *config, size_t size,
	void **storage, uint8_t **bytes, void **arena, payload_state **payload)
{
	*storage = config->allocator.allocate(config->allocator.user, size);
	if (!*storage)
		return VMF_OUT_OF_MEMORY;
	if (!aligned(*storage)) {
		config->allocator.deallocate(config->allocator.user, *storage);
		*storage = nullptr;
		return VMF_INVALID_ARGUMENT;
	}
	*arena = *storage;
	*bytes = static_cast<uint8_t *>(*storage) +
		 config->limits.max_header_bytes;
	uintptr_t address = reinterpret_cast<uintptr_t>(
		*bytes + config->limits.max_header_bytes);
	uintptr_t alignment = alignof(std::max_align_t);

	address = (address + alignment - 1) / alignment * alignment;
	return payload_init(
		reinterpret_cast<void *>(address), &config->limits, payload);
}

static vmf_status allocate_managed_storage(vmf_encoder *encoder)
{
	const vmf_limits *limits = &encoder->config.limits;
	size_t alignment = alignof(std::max_align_t);
	size_t count = limits->max_depth + 1;

	if (limits->max_header_bytes > SIZE_MAX - alignment ||
		count > (SIZE_MAX - alignment - limits->max_header_bytes) /
				sizeof(vmf_repeat_occurrence))
		return VMF_LIMIT_EXCEEDED;
	size_t size = limits->max_header_bytes + alignment +
		count * sizeof(vmf_repeat_occurrence);
	const vmf_allocator *allocator = &encoder->config.allocator;
	void *memory = allocator->allocate(allocator->user, size);

	if (!memory)
		return VMF_OUT_OF_MEMORY;
	if (!aligned(memory)) {
		allocator->deallocate(allocator->user, memory);
		return VMF_INVALID_ARGUMENT;
	}
	encoder->managed_storage = memory;
	encoder->candidate_arena = memory;
	uintptr_t address = reinterpret_cast<uintptr_t>(memory) +
		limits->max_header_bytes;

	address = (address + alignment - 1) / alignment * alignment;
	encoder->request_path =
		reinterpret_cast<vmf_repeat_occurrence *>(address);
	return VMF_OK;
}

vmf_options vmf_options_for_schemas(const vmf_schema_set *schemas)
{
	vmf_options options = {};

	options.limits = vmf_default_limits();
	options.schemas = schemas;
	if (schemas)
		options.profile = schemas->envelope;
	return options;
}

vmf_status vmf_decoder_create(
	const vmf_options *options, vmf_decoder **result, vmf_error *error)
{
	if (result)
		*result = nullptr;
	if (error)
		*error = make_error(VMF_OK);
	if (!result) {
		if (error)
			error->status = VMF_INVALID_ARGUMENT;
		return VMF_INVALID_ARGUMENT;
	}
	configuration config = {};
	size_t payload_size = 0, storage_size = 0;
	vmf_status status = configure(
		options, &config, &payload_size, &storage_size, error);

	if (status != VMF_OK) {
		if (error)
			error->status = status;
		return status;
	}
	void *memory = config.allocator.allocate(
		config.allocator.user, sizeof(vmf_decoder));

	if (!memory)
		status = VMF_OUT_OF_MEMORY;
	else if (!aligned(memory))
		status = VMF_INVALID_ARGUMENT;
	if (status != VMF_OK) {
		destroy_normalized_set(&config.resolved, &config.allocator);
		if (memory)
			config.allocator.deallocate(
				config.allocator.user, memory);
		if (error)
			error->status = status;
		return status;
	}
	vmf_decoder *decoder = new (memory) vmf_decoder{};

	decoder->config = config;
	status = allocate_storage(&config, storage_size, &decoder->storage,
		&decoder->header_bytes, &decoder->arena, &decoder->payload);
	if (status != VMF_OK) {
		vmf_decoder_destroy(decoder);
		if (error)
			error->status = status;
		return status;
	}
	vmf_decoder_reset(decoder);
	*result = decoder;
	return VMF_OK;
}

vmf_status vmf_encoder_create(const vmf_options *options, bool counting,
	vmf_encoder **result, vmf_error *error)
{
	if (result)
		*result = nullptr;
	if (error)
		*error = make_error(VMF_OK);
	if (!result) {
		if (error)
			error->status = VMF_INVALID_ARGUMENT;
		return VMF_INVALID_ARGUMENT;
	}
	configuration config = {};
	size_t payload_size = 0, storage_size = 0;
	vmf_status status = configure(
		options, &config, &payload_size, &storage_size, error);

	if (status != VMF_OK) {
		if (error)
			error->status = status;
		return status;
	}
	void *memory = config.allocator.allocate(
		config.allocator.user, sizeof(vmf_encoder));

	if (!memory)
		status = VMF_OUT_OF_MEMORY;
	else if (!aligned(memory))
		status = VMF_INVALID_ARGUMENT;
	if (status != VMF_OK) {
		destroy_normalized_set(&config.resolved, &config.allocator);
		if (memory)
			config.allocator.deallocate(
				config.allocator.user, memory);
		if (error)
			error->status = status;
		return status;
	}
	vmf_encoder *encoder = new (memory) vmf_encoder{};

	encoder->config = config;
	encoder->counting = counting;
	status = allocate_storage(&config, storage_size, &encoder->storage,
		&encoder->header_bytes, &encoder->arena, &encoder->payload);
	if (status == VMF_OK)
		status = allocate_managed_storage(encoder);
	if (status != VMF_OK) {
		vmf_encoder_destroy(encoder);
		if (error)
			error->status = status;
		return status;
	}
	vmf_encoder_reset(encoder);
	*result = encoder;
	return VMF_OK;
}

void vmf_decoder_destroy(vmf_decoder *decoder)
{
	if (!decoder)
		return;
	vmf_allocator allocator = decoder->config.allocator;

	destroy_normalized_set(&decoder->config.resolved, &allocator);

	if (decoder->storage)
		allocator.deallocate(allocator.user, decoder->storage);
	allocator.deallocate(allocator.user, decoder);
}

void vmf_encoder_destroy(vmf_encoder *encoder)
{
	if (!encoder)
		return;
	vmf_allocator allocator = encoder->config.allocator;

	destroy_normalized_set(&encoder->config.resolved, &allocator);

	if (encoder->storage)
		allocator.deallocate(allocator.user, encoder->storage);
	if (encoder->managed_storage)
		allocator.deallocate(allocator.user, encoder->managed_storage);
	allocator.deallocate(allocator.user, encoder);
}

void vmf_decoder_reset(vmf_decoder *decoder)
{
	if (!decoder)
		return;
	decoder->phase = DECODER_IDLE;
	decoder->error = make_error(VMF_OK);
	decoder->envelope = {};
	decoder->schema = nullptr;
	decoder->header_size = 0;
	decoder->header_needed = 1;
	decoder->descriptor = 0;
	decoder->completed_body_octets = 0;
	decoder->events = 0;
	decoder->header_only = false;
}

void vmf_encoder_reset(vmf_encoder *encoder)
{
	if (!encoder)
		return;
	encoder->phase = ENCODER_IDLE;
	encoder->mode = ENCODER_MANUAL;
	encoder->reply_owner = REPLY_NONE;
	encoder->error = make_error(VMF_OK);
	encoder->envelope = {};
	encoder->schema = nullptr;
	encoder->header_size = encoder->header_written = 0;
	encoder->descriptor = 0;
	encoder->completed_body_octets = encoder->bits = encoder->events = 0;
	encoder->begin_length = VMF_LENGTH_UNKNOWN;
	encoder->header_only = encoder->header_pending = false;
	encoder->field_pending = encoder->event_pending = false;
	encoder->request_pending = false;
	encoder->request = {};
	encoder->reply = {};
	encoder->provider = nullptr;
	encoder->provider_user = nullptr;
	encoder->provider_action = VMF_EVENT_CONTINUE;
	std::memset(encoder->measurements, 0, sizeof(encoder->measurements));
	std::memset(encoder->measured, 0, sizeof(encoder->measured));
}

vmf_error vmf_decoder_error(const vmf_decoder *decoder)
{
	return decoder ? decoder->error : make_error(VMF_INVALID_ARGUMENT);
}

vmf_error vmf_encoder_error(const vmf_encoder *encoder)
{
	return encoder ? encoder->error : make_error(VMF_INVALID_ARGUMENT);
}
