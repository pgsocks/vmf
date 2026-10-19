#ifndef VMF_H_INCLUDED
#define VMF_H_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>

#if CHAR_BIT != 8
#error "VMF requires eight-bit bytes"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * VMF: C-defined message schemas and a bidirectional event-stream codec.
 *
 * Streaming codec for the profiles and schema features documented below.
 * See docs/format.md for supported scope and revision-specific wire rules.
 *
 * Define ordered message indexes with ordinary C initializers or application
 * code and give their profile-bound registry to the codec. Construction
 * resolves defaults and private traversal storage. No builder or generated
 * source is required.
 *
 * The decoder identifies each message from its envelope metadata, selects a
 * schema, and returns events containing decoded values. The encoder accepts
 * those same events, or traverses the schema and requests application values.
 * Both encoding modes share validation and serialization. Neither direction
 * constructs a whole payload object or performs transport I/O.
 *
 * Schemas, keys, arrays and schema sets are borrowed and immutable.
 * They must remain at stable addresses throughout every codec using them.
 * Handles own bounded storage; schemas and I/O buffers belong to the caller.
 * Mutable codecs are single-threaded and must not be reentered from callbacks.
 * Structs contain logical values, never packed overlays of wire bytes.
 */

/* ---------- Status, bounded buffers and limits ---------- */

/* VMF_OK means one operation/event completed, not that a message is valid. */
typedef enum vmf_status {
	VMF_OK = 0,
	VMF_NEED_INPUT,	 /* Refill input, then resume the same operation. */
	VMF_NEED_OUTPUT, /* Drain output, then resume the same operation. */
	VMF_DONE,	 /* The active framed unit has fully completed. */
	VMF_PAUSED,	 /* Callback paused after consuming an event/reply. */
	VMF_ABORTED,	 /* Callback aborted; reset before reuse. */
	VMF_INVALID_ARGUMENT,
	VMF_INVALID_STATE,
	VMF_INVALID_SCHEMA,
	VMF_UNSUPPORTED_SCHEMA_FEATURE,
	VMF_UNSUPPORTED_ENVELOPE,
	VMF_UNSUPPORTED_ENVELOPE_FEATURE,
	VMF_UNKNOWN_MESSAGE,   /* No schema matches the complete identity. */
	VMF_DUPLICATE_MESSAGE, /* A schema set contains duplicate identities. */
	VMF_LENGTH_REQUIRED,
	VMF_TYPE_MISMATCH,
	VMF_OUT_OF_RANGE,
	VMF_MALFORMED,
	VMF_TRUNCATED,
	VMF_BUFFER_TOO_SMALL,
	VMF_LIMIT_EXCEEDED,
	VMF_OUT_OF_MEMORY,
	VMF_NOT_IMPLEMENTED
} vmf_status;

/* UINT64_MAX is reserved for unknown event lengths, never an actual length. */
#define VMF_UNITS_UNKNOWN UINT64_MAX
#define VMF_LENGTH_UNKNOWN UINT64_MAX

/* A caller-owned block must satisfy both returned size and alignment. */
typedef struct vmf_layout {
	size_t size;
	size_t alignment;
} vmf_layout;

/* Advance used on EVERY return. size and used count octets, not fields.
 * final means no bytes follow this span, not merely "buffer currently empty".
 * Input bytes before used may be released after a codec call. The codec retains
 * partial octets/values itself and never borrows an input pointer across calls.
 */
typedef struct vmf_input {
	const uint8_t *data; /* NULL only when size is zero. */
	size_t size;
	size_t used; /* Input/output; initially 0, always <= size. */
	bool final;
} vmf_input;

/* Only complete octets are published. A pending partial octet stays in state.
 */
typedef struct vmf_output {
	uint8_t *data; /* NULL only when capacity is zero. */
	size_t capacity;
	size_t used; /* Input/output; initially 0, always <= capacity. */
} vmf_output;

/* All members are explicit limits, not zero-means-default settings. Use the
 * defaults function, then override selected members. Limits apply to controls,
 * traversal and allocations as well as data, including zero-bit/absent nodes.
 */
typedef struct vmf_limits {
	size_t max_schemas;
	size_t max_schema_nodes; /* Per message, including normalized scopes. */
	size_t max_depth;	 /* Root is depth 0. */
	size_t max_header_items; /* Aggregate count of variable header items. */
	size_t max_header_bytes; /* Header objects/data and encoder metadata
				  * copies.
				  */
	size_t field_chunk_bytes; /* Decoder-owned chunk space, at least 4. */
	uint64_t max_field_bits;
	uint64_t max_message_octets;
	uint64_t max_pdu_octets;
	uint64_t max_repetitions; /* Per repeat occurrence. */
	uint64_t max_events_per_message;
} vmf_limits;

/* Schema locations are source array positions, never generated IDs.
 * SIZE_MAX means unavailable. offset counts PDU bits; message_index is the
 * envelope descriptor position. Other positions address the native registry.
 */
typedef enum vmf_schema_property {
	VMF_PROPERTY_NONE,
	VMF_PROPERTY_PROFILE,
	VMF_PROPERTY_IDENTITY,
	VMF_PROPERTY_INDEX,
	VMF_PROPERTY_ENCODING,
	VMF_PROPERTY_GROUP,
	VMF_PROPERTY_REPEAT,
	VMF_PROPERTY_INDICATOR,
	VMF_PROPERTY_TARGET,
	VMF_PROPERTY_DEPENDENCY,
	VMF_PROPERTY_CONSTRAINT,
	VMF_PROPERTY_KEY
} vmf_schema_property;
typedef struct vmf_error {
	vmf_status status;
	uint64_t offset;
	size_t index_position;
	size_t message_index;
	size_t schema_index;
	vmf_schema_property property;
	size_t related_index_position;
	uint32_t related_scope_id;
} vmf_error;

