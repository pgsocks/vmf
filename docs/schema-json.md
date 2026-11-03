# JSON payload schemas

The optional `vmf::schema_json` target loads strict `vmf-schema/1` documents
into the native message/index types in `<vmf/wire.h>`. Ordered, nested JSON
elements describe the layout; the loader generates explicit native indexes,
controllers, memberships and repeat declarations. Include
`<vmf/schema_json.h>` from C11 or `<vmf/schema_json.hpp>` from C++17. Direct C
callers use the core library alone. Both paths share profile resolution,
structural validation, defaults, exact message lookup and event semantics.
There are no built-in MIL-STD-6017 message definitions.

## Document and identities

```json
{
  "format": "vmf-schema/1",
  "message_standard": "MIL-STD-6017B",
  "messages": [
    {
      "message_number": "K01.1",
      "elements": [
        {"key": "code", "number_of_bits": 5},
        {"key": "value", "number_of_bits": 13}
      ]
    }
  ]
}
```

This synthetic layout demonstrates syntax, without asserting an assigned
message definition. `format` and `messages` are required. Optional `envelope`
selects a default `B`, `C`, `D` or `D1` profile for convenience helpers; omission
leaves the registry independent of an envelope. Set `vmf_options.profile` when
creating a codec. The same loaded registry can serve compatible profiles without
copying or redefining its messages. An empty message array is valid when the
shared standard selection and any default profile are valid.
Each message requires a VMF K-series `message_number` and ordered `elements`.
Optional `subtype` preserves present zero. All payload definitions select
UMF 2; header-only codec operations remain independent of this restriction.

The registry and each message can select `message_standard` (a supported named
edition) or `standard_version` (a raw code or `"absent"`). These properties are
mutually exclusive at one level. A message-level selection replaces the
registry selection as a unit. The root cannot inherit a process-global value.
Named editions retain their meaning when an explicit codec profile overrides the
default. MIL-STD-6017B uses code 8 under C and code 7 under D/D1; B omits the
version for named editions. C permits an absent selection, while D/D1 require a
version. Missing values never select the newest edition or match a present zero.

Raw `standard_version` codes remain literal. A supplied `envelope` binds them
to that profile, so a different codec profile is rejected. Without `envelope`,
raw codes are interpreted in the explicitly selected profile; named editions
provide stable meaning across profiles. B rejects a present raw version.

Loading without `envelope` validates semantic identities and payload structure.
Loading with a default also validates its version mappings. Codec creation and
`vmf_schema_set_validate_for_profile` check the chosen profile, including
unsupported editions and duplicate resolved identities. Distinct named editions
can collide when B omits their versions. Resolve or select exact identities for
an explicit profile with `vmf_schema_message_id_for_profile` and
`vmf_schema_select_for_profile`; the ordinary identity helpers require a default.

## Elements and ordering

An `elements` array contains fields, nested arrays, or control wrappers:

```text
Field
[Element, ...]
{"optional": Element}
{"recurrence": {"maximum": N, "body": Element}}
{"repeat": {"count": N, "body": Element}}
{"repeat": {"count_from": FieldKey, "maximum": N,
            "minimum": optional N, "bias": optional integer,
            "body": Element}}
```

Array order determines wire order. Nested arrays describe ordered groups and
must be nonempty; the message's root `elements` array may be empty. Plain
arrays add no bits or independent native group scope. A group with one element
is still a group when choosing an indicator role.

Wrappers contain exactly their specified properties. Nesting determines control
order: `optional` around `recurrence` controls the whole recurrence, while
`recurrence` around `optional` controls a sequence whose individual occurrences
have their own presence controls. No implicit precedence combines the two.

Authored JSON contains no `index_number`, `group_code`, `repeat_code`, `target`,
`indicator`, or separate `repeats` declarations. The loader generates these
from the ordered structure. The format remains `vmf-schema/1`; the previous
flat authoring syntax is not supported.

## Fields, types and references

Every ordinary data field requires a nonempty, NUL-free UTF-8 `key`. Keys must
be unique across the complete nested body of their message, including inactive
branches and zero-count repeats. Another message may reuse the same keys.
Keys name application bindings and schema dependencies; they do not add wire
bytes and do not assign MIL-STD data-field identities.

`length_from`, `count_from`, and a condition term's `field` refer to these keys.
References are exact, case-sensitive strings within the same message. There
is no numeric-address fallback: a reference such as `"1.1"` names a field whose
key is literally `1.1`. Generated indexes can change when the structure changes
without changing these named references.

