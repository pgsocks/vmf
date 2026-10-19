#ifndef VMF_SCHEMA_JSON_H_INCLUDED
#define VMF_SCHEMA_JSON_H_INCLUDED

#include <vmf/wire.h>

#ifdef __cplusplus
extern "C" {
#endif

/* An owner of ordinary immutable VMF schemas, not a compiled schema format.
 * All returned descriptors, arrays and strings live until destroy. Destroy
 * every codec borrowing these schemas first. Published schemas may be shared
 * across threads; destruction must not race any access. No API throws.
 */
typedef struct vmf_schema_json_document vmf_schema_json_document;

/* Explicit limits; zero never selects a default. max_memory_bytes bounds all
 * simultaneously live allocations, including owner/allocator metadata,
 * parser storage, conversion scratch and VMF validation scratch. The input
 * buffer and allocator's own bookkeeping are outside this budget.
 * allocator follows vmf_allocator's alignment and paired-callback contract.
 * NULL or two NULL callbacks selects malloc/free. Configuration is copied;
 * allocator user state must survive destruction. Callbacks must not reenter
 * this load/destroy operation. Independent loads are reentrant if their
 * allocator callbacks are. No global allocator hooks are changed.
 */
typedef struct vmf_schema_json_options {
	vmf_limits limits;
	size_t max_json_bytes;
	size_t max_memory_bytes;
	const vmf_allocator *allocator;
} vmf_schema_json_options;

/* Always initialized when supplied. message is static NUL-terminated text,
 * independent of input/document lifetime; it never needs releasing.
 * byte_offset is a zero-based JSON byte offset for syntax errors, SIZE_MAX
 * when unavailable (never a PDU bit offset). schema_index and index_position
 * are zero-based array positions or SIZE_MAX when unavailable. property and
 * related locations preserve core diagnostics without borrowing storage.
 * Diagnostics remain valid after a failed load has released its storage.
 * Success clears all locations, with status OK and message "ok".
 */
typedef struct vmf_schema_json_error {
	vmf_status status;
	const char *message;
	size_t byte_offset;
	size_t schema_index;
	size_t index_position;
	vmf_schema_property property;
	size_t related_index_position;
	uint32_t related_scope_id;
} vmf_schema_json_error;

/* VMF defaults, 16 MiB input, 64 MiB live allocations, default allocator.
 * This function does not allocate.
 */
vmf_schema_json_options vmf_schema_json_default_options(void);

/* Read one complete strict UTF-8 vmf-schema/1 document; see
 * docs/schema-json.md for the complete format. data, options and result are
 * mandatory. data spans size readable bytes; no terminator/padding required.
 * An empty non-NULL buffer is TRUNCATED. Input is not modified or retained.
 * Arguments/output storage must not overlap input or live owned storage.
 * options/allocator configuration need only survive this call.
 *
 * On OK, *result owns a validated native schema set (possibly empty).
 * On every failure, *result is NULL and all partial allocations are freed.
 * error is optional and set on every return. INVALID_ARGUMENT denotes API or
 * allocator misuse; MALFORMED/TRUNCATED denotes JSON syntax; INVALID_SCHEMA
 * denotes definition/type/range/key errors. VMF validation statuses are
 * preserved, including DUPLICATE_MESSAGE and UNSUPPORTED_SCHEMA_FEATURE.
 * LIMIT_EXCEEDED denotes input, memory, arithmetic or schema limits;
 * OUT_OF_MEMORY denotes allocator failure. No file/transport I/O occurs.
 */
vmf_status vmf_schema_json_load(const char *data, size_t size,
	const vmf_schema_json_options *options,
	vmf_schema_json_document **result, vmf_schema_json_error *error);

/* Borrow the immutable native set. NULL document returns NULL. No allocation.
 * The set and every pointee remain valid until document destruction.
 */
const vmf_schema_set *vmf_schema_json_schemas(
	const vmf_schema_json_document *document);

/* NULL-safe; release all owned memory using the copied allocator. No other
 * thread or codec may still access the owner or any of its schema storage.
 */
void vmf_schema_json_destroy(vmf_schema_json_document *document);

#ifdef __cplusplus
}
#endif
#endif /* VMF_SCHEMA_JSON_H_INCLUDED */
