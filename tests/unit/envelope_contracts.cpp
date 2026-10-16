#include <gtest/gtest.h>
#include "../../src/envelope.hpp"
#include "../acceptance/vectors.h"
#include <cstring>
#include <vector>

static vmf_header_fields *envelope_fields(
	vmf_header *header, vmf_envelope_version profile)
{
	switch (profile) {
	case VMF_ENVELOPE_B:
		return &header->b.fields;
	case VMF_ENVELOPE_C:
		return &header->c.fields;
	case VMF_ENVELOPE_D:
		return &header->d.fields;
	case VMF_ENVELOPE_D1:
		return &header->d1.fields;
	}
	return nullptr;
}

TEST(EnvelopeCodec, IndependentLiteralProfilesRoundTripAndAllPrefixes)
{
	const uint8_t *vectors[] = {vector_b, vector_c, vector_d, vector_d1};
	const size_t lengths[] = {8, 11, 13, 13};
	auto limits = vmf_default_limits();
	std::vector<uint8_t> arena(limits.max_header_bytes);
	uint8_t output[256] = {};

	for (unsigned i = 0; i < 4; ++i) {
		auto profile = (vmf_envelope_version)(i + 1);
		vmf_envelope_event event = {};
		vmf_error error = {};
		size_t needed = 0;

		ASSERT_EQ(VMF_OK, vmf_internal::parse_header(profile,
					  vectors[i], lengths[i] + 1, true,
					  arena.data(), arena.size(), &limits,
					  false, &event, &needed, &error));
		EXPECT_EQ(lengths[i], event.header_octets);
		EXPECT_EQ(lengths[i] + 1, event.pdu_octets);
		size_t size = 0;

		ASSERT_EQ(VMF_OK, vmf_internal::serialize_header(profile,
					  &event, false, output, sizeof(output),
					  &size, &limits, &error));
		ASSERT_EQ(lengths[i], size);
		EXPECT_EQ(0, std::memcmp(output, vectors[i], size));
		for (size_t prefix = 0; prefix < lengths[i]; ++prefix) {
			EXPECT_EQ(VMF_NEED_INPUT,
				vmf_internal::parse_header(profile, vectors[i],
					prefix, false, arena.data(),
					arena.size(), &limits, false, &event,
					&needed, &error));
			EXPECT_GT(needed, prefix);
			EXPECT_LE(needed, lengths[i]);
			EXPECT_EQ(nullptr, event.header);
			EXPECT_EQ(VMF_TRUNCATED,
				vmf_internal::parse_header(profile, vectors[i],
					prefix, true, arena.data(),
					arena.size(), &limits, false, &event,
					&needed, &error));
		}
	}
}

TEST(EnvelopeCodec, PublicComparisonCHeaderHasTwentyFourOctets)
{
	/* DFDLSchemas b1a4891, test_2045_C_msghdr1. Body is intentionally
	 * absent; this independently published fixture checks its envelope.
	 */
	const uint8_t bytes[] = {0xe2, 0x9b, 0x61, 0x09, 0x65, 0x31, 0x51, 0x20,
		0x06, 0x00, 0x79, 0x25, 0x40, 0x00, 0x00, 0x08, 0x51, 0xa2,
		0x8c, 0xc2, 0x41, 0x0f, 0x10, 0x00};
	auto limits = vmf_default_limits();
	std::vector<uint8_t> arena(limits.max_header_bytes);
	vmf_envelope_event event = {};
	vmf_error error = {};
	size_t needed = 0;

	ASSERT_EQ(
		VMF_OK, vmf_internal::parse_header(VMF_ENVELOPE_C, bytes,
				sizeof(bytes), true, arena.data(), arena.size(),
				&limits, false, &event, &needed, &error));
	EXPECT_EQ(24u, event.header_octets);
	const auto &fields =
		static_cast<const vmf_header_c *>(event.header)->fields;

	EXPECT_EQ(1229623u, fields.originator.urn.value);
	ASSERT_EQ(1u, fields.recipient_count);
	EXPECT_EQ(665132u, fields.recipients[0].urn.value);
	ASSERT_EQ(1u, fields.message_count);
	const auto &message = fields.messages[0];

	EXPECT_EQ(2u, message.id.user_message_format);
	EXPECT_EQ(7u, message.id.standard_version.value);
	EXPECT_EQ(2u, message.id.functional_area);
	EXPECT_EQ(1u, message.id.message_number);
	EXPECT_EQ(4u, message.precedence);
	EXPECT_EQ(18u, message.originator_dtg.year);
	EXPECT_EQ(30u, message.originator_dtg.extension.value);
	uint8_t output[24] = {};
	size_t size = 0;

	ASSERT_EQ(VMF_OK,
		vmf_internal::serialize_header(VMF_ENVELOPE_C, &event, false,
			output, sizeof(output), &size, &limits, &error));
	EXPECT_EQ(0, std::memcmp(bytes, output, sizeof(bytes)));
}

