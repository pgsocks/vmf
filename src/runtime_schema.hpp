#ifndef VMF_PRIVATE_RUNTIME_SCHEMA_HPP
#define VMF_PRIVATE_RUNTIME_SCHEMA_HPP

#include <vmf/wire.h>

typedef uint32_t vmf_node_id;

/* No node/reference. The actual root has a nonzero ID like any other node. */
#define VMF_NODE_NONE ((vmf_node_id)0)

/* Optional name for an enumeration code or a scaled-value sentinel. */
typedef struct vmf_code_label {
	uint64_t code;
	vmf_string name;
} vmf_code_label;

/* For non-sentinel codes, physical value = decoded_integer * factor + bias.
 * The streaming API still exchanges the exact raw code, not a float.
 */
typedef struct vmf_runtime_scale_definition {
	vmf_rational factor;
	vmf_rational bias;
	const vmf_code_label *sentinels; /* Bypass the numeric scale mapping. */
	size_t sentinel_count;
} vmf_runtime_scale_definition;

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
	vmf_runtime_scale_definition scale; /* Used only by VMF_FIELD_SCALED. */
} vmf_scalar_definition;

/* Compare a preceding scalar's raw code, masked before interval testing.
 * A term is true when (code & mask) belongs to any supplied interval, then
 * inverted when negate is true. A term requires at least one interval.
 */
typedef struct vmf_runtime_condition_term {
	vmf_node_id field;
	uint64_t mask; /* UINT64_MAX tests the entire raw code. */
	const vmf_code_range *values;
	size_t value_count;
	bool negate;
} vmf_runtime_condition_term;

/* Conditions contain at least one term; no callbacks or source expressions.
 * Terms read already completed scalar occurrences, never future values.
 */
typedef struct vmf_runtime_condition {
	vmf_condition_mode mode;
	const vmf_runtime_condition_term *terms;
	size_t term_count;
	bool negate; /* Invert the combined result. */
} vmf_runtime_condition;

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
	vmf_runtime_condition condition; /* Used only by CONDITION. */
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

/* Private normalized nodes, traversed by both codecs. */
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
	const vmf_index_schema *source;
	vmf_scope_ref scope;
	vmf_node_kind kind;
	union {
		vmf_field_schema field;
		vmf_group_schema group;
		vmf_repeat_schema repeat;
		vmf_choice_schema choice;
	} as;
};


struct vmf_runtime_schema {
	vmf_message_id id;
	const char *name;
	const vmf_node *root;
	vmf_bit_order octet_order;
	uint8_t padding_bit;
	const vmf_message_schema *source;
	const vmf_node *const *bindings;
	size_t binding_count;
};
struct vmf_runtime_set {
	const vmf_runtime_schema *const *schemas;
	size_t count;
	void *allocations;
};

/* Private traversal events retain runtime pointers only inside the core. */
struct vmf_runtime_event {
	vmf_event_kind kind;
	const vmf_runtime_schema *schema;
	const vmf_node *node;
	size_t depth;
	uint64_t occurrence;
	union {
		vmf_envelope_event envelope;
		vmf_message_event message;
		vmf_group_event group;
		vmf_field_event field;
	} as;
};

vmf_status runtime_schema_validate(const vmf_runtime_schema *schema,
	const vmf_limits *limits, const vmf_allocator *allocator,
	vmf_error *error);
vmf_status runtime_set_validate(const vmf_runtime_set *set,
	const vmf_limits *limits, const vmf_allocator *allocator,
	vmf_error *error);
const vmf_node *runtime_schema_find(
	const vmf_runtime_schema *schema, vmf_node_id id);

#endif
