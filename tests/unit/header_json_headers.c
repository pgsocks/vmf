#include <vmf/header_json.h>

/* C11 consumer: public headers need neither C++ nor yyjson declarations. */
int vmf_header_json_c_consumer(void)
{
	vmf_header_json_options options = vmf_header_json_default_options();
	vmf_header_json_document *document = NULL;
	vmf_header_json_error error;
	vmf_status status = vmf_header_json_load(VMF_ENVELOPE_C, "", 0,
		&options, &document, &error);

	vmf_header_json_destroy(document);
	return status == VMF_TRUNCATED && error.byte_offset == 0;
}
