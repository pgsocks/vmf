#include "cli.hpp"
#include <vmf/schema_json.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

static const size_t json_limit = 16 * 1024 * 1024;

struct arguments {
	vmf_envelope_version profile;
	const char **schemas;
	size_t schema_count;
	const char *record;
	const char *input;
	unsigned verbosity;
	bool replay;
	bool forward;
	bool headers;
	bool timed;
	bool help;
};

struct schema_storage {
	vmf_schema_json_document **documents;
	size_t count;
	vmf_message_schema *entries;
	vmf_schema_set set;
};

struct bytes {
	uint8_t *data;
	size_t size;
	size_t capacity;
};

static void help(void)
{
	fputs(
		"Usage: vmf [inspect] --profile B|C|D|D1 [OPTIONS]\n"
		"       vmf replay FILE|- [OPTIONS]\n\n"
		"Inspect binary VMF from stdin; print events to stderr.\n"
		"Replay editable JSON Lines as binary VMF on stdout.\n\n"
		"  --schema FILE   Load a VMF JSON schema; repeat for "
		"more files\n"
		"  -v, --verbose   Add detail; -vvvv includes wire hex\n"
		"  --record FILE   Create or truncate a complete event log\n"
		"  --forward       Forward consumed input bytes to stdout\n"
		"  --headers-only  Inspect headers and skip bodies, no "
		"recording\n"
		"  --timed         Replay recorded intervals between PDUs\n"
		"  -h, --help      Show this help\n\n"
		"Default: one PDU summary. -v: message boundaries. "
		"-vv: values.\n"
		"-vvv: every event and chunk metadata. -vvvv: headers "
		"and hex.\n"
		"An unsized PDU must be last and ends at stdin EOF.\n"
		"Replay uses the log's profile and validates sizes as "
		"written.\n"
		"Schemas must match resolved identities and functional "
		"definitions.\n"
		"Unknown lengths are null; 64-bit values are decimal "
		"strings.\n\n"
		"Example:\n"
		"  nc localhost 3000 | vmf --profile D1 --schema msg.json"
		" -vvvv --record session.jsonl\n"
		"  vmf replay session.jsonl --schema msg.json --timed"
		" | nc localhost 3001\n", stdout);
}

static const char *option_value(int argc, char **argv, int *index,
	const char *name)
{
	const char *argument = argv[*index];
	size_t length = strlen(name);

	if (strncmp(argument, name, length))
		return nullptr;
	if (argument[length] == '=')
		return argument + length + 1;
	if (argument[length] || *index + 1 >= argc)
		return nullptr;
	return argv[++*index];
}

static bool parse_arguments(int argc, char **argv, arguments *args)
{
	args->schemas = static_cast<const char **>(
		calloc(static_cast<size_t>(argc), sizeof(const char *)));
	if (!args->schemas)
		return false;
	int start = 1;

	if (argc > 1 && !strcmp(argv[1], "replay")) {
		args->replay = true;
		start = 2;
	} else if (argc > 1 && !strcmp(argv[1], "inspect")) {
		start = 2;
	}
	bool positional = false;

	for (int i = start; i < argc; ++i) {
		const char *arg = argv[i];
		const char *value;

		if (!positional && !strcmp(arg, "--")) {
			positional = true;
			continue;
		}
		if (positional || arg[0] != '-' || !strcmp(arg, "-")) {
			if (!args->replay || args->input)
				return false;
			args->input = arg;
		} else if (!strcmp(arg, "--help") || !strcmp(arg, "-h")) {
			args->help = true;
		} else if (!strcmp(arg, "--verbose")) {
			if (args->verbosity < 4)
				++args->verbosity;
		} else if (arg[0] == '-' && arg[1] == 'v') {
			for (size_t j = 1; arg[j]; ++j) {
				if (arg[j] != 'v')
					return false;
				if (args->verbosity < 4)
					++args->verbosity;
			}
		} else if (!strcmp(arg, "--forward")) {
			args->forward = true;
		} else if (!strcmp(arg, "--headers-only")) {
			args->headers = true;
		} else if (!strcmp(arg, "--timed")) {
			args->timed = true;
		} else if ((value = option_value(argc, argv, &i, "--schema"))) {
			if (!*value || !strcmp(value, "-"))
				return false;
			args->schemas[args->schema_count++] = value;
		} else if ((value = option_value(argc, argv, &i, "--record"))) {
			if (!*value || !strcmp(value, "-") || args->record)
				return false;
			args->record = value;
		} else if ((value =
			option_value(argc, argv, &i, "--profile"))) {
			if (args->profile ||
				!cli_profile_parse(value, &args->profile))
				return false;
		} else {
			return false;
		}
	}
	if (args->help)
		return true;
	if (args->replay)
		return args->input && !args->profile && !args->record &&
		       !args->headers && !args->forward;
	return args->profile && !args->timed &&
	       !(args->headers && args->record);
}

