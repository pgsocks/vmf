#include <gtest/gtest.h>
#include <vmf/author.h>
#include "managed_consumer.h"
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" int vmf_author_c_consumer(void);
int vmf_author_cpp_consumer();

namespace
{
struct author_fixture {
	managed_consumer source;
	vmf_author_field fields[4] = {};
	vmf_repeat_occurrence paths[4][2] = {};
	vmf_author_result *result = nullptr;
	vmf_author_error error = {};
	vmf_author_options options = {};

	author_fixture()
	{
		managed_consumer_initialize(&source);
		options = vmf_author_options_for_schemas(&source.set);
		for (size_t i = 0; i < 4; ++i) {
			paths[i][0] = {10, i / 2};
			paths[i][1] = {20, i % 2};
			fields[i].address = {0, &source.schema, &source.index,
				{}, paths[i], 2};
			fields[i].kind = VMF_FIELD_UNSIGNED;
			fields[i].value.unsigned_integer.value =
				source.rows[i / 2][i % 2];
		}
	}
	~author_fixture()
	{
		vmf_author_result_destroy(result);
	}
	vmf_status encode(size_t count = 4,
		const vmf_author_control *controls = nullptr,
		size_t control_count = 0)
	{
		return vmf_author_encode(&options, &source.envelope, fields,
			count, controls, control_count, &result, &error);
	}
};

TEST(Author, PublicC11AndProceduralCppConsumers)
{
	EXPECT_EQ(1, vmf_author_c_consumer());
	EXPECT_EQ(1, vmf_author_cpp_consumer());
}

TEST(Author, DerivesSizesWithoutChangingBorrowedHeader)
{
	author_fixture f;

	f.source.header.fields.header_size_octets = {};
	f.source.message.message_size_octets = {};
	ASSERT_EQ(VMF_OK, f.encode());
	EXPECT_FALSE(f.source.header.fields.header_size_octets.present);
	EXPECT_FALSE(f.source.message.message_size_octets.present);
	vmf_measurement measured = {};

	ASSERT_EQ(VMF_OK,
		vmf_author_result_measurement(f.result, 0, &measured));
	EXPECT_EQ(4u, measured.body_octets);
	EXPECT_EQ(measured.pdu_octets, vmf_author_result_size(f.result));
	EXPECT_EQ(1u, vmf_author_result_descriptor_count(f.result));
	for (size_t i = 0; i < 4; ++i)
		EXPECT_TRUE(vmf_author_result_field_active(f.result, i));
	EXPECT_FALSE(vmf_author_result_field_active(f.result, 4));
	EXPECT_EQ(VMF_INVALID_ARGUMENT,
		vmf_author_result_measurement(f.result, 1, &measured));
	EXPECT_EQ(0u, measured.pdu_octets);
}

TEST(Author, OneByteSlicesMatchNormalOutputAndIndependentReplay)
{
	author_fixture f;

	ASSERT_EQ(VMF_OK, f.encode());
	std::vector<uint8_t> normal(vmf_author_result_data(f.result),
		vmf_author_result_data(f.result) +
			vmf_author_result_size(f.result));

	vmf_author_result_destroy(f.result);
	f.result = nullptr;
	f.options.output_slice_bytes = 1;
	ASSERT_EQ(VMF_OK, f.encode());
	EXPECT_EQ(normal, std::vector<uint8_t>(
		vmf_author_result_data(f.result),
		vmf_author_result_data(f.result) +
			vmf_author_result_size(f.result)));
	vmf_decoder *reader = nullptr;

	ASSERT_EQ(VMF_OK,
		vmf_decoder_create(&f.options.codec, &reader, nullptr));
	ASSERT_EQ(VMF_OK, vmf_decoder_begin_pdu(reader));
	vmf_input input = {normal.data(), normal.size(), 0, true};
	std::vector<uint64_t> values;
	vmf_status status = VMF_OK;

	while (status == VMF_OK) {
		vmf_event event = {};

		status = vmf_decoder_next(reader, &input, &event);
		if (status == VMF_OK && event.kind == VMF_EVENT_FIELD)
			values.push_back(event.as.field.value
				.unsigned_integer.value);
	}
	EXPECT_EQ(VMF_DONE, status);
	EXPECT_EQ((std::vector<uint64_t>{0, 7, 11, 255}), values);
	vmf_decoder_destroy(reader);
}

TEST(Author, MissingMandatoryFieldReportsCanonicalOccurrence)
{
	author_fixture f;

	EXPECT_EQ(VMF_LENGTH_REQUIRED, f.encode(3));
	EXPECT_EQ(nullptr, f.result);
	EXPECT_EQ(SIZE_MAX, f.error.entry_index);
	EXPECT_EQ(&f.source.index, f.error.index);
	ASSERT_EQ(2u, f.error.path_count);
	EXPECT_EQ(10u, f.error.path[0].id);
	EXPECT_EQ(1u, f.error.path[0].occurrence);
	EXPECT_EQ(20u, f.error.path[1].id);
	EXPECT_EQ(1u, f.error.path[1].occurrence);
}

TEST(Author, RejectsIncompleteOrOutOfBoundsFullOccurrencePaths)
{
	author_fixture f;

	f.fields[0].address.path_count = 1;
	EXPECT_EQ(VMF_INVALID_ARGUMENT,
		vmf_author_validate_address(&f.source.set,
			&f.fields[0].address));
	f.fields[0].address.path_count = 2;
	f.paths[0][1].occurrence = 2;
	EXPECT_EQ(VMF_OUT_OF_RANGE, f.encode());
	EXPECT_EQ(0u, f.error.entry_index);
	EXPECT_EQ(nullptr, f.result);
}

TEST(Author, RejectsDuplicateLogicalAddresses)
{
	author_fixture f;

	f.paths[1][1].occurrence = 0;
	EXPECT_EQ(VMF_INVALID_ARGUMENT, f.encode());
	EXPECT_EQ(1u, f.error.entry_index);
}

TEST(Author, DormantValuesAreValidButInvalidDormantConstantsFail)
{
	author_fixture f;
	uint32_t addresses[] = {1, 2};
	vmf_index_schema indexes[2] = {};
	vmf_code_range one = {1, 1};
	vmf_condition_term term = {{addresses, 1}, {}, &one, 1, false};
	vmf_condition when = {VMF_CONDITION_ALL, &term, 1, false};
	vmf_index_refinements refinements = {};

	refinements.when = &when;
	refinements.constant = {true, 7};
	indexes[0].index_number = {addresses, 1};
	indexes[0].number_of_bits = 8;
	indexes[1].index_number = {addresses + 1, 1};
	indexes[1].number_of_bits = 8;
	indexes[1].refinements = &refinements;
	f.source.schema.indexes = indexes;
	f.source.schema.index_count = 2;
	f.source.schema.repeats = nullptr;
	f.source.schema.repeat_count = 0;
	for (size_t i = 0; i < 2; ++i) {
		f.fields[i].address.index = &indexes[i];
		f.fields[i].address.path_count = 0;
	}
	f.fields[1].value.unsigned_integer.value = 7;
	ASSERT_EQ(VMF_OK, f.encode(2));
	EXPECT_TRUE(vmf_author_result_field_active(f.result, 0));
	EXPECT_FALSE(vmf_author_result_field_active(f.result, 1));
	vmf_author_result_destroy(f.result);
	f.result = nullptr;
	f.fields[1].value.unsigned_integer.value = 8;
	EXPECT_EQ(VMF_OUT_OF_RANGE, f.encode(2));
	EXPECT_EQ(1u, f.error.entry_index);
}

TEST(Author, ValidatesCompleteScalarRepresentationsAndAllowedRawCodes)
{
	author_fixture f;
	vmf_index_refinements refinements = {};
	vmf_code_range allowed = {0xf9, 0xf9};

	f.source.index.encode_type = VMF_ENCODE_SIGNED;
	refinements.representation = VMF_INTEGER_ONES_COMPLEMENT;
	refinements.allowed = &allowed;
	refinements.allowed_count = 1;
	f.source.index.refinements = &refinements;
	f.fields[0].kind = VMF_FIELD_SIGNED;
	f.fields[0].value.signed_integer.value = -6;
	uint64_t units = 99;

	EXPECT_EQ(VMF_OK, vmf_author_validate_field(&f.source.set,
		&f.fields[0], &units));
	EXPECT_EQ(1u, units);
	f.fields[0].value.signed_integer.value = -128;
	EXPECT_EQ(VMF_OUT_OF_RANGE, vmf_author_validate_field(&f.source.set,
		&f.fields[0], &units));
	EXPECT_EQ(0u, units);
	f.fields[0].kind = VMF_FIELD_UNSIGNED;
	EXPECT_EQ(VMF_TYPE_MISMATCH, vmf_author_validate_field(&f.source.set,
		&f.fields[0], nullptr));
}

TEST(Author, CompleteTextValidatesUtf8WireUnitsAndTargetEncoding)
{
	author_fixture f;
	vmf_index_refinements refinements = {};

	f.source.index.encode_type = VMF_ENCODE_TEXT;
	f.source.index.number_of_bits = 8;
	refinements.text_encoding = VMF_TEXT_LATIN_1;
	f.source.index.refinements = &refinements;
	f.fields[0].kind = VMF_FIELD_TEXT;
	f.fields[0].value.text = {"\xc3\xa9", 2, true};
	uint64_t units = 0;

	EXPECT_EQ(VMF_OK, vmf_author_validate_field(&f.source.set,
		&f.fields[0], &units));
	EXPECT_EQ(1u, units);
	refinements.text_encoding = VMF_TEXT_ASCII_8;
	EXPECT_EQ(VMF_OUT_OF_RANGE, vmf_author_validate_field(&f.source.set,
		&f.fields[0], &units));
	f.fields[0].value.text = {"\xc0\xaf", 2, true};
	EXPECT_EQ(VMF_MALFORMED, vmf_author_validate_field(&f.source.set,
		&f.fields[0], &units));
	f.fields[0].value.text = {"a", 1, false};
	EXPECT_EQ(VMF_INVALID_ARGUMENT, vmf_author_validate_field(&f.source.set,
		&f.fields[0], &units));
}

TEST(Author, CompleteBitsRejectUnusedHighBits)
{
	author_fixture f;
	uint8_t bytes[] = {0xff, 1};

	f.source.index.encode_type = VMF_ENCODE_BITS;
	f.source.index.number_of_bits = 9;
	f.fields[0].kind = VMF_FIELD_BITS;
	f.fields[0].value.bits = {bytes, 9, true};
	uint64_t units = 0;

	EXPECT_EQ(VMF_OK, vmf_author_validate_field(&f.source.set,
		&f.fields[0], &units));
	EXPECT_EQ(9u, units);
	bytes[1] = 3;
	EXPECT_EQ(VMF_OUT_OF_RANGE, vmf_author_validate_field(&f.source.set,
		&f.fields[0], &units));
}

TEST(Author, LongCompleteTextSurvivesOutputExhaustion)
{
	author_fixture f;
	std::string text(17000, 'x');

	f.source.index.repeat_code = {};
	f.source.schema.repeats = nullptr;
	f.source.schema.repeat_count = 0;
	f.source.index.encode_type = VMF_ENCODE_TEXT;
	f.source.index.number_of_bits = text.size() * 7;
	f.fields[0].address.path_count = 0;
	f.fields[0].kind = VMF_FIELD_TEXT;
	f.fields[0].value.text = {text.data(), text.size(), true};
	f.options.output_slice_bytes = 7;
	ASSERT_EQ(VMF_OK, f.encode(1));
	vmf_measurement measurement = {};

	ASSERT_EQ(VMF_OK,
		vmf_author_result_measurement(f.result, 0, &measurement));
	EXPECT_EQ((text.size() * 7 + 7) / 8, measurement.body_octets);
	EXPECT_EQ(measurement.pdu_octets, vmf_author_result_size(f.result));
	EXPECT_EQ(std::string(17000, 'x'), text);
}

TEST(Author, RejectsSecurityMaterialBeforeAuthoring)
{
	author_fixture f;

	f.source.message.security.authentication_a = {true, nullptr, 0};
	EXPECT_EQ(VMF_UNSUPPORTED_ENVELOPE_FEATURE, f.encode());
	EXPECT_EQ(nullptr, f.result);
}

struct allocation_tracker {
	size_t calls = 0;
	size_t live = 0;
	size_t fail_at = SIZE_MAX;
};
void *tracked_allocate(void *user, size_t size)
{
	auto *tracker = static_cast<allocation_tracker *>(user);

	if (tracker->calls++ == tracker->fail_at)
		return nullptr;
	void *block = std::malloc(size);

	if (block)
		++tracker->live;
	return block;
}
void tracked_deallocate(void *user, void *block)
{
	--static_cast<allocation_tracker *>(user)->live;
	std::free(block);
}

TEST(Author, AllocationFailuresCleanEveryPartialAllocation)
{
	size_t successful_calls = 0;
	{
		author_fixture f;
		allocation_tracker tracker;
		vmf_allocator allocator = {tracked_allocate,
			tracked_deallocate, &tracker};

		f.options.codec.allocator = &allocator;
		ASSERT_EQ(VMF_OK, f.encode());
		successful_calls = tracker.calls;
		vmf_author_result_destroy(f.result);
		f.result = nullptr;
		EXPECT_EQ(0u, tracker.live);
	}
	for (size_t fail_at = 0; fail_at <= successful_calls; ++fail_at) {
		author_fixture f;
		allocation_tracker tracker;

		tracker.fail_at = fail_at;
		vmf_allocator allocator = {tracked_allocate,
			tracked_deallocate, &tracker};

		f.options.codec.allocator = &allocator;
		auto status = f.encode();

		EXPECT_TRUE(status == VMF_OK || status == VMF_OUT_OF_MEMORY);
		vmf_author_result_destroy(f.result);
		f.result = nullptr;
		EXPECT_EQ(0u, tracker.live) << "failure allocation " << fail_at;
		EXPECT_EQ(fail_at == successful_calls ? VMF_OK :
			VMF_OUT_OF_MEMORY, status);
	}
}

TEST(Author, EnforcesMemoryBudgetAndInputEntryLimit)
{
	author_fixture f;

	f.options.max_working_bytes = 1;
	EXPECT_EQ(VMF_LIMIT_EXCEEDED, f.encode());
	EXPECT_EQ(nullptr, f.result);
	f.options.max_working_bytes = 64 * 1024 * 1024;
	f.options.max_entries = 3;
	EXPECT_EQ(VMF_LIMIT_EXCEEDED, f.encode());
}

TEST(Author, OptionalGroupInferenceHonorsMixedNestingAndOccurrencePrefix)
{
	author_fixture f;
	uint32_t addresses[] = {1, 2, 3};
	vmf_group_id groups[] = {42, 43};
	vmf_index_schema indexes[3] = {};

	indexes[0].index_number = {addresses, 1};
	indexes[0].indicator = VMF_INDICATOR_GPI;
	indexes[0].target = {VMF_TARGET_GROUP, {}};
	indexes[0].target.as.group = 42;
	indexes[1].index_number = {addresses + 1, 1};
	indexes[1].indicator = VMF_INDICATOR_GPI;
	indexes[1].target = {VMF_TARGET_GROUP, {}};
	indexes[1].target.as.group = 43;
	indexes[1].group_code = {groups, 1};
	indexes[1].repeat_code = {f.source.membership, 1};
	indexes[2].index_number = {addresses + 2, 1};
	indexes[2].number_of_bits = 8;
	indexes[2].group_code = {groups, 2};
	indexes[2].repeat_code = {f.source.membership, 2};
	f.source.schema.indexes = indexes;
	f.source.schema.index_count = 3;
	f.fields[0].address.index = &indexes[2];
	f.fields[1].address.index = &indexes[2];
	f.paths[0][0].occurrence = 1;
	f.paths[1][0].occurrence = 1;
	vmf_scope_info info = {};

	ASSERT_EQ(VMF_OK, vmf_schema_scope_info(&f.source.set,
		&f.source.schema, {VMF_SCOPE_GROUP, 43}, &info));
	EXPECT_EQ(&indexes[1], info.controller);
	ASSERT_EQ(1u, info.ancestry.count);
	EXPECT_EQ(10u, info.ancestry.parts[0].id);
	ASSERT_EQ(VMF_OK, f.encode(2));
	EXPECT_TRUE(vmf_author_result_field_active(f.result, 0));
	EXPECT_TRUE(vmf_author_result_field_active(f.result, 1));
	vmf_author_result_destroy(f.result);
	f.result = nullptr;
	vmf_author_control omit = {};

	omit.address = {0, &f.source.schema, nullptr,
		{VMF_SCOPE_GROUP, 42}, nullptr, 0};
	omit.kind = VMF_ENCODE_REQUEST_PRESENCE;
	omit.as.present = false;
	ASSERT_EQ(VMF_OK, f.encode(2, &omit, 1));
	EXPECT_FALSE(vmf_author_result_field_active(f.result, 0));
	EXPECT_FALSE(vmf_author_result_field_active(f.result, 1));
	EXPECT_TRUE(vmf_author_result_control_active(f.result, 0));
}

TEST(Author, IndicatorCountsAndDormantOccurrenceBoundsAreStatic)
{
	author_fixture f;
	uint32_t addresses[] = {1, 2};
	vmf_index_schema indexes[2] = {};
	vmf_repeat_component controller = {10, {true, 4}};
	vmf_repeat_component member = {10, {}};

	indexes[0].index_number = {addresses, 1};
	indexes[0].indicator = VMF_INDICATOR_FRI;
	indexes[0].repeat_code = {&controller, 1};
	indexes[1].index_number = {addresses + 1, 1};
	indexes[1].number_of_bits = 8;
	indexes[1].repeat_code = {&member, 1};
	f.source.schema.indexes = indexes;
	f.source.schema.index_count = 2;
	f.source.schema.repeats = nullptr;
	f.source.schema.repeat_count = 0;
	for (size_t i = 0; i < 3; ++i) {
		f.fields[i].address.index = &indexes[1];
		f.fields[i].address.path_count = 1;
		f.paths[i][0].occurrence = i;
	}
	vmf_author_control count = {};

	count.address = {0, &f.source.schema, nullptr,
		{VMF_SCOPE_REPEAT, 10}, nullptr, 0};
	count.kind = VMF_ENCODE_REQUEST_REPEAT_COUNT;
	count.as.repeat_count = 2;
	ASSERT_EQ(VMF_OK,
		vmf_author_validate_control(&f.source.set, &count));
	ASSERT_EQ(VMF_OK, f.encode(3, &count, 1));
	EXPECT_TRUE(vmf_author_result_field_active(f.result, 0));
	EXPECT_TRUE(vmf_author_result_field_active(f.result, 1));
	EXPECT_FALSE(vmf_author_result_field_active(f.result, 2));
	EXPECT_TRUE(vmf_author_result_control_active(f.result, 0));
	vmf_author_result_destroy(f.result);
	f.result = nullptr;
	count.as.repeat_count = 5;
	EXPECT_EQ(VMF_OUT_OF_RANGE,
		vmf_author_validate_control(&f.source.set, &count));
	EXPECT_EQ(VMF_OUT_OF_RANGE, f.encode(3, &count, 1));
	EXPECT_TRUE(f.error.entry_is_control);
	EXPECT_EQ(0u, f.error.entry_index);
	count.as.repeat_count = 0;
	EXPECT_EQ(VMF_OUT_OF_RANGE,
		vmf_author_validate_control(&f.source.set, &count));
	count.as.repeat_count = VMF_UNITS_UNKNOWN;
	EXPECT_EQ(VMF_LENGTH_REQUIRED,
		vmf_author_validate_control(&f.source.set, &count));
}

TEST(Author, RejectsStructuralAssertionsForFixedRepeats)
{
	author_fixture f;
	vmf_author_control count = {};

	count.address = {0, &f.source.schema, nullptr,
		{VMF_SCOPE_REPEAT, 10}, nullptr, 0};
	count.kind = VMF_ENCODE_REQUEST_REPEAT_COUNT;
	count.as.repeat_count = 2;
	EXPECT_EQ(VMF_UNSUPPORTED_SCHEMA_FEATURE,
		vmf_author_validate_control(&f.source.set, &count));
	EXPECT_EQ(VMF_UNSUPPORTED_SCHEMA_FEATURE, f.encode(4, &count, 1));
}

TEST(Author, ExplicitDynamicLengthsRemainRequiredAndContradictionsFail)
{
	author_fixture f;
	uint32_t addresses[] = {1, 2};
	vmf_index_schema indexes[2] = {};
	vmf_index_refinements refinements = {};
	uint8_t bytes[] = {0xff, 0x12};

	indexes[0].index_number = {addresses, 1};
	indexes[0].number_of_bits = 8;
	indexes[1].index_number = {addresses + 1, 1};
	indexes[1].number_of_bits = 32;
	indexes[1].encode_type = VMF_ENCODE_OCTETS;
	refinements.length_from = indexes[0].index_number;
	indexes[1].refinements = &refinements;
	f.source.schema.indexes = indexes;
	f.source.schema.index_count = 2;
	f.source.schema.repeats = nullptr;
	f.source.schema.repeat_count = 0;
	for (size_t i = 0; i < 2; ++i) {
		f.fields[i].address.index = &indexes[i];
		f.fields[i].address.path_count = 0;
	}
	f.fields[0].value.unsigned_integer.value = 2;
	f.fields[1].kind = VMF_FIELD_OCTETS;
	f.fields[1].value.octets = {bytes, sizeof(bytes), true};
	ASSERT_EQ(VMF_OK, f.encode(2));
	vmf_author_result_destroy(f.result);
	f.result = nullptr;
	f.fields[0].value.unsigned_integer.value = 3;
	EXPECT_EQ(VMF_OUT_OF_RANGE, f.encode(2));
	EXPECT_EQ(nullptr, f.result);
	EXPECT_EQ(&indexes[1], f.error.index);
	EXPECT_EQ(1u, f.error.entry_index);
}

TEST(Author, HeaderValidationRejectsWrongProfileAndUnknownIdentity)
{
	author_fixture f;

	ASSERT_EQ(VMF_OK, vmf_author_validate_header(&f.options,
		&f.source.envelope, &f.error));
	f.source.envelope.wire_version = VMF_ENVELOPE_C;
	EXPECT_EQ(VMF_INVALID_ARGUMENT, vmf_author_validate_header(&f.options,
		&f.source.envelope, &f.error));
	f.source.envelope.wire_version = VMF_ENVELOPE_D1;
	f.source.message.id.message_number = 12;
	EXPECT_EQ(VMF_UNKNOWN_MESSAGE, vmf_author_validate_header(&f.options,
		&f.source.envelope, &f.error));
	EXPECT_EQ(0u, f.error.descriptor_index);
}

TEST(Author, AuthorsAllFourEnvelopeProfiles)
{
	for (auto profile : {VMF_ENVELOPE_B, VMF_ENVELOPE_C,
		VMF_ENVELOPE_D, VMF_ENVELOPE_D1}) {
		SCOPED_TRACE(profile);
		author_fixture f;
		vmf_header header = {};
		vmf_header_fields *fields = nullptr;

		f.source.set.envelope = profile;
		if (profile == VMF_ENVELOPE_B)
			f.source.set.standard = {};
		ASSERT_EQ(VMF_OK, vmf_schema_message_id(&f.source.set,
			&f.source.schema, &f.source.message.id));
		f.source.message.precedence = profile <= VMF_ENVELOPE_C ?
			2 : 0;
		switch (profile) {
		case VMF_ENVELOPE_B:
			fields = &header.b.fields;
			break;
		case VMF_ENVELOPE_C:
			fields = &header.c.fields;
			break;
		case VMF_ENVELOPE_D:
			fields = &header.d.fields;
			break;
		case VMF_ENVELOPE_D1:
			fields = &header.d1.fields;
			break;
		}
		fields->messages = &f.source.message;
		fields->message_count = 1;
		f.source.envelope = {static_cast<uint8_t>(profile),
			&header, vmf_header_layout(profile).size,
			VMF_LENGTH_UNKNOWN, VMF_LENGTH_UNKNOWN};
		f.options = vmf_author_options_for_schemas(&f.source.set);
		ASSERT_EQ(VMF_OK, f.encode());
		vmf_measurement measurement = {};

		ASSERT_EQ(VMF_OK, vmf_author_result_measurement(f.result,
			0, &measurement));
		EXPECT_EQ(4u, measurement.body_octets);
	}
}

} /* namespace */
