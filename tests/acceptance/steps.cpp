#include <gtest/gtest.h>
#include <cucumber-cpp/autodetect.hpp>
#include <vmf/wire.h>
#include "vectors.h"
#include "../unit/fixtures.hpp"
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

struct codec_context {
	vmf_decoder *reader = nullptr;
	vmf_encoder *writer = nullptr;
	uint32_t part = 1;
	vmf_index_schema field = {};
	vmf_message_schema schema = {};
	vmf_schema_set schemas = {};
	vmf_options options = {};
	vmf_status status = VMF_OK;
	std::vector<uint8_t> bytes;
	std::vector<uint8_t> encoded;
	std::vector<vmf_event_kind> kinds;
	size_t consumed = 0;
	size_t header_size = 0;
	size_t fragment = SIZE_MAX;
	bool header_only = false;
	bool replay = false;
	bool final = true;
	vmf_event_action action = VMF_EVENT_CONTINUE;
	size_t callbacks = 0;
	size_t envelope_callbacks = 0;
	size_t bodies = 0;
	const vmf_header_fields *decoded_fields = nullptr;
	vmf_header retained = {};

	~codec_context()
	{
		vmf_decoder_destroy(reader);
		vmf_encoder_destroy(writer);
	}
};

static vmf_event_action receive(void *user, const vmf_event *event)
{
	auto *context = static_cast<codec_context *>(user);

	++context->callbacks;
	if (event->kind == VMF_EVENT_ENVELOPE)
		++context->envelope_callbacks;
	return context->action;
}

static void configure(codec_context *c, int profile)
{
	c->options.profile = (vmf_envelope_version)profile;
	c->options.limits = vmf_default_limits();
	c->options.schemas = &c->schemas;
	c->field.index_number = {&c->part, 1};
	c->field.number_of_bits = 8;
	c->schema.message_number = {1, 1};
	c->schema.indexes = &c->field;
	c->schema.index_count = 1;
	c->schemas = {(vmf_envelope_version)profile, &c->schema, 1, {}};
	if (profile != 1) {
		c->schemas.standard.kind = VMF_STANDARD_RAW_CODE;
		c->schemas.standard.as.raw_code = 0;
	}
	const uint8_t *data = nullptr;
	size_t size = 0;

	switch (profile) {
	case 1:
		data = vector_vmf_b;
		size = sizeof(vector_vmf_b);
		break;
	case 2:
		data = vector_vmf_c;
		size = sizeof(vector_vmf_c);
		break;
	case 3:
		data = vector_vmf_d;
		size = sizeof(vector_vmf_d);
		break;
	case 4:
		data = vector_vmf_d1;
		size = sizeof(vector_vmf_d1);
		break;
	}
	c->bytes.assign(data, data + size);
	c->header_size = size - 1;
}

GIVEN("^the literal envelope for profile ([1-4])$")
{
	REGEX_PARAM(int, profile);
	cucumber::ScenarioScope<codec_context> c;

	configure(c.get(), profile);
}

GIVEN("^no payload schemas$")
{
	cucumber::ScenarioScope<codec_context> c;

	c->options.schemas = nullptr;
}

GIVEN("^an exact identity with another standard version$")
{
	cucumber::ScenarioScope<codec_context> c;

	c->schema.standard.kind = VMF_STANDARD_RAW_CODE;
	c->schema.standard.as.raw_code = 1;
}

GIVEN("^input arrives one octet at a time$")
{
	cucumber::ScenarioScope<codec_context> c;

	c->fragment = 1;
}

GIVEN("^the selected profile is ([1-4])$")
{
	REGEX_PARAM(int, profile);
	cucumber::ScenarioScope<codec_context> c;

	c->options.profile = (vmf_envelope_version)profile;
}

GIVEN("^the wire version is ([0-9]+)$")
{
	REGEX_PARAM(int, version);
	cucumber::ScenarioScope<codec_context> c;

	c->bytes[0] = (c->bytes[0] & 0xf0) | version;
}

GIVEN("^one extra PDU follows$")
{
	cucumber::ScenarioScope<codec_context> c;
	auto copy = c->bytes;

	c->bytes.insert(c->bytes.end(), copy.begin(), copy.end());
}

GIVEN("^the last header padding bit is one$")
{
	cucumber::ScenarioScope<codec_context> c;

	c->bytes[c->header_size - 1] |= 0x80;
}

