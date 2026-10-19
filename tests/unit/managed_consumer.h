#ifndef VMF_TEST_MANAGED_CONSUMER_H
#define VMF_TEST_MANAGED_CONSUMER_H

#include <vmf/wire.h>
#include <string.h>

/* A caller-owned nested object and native schema, with no JSON dependency. */
struct managed_consumer {
	uint32_t address;
	vmf_repeat_component membership[2];
	vmf_repeat_definition repeats[2];
	vmf_index_schema index;
	vmf_message_schema schema;
	vmf_schema_set set;
	vmf_message_header message;
	vmf_header_d1 header;
	vmf_envelope_event envelope;
	uint8_t rows[2][2];
	size_t cursor;
};

static inline void managed_consumer_initialize(struct managed_consumer *c)
{
	memset(c, 0, sizeof(*c));
	c->address = 1;
	c->membership[0].id = 10;
	c->membership[1].id = 20;
	c->repeats[0].id = 10;
	c->repeats[0].count = 2;
	c->repeats[1].id = 20;
	c->repeats[1].count = 2;
	c->index.index_number.parts = &c->address;
	c->index.index_number.count = 1;
	c->index.number_of_bits = 8;
	c->index.repeat_code.parts = c->membership;
	c->index.repeat_code.count = 2;
	c->index.key.data = "value";
	c->index.key.size = 5;
	c->schema.message_number.functional_area = 1;
	c->schema.message_number.number = 1;
	c->schema.indexes = &c->index;
	c->schema.index_count = 1;
	c->schema.repeats = c->repeats;
	c->schema.repeat_count = 2;
	c->set.envelope = VMF_ENVELOPE_D1;
	c->set.messages = &c->schema;
	c->set.message_count = 1;
	c->set.standard.kind = VMF_STANDARD_RAW_CODE;
	c->set.standard.as.raw_code = 0;
	c->message.has_body = true;
	c->message.message_size_octets.present = true;
	vmf_schema_message_id(&c->set, &c->schema, &c->message.id);
	c->header.fields.messages = &c->message;
	c->header.fields.message_count = 1;
	c->header.fields.header_size_octets.present = true;
	c->envelope.wire_version = VMF_ENVELOPE_D1;
	c->envelope.header = &c->header;
	c->envelope.object_size = sizeof(c->header);
	c->envelope.pdu_octets = VMF_LENGTH_UNKNOWN;
	c->rows[0][0] = 0;
	c->rows[0][1] = 7;
	c->rows[1][0] = 11;
	c->rows[1][1] = 255;
}

static inline bool managed_consumer_reply(struct managed_consumer *c,
	const vmf_encode_request *request, vmf_encode_reply *reply)
{
	if (request->kind != VMF_ENCODE_REQUEST_FIELD ||
		request->schema != &c->schema || request->index != &c->index ||
		request->descriptor_index || request->path_count != 2 ||
		request->path[0].id != 10 || request->path[1].id != 20 ||
		request->path[0].occurrence >= 2 ||
		request->path[1].occurrence >= 2 ||
		request->field_kind != VMF_FIELD_UNSIGNED ||
		request->offset_units || request->total_units != 1)
		return false;
	memset(reply, 0, sizeof(*reply));
	reply->kind = request->kind;
	reply->as.field.unsigned_integer.value =
		c->rows[request->path[0].occurrence]
		       [request->path[1].occurrence];
	++c->cursor;
	return true;
}

#endif
