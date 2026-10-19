#ifndef VMF_AUTHOR_H_INCLUDED
#define VMF_AUTHOR_H_INCLUDED

#include <vmf/wire.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Complete-value authoring composes the streaming codec. Registry references
 * are canonical and borrowed; no input is changed or retained after return.
 * Validation helpers require an already validated set. They allocate nothing.
 */

/* A field uses index and scope NONE. A control uses index NULL and GROUP or
 * REPEAT scope. path contains EVERY enclosing repeat, outermost first. An
 * indicator repeat's own count address excludes that repeat. Descriptor
 * indexes include response-only descriptors. schema must belong to the set.
 */
typedef struct vmf_author_address {
	size_t descriptor_index;
	const vmf_message_schema *schema;
	const vmf_index_schema *index;
	vmf_scope_ref scope;
	const vmf_repeat_occurrence *path;
	size_t path_count;
} vmf_author_address;

/* Canonical scope interpretation. controller is NULL for fixed/scalar-count
 * repeats and uncontrolled groups. ancestry borrows canonical repeat storage
 * for controlled groups and all repeats, excluding the scope itself; it is
 * empty for uncontrolled groups. maximum is an exclusive occurrence
 * bound, not the currently selected dynamic count. minimum applies to counts.
 */
typedef struct vmf_scope_info {
	const vmf_index_schema *controller;
	vmf_repeat_path ancestry;
	uint64_t minimum;
	uint64_t maximum;
} vmf_scope_info;

/* result is required and cleared on failure. Missing scope -> INVALID_ARGUMENT.
 * A successful group may have no controller; such groups cannot be asserted.
 */
vmf_status vmf_schema_scope_info(const vmf_schema_set *set,
	const vmf_message_schema *message, vmf_scope_ref scope,
	vmf_scope_info *result);

/* Check canonical membership, complete ancestry and absolute occurrence
 * bounds, including dormant addresses. Descriptor bounds require an envelope
 * and are checked by author_encode. Unsupported controls return
 * UNSUPPORTED_SCHEMA_FEATURE: only GPI presence and FRI/GRI counts are allowed.
 * address is required; it is never changed. No error object is needed.
 */
vmf_status vmf_author_validate_address(const vmf_schema_set *set,
	const vmf_author_address *address);

/* Complete raw value, never a sequence of chunks. The active value union
 * member is kind; text/octet/bit last MUST be true. Empty complete spans are
 * permitted by zero-capacity/variable schemas. Data pointers may be NULL only
 * for empty spans. Scalars, constants, allowed codes, UTF-8, target text
 * encoding, unused high bits and fixed/capacity lengths are all checked.
 * Dynamic lengths are checked against explicit preceding values by encoding.
 */
typedef struct vmf_author_field {
	vmf_author_address address;
	vmf_field_kind kind;
	vmf_field value;
} vmf_author_field;

/* set/field are required. units, if non-NULL, clears on failure and receives
 * wire units (one for scalars), including for literal text. No allocation.
 * Limits are enforced separately by author_encode and its underlying codec.
 */
vmf_status vmf_author_validate_field(const vmf_schema_set *set,
	const vmf_author_field *field, uint64_t *units);

/* kind is PRESENCE (GROUP/GPI) or REPEAT_COUNT (REPEAT/FRI/GRI).
 * FIELD is invalid. Indicator counts are 1..maximum; false presence does not
 * populate parent groups.
 */
typedef struct vmf_author_control {
	vmf_author_address address;
	vmf_encode_request_kind kind;
	union {
		bool present;
		uint64_t repeat_count;
	} as;
} vmf_author_control;

/* Check supported control kind/scope, complete ancestry and count bounds.
 * All parameters required; unknown counts return LENGTH_REQUIRED. Dormant
 * entries receive the same validation. No allocation or input mutation.
 */
vmf_status vmf_author_validate_control(const vmf_schema_set *set,
	const vmf_author_control *control);

/* options_for_schemas provides defaults. max_working_bytes bounds all owned
 * allocations, including codec storage/output, excluding borrowed inputs.
 * output_slice_bytes is positive; small values exercise output exhaustion.
 * max_entries bounds each input array before validation (default 65536).
 * codec allocator callbacks or malloc/free own all storage and must remain
 * callable through result destruction. Helpers do not use global state.
 */
