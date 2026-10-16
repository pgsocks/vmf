# JSON payload schemas

The `vmf::schema_json` static library uses yyjson 0.12.0 to load VMF-specific
JSON definitions into the native schema types in `<vmf/wire.h>`. Include
`<vmf/schema_json.h>` from C11 or `<vmf/schema_json.hpp>` from C++17. Link
`vmf::schema_json`; it supplies the core VMF dependency transitively. The
companion builds by default. `BUILD_SCHEMA_JSON=OFF`, or the shared
`core-only` preset, omits it and yyjson.

This format describes payload layouts, with the same validation rules as
directly authored C schemas. It supplies no MIL-STD-6017 definitions. It is
not the standard JSON Schema vocabulary and does not infer a layout from
message data. Callers provide input bytes; the library performs no file I/O.
Serialization, named references, and automatic node IDs are not supported.

## Document and names

A document is exactly one JSON object containing a required `schemas` array:

```json
{
  "schemas": [
    {
      "id": {"user_message_format": 1},
      "name": "synthetic octet",
      "octet_order": "lsb_first",
      "root": {
        "id": 1,
        "kind": "group",
        "group": {
          "children": [
            {
              "id": 2,
              "name": "value",
              "kind": "field",
              "field": {
                "kind": "unsigned",
                "scalar": {
                  "encoding": {
                    "bit_width": 8,
                    "order": "lsb_first",
                    "representation": "unsigned"
                  }
                }
              }
            }
          ]
        }
      }
    }
  ]
}
```

An empty array is valid. All schemas load atomically; a failure exposes none.
Array order is preserved. Each message requires `id`, `root`, and
`octet_order`; `name` is optional and `padding_bit` defaults to zero. The root
must be an unconditional group, and padding must be zero.

Each identity requires `user_message_format`. Optional `standard_version` is
a number. Optional `identification` is an object requiring `functional_area`
and `message_number`, with an optional numeric `subtype`. For example:

```json
{
  "user_message_format": 2,
  "standard_version": 0,
  "identification": {
    "functional_area": 1,
    "message_number": 2,
    "subtype": 0
  }
}
```

Omission means absent; an explicitly present zero remains present. These
presence distinctions participate in exact schema selection. No omitted
identity value is a wildcard or a request for the latest version.

Property names follow native struct member names with these transformations:

- Omit each union's `as` layer and retain its selected member name. For
  example, a field node has `field`, and a scalar field has `scalar`.
- Use JSON arrays for native pointer/count pairs and omit count properties:
  `schemas`, `children`, `allowed`, `labels`, `sentinels`, `terms`, `values`,
  and `cases`.
- Use lowercase enum suffixes such as `"unsigned"` and `"lsb_first"`.
- Use strings for names, including the length-delimited `vmf_string` in a
  code label. Message and node names reject embedded NUL; label names preserve
  it. All strings must be valid UTF-8.

Numbers must be exact integer tokens in the target native member's range.
Fractional and exponent forms, numeric strings, and values outside that range
are rejected. A maximum of `18446744073709551615` preserves VMF's unlimited
schema-maximum sentinel. That sentinel cannot be an actual runtime extent.
Unknown or duplicate properties, inactive members, `null`, comments, trailing
commas, invalid UTF-8, and extra content after the document are rejected.

## Nodes and field definitions

Every node requires an explicit nonzero `id`, a `kind`, and exactly one
matching payload member. IDs must be unique within each schema tree. A node's
`name` and `presence` are optional. Without `presence`, the node is always
present. Nodes nest directly; no references to shared node definitions exist.

| Node `kind` | Payload member and contents |
| --- | --- |
| `field` | `field`: field `kind` and its active definition below |
| `group` | `group`: required `children` array of nodes |
| `repeat` | `repeat`: required `count` extent and `body` node |
| `choice` | `choice`: required `selector` node ID and `cases` array; optional `mask` and `otherwise` node |

A repeat body must be an unconditional group. Each choice case requires a
`values` array of inclusive code ranges and an unconditional `body` node.
An `otherwise` node is an unconditional fallback. Choice masks default to
`18446744073709551615`. Ranges across all arms must be disjoint. The masked
selector is tested against the supplied range endpoints; endpoints are not
masked.

| Field `kind` | Active definition |
| --- | --- |
| `bool`, `unsigned`, `signed`, `enum`, `scaled` | `scalar` |
| `text` | `text`: required `encoding`, `order`, and `length` extent |
| `octets` | `octets`: required `order` and `length` extent |
| `bits` | `bits`: required `length` extent |

Scalar definitions require `encoding`. The optional `allowed` and `labels`
arrays default to empty. An encoding requires `bit_width`, `order`, and
`representation`. Orders are `msb_first` or `lsb_first`; representations are
`unsigned`, `twos_complement`, `ones_complement`, or `sign_magnitude`.
The native field restrictions still apply: bool is one unsigned bit,
unsigned/enum use unsigned representation, signed uses a signed representation,
and scalar widths cannot exceed 64 bits.

A code range is `{"first":0,"last":3}`. A code label is
`{"code":1,"name":"ready"}`. Labels describe codes without implicitly
constraining them. An empty `allowed` array permits all representable codes.

Only scaled fields accept a required `scale` object within `scalar`. It
requires `factor` and `bias` rationals and permits an optional `sentinels`
label array, which defaults to empty. A rational contains a signed `numerator`
and a nonzero unsigned `denominator`, for example
`{"numerator":-1,"denominator":10}`. Scaled events preserve the exact raw
code; the scale is metadata for interpreting it.