TEST(EnvelopeCodec, EveryProfileCarriesItsFullOptionalGrammar)
{
	auto limits = vmf_default_limits();
	std::vector<uint8_t> arena(limits.max_header_bytes);
	std::vector<uint8_t> encoded(limits.max_header_bytes);
	std::vector<uint8_t> replay(limits.max_header_bytes);
	char name[64];
	char marking[32];

	std::memset(name, 'N', sizeof(name));
	std::memset(marking, 'R', sizeof(marking));
	uint8_t key[8] = {};
	uint8_t initialization[128] = {};
	uint8_t token_data[2048] = {};
	uint8_t authentication[1024] = {};
	uint8_t padding[255] = {};
	vmf_header_bytes tokens[] = {{true, token_data, sizeof(token_data)},
		{true, token_data, sizeof(token_data)}};
	uint16_t country[] = {0, 1, 511};

	for (unsigned revision = 1; revision <= 4; ++revision) {
		auto profile = (vmf_envelope_version)revision;
		vmf_header header = {};
		auto *fields = envelope_fields(&header, profile);
		vmf_address recipients[2] = {};
		vmf_address information = {};
		vmf_message_header message = {};
		vmf_reference references[4] = {};

		fields->originator = {true, {true, 1}, {}};
		recipients[0] = {true, {}, {true, {name, sizeof(name)}}};
		recipients[1] = {true, {true, 2}, {}};
		information = {true, {true, 3}, {}};
		fields->recipients = recipients;
		fields->recipient_count = 2;
		fields->information_addressees = &information;
		fields->information_addressee_count = 1;
		fields->header_size_octets = {revision != 1, 0};
		fields->messages = &message;
		fields->message_count = 1;
		message.id.user_message_format = 2;
		message.id.standard_version = {
			revision != 1, (uint8_t)(revision == 2 ? 6 : 5)};
		message.id.identification_present = true;
		message.id.functional_area = 2;
		message.id.message_number = 1;
		message.id.subtype = {true, 3};
		message.file_name = {true, {name, sizeof(name)}};
		message.message_size_octets = {true, 3};
		message.operation = 2;
		message.retransmit = true;
		message.precedence = revision < 3 ? 7 : 5;
		message.classification = 3;
		message.has_body = true;
		message.originator_dtg = {true, 26, 10, 15, 12, 30,
			(uint8_t)(revision < 3 ? 59 : 63), {true, 4095}};
		message.perishability = message.originator_dtg;
		message.perishability.extension = {};
		message.acknowledgment = {true, true, false, false};
		if (revision == 1)
			message.marking_b = {true, 'U' | ('S' << 7)};
		else if (revision == 2)
			message.marking_c = {true, {marking, sizeof(marking)}};
		else {
			message.markings = country;
			message.marking_count = 3;
		}
		for (auto &reference : references) {
			reference.originator = {true, {true, 4}, {}};
			reference.dtg = message.originator_dtg;
			if (revision == 1) {
				reference.functional_area = 2;
				reference.message_number = 9;
			}
		}
		message.references = references;
		message.reference_count = 4;
		if (revision != 1) {
			message.security.present = true;
			message.security.spi = 1;
			message.security.keying_material = {
				true, key, sizeof(key)};
			message.security.initialization = {
				true, initialization, sizeof(initialization)};
			message.security.tokens_present = true;
			message.security.tokens = tokens;
			message.security.token_count = 2;
			message.security.authentication_a = {
				true, authentication, sizeof(authentication)};
			message.security.signed_ack_request = true;
			message.security.padding_present = true;
			message.security.padding_length = 255;
			message.security.padding = {
				true, padding, sizeof(padding)};
		}
		vmf_envelope_event event = {(uint8_t)profile, &header,
			vmf_header_layout(profile).size, 0, VMF_LENGTH_UNKNOWN};
		vmf_error error = {};
		size_t size = 0;

		ASSERT_EQ(
			VMF_OK, vmf_internal::serialize_header(profile, &event,
					true, encoded.data(), encoded.size(),
					&size, &limits, &error))
			<< revision << ": " << error.offset;
		fields->header_size_octets.value = (uint32_t)size;
		ASSERT_EQ(
			VMF_OK, vmf_internal::serialize_header(profile, &event,
					false, encoded.data(), encoded.size(),
					&size, &limits, &error));
		vmf_envelope_event decoded = {};
		size_t needed = 0;

		ASSERT_EQ(VMF_OK,
			vmf_internal::parse_header(profile, encoded.data(),
				size, true, arena.data(), arena.size(), &limits,
				false, &decoded, &needed, &error));
		const auto *copy =
			static_cast<const vmf_header *>(decoded.header);
		const auto *decoded_fields = envelope_fields(
			const_cast<vmf_header *>(copy), profile);

		EXPECT_EQ(2u, decoded_fields->recipient_count);
		EXPECT_EQ(64u, decoded_fields->recipients[0].name.value.size);
		EXPECT_EQ(4u, decoded_fields->messages[0].reference_count);
		if (revision != 1) {
			EXPECT_EQ(2u, decoded_fields->messages[0]
					      .security.token_count);
			EXPECT_NE(token_data, decoded_fields->messages[0]
						      .security.tokens[0]
						      .data);
		}
		size_t replay_size = 0;

		ASSERT_EQ(VMF_OK,
			vmf_internal::serialize_header(profile, &decoded, false,
				replay.data(), replay.size(), &replay_size,
				&limits, &error));
		EXPECT_EQ(size, replay_size);
		EXPECT_EQ(0, std::memcmp(encoded.data(), replay.data(), size));
		if (revision != 1) {
			bool requested_span = false;

			for (size_t prefix = 0; prefix < size;) {
				ASSERT_EQ(VMF_NEED_INPUT,
					vmf_internal::parse_header(profile,
						encoded.data(), prefix, false,
						arena.data(), arena.size(),
						&limits, false, &decoded,
						&needed, &error));
				if (needed > prefix + 64)
					requested_span = true;
				ASSERT_GT(needed, prefix);
				prefix = needed;
			}
			EXPECT_TRUE(requested_span);
		}
	}
}