GIVEN("^the input ends before the body$")
{
	cucumber::ScenarioScope<codec_context> c;

	c->bytes.resize(c->header_size);
}

GIVEN("^the header size is incorrect$")
{
	cucumber::ScenarioScope<codec_context> c;

	c->bytes[1] ^= 2;
}

GIVEN("^events are replayed with one octet of output capacity$")
{
	cucumber::ScenarioScope<codec_context> c;

	c->replay = true;
}

static void replay_event(codec_context *c, const vmf_event *event)
{
	vmf_status status;
	size_t attempts = 0;

	do {
		uint8_t byte = 0;
		vmf_output output = {&byte, 1, 0};

		status = vmf_encoder_write(c->writer, &output, event);
		if (output.used)
			c->encoded.push_back(byte);
		ASSERT_LT(++attempts, 4096u);
	} while (status == VMF_NEED_OUTPUT);
	ASSERT_EQ(VMF_OK, status);
}

static void decode(codec_context *c, bool header_only)
{
	if (header_only)
		c->options.schemas = nullptr;
	c->status = vmf_decoder_create(&c->options, &c->reader, nullptr);
	if (c->status != VMF_OK)
		return;
	if (c->replay) {
		c->status = vmf_encoder_create(
			&c->options, false, &c->writer, nullptr);
		if (c->status != VMF_OK)
			return;
	}
	c->status = header_only ? vmf_decoder_begin_envelope(c->reader)
				: vmf_decoder_begin_pdu(c->reader);
	if (c->status != VMF_OK)
		return;
	size_t attempts = 0;

	while (c->consumed <= c->bytes.size()) {
		size_t size =
			std::min(c->fragment, c->bytes.size() - c->consumed);
		vmf_input input = {c->bytes.data() + c->consumed, size, 0,
			c->final && c->consumed + size == c->bytes.size()};
		vmf_event event = {};

		c->status = vmf_decoder_next(c->reader, &input, &event);
		c->consumed += input.used;
		ASSERT_LE(input.used, input.size);
		ASSERT_LT(++attempts, 4096u);
		if (c->status == VMF_OK) {
			c->kinds.push_back(event.kind);
			if (event.kind == VMF_EVENT_ENVELOPE) {
				EXPECT_EQ(c->header_size,
					event.as.envelope.header_octets);
				const void *header = event.as.envelope.header;
				switch (c->options.profile) {
				case VMF_ENVELOPE_B:
					c->decoded_fields = &static_cast<
						const vmf_header_b *>(header)
								     ->fields;
					break;
				case VMF_ENVELOPE_C:
					c->decoded_fields = &static_cast<
						const vmf_header_c *>(header)
								     ->fields;
					break;
				case VMF_ENVELOPE_D:
					c->decoded_fields = &static_cast<
						const vmf_header_d *>(header)
								     ->fields;
					break;
				case VMF_ENVELOPE_D1:
					c->decoded_fields = &static_cast<
						const vmf_header_d1 *>(header)
								     ->fields;
					break;
				}
			}
			if (event.kind == VMF_EVENT_MESSAGE_BEGIN) {
				EXPECT_EQ(c->bodies++, event.as.message.index);
				EXPECT_EQ(&c->schema, event.schema);
			}
			if (event.kind == VMF_EVENT_FIELD) {
				EXPECT_EQ(&c->field, event.index);
				EXPECT_EQ(
					0x5au, event.as.field.value
						       .unsigned_integer.value);
			}
			if (c->replay)
				replay_event(c, &event);
		} else if (c->status != VMF_NEED_INPUT) {
			break;
		} else if (c->consumed == c->bytes.size()) {
			break;
		}
	}
}

WHEN("^I inspect the envelope independently$")
{
	cucumber::ScenarioScope<codec_context> c;

	decode(c.get(), true);
}

WHEN("^I decode one complete PDU$")
{
	cucumber::ScenarioScope<codec_context> c;

	decode(c.get(), false);
}