/* Return conservative explicit limits; neither function allocates. */
vmf_limits vmf_default_limits(void);
/* Static, NUL-terminated diagnostic; unknown status values return "unknown". */
const char *vmf_status_string(vmf_status status);

/* ---------- Value types exchanged in field events ---------- */

/* The schema selects the active member of vmf_field. */
typedef enum vmf_field_kind {
	VMF_FIELD_BOOL,
	VMF_FIELD_UNSIGNED,
	VMF_FIELD_SIGNED,
	VMF_FIELD_ENUM,
	VMF_FIELD_SCALED,
	VMF_FIELD_TEXT,
	VMF_FIELD_OCTETS,
	VMF_FIELD_BITS
} vmf_field_kind;

/* Individual types may be used without allocating the value union. */
typedef struct vmf_bool_field {
	bool value;
} vmf_bool_field;
typedef struct vmf_unsigned_field {
	uint64_t value;
} vmf_unsigned_field;
typedef struct vmf_signed_field {
	int64_t value;
} vmf_signed_field;
typedef struct vmf_enum_field {
	uint64_t code;
} vmf_enum_field;
/* Exact raw wire code; scale, sign and sentinel semantics are in the schema. */
typedef struct vmf_scaled_field {
	uint64_t code;
} vmf_scaled_field;

/* Chunks contain valid UTF-8, are length-delimited and are not NUL-terminated.
 * size counts API bytes; event offsets/lengths count WIRE text code units.
 */
typedef struct vmf_text_field {
	const char *data;
	size_t size;
	bool last;
} vmf_text_field;

/* An arbitrary octet sequence; no implicit alignment on the wire. */
typedef struct vmf_octets_field {
	const uint8_t *data;
	size_t size;
	bool last;
} vmf_octets_field;

/* API bit i is data[i/8] bit (i%8), independent of physical wire packing.
 * Non-final chunks contain whole octets. Unused final high bits must be zero.
 */
typedef struct vmf_bits_field {
	const uint8_t *data;
	size_t bit_count;
	bool last;
} vmf_bits_field;

/* Reusable value; active member is the event/request's resolved field_kind. */
typedef union vmf_field {
	vmf_bool_field boolean;
	vmf_unsigned_field unsigned_integer;
	vmf_signed_field signed_integer;
	vmf_enum_field enumeration;
	vmf_scaled_field scaled;
	vmf_text_field text;
	vmf_octets_field octets;
	vmf_bits_field bits;
} vmf_field;

/* Return the layout of the individual value struct; unknown kind -> {0,0}. */
vmf_layout vmf_field_layout(vmf_field_kind kind);

/* ---------- Canonical native schemas ---------- */

/* Length-delimited UTF-8 binding data; NULL only with size zero. */
typedef struct vmf_string {
	const char *data;
	size_t size;
} vmf_string;

/* DEFAULT resolves to LSB_FIRST; packing and significance are separate. */
typedef enum vmf_bit_order {
	VMF_BIT_DEFAULT,
	VMF_BIT_MSB_FIRST,
	VMF_BIT_LSB_FIRST
} vmf_bit_order;

/* SIGNED requires an explicit signed representation and width >=2.
 * Other numeric modes default to unsigned. SIGNED rejects negative zero;
 * SCALED retains exact raw codes, including sentinels and negative zero.
 */
typedef enum vmf_integer_representation {
	VMF_INTEGER_DEFAULT,
	VMF_INTEGER_UNSIGNED,
	VMF_INTEGER_TWOS_COMPLEMENT,
	VMF_INTEGER_ONES_COMPLEMENT,
	VMF_INTEGER_SIGN_MAGNITUDE
} vmf_integer_representation;

/* Resolved scalar encoding, returned by inspection. */
typedef struct vmf_integer_encoding {
	uint8_t bit_width;
	vmf_bit_order order;
	vmf_integer_representation representation;
} vmf_integer_encoding;

/* Inclusive, disjoint intervals of exact unsigned raw wire codes. */
typedef struct vmf_code_range {
	uint64_t first;
	uint64_t last;
} vmf_code_range;

typedef struct vmf_rational {
	int64_t numerator;
	uint64_t denominator; /* Nonzero when the rational is used. */
} vmf_rational;

/* Streaming retains raw codes. Sentinel codes bypass factor/bias mapping. */
typedef struct vmf_scale_definition {
	vmf_rational factor;
	vmf_rational bias;
	const uint64_t *sentinels;
	size_t sentinel_count;
} vmf_scale_definition;

typedef struct vmf_optional_u8 {
	bool present;
	uint8_t value;
} vmf_optional_u8;
typedef struct vmf_optional_u32 {
	bool present;
	uint32_t value;
} vmf_optional_u32;
typedef struct vmf_optional_u64 {
	bool present;
	uint64_t value;
} vmf_optional_u64;
typedef struct vmf_optional_i64 {
	bool present;
	int64_t value;
} vmf_optional_i64;

/* Numeric components, compared numerically. A field address is nonempty. */
typedef struct vmf_index_number {
	const uint32_t *parts;
	size_t count;
} vmf_index_number;
typedef uint32_t vmf_group_id;
typedef uint32_t vmf_repeat_id;
typedef struct vmf_group_path {
	const vmf_group_id *parts;
	size_t count;
} vmf_group_path;

/* Only a recurrence controller declares maximum on its final component. */
typedef struct vmf_repeat_component {
	vmf_repeat_id id;
	vmf_optional_u64 maximum;
} vmf_repeat_component;
typedef struct vmf_repeat_path {
	const vmf_repeat_component *parts;
	size_t count;
} vmf_repeat_path;

