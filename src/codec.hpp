#ifndef VMF_PRIVATE_CODEC_HPP
#define VMF_PRIVATE_CODEC_HPP

#include "payload.hpp"
#include "envelope.hpp"
#include "schema_validation.hpp"

namespace vmf_internal
{

struct configuration {
	vmf_envelope_version profile;
	vmf_limits limits;
	vmf_allocator allocator;
	vmf_schema_set schemas;
};

enum decoder_phase {
	DECODER_IDLE,
	DECODER_HEADER,
	DECODER_SELECT,
	DECODER_PAYLOAD,
	DECODER_MESSAGE_END,
	DECODER_PDU_END,
	DECODER_DONE,
	DECODER_FAILED
};
enum encoder_phase {
	ENCODER_IDLE,
	ENCODER_HEADER,
	ENCODER_SELECT,
	ENCODER_PAYLOAD,
	ENCODER_DONE,
	ENCODER_FAILED
};

const vmf_header_fields *fields(const vmf_envelope_event *event);
vmf_error make_error(vmf_status status);

} /* namespace vmf_internal */

struct vmf_decoder {
	vmf_internal::configuration config;
	void *storage;
	uint8_t *header_bytes;
	void *arena;
	vmf_internal::payload_state *payload;
	vmf_internal::decoder_phase phase;
	vmf_error error;
	vmf_envelope_event envelope;
	const vmf_message_schema *schema;
	size_t header_size;
	size_t header_needed;
	size_t descriptor;
	uint64_t completed_body_octets;
	uint64_t events;
	bool header_only;
};

struct vmf_encoder {
	vmf_internal::configuration config;
	void *storage;
	uint8_t *header_bytes;
	void *arena;
	vmf_internal::payload_state *payload;
	vmf_internal::encoder_phase phase;
	vmf_error error;
	vmf_envelope_event envelope;
	const vmf_message_schema *schema;
	vmf_measurement measurements[16];
	bool measured[16];
	size_t header_size;
	size_t header_written;
	size_t descriptor;
	uint64_t completed_body_octets;
	uint64_t bits;
	uint64_t events;
	uint64_t begin_length;
	bool counting;
	bool header_only;
	bool header_pending;
	bool field_pending;
	vmf_event field_event;
	bool event_pending;
	vmf_event pending_event;
};

#endif
