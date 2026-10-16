# JSON payload schemas

The optional `vmf::schema_json` target loads strict `vmf-schema/1` documents
into the native message/index types in `<vmf/wire.h>`. Include
`<vmf/schema_json.h>` from C11 or `<vmf/schema_json.hpp>` from C++17. Direct C
callers use the core library alone. Both paths share profile resolution,
structural validation, defaults, exact message lookup and event semantics.
There are no built-in MIL-STD-6017 message definitions.

## Document and identities

```json
{
  "format": "vmf-schema/1",
  "envelope": "C",
  "messages": [
    {
      "message_number": "K01.1",
      "indexes": [
        {"index_number": "1.", "number_of_bits": 5},
        {"index_number": "2.", "number_of_bits": 13}
      ]
    }
  ]
}
```

This synthetic layout demonstrates syntax, without asserting an assigned
message definition. `format`, `envelope` and `messages` are required.
Supported envelopes are `B`, `C`, `D` and `D1`. An empty message array is valid
when the registry profile and shared standard selection are valid.
Each message requires a VMF K-series `message_number` and ordered `indexes`.
Optional `subtype` preserves present zero. All payload definitions select
UMF 2; header-only codec operations remain independent of this restriction.

The registry and each message can select `message_standard` (a supported named
edition) or `standard_version` (a raw code or `"absent"`). These properties are
mutually exclusive at one level. A message-level selection replaces the
registry selection as a unit. The root cannot inherit a process-global value.
B rejects an emitted version, C permits absence, and D/D1 require a selection.
Core rules map editions by envelope; MIL-STD-6017B uses code 8 under C and code
7 under D/D1. Missing values never select the newest edition or match a
present zero. Duplicate resolved wire identities fail even when their
out-of-band descriptions would differ.

Array order is authoritative. Index notation `1` and `1.` identifies the same
numeric address; it does not sort declarations. References use numeric path
components rather than lexical string comparison. `group_code` and
`repeat_code` spell typed native membership paths, such as `G1/G2` and
`R1/R2`. Omission means no membership in that category.

## Index properties and defaults

Ordinary indexes require `index_number` and `number_of_bits`. Omitted
`encode_type` means unsigned raw scalar data for widths 1–64. Fixed width zero
is invalid; wider fields require an explicit bits, octets or text mode.
Explicit variable modes use `number_of_bits` as a finite data capacity.
Capacity alone never enables variable length, text, or termination.

| Property | Meaning |
| --- | --- |
| `encode_type` | Bool, unsigned, enum, signed, scaled, bits, octets, fixed text or literal text |
| `representation` | Compatible explicit integer representation; signed modes require one |
| `bit_order` | Logical significance override, separate from octet packing |
| `text_encoding` | Supported wire encoding; ordinary text defaults to ASCII7 |
| `length_from` | Earlier scalar index supplying length in this field's wire units |
| `length_bias` | Checked signed adjustment, valid only with `length_from` |
| `when` | Bounded all/any raw-code predicate using preceding scalar indexes |
| `allowed` | Disjoint inclusive raw-code intervals |
| `constant` | Required raw scalar code, never a manufactured encoder value |
| `scale` | Exact rational factor/bias and raw sentinel codes for inspection |
| `key` | Unique application binding key within the message |

Accepted `encode_type` strings are `default`, `bool`, `unsigned`, `signed`,
`enum`, `scaled`, `bits`, `octets`, `text` and `literal`. Representation strings
are `default`, `unsigned`, `twos_complement`, `ones_complement` and
`sign_magnitude`. Orders use `default`, `lsb_first` or `msb_first`; text encodings
use `default`, `ascii_7`, `ascii_8`, `latin_1` or `utf_8`. The optional message
property `octet_order` selects physical packing independently of field order.

Named standards are `MIL-STD-6017`, `MIL-STD-6017A` through `MIL-STD-6017E`,
`TIDP-TE-R2` through `TIDP-TE-R7`, and `TIDP-FTE-R6`. Each is accepted only
where the selected profile defines its mapping.