/* Explicit profile; shortened on-wire version 15 is not a profile. */
typedef enum vmf_envelope_version {
	VMF_ENVELOPE_B = 1,
	VMF_ENVELOPE_C = 2,
	VMF_ENVELOPE_D = 3,
	VMF_ENVELOPE_D1 = 4
} vmf_envelope_version;

typedef struct vmf_message_number {
	uint8_t functional_area; /* VMF: 0..10. */
	uint8_t number; /* 1..127. */
} vmf_message_number;

typedef enum vmf_message_standard {
	VMF_STANDARD_6017,
	VMF_STANDARD_6017A,
	VMF_STANDARD_6017B,
	VMF_STANDARD_6017C,
	VMF_STANDARD_6017D,
	VMF_STANDARD_6017E,
	VMF_STANDARD_TIDP_TE_R2,
	VMF_STANDARD_TIDP_TE_R3,
	VMF_STANDARD_TIDP_TE_R4,
	VMF_STANDARD_TIDP_TE_R5,
	VMF_STANDARD_TIDP_TE_R6,
	VMF_STANDARD_TIDP_TE_R7,
	VMF_STANDARD_TIDP_FTE_R6
} vmf_message_standard;
typedef enum vmf_standard_selection_kind {
	VMF_STANDARD_INHERIT,
	VMF_STANDARD_ABSENT,
	VMF_STANDARD_EDITION,
	VMF_STANDARD_RAW_CODE
} vmf_standard_selection_kind;

/* Message selection replaces the registry selection as one complete value. */
typedef struct vmf_standard_selection {
	vmf_standard_selection_kind kind;
	union {
		vmf_message_standard edition;
		uint8_t raw_code;
	} as;
} vmf_standard_selection;

/* Exact envelope identity, also used for unrestricted header inspection. */
typedef struct vmf_message_id {
	uint8_t user_message_format;
	vmf_optional_u8 standard_version;
	bool identification_present;
	uint8_t functional_area;
	uint8_t message_number;
	vmf_optional_u8 subtype;
} vmf_message_id;

/* Scalars use widths 1..64; BOOL requires 1. BITS/OCTETS/TEXT are fixed
 * unless length_from selects bounded variable length. LITERAL selects bounded
 * ASCII7 termination. SCALED requires exact scale metadata.
 */
typedef enum vmf_encode_type {
	VMF_ENCODE_DEFAULT,
	VMF_ENCODE_BOOL,
	VMF_ENCODE_UNSIGNED,
	VMF_ENCODE_SIGNED,
	VMF_ENCODE_ENUM,
	VMF_ENCODE_SCALED,
	VMF_ENCODE_BITS,
	VMF_ENCODE_OCTETS,
	VMF_ENCODE_TEXT,
	VMF_ENCODE_LITERAL
} vmf_encode_type;
/* DEFAULT resolves to ASCII7. API text chunks are always valid UTF-8;
 * lengths count wire code units. LITERAL permits ASCII7 only.
 */
typedef enum vmf_text_encoding {
	VMF_TEXT_DEFAULT,
	VMF_TEXT_ASCII_7,
	VMF_TEXT_ASCII_8,
	VMF_TEXT_LATIN_1,
	VMF_TEXT_UTF_8
} vmf_text_encoding;

/* Test (preceding raw code & mask) against disjoint inclusive ranges, then
 * optionally negate. Sources must dominate use in the current/ancestor scope.
 */
typedef struct vmf_condition_term {
	vmf_index_number field;
	vmf_optional_u64 mask; /* Omitted means UINT64_MAX. */
	const vmf_code_range *values;
	size_t value_count;
	bool negate;
} vmf_condition_term;
typedef enum vmf_condition_mode {
	VMF_CONDITION_ALL,
	VMF_CONDITION_ANY
} vmf_condition_mode;
/* Nonempty bounded all/any terms, optionally negated as one result. */
typedef struct vmf_condition {
	vmf_condition_mode mode;
	const vmf_condition_term *terms;
	size_t term_count;
	bool negate;
} vmf_condition;

typedef enum vmf_indicator_role {
	VMF_INDICATOR_NONE,
	VMF_INDICATOR_FPI,
	VMF_INDICATOR_GPI,
	VMF_INDICATOR_FRI,
	VMF_INDICATOR_GRI
} vmf_indicator_role;
typedef enum vmf_target_kind {
	VMF_TARGET_NONE,
	VMF_TARGET_INDEX,
	VMF_TARGET_GROUP
} vmf_target_kind;
typedef struct vmf_control_target {
	vmf_target_kind kind;
	union {
		vmf_index_number index;
		vmf_group_id group;
	} as;
} vmf_control_target;

/* Optional refinements are borrowed. Incompatible active properties fail.
 * length_from is empty for fixed data; its presence selects variable length.
 * Its units are bits, octets, or wire text units, according to encode_type.
 * A constant is a required supplied raw code, never an encoder default.
 */
typedef struct vmf_index_refinements {
	vmf_integer_representation representation;
	vmf_bit_order bit_order;
	vmf_text_encoding text_encoding;
	vmf_index_number length_from;
	vmf_optional_i64 length_bias;
	const vmf_condition *when;
	const vmf_code_range *allowed;
	size_t allowed_count;
	vmf_optional_u64 constant;
	const vmf_scale_definition *scale;
} vmf_index_refinements;

/* Authoritative wire order is array order, never sorted index order.
 * Fixed data width must be positive. Variable capacity is finite, possibly 0.
 * Indicator width 0 derives one bit; explicit widths other than 1 fail.
 * Membership omission means no membership; it never copies a previous row.
 */
