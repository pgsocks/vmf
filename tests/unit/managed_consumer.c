#include "managed_consumer.h"

static vmf_status encode_values(struct managed_consumer *c,
	vmf_encoder *encoder, bool counting, uint8_t *bytes, size_t capacity,
	size_t *size)
{
	vmf_encode_reply reply = {0};
	bool pending = false;

	*size = 0;
	for (size_t attempt = 0; attempt < 1024; ++attempt) {
		vmf_output output = {bytes ? bytes + *size : NULL,
			*size < capacity ? 1 : 0, 0};
		vmf_status status;

		if (pending) {
			status = vmf_encoder_respond(encoder,
				counting ? NULL : &output, &reply);
		} else {
			vmf_encode_request request = {0};

			status = vmf_encoder_next_request(encoder,
				counting ? NULL : &output, &request);
			if (status == VMF_OK &&
				!managed_consumer_reply(c, &request, &reply))
				return VMF_INVALID_STATE;
		}
		*size += output.used;
		if (status == VMF_NEED_OUTPUT)
			continue;
		if (status != VMF_OK)
			return status;
		pending = !pending;
	}
	return VMF_LIMIT_EXCEEDED;
}

/* This translation unit compiles as strict C11 against the public header. */
int vmf_managed_c_consumer(uint8_t *bytes, size_t capacity, size_t *size)
{
	struct managed_consumer source;
	vmf_encoder *encoder = NULL;
	vmf_measurement measured = {0};
	size_t ignored = 0;
	int success = 0;

	managed_consumer_initialize(&source);
	vmf_options options = vmf_options_for_schemas(&source.set);

	if (vmf_encoder_create(&options, true, &encoder, NULL) != VMF_OK ||
		vmf_encoder_begin_pdu(encoder, &source.envelope) != VMF_OK ||
		encode_values(&source, encoder, true, NULL, 0, &ignored) !=
			VMF_DONE || source.cursor != 4 ||
		vmf_encoder_measurement(encoder, 0, &measured) != VMF_OK)
		goto done;
	vmf_encoder_destroy(encoder);
	encoder = NULL;
	/* Rewind the application's source and supply measured sizes. */
	source.cursor = 0;
	source.header.fields.header_size_octets.value =
		(uint32_t)measured.header_octets;
	source.message.message_size_octets.value =
		(uint32_t)measured.body_octets;
	if (vmf_encoder_create(&options, false, &encoder, NULL) != VMF_OK ||
		vmf_encoder_begin_pdu(encoder, &source.envelope) != VMF_OK ||
		encode_values(&source, encoder, false, bytes, capacity, size) !=
			VMF_DONE)
		goto done;
	success = source.cursor == 4 && *size == measured.pdu_octets;
done:
	vmf_encoder_destroy(encoder);
	return success;
}
