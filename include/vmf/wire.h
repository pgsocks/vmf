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
 * A vmf_message_schema IS the schema. Define it and its vmf_node tree using
 * ordinary C initializers or application code. Give a set of these schemas
 * to the codec. There is no schema parser, compiler, builder object, generated
 * source requirement, or second opaque schema representation in this API.
 *
 * The decoder identifies each message from its envelope metadata, selects a
 * schema, and returns events containing decoded values. The encoder accepts
 * those same events and uses the selected schema to validate and encode them.
 * Neither direction constructs a whole payload object or performs transport
 * I/O.
 *
 * Schemas, names, arrays and schema sets are borrowed, immutable and shareable.
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
	VMF_PAUSED,	 /* Callback consumed an event and requested pause. */
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
	size_t max_schema_nodes; /* Per schema; includes every choice arm. */
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

/* Optional error output. offset is a bit offset relative to the active PDU;
 * node_id is 0 when not applicable. Schema errors use offset 0. message_index
 * is zero-based; SIZE_MAX means no message is selected.
 */
typedef struct vmf_error {
	vmf_status status;
	uint64_t offset;
	uint32_t node_id;
	size_t message_index;
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

/* Reusable storage for any value; metadata and type are in the field's node. */
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

/* ---------- Public C schema types ---------- */
/* Nonzero, caller-assigned identity, unique throughout one schema tree. */
typedef uint32_t vmf_node_id;

/* No node/reference. The actual root has a nonzero ID like any other node. */
#define VMF_NODE_NONE ((vmf_node_id)0)

/* Length-delimited UTF-8 metadata; not required to be NUL-terminated. */
typedef struct vmf_string {
	const char *data; /* May be NULL only when size is zero. */
	size_t size;	  /* Number of bytes, excluding any terminator. */
} vmf_string;

/* Logical significance order within an integer or encoded text/octet unit.
 * This does not change the codec's physical packing of bits into wire octets.
 */
typedef enum vmf_bit_order {
	VMF_BIT_MSB_FIRST, /* Emit the most significant bit first. */
	VMF_BIT_LSB_FIRST  /* Emit the least significant bit first. */
} vmf_bit_order;

/* Numeric interpretation of a fixed-width, right-justified integer code. */
typedef enum vmf_integer_representation {
	VMF_INTEGER_UNSIGNED,
	VMF_INTEGER_TWOS_COMPLEMENT,
	VMF_INTEGER_ONES_COMPLEMENT,
	VMF_INTEGER_SIGN_MAGNITUDE
} vmf_integer_representation;

/* Scalar codes are at most 64 bits. Use VMF_FIELD_BITS for wider values. */
typedef struct vmf_integer_encoding {
	uint8_t bit_width;   /* 1..64; signed forms require at least 2. */
	vmf_bit_order order; /* Order of code bits on the logical stream. */
	vmf_integer_representation representation;
} vmf_integer_encoding;

/* Inclusive interval of unsigned, right-justified raw wire codes. */
typedef struct vmf_code_range {
	uint64_t first;
	uint64_t last; /* Must be >= first. */
} vmf_code_range;

/* Optional name for an enumeration code or a scaled-value sentinel. */
typedef struct vmf_code_label {
	uint64_t code;
	vmf_string name;
} vmf_code_label;

/* Exact rational metadata; a used denominator must be nonzero. */
typedef struct vmf_rational {
	int64_t numerator;
	uint64_t denominator;
} vmf_rational;

/* For non-sentinel codes, physical value = decoded_integer * factor + bias.
 * The streaming API still exchanges the exact raw code, not a float.
 */
typedef struct vmf_scale_definition {
	vmf_rational factor;
	vmf_rational bias;
	const vmf_code_label *sentinels; /* Bypass the numeric scale mapping. */
	size_t sentinel_count;
} vmf_scale_definition;

/* Scalar format and code constraints for BOOL/UNSIGNED/SIGNED/ENUM/SCALED.
 * An empty allowed set means all representable codes. Otherwise the code
 * must fall within at least one interval; intervals must be disjoint.
 * Signed numeric ranges must be mapped to raw-code intervals by the caller.
 * Labels are metadata, not an implicit closed enumeration constraint.
 */
typedef struct vmf_scalar_definition {
	vmf_integer_encoding encoding;
	const vmf_code_range *allowed;
	size_t allowed_count;
	const vmf_code_label
		*labels; /* Codes and nonempty names must be unique. */
	size_t label_count;
	vmf_scale_definition scale; /* Used only by VMF_FIELD_SCALED. */
} vmf_scalar_definition;

/* Compare a preceding scalar's raw code, masked before interval testing.
 * A term is true when (code & mask) belongs to any supplied interval, then
 * inverted when negate is true. A term requires at least one interval.
 */
typedef struct vmf_condition_term {
	vmf_node_id field;
	uint64_t mask; /* UINT64_MAX tests the entire raw code. */
	const vmf_code_range *values;
	size_t value_count;
	bool negate;
} vmf_condition_term;

/* How to combine the terms of one condition. */
typedef enum vmf_condition_mode {
	VMF_CONDITION_ALL, /* Logical AND of all terms. */
	VMF_CONDITION_ANY  /* Logical OR of all terms. */
} vmf_condition_mode;

/* Conditions contain at least one term; no callbacks or source expressions.
 * Terms read already completed scalar occurrences, never future values.
 */
typedef struct vmf_condition {
	vmf_condition_mode mode;
	const vmf_condition_term *terms;
	size_t term_count;
	bool negate; /* Invert the combined result. */
} vmf_condition;

/* Whether a node exists in the current parent occurrence. */
typedef enum vmf_presence_kind {
	VMF_PRESENCE_ALWAYS,   /* No presence information on the wire. */
	VMF_PRESENCE_BIT,      /* One leading presence bit. */
	VMF_PRESENCE_CONDITION /* Determined by preceding scalar values. */
} vmf_presence_kind;

/* Presence is evaluated/emitted before the node's own length/count prefix.
 * An absent node emits one ABSENT event, with no body or child events.
 */
typedef struct vmf_presence_definition {
	vmf_presence_kind kind;
	bool present_bit;	 /* For BIT: bit value that means present. */
	vmf_condition condition; /* Used only by CONDITION. */
} vmf_presence_definition;

/* How a field length or a repeat count is obtained. */
typedef enum vmf_extent_kind {
	VMF_EXTENT_FIXED,	/* Use fixed directly; no wire control. */
	VMF_EXTENT_PREFIX,	/* Read/write an unsigned integer prefix. */
	VMF_EXTENT_FIELD,	/* Use an earlier unsigned scalar code. */
	VMF_EXTENT_REMAINING,	/* Terminal field consumes bounded remainder. */
	VMF_EXTENT_MORE_BEFORE, /* Repeat only: flag before each occurrence. */
	VMF_EXTENT_MORE_AFTER,	/* Repeat only: flag after each occurrence. */
	VMF_EXTENT_RECURRENCE_BEFORE, /* [more, body], including last body. */
	VMF_EXTENT_LITERAL /* ASCII7 text: DELETE or maximum terminates. */
} vmf_extent_kind;

/* Zero maximum means zero, not unlimited; UINT64_MAX means no schema maximum.
 * Configured vmf_limits still apply. UINT64_MAX is reserved as the stream
 * API's UNKNOWN sentinel and cannot be an actual extent. Arithmetic must
 * not wrap; an absent schema maximum does not disable these checks.
 * Field lengths use FIXED/PREFIX/FIELD/REMAINING; ASCII7 text also permits
 * LITERAL. LITERAL bounds count data characters excluding DELETE, with a
 * finite maximum. DELETE is emitted below maximum, omitted at maximum.
 * For a repeat count, REMAINING is invalid. FIELD requires an earlier
 * BOOL/UNSIGNED/ENUM scalar, not a signed or scaled value.
 * PREFIX/FIELD: extent = unsigned code + bias, checked in the mathematical
 * integers before narrowing to uint64_t. Other kinds require bias == 0.
 */
typedef struct vmf_extent_definition {
	vmf_extent_kind kind;
	uint64_t fixed;		     /* Used by FIXED. */
	vmf_integer_encoding prefix; /* Used by PREFIX; must be unsigned. */
	vmf_node_id field; /* Used by FIELD; never an implicit prefix. */
	int64_t bias;	   /* Used only by PREFIX/FIELD. */
	bool more_bit;	   /* Used by MORE_BEFORE/MORE_AFTER. */
	uint64_t minimum;  /* Inclusive bound on the logical extent. */
	uint64_t maximum;  /* Inclusive bound; must be >= minimum. */
} vmf_extent_definition;

/* Built-in wire text encodings. More encodings require explicit API support;
 * the codec must not substitute one for an unsupported encoding.
 */
typedef enum vmf_text_encoding {
	VMF_TEXT_ASCII_7, /* Seven-bit ASCII code units. */
	VMF_TEXT_ASCII_8, /* Eight-bit units; codes > 127 invalid. */
	VMF_TEXT_LATIN_1, /* Eight-bit ISO-8859-1 code units. */
	VMF_TEXT_UTF_8	  /* Eight-bit UTF-8 code units. */
} vmf_text_encoding;

/* Text lengths and field_event.total_units count WIRE code units, not output
 * bytes or grapheme clusters. UTF-8 therefore counts octets; ASCII/Latin-1
 * count their respective encoded characters. API chunks are always valid UTF-8.
 * Invalid wire text or input UTF-8 is rejected, not silently replaced.
 */
typedef struct vmf_text_definition {
	vmf_text_encoding encoding;
	vmf_bit_order order; /* Within each seven/eight-bit code unit. */
	vmf_extent_definition length;
} vmf_text_definition;

/* Lengths and field_event.total_units are octets; no implicit byte alignment.
 */
typedef struct vmf_octets_definition {
	vmf_bit_order order; /* Within each eight-bit octet. */
	vmf_extent_definition length;
} vmf_octets_definition;

/* Lengths and field_event.total_units are bits. Logical bit zero from the API's
 * vmf_bits_field buffer is the first bit on the logical wire stream.
 */
typedef struct vmf_bits_definition {
	vmf_extent_definition length;
} vmf_bits_definition;

/* Logical field type and its corresponding native wire-format definition.
 * BOOL: unsigned, one-bit encoding, with codes 0=false and 1=true.
 * UNSIGNED/ENUM: unsigned encoding. SIGNED: a signed encoding.
 * SCALED: any integer representation; vmf_scaled_field retains its raw code.
 * Negative-zero codes in signed representations are rejected for SIGNED;
 * use SCALED or BITS when preserving such an exact code is necessary.
 */
typedef struct vmf_field_schema {
	vmf_field_kind kind;
	union {
		vmf_scalar_definition
			scalar; /* BOOL/UNSIGNED/SIGNED/ENUM/SCALED. */
		vmf_text_definition text;     /* TEXT. */
		vmf_octets_definition octets; /* OCTETS. */
		vmf_bits_definition bits;     /* BITS. */
	} as;
} vmf_field_schema;

/* Public schema nodes, traversed directly by both codecs. */
typedef struct vmf_node vmf_node;

/* A sequence of children in their exact logical wire/event order. */
typedef struct vmf_group_schema {
	const vmf_node *children;
	size_t child_count;
} vmf_group_schema;

/* A repeat container always has its own GROUP_BEGIN/GROUP_END, even for
 * zero occurrences. Each occurrence traverses body, which must be an
 * unconditional GROUP and supplies explicit per-occurrence boundaries.
 * REMAINING is not a valid repeat count mode.
 * MORE_BEFORE: [more_bit, body] ... [!more_bit]; zero bodies are possible.
 * MORE_AFTER:  [body, flag] ...; the last flag is !more_bit; minimum >= 1.
 * RECURRENCE_BEFORE: [flag, body] ...; final flag is !more_bit; minimum >=1.
 * A leading-repeat body GROUP_BEGIN carries more_present/more on decode.
 * Encoding unknown counts requires that metadata; known counts derive it
 * and validate supplied metadata. No separate terminal flag is emitted.
 * Validation rejects a potentially nonempty repeat when neither its
 * controls nor its body guarantee consuming at least one bit per item;
 * payload-size limits must also bound traversal work, not just byte I/O.
 */
typedef struct vmf_repeat_schema {
	vmf_extent_definition count;
	const vmf_node *body;
} vmf_repeat_schema;

/* One choice arm. Code intervals across ALL arms must be disjoint. Tests
 * use the masked selector, not masked interval endpoints. Each arm must
 * have at least one interval.
 */
typedef struct vmf_choice_case {
	const vmf_code_range *values;
	size_t value_count;
	const vmf_node *body; /* Unconditional FIELD/GROUP/REPEAT/CHOICE. */
} vmf_choice_case;

/* Select one arm from an earlier scalar's raw code & mask. The choice has
 * GROUP_BEGIN/GROUP_END events surrounding the selected arm. Unselected
 * arms produce no events. No match without otherwise is a runtime error.
 */
typedef struct vmf_choice_schema {
	vmf_node_id selector;
	uint64_t mask;
	const vmf_choice_case *cases;
	size_t case_count;
	const vmf_node *otherwise; /* Optional unconditional fallback. */
} vmf_choice_schema;

/* All non-FIELD kinds produce balanced GROUP_BEGIN/GROUP_END events. */
typedef enum vmf_node_kind {
	VMF_NODE_FIELD,
	VMF_NODE_GROUP,
	VMF_NODE_REPEAT,
	VMF_NODE_CHOICE
} vmf_node_kind;

/* One node in an acyclic, unshared schema tree; IDs must be unique.
 * Names are optional metadata; IDs, not names, identify dependency sources.
 * Names, IDs, and labels have no wire representation. A tree can be shared
 * by different message schemas, but a node occurs at only one position
 * within any single tree. Runtime repetitions do not duplicate descriptors.
 */
struct vmf_node {
	vmf_node_id id;
	const char *name; /* Optional NUL-terminated metadata. */
	vmf_presence_definition presence;
	vmf_node_kind kind;
	union {
		vmf_field_schema field;
		vmf_group_schema group;
		vmf_repeat_schema repeat;
		vmf_choice_schema choice;
	} as;
};

/* ---------- Message identification and directly usable schemas ---------- */

/* An absent optional value is distinct from a present value equal to zero. */
typedef struct vmf_optional_u8 {
	bool present;
	uint8_t value; /* Ignored when absent. */
} vmf_optional_u8;

/* Optional header size or message size. */
typedef struct vmf_optional_u32 {
	bool present;
	uint32_t value; /* Ignored when absent. */
} vmf_optional_u32;

/* Semantic message identity, extracted from an application envelope. This is
 * NOT a new wire prefix. standard_version describes the MESSAGE standard, not
 * the envelope revision. The field names do not imply that every envelope
 * revision carries every item.
 */
typedef struct vmf_message_id {
	uint8_t user_message_format;
	vmf_optional_u8 standard_version;
	bool identification_present;
	uint8_t functional_area; /* Ignored when identification is absent. */
	uint8_t message_number;	 /* Ignored when identification is absent. */
	vmf_optional_u8 subtype; /* Must be absent without identification. */
} vmf_message_id;

/* The schema itself. root is an unconditional GROUP. Static const C
 * initializers are sufficient; there is no build/load/compile prerequisite.
 * octet_order selects whether consecutive logical bits occupy physical bits
 * 0..7 or 7..0 of each payload octet. Field order selects code significance.
 * All body starts are octet-aligned. padding_bit must be zero; the codec
 * pads/checks the last octet after root completion. No implicit field padding.
 */
typedef struct vmf_message_schema {
	vmf_message_id id;
	const char *name; /* Optional NUL-terminated metadata. */
	const vmf_node *root;
	vmf_bit_order octet_order;
	uint8_t padding_bit; /* Must be zero. */
} vmf_message_schema;

/* A registry is simply an immutable array of schema pointers. It can be built
 * in C at runtime, including by collecting pointers exported by modules.
 * Array, descriptors, and module code/data must outlive all codecs using them.
 * No process-global registry and no first-match/wildcard selection policy.
 */
typedef struct vmf_schema_set {
	const vmf_message_schema *const *schemas;
	size_t count;
} vmf_schema_set;

/* Compare every meaningful member, including optional-presence flags; ignore
 * values of absent members. Never compare struct padding with memcmp.
 * Both arguments must be non-NULL and structurally valid.
 */
bool vmf_message_id_equal(const vmf_message_id *a, const vmf_message_id *b);

/* Check a native schema without creating or owning another representation.
 * Checks types, widths, bounds, IDs, cycles, disjoint choices, dependencies and
 * resource limits. A node reference must dominate its use and identify one
 * completed scalar in the current/ancestor scope, never a future, absent,
 * sibling-repeat or previous-iteration value. Separate count fields remain
 * ordinary events; the writer validates them and never backpatches them.
 * REMAINING extents require a terminal field with an unambiguous unit count
 * after accounting for padding; otherwise UNSUPPORTED_SCHEMA_FEATURE.
 * Arrays and strings must designate valid memory. Unknown encodings are errors,
 * never silently replaced. Only active union members and kind-specific members
 * are read. Zero-initialize descriptors before filling active members.
 * schema/limits are mandatory; error is optional and set on every return.
 */
vmf_status vmf_schema_validate(const vmf_message_schema *schema,
	const vmf_limits *limits, vmf_error *error);

/* Validate every schema and reject duplicate semantic identities, even if two
 * entries point to the same schema. Empty sets are valid but identify nothing.
 * schemas may be NULL only for count 0. No schema is modified or retained here.
 */
vmf_status vmf_schema_set_validate(
	const vmf_schema_set *set, const vmf_limits *limits, vmf_error *error);

/* Exact identity lookup in a validated set. Missing revision/subtype is not a
 * wildcard and never means "latest". No match -> VMF_UNKNOWN_MESSAGE. Bodies
 * are not trial-parsed to guess a message type. No fallback to the first
 * schema. set/id/result are mandatory; *result = NULL on failure. Duplicate
 * matching entries return VMF_DUPLICATE_MESSAGE, even for an unvalidated set.
 */
vmf_status vmf_schema_select(const vmf_schema_set *set,
	const vmf_message_id *id, const vmf_message_schema **result);

/* Find an original node by its nonzero ID. No compiled handle or path syntax.
 * Requires a validated schema; returns NULL for missing ID, zero or NULL
 * schema.
 */
const vmf_node *vmf_schema_find(
	const vmf_message_schema *schema, vmf_node_id id);

/* ---------- Revision-specific logical envelope values ---------- */

/* Explicit profile; version 15 is a form, not a profile. */
typedef enum vmf_envelope_version {
	VMF_ENVELOPE_B = 1,
	VMF_ENVELOPE_C = 2,
	VMF_ENVELOPE_D = 3,
	VMF_ENVELOPE_D1 = 4
} vmf_envelope_version;

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

/* GROUP events cover GROUP, REPEAT and CHOICE nodes; node->kind distinguishes
 * them. MESSAGE_END is emitted only after schema, length and padding checks.
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
 * be UNKNOWN. Other GROUP/CHOICE/body groups use count 1. No count is a byte
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
 * bits. Literal and EOF-delimited remainder fields report UNKNOWN totals;
 * their final chunk ends the value. Other reader totals precede field data.
 */
typedef struct vmf_field_event {
	uint64_t offset_units;
	uint64_t total_units;
	vmf_field value; /* Active member from node->as.field.kind. */
} vmf_field_event;

/* One event, one scalar or one variable-field chunk, never a whole payload.
 * On decode, schema is non-NULL for every message/body event. node is an
 * ORIGINAL descriptor pointer for FIELD/GROUP/ABSENT and otherwise NULL.
 * Root depth=0. A repeat-body group has occurrence=0,1,...; other nodes have
 * occurrence=0 in their immediate parent. Group nesting identifies ancestors.
 * ENVELOPE and PDU_END use schema=NULL, node=NULL, depth=occurrence=0.
 * MESSAGE_BEGIN/END use node=NULL, depth=occurrence=0.
 * On encode schema may be NULL (use selected schema), otherwise it must match.
 * Inactive union members are ignored. Zero-initialization is recommended.
 */
typedef struct vmf_event {
	vmf_event_kind kind;
	const vmf_message_schema *schema;
	const vmf_node *node;
	size_t depth;
	uint64_t occurrence;
	union {
		vmf_envelope_event envelope;
		vmf_message_event message;
		vmf_group_event group;
		vmf_field_event field;
	} as;
} vmf_event;

/* Traversal contract, identical for read and write:
 *
 *   ENVELOPE
 *     MESSAGE_BEGIN
 *       GROUP_BEGIN(root)
 *         FIELD / ABSENT / nested GROUP_BEGIN ... GROUP_END
 *       GROUP_END(root)
 *     MESSAGE_END
 *     ... further envelope-declared messages ...
 *   PDU_END
 *
 * An absent node emits exactly one ABSENT, no value or descendants. A repeat
 * container always has BEGIN/END, even for count 0; each item traverses its
 * unconditional body GROUP. A CHOICE wraps only its selected arm.
 *
 * Presence bits, inline length/count prefixes and recurrence indicators are
 * handled by the codec. They do not become application FIELD events. Explicit
 * scalar count/discriminator fields DO become FIELD events and are retained
 * internally when later schema rules reference them.
 *
 * PREFIX repeats require count before any body output; FIXED/FIELD counts must
 * match the schema/already written scalar. MORE_BEFORE writes true before each
 * item and false at container END, with polarity determined by more_bit.
 * MORE_AFTER writes the next positive flag when the next body BEGIN arrives,
 * or its final negative flag at container END. Thus no advance last-item guess
 * is required. Its first body is mandatory. A body END alone does not flush a
 * pending recurrence flag. Decoder can consume controls between returned
 * events.
 *
 * Variable FIELD prefixes are read before the first value event and written
 * from total_units before the first chunk. The writer checks that chunks add up
 * exactly. It never reads caller data beyond that chunk or advances a new field
 * merely because output capacity remains. Group counts/field lengths, not API
 * chunk boundaries, determine the wire representation.
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
typedef struct vmf_allocator {
	vmf_allocate allocate;
	vmf_deallocate deallocate;
	void *user;
} vmf_allocator;

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

/* NULL -> INVALID_ARGUMENT with offset/node=0 and message_index=SIZE_MAX. */
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

/* Begin independent header output; write then accepts only ENVELOPE.
 * Default idle write accepts ENVELOPE followed by all body events and PDU_END.
 * Header-only completion returns OK then requires begin/reset for reuse.
 */
vmf_status vmf_encoder_begin_envelope(vmf_encoder *encoder);

/* Accept exactly one event from the shared grammar. Schema/node identities,
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
 * replay events with populated lengths to a normal encoder. Unknown inline
 * prefixes are still invalid. Unambiguous terminal byte REMAINING fields may
 * use UNKNOWN lengths and terminate at their last chunk.
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
	const vmf_node *node, const void *value, size_t value_size,
	uint64_t total_units);

/* Copy individual value struct, not pointed-to chunk data. Failure leaves
 * value untouched. Requires FIELD event, original field node, exact size and
 * aligned nonoverlapping output. Does not mutate a codec.
 */
vmf_status vmf_event_copy_field(
	const vmf_event *event, void *value, size_t value_size);

/* Malformed wire, traversal/schema and resource errors are terminal until
 * reset. Invalid arguments and failed begin preserve usable state and retained
 * headers. Handles are single-threaded/non-reentrant; distinct handles can use
 * shared immutable schemas concurrently. No exceptions cross the API.
 */

#ifdef __cplusplus
}
#endif
#endif /* VMF_H_INCLUDED */