typedef struct vmf_index_schema {
	vmf_index_number index_number;
	uint64_t number_of_bits;
	vmf_group_path group_code;
	vmf_repeat_path repeat_code;
	vmf_encode_type encode_type;
	vmf_indicator_role indicator;
	vmf_control_target target;
	const vmf_index_refinements *refinements;
	vmf_string key; /* Optional binding key, unique within this message. */
} vmf_index_schema;

typedef enum vmf_repeat_count_kind {
	VMF_REPEAT_FIXED,
	VMF_REPEAT_FIELD
} vmf_repeat_count_kind;
typedef struct vmf_repeat_definition {
	vmf_repeat_id id;
	vmf_repeat_count_kind kind;
	uint64_t count; /* FIXED, including zero. */
	vmf_index_number count_from; /* FIELD: earlier unsigned scalar. */
	vmf_optional_i64 bias;
	vmf_optional_u64 minimum; /* FIELD: omission means zero. */
	uint64_t maximum; /* FIELD: finite inclusive bound. */
} vmf_repeat_definition;

typedef struct vmf_message_schema {
	vmf_message_number message_number;
	const vmf_index_schema *indexes;
	size_t index_count;
	vmf_standard_selection standard;
	vmf_optional_u8 subtype;
	const vmf_repeat_definition *repeats;
	size_t repeat_count;
	vmf_bit_order octet_order;
} vmf_message_schema;

/* Immutable borrowed registry; all nested arrays/strings outlive its codecs. */
typedef struct vmf_schema_set {
	vmf_envelope_version envelope;
	const vmf_message_schema *messages;
	size_t message_count;
	vmf_standard_selection standard;
} vmf_schema_set;

typedef enum vmf_scope_kind {
	VMF_SCOPE_NONE,
	VMF_SCOPE_ROOT,
	VMF_SCOPE_GROUP,
	VMF_SCOPE_REPEAT,
	VMF_SCOPE_REPEAT_BODY
} vmf_scope_kind;
typedef struct vmf_scope_ref {
	vmf_scope_kind kind;
	uint32_t id; /* Group/repeat ID; zero for root/none. */
} vmf_scope_ref;

/* Effective interpretation, independent of codec normalization/lifetimes.
 * Pointers reference canonical registry storage. Indicator mappings remain
 * inspectable although indicators do not emit ordinary FIELD events.
 */
typedef struct vmf_index_info {
	vmf_field_kind kind;
	uint64_t number_of_bits;
	vmf_integer_encoding integer;
	vmf_text_encoding text_encoding;
	uint8_t unit_bits;
	bool variable;
	bool literal;
	uint64_t capacity_units;
	vmf_index_number length_from;
	int64_t length_bias;
	const vmf_code_range *allowed;
	size_t allowed_count;
	vmf_optional_u64 constant;
	const vmf_scale_definition *scale;
	const vmf_condition *when;
	vmf_indicator_role indicator;
	vmf_control_target target;
	vmf_string key;
} vmf_index_info;

typedef struct vmf_allocator vmf_allocator;

/* Inputs must be readable. Output errors are initialized on every return.
 * NULL allocator selects malloc/free; paired callbacks follow codec rules.
 * Validation uses bounded temporary storage, retains nothing and changes no
 * descriptor. Single-message validation includes registry defaults/context.
 */
vmf_status vmf_schema_validate(const vmf_schema_set *set,
	const vmf_message_schema *message, const vmf_limits *limits,
	const vmf_allocator *allocator, vmf_error *error);
vmf_status vmf_schema_set_validate(const vmf_schema_set *set,
	const vmf_limits *limits, const vmf_allocator *allocator,
	vmf_error *error);

/* Exact identity; absent values never mean wildcard/latest. Equality requires
 * two non-NULL structurally valid identities and ignores absent value storage.
 */
bool vmf_message_id_equal(const vmf_message_id *a, const vmf_message_id *b);
/* Resolve a member of set; arguments are mandatory, result clears on failure.
 * No allocation. Invalid selection/layout identity returns INVALID_SCHEMA.
 */
vmf_status vmf_schema_message_id(const vmf_schema_set *set,
	const vmf_message_schema *message, vmf_message_id *result);
/* Select from a validated set. result is mandatory and cleared on failure. */
vmf_status vmf_schema_select(const vmf_schema_set *set,
	const vmf_message_id *id, const vmf_message_schema **result);
/* Native lookups require a validated message; NULL/missing returns NULL. */
const vmf_index_schema *vmf_schema_find(const vmf_message_schema *message,
	vmf_index_number number);
const vmf_index_schema *vmf_schema_find_key(
	const vmf_message_schema *message, vmf_string key);
/* Validated set; message/index must belong to it. Clears result on failure.
 * No allocation. Returned borrowed references require the set to survive.
 */
vmf_status vmf_schema_index_info(const vmf_schema_set *set,
	const vmf_message_schema *message, const vmf_index_schema *index,
	vmf_index_info *result);

/* ---------- Revision-specific logical envelope values ---------- */



/* Optional spans distinguish absence from a present empty value. Header text
 * is ASCII without its wire DELETE terminator; binary spans count octets.
 * All spans are borrowed. Decoder spans live with the decoded header.
 */
typedef struct vmf_header_text {
	bool present;
	vmf_string value;
} vmf_header_text;
typedef struct vmf_header_bytes {
	bool present;
	const uint8_t *data;
	size_t size;
} vmf_header_bytes;

/* URN and name are mutually exclusive. A present group requires exactly one. */
typedef struct vmf_address {
	bool present;
	vmf_optional_u32 urn;
	vmf_header_text name;
} vmf_address;

/* Raw DTG codes, with an optional 12-bit extension. Perishability excludes
 * the extension. Year codes 0..94 mean 2000..2094; 95..99 mean 1995..1999.
 */
