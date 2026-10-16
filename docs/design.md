# Codec design and accepted costs

The module owns a bounded, bidirectional event codec over immutable C schema
trees and explicit MIL-STD-2045-47001 envelope profiles. The [API](api.md),
[format](format.md) and [local requirements](conformance.md) define its contract.
Public-fixture checks supplement unit and acceptance tests; they do not establish
compatibility with a fielded system or provide MIL-STD-6017 definitions.

## Why these interfaces

[LibYAML's event model][yaml] separates parser/emitter from application object
models. A common event grammar supports incremental translation and replay
without constructing message-sized objects. VMF adopts balanced structural
events and values in the event itself; it also exposes original schema node
pointers for exact field identity. An object-tree decoder would simplify some
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
VMF descriptors remain immutable caller-authored C trees: there is no schema
compiler, generated source requirement or second opaque compiled schema.
The cost is runtime validation, pointer lifetimes and explicit dependency
rules. A compact schema bytecode could improve locality and startup work but
would introduce another representation and toolchain contract. C++ wrappers
therefore remain aliases and free functions, not owners or a parallel model.

The chosen interface accepts exact raw integer/scaled codes and chunked text,
octets and bits. Floating-point physical values would lose sentinel and exact
wire semantics. Explicit raw metadata requires more caller work but preserves
replay fidelity. Schema-driven payload packing is separate from fixed LSB-first
envelope packing; conflating these would obscure each schema's obligations.

## State and decomposition

Creation must validate profile, limits, schema identities and trees, compute
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
Dependency validation must prove scope and progress, including zero-width or
absent nodes, not merely impose byte limits. Header retention costs bounded
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
