#ifndef VMF_CLI_HPP_INCLUDED
#define VMF_CLI_HPP_INCLUDED

#include <vmf/wire.h>
#include <yyjson.h>
#include <stddef.h>

/* Private tool interfaces; none are installed or part of the VMF ABI. */
struct cli_json {
	yyjson_alc allocator;
	size_t used;
};

struct cli_arena {
	void *blocks;
	size_t used;
};

/* Contexts must outlive documents allocated through them. 64 MiB budget. */
void cli_json_init(cli_json *context);
yyjson_doc *cli_json_read(cli_json *context, const char *data, size_t size);
yyjson_mut_doc *cli_json_new(cli_json *context);
void *cli_arena_alloc(cli_arena *arena, size_t size);
void cli_arena_destroy(cli_arena *arena);

/* Strict decimal strings for 64-bit values; null denotes UNKNOWN if allowed. */
bool cli_json_u64(yyjson_val *value, uint64_t *result, bool unknown);
bool cli_json_i64(yyjson_val *value, int64_t *result);
yyjson_mut_val *cli_json_uint(yyjson_mut_doc *document, uint64_t value,
	bool unknown);
yyjson_mut_val *cli_json_int(yyjson_mut_doc *document, int64_t value);
yyjson_mut_val *cli_id_to_json(yyjson_mut_doc *document,
	const vmf_message_id *id);
bool cli_id_from_json(yyjson_val *value, vmf_message_id *id);

/* Headers preserve active fields and optional presence, using arena storage. */
yyjson_mut_val *cli_header_to_json(yyjson_mut_doc *document,
	vmf_envelope_version profile, const void *header);
bool cli_header_from_json(yyjson_val *value, vmf_envelope_version profile,
	cli_arena *arena, vmf_header *header);

/* Canonical numeric references and exact resolved schema manifests. */
yyjson_mut_val *cli_index_to_json(yyjson_mut_doc *document,
	vmf_index_number number);
bool cli_index_from_json(yyjson_val *value, cli_arena *arena,
	vmf_index_number *number);
yyjson_mut_val *cli_scope_to_json(yyjson_mut_doc *document,
	vmf_scope_ref scope);
bool cli_scope_from_json(yyjson_val *value,
	const vmf_message_schema *message, vmf_scope_ref *scope);
yyjson_mut_doc *cli_metadata_to_json(cli_json *context,
	vmf_envelope_version profile, const vmf_schema_set *schemas);
vmf_status cli_metadata_from_json(yyjson_val *value,
	const vmf_schema_set *schemas, vmf_envelope_version *profile);

/* Returned JSON owns copied event data. Caller frees the returned document. */
yyjson_mut_doc *cli_event_to_json(cli_json *context,
	const vmf_event *event, vmf_envelope_version profile, uint64_t time_ns);

/* Decode one record. Schemas outlive its use; arena owns all other pointees.
 * active selects body indexes and is updated by a valid MESSAGE_BEGIN.
 * Errors do not publish an event. time_ns applies only to ENVELOPE.
 */
vmf_status cli_event_from_json(yyjson_val *value,
	vmf_envelope_version profile, const vmf_schema_set *schemas,
	const vmf_message_schema **active, cli_arena *arena,
	vmf_event *event, uint64_t *time_ns);

const char *cli_event_name(vmf_event_kind kind);
const char *cli_profile_name(vmf_envelope_version profile);
bool cli_profile_parse(const char *text, vmf_envelope_version *profile);

/* Absolute monotonic scheduling. Hooks allow deterministic timing tests. */
struct cli_clock {
	uint64_t (*now)(void *user);
	bool (*sleep_until)(void *user, uint64_t deadline);
	void *user;
	uint64_t origin;
	uint64_t first;
	uint64_t previous;
	bool started;
};
bool cli_schedule(cli_clock *clock, uint64_t timestamp, bool timed);

#endif