THEN("^the status is "
	"(DONE|MALFORMED|TRUNCATED|UNKNOWN_MESSAGE|LENGTH_REQUIRED|UNSUPPORTED_"
	"ENVELOPE|UNSUPPORTED_ENVELOPE_FEATURE)$")
{
	REGEX_PARAM(std::string, expected);
	cucumber::ScenarioScope<codec_context> c;
	vmf_status value = VMF_DONE;

	if (expected == "MALFORMED")
		value = VMF_MALFORMED;
	if (expected == "TRUNCATED")
		value = VMF_TRUNCATED;
	if (expected == "UNKNOWN_MESSAGE")
		value = VMF_UNKNOWN_MESSAGE;
	if (expected == "LENGTH_REQUIRED")
		value = VMF_LENGTH_REQUIRED;
	if (expected == "UNSUPPORTED_ENVELOPE")
		value = VMF_UNSUPPORTED_ENVELOPE;
	if (expected == "UNSUPPORTED_ENVELOPE_FEATURE")
		value = VMF_UNSUPPORTED_ENVELOPE_FEATURE;
	EXPECT_EQ(value, c->status) << vmf_status_string(c->status);
}

THEN("^only the header octets were consumed$")
{
	cucumber::ScenarioScope<codec_context> c;

	EXPECT_EQ(c->header_size, c->consumed);
	ASSERT_EQ(1u, c->kinds.size());
	EXPECT_EQ(VMF_EVENT_ENVELOPE, c->kinds[0]);
}

THEN("^exactly one PDU was consumed$")
{
	cucumber::ScenarioScope<codec_context> c;

	EXPECT_EQ(c->header_size + 1, c->consumed);
	const std::vector<vmf_event_kind> expected = {VMF_EVENT_ENVELOPE,
		VMF_EVENT_MESSAGE_BEGIN, VMF_EVENT_GROUP_BEGIN, VMF_EVENT_FIELD,
		VMF_EVENT_GROUP_END, VMF_EVENT_MESSAGE_END, VMF_EVENT_PDU_END};

	EXPECT_EQ(expected, c->kinds);
}

THEN("^the replay is byte identical$")
{
	cucumber::ScenarioScope<codec_context> c;

	EXPECT_EQ(c->bytes, c->encoded);
}

GIVEN("^a shortened response with originator 1 and recipient 2$")
{
	cucumber::ScenarioScope<codec_context> c;

	/* 15, absent compression, present originator/URN1/no name,
	 * present recipient, no recurrence, URN2/no name, zero fill.
	 */
	const uint8_t bytes[] = {
		0xef, 0x00, 0x00, 0x00, 0x15, 0x00, 0x00, 0x00};

	c->bytes.assign(bytes, bytes + sizeof(bytes));
	c->header_size = sizeof(bytes);
}

WHEN("^I dispatch with callback (pause|abort)$")
{
	REGEX_PARAM(std::string, action);
	cucumber::ScenarioScope<codec_context> c;

	c->action = action == "pause" ? VMF_EVENT_PAUSE : VMF_EVENT_ABORT;
	c->status = vmf_decoder_create(&c->options, &c->reader, nullptr);
	if (c->status != VMF_OK)
		return;
	c->status = vmf_decoder_begin_pdu(c->reader);
	if (c->status != VMF_OK)
		return;
	vmf_input input = {c->bytes.data(), c->bytes.size(), 0, true};
	size_t delivered = 0;

	c->status = vmf_decoder_dispatch(
		c->reader, &input, receive, c.get(), &delivered);
	c->consumed = input.used;
	EXPECT_EQ(c->callbacks, delivered);
}

THEN("^one callback was consumed and processing (paused|aborted)$")
{
	REGEX_PARAM(std::string, action);
	cucumber::ScenarioScope<codec_context> c;

	EXPECT_EQ(1u, c->callbacks);
	EXPECT_EQ(action == "paused" ? VMF_PAUSED : VMF_ABORTED, c->status);
}

WHEN("^I reset after partial input and decode again$")
{
	cucumber::ScenarioScope<codec_context> c;

	c->status = vmf_decoder_create(&c->options, &c->reader, nullptr);
	if (c->status != VMF_OK)
		return;
	c->status = vmf_decoder_begin_pdu(c->reader);
	if (c->status != VMF_OK)
		return;
	vmf_input input = {c->bytes.data(), 1, 0, false};
	vmf_event event = {};

	EXPECT_EQ(VMF_NEED_INPUT, vmf_decoder_next(c->reader, &input, &event));
	vmf_decoder_reset(c->reader);
	EXPECT_EQ(VMF_OK, vmf_decoder_error(c->reader).status);
	vmf_decoder_destroy(c->reader);
	c->reader = nullptr;
	decode(c.get(), false);
}

