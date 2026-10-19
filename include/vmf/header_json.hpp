#ifndef VMF_HEADER_JSON_HPP_INCLUDED
#define VMF_HEADER_JSON_HPP_INCLUDED

#include <vmf/header_json.h>

/* Procedural aliases/forwarders; contracts are those of header_json.h. */
namespace vmf
{
namespace header_json
{

using document = ::vmf_header_json_document;
using text = ::vmf_header_json_text;
using options = ::vmf_header_json_options;
using error = ::vmf_header_json_error;
using reason = ::vmf_header_json_reason;

inline options default_options(void)
{
	return ::vmf_header_json_default_options();
}

inline vmf_status load(vmf_envelope_version profile, const char *data,
	size_t size, const options *config, document **result,
	error *diagnostic)
{
	return ::vmf_header_json_load(profile, data, size, config, result,
		diagnostic);
}

inline const vmf_header *header(const document *owner)
{
	return ::vmf_header_json_header(owner);
}

inline vmf_envelope_version profile(const document *owner)
{
	return ::vmf_header_json_profile(owner);
}

inline void destroy(document *owner)
{
	::vmf_header_json_destroy(owner);
}

inline vmf_status serialize(vmf_envelope_version profile,
	const void *header, const options *config, text **result,
	error *diagnostic)
{
	return ::vmf_header_json_serialize(profile, header, config, result,
		diagnostic);
}

inline vmf_status minimal_template(vmf_envelope_version profile,
	const vmf_message_id *ids, size_t count, const options *config,
	text **result, error *diagnostic)
{
	return ::vmf_header_json_template(profile, ids, count, config, result,
		diagnostic);
}

inline const char *data(const text *owner)
{
	return ::vmf_header_json_text_data(owner);
}

inline size_t size(const text *owner)
{
	return ::vmf_header_json_text_size(owner);
}

inline void destroy_text(text *owner)
{
	::vmf_header_json_text_destroy(owner);
}

} /* namespace header_json */
} /* namespace vmf */
#endif /* VMF_HEADER_JSON_HPP_INCLUDED */
