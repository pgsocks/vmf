#include <vmf/schema_json.h>

/* Compile and link an independent C11 caller of the companion library. */
int vmf_schema_json_c_consumer(void)
{
	vmf_schema_json_options options = vmf_schema_json_default_options();
	const char input[] = "{\"format\":\"vmf-schema/1\",\"envelope\":\"C\","
		"\"messages\":[]}";
	vmf_schema_json_document *document = NULL;
	vmf_schema_json_error error;

	if (vmf_schema_json_load(input, sizeof(input) - 1, &options,
		&document, &error) != VMF_OK)
		return 0;
	const vmf_schema_set *set = vmf_schema_json_schemas(document);
	int valid = set != NULL && set->envelope == VMF_ENVELOPE_C &&
		set->message_count == 0 && error.index_position == SIZE_MAX;

	vmf_schema_json_destroy(document);

	vmf_schema_json_destroy(NULL);
	return valid && options.max_json_bytes == 16u * 1024u * 1024u &&
	       options.max_memory_bytes == 64u * 1024u * 1024u &&
	       vmf_schema_json_schemas(NULL) == NULL;
}