GIVEN("^the literal (missing|compressed|ack|multiple|security) variant$")
{
	REGEX_PARAM(std::string, variant);
	cucumber::ScenarioScope<codec_context> c;
	const uint8_t *data = nullptr;
	size_t size = 0;
	size_t bodies = 1;

	if (variant == "missing") {
		data = vector_vmf_missing;
		size = sizeof(vector_vmf_missing);
	} else if (variant == "compressed") {
		data = vector_compressed;
		size = sizeof(vector_compressed);
	} else if (variant == "security") {
		data = vector_security;
		size = sizeof(vector_security);
	} else if (variant == "ack") {
		data = vector_ack;
		size = sizeof(vector_ack);
		bodies = 0;
		c->options.schemas = nullptr;
	} else {
		data = vector_vmf_multiple;
		size = sizeof(vector_vmf_multiple);
		bodies = 2;
	}
	c->bytes.assign(data, data + size);
	c->header_size = size - bodies;
}

THEN("^there were no payload events$")
{
	cucumber::ScenarioScope<codec_context> c;
	const std::vector<vmf_event_kind> expected = {
		VMF_EVENT_ENVELOPE, VMF_EVENT_PDU_END};

	EXPECT_EQ(expected, c->kinds);
	EXPECT_EQ(c->header_size, c->consumed);
}

THEN("^two body messages completed in descriptor order$")
{
	cucumber::ScenarioScope<codec_context> c;

	EXPECT_EQ(2, std::count(c->kinds.begin(), c->kinds.end(),
			     VMF_EVENT_MESSAGE_END));
	EXPECT_EQ(c->bytes.size(), c->consumed);
}

GIVEN("^the D1 precedence code is reserved$")
{
	cucumber::ScenarioScope<codec_context> c;

	/* Synthetic VMF identity adds 16 bits before precedence. */
	c->bytes[11] |= 0x70;
}

GIVEN("^the D1 operation requests retransmission without an originator DTG$")
{
	cucumber::ScenarioScope<codec_context> c;

	/* Retransmit bit 91 requires the originator DTG group. */
	c->bytes[11] |= 0x08;
}

WHEN("^I measure and encode the fictitious D1 payload in two chunks$")
{
	cucumber::ScenarioScope<codec_context> c;

	c->field.encode_type = VMF_ENCODE_OCTETS;
	c->field.number_of_bits = 40;
	vmf_message_header message = {};

	ASSERT_EQ(VMF_OK, vmf_schema_message_id(
		&c->schemas, &c->schema, &message.id));
	message.has_body = true;
	message.file_name = {true, {"x", 1}};
	message.message_size_octets = {true, 0};
	vmf_header_d1 header = {};

	header.fields.header_size_octets = {true, 0};
	header.fields.messages = &message;
	header.fields.message_count = 1;
	vmf_event events[8] = {};

	events[0].kind = VMF_EVENT_ENVELOPE;
	events[0].as.envelope = {4, &header, sizeof(header), 15, 20};
	events[1].kind = VMF_EVENT_MESSAGE_BEGIN;
	events[1].as.message = {message.id, 0, 5};
	events[2].kind = VMF_EVENT_GROUP_BEGIN;
	events[2].scope = {VMF_SCOPE_ROOT, 0};
	events[2].as.group.count = 1;
	events[3].kind = VMF_EVENT_FIELD;
	events[3].index = &c->field;
	events[3].field_kind = VMF_FIELD_OCTETS;
	events[3].depth = 1;
	events[3].as.field.total_units = 5;
	events[3].as.field.value.octets = {fictitious_d1, 2, false};
	events[4] = events[3];
	events[4].as.field.offset_units = 2;
	events[4].as.field.value.octets = {fictitious_d1 + 2, 3, true};
	events[5].kind = VMF_EVENT_GROUP_END;
	events[5].scope = {VMF_SCOPE_ROOT, 0};
	events[5].as.group.count = 1;
	events[6].kind = VMF_EVENT_MESSAGE_END;
	events[6].as.message = events[1].as.message;
	events[7].kind = VMF_EVENT_PDU_END;
	c->status = vmf_encoder_create(&c->options, true, &c->writer, nullptr);
	if (c->status != VMF_OK)
		return;
	for (const auto &event : events) {
		c->status = vmf_encoder_write(c->writer, nullptr, &event);
		if (c->status != VMF_OK)
			return;
	}
	vmf_measurement measurement = {};

	c->status = vmf_encoder_measurement(c->writer, 0, &measurement);
	if (c->status != VMF_OK)
		return;
	EXPECT_EQ(15u, measurement.header_octets);
	EXPECT_EQ(5u, measurement.body_octets);
	EXPECT_EQ(20u, measurement.pdu_octets);
	message.message_size_octets.value = measurement.body_octets;
	header.fields.header_size_octets.value = measurement.header_octets;
	vmf_encoder_destroy(c->writer);
	c->writer = nullptr;
	c->status = vmf_encoder_create(&c->options, false, &c->writer, nullptr);
	if (c->status != VMF_OK)
		return;
	for (const auto &event : events)
		replay_event(c.get(), &event);
	c->bytes = fixtures::packet(VMF_ENVELOPE_D1,
		std::vector<uint8_t>(fictitious_d1,
			fictitious_d1 + sizeof(fictitious_d1)));
	c->status = VMF_DONE;
}