Ordinary fields require `number_of_bits`, except that `type: "bool"` derives
one bit. An explicitly supplied boolean width must equal one. Omitted `type`
means unsigned raw scalar data for widths 1–64. Fixed width zero is invalid;
wider data requires an explicit `bits`, `octets`, `text` or `literal` type.
Variable fields still declare their finite capacity in bits. Capacity alone
never enables variable length, text, or termination.

| Property | Meaning |
| --- | --- |
| `key` | Required field identity, unique within its message |
| `type` | Wire data type; omission means `unsigned` |
| `number_of_bits` | Field width or finite data capacity, in bits |
| `bit_order` | Logical significance override, separate from octet packing |
| `text_encoding` | Supported wire encoding; ordinary text defaults to ASCII7 |
| `length_from` | Earlier scalar field key supplying length in this field's wire units |
| `length_bias` | Checked signed adjustment, valid only with `length_from` |
| `when` | Bounded all/any raw-code predicate using preceding scalar fields |
| `allowed` | Disjoint inclusive raw-code intervals |
| `constant` | Required raw scalar code, never a manufactured encoder value |
| `scale` | Exact rational factor/bias and raw sentinel codes for inspection |

Accepted `type` strings map to native definitions as follows:

| JSON type | Native encoding and integer representation |
| --- | --- |
| Omitted or `unsigned` | Unsigned scalar |
| `bool` | One-bit boolean |
| `enum` | Unsigned enumerated scalar |
| `twos_complement` | Signed scalar, two's-complement representation |
| `ones_complement` | Signed scalar, one's-complement representation |
| `sign_magnitude` | Signed scalar, sign-magnitude representation |
| `bits` | Bit sequence |
| `octets` | Octet sequence |
| `text` | Fixed or field-length text |
| `literal` | Bounded, DELETE-terminated ASCII7 text |

`scale` implies native scaled encoding and retains the raw representation
selected by an omitted/`unsigned` type or one of the three signed types. Scale
with any other type is invalid. There is no separate JSON `signed` or `scaled`
type, `encode_type`, or `representation` property. The chosen signed
representation is explicit; width, constraints and labels do not select it.

Orders use `default`, `lsb_first` or `msb_first`; text encodings use `default`,
`ascii_7`, `ascii_8`, `latin_1` or `utf_8`. The optional message property
`octet_order` selects physical packing independently of field significance.
Physical octet packing and field significance independently default to LSB
first. Overrides do not insert alignment.

Named standards are `MIL-STD-6017`, `MIL-STD-6017A` through `MIL-STD-6017E`,
`TIDP-TE-R2` through `TIDP-TE-R7`, and `TIDP-FTE-R6`. A registry without a
default can contain any supported edition; binding to C/D/D1 requires that
profile to define its mapping.

Ranges are objects with `first` and `last` integer properties. A condition
contains exactly one nonempty `all` or `any` array, with optional `negate`:

```json
{
  "key": "text",
  "type": "text",
  "number_of_bits": 224,
  "length_from": "text_length",
  "when": {
    "all": [
      {"field": "format", "values": [{"first": 1, "last": 1}]}
    ]
  }
}
```

The fragment assumes preceding fields named `text_length` and `format`. Each
term requires `field` and `values`, with optional `mask` and `negate`; an
omitted mask means all raw bits. `when` applies to fields, not arrays or
wrappers. A directly FPI-controlled field cannot also declare `when`.

A scale requires `factor` and `bias`, each an object with signed `numerator`
and unsigned nonzero `denominator`. Its optional `sentinels` array contains
unsigned raw codes. JSON integer tokens preserve the full signed/unsigned
64-bit bounds; decimals, exponents and quoted numeric strings are rejected
where an integer is required.

Octet capacities must be divisible by eight; text capacities must be divisible
by the selected seven/eight-bit code-unit width. Text lengths count wire code
units, not UTF-8 API bytes. UTF-8 wire units are bytes, not Unicode character
counts. Literal text uses bounded ASCII7 data and DELETE termination below
capacity, with no terminator at capacity. DELETE is not returned as data. A
field cannot combine literal termination with `length_from`. Body padding is
always zero.

Constraints and predicates retain raw-code semantics. Signed values do not
change how `allowed`, masks or predicate intervals interpret codes. Scaled
events carry exact raw codes; applications can inspect rational metadata.
No display names, descriptive labels, defaults, notes or arbitrary callbacks
are accepted as executable schema properties.

