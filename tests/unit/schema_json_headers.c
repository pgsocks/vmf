#include <vmf/schema_json.h>

/* Compile and link an independent C11 caller of the companion library. */
int vmf_schema_json_c_consumer(void)
{
	vmf_schema_json_options options = vmf_schema_json_default_options();

	vmf_schema_json_destroy(NULL);
	return options.max_json_bytes == 16u * 1024u * 1024u &&
	       options.max_memory_bytes == 64u * 1024u * 1024u &&
	       vmf_schema_json_schemas(NULL) == NULL;
}