static bool reserve(bytes *buffer, size_t required, size_t limit)
{
	if (required > limit)
		return false;
	if (required <= buffer->capacity)
		return true;
	size_t capacity = buffer->capacity ? buffer->capacity : 4096;

	if (capacity > limit)
		capacity = limit;
	while (capacity < required)
		capacity = capacity > limit / 2 ? limit : capacity * 2;
	void *memory = realloc(buffer->data, capacity);

	if (!memory)
		return false;
	buffer->data = static_cast<uint8_t *>(memory);
	buffer->capacity = capacity;
	return true;
}

static bool append(bytes *buffer, const void *data, size_t size,
	size_t limit)
{
	if (buffer->size > limit || size > limit - buffer->size ||
		!reserve(buffer, buffer->size + size, limit))
		return false;
	if (size)
		memcpy(buffer->data + buffer->size, data, size);
	buffer->size += size;
	return true;
}

static bool read_file(const char *path, bytes *buffer)
{
	FILE *file = fopen(path, "rb");

	if (!file) {
		fprintf(stderr, "vmf: cannot read schema %s: %s\n",
			path, strerror(errno));
		return false;
	}
	uint8_t chunk[8192];
	bool success = true;

	for (;;) {
		size_t size = fread(chunk, 1, sizeof(chunk), file);

		if (!append(buffer, chunk, size, json_limit)) {
			fprintf(stderr, "vmf: schema %s exceeds input or"
				" memory limit\n", path);
			success = false;
			break;
		}
		if (size != sizeof(chunk)) {
			success = !ferror(file);
			if (!success)
				fprintf(stderr, "vmf: cannot read schema %s:"
					" %s\n", path, strerror(errno));
			break;
		}
	}
	if (fclose(file)) {
		fprintf(stderr, "vmf: cannot close schema %s: %s\n",
			path, strerror(errno));
		success = false;
	}
	return success;
}

static void destroy_schemas(schema_storage *storage)
{
	for (size_t i = 0; i < storage->count; ++i)
		vmf_schema_json_destroy(storage->documents[i]);
	free(storage->documents);
	free(storage->entries);
}

static bool load_schemas(const arguments *args, schema_storage *storage)
{
	vmf_schema_json_options options = vmf_schema_json_default_options();

	if (!args->schema_count)
		return true;
	storage->documents = static_cast<vmf_schema_json_document **>(
		calloc(args->schema_count, sizeof(*storage->documents)));
	storage->entries = static_cast<vmf_message_schema *>(
		calloc(options.limits.max_schemas, sizeof(*storage->entries)));
	if (!storage->documents || !storage->entries) {
		fputs("vmf: cannot allocate schema registry\n", stderr);
		return false;
	}
	storage->set.messages = storage->entries;
	for (size_t i = 0; i < args->schema_count; ++i) {
		bytes input = {};
		bool read = read_file(args->schemas[i], &input);
		vmf_schema_json_error error = {};
		vmf_schema_json_document *document = nullptr;
		vmf_status status = VMF_MALFORMED;
		const char *data = input.data ?
			reinterpret_cast<const char *>(input.data) : "";

		if (read)
			status = vmf_schema_json_load(data, input.size,
				&options, &document, &error);
		free(input.data);
		if (!read)
			return false;
		if (status != VMF_OK) {
			fprintf(stderr, "vmf: schema %s: %s, byte=%zu"
				" schema=%zu index=%zu property=%u\n",
				args->schemas[i], vmf_status_string(status),
				error.byte_offset, error.schema_index,
				error.index_position, error.property);
			return false;
		}
		storage->documents[storage->count++] = document;
		const vmf_schema_set *set = vmf_schema_json_schemas(document);

		if (storage->set.envelope &&
			storage->set.envelope != set->envelope) {
			fputs("vmf: schema files select different profiles\n",
				stderr);
			return false;
		}
		if (!storage->set.envelope)
			storage->set.standard = set->standard;
		storage->set.envelope = set->envelope;
		if (args->profile && args->profile != set->envelope) {
			fputs("vmf: --profile disagrees with schema profile\n",
				stderr);
			return false;
		}
		if (set->message_count > options.limits.max_schemas -
			storage->set.message_count) {
			fputs("vmf: aggregate schema limit exceeded\n", stderr);
			return false;
		}
		for (size_t j = 0; j < set->message_count; ++j) {
			vmf_message_id id = {};

			if (vmf_schema_message_id(set, &set->messages[j],
				&id) != VMF_OK)
				return false;
			vmf_message_schema *message =
				&storage->entries[storage->set.message_count++];

			*message = set->messages[j];
			message->standard.kind = id.standard_version.present ?
				VMF_STANDARD_RAW_CODE : VMF_STANDARD_ABSENT;
			message->standard.as.raw_code =
				id.standard_version.value;
		}

	}
	vmf_error error = {};
	vmf_status status = vmf_schema_set_validate(
		&storage->set, &options.limits, nullptr, &error);

	if (status != VMF_OK)
		fprintf(stderr, "vmf: schema registry: %s index=%zu"
			" property=%u\n",
			vmf_status_string(status), error.index_position,
			error.property);
	return status == VMF_OK;
}