TEST(EnvelopeCodec, ShortenedResponseUsesOnlyItsDefinedFields)
{
	const uint8_t bytes[] = {0xef, 0, 0, 0, 0x15, 0, 0, 0};
	auto limits = vmf_default_limits();
	std::vector<uint8_t> arena(limits.max_header_bytes);
	vmf_envelope_event event = {};
	vmf_error error = {};
	size_t needed = 0;

	for (unsigned revision = 1; revision <= 4; ++revision) {
		auto profile = (vmf_envelope_version)revision;
		auto status = vmf_internal::parse_header(profile, bytes,
			sizeof(bytes), true, arena.data(), arena.size(),
			&limits, false, &event, &needed, &error);

		if (revision == 1) {
			EXPECT_EQ(VMF_UNSUPPORTED_ENVELOPE, status);
			continue;
		}
		ASSERT_EQ(VMF_OK, status);
		EXPECT_EQ(8u, event.header_octets);
		EXPECT_EQ(8u, event.pdu_octets);
		uint8_t output[8] = {};
		size_t size = 0;

		ASSERT_EQ(VMF_OK, vmf_internal::serialize_header(profile,
					  &event, false, output, sizeof(output),
					  &size, &limits, &error));
		EXPECT_EQ(0, std::memcmp(bytes, output, sizeof(bytes)));
	}
}