typedef struct vmf_dtg {
	bool present;
	uint8_t year, month, day, hour, minute, second;
	vmf_optional_u32 extension;
} vmf_dtg;

/* Three independent one-bit requests inside a presence-controlled group. */
typedef struct vmf_ack_request {
	bool present;
	bool machine, operator_ack, operator_reply;
} vmf_ack_request;

/* Response data carries no user body by itself. RC 1..6 is defined. */
typedef struct vmf_response {
	bool present;
	vmf_dtg acknowledged;
	uint8_t receipt_compliance;
	vmf_optional_u8 cantco_reason;
	vmf_optional_u8 cantpro_reason;
	vmf_header_text amplification;
} vmf_response;

/* B references also carry FAD/message number; later revisions omit them. */
typedef struct vmf_reference {
	vmf_address originator;
	vmf_dtg dtg;
	uint8_t functional_area;
	uint8_t message_number;
} vmf_reference;

/* Security metadata is parsed independently of cryptographic processing.
 * All key tokens share the encoded token length. Arrays have <=17 entries.
 * Presence and raw spans preserve legal omissions, including zero padding.
 */
typedef struct vmf_security {
	bool present;
	uint8_t spi;
	vmf_header_bytes keying_material;
	vmf_header_bytes initialization;
	bool tokens_present;
	const vmf_header_bytes *tokens;
	size_t token_count;
	vmf_header_bytes authentication_a;
	vmf_header_bytes authentication_b;
	bool signed_ack_request;
	bool padding_present;
	uint8_t padding_length;
	vmf_header_bytes padding;
} vmf_security;

/* A handling descriptor is not necessarily a body: response-only descriptors
 * have no payload. has_body is derived/validated from standard conditionality,
 * never encoded as an extra bit. descriptor indexes include acknowledgments.
 * B uses marking_b; C uses marking_c; D/D1 use markings (<=16 9-bit codes).
 * Inactive revision members must be zero. References number at most four.
 */
typedef struct vmf_message_header {
	vmf_message_id id;
	vmf_header_text file_name;
	vmf_optional_u32 message_size_octets;
	uint8_t operation;
	bool retransmit;
	uint8_t precedence;
	uint8_t classification;
	vmf_optional_u32 marking_b;
	vmf_header_text marking_c;
	const uint16_t *markings;
	size_t marking_count;
	vmf_dtg originator_dtg;
	vmf_dtg perishability;
	vmf_ack_request acknowledgment;
	vmf_response response;
	const vmf_reference *references;
	size_t reference_count;
	vmf_security security;
	bool has_body;
} vmf_message_header;

/* Common logical fields; revision constraints remain strict. B has no header
 * size, message-standard version or security group. C has no future-use GPIs.
 * D/D1 future-use groups must all be absent for these explicit profiles.
 * Recipient + information address count <=16; descriptor count is 1..16.
 */
typedef struct vmf_header_fields {
	vmf_optional_u8 compression;
	vmf_optional_u32 header_size_octets;
	vmf_address originator;
	const vmf_address *recipients;
	size_t recipient_count;
	const vmf_address *information_addressees;
	size_t information_addressee_count;
	const vmf_message_header *messages;
	size_t message_count;
} vmf_header_fields;

/* Each revision has its own object layout even where members coincide.
 * shortened is legal only for version 15 under C/D/D1. In that form only
 * compression-absent, originator and recipients are serialized.
 */
typedef struct vmf_header_b {
	vmf_header_fields fields;
} vmf_header_b;
typedef struct vmf_header_c {
	vmf_header_fields fields;
	bool shortened;
} vmf_header_c;
typedef struct vmf_header_d {
	vmf_header_fields fields;
	bool shortened;
} vmf_header_d;
typedef struct vmf_header_d1 {
	vmf_header_fields fields;
	bool shortened;
} vmf_header_d1;
typedef union vmf_header {
	vmf_header_b b;
	vmf_header_c c;
	vmf_header_d d;
	vmf_header_d1 d1;
} vmf_header;

/* Profile struct layout, not serialized size; unknown profile -> {0,0}. */
vmf_layout vmf_header_layout(uint8_t profile);

/* ---------- Events: parsed values, not promises to read a value later
 * ---------- */

/* Scope roles distinguish groups and repeat containers/bodies. MESSAGE_END
 * follows schema, length and padding checks.
 */
typedef enum vmf_event_kind {
	VMF_EVENT_ENVELOPE,
	VMF_EVENT_MESSAGE_BEGIN,
	VMF_EVENT_FIELD,
	VMF_EVENT_GROUP_BEGIN,
	VMF_EVENT_GROUP_END,
	VMF_EVENT_ABSENT,
	VMF_EVENT_MESSAGE_END,
	VMF_EVENT_PDU_END
} vmf_event_kind;

/* header points to the individual revision-specific struct or union member.
 * object_size is sizeof that struct, never the serialized header length.
 * header_octets is measured, including zero padding. pdu_octets is UNKNOWN
 * if a body lacks a MessageSize; there is no external framing input.
 */
typedef struct vmf_envelope_event {
	uint8_t wire_version;
	const void *header;
	size_t object_size;
	uint64_t header_octets;
	uint64_t pdu_octets;
} vmf_envelope_event;

/* Identity and boundary for a selected body. index identifies its handling
 * descriptor, including preceding descriptors without bodies. BEGIN and END
 * carry these values. Encoder END may use UNKNOWN for payload_octets to request
 * an actual-length check against BEGIN rather than repeating the numeric size.
 */
typedef struct vmf_message_event {
	vmf_message_id id;
	size_t index;
	uint64_t payload_octets; /* Includes final payload padding. */
} vmf_message_event;