static uint64_t monotonic_now(void *)
{
#ifdef _WIN32
	LARGE_INTEGER ticks, frequency;

	if (!QueryPerformanceCounter(&ticks) ||
		!QueryPerformanceFrequency(&frequency) ||
		frequency.QuadPart <= 0 ||
		static_cast<uint64_t>(frequency.QuadPart) >
			UINT64_MAX / 1000000000)
		return 0;
	uint64_t value = static_cast<uint64_t>(ticks.QuadPart);
	uint64_t scale = static_cast<uint64_t>(frequency.QuadPart);

	return value / scale * 1000000000 +
	       value % scale * 1000000000 / scale;
#else
	struct timespec value = {};

	if (clock_gettime(CLOCK_MONOTONIC, &value))
		return 0;
	return static_cast<uint64_t>(value.tv_sec) * 1000000000 +
	       static_cast<uint64_t>(value.tv_nsec);
#endif
}

static bool sleep_until(void *, uint64_t deadline)
{
	for (;;) {
		uint64_t now = monotonic_now(nullptr);

		if (now >= deadline)
			return true;
		uint64_t remaining = deadline - now;

#ifdef _WIN32
		uint64_t milliseconds = remaining / 1000000;

		if (remaining % 1000000)
			++milliseconds;
		Sleep(static_cast<DWORD>(
			milliseconds > 1000 ? 1000 : milliseconds));
#else
		struct timespec delay = {
			static_cast<time_t>(remaining / 1000000000),
			static_cast<long>(remaining % 1000000000)};

		if (nanosleep(&delay, nullptr) && errno != EINTR)
			return false;
#endif
	}
}

static bool write_stdout(const uint8_t *data, size_t size)
{
	while (size) {
#ifdef _WIN32
		unsigned count = size > 1048576 ? 1048576 :
			static_cast<unsigned>(size);
		int written = _write(_fileno(stdout), data, count);
#else
		ssize_t written = write(STDOUT_FILENO, data, size);
#endif
		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0) {
			fprintf(stderr, "vmf: stdout: %s\n", strerror(errno));
			return false;
		}
		size -= static_cast<size_t>(written);
		data += written;
	}
	return true;
}

static bool refill(uint8_t *buffer, size_t capacity, vmf_input *input)
{
	size_t retained = input->size - input->used;

	if (retained)
		memmove(buffer, buffer + input->used, retained);
	if (retained == capacity)
		return false;
	for (;;) {
#ifdef _WIN32
		int count = _read(_fileno(stdin), buffer + retained,
			static_cast<unsigned>(capacity - retained));
#else
		ssize_t count = read(STDIN_FILENO, buffer + retained,
			capacity - retained);
#endif
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0) {
			fprintf(stderr, "vmf: stdin: %s\n", strerror(errno));
			return false;
		}
		*input = {buffer, retained + static_cast<size_t>(count),
			0, count == 0};
		return true;
	}
}