TEST(EnvelopeCodec, RejectsLimitsPaddingProfileAndIncorrectHeaderSize)
{
	auto limits = vmf_default_limits();
	std::vector<uint8_t> arena(limits.max_header_bytes);
	vmf_envelope_event event = {};
	vmf_error error = {};
	size_t needed = 0;
	uint8_t bytes[sizeof(vector_d1)];

	std::memcpy(bytes, vector_d1, sizeof(bytes));
	EXPECT_EQ(VMF_UNSUPPORTED_ENVELOPE,
		vmf_internal::parse_header(VMF_ENVELOPE_C, bytes, sizeof(bytes),
			true, arena.data(), arena.size(), &limits, false,
			&event, &needed, &error));
	bytes[12] |= 0x80;
	EXPECT_EQ(VMF_MALFORMED,
		vmf_internal::parse_header(VMF_ENVELOPE_D1, bytes,
			sizeof(bytes), true, arena.data(), arena.size(),
			&limits, false, &event, &needed, &error));
	std::memcpy(bytes, vector_d1, sizeof(bytes));
	bytes[1] ^= 2;
	EXPECT_EQ(VMF_MALFORMED,
		vmf_internal::parse_header(VMF_ENVELOPE_D1, bytes,
			sizeof(bytes), true, arena.data(), arena.size(),
			&limits, false, &event, &needed, &error));
	EXPECT_EQ(
		VMF_OK, vmf_internal::parse_header(VMF_ENVELOPE_D1, bytes,
				sizeof(bytes), true, arena.data(), arena.size(),
				&limits, true, &event, &needed, &error));
	EXPECT_EQ(VMF_LIMIT_EXCEEDED,
		vmf_internal::parse_header(VMF_ENVELOPE_D1, vector_d1,
			sizeof(vector_d1), true, arena.data(), 8, &limits,
			false, &event, &needed, &error));
	limits.max_header_items = 0;
	EXPECT_EQ(VMF_LIMIT_EXCEEDED,
		vmf_internal::parse_header(VMF_ENVELOPE_D1, vector_d1,
			sizeof(vector_d1), true, arena.data(), arena.size(),
			&limits, false, &event, &needed, &error));
}