## Mapping to explicit MIL fields

The implicit structure expands into the explicit presence and recurrence
indicator roles used by VMF:

| JSON structure | Generated explicit definition |
| --- | --- |
| Ordinary field | Data index with its resolved width, encoding and refinements |
| `optional` directly containing a field | One-bit Field Presence Indicator (FPI) targeting that field |
| `optional` containing an array or wrapper | One-bit Group Presence Indicator (GPI) targeting a generated group |
| `recurrence` directly containing a field | One-bit Field Recurrence Indicator (FRI) and a repeat with the stated maximum |
| `recurrence` containing an array or wrapper | One-bit Group Recurrence Indicator (GRI) and a repeat with the stated maximum |
| `repeat` with `count` | Fixed repeat declaration, with no control bit |
| `repeat` with `count_from` | Scalar-count repeat declaration, with no control bit |
| Plain nested array | Ordered children; no extra bits or independent native scope |

Controller choice depends on the direct body shape, not the number of fields.
For example, `recurrence` around a one-field array generates GRI; recurrence
directly around the field generates FRI. Controllers receive no application
keys, data encodings, or independent FIELD events.

For example, this synthetic message body declares an optional field, an
optional grouped recurrence, and both forms of count-based repeat:

```json
{
  "message_number": "K01.1",
  "elements": [
    {"key": "copies", "number_of_bits": 3},
    {"optional": {
      "key": "height", "type": "twos_complement", "number_of_bits": 12
    }},
    {"optional": {
      "recurrence": {
        "maximum": 4,
        "body": [
          {"key": "x", "number_of_bits": 7},
          {"key": "y", "number_of_bits": 9}
        ]
      }
    }},
    {"repeat": {
      "count": 2,
      "body": {"key": "tag", "number_of_bits": 8}
    }},
    {"repeat": {
      "count_from": "copies", "maximum": 4,
      "body": {"key": "status", "number_of_bits": 2}
    }}
  ]
}
```

The loader emits these native index definitions in order. A dash means no
membership in that category:

| Generated index | Role or field | Bits | Group path | Repeat path | Target or definition |
| --- | --- | --- | --- | --- | --- |
| `1` | `copies`, unsigned | 3 | — | — | Count source for R3 |
| `2` | FPI | 1 | — | — | Data index `2.1` |
| `2.1` | `height`, signed two's complement | 12 | — | — | — |
| `3` | GPI | 1 | — | — | Group G1 |
| `3.1` | GRI | 1 | G1 | R1(4) | Recurrence maximum 4 |
| `3.1.1` | `x`, unsigned | 7 | G1 | R1 | — |
| `3.1.2` | `y`, unsigned | 9 | G1 | R1 | — |
| `4.1` | `tag`, unsigned | 8 | — | R2 | Fixed count 2 |
| `5.1` | `status`, unsigned | 2 | — | R3 | Count from index `1`, maximum 4 |

R2 and R3 also have native repeat declarations. Their wrappers emit no index
row of their own. Bit widths, raw encodings and repeat maxima still come from
the authored schema; nesting supplies ownership and membership, not those
specification values. This layout asserts no assigned K01.1 definition.

Generated indexes follow positive, one-based list positions. A wrapper at
path `P` puts its controller at `P`, when it has one. A direct body occupies
`P.1`; an array body supplies children `P.1`, `P.2`, and so on. A plain nested
array at `P` likewise supplies children `P.1`, `P.2`, and so on. The message's
root list supplies `1`, `2`, and so on. Addresses compare as numeric component
paths and do not reorder declarations.

Optional composites receive group IDs and all repeats receive repeat IDs in
preorder, using separate counters starting at one for each message. Descendant
rows inherit complete enclosing group and repeat paths. An FPI/GPI precedes
its controlled body and belongs only to outer scopes. An FRI/GRI belongs to
its own repeat as well as outer scopes; its final repeat component declares
the maximum. Only GPI-controlled composites need independent native group
scopes; plain arrays remain transparent and repeats have their native body
scope.

These generated addresses and IDs are library schema identifiers. They are
not transmitted in the payload and need not equal independently assigned
index labels or group codes in a printed MIL table. When transcribing a table,
use its order, field widths, presence/recurrence roles, nesting and limits;
the loader reconstructs the corresponding explicit relationships. The loaded
native arrays and `vmf_schema_index_info` expose those relationships for
inspection. Application bindings can resolve data fields by key through
`vmf_schema_find_key`.

