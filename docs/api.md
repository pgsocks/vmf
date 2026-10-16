# Public API contract

`<vmf/wire.h>` is the C11 interface. `<vmf/wire.hpp>` supplies C++17 namespace
aliases and thin free-function wrappers with identical behavior. Link the
static CMake target `vmf::vmf`. Logical structs are not packed wire overlays.
The [format reference](format.md) defines the supported envelopes; no built-in
MIL-STD-6017 schemas are supplied.

The optional `vmf::schema_json` companion accepts VMF-specific JSON payload
definitions and owns the resulting native schema set. Its C11 header is
`<vmf/schema_json.h>`; `<vmf/schema_json.hpp>` supplies procedural C++17
forwarders in `vmf::schema_json`. See the [JSON schema guide](schema-json.md)
for the format, limits, diagnostics, ownership, and a load-to-codec example.
Directly authored C schemas continue to use `vmf::vmf` alone.

The codec validates the selected envelope profile and schema before exposing
body values. Errors use statuses and deterministic failure outputs: creation
clears the result on failure, reads clear their event on failure, dispatch
initializes its delivered count, and invalid measurements clear their result.
`NOT_IMPLEMENTED` remains a reserved explicit failure, never fabricated success.

## Ownership and configuration

Create a decoder/encoder with an explicit B/C/D/D1 profile, explicit limits and
an optional schema set and allocator. Copy the result of `vmf_default_limits`
and override only the limits that your application needs. Passing no schema set
permits independent envelope parsing/encoding. Creation validates complete schema
trees and duplicate identities, checks allocation arithmetic, and allocates all
bounded traversal, header and chunk storage. Reset and streaming allocate nothing.
No caller workspace sizing or externally framed/headerless body APIs exist.

Allocator callbacks must be paired and return storage aligned for `max_align_t`.
NULL configuration selects malloc/free. The codec copies callback pointers and
caller state, and uses that copy for rollback and destruction; the configuration
struct itself need not survive creation. The callback state must survive
destruction. Partial failures free every acquired allocation and return
OUT_OF_MEMORY with a NULL handle. Destroy and reset accept NULL. A handle is
single-threaded and must not be reentered, including from callbacks. Distinct
handles may share immutable schemas across threads.

The schema set, pointer array, nodes, strings, ranges and all referenced data
are borrowed until handle destruction. Nodes form an acyclic unshared tree per
schema; the same tree may be used by multiple message identities. Root is an
unconditional group. IDs are nonzero and unique within a tree. Conditions and
extents may refer only to already completed dominating scalar occurrences in
the current/ancestor scope. They cannot read future, optional-unavailable,
sibling-repeat or previous-iteration values. Limits bound traversal work as
well as bytes, including zero-bit nodes. Validate widths, progress, ranges and
branch disjointness before touching wire data. The per-message event limit
includes MESSAGE_BEGIN and MESSAGE_END as well as field and group events.

Identity comparison includes UMF, standard-version presence/value,
identification presence, FAD, number and subtype presence/value. Absent values
are ignored. Missing values never select the latest revision or act as
wildcards. Schema lookup returns UNKNOWN_MESSAGE before the first body field;
acknowledgment-only handling descriptors do not select a payload schema.

## Reading a header

```c
#include <vmf/wire.h>

vmf_status inspect_header(const uint8_t *data, size_t size)
{
	vmf_options options = {0};
	options.profile = VMF_ENVELOPE_D1;
	options.limits = vmf_default_limits();
	vmf_decoder *decoder = NULL;
	vmf_error error;
	vmf_status status = vmf_decoder_create(&options, &decoder, &error);
	if (status != VMF_OK)
		return status;
	status = vmf_decoder_begin_envelope(decoder);
	if (status == VMF_OK) {
		vmf_input input = {data, size, 0, true};
		vmf_event event;
		status = vmf_decoder_next(decoder, &input, &event);
		/* OK returns ENVELOPE, including measured header_octets and optional
		 * pdu_octets. Inspect event.as.envelope.header before destruction.
		 * A following next returns DONE without reading any body bytes.
		 */
	}
	vmf_decoder_destroy(decoder);
	return status;
}
```

In incremental use, set `final=false` until no later octets exist, retain the
handle on NEED_INPUT and provide a fresh span with `used=0`. `used` advances
on every return; bytes before it may be released. Begin itself consumes no
input. A successful begin invalidates the prior header; a failed begin preserves
it. Decoded header structs, arrays and spans survive later read calls until
successful begin, reset or destruction. Field chunks have a shorter lifetime:
only until the next next/dispatch/reset/destruction. Copy data when retaining
it. Scalar `vmf_event_copy_field` copies the individual struct; it does not
deep-copy a text/octet/bit pointer.

Header-only parsing accepts legal omitted lengths and exposes valid compression
and security metadata without requiring schemas. Profile/version mismatch
returns UNSUPPORTED_ENVELOPE; version 15 is allowed only under C/D/D1 and does
not change the configured profile. Present HeaderSize must match measured
padded header octets. `VMF_LENGTH_UNKNOWN` indicates an uncomputable PDU size.

## Complete PDUs and the shared event grammar

Begin full decoding with `vmf_decoder_begin_pdu`, then call next until DONE.
Present MessageSize fields delimit bodies and preserve subsequent concatenated
PDUs. For a sole descriptor without MessageSize, `input.final` supplies the end
of the enclosing delimited input; until then the decoder requests more input.
Do not concatenate a later PDU into that input. MESSAGE_BEGIN reports an unknown
length, and MESSAGE_END reports the actual padded length. A terminal REMAINING
field and a LITERAL field can report unknown totals on every chunk, including
the final one; the chunk's last flag ends the value. Multiple descriptors
require their standard size fields.