TEST(EnvelopeCodec, AcknowledgmentsHaveAddressesAndRevisionSpecificResponses)
{
	auto limits = vmf_default_limits();
	std::vector<uint8_t> arena(limits.max_header_bytes);
	std::vector<uint8_t> bytes(limits.max_header_bytes);
	uint8_t signature[40] = {};

	for (unsigned revision = 1; revision <= 4; ++revision) {
		auto profile = (vmf_envelope_version)revision;
		vmf_header header = {};
		auto *fields = envelope_fields(&header, profile);
		vmf_address recipient = {true, {true, 2}, {}};
		vmf_message_header message = {};

		fields->originator = {true, {true, 1}, {}};
		fields->recipients = &recipient;
		fields->recipient_count = 1;
		fields->messages = &message;
		fields->message_count = 1;
		message.precedence = revision < 3 ? 7 : 0;
		message.response.present = true;
		message.response.acknowledged = {
			true, 26, 10, 15, 12, 30, 59, {true, 7}};
		message.response.receipt_compliance = 1;
		vmf_envelope_event event = {(uint8_t)profile, &header,
			vmf_header_layout(profile).size, 0, VMF_LENGTH_UNKNOWN};
		vmf_error error = {};
		size_t size = 0;
		size_t needed = 0;
		vmf_envelope_event decoded = {};

		ASSERT_EQ(
			VMF_OK, vmf_internal::serialize_header(profile, &event,
					false, bytes.data(), bytes.size(),
					&size, &limits, &error));
		ASSERT_EQ(VMF_OK,
			vmf_internal::parse_header(profile, bytes.data(), size,
				true, arena.data(), arena.size(), &limits,
				false, &decoded, &needed, &error));
		EXPECT_EQ(size, decoded.pdu_octets);
		auto *copy =
			envelope_fields((vmf_header *)decoded.header, profile);

		EXPECT_FALSE(copy->messages[0].has_body);
		message.response.acknowledged.second = 63;
		EXPECT_EQ(revision < 3 ? VMF_OUT_OF_RANGE : VMF_OK,
			vmf_internal::serialize_header(profile, &event, false,
				bytes.data(), bytes.size(), &size, &limits,
				&error));
		message.response.acknowledged.second = 59;
		message.response.amplification = {true, {"ACK", 3}};
		EXPECT_EQ(revision == 1 ? VMF_OUT_OF_RANGE : VMF_OK,
			vmf_internal::serialize_header(profile, &event, false,
				bytes.data(), bytes.size(), &size, &limits,
				&error));
		message.response.amplification = {};
		fields->originator.present = false;
		EXPECT_EQ(VMF_OUT_OF_RANGE,
			vmf_internal::serialize_header(profile, &event, false,
				bytes.data(), bytes.size(), &size, &limits,
				&error));
		fields->originator.present = true;
		if (revision == 1)
			continue;
		message.security.present = true;
		message.security.authentication_a = {
			true, signature, sizeof(signature)};
		message.security.authentication_b = {
			true, signature, sizeof(signature)};
		ASSERT_EQ(
			VMF_OK, vmf_internal::serialize_header(profile, &event,
					false, bytes.data(), bytes.size(),
					&size, &limits, &error));
		ASSERT_EQ(VMF_OK,
			vmf_internal::parse_header(profile, bytes.data(), size,
				true, arena.data(), arena.size(), &limits,
				false, &decoded, &needed, &error));
		message.security.authentication_a.size = 8;
		EXPECT_EQ(VMF_OUT_OF_RANGE,
			vmf_internal::serialize_header(profile, &event, false,
				bytes.data(), bytes.size(), &size, &limits,
				&error));
		message.security.authentication_a.size = sizeof(signature);
		message.security.signed_ack_request = true;
		EXPECT_EQ(VMF_OUT_OF_RANGE,
			vmf_internal::serialize_header(profile, &event, false,
				bytes.data(), bytes.size(), &size, &limits,
				&error));
	}
}

TEST(EnvelopeCodec, RepeatedDescriptorsRequireAllLengthsAndPreserveIndexes)
{
	auto limits = vmf_default_limits();
	std::vector<uint8_t> arena(limits.max_header_bytes);
	uint8_t bytes[256] = {};

	for (unsigned revision = 1; revision <= 4; ++revision) {
		auto profile = (vmf_envelope_version)revision;
		vmf_header header = {};
		auto *fields = envelope_fields(&header, profile);
		vmf_message_header messages[2] = {};

		fields->messages = messages;
		fields->message_count = 2;
		fields->header_size_octets = {revision != 1, 0};
		for (auto &message : messages) {
			message.precedence = revision < 3 ? 7 : 0;
			message.message_size_octets = {true, 3};
			message.has_body = true;
		}
		vmf_envelope_event event = {(uint8_t)profile, &header,
			vmf_header_layout(profile).size, 0, VMF_LENGTH_UNKNOWN};
		vmf_error error = {};
		size_t size = 0;
		size_t needed = 0;
		vmf_envelope_event decoded = {};

		ASSERT_EQ(VMF_OK,
			vmf_internal::serialize_header(profile, &event, true,
				bytes, sizeof(bytes), &size, &limits, &error));
		fields->header_size_octets.value = (uint32_t)size;
		ASSERT_EQ(VMF_OK,
			vmf_internal::serialize_header(profile, &event, false,
				bytes, sizeof(bytes), &size, &limits, &error));
		ASSERT_EQ(VMF_OK,
			vmf_internal::parse_header(profile, bytes, size, true,
				arena.data(), arena.size(), &limits, false,
				&decoded, &needed, &error));
		EXPECT_EQ(size + 6, decoded.pdu_octets);
		messages[1].message_size_octets.present = false;
		EXPECT_EQ(VMF_OUT_OF_RANGE,
			vmf_internal::serialize_header(profile, &event, false,
				bytes, sizeof(bytes), &size, &limits, &error));
		EXPECT_EQ(1u, error.message_index);
		EXPECT_EQ(0u, size);
	}
}