static bool wire_bytes(const arguments *args, const uint8_t *data,
	size_t size, uint64_t offset)
{
	if (args->forward && !write_stdout(data, size))
		return false;
	if (args->verbosity < 4)
		return true;
	for (size_t i = 0; i < size; i += 16) {
		fprintf(stderr, "WIRE %012" PRIu64 ":", offset + i);
		size_t count = size - i < 16 ? size - i : 16;

		for (size_t j = 0; j < count; ++j)
			fprintf(stderr, " %02x", data[i + j]);
		fputc('\n', stderr);
	}
	return !ferror(stderr);
}

static void diagnostic(const char *operation, vmf_status status,
	const vmf_error *error, size_t pdu, uint64_t start, size_t line)
{
	fprintf(stderr, "vmf: %s: %s pdu=%zu",
		operation, vmf_status_string(status), pdu);
	if (line)
		fprintf(stderr, " line=%zu", line);
	if (error)
		fprintf(stderr, " stream_byte=%" PRIu64
			" bit=%" PRIu64 " descriptor=%zu index=%zu property=%u",
			start + error->offset / 8, error->offset,
			error->message_index, error->index_position,
			error->property);
	fputc('\n', stderr);
}

static bool summary(size_t pdu, uint64_t size)
{
	return fprintf(stderr, "PDU %zu complete octets=%" PRIu64 "\n",
		pdu, size) >= 0;
}

static const vmf_header_fields *header_fields(vmf_envelope_version profile,
	const void *header)
{
	switch (profile) {
	case VMF_ENVELOPE_B:
		return &static_cast<const vmf_header_b *>(header)->fields;
	case VMF_ENVELOPE_C:
		return &static_cast<const vmf_header_c *>(header)->fields;
	case VMF_ENVELOPE_D:
		return &static_cast<const vmf_header_d *>(header)->fields;
	case VMF_ENVELOPE_D1:
		return &static_cast<const vmf_header_d1 *>(header)->fields;
	}
	return nullptr;
}

static bool unsized_header(const vmf_header_fields *fields)
{
	for (size_t i = 0; i < fields->message_count; ++i)
		if (fields->messages[i].has_body &&
			!fields->messages[i].message_size_octets.present)
			return true;
	return false;
}

static void missing_identity(const vmf_header_fields *fields, size_t index)
{
	if (!fields || index >= fields->message_count)
		return;
	const vmf_message_id *id = &fields->messages[index].id;

	fprintf(stderr, "vmf: unknown identity user_message_format=%u",
		id->user_message_format);
	if (id->standard_version.present)
		fprintf(stderr, " standard_version=%u",
			id->standard_version.value);
	else
		fputs(" standard_version=absent", stderr);
	if (id->identification_present) {
		fprintf(stderr, " functional_area=%u message_number=%u",
			id->functional_area, id->message_number);
		if (id->subtype.present)
			fprintf(stderr, " subtype=%u", id->subtype.value);
		else
			fputs(" subtype=absent", stderr);
	} else {
		fputs(" identification=absent", stderr);
	}
	fputc('\n', stderr);
}