Ranges are objects with `first` and `last` integer properties. A condition
requires `mode` (`all` or `any`) and a nonempty `terms` array, with optional
`negate`. Each term requires `field` and `values`, with optional `mask` and
`negate`; an omitted mask means all raw bits. A scale requires `factor` and
`bias`, each an object with signed `numerator` and unsigned nonzero
`denominator`. Its optional `sentinels` array contains unsigned raw codes.
A scaled field requires scale metadata. Keys are length-delimited UTF-8 data.
JSON integer tokens preserve the full signed/unsigned 64-bit bounds; decimals,
exponents and quoted numeric strings are rejected in schema definitions.

Physical octet packing and field significance independently default to LSB
first. Overrides do not insert alignment. Octet capacities must be divisible
by eight; text capacities must be divisible by the selected seven/eight-bit
code-unit width. Text lengths count wire code units, not UTF-8 API bytes.
Literal text uses bounded ASCII7 data and DELETE termination below capacity,
with no terminator at capacity. DELETE is not returned as data. A field cannot
combine literal termination with `length_from`. Body padding is always zero.

Constraints and predicates retain raw-code semantics. Signed values do not
change how `allowed`, masks or predicate intervals interpret codes. Scaled
events carry exact raw codes; applications can inspect rational metadata.
No display names, descriptive labels, defaults, notes or arbitrary callbacks
are accepted as executable schema properties.

## Groups, presence and repeats

Groups are contiguous ordered regions and add no bits by membership alone.
Indicators are explicit definitions with one-bit width; an omitted indicator
width resolves to one. Any other width fails. Indicators do not also carry
ordinary data encodings or emit separate FIELD events.

```json
[
  {"index_number": "3.", "indicator": "fpi", "target": "3.1"},
  {"index_number": "3.1", "number_of_bits": 9}
]
```

FPI controls one data index; GPI controls one group ID. The control immediately
precedes its target in the enclosing logical sequence. One means present.
FRI controls a single-field recurrence; GRI controls a grouped recurrence.
They identify the controlled repeat through their repeat membership and declare
its finite positive maximum, for example `"repeat_code":"R1(4)"`.

```json
[
  {"index_number": "4.", "indicator": "gpi", "target": "G1"},
  {"index_number": "4.1.1", "indicator": "gri",
   "group_code": "G1", "repeat_code": "R1(4)"},
  {"index_number": "4.1.2", "number_of_bits": 7,
   "group_code": "G1", "repeat_code": "R1"},
  {"index_number": "4.1.3", "number_of_bits": 9,
   "group_code": "G1", "repeat_code": "R1"}
]
```

A present recurrence contains at least one occurrence. Its flag indicates
whether another occurrence follows: two bodies use `1, body, 0, body`.
An enclosing absent region consumes only its presence bit and no recurrence
bit. The recurrence controller is emitted once per occurrence, never again as
a body field. Membership, control ownership and index ancestry must establish
one unambiguous nesting. Crossing, reopened or contradictory scopes fail.

Fixed and scalar-count repeats use a message-level `repeats` array instead:

```json
[
  {"repeat_code": "R1", "count_from": "1.", "maximum": 4},
  {"repeat_code": "R2", "count": 3}
]
```

Members identify these repeats through the same membership paths. Fixed count
zero is valid. Scalar counts can use optional `minimum` and checked `bias` with a
finite maximum. Count fields remain ordinary data events and are validated,
never backpatched. A repeat cannot combine a declaration with FRI/GRI control.
Dependencies must be completed, dominating scalars in the current/ancestor
scope; future, absent, sibling-repeat and previous-occurrence values fail.

## Strict loading, limits and ownership

Unknown/duplicate properties, unexpected null, inactive properties, malformed
notation, unsupported enums and noninteger/out-of-range numbers fail. No syntax
repair or format detection occurs. Unrecognized schema vocabularies receive
an unsupported-format diagnostic. Mechanical notation parsing does not supply a
second implementation of scope or profile semantics.

Initialize loading options with `vmf_schema_json_default_options`, then change
explicit limits as needed. Zero is never shorthand for a default. The default
input budget is 16 MiB; the live allocation budget is 64 MiB and includes owner
metadata, parsing, conversion and native validation scratch. Input bytes and
the allocator's own bookkeeping are outside that budget. Core limits also
bound messages, indexes, normalized scopes, depth, field sizes and traversal.

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
status and identify the source message/index and property where available.
No failed-load diagnostic points into freed descriptor storage. Core semantic
errors are identical for equivalent native and JSON definitions; JSON may add
representation-specific location information.
