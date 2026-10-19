#include <vmf/wire.hpp>
#include "managed_consumer.h"

static vmf_event_action nested_values(void *user,
	const vmf::encode_request *request, vmf::encode_reply *reply)
{
	auto *source = static_cast<managed_consumer *>(user);

	return managed_consumer_reply(source, request, reply) ?
		VMF_EVENT_CONTINUE : VMF_EVENT_ABORT;
}

static vmf::status encode_values(managed_consumer *source,
	vmf::encoder *encoder, bool counting, uint8_t *bytes, size_t capacity,
	size_t *size)
{
	*size = 0;
	for (size_t attempt = 0; attempt < 1024; ++attempt) {
		vmf::output output = {bytes ? bytes + *size : nullptr,
			*size < capacity ? 1u : 0u, 0};
		size_t delivered = 0;
		auto status = vmf::encoder_dispatch(encoder,
			counting ? nullptr : &output, nested_values, source,
			&delivered);

		*size += output.used;
		if (status != VMF_NEED_OUTPUT)
			return status;
	}
	return VMF_LIMIT_EXCEEDED;
}

/* Public-only C++17 procedural aliases, with no private schema traversal. */
int vmf_managed_cpp_consumer(uint8_t *bytes, size_t capacity, size_t *size)
{
	managed_consumer source;

	managed_consumer_initialize(&source);
	auto options = vmf::options_for_schemas(&source.set);
	vmf::encoder *encoder = nullptr;
	vmf::measurement measured = {};
	size_t ignored = 0;
	bool success = false;

	if (vmf::encoder_create(&options, true, &encoder, nullptr) != VMF_OK ||
		vmf::encoder_begin_pdu(encoder, &source.envelope) != VMF_OK ||
		encode_values(&source, encoder, true, nullptr, 0, &ignored) !=
			VMF_DONE || source.cursor != 4 ||
		vmf::encoder_measurement(encoder, 0, &measured) != VMF_OK)
		goto done;
	vmf::encoder_destroy(encoder);
	encoder = nullptr;
	source.cursor = 0;
	source.header.fields.header_size_octets.value =
		static_cast<uint32_t>(measured.header_octets);
	source.message.message_size_octets.value =
		static_cast<uint32_t>(measured.body_octets);
	if (vmf::encoder_create(&options, false, &encoder, nullptr) != VMF_OK ||
		vmf::encoder_begin_pdu(encoder, &source.envelope) != VMF_OK ||
		encode_values(&source, encoder, false, bytes, capacity, size) !=
			VMF_DONE)
		goto done;
	success = source.cursor == 4 && *size == measured.pdu_octets;
done:
	vmf::encoder_destroy(encoder);
	return success;
}