static bool emit_event(const arguments *args, FILE *record,
	const vmf_event *event, size_t pdu, uint64_t timestamp)
{
	bool display = args->verbosity >= 3 ||
		(args->verbosity >= 1 &&
		 (event->kind == VMF_EVENT_ENVELOPE ||
		  event->kind == VMF_EVENT_MESSAGE_BEGIN ||
		  event->kind == VMF_EVENT_MESSAGE_END)) ||
		(args->verbosity >= 2 &&
		 (event->kind == VMF_EVENT_FIELD ||
		  event->kind == VMF_EVENT_ABSENT));

	if (!display && !record)
		return true;
	cli_json context;

	cli_json_init(&context);
	yyjson_mut_doc *document = cli_event_to_json(
		&context, event, args->profile, timestamp);
	size_t size = 0;
	char *line = document ? yyjson_mut_write_opts(document,
		YYJSON_WRITE_ESCAPE_UNICODE, &context.allocator,
		&size, nullptr) : nullptr;
	bool success = line && size <= json_limit;

	if (success && record)
		success = fwrite(line, 1, size, record) == size &&
			  fputc('\n', record) != EOF && !fflush(record);
	if (success && display) {
		fprintf(stderr, "pdu=%zu %s ", pdu,
			cli_event_name(event->kind));
		if (event->kind == VMF_EVENT_ENVELOPE &&
			args->verbosity < 4) {
			fprintf(stderr, "version=%u header_octets=%" PRIu64,
				event->as.envelope.wire_version,
				event->as.envelope.header_octets);
		} else if (event->kind == VMF_EVENT_FIELD &&
			args->verbosity == 2) {
			yyjson_mut_val *root =
				yyjson_mut_doc_get_root(document);
			yyjson_mut_val *value =
				yyjson_mut_obj_get(root, "value");
			char *text = yyjson_mut_val_write_opts(value,
				YYJSON_WRITE_ESCAPE_UNICODE,
				&context.allocator, nullptr, nullptr);

			fprintf(stderr, "index=");
			vmf_index_number number = event->index->index_number;

			for (size_t i = 0; i < number.count; ++i)
				fprintf(stderr, "%s%" PRIu32, i ? "." : "",
					number.parts[i]);
			fprintf(stderr, " %s", text ? text : "");
			if (!text)
				success = false;
			if (text)
				context.allocator.free(
					context.allocator.ctx, text);
		} else {
			fwrite(line, 1, size, stderr);
		}
		fputc('\n', stderr);
		success = success && !ferror(stderr);
	}
	if (!success)
		fputs("vmf: cannot serialize or write event\n", stderr);
	if (line)
		context.allocator.free(context.allocator.ctx, line);
	yyjson_mut_doc_free(document);
	return success;
}

static vmf_status skip_body(const arguments *args, vmf_input *input,
	uint8_t *buffer, size_t capacity, uint64_t remaining,
	uint64_t *offset, uint64_t start)
{
	vmf_limits limits = vmf_default_limits();
	uint64_t limit = limits.max_pdu_octets;

	if (args->profile <= VMF_ENVELOPE_C && limit > 1048575)
		limit = 1048575;
	if (remaining == VMF_LENGTH_UNKNOWN) {
		uint64_t header = *offset - start;

		if (header > limit)
			return VMF_LIMIT_EXCEEDED;
		if (limits.max_message_octets < limit - header)
			limit = header + limits.max_message_octets;
	}

	while (remaining) {
		size_t available = input->size - input->used;

		if (!available) {
			if (input->final)
				return remaining == VMF_LENGTH_UNKNOWN ?
					VMF_OK : VMF_TRUNCATED;
			if (!refill(buffer, capacity, input))
				return VMF_ABORTED;
			continue;
		}
		size_t count = available;

		if (remaining != VMF_LENGTH_UNKNOWN && remaining < count)
			count = static_cast<size_t>(remaining);
		if (*offset - start > limit ||
			count > limit - (*offset - start))
			return VMF_LIMIT_EXCEEDED;
		if (!wire_bytes(args, input->data + input->used,
			count, *offset))
			return VMF_ABORTED;
		input->used += count;
		*offset += count;
		if (remaining != VMF_LENGTH_UNKNOWN)
			remaining -= count;
	}
	return VMF_OK;
}