/* For REPEAT: count at BEGIN, actual count at END. Indicator-driven BEGIN may
 * be UNKNOWN. Other groups and repeat bodies use count 1. No count is a byte
 * size.
 */
typedef struct vmf_group_event {
	uint64_t count;
	bool more_present; /* Leading-repeat body BEGIN only. */
	bool more; /* Logical next occurrence exists, independent of polarity.
		    */
} vmf_group_event;

/* A scalar is one complete event: offset_units=0, total_units=1. Variable-size
 * fields emit one or more events: contiguous wire-unit offsets, same total,
 * last in the individual value struct. Empty field = one empty final event.
 * Non-final chunks must contain at least one wire unit. A final empty chunk
 * is allowed only for an empty field; no extra empty event follows its last
 * data. TEXT units are wire code units, OCTETS units are octets, BITS units are
 * bits. Literal text fields report UNKNOWN totals;
 * their final chunk ends the value. Other reader totals precede field data.
 */
typedef struct vmf_field_event {
	uint64_t offset_units;
	uint64_t total_units;
	vmf_field value; /* Active member from event.field_kind. */
} vmf_field_event;

/* One scalar or variable-field chunk per FIELD event. index is the original
 * canonical definition for FIELD/field ABSENT; structural events use scope.
 * Field kind is resolved, including DEFAULT encodings. schema identifies the
 * canonical message. Root depth is zero; repeat bodies carry occurrence and
 * leading recurrence metadata. Independently created codecs sharing a registry
 * accept replayed references. No pointer refers to private normalized storage.
 * ENVELOPE/PDU_END have no schema/index/scope; MESSAGE events have only schema.
 */
typedef struct vmf_event {
	vmf_event_kind kind;
	const vmf_message_schema *schema;
	const vmf_index_schema *index;
	vmf_scope_ref scope;
	vmf_field_kind field_kind; /* Resolved FIELD value union member. */
	size_t depth;
	uint64_t occurrence;
	union {
		vmf_envelope_event envelope;
		vmf_message_event message;
		vmf_group_event group;
		vmf_field_event field;
	} as;
} vmf_event;

/* ---------- Schema-driven encoding requests and replies ---------- */

/* FIELD supplies a raw value/chunk; PRESENCE decides bit-controlled optional
 * presence; REPEAT_COUNT supplies a known indicator-driven repeat count.
 * Fixed/scalar counts and conditional presence require no application reply.
 */
typedef enum vmf_encode_request_kind {
	VMF_ENCODE_REQUEST_FIELD,
	VMF_ENCODE_REQUEST_PRESENCE,
	VMF_ENCODE_REQUEST_REPEAT_COUNT
} vmf_encode_request_kind;

/* One zero-based repeat-body occurrence in the current canonical message. */
typedef struct vmf_repeat_occurrence {
	vmf_repeat_id id;
	uint64_t occurrence;
} vmf_repeat_occurrence;

/* descriptor_index includes descriptors without bodies. schema and index are
 * borrowed canonical definitions, never normalized nodes. FIELD and optional
 * field PRESENCE use index with scope NONE; optional group PRESENCE uses a
 * GROUP scope and NULL index; REPEAT_COUNT uses a REPEAT scope and NULL index.
 * path lists every enclosing repeat body, outermost first. A repeat's own
 * count request precedes its first body and excludes that repeat from path.
 * No enclosing repeats means path=NULL and path_count=0.
 * field_kind/offset_units/total_units apply only to FIELD. Units match FIELD
 * events; scalars use offset 0 and total 1, literal text uses UNKNOWN total.
 * The request/path remains unchanged until its reply is consumed, successful
 * begin, reset or destruction. Copying the struct does not copy path storage.
 */
typedef struct vmf_encode_request {
	vmf_encode_request_kind kind;
	size_t descriptor_index;
	const vmf_message_schema *schema;
	const vmf_index_schema *index;
	vmf_scope_ref scope;
	const vmf_repeat_occurrence *path;
	size_t path_count;
	vmf_field_kind field_kind;
	uint64_t offset_units;
	uint64_t total_units;
} vmf_encode_request;

/* kind must match the outstanding request. FIELD uses its resolved kind to
 * select the active vmf_field member. Text/octet/bit data is borrowed through
 * reply consumption, including all NEED_OUTPUT retries; no chunk is copied.
 * present=false omits the controlled region; zero/empty FIELD is still data.
 * repeat_count is known, never VMF_UNITS_UNKNOWN; schema/codec bounds apply.
 * Constants and explicit scalar length/count fields remain FIELD replies.
 */
typedef struct vmf_encode_reply {
	vmf_encode_request_kind kind;
	union {
		vmf_field field;
		bool present;
		uint64_t repeat_count;
	} as;
} vmf_encode_reply;

/* Read and write share balanced ENVELOPE, MESSAGE_BEGIN, root/group/field,
 * MESSAGE_END, PDU_END events. An absent region emits one ABSENT.
 * Repeats have container and per-occurrence body boundaries, including an empty
 * container for count zero. Indicators are controls, not ordinary data events.
 * Explicit scalar length/count fields remain events and are never backpatched.
 */

/* ---------- Owned bounded state ---------- */

typedef struct vmf_decoder vmf_decoder;
typedef struct vmf_encoder vmf_encoder;

/* Both callbacks or neither. allocate must return max_align_t-aligned memory
 * or NULL; deallocate receives only non-NULL successful allocations. user is
 * borrowed through destruction. Configuration is copied, never retained by
 * pointer. Callbacks must not reenter this handle. Defaults are malloc/free.
 */
typedef void *(*vmf_allocate)(void *user, size_t size);
typedef void (*vmf_deallocate)(void *user, void *memory);
struct vmf_allocator {
	vmf_allocate allocate;
	vmf_deallocate deallocate;
	void *user;
};

