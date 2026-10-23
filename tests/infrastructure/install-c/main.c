#include <vmf/wire.h>

#ifdef TEST_SCHEMA_JSON
#include <vmf/schema_json.h>
#endif
#ifdef TEST_HEADER_JSON
#include <vmf/header_json.h>
#endif

int main(void)
{
#ifdef TEST_SCHEMA_JSON
	vmf_schema_json_default_options();
#endif
#ifdef TEST_HEADER_JSON
	vmf_header_json_default_options();
#endif
	return vmf_status_string(VMF_OK) ? 0 : 1;
}
