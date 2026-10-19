#ifndef VMF_SCHEMA_JSON_HPP_INCLUDED
#define VMF_SCHEMA_JSON_HPP_INCLUDED

#include <vmf/schema_json.h>

/* Procedural aliases/forwarders; all contracts are those of schema_json.h. */
namespace vmf
{
namespace schema_json
{

using document = ::vmf_schema_json_document;
using options = ::vmf_schema_json_options;
using error = ::vmf_schema_json_error;

inline options default_options(void)
{
	return ::vmf_schema_json_default_options();
}

inline vmf_status load(const char *data, size_t size, const options *config,
	document **result, error *diagnostic)
{
	return ::vmf_schema_json_load(data, size, config, result, diagnostic);
}

inline const vmf_schema_set *schemas(const document *owner)
{
	return ::vmf_schema_json_schemas(owner);
}

inline void destroy(document *owner)
{
	::vmf_schema_json_destroy(owner);
}

} /* namespace schema_json */
} /* namespace vmf */
#endif /* VMF_SCHEMA_JSON_HPP_INCLUDED */
