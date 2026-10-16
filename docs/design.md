# Codec design and accepted costs

The module owns a bounded, bidirectional event codec over immutable C schema
message/index definitions and explicit MIL-STD-2045-47001 envelope profiles. The [API](api.md),
[format](format.md) and [local requirements](conformance.md) define its contract.
Public-fixture checks supplement unit and acceptance tests; they do not establish
compatibility with a fielded system or provide MIL-STD-6017 definitions.

## Why these interfaces

[LibYAML's event model][yaml] separates parser/emitter from application object
models. A common event grammar supports incremental translation and replay
without constructing message-sized objects. VMF adopts balanced structural
events and values in the event itself. Ordinary field events identify the
original index definitions; structural events identify canonical group and
repeat roles. These references work across separate codecs using one registry.
An object-tree decoder would simplify some
callers, but its body-sized storage and second serialization model conflict
with the bounded streaming goal. A callback-only API would make backpressure
and manual replay harder, so push delivery composes a pull primitive.

[zlib's streaming interface][zlib] establishes familiar conventions for caller
I/O spans, explicit status, retained partial state and paired allocator hooks.
VMF owns handles and bounded buffers, copies allocator configuration and frees
partial allocations on failure. Stream and reset never allocate. This trades
maximum configured storage at creation for predictable processing. Caller-owned
workspaces would avoid allocation entirely, but add sizing/alignment/version
coupling to every application and complicate safe failure recovery. Allocator
callbacks preserve embedded integration without exposing private layout.

[Nanopb's descriptors and callbacks][nano] demonstrate that small native
metadata can drive serialization while streams avoid whole-message storage.
VMF descriptors are immutable ordered C arrays. A registry selects an envelope
profile and shared defaults; each message supplies ordered index definitions
and explicit functional refinements. Core validation and construction share
default resolution, exact identity checks, scope reconstruction and control
ownership. The codec owns private normalized traversal storage. Callers need
no public tree, generated node IDs, separate builder or compilation step.
C++ wrappers remain aliases and free functions for the C contract.

The optional JSON loader converts strict `vmf-schema/1` syntax into these same
native definitions. It owns descriptors and converts notation, while the core
decides semantics. It cannot supply different defaults or validation rules.
Input bytes can be released after loading; codecs borrow the document
descriptors until destruction.

The chosen interface accepts exact raw integer/scaled codes and chunked text,
octets and bits. Floating-point physical values would lose sentinel and exact
wire semantics. Explicit raw metadata requires more caller work but preserves
replay fidelity. Schema-driven payload packing is separate from fixed LSB-first
envelope packing; conflating these would obscure each schema's obligations.

## State and decomposition

Creation must validate profile, limits, schema identities and scopes, compute
bounded storage with overflow checks, and publish a handle only after all
allocations succeed. Allocation rollback and destruction use the copied
allocator. The private implementation is divided into allocation, schema,
envelope, bit operations, traversal and adapters. Errors preserve explicit
progress and output contracts rather than synthesizing successful results.

Decoder states are idle, envelope, body selection, traversal, end and terminal
error. Successful begin is transactional and invalidates retained header
storage; failed begin preserves it. Header arena storage is separate from
reusable field chunks. The parser checkpoints partial values and indicators,
never retains caller input spans, and never advances into the next PDU.
Header-only mode ends after the header event without consulting payload schemas.
Full mode resolves each body-bearing descriptor exactly. Present MessageSize
fields supply body boundaries; a sole unsized descriptor uses input.final as
its enclosing-input boundary and publishes its actual size at MESSAGE_END.

Encoder states mirror this grammar, with an additional pending-event cursor
for output exhaustion. On NEED_OUTPUT the same event must be retried; switching
entry points or mutating pointees during a retry violates the contract. Only
complete octets are published. Counting shares validation and traversal,
validates size-field presence and records per-descriptor measurements while
replacing numeric size comparisons with measurement. It is not a second
serializer or a backpatch mechanism.

The field convenience and dispatch adapters compose the primitives. Callback
pause is after consumption; callback abort becomes terminal until reset.
Reset clears cursor, event, counter and error state without freeing configuration
storage. Shared schemas contain no mutable traversal state.

## Boundaries and risks

Profile selection is explicit because revisions alter fields and conditionality;
version detection alone cannot silently change the caller's contract. Envelope
inspection accepts metadata independent of payload capability. Full traversal
rejects unsupported transformations before consuming bodies. A final input flag
can delimit a sole size-omitting message; concatenation requires explicit sizes.
SPI 0 plaintext remains traversable without claiming signature verification.
Other SPI metadata remains inspectable without assuming plaintext.

Exact schema selection avoids wildcard-version ambiguity and trial parsing.
Envelope profiles never imply a payload-standard edition. Shared selections
are replaced as a unit by message overrides, preserving absence and present
zero. A codec rejects a profile that disagrees with its registry.

Groups and repeats follow declaration order and typed membership paths.
Explicit indicators own presence or leading recurrence bits; ordinary one-bit
fields remain data. Scope membership alone never adds a bit. Ambiguous or
crossing scopes fail validation instead of acquiring arbitrary nesting.
Recurrence always includes a final occurrence preceded by a zero flag.
Schema values constrain application input; they never manufacture missing
encoder values.

The public model excludes generic trees, choices, prefix/remainder extents,
alternative repeat grammars and display labels. Private traversal primitives
may retain these features where useful. Event logs use canonical references
and resolved functional manifests to reject replay against incompatible
definitions.
Dependency validation must prove scope and progress, including empty fields
and absent regions, not merely impose byte limits. Header retention costs bounded
arena storage; chunk retention deliberately remains short. Measurement and
replay cost a second traversal and require a replayable event source in exchange
for bounded memory and no output backpatching. Applications needing queued
messages, transport adapters, physical-value conversion or cryptographic
verification must provide those layers separately.

Tests distinguish helpers and failure-output scaffolding from desired behavior.
All product tests, builds, C11/C++17 consumer checks and runner infrastructure
must pass. C/D1 interoperability with the public DFDL definition has been
tested using Apache Daffodil in both directions; the
[testing guide](testing.md#interoperability-record) records the tested scope.
No pending steps, skips or expected-failure flags hide behavior gaps.
Ambiguities and conflicting standard guidance are recorded in the format
reference; they are not resolved by copying a convenient fixture.

[yaml]: https://pyyaml.org/wiki/LibYAML
[zlib]: https://zlib.net/manual.html
[nano]: https://jpa.kapsi.fi/nanopb/docs/concepts.html
