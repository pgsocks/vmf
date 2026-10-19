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

The independent `vmf::header_json` companion owns profile-specific native
envelope configurations through `<vmf/header_json.h>` and procedural C++17
forwarders in `<vmf/header_json.hpp>`. It loads optional-member templates,
serializes raw inspection metadata and generates minimal exact-identity
templates. See the [header JSON guide](header-json.md) for ownership, bounded
allocation, diagnostics and the complete profile-specific format.

The codec validates the selected envelope profile and schema before exposing
body values. Errors use statuses and deterministic failure outputs: creation
clears the result on failure, reads clear their event on failure, dispatch
initializes its delivered count, and invalid measurements clear their result.
`NOT_IMPLEMENTED` remains a reserved explicit failure, never fabricated success.

## Ownership and configuration

Create a decoder/encoder with explicit limits and an optional schema registry
and allocator. A registry selects B/C/D/D1 once; `vmf_options_for_schemas`
selects that profile and default limits. Manually supplied options must match
the registry profile. Without a registry, select the profile in ordinary codec
options for independent envelope parsing/encoding. Creation resolves defaults,
validates scopes and duplicate wire identities, checks allocation arithmetic,
and allocates all bounded traversal, header and chunk storage. Reset and
streaming allocate nothing. Copy `vmf_default_limits` before overriding limits.
No caller workspace sizing or externally framed/headerless body APIs exist.
Encoder request and occurrence-path storage is also reserved at creation;
managed begin, request/reply processing and callback dispatch allocate nothing.

Allocator callbacks must be paired and return storage aligned for `max_align_t`.
NULL configuration selects malloc/free. The codec copies callback pointers and
caller state, and uses that copy for rollback and destruction; the configuration
struct itself need not survive creation. The callback state must survive
destruction. Every failure frees partial allocations and clears the handle;
allocation failure returns OUT_OF_MEMORY. Destroy and reset accept NULL. A handle is
single-threaded and must not be reentered, including from callbacks. Distinct
handles may share immutable schemas across threads.

The immutable registry, ordered message/index arrays, paths, keys, ranges and
all referenced data are borrowed until handle destruction. Distinct codecs
share definitions but own their resolved traversal and runtime state. No
public root node or generated node ID is required. Standalone validation and
codec construction use the same core resolver; the optional JSON loader
merely constructs these native definitions.

An ordinary index requires its typed number and bit width. Omitted encoding
means unsigned raw data for widths 1–64. Wider data requires explicit bits,
octets or text. Fixed width zero is invalid. Physical octet packing and field
significance independently default to LSB first and support explicit overrides.
Signed modes require an explicit representation. Text defaults to ASCII7;
capacity alone never enables variable length or literal termination.

Declaration order controls the wire. Group and repeat paths describe typed
membership; omitted paths mean no membership. Groups add no automatic bit.
Explicit FPI/GPI definitions control fields/groups; FRI/GRI definitions control
leading recurrence with a positive finite maximum. Indicator width is one,
with omitted width derived from the role. An ordinary one-bit field is data.
Presence and recurrence use one for present/more. A present recurrence has at
least one occurrence; a zero flag precedes its final body. Fixed and earlier
scalar-count repeats use explicit declarations and add no control bit.

Validation rejects crossing, reopened, contradictory or ambiguous scopes,
duplicate controls and unavailable dependencies. Conditions and lengths/counts
read only already completed dominating scalars in the current/ancestor scope,
never future, optional-unavailable, sibling-repeat or previous-iteration values.
Limits bound traversal work as well as bytes, including empty/absent regions.
Raw-code constraints and constants validate encoder values; they never supply
missing values. Scale metadata preserves exact rational factors/bias and raw
sentinel codes for application inspection, without floating-point conversion.

Payload definitions support VMF (UMF 2). Header-only operations preserve the
full envelope formats. Registry and message standard selections distinguish
inheritance, explicit absence, named editions and raw codes. A message replaces
the entire registry selection; absence is not a wildcard. B cannot emit a
standard version, C permits absence, and D/D1 require a selection. Envelope
profiles never imply a payload edition. MIL-STD-6017B resolves to code 8 under C
and code 7 under D/D1. Subtype presence is independent of its numeric value.