Text encodings are `ascii_7`, `ascii_8`, `latin_1`, and `utf_8`. Text lengths
count wire code units, octet lengths count bytes, and bit lengths count bits.
Field order controls significance within each encoded unit; message
`octet_order` controls packing of logical bits into physical payload octets.

## Presence, conditions, and extents

An explicitly supplied `presence` object requires `kind` and only that kind's
active controls:

| Presence `kind` | Additional properties |
| --- | --- |
| `always` | None |
| `bit` | Required boolean `present_bit` |
| `condition` | Required `condition` object |

A condition requires `mode` (`all` or `any`) and a nonempty `terms` array.
Optional boolean `negate` defaults to false. Each term requires `field`, a
preceding scalar's node ID, and a nonempty `values` range array. Its optional
`mask` defaults to `18446744073709551615`; optional `negate` defaults to false.
References must satisfy VMF's existing dominance and availability rules;
conditions cannot read future values or another repeat iteration.

Every extent requires `kind`, `minimum`, and `maximum`. The kind determines
its additional controls:

| Extent `kind` | Additional properties |
| --- | --- |
| `fixed` | Required `fixed` count |
| `prefix` | Required `prefix` integer encoding; optional signed `bias`, default zero |
| `field` | Required `field` node ID; optional signed `bias`, default zero |
| `remaining` | None |
| `more_before`, `more_after`, `recurrence_before` | Required boolean `more_bit` |
| `literal` | None |

Prefix encodings must be unsigned. A `field` extent reads a preceding
bool/unsigned/enum scalar. Prefix and field extents add `bias` with checked
arithmetic. Field lengths support fixed, prefix, field, and remaining;
ASCII7 text also supports literal DELETE termination with a finite maximum.
Repeat counts support fixed, prefix, field, and the three indicator grammars.
Their progress and minimum-count restrictions remain those in the native API.
`maximum: 0` means zero, not unlimited.

## Loading and lifetime

Initialize options with `vmf_schema_json_default_options()`, then adjust
explicit limits as needed. Defaults are `vmf_default_limits()`, a 16 MiB input
limit, a 64 MiB live allocation budget, and malloc/free. Zero never requests
a default. The memory budget covers owner and allocation metadata, parser
storage, conversion scratch, and VMF validation scratch, including temporary
allocations during growth. It excludes caller input and an allocator's own
bookkeeping.

An optional `vmf_allocator` follows the core paired-callback and alignment
contract. Its configuration is copied; its caller state must remain valid
until document destruction. Independent loads are reentrant when their
allocator callbacks are. Allocator callbacks must not reenter the current
load or destruction operation. No global allocator hooks are changed.

This C11 example loads the JSON above from a caller-provided buffer and
creates a decoder borrowing the resulting set. The buffer spans exactly
`size` readable bytes and needs no terminator or extra padding:

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

	vmf_options options = {0};
	options.profile = VMF_ENVELOPE_D1;
	options.limits = loading.limits;
	options.schemas = vmf_schema_json_schemas(document);
	vmf_decoder *decoder = NULL;
	vmf_error error;
	status = vmf_decoder_create(&options, &decoder, &error);
	if (status == VMF_OK) {
		/* Begin and decode a PDU using the ordinary VMF event API. */
	}
	vmf_decoder_destroy(decoder);
	vmf_schema_json_destroy(document);
	return status;
}
```

On success, the document owns all native descriptors, arrays, and copied
strings. Input bytes may immediately be changed or released. The schema set
is borrowed and immutable; destroy all borrowing codecs before destroying
the document. Published schemas may be shared across threads, provided
destruction does not race any access. `vmf_schema_json_destroy(NULL)` is safe;
`vmf_schema_json_schemas(NULL)` returns NULL.

The C++17 wrappers use the same explicit lifetime and statuses:

```cpp
#include <vmf/schema_json.hpp>

vmf::schema_json::options options = vmf::schema_json::default_options();
vmf::schema_json::document *document = nullptr;
vmf::schema_json::error diagnostic;
vmf_status status = vmf::schema_json::load(json, size, &options,
	&document, &diagnostic);
/* Borrow vmf::schema_json::schemas(document) after VMF_OK. */
vmf::schema_json::destroy(document);
```

Every failed load sets the result document to NULL and releases all partial
allocations. Optional diagnostics are initialized on every return. Their
message is static text independent of input and document lifetime. A syntax
error can report a zero-based JSON byte offset; unavailable offsets and schema
indexes are `SIZE_MAX`, and an unavailable node ID is zero. Success sets status
to `VMF_OK`, message to `"ok"`, both locations to `SIZE_MAX`, and node ID to zero.

| Status | Meaning |
| --- | --- |
| `VMF_INVALID_ARGUMENT` | API arguments or allocator configuration are invalid |
| `VMF_MALFORMED`, `VMF_TRUNCATED` | Input is not a complete strict JSON document |
| `VMF_INVALID_SCHEMA` | A definition has invalid properties, types, or numeric ranges |
| `VMF_LIMIT_EXCEEDED` | An input, memory, arithmetic, or schema limit is exceeded |
| `VMF_OUT_OF_MEMORY` | The selected allocator failed |
| Other VMF validation status | Native schema validation rejected the definition, including duplicate identities or unsupported features |

All public operations use explicit statuses and cleanup; none throws exceptions.
