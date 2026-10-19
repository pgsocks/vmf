# Public envelope header JSON

The optional `vmf::header_json` CMake target provides an owned, native envelope
configuration API through `<vmf/header_json.h>` and its procedural C++17
forwarders in `<vmf/header_json.hpp>`. It has no dependency on payload schema
JSON. Set `BUILD_HEADER_JSON=OFF` to omit it; the CLI requires both JSON targets.
The core-only preset omits both companions.

`vmf_header_json_load(profile, data, size, options, &document, &error)` loads one
strict UTF-8 JSON object into an immutable `vmf_header` union. All header arrays,
strings and byte spans belong to the document, independently of the input.
Borrow them through `vmf_header_json_header` until
`vmf_header_json_destroy(document)`. Destroy a borrowing codec first if the
codec's operation still requires those spans. The managed encoder copies header
pointees during successful begin.

For a C-profile synthetic identity, a complete minimal configuration is:

```json
{"fields":{"messages":[{"id":{"user_message_format":2,
  "standard_version":0,"identification":{"functional_area":1,
  "message_number":1}}}]}}
```

Profiles are selected explicitly by the API argument, never inferred from JSON.
Identities are exact: omitting a standard version or subtype means absent,
never wildcard or latest. The core codec validates every active native field,
profile constraint and conditional combination. It validates size presence
using counting semantics; actual size numbers remain authoring/replay inputs.
No actual assigned VMF payload layout is defined by this example.

## Optional properties and defaults

Objects accept omitted optional members. Absence-controlled groups and spans
are absent; arrays are empty; operation and classification are zero;
retransmit and shortened are false. Default precedence is 2 in B/C and 0 in
D/D1 (5 for D/D1 user-message-format 6). Required object identities, DTG
components and response receipt codes still must be supplied. Explicit JSON
null denotes absence only for optional groups/spans, not numeric codes, flags
or arrays. Present addresses still require exactly one of URN or name.

`vmf_header_json_template` generates a minimal full header for caller-supplied
exact identities. Multiple descriptors include zero message sizes and, except
in B, a zero header size: these placeholders preserve required size presence
for later complete-PDU authoring. This function does not invent a standard
edition, infer a payload schema or satisfy identity-specific filename rules.

`vmf_header_json_serialize` preserves the established VMF CLI shape: it emits
null members and empty arrays for absent optional values. The returned
`vmf_header_json_text` owns its NUL-terminated UTF-8 data until
`vmf_header_json_text_destroy`; the length excludes the terminator. Both APIs
leave the output owner NULL on every failure.

## Profile-specific shape

The root contains `fields` and, except in B, `shortened`. Common fields are
`compression`, `originator`, `recipients`, `information_addressees`, `messages`;
C/D/D1 also permit `header_size_octets`. Shortened C/D/D1 headers require
originator/recipients and omit full-header-only data according to the core
codec contract. The serializer emits an empty messages array for that form.

Each message contains exact `id`, optional `file_name`, `message_size_octets`,
`operation`, `retransmit`, `precedence`, `classification`, `originator_dtg`,
`perishability`, `acknowledgment`, `response`, `references`. B permits
`marking_b`, C `marking_c`, and D/D1 `markings`. Only C/D/D1 permit `security`.
References contain `originator` and `dtg`; only B permits `functional_area` and
`message_number`. Unknown, duplicate decoded keys and profile-incompatible
properties are rejected, including properties whose value is null.

Addresses use `urn` or `name`. DTGs use `year`, `month`, `day`, `hour`, `minute`,
`second` and optional `extension`. Acknowledgments use boolean `machine`,
`operator_ack`, `operator_reply`. Response metadata uses `acknowledged`,
`receipt_compliance`, optional `cantco_reason`, `cantpro_reason`, `amplification`.
The native codec's conditional rules apply: for example, acknowledgment requests
and response-only metadata cannot coexist in the same message descriptor.

Security uses `spi`, `keying_material`, `initialization`, `tokens_present`,
`tokens`, `authentication_a`, `authentication_b`, `signed_ack_request`,
`padding_present`, `padding_length`, `padding`. Binary data is an even-length
hexadecimal JSON string. Native span lengths and conditional presence remain
strict. This API preserves raw security metadata for inspection and replay;
it performs no authentication verification and grants no permission to reuse
authentication material when authoring a changed message.

## Limits and diagnostics

Defaults are 16 MiB JSON input/output and 64 MiB simultaneously live memory.
The memory budget includes owners, allocator metadata, parser/conversion
storage, serialization output and native validation scratch. Input storage and
allocator-private overhead are excluded. Zero limits remain zero. A custom
allocator must supply paired allocate/deallocate callbacks with `max_align_t`
alignment. Configuration is copied; its user state must survive every owner.
Failure releases all partial storage. No global allocator hook is changed.

Errors provide a core status, stable reason category, static message, copied
RFC 6901 JSON pointer, truncation flag and JSON syntax byte offset when
available. Truncated pointers retain complete UTF-8 characters; keys containing
embedded NUL mark the pointer as truncated because the C string cannot represent
them completely. Native cross-field errors identify the containing message or fields
object. Allocation failure (`OUT_OF_MEMORY`) remains distinguishable from a
configured budget (`LIMIT_EXCEEDED`). Diagnostics survive failed-load cleanup.

```c
vmf_header_json_options options = vmf_header_json_default_options();
vmf_header_json_document *document = NULL;
vmf_header_json_error error;
vmf_status status = vmf_header_json_load(VMF_ENVELOPE_C, bytes, size,
    &options, &document, &error);
if (status == VMF_OK) {
    const vmf_header_c *header = &vmf_header_json_header(document)->c;
    /* Use header and its owned pointees while document survives. */
    (void)header;
}
vmf_header_json_destroy(document);
```
