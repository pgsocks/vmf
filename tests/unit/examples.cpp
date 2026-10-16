#include <gtest/gtest.h>
#include "fixtures.hpp"
#include <string>

TEST(PublicExamples, PublicCHeaderMatchesIndependentExpectedFields)
{
	auto config = fixtures::options(VMF_ENVELOPE_C);
	vmf_decoder *reader = nullptr;
	vmf_encoder *writer = nullptr;

	fixtures::require(
		VMF_OK, vmf_decoder_create(&config, &reader, nullptr));
	fixtures::require(
		VMF_OK, vmf_encoder_create(&config, false, &writer, nullptr));
	fixtures::require(VMF_OK, vmf_decoder_begin_envelope(reader));
	fixtures::require(VMF_OK, vmf_encoder_begin_envelope(writer));
	vmf_input input = {fixtures::public_c_header,
		sizeof(fixtures::public_c_header), 0, true};
	vmf_event event = {};

	fixtures::require(VMF_OK, vmf_decoder_next(reader, &input, &event));
	EXPECT_EQ(VMF_EVENT_ENVELOPE, event.kind);
	EXPECT_EQ(24u, event.as.envelope.header_octets);
	const auto *header =
		static_cast<const vmf_header_c *>(event.as.envelope.header);
	const auto &fields = header->fields;

	EXPECT_FALSE(fields.compression.present);
	EXPECT_TRUE(fields.originator.present);
	EXPECT_TRUE(fields.originator.urn.present);
	EXPECT_FALSE(fields.originator.name.present);
	EXPECT_EQ(1229623u, fields.originator.urn.value);
	ASSERT_EQ(1u, fields.recipient_count);
	EXPECT_TRUE(fields.recipients[0].present);
	EXPECT_TRUE(fields.recipients[0].urn.present);
	EXPECT_FALSE(fields.recipients[0].name.present);
	EXPECT_EQ(665132u, fields.recipients[0].urn.value);
	EXPECT_EQ(0u, fields.information_addressee_count);
	EXPECT_TRUE(fields.header_size_octets.present);
	EXPECT_EQ(24u, fields.header_size_octets.value);
	ASSERT_EQ(1u, fields.message_count);
	const auto &message = fields.messages[0];

	EXPECT_EQ(2u, message.id.user_message_format);
	EXPECT_TRUE(message.id.standard_version.present);
	EXPECT_EQ(7u, message.id.standard_version.value);
	EXPECT_TRUE(message.id.identification_present);
	EXPECT_EQ(2u, message.id.functional_area);
	EXPECT_EQ(1u, message.id.message_number);
	EXPECT_FALSE(message.id.subtype.present);
	EXPECT_FALSE(message.file_name.present);
	EXPECT_TRUE(message.message_size_octets.present);
	EXPECT_EQ(0u, message.message_size_octets.value);
	EXPECT_EQ(1u, message.operation);
	EXPECT_FALSE(message.retransmit);
	EXPECT_EQ(4u, message.precedence);
	EXPECT_EQ(0u, message.classification);
	EXPECT_FALSE(message.marking_c.present);
	EXPECT_TRUE(message.originator_dtg.present);
	EXPECT_FALSE(message.perishability.present);
	EXPECT_EQ(0u, message.reference_count);
	EXPECT_EQ(18u, message.originator_dtg.year);
	EXPECT_EQ(10u, message.originator_dtg.month);
	EXPECT_EQ(12u, message.originator_dtg.day);
	EXPECT_EQ(20u, message.originator_dtg.hour);
	EXPECT_EQ(48u, message.originator_dtg.minute);
	EXPECT_EQ(1u, message.originator_dtg.second);
	EXPECT_TRUE(message.originator_dtg.extension.present);
	EXPECT_EQ(30u, message.originator_dtg.extension.value);
	EXPECT_TRUE(message.acknowledgment.present);
	EXPECT_FALSE(message.acknowledgment.machine);
	EXPECT_FALSE(message.acknowledgment.operator_ack);
	EXPECT_FALSE(message.acknowledgment.operator_reply);
	EXPECT_FALSE(message.response.present);
	EXPECT_FALSE(message.security.present);
	std::vector<uint8_t> encoded;

	fixtures::write(writer, &event, &encoded);
	EXPECT_EQ(std::vector<uint8_t>(fixtures::public_c_header,
			  fixtures::public_c_header +
				  sizeof(fixtures::public_c_header)),
		encoded);
	fixtures::require(VMF_DONE, vmf_decoder_next(reader, &input, &event));
	EXPECT_EQ(sizeof(fixtures::public_c_header), input.used);
	vmf_encoder_destroy(writer);
	vmf_decoder_destroy(reader);
}

static void check_example(bool d1, uint8_t second, char expected)
{
	fixtures::example_schema schema;

	fixtures::initialize(&schema, d1);
	auto profile = d1 ? VMF_ENVELOPE_D1 : VMF_ENVELOPE_C;
	auto config = fixtures::options(profile, &schema.set);
	std::vector<uint8_t> body =
		d1 ? std::vector<uint8_t>{0xa0, second, 0x50, 0x9a, 0x00}
		   : std::vector<uint8_t>{0xc8, 0xa0, 0xf0, 0x03, 0x00};
	auto bytes = fixtures::packet(profile, body);
	vmf_decoder *reader = nullptr;
	vmf_encoder *writer = nullptr;

	fixtures::require(
		VMF_OK, vmf_decoder_create(&config, &reader, nullptr));
	fixtures::require(
		VMF_OK, vmf_encoder_create(&config, false, &writer, nullptr));
	fixtures::require(VMF_OK, vmf_decoder_begin_pdu(reader));
	vmf_input input = {bytes.data(), bytes.size(), 0, true};
	std::vector<uint8_t> encoded;
	std::string characters;
	std::vector<uint64_t> integers;
	size_t absent = 0;
	vmf_status status = VMF_OK;

	for (size_t i = 0; i < 100; ++i) {
		vmf_event event = {};

		status = vmf_decoder_next(reader, &input, &event);
		if (status == VMF_DONE)
			break;
		fixtures::require(VMF_OK, status);
		if (event.kind == VMF_EVENT_ABSENT)
			++absent;
		if (event.kind == VMF_EVENT_FIELD) {
			if (event.node->as.field.kind == VMF_FIELD_TEXT) {
				auto text = event.as.field.value.text;

				characters.append(text.data, text.size);
			} else {
				integers.push_back(event.as.field.value
						.unsigned_integer.value);
			}
		}
		fixtures::write(writer, &event, &encoded);
	}
	EXPECT_EQ(VMF_DONE, status);
	EXPECT_EQ(bytes.size(), input.used);
	EXPECT_EQ(bytes, encoded);
	EXPECT_EQ(4u, absent);
	if (d1) {
		EXPECT_EQ(std::string(1, expected) + "A", characters);
		EXPECT_EQ((std::vector<uint64_t>{0, 1234}), integers);
	} else {
		EXPECT_EQ("AB", characters);
		EXPECT_EQ((std::vector<uint64_t>{0, 1, 31}), integers);
	}
	vmf_encoder_destroy(writer);
	vmf_decoder_destroy(reader);
}

TEST(PublicExamples, PublicCExampleHasTargetAB0031)
{
	check_example(false, 0, 0);
}

TEST(PublicExamples, PublishedD1OctetsDecodeLowercaseB)
{
	check_example(true, 0x78, 'b');
}

TEST(PublicExamples, IntendedD1ValuesEncodeCorrectedOctets)
{
	check_example(true, 0x70, 'B');
}