Identity comparison includes UMF, standard-version presence/value,
identification presence, FAD, number and subtype presence/value. Absent values
are ignored. Missing values never select the latest revision or act as
wildcards. Schema lookup returns UNKNOWN_MESSAGE before the first body field;
acknowledgment-only handling descriptors do not select a payload schema.

## Native definitions and inspection

```c
#include <vmf/wire.h>

static const uint32_t first[] = {1};
static const uint32_t second[] = {2};
static const vmf_index_schema indexes[] = {
	{.index_number = {first, 1}, .number_of_bits = 5},
	{.index_number = {second, 1}, .number_of_bits = 13}
};
static const vmf_message_schema messages[] = {
	{.message_number = {1, 1}, .indexes = indexes, .index_count = 2}
};
static const vmf_schema_set schemas = {
	.envelope = VMF_ENVELOPE_C, .messages = messages, .message_count = 1
};

/* Synthetic layout; no assigned message definition is implied. */
vmf_status create_example(vmf_decoder **decoder, vmf_error *error)
{
	vmf_options options = vmf_options_for_schemas(&schemas);
	return vmf_decoder_create(&options, decoder, error);
}
```

`vmf_schema_set_validate` validates the registry with explicit limits and an
optional allocator; `vmf_schema_validate` additionally selects a message in
that registry. Both release temporary storage before returning. Errors carry
schema/index array positions, a property category and related locations;
these are source locations, never generated traversal IDs.

`vmf_schema_message_id` resolves exact envelope identity.
`vmf_schema_find(message, index_number)` and `vmf_schema_find_key` return
canonical index definitions. `vmf_schema_index_info` returns the effective
kind, width, encoding, length, constraints, scale and indicator mapping for a
validated registry. Its output is caller-owned; borrowed nested pointers retain
the registry lifetime. It allocates nothing. `vmf_event.field_kind` identifies
the active value union even when the source omitted `encode_type`.

Structural event references distinguish `VMF_SCOPE_ROOT`, `VMF_SCOPE_GROUP`,
`VMF_SCOPE_REPEAT` and `VMF_SCOPE_REPEAT_BODY`. Root uses ID zero; other roles
use typed group/repeat IDs. A field event uses its canonical `index` pointer.
Private traversal addresses never appear in public events. An indicator index
remains available through lookup/inspection without emitting a FIELD event.

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
length, and MESSAGE_END reports the actual padded length. Literal text can report unknown totals on every chunk, including the final
one; the chunk's last flag ends the value. Multiple descriptors
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

The encoder consumes this same grammar with canonical index and structural
references from the registry. Independently created codecs using that registry
can replay events directly. A scalar
is one FIELD. Variable fields use contiguous chunks and wire-unit offsets;
text chunks carry valid UTF-8, octets carry bytes and bits use API bit i at
`data[i/8]` bit `i%8`. Presence bits and recurrence indicators
are codec controls rather than FIELD events. Explicit scalar count fields
remain events and cannot be silently backpatched. Repeats have a container
begin/end even for zero items, plus per-occurrence body groups. Absent fields/groups emit exactly one ABSENT.

VMF leading recurrence places its flag before each occurrence. Every occurrence
has a flag before its body indicating whether another occurrence follows.
Its body GROUP_BEGIN carries `more_present=true` and the `more` decision;
the decoder exposes that decision and the encoder validates it. The last body
has `more=false`; there is no extra terminal flag.

Literal text modes use bounded seven-bit ASCII with DELETE termination below
the maximum length and no terminator at the maximum. DELETE is a wire control,
not returned text. The encoder rejects DELETE as content. Payload padding is
always zero, with no per-field alignment or padding override.

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
their numeric values instead of trusting placeholder numbers. Field-derived lengths and counts remain ordinary preceding scalar values;
they are validated rather than backpatched.
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

## Schema-driven encoding

`vmf_encoder_begin_pdu` copies and validates a public `vmf_envelope_event`
without publishing output. After success the application may release the header
and all its pointees. It starts a managed PDU and clears previous measurements;
a failed begin preserves completed measurements. `vmf_encoder_bit_count` remains
cumulative until reset, including across successive managed and manual PDUs.