static int inspect(const arguments *args, const vmf_schema_set *schemas)
{
	vmf_options options = {};

	options.profile = args->profile;
	options.limits = vmf_default_limits();
	options.schemas = schemas && schemas->message_count ? schemas : nullptr;
	vmf_decoder *decoder = nullptr;
	vmf_error error = {};
	vmf_status status = vmf_decoder_create(&options, &decoder, &error);

	if (status != VMF_OK) {
		diagnostic("create decoder", status, &error, 0, 0, 0);
		return 1;
	}
	FILE *record = args->record ? fopen(args->record, "wb") : nullptr;
	int result = 0;

	if (args->record && !record) {
		fprintf(stderr, "vmf: cannot write recording: %s\n",
			strerror(errno));
		result = 1;
	} else if (record) {
		cli_json context;

		cli_json_init(&context);
		yyjson_mut_doc *metadata = cli_metadata_to_json(&context,
			args->profile, schemas);
		size_t size = 0;
		char *line = metadata ? yyjson_mut_write_opts(metadata,
			YYJSON_WRITE_ESCAPE_UNICODE, &context.allocator,
			&size, nullptr) : nullptr;

		if (!line || size > json_limit ||
			fwrite(line, 1, size, record) != size ||
			fputc('\n', record) == EOF || fflush(record)) {
			fputs("vmf: cannot write recording metadata\n", stderr);
			result = 1;
		}
		if (line)
			context.allocator.free(context.allocator.ctx, line);
		yyjson_mut_doc_free(metadata);
	}

	uint8_t buffer[8192];
	vmf_input input = {buffer, 0, 0, false};
	size_t pdu = 0;
	uint64_t offset = 0, start = 0, origin = 0, timestamp = 0;
	uint64_t header_octets = 0, pdu_octets = VMF_LENGTH_UNKNOWN;
	const vmf_header_fields *fields = nullptr;
	bool begun = false;

	while (!result) {
		if (!begun) {
			if (input.used == input.size && !input.final &&
				!refill(buffer, sizeof(buffer), &input)) {
				result = 1;
				break;
			}
			if (input.used == input.size && input.final)
				break;
			status = args->headers ?
				vmf_decoder_begin_envelope(decoder) :
				vmf_decoder_begin_pdu(decoder);
			if (status != VMF_OK) {
				result = 1;
				break;
			}
			start = offset;
			uint64_t now = monotonic_now(nullptr);

			if (!pdu)
				origin = now;
			timestamp = now - origin;
			++pdu;
			begun = true;
		}
		vmf_event event = {};
		size_t before = input.used;

		status = vmf_decoder_next(decoder, &input, &event);
		size_t consumed = input.used - before;

		if (!wire_bytes(args, input.data + before, consumed, offset)) {
			result = 1;
			break;
		}
		offset += consumed;
		if (status == VMF_OK) {
			if (event.kind == VMF_EVENT_ENVELOPE) {
				header_octets = event.as.envelope.header_octets;
				pdu_octets = event.as.envelope.pdu_octets;
				fields = header_fields(args->profile,
					event.as.envelope.header);
			}
			if (!emit_event(args, record, &event, pdu, timestamp)) {
				result = 1;
				break;
			}
			if (event.kind == VMF_EVENT_PDU_END &&
				!summary(pdu, offset - start))
				result = 1;
		} else if (status == VMF_DONE) {
			if (args->headers) {
				uint64_t remaining = pdu_octets;

				if (remaining != VMF_LENGTH_UNKNOWN)
					remaining -= header_octets;
				status = skip_body(args, &input, buffer,
					sizeof(buffer), remaining,
					&offset, start);
				if (status != VMF_OK) {
					diagnostic("skip body", status,
						nullptr, pdu, start, 0);
					result = 1;
				} else if (!summary(pdu, offset - start)) {
					result = 1;
				}
			}
			begun = false;
		} else if (status == VMF_NEED_INPUT && !input.final) {
			if (!refill(buffer, sizeof(buffer), &input))
				result = 1;
		} else {
			error = vmf_decoder_error(decoder);
			diagnostic("decode", status, &error, pdu, start, 0);
			if (status == VMF_UNKNOWN_MESSAGE) {
				missing_identity(fields, error.message_index);
				fputs("vmf: supply a matching --schema or use"
					" --headers-only\n", stderr);
			}
			result = 1;
		}
	}
	if (record && fclose(record)) {
		fputs("vmf: cannot close recording\n", stderr);
		result = 1;
	}
	vmf_decoder_destroy(decoder);
	return result;
}

/* Returns 1 for a line, 0 for EOF, and -1 for I/O/allocation/size failure. */
static int read_line(FILE *file, bytes *line)
{
	line->size = 0;
	for (;;) {
		int character = fgetc(file);

		if (character == EOF) {
			if (ferror(file))
				return -1;
			return line->size ? 1 : 0;
		}
		if (character == '\n')
			return 1;
		uint8_t value = static_cast<uint8_t>(character);

		if (!append(line, &value, 1, json_limit))
			return -1;
	}
}