### Presence, recurrence and counts on the wire

FPI/GPI use one for present. An absent body consumes only its presence bit.
A present indicator-controlled recurrence has at least one occurrence. Its
leading flag indicates whether another occurrence follows: two bodies use
`1, body, 0, body`. The controller appears once per occurrence. An enclosing
absent optional region consumes no inner recurrence bits.

Fixed repeats allow count zero. Scalar-count repeats allow optional `minimum`
and checked signed `bias`, defaulting to zero, and require an explicit finite
`maximum`. Indicator-controlled recurrence requires a strictly positive finite
maximum. A fixed repeat rejects `count_from`, `maximum`, `minimum` and `bias`;
a scalar-count repeat rejects `count`. Count fields remain ordinary data
events and are validated, never backpatched. Omitting a count source cannot
silently select recurrence; that requires the separate `recurrence` wrapper.

Dependencies must identify already completed dominating scalars in the
current or an ancestor scope. Future, self, absent, sibling-repeat and
previous-occurrence dependencies fail. Count and length sources retain their
native boolean/unsigned/enum eligibility. Named references change authoring
notation without changing these core rules.

## Strict loading, limits and ownership

Unknown/duplicate properties, unexpected null, inactive properties, empty
nested arrays, missing/duplicate keys, unsupported types and noninteger or
out-of-range numbers fail. Retired flat-schema properties fail rather than
being ignored. No syntax repair or format detection occurs. Unrecognized
schema vocabularies receive an unsupported-format diagnostic. The loader
generates native definitions and named references; the core remains the
authority for scope, dependency and profile semantics.

Initialize loading options with `vmf_schema_json_default_options`, then change
explicit limits as needed. Zero is never shorthand for a default. The default
input budget is 16 MiB; the live allocation budget is 64 MiB and includes owner
metadata, parsing, conversion/source-mapping scratch and native validation
scratch. Input bytes and the allocator's own bookkeeping are outside that
budget. Core limits also bound messages, generated indexes/controllers,
normalized scopes, depth, field sizes and traversal. Limits apply to the expanded definitions, not only the
number of visible JSON elements.

Allocator callbacks are paired, copied, and must return suitably aligned
storage. NULL configuration selects malloc/free. Caller state survives document
destruction; callbacks do not reenter the operation. Every failure frees all
partial storage and clears the returned owner. Input is neither modified nor
retained. Successful documents own copied descriptors, paths, keys and arrays.
Destroy every codec borrowing a document before destroying the document.
Independent codecs and loads can share immutable definitions across threads.

```c
#include <vmf/schema_json.h>

vmf_status use_schema_json(const char *json, size_t size)
{
	vmf_schema_json_options loading = vmf_schema_json_default_options();
	vmf_schema_json_document *document = NULL;
	vmf_schema_json_error diagnostic;
	vmf_status status = vmf_schema_json_load(json, size, &loading,
		&document, &diagnostic);
	if (status != VMF_OK)
		return status;
	vmf_options options =
		vmf_options_for_schemas(vmf_schema_json_schemas(document));
	options.profile = VMF_ENVELOPE_D1;
	vmf_decoder *decoder = NULL;
	vmf_error error;
	status = vmf_decoder_create(&options, &decoder, &error);
	/* Read with decoder while document remains alive. */
	vmf_decoder_destroy(decoder);
	vmf_schema_json_destroy(document);
	return status;
}
```

Diagnostics are initialized on every return. Syntax locations use JSON byte
offsets, separately from PDU bit offsets. Semantic diagnostics preserve core
status, message/index positions, property category and related locations.
Native index positions refer to the expanded arrays, including generated
controller rows.

`path` and `related_path` copy RFC 6901 JSON pointers into separate 512-byte
buffers; each has an explicit truncation flag. They point to the authored
element/property, including the wrapper that generated an offending controller
or repeat declaration. A related path is present when a related source can be
identified. Paths use zero-based JSON array positions, independently of the
one-based generated field indexes. Long paths retain valid UTF-8 and mark
truncation. The empty string means root or unavailable. Success clears paths
and truncation flags along with the other locations.

No failed-load diagnostic points into freed input or descriptor storage.
The loader retains source mappings through native validation, copies diagnostic
paths before cleanup, and releases the parsing/conversion scratch. CLI schema
failures report the message and authored paths. Core semantic errors remain
identical for equivalent native and JSON definitions; JSON adds source
locations for the authoring structure.
