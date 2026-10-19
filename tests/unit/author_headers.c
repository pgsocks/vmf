#include <vmf/author.h>
#include "managed_consumer.h"

/* This authoring consumer compiles as C11 and owns no VMF private objects. */
int vmf_author_c_consumer(void)
{
	struct managed_consumer source;
	vmf_author_field fields[4] = {0};
	vmf_repeat_occurrence paths[4][2] = {0};
	vmf_author_result *result = NULL;
	vmf_author_error error;

	managed_consumer_initialize(&source);
	for (size_t i = 0; i < 4; ++i) {
		paths[i][0].id = 10;
		paths[i][0].occurrence = i / 2;
		paths[i][1].id = 20;
		paths[i][1].occurrence = i % 2;
		fields[i].address.schema = &source.schema;
		fields[i].address.index = &source.index;
		fields[i].address.path = paths[i];
		fields[i].address.path_count = 2;
		fields[i].kind = VMF_FIELD_UNSIGNED;
		fields[i].value.unsigned_integer.value =
			source.rows[i / 2][i % 2];
	}
	vmf_author_options options =
		vmf_author_options_for_schemas(&source.set);
	vmf_status status = vmf_author_encode(&options, &source.envelope,
		fields, 4, NULL, 0, &result, &error);
	vmf_measurement measured = {0};
	int success = status == VMF_OK && error.status == VMF_OK &&
		vmf_author_result_measurement(result, 0, &measured) == VMF_OK &&
		measured.body_octets == 4 &&
		measured.pdu_octets == vmf_author_result_size(result) &&
		vmf_author_result_field_active(result, 3);

	vmf_author_result_destroy(result);
	vmf_author_result_destroy(NULL);
	return success;
}
