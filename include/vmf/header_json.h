#ifndef VMF_HEADER_JSON_H_INCLUDED
#define VMF_HEADER_JSON_H_INCLUDED

#include <vmf/wire.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Optional companion for profile-specific logical envelope JSON. Documents
 * own all native header spans. No file I/O, global state, or exceptions.
 * Immutable documents/text may be shared if destruction does not race access.
 */
typedef struct vmf_header_json_document vmf_header_json_document;
typedef struct vmf_header_json_text vmf_header_json_text;

/* Explicit limits; zero never selects defaults. Memory bounds every live
 * allocation, including owner/parser/conversion/validation/output scratch.
 * Input bytes and allocator-private bookkeeping are outside this budget.
 * NULL or two NULL callbacks selects malloc/free. Otherwise callbacks must
 * be paired and follow vmf_allocator alignment rules. Configuration is copied;
 * allocator user state survives every resulting owner. No callback reentry.
 */
typedef struct vmf_header_json_options {
	vmf_limits limits;
	size_t max_json_bytes;
	size_t max_memory_bytes;
	const vmf_allocator *allocator;
} vmf_header_json_options;

/* Machine-readable diagnostic categories; status retains core codec errors. */
typedef enum vmf_header_json_reason {
	VMF_HEADER_JSON_NONE,
	VMF_HEADER_JSON_ARGUMENT,
	VMF_HEADER_JSON_SYNTAX,
	VMF_HEADER_JSON_TYPE,
	VMF_HEADER_JSON_MISSING,
	VMF_HEADER_JSON_PROPERTY,
	VMF_HEADER_JSON_DUPLICATE,
	VMF_HEADER_JSON_RANGE,
	VMF_HEADER_JSON_NATIVE,
	VMF_HEADER_JSON_LIMIT,
	VMF_HEADER_JSON_ALLOCATION
} vmf_header_json_reason;

/* Always initialized when supplied. message is static. path is an RFC 6901
 * JSON pointer, "" at the root/unavailable, with truncation explicitly marked.
 * Truncated paths retain valid UTF-8; embedded NUL keys also mark truncation.
 * byte_offset is a zero-based JSON syntax location, SIZE_MAX otherwise.
 * The complete diagnostic survives release of input and partial owners.
 */
typedef struct vmf_header_json_error {
	vmf_status status;
	vmf_header_json_reason reason;
	const char *message;
	size_t byte_offset;
	char path[512];
	bool path_truncated;
} vmf_header_json_error;

/* Defaults: core limits, 16 MiB JSON, 64 MiB live memory, malloc/free. */
vmf_header_json_options vmf_header_json_default_options(void);

/* Load one strict UTF-8 JSON header in the selected explicit profile. See
 * docs/header-json.md. Optional properties may be omitted; message identities
 * are exact, never wildcard/latest. Unknown, duplicate, profile-incompatible,
 * invalid native header values are rejected. Security bytes are preserved
 * for inspection/replay; this does not authenticate or authorize authoring.
 * data/options/result mandatory; data spans size bytes, no padding required.
 * Empty input is TRUNCATED. Inputs are copied, never retained or modified.
 * On OK *result is an immutable owner, otherwise NULL and no memory survives.
 * INVALID_ARGUMENT denotes API/allocator misuse; MALFORMED/TRUNCATED syntax
 * or JSON definitions; OUT_OF_RANGE native values; LIMIT_EXCEEDED configured
 * bounds; OUT_OF_MEMORY allocator failure. Core validation status preserved.
 * error is optional; all argument storage must be disjoint from input/owners.
 */
vmf_status vmf_header_json_load(vmf_envelope_version profile,
	const char *data, size_t size, const vmf_header_json_options *options,
	vmf_header_json_document **result, vmf_header_json_error *error);

/* Borrow immutable native union/profile; NULL returns NULL/profile value 0.
 * Every pointee survives until destroy. No allocation, mutation, or I/O.
 */
const vmf_header *vmf_header_json_header(
	const vmf_header_json_document *document);
vmf_envelope_version vmf_header_json_profile(
	const vmf_header_json_document *document);

/* NULL-safe; no outstanding borrows may remain. */
void vmf_header_json_destroy(vmf_header_json_document *document);

/* Validate and serialize a revision-specific native header to owned compact
 * UTF-8 JSON. header points to the selected revision struct or union member;
 * all active spans/arrays must be readable for this call. Options/result are
 * mandatory. Absent optional properties are emitted as null/empty arrays to
 * preserve the CLI inspection shape. Security data remains lossless.
 * Size values are preserved; the core counting validator ignores their
 * numeric contents. On failure *result=NULL with complete cleanup.
 * Error, allocator, ownership, disjointness and status contracts follow load.
 */
vmf_status vmf_header_json_serialize(vmf_envelope_version profile,
	const void *header, const vmf_header_json_options *options,
	vmf_header_json_text **result, vmf_header_json_error *error);

/* Generate a minimal valid full-header template for 1..16 exact identities.
 * ids/options/result mandatory. No standard version or identity is inferred.
 * Unsupported profile/identity combinations are rejected by core validation.
 * Defaults: operation/classification zero, retransmit false, precedence 2
 * for B/C, zero for D/D1, except identity UMF 6 uses precedence 5 in D/D1.
 * Multiple descriptors explicitly include zero header/message sizes for later
 * complete-PDU authoring. Optional properties and size values are omitted
 * otherwise. Output/status/lifetime contracts follow serialize.
 */
vmf_status vmf_header_json_template(vmf_envelope_version profile,
	const vmf_message_id *ids, size_t count,
	const vmf_header_json_options *options, vmf_header_json_text **result,
	vmf_header_json_error *error);

/* Borrow NUL-terminated UTF-8 bytes (size excludes terminator). NULL owner
 * returns NULL/zero. No allocation. Bytes survive until text_destroy.
 */
const char *vmf_header_json_text_data(const vmf_header_json_text *text);
size_t vmf_header_json_text_size(const vmf_header_json_text *text);
void vmf_header_json_text_destroy(vmf_header_json_text *text);

#ifdef __cplusplus
}
#endif
#endif /* VMF_HEADER_JSON_H_INCLUDED */