struct allocation_probe {
	bool fail;
	size_t live;
};

static void *probe_allocate(void *user, size_t size)
{
	auto *probe = static_cast<allocation_probe *>(user);

	if (probe->fail)
		return nullptr;
	void *memory = std::malloc(size);

	if (memory)
		++probe->live;
	return memory;
}

static void probe_deallocate(void *user, void *memory)
{
	auto *probe = static_cast<allocation_probe *>(user);

	--probe->live;
	std::free(memory);
}

WHEN("^allocation fails once and then succeeds$")
{
	cucumber::ScenarioScope<codec_context> c;
	allocation_probe probe = {true, 0};
	vmf_allocator allocator = {probe_allocate, probe_deallocate, &probe};

	c->options.allocator = &allocator;
	c->status = vmf_decoder_create(&c->options, &c->reader, nullptr);
	EXPECT_EQ(VMF_OUT_OF_MEMORY, c->status);
	EXPECT_EQ(nullptr, c->reader);
	EXPECT_EQ(0u, probe.live);
	probe.fail = false;
	c->status = vmf_decoder_create(&c->options, &c->reader, nullptr);
	EXPECT_EQ(VMF_OK, c->status);
	vmf_decoder_destroy(c->reader);
	c->reader = nullptr;
	c->options.allocator = nullptr;
	EXPECT_EQ(0u, probe.live);
}

THEN("^the retained header still describes one VMF body$")
{
	cucumber::ScenarioScope<codec_context> c;

	ASSERT_NE(nullptr, c->decoded_fields);
	ASSERT_EQ(1u, c->decoded_fields->message_count);
	const vmf_message_header *message = c->decoded_fields->messages;

	ASSERT_NE(nullptr, message);
	EXPECT_TRUE(message->has_body);
	EXPECT_EQ(2u, message->id.user_message_format);
	ASSERT_TRUE(message->file_name.present);
	EXPECT_EQ("x", std::string(message->file_name.value.data,
			       message->file_name.value.size));
}

THEN("^the header exposes an unverified forty-octet signature$")
{
	cucumber::ScenarioScope<codec_context> c;

	ASSERT_NE(nullptr, c->decoded_fields);
	ASSERT_EQ(1u, c->decoded_fields->message_count);
	const vmf_security *security = &c->decoded_fields->messages[0].security;

	EXPECT_TRUE(security->present);
	EXPECT_EQ(0u, security->spi);
	EXPECT_TRUE(security->authentication_a.present);
	EXPECT_EQ(40u, security->authentication_a.size);
}

WHEN("^I resume callback delivery$")
{
	cucumber::ScenarioScope<codec_context> c;

	if (c->status != VMF_PAUSED)
		return;
	c->action = VMF_EVENT_CONTINUE;
	vmf_input input = {c->bytes.data() + c->consumed,
		c->bytes.size() - c->consumed, 0, true};
	size_t delivered = 0;

	c->status = vmf_decoder_dispatch(
		c->reader, &input, receive, c.get(), &delivered);
	c->consumed += input.used;
}

THEN("^the envelope was delivered once among seven callbacks$")
{
	cucumber::ScenarioScope<codec_context> c;

	EXPECT_EQ(1u, c->envelope_callbacks);
	EXPECT_EQ(7u, c->callbacks);
}