Call `vmf_encoder_next_request` to let VMF traverse the schema, write structural
controls and padding, and select each body-bearing descriptor. It returns one
request on OK, NEED_OUTPUT for automatic output exhaustion, or DONE after the
complete PDU. Every non-OK return clears the request output. Response-only
descriptors select no schema and request no application input.

| Request kind | Reply | Canonical target |
| --- | --- | --- |
| FIELD | Existing raw `vmf_field` value or chunk | `index`, scope NONE |
| PRESENCE | `present` boolean for an FPI/GPI-controlled region | Field `index` or GROUP scope |
| REPEAT_COUNT | Known `repeat_count` for an FRI/GRI repeat | REPEAT scope |

Every request identifies its original descriptor index and canonical message
schema. Its complete `path` contains `{repeat id, zero-based occurrence}` entries
from outermost to innermost active repeat body. A repeat-count request excludes
its target repeat because that body's first occurrence has not begun. Empty
paths use NULL and count zero. No private node or generated ID is exposed.

FIELD requests expose resolved `field_kind`, current wire-unit `offset_units`
and `total_units`; scalars use offset zero and total one. Literal text retains
`VMF_UNITS_UNKNOWN` totals through its final chunk. Existing UTF-8, chunk progress,
bit layout, raw-code and size constraints apply unchanged. A false PRESENCE
omits a region; a zero scalar or empty final chunk supplies a present value.
VMF derives fixed/scalar repeat counts and conditional presence, generates all
structural events, and checks the same depth/event limits as manual encoding.
Constants and explicit length/count scalar fields still require FIELD replies.

Repeated next calls before responding return the same request and path without
advancing. `vmf_encoder_respond` consumes one reply and returns OK only when that
reply is fully consumed. On NEED_OUTPUT, retry respond with identical active
reply members and unchanged chunk data; the reply struct may move. Keep borrowed
chunk storage alive until OK. While that retry is pending, next_request and a
different entry point return recoverable INVALID_STATE without changing it.
Invalid tags or changed retries return recoverable INVALID_ARGUMENT, and a valid
but mismatched reply kind returns recoverable TYPE_MISMATCH. The request survives
these errors. Unknown repeat counts return terminal LENGTH_REQUIRED; invalid
data, traversal and resource failures require reset and discarding output.

Manual event encoding remains available. Selecting another encoding mode inside
an active PDU returns recoverable INVALID_STATE. Either mode may start after
full completion or reset. Independent header output retains its explicit
`vmf_encoder_begin_envelope` lifecycle.

This C11 example maps a nested application source through public schema
references and occurrence paths. Its schema has an indicator-driven outer
repeat, an optional details group in each row, and an indicator-driven inner
repeat containing one unsigned code field. The application binds the canonical
message/index pointers and group/repeat IDs when configuring the source.