/* Mandatory limits, explicit profile and optional immutable schemas/allocator.
 * schemas==NULL means an empty set, permitting independent envelope work.
 * Creation validates schemas, checks size arithmetic, allocates bounded state
 * and copies options. It releases every partial allocation on failure.
 * result is mandatory and set NULL on failure; error is optional and always
 * set. Allocation failure -> OUT_OF_MEMORY. No allocation after creation.
 */
typedef struct vmf_options {
	vmf_envelope_version profile;
	vmf_limits limits;
	const vmf_schema_set *schemas;
	const vmf_allocator *allocator;
} vmf_options;
/* Borrow schemas and select its profile with default limits/allocator.
 * schemas is mandatory. This allocates nothing; creation still validates.
 */
vmf_options vmf_options_for_schemas(const vmf_schema_set *schemas);
vmf_status vmf_decoder_create(
	const vmf_options *options, vmf_decoder **result, vmf_error *error);
vmf_status vmf_encoder_create(const vmf_options *options, bool counting_only,
	vmf_encoder **result, vmf_error *error);

/* NULL-safe. Destroy releases all owned storage with the copied allocator.
 * Reset aborts pending work, invalidates headers/chunks, retains configuration
 * and clears counters/errors; it allocates nothing and returns to idle.
 */
void vmf_decoder_destroy(vmf_decoder *decoder);
void vmf_encoder_destroy(vmf_encoder *encoder);
void vmf_decoder_reset(vmf_decoder *decoder);
void vmf_encoder_reset(vmf_encoder *encoder);

/* NULL -> INVALID_ARGUMENT, offset zero and unavailable locations. */
vmf_error vmf_decoder_error(const vmf_decoder *decoder);
vmf_error vmf_encoder_error(const vmf_encoder *encoder);
/* Logical bits since reset, including pending bits; NULL -> 0. */
uint64_t vmf_encoder_bit_count(const vmf_encoder *encoder);

/* Begin one complete PDU or independent envelope at an octet boundary.
 * Requires idle/DONE. Failure preserves state and retained header. Success
 * invalidates the previous header. No input consumed by begin. Version bits
 * must match the configured profile; C/D/D1 also allow shortened version 15.
 * No automatic revision switch. Envelope mode next emits ENVELOPE then DONE;
 * full mode emits the grammar above. Headers require no payload schemas.
 * Full mode uses MessageSize when present. A legal sole descriptor without
 * MessageSize is delimited by input.final; BEGIN length is UNKNOWN and END
 * carries actual padded octets. A completed fixed schema waits for final and
 * rejects extra octets. Sized PDUs preserve subsequent concatenated bytes;
 * an unsized PDU consumes its logical record through final.
 * Compression/security metadata is exposed first; unsupported transforms fail
 * before traversing the affected body. Authentication is never verified here.
 */
vmf_status vmf_decoder_begin_pdu(vmf_decoder *decoder);
vmf_status vmf_decoder_begin_envelope(vmf_decoder *decoder);

/* One event per OK. NEED_INPUT retains partial state; final exhaustion before
 * completion -> TRUNCATED. input.used advances on every return, never beyond
 * this PDU. event is zeroed on every non-OK return. Header and its pointees
 * survive until successful begin/reset/destroy. Field chunks survive only
 * until the next next/dispatch/reset/destroy. Input buffers are never retained.
 * Arguments are mandatory, disjoint from each other and handle storage.
 */
vmf_status vmf_decoder_next(
	vmf_decoder *decoder, vmf_input *input, vmf_event *event);

/* Callback pause consumes the event; abort is terminal until reset. */
typedef enum vmf_event_action {
	VMF_EVENT_CONTINUE,
	VMF_EVENT_PAUSE,
	VMF_EVENT_ABORT
} vmf_event_action;
typedef vmf_event_action (*vmf_event_handler)(
	void *user, const vmf_event *event);
/* Adapter over next. All except user mandatory; *delivered counts callbacks,
 * including pause/abort, on every return. Unknown actions are terminal errors.
 * Event data is borrowed during callback; do not reenter this decoder.
 */
vmf_status vmf_decoder_dispatch(vmf_decoder *decoder, vmf_input *input,
	vmf_event_handler handler, void *user, size_t *delivered);

/* Provider writes one reply to request and returns CONTINUE or PAUSE. PAUSE
 * takes effect only after that reply is consumed. ABORT ignores reply and is
 * terminal until reset; an unknown action is terminal INVALID_ARGUMENT.
 * Request/path data is borrowed; do not reenter this encoder. Reply chunk
 * storage must survive output exhaustion until the cached reply is consumed.
 */
typedef vmf_event_action (*vmf_encode_provider)(void *user,
	const vmf_encode_request *request, vmf_encode_reply *reply);

/* Validate and copy one envelope and all header pointees, producing no output.
 * Both arguments are mandatory and must not overlap handle/schema storage.
 * Requires idle/completed state. Success starts a managed PDU, invalidates the
 * prior request/header and clears measurements, retaining bits since reset.
 * Failure preserves usable state and completed measurements. The envelope and
 * its pointees may be released after OK. Header/body size values follow write
 * and counting rules; derived event sizes are measured by the codec.
 * Compression and unsupported security fail when selecting affected bodies.
 * Manual write/write_field and managed encoding cannot mix in an active PDU.
 * Either mode may start after full PDU completion or reset; errors require
 * reset except recoverable argument/kind/call-order errors described below.
 */
vmf_status vmf_encoder_begin_pdu(
	vmf_encoder *encoder, const vmf_envelope_event *envelope);

