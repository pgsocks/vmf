#include <vmf/wire.h>
#include "../acceptance/vectors.h"
#include <string.h>

static const uint32_t address[] = {1};
static const vmf_index_schema indexes[] = {
	{.index_number = {address, 1}, .number_of_bits = 8}
};
static const vmf_message_schema messages[] = {
	{.message_number = {1, 1}, .indexes = indexes, .index_count = 1}
};
static const vmf_schema_set schemas = {
	.envelope = VMF_ENVELOPE_C,
	.messages = messages,
	.message_count = 1,
	.standard = {.kind = VMF_STANDARD_RAW_CODE, .as.raw_code = 0}
};

/* Compile and link a C11 caller against the C++17 library. */
int vmf_c_consumer(void)
{
	vmf_options options = vmf_options_for_schemas(&schemas);
	vmf_decoder *reader = NULL;
	vmf_encoder *writer = NULL;
	vmf_index_info info;
	uint8_t bytes[64];
	vmf_input input = {vector_vmf_c, sizeof(vector_vmf_c), 0, true};
	vmf_output output = {bytes, sizeof(bytes), 0};
	int result = 0;

	if (vmf_schema_set_validate(&schemas, &options.limits, NULL, NULL) !=
		VMF_OK || vmf_schema_index_info(&schemas, messages, indexes,
			&info) != VMF_OK || info.kind != VMF_FIELD_UNSIGNED)
		return 0;
	if (vmf_decoder_create(&options, &reader, NULL) != VMF_OK ||
		vmf_encoder_create(&options, false, &writer, NULL) != VMF_OK)
		goto done;
	if (vmf_decoder_begin_pdu(reader) != VMF_OK)
		goto done;
	for (;;) {
		vmf_event event;
		vmf_status status = vmf_decoder_next(reader, &input, &event);

		if (status == VMF_DONE)
			break;
		if (status != VMF_OK ||
			vmf_encoder_write(writer, &output, &event) != VMF_OK)
			goto done;
	}
	result = output.used == sizeof(vector_vmf_c) &&
		!memcmp(bytes, vector_vmf_c, sizeof(vector_vmf_c));
done:
	vmf_encoder_destroy(writer);
	vmf_decoder_destroy(reader);
	return result;
}