```c
#include <vmf/wire.h>
#include <stdio.h>

struct mapped_row {
	bool details_present;
	const uint64_t *codes;
	size_t code_count;
};

struct mapped_source {
	const vmf_message_schema *message;
	const vmf_index_schema *code;
	vmf_repeat_id rows_repeat;
	vmf_repeat_id codes_repeat;
	vmf_group_id details_group;
	const struct mapped_row *rows;
	size_t row_count;
};

static vmf_status map_reply(const struct mapped_source *source,
	const vmf_encode_request *request, vmf_encode_reply *reply)
{
	if (request->schema != source->message)
		return VMF_TYPE_MISMATCH;
	reply->kind = request->kind;
	if (request->kind == VMF_ENCODE_REQUEST_REPEAT_COUNT &&
		request->scope.id == source->rows_repeat &&
		!request->path_count) {
		reply->as.repeat_count = source->row_count;
		return VMF_OK;
	}
	if (!request->path_count ||
		request->path[0].id != source->rows_repeat ||
		request->path[0].occurrence >= source->row_count)
		return VMF_TYPE_MISMATCH;
	const struct mapped_row *row =
		&source->rows[request->path[0].occurrence];

	if (request->kind == VMF_ENCODE_REQUEST_PRESENCE &&
		request->scope.kind == VMF_SCOPE_GROUP &&
		request->scope.id == source->details_group) {
		reply->as.present = row->details_present;
		return VMF_OK;
	}
	if (request->kind == VMF_ENCODE_REQUEST_REPEAT_COUNT &&
		request->scope.id == source->codes_repeat &&
		request->path_count == 1) {
		reply->as.repeat_count = row->code_count;
		return VMF_OK;
	}
	if (request->kind != VMF_ENCODE_REQUEST_FIELD ||
		request->index != source->code ||
		request->path_count != 2 ||
		request->path[1].id != source->codes_repeat ||
		request->path[1].occurrence >= row->code_count)
		return VMF_TYPE_MISMATCH;
	reply->as.field.unsigned_integer.value =
		row->codes[request->path[1].occurrence];
	return VMF_OK;
}

static bool publish(FILE *destination, vmf_output *output)
{
	if (fwrite(output->data, 1, output->used, destination) != output->used)
		return false;
	output->used = 0;
	return true;
}

vmf_status write_mapped(vmf_encoder *writer,
	const vmf_envelope_event *envelope,
	const struct mapped_source *source, FILE *destination)
{
	uint8_t bytes[32];
	vmf_output output = {bytes, sizeof(bytes), 0};
	vmf_status status = vmf_encoder_begin_pdu(writer, envelope);

	if (status != VMF_OK)
		return status;
	for (;;) {
		vmf_encode_request request = {0};

		status = vmf_encoder_next_request(writer, &output, &request);
		if (!publish(destination, &output))
			return VMF_ABORTED;
		if (status == VMF_NEED_OUTPUT)
			continue;
		if (status != VMF_OK)
			return status;
		vmf_encode_reply reply = {0};

		status = map_reply(source, &request, &reply);
		if (status != VMF_OK)
			return status;
		do {
			status = vmf_encoder_respond(writer, &output, &reply);
			if (!publish(destination, &output))
				return VMF_ABORTED;
		} while (status == VMF_NEED_OUTPUT);
		if (status != VMF_OK)
			return status;
	}
}
```

The caller creates a normal writer with the source's registry and destroys it
after use. DONE means the complete PDU was written. File-output failure is an
application error in this example; reset or destroy before abandoning the PDU.
The source uses indexed lookup, so a second pass naturally starts at the first
request. A cursor-based source must explicitly rewind between passes.

`vmf_encoder_dispatch` composes next_request/respond. A provider receives the
request, writes a reply, and returns the existing CONTINUE, PAUSE or ABORT action.
Output exhaustion caches the reply and action; the provider is never invoked
again for that reply. Resume dispatch with the same provider/user until it is
consumed. A changed identity returns recoverable INVALID_ARGUMENT. PAUSE takes
effect after consumption, which can happen in a later call whose `delivered` is
zero. ABORT ignores the reply and is terminal; unknown actions are terminal
INVALID_ARGUMENT. `delivered` counts all callbacks on every return, including
pause/abort. Recoverable reply errors discard the unconsumed callback cache and
retain the request, allowing a corrected provider or primitive reply.

The C++17 example below reuses the mapping above. It first measures a single
body-bearing C descriptor, then populates the caller-owned public header's
already-present sizes and replays into a normal encoder. The caller supplies a
valid header, descriptor, registry and matching source. No private CLI header
JSON helpers, normalized schemas, generated IDs or whole-PDU buffer are needed.