static vmf_status recorded_schemas(yyjson_val *root,
	const vmf_schema_set *available, vmf_envelope_version *profile,
	vmf_schema_set *recorded, vmf_message_schema **messages)
{
	vmf_status status = cli_metadata_from_json(root, available, profile);

	if (status != VMF_OK)
		return status;
	yyjson_val *definitions = yyjson_obj_get(root, "schemas");
	size_t count = yyjson_arr_size(definitions);
	auto *entries = count ? static_cast<vmf_message_schema *>(
		calloc(count, sizeof(vmf_message_schema))) : nullptr;

	if (count && !entries)
		return VMF_OUT_OF_MEMORY;
	for (size_t i = 0; i < count; ++i) {
		vmf_message_id id = {};
		const vmf_message_schema *message = nullptr;

		cli_id_from_json(yyjson_obj_get(yyjson_arr_get(definitions, i),
			"id"), &id);
		status = vmf_schema_select(available, &id, &message);
		if (status != VMF_OK) {
			free(entries);
			return status;
		}
		entries[i] = *message;
	}
	*recorded = {};
	recorded->envelope = *profile;
	recorded->messages = entries;
	recorded->message_count = count;
	if (available)
		recorded->standard = available->standard;
	*messages = entries;
	return VMF_OK;
}

static vmf_status encode_event(vmf_encoder *encoder,
	const vmf_event *event, bytes *buffer, size_t limit)
{
	for (;;) {
		uint8_t chunk[4096];
		vmf_output output = {chunk, sizeof(chunk), 0};
		vmf_status status = vmf_encoder_write(encoder, &output, event);

		if (!append(buffer, chunk, output.used, limit))
			return VMF_LIMIT_EXCEEDED;
		if (status != VMF_NEED_OUTPUT)
			return status;
	}
}

static bool check_measurement(vmf_encoder *encoder, uint64_t header,
	uint64_t pdu, size_t size)
{
	vmf_measurement measurement = {};
	vmf_status status = vmf_encoder_measurement(encoder, 0, &measurement);

	/* Shortened version 15 has no descriptors; all bytes are header. */
	uint64_t actual_header = status == VMF_OK ?
		measurement.header_octets : vmf_encoder_bit_count(encoder) / 8;

	return header == actual_header &&
	       (pdu == VMF_LENGTH_UNKNOWN || pdu == size);
}

static bool replay_output(const arguments *args, cli_clock *clock,
	uint64_t timestamp, const bytes *output, uint64_t sent, size_t pdu)
{
	return cli_schedule(clock, timestamp, args->timed) &&
	       wire_bytes(args, output->data, output->size, sent) &&
	       write_stdout(output->data, output->size) &&
	       summary(pdu, output->size);
}