typedef struct vmf_author_options {
	vmf_options codec;
	size_t max_working_bytes;
	size_t output_slice_bytes;
	size_t max_entries;
} vmf_author_options;
vmf_author_options vmf_author_options_for_schemas(const vmf_schema_set *set);

#define VMF_AUTHOR_ERROR_PATH_CAPACITY 64

/* error is initialized on every return. SIZE_MAX means no entry/descriptor.
 * entry_is_control selects which input array entry_index identifies. For a
 * missing mandatory value entry_index is SIZE_MAX and canonical identity plus
 * occurrence path identifies the outstanding request. path_count is the full
 * length; only the first ERROR_PATH_CAPACITY components are copied. codec
 * retains detailed low-level locations when failure came from the codec.
 */
typedef struct vmf_author_error {
	vmf_status status;
	size_t entry_index;
	bool entry_is_control;
	size_t descriptor_index;
	const vmf_message_schema *schema;
	const vmf_index_schema *index;
	vmf_scope_ref scope;
	size_t path_count;
	vmf_repeat_occurrence path[VMF_AUTHOR_ERROR_PATH_CAPACITY];
	vmf_error codec;
} vmf_author_error;

typedef struct vmf_author_result vmf_author_result;

/* Validate authored header/descriptor identities without requesting body
 * values. Same authentication/compression restrictions as encode; validates
 * native revision properties with a bounded counting encoder. No output or
 * retained storage. options/envelope required; error optional and initialized.
 * Envelope size numeric values are ignored according to counting semantics.
 */
vmf_status vmf_author_validate_header(const vmf_author_options *options,
	const vmf_envelope_event *envelope, vmf_author_error *error);

/* Encode one complete PDU synchronously. result is mandatory, set NULL on
 * every failure; error is optional. NULL input arrays require zero counts.
 * All inputs/pointed data must be readable, immutable, and disjoint from
 * outputs throughout this call. Validates EVERY entry, even dormant ones;
 * duplicate logical addresses fail. Every active mandatory field and
 * indicator repeat count must be supplied explicitly. Constants and preceding
 * scalar lengths/counts are never synthesized. Optional field presence is
 * inferred from entries; GPI groups use explicit control or descendant field/
 * control membership at the current path prefix. Conditions, fixed/scalar
 * counts, ordering and padding are owned by the streaming codec.
 *
 * Counts once, derives envelope header/body size fields, then emits using the
 * same complete raw values in bounded slices. Input size values are replaced;
 * their presence may also be derived. No file I/O and no partial result.
 * All security groups/material are rejected for newly authored messages;
 * this convenience performs no authentication. Other native header checks
 * follow the codec. On success, result owns bytes, measurements and activity.
 * Valid dormant entries are successful and have false activity. No warnings
 * are generated; caller decides whether unused data is acceptable.
 *
 * Work includes schema/value validation in both passes and entry/request
 * matching; matching is a bounded linear scan per request. Separate calls
 * are reentrant when inputs/allocators permit; one result is single-owner.
 */
vmf_status vmf_author_encode(const vmf_author_options *options,
	const vmf_envelope_event *envelope, const vmf_author_field *fields,
	size_t field_count, const vmf_author_control *controls,
	size_t control_count, vmf_author_result **result,
	vmf_author_error *error);

/* NULL-safe. Borrowed output lives until result_destroy. A NULL result gives
 * NULL data and zero counts. Returned measurements count octets. Out-of-range
 * activity queries return false; out-of-range measurement returns ARGUMENT
 * and clears its required output. No allocation or mutation.
 */
const uint8_t *vmf_author_result_data(const vmf_author_result *result);
size_t vmf_author_result_size(const vmf_author_result *result);
size_t vmf_author_result_descriptor_count(const vmf_author_result *result);
vmf_status vmf_author_result_measurement(const vmf_author_result *result,
	size_t descriptor, vmf_measurement *measurement);
bool vmf_author_result_field_active(const vmf_author_result *result,
	size_t entry);
bool vmf_author_result_control_active(const vmf_author_result *result,
	size_t entry);
/* NULL-safe; releases all result allocations with its copied allocator. */
void vmf_author_result_destroy(vmf_author_result *result);

#ifdef __cplusplus
}
#endif
#endif