```cpp
#include <vmf/wire.hpp>

static vmf::event_action provide(void *user,
	const vmf::encode_request *request, vmf::encode_reply *reply)
{
	auto *source = static_cast<const mapped_source *>(user);

	return map_reply(source, request, reply) == VMF_OK
		? VMF_EVENT_CONTINUE : VMF_EVENT_ABORT;
}

vmf::status measure_and_write(const vmf::schema_set *schemas,
	vmf::header_c *header, vmf::message_header *message,
	mapped_source *source, FILE *destination)
{
	vmf::options options = vmf::options_for_schemas(schemas);
	vmf::envelope_event envelope = {2, header, sizeof(*header), 0, 0};
	vmf::encoder *counter = nullptr;
	vmf::encoder *writer = nullptr;
	vmf::status status = vmf::encoder_create(
		&options, true, &counter, nullptr);
	vmf::measurement measurement = {};
	size_t delivered = 0;

	if (status == VMF_OK)
		status = vmf::encoder_begin_pdu(counter, &envelope);
	if (status == VMF_OK)
		status = vmf::encoder_dispatch(
			counter, nullptr, provide, source, &delivered);
	if (status == VMF_DONE)
		status = vmf::encoder_measurement(counter, 0, &measurement);
	vmf::encoder_destroy(counter);
	if (status != VMF_OK)
		return status;
	if (header->fields.header_size_octets.present)
		header->fields.header_size_octets.value =
			static_cast<uint32_t>(measurement.header_octets);
	if (message->message_size_octets.present)
		message->message_size_octets.value =
			static_cast<uint32_t>(measurement.body_octets);
	/* Indexed lookup needs no rewind. Keep raw values unchanged. */
	status = vmf::encoder_create(&options, false, &writer, nullptr);
	if (status == VMF_OK)
		status = vmf::encoder_begin_pdu(writer, &envelope);
	if (status == VMF_OK) {
		do {
			uint8_t bytes[32];
			vmf::output output = {bytes, sizeof(bytes), 0};

			status = vmf::encoder_dispatch(writer, &output,
				provide, source, &delivered);
			if (!publish(destination, &output)) {
				status = VMF_ABORTED;
				break;
			}
		} while (status == VMF_NEED_OUTPUT);
	}
	vmf::encoder_destroy(writer);
	return status;
}
```

Counting uses the same requests and validation with output=NULL. Size-field
presence must already match the output pass; numeric placeholders are measured.
For multiple descriptors inspect each original descriptor's measurement,
including zero bodies for responses, then populate every required MessageSize.
PDU totals become available at managed DONE. Applications retain explicit
measurement/replay and supply scalar length/count values; VMF adds no backpatching.
CoT mapping, engineering units, rounding and sentinel meaning remain application
responsibilities. Header JSON belongs to the tool using the public header structs.

Detailed nullability, outputs, constraints and invalidation rules are adjacent
to each declaration in the public header. Input/output/event buffers must not
overlap each other, schema storage or codec storage. No API throws exceptions,
performs transport I/O or changes a process-global registry.

Public schemas use ordered indexes and explicit controls. Generic node trees,
choices, prefix/remainder extents, alternative recurrence grammars and display
labels are outside the public schema model. The JSON loader accepts
`vmf-schema/1`. CLI recordings store canonical references and exact resolved
schema manifests.

## CLI recordings

`vmf --profile D1 --schema message.json --record capture.jsonl` reads binary
stdin and writes diagnostics to stderr. `--forward` additionally copies consumed
wire bytes to stdout. `--headers-only` needs no schemas and skips bodies; it
cannot record an event log. Multiple schema files must select one profile.
Each file's shared message standard is resolved before registries are combined.

`vmf replay capture.jsonl --schema message.json` stages and validates each PDU
before writing binary stdout. `--timed` preserves recorded intervals. The first
JSON Lines record has `format: "vmf-events"`, `version: 1`, `profile` and a
`schemas` array. Each schema entry contains its resolved identity, octet packing,
ordered functional index definitions and repeat declarations. Replay requires
an exact matching definition for each recorded identity before producing any
output. Additional unrelated loaded schemas are allowed; they cannot select
unguarded bodies. Unsupported archive versions are rejected.

Body event references use numeric `index_number` component arrays or a `scope`
object with `kind` (`root`, `group`, `repeat` or `repeat_body`) and `id`. They
include `depth` and decimal-string `occurrence`; generated node IDs and display
annotations are unsupported. Field values retain exact decimal strings for
64-bit values, UTF-8 text, and hex octet/bit chunks. Unknown lengths are null.
Changing a scalar value is supported; changing its schema requires a matching
manifest. Event ordering, sizes, controls and body padding remain validated by
the core encoder.