static int replay(const arguments *args, const vmf_schema_set *schemas)
{
	FILE *file = !strcmp(args->input, "-") ?
		stdin : fopen(args->input, "rb");

	if (!file) {
		fprintf(stderr, "vmf: cannot read replay file: %s\n",
			strerror(errno));
		return 1;
	}
	bytes line = {}, output = {};
	vmf_schema_set recorded = {};
	vmf_message_schema *recorded_messages = nullptr;
	vmf_encoder *encoder = nullptr;
	const vmf_message_schema *active = nullptr;
	cli_clock clock = {monotonic_now, sleep_until, nullptr, 0, 0, 0, false};
	vmf_envelope_version profile = VMF_ENVELOPE_D1;
	size_t line_number = 0, pdu = 0;
	uint64_t header_octets = 0, pdu_octets = 0, timestamp = 0;
	uint64_t sent = 0;
	bool in_pdu = false, unsized_pending = false;
	int result = 0;
	vmf_options options = {};

	options.limits = vmf_default_limits();
	options.schemas = schemas && schemas->message_count ? schemas : nullptr;
	for (;;) {
		int read = read_line(file, &line);

		if (read == 0)
			break;
		++line_number;
		if (read < 0) {
			diagnostic("read log", VMF_LIMIT_EXCEEDED,
				nullptr, pdu, 0, line_number);
			result = 1;
			break;
		}
		cli_json context;

		cli_json_init(&context);
		const char *data = line.data ?
			reinterpret_cast<const char *>(line.data) : "";
		yyjson_doc *document = cli_json_read(&context, data, line.size);
		vmf_status status = document ? VMF_OK : VMF_MALFORMED;
		yyjson_val *root = yyjson_doc_get_root(document);

		if (line_number == 1) {
			status = recorded_schemas(root, schemas, &profile,
				&recorded, &recorded_messages);
			if (status == VMF_OK) {
				options.profile = profile;
				options.schemas = recorded.message_count ?
					&recorded : nullptr;
				status = vmf_encoder_create(
					&options, false, &encoder, nullptr);
			}
		} else if (status == VMF_OK) {
			cli_arena arena = {};
			vmf_event event = {};
			uint64_t time_ns = 0;

			status = cli_event_from_json(root, profile, &recorded,
				&active, &arena, &event, &time_ns);
			if (status == VMF_OK && unsized_pending)
				status = VMF_LENGTH_REQUIRED;
			if (status == VMF_OK && !in_pdu) {
				if (event.kind != VMF_EVENT_ENVELOPE) {
					status = VMF_INVALID_STATE;
				} else {
					++pdu;
					in_pdu = true;
					header_octets =
						event.as.envelope.header_octets;
					pdu_octets =
						event.as.envelope.pdu_octets;
					timestamp = time_ns;
					const auto *f = header_fields(
						profile,
						event.as.envelope.header);
					bool unsized = unsized_header(f);

					if (unsized != (pdu_octets ==
						VMF_LENGTH_UNKNOWN))
						status = VMF_OUT_OF_RANGE;
				}
			} else if (status == VMF_OK &&
				event.kind == VMF_EVENT_ENVELOPE) {
				status = VMF_INVALID_STATE;
			}
			if (status == VMF_OK)
				status = encode_event(encoder, &event, &output,
					static_cast<size_t>(
						options.limits.max_pdu_octets));
			if (status == VMF_OK && args->verbosity) {
				arguments display = *args;

				display.profile = profile;
				if (!emit_event(&display, nullptr, &event,
					pdu, timestamp))
					status = VMF_ABORTED;
			}
			if (status == VMF_OK &&
				event.kind == VMF_EVENT_PDU_END) {
				if (!check_measurement(encoder, header_octets,
					pdu_octets, output.size)) {
					status = VMF_OUT_OF_RANGE;
				} else {
					in_pdu = false;
					unsized_pending = pdu_octets ==
						VMF_LENGTH_UNKNOWN;
					if (!unsized_pending) {
						if (!replay_output(args,
							&clock, timestamp,
							&output, sent, pdu))
							status = VMF_ABORTED;
						sent += output.size;
						output.size = 0;
					}
					vmf_encoder_reset(encoder);
					active = nullptr;
				}
			}
			cli_arena_destroy(&arena);
		}
		yyjson_doc_free(document);
		if (status != VMF_OK) {
			vmf_error error = vmf_encoder_error(encoder);

			diagnostic("replay", status, &error, pdu, 0,
				line_number);
			result = 1;
			break;
		}
	}
	if (!result && (in_pdu || !line_number)) {
		diagnostic("replay", VMF_TRUNCATED,
			nullptr, pdu, 0, line_number);
		result = 1;
	}
	if (!result && unsized_pending &&
		(!cli_schedule(&clock, timestamp, args->timed) ||
		 !wire_bytes(args, output.data, output.size, sent) ||
		 !write_stdout(output.data, output.size) ||
		 !summary(pdu, output.size)))
		result = 1;
	if (file != stdin && fclose(file)) {
		fputs("vmf: cannot close replay file\n", stderr);
		result = 1;
	}
	vmf_encoder_destroy(encoder);
	free(line.data);
	free(output.data);
	free(recorded_messages);
	return result;
}

int main(int argc, char **argv)
{
	/* Parse arguments, retain schemas, and dispatch inspect or replay.
	 * Keep binary stdout separate from diagnostics and stage replay PDUs.
	 */
	setvbuf(stderr, nullptr, _IONBF, 0);
#ifdef _WIN32
	if (_setmode(_fileno(stdin), _O_BINARY) == -1 ||
		_setmode(_fileno(stdout), _O_BINARY) == -1) {
		fputs("vmf: cannot configure binary standard streams\n",
			stderr);
		return 1;
	}
#else
	signal(SIGPIPE, SIG_IGN);
#endif
	arguments args = {};
	bool valid = parse_arguments(argc, argv, &args);
	int result = 0;

	if (!valid) {
		fputs("vmf: invalid arguments; use --help\n", stderr);
		result = 2;
	} else if (args.help) {
		help();
		if (fflush(stdout) || ferror(stdout)) {
			fputs("vmf: cannot write help\n", stderr);
			result = 1;
		}
	} else {
		schema_storage storage = {};

		if (!load_schemas(&args, &storage))
			result = 1;
		else if (args.replay)
			result = replay(&args, &storage.set);
		else
			result = inspect(&args, &storage.set);
		destroy_schemas(&storage);
	}
	free(args.schemas);
	return result;
}