/* Advance managed traversal, generating structure, controls and padding.
 * OK returns one request. Before responding, repeated calls return the same
 * request without advancing. NEED_OUTPUT retains automatic progress; DONE
 * means this PDU is complete and may be returned repeatedly. request is
 * zeroed on every non-OK return. encoder/request are mandatory; output is
 * mandatory outside counting mode, NULL in counting mode. output.used advances
 * on every return. Buffers must be disjoint from handle/schema/request storage.
 * A pending NEED_OUTPUT reply must resume through its original entry point;
 * next_request then returns recoverable INVALID_STATE, preserving that work.
 */
vmf_status vmf_encoder_next_request(vmf_encoder *encoder,
	vmf_output *output, vmf_encode_request *request);

/* Consume one reply to the outstanding request. OK means the entire reply is
 * consumed; next_request advances to further requests or completion. On
 * NEED_OUTPUT retry respond with identical active reply members and unchanged
 * chunk pointees; their storage remains borrowed. The reply struct itself may
 * move. Changed retries/invalid tags return recoverable INVALID_ARGUMENT;
 * a valid mismatched kind returns recoverable TYPE_MISMATCH. These and wrong
 * entry points (INVALID_STATE) preserve the request and pending work. Other
 * data/traversal/resource failures are terminal until reset; discard output.
 * An unknown repeat_count returns terminal LENGTH_REQUIRED. Both encoder and
 * reply are mandatory. Output/nullability/disjointness follow next_request;
 * output must also be disjoint from reply and any borrowed chunk storage.
 */
vmf_status vmf_encoder_respond(vmf_encoder *encoder, vmf_output *output,
	const vmf_encode_reply *reply);

/* Compose next_request/respond with one provider call per new reply. All
 * except user/output are mandatory; output follows counting rules above.
 * delivered is zeroed first and counts callbacks on every return, including
 * PAUSE/ABORT. NEED_OUTPUT caches reply/action and never recalls the provider
 * for that reply. Retry dispatch with the same provider/user; changed identity
 * is recoverable INVALID_ARGUMENT. Primitive entry points cannot take over a
 * cached pending reply. PAUSED follows consumption, possibly on a later call
 * with delivered=0. ABORTED requires reset. Recoverable reply-validation errors
 * preserve the request and discard the unconsumed callback reply, permitting
 * another provider or primitives. No pending reply means either may resume.
 * delivered must not overlap I/O, schema, callback data or handle storage.
 */
vmf_status vmf_encoder_dispatch(vmf_encoder *encoder, vmf_output *output,
	vmf_encode_provider provider, void *user, size_t *delivered);

/* Begin independent header output; write then accepts only ENVELOPE.
 * Default idle write accepts ENVELOPE followed by all body events and PDU_END.
 * Header-only completion returns OK then requires begin/reset for reuse.
 */
vmf_status vmf_encoder_begin_envelope(vmf_encoder *encoder);

/* Accept one event from the shared grammar. Canonical field/scope identities,
 * nesting, counts, chunks, value constraints and padding must match. Body
 * boundaries use MessageSize only; HeaderSize, when present, must equal the
 * measured padded header. Header ranges/size are checked before output; actual
 * body sizes are checked at MESSAGE_END. A legal sole unsized descriptor may
 * use UNKNOWN event lengths; PDU_END marks the caller record end, adding no
 * wire delimiter. No backpatching. Envelope metadata is copied on OK.
 * NEED_OUTPUT requires retrying the identical event and unchanged pointees;
 * partial progress never duplicates bits. output.used advances on every return.
 * Other errors are terminal until reset; discard previously produced output.
 * Counting mode requires output=NULL and validates size-field presence while
 * ignoring their numeric values. It measures the full envelope and each body;
 * replay events with populated lengths to a normal encoder. Literal fields
 * may report UNKNOWN totals and terminate at their last chunk.
 * encoder/event mandatory; output mandatory outside counting mode. No aliasing
 * with handle/schema/event storage. Returns OK, NEED_OUTPUT or an error.
 */
vmf_status vmf_encoder_write(
	vmf_encoder *encoder, vmf_output *output, const vmf_event *event);

/* Per-descriptor measurements are available after its MESSAGE_END (zero for
 * descriptors without bodies); PDU total only after PDU_END. Values are octets.
 * result mandatory, zeroed on failure; invalid index -> INVALID_ARGUMENT;
 * incomplete measurement -> INVALID_STATE. Reset clears measurements.
 */
typedef struct vmf_measurement {
	uint64_t header_octets;
	uint64_t body_octets;
	uint64_t pdu_octets;
} vmf_measurement;
vmf_status vmf_encoder_measurement(
	const vmf_encoder *encoder, size_t descriptor, vmf_measurement *result);

/* FIELD convenience composes write; value points to its individual value struct
 * of exactly field_layout(kind).size. Infers nesting, occurrence and offset.
 * NEED_OUTPUT must retry this entry point with identical arguments.
 */
vmf_status vmf_encoder_write_field(vmf_encoder *encoder, vmf_output *output,
	const vmf_index_schema *index, const void *value, size_t value_size,
	uint64_t total_units);

/* Copy individual value struct, not pointed-to chunk data. Failure leaves
 * value untouched. Requires FIELD event, original field index, exact size and
 * aligned nonoverlapping output. Does not mutate a codec.
 */
vmf_status vmf_event_copy_field(
	const vmf_event *event, void *value, size_t value_size);

/* Malformed wire, traversal/schema and resource errors are terminal until
 * reset. Invalid arguments, managed reply-kind errors, wrong managed entry
 * points and failed begin preserve usable state and retained headers.
 * Handles are single-threaded/non-reentrant; distinct handles can use
 * shared immutable schemas concurrently. No exceptions cross the API.
 */

#ifdef __cplusplus
}
#endif
#endif /* VMF_H_INCLUDED */
