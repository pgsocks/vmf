# Complete-message authoring

`<vmf/author.h>` and the procedural C++17 forwards in `<vmf/author.hpp>`
provide complete-PDU authoring on top of the managed streaming encoder. Link
`vmf::vmf`; there is no JSON or CLI dependency. Use the streaming API directly
when the application must own incremental publication or inspection/replay.

Load or define and validate the schema registry, resolve canonical message and
index references once, and construct a native revision-specific envelope.
Supply `vmf_author_field` entries for raw field values and
`vmf_author_control` entries for GPI optional-group presence or FRI/GRI repeat
counts. Addresses include a descriptor position, the canonical message/index
or group/repeat scope, and **every** outer-to-inner repeat occurrence. Occurrence
positions are zero-based; a repeat's own count address excludes that repeat.

```c
vmf_author_options options = vmf_author_options_for_schemas(schemas);
vmf_author_result *result = NULL;
vmf_author_error error;
vmf_status status = vmf_author_encode(&options, &envelope,
	fields, field_count, controls, control_count, &result, &error);
if (status == VMF_OK) {
	/* Consume vmf_author_result_data/size before destroying result. */
}
vmf_author_result_destroy(result);
```

The function validates every supplied entry before encoding, including entries
in inactive branches and beyond a currently selected dynamic repeat count.
`vmf_author_validate_address`, `vmf_author_validate_field`, and
`vmf_author_validate_control` expose the same static checks independently of
an input event. They require an already validated registry. Scope inspection
returns the canonical control definition, repeat ancestry and absolute bounds.
`vmf_author_validate_header` checks authored envelope syntax and exact body
identities without requiring field values.

Optional field presence follows whether its complete raw entry exists. Optional
groups use an explicit GPI control when supplied; otherwise descendant field
and positive control membership determines presence at the current occurrence
path prefix. Fixed repeats, scalar-derived counts, conditional branches,
ordering and padding come from the schema and managed encoder. Constants,
preceding scalar length fields, and scalar/indicator repeat counts are explicit
inputs. The authoring helper does not guess application semantics or fill a
mandatory value. A missing active mandatory value or contradictory length/count
fails and returns no output.

Only envelope sizes are derived. Caller headers and field values are immutable
and remain borrowed during the synchronous call; complete text and binary spans
must set `last=true`. Text is counted UTF-8 interpreted using the schema's wire
encoding. Bits have an explicit bit count and zero unused high bits. The helper
measures and emits the same raw entries in separate managed passes. The result
owns its complete output and per-descriptor measurements. No partial bytes are
published and no file I/O occurs.

Use the activity getters to distinguish consumed values/controls from valid
dormant configuration. Unused entries are successful: deciding whether omitted
application data warrants a warning or a failure belongs to the caller. Errors
identify the offending input entry or missing canonical field/control, descriptor
and complete repeat path. Error paths copy the first 64 components and report
the full length; applications using deeper custom limits must account for that
explicit diagnostic truncation.

The default memory budget is 64 MiB and includes codec storage, output, activity
and measurement arrays. The caller's borrowed input storage is excluded.
Custom paired allocator callbacks apply to every owned allocation and must remain
callable through result destruction. Failures release every partial allocation.
Normal output uses 64 KiB slices; smaller slices can verify output exhaustion.
The entry/request match is a bounded linear scan, suitable for configured
message authoring with modest mapping tables.

New authoring rejects all security groups and material, including authentication
bytes and signed acknowledgments, and rejects unsupported compression. The helper
does not authenticate messages. Raw envelope inspection and event replay retain
their separate codec behavior.