Compression and unsupported SPI processing return
UNSUPPORTED_ENVELOPE_FEATURE before consuming the affected body. SPI 0 specifies
no encryption: its plaintext body can be traversed, while exposed signatures
remain unverified. The library never decompresses, decrypts, verifies signatures
or generates network replies.

```text
ENVELOPE
  MESSAGE_BEGIN(descriptor index and exact identity)
    GROUP_BEGIN(root)
      FIELD | ABSENT | GROUP_BEGIN ... GROUP_END
    GROUP_END(root)
  MESSAGE_END
  ... additional body-bearing descriptors ...
PDU_END
```

The encoder consumes this same grammar with original node pointers. A scalar
is one FIELD. Variable fields use contiguous chunks and wire-unit offsets;
text chunks carry valid UTF-8, octets carry bytes and bits use API bit i at
`data[i/8]` bit `i%8`. Prefix counts, presence bits and recurrence indicators
are codec controls rather than FIELD events. Explicit scalar count fields
remain events and cannot be silently backpatched. Repeats have a container
begin/end even for zero items, plus per-occurrence body groups. Choices wrap
only the selected arm. Absent nodes emit exactly one ABSENT.

`RECURRENCE_BEFORE` matches the VMF leading recurrence grammar. Every occurrence
has a flag before its body indicating whether another occurrence follows.
Its body GROUP_BEGIN carries `more_present=true` and the `more` decision;
the decoder exposes that decision and the encoder validates it. The last body
has `more=false`; there is no extra terminal flag. `MORE_BEFORE` and `MORE_AFTER`
remain distinct generic grammars.

`LITERAL` text extents use bounded seven-bit ASCII with DELETE termination below
the maximum length and no terminator at the maximum. DELETE is a wire control,
not returned text. The encoder rejects DELETE as content. Payload padding is
always zero; schema `padding_bit` must be zero.

FIELD lifetime and API chunk sizes do not change the wire grammar. Non-final
chunks must make progress; an empty field emits one empty final event. Root
completion validates schema, exact size and final padding before MESSAGE_END.
A callback adapter calls next repeatedly; PAUSE consumes its event, ABORT is
terminal until reset, and delivered counts all callbacks including pause/abort.

## Encoding, retries and measurement

An idle normal writer accepts ENVELOPE, subsequent body events and PDU_END.
Call `vmf_encoder_begin_envelope` first for independent header encoding.
Output.used is advanced on every return. On NEED_OUTPUT drain the buffer and
retry the *identical* event and unchanged pointees. The codec records partial
progress and must not duplicate bits. On OK all input belonging to that event
may be reused; required envelope metadata has been copied. Malformed values,
traversal and resource errors are terminal until reset; discard previously
published bytes. Invalid arguments and changed retry arguments preserve usable
state so the caller can correct them and resume. Output.used still records any
progress before the argument error.

Normal encoding validates numeric MessageSize values when present and HeaderSize
when present. Use an envelope-scoped counting encoder to avoid buffering an
entire PDU: create with counting_only=true, replay events using output=NULL,
then inspect `vmf_encoder_measurement` for each original descriptor index.
Populate the normal header's size values and replay the same event source to
a normal encoder. Counting validates the intended presence of sizes and measures
their numeric values instead of trusting placeholder numbers. It still needs
inline length-prefix totals and known lengths for ambiguous REMAINING fields.
There is no body-only counting entry point. PDU totals are available after
PDU_END; an incomplete measurement returns INVALID_STATE, clearing output.

C++ callers use the same explicit cleanup and status flow:

```cpp
#include <vmf/wire.hpp>

vmf::options options = {};
options.profile = VMF_ENVELOPE_C;
options.limits = vmf::default_limits();
vmf::encoder *writer = nullptr;
vmf::error error;
vmf::status status = vmf::encoder_create(&options, true, &writer, &error);
/* Write ENVELOPE/body/PDU_END with nullptr output,
 * measure each descriptor, then replay into a normal writer.
 */
vmf::encoder_destroy(writer);
```

Detailed nullability, outputs, constraints and invalidation rules are adjacent
to each declaration in the public header. Input/output/event buffers must not
overlap each other, schema storage or codec storage. No API throws exceptions,
performs transport I/O or changes a process-global registry.

A minimal synthetic binary-file schema can be authored directly in C11:

```c
static const vmf_node octet = {
	.id = 2,
	.kind = VMF_NODE_FIELD,
	.as.field = {
		.kind = VMF_FIELD_UNSIGNED,
		.as.scalar.encoding = {
			.bit_width = 8,
			.order = VMF_BIT_LSB_FIRST,
			.representation = VMF_INTEGER_UNSIGNED
		}
	}
};
static const vmf_node root = {
	.id = 1,
	.kind = VMF_NODE_GROUP,
	.as.group = {.children = &octet, .child_count = 1}
};
static const vmf_message_schema binary = {
	.id = {.user_message_format = 1},
	.root = &root,
	.octet_order = VMF_BIT_LSB_FIRST
};
static const vmf_message_schema *const entries[] = {&binary};
static const vmf_schema_set schemas = {.schemas = entries, .count = 1};
/* Set options.schemas = &schemas before create. This exact identity has
 * neither message-standard version nor VMF identification. It does not match
 * a header whose optional identity fields are present, even if all are zero.
 */
```

For a custom allocator, initialize a `vmf_allocator` with allocation and release
callbacks and caller state, set `options.allocator` before creation, and keep
only the callback state alive after create. On OUT_OF_MEMORY, the handle is
NULL and the library has released partial storage; applications may adjust
limits or retry creation. Changing the allocator configuration struct after
successful create cannot change which callback frees the handle. User callback
state itself must not be changed incompatibly while allocations remain live.
