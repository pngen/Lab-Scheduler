#include "lab_scheduler/canonical.hpp"
#include "lab_scheduler/codec.hpp"
#include "lab_scheduler/identity.hpp"
#include "lab_scheduler/resource.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

using namespace lab_scheduler;

LS_TEST(identity_round_trip_and_canonical_form) {
    const ResourceId id = ResourceId::from_value(0x1234);
    LS_CHECK_EQ(id.to_string(), std::string("res-0000000000001234"));
    const std::optional<ResourceId> parsed = ResourceId::parse("res-0000000000001234");
    LS_CHECK(parsed.has_value());
    LS_CHECK_EQ(*parsed, id);
    LS_CHECK(ResourceId::parse("res-0000000000001234 ").has_value() == false);
    LS_CHECK(ResourceId::parse("res-000000000000123").has_value() == false);
    LS_CHECK(ResourceId::parse("res-000000000000123G").has_value() == false);
}

LS_TEST(identity_rejects_null_and_wrong_prefix) {
    LS_CHECK(ResourceId{}.is_null());
    LS_CHECK(!ResourceId{}.validate().ok());
    LS_CHECK(!ResourceId::parse("res-0000000000000000").has_value());
    LS_CHECK(!PlacementId::parse("res-0000000000000001").has_value());
    LS_CHECK(!WorkerBootId::parse("wrk-0000000000000001").has_value());
    LS_CHECK(PlacementId::parse("plc-ffffffffffffffff").has_value());
}

LS_TEST(capability_identities_are_content_addressed) {
    const CapabilityId first = capability_id_from_name("cuda");
    const CapabilityId second = capability_id_from_name("cuda");
    const CapabilityId other = capability_id_from_name("rocm");
    LS_CHECK_EQ(first, second);
    LS_CHECK(!(first == other));
    LS_CHECK(!first.is_null());
    LS_CHECK_EQ(capability_id_from_name("CUDA"), first);
}

LS_TEST(capability_names_must_be_canonical) {
    LS_CHECK(is_canonical_capability_name("gpu.memory.24gb"));
    LS_CHECK(!is_canonical_capability_name("GPU"));
    LS_CHECK(!is_canonical_capability_name("gpu memory"));
    LS_CHECK(!is_canonical_capability_name(""));
    LS_CHECK(!validate_capability_name("Cuda").ok());
}

LS_TEST(worker_boot_identity_is_incarnation_specific) {
    const WorkerId worker = WorkerId::from_value(7);
    const WorkerBootId first = make_worker_boot_id(worker, 1, 0xabc);
    const WorkerBootId second = make_worker_boot_id(worker, 1, 0xabd);
    const WorkerBootId third = make_worker_boot_id(worker, 2, 0xabc);
    LS_CHECK(!(first == second));
    LS_CHECK(!(first == third));
    LS_CHECK_EQ(first, make_worker_boot_id(worker, 1, 0xabc));
    LS_CHECK(!first.is_null());
}

LS_TEST(id_generator_never_reuses_values) {
    IdGenerator generator(10);
    const std::uint64_t first = generator.next_value();
    const std::uint64_t second = generator.next_value();
    LS_CHECK(second > first);
    generator.observe(1000);
    LS_CHECK(generator.next_value() > 1000);
}

LS_TEST(canonical_decimal_and_hex_helpers) {
    LS_CHECK_EQ(decimal_u64(0), std::string("0"));
    LS_CHECK_EQ(decimal_u64(123456789), std::string("123456789"));
    LS_CHECK_EQ(decimal_i64(-42), std::string("-42"));
    LS_CHECK_EQ(decimal_i64(-9223372036854775807LL - 1), std::string("-9223372036854775808"));
    LS_CHECK_EQ(hex_u64(0xabc, 8), std::string("00000abc"));
    std::uint64_t value = 0;
    LS_CHECK(parse_decimal_u64("42", value));
    LS_CHECK_EQ(value, std::uint64_t{42});
    LS_CHECK(!parse_decimal_u64("042", value));
    LS_CHECK(!parse_decimal_u64("", value));
    LS_CHECK(!parse_decimal_u64("99999999999999999999", value));
    std::int64_t signed_value = 0;
    LS_CHECK(parse_decimal_i64("-7", signed_value));
    LS_CHECK_EQ(signed_value, std::int64_t{-7});
    LS_CHECK(!parse_decimal_i64("+7", signed_value));
}

LS_TEST(codec_rejects_truncation_and_trailing_bytes) {
    ByteWriter writer;
    writer.u32(7);
    writer.str("hello");
    writer.boolean(true);
    ByteReader reader(writer.span());
    std::uint32_t number = 0;
    std::string text;
    bool flag = false;
    LS_CHECK(reader.u32(number).ok());
    LS_CHECK_EQ(number, std::uint32_t{7});
    LS_CHECK(reader.str(text, 16).ok());
    LS_CHECK_EQ(text, std::string("hello"));
    LS_CHECK(reader.boolean(flag).ok());
    LS_CHECK(flag);
    LS_CHECK(reader.require_end().ok());

    ByteReader short_reader(std::span<const std::byte>(writer.span().data(), 2));
    LS_CHECK(!short_reader.u32(number).ok());

    ByteReader trailing(writer.span());
    LS_CHECK(trailing.str(text, 1).ok() == false);
}

LS_TEST(codec_enforces_bounds_and_canonical_booleans) {
    ByteWriter writer;
    writer.u32(1000);
    ByteReader reader(writer.span());
    std::uint32_t count = 0;
    LS_CHECK(!reader.count(count, 16).ok());

    ByteWriter bools;
    bools.u8(7);
    ByteReader bool_reader(bools.span());
    bool flag = false;
    LS_CHECK(!bool_reader.boolean(flag).ok());

    ByteWriter long_string;
    long_string.str(std::string(50, 'x'));
    ByteReader string_reader(long_string.span());
    std::string text;
    LS_CHECK(!string_reader.str(text, 8).ok());
}

LS_TEST(codec_error_code_is_configurable) {
    ByteWriter writer;
    writer.u32(5);
    ByteReader reader(std::span<const std::byte>(writer.span().data(), 2), ErrorCode::CorruptPersistence);
    std::uint32_t value = 0;
    const Status status = reader.u32(value);
    LS_CHECK(!status.ok());
    LS_CHECK_EQ(status.code, ErrorCode::CorruptPersistence);
}

LS_TEST(resource_validation_rejects_malformed_advertisements) {
    const Limits limits;
    ResourceAdvertisement worker_owned = test::gpu_resource(1, 2, 0, "host-a");
    worker_owned.boot = WorkerBootId{};
    LS_CHECK_EQ(validate_advertisement(worker_owned, limits).code, ErrorCode::InvalidIdentity);

    ResourceAdvertisement over_capacity = test::gpu_resource(1, 2, 3, "host-a", 2);
    over_capacity.current_occupancy = 3;
    LS_CHECK_EQ(validate_advertisement(over_capacity, limits).code, ErrorCode::InvalidArgument);

    ResourceAdvertisement no_accelerator = test::gpu_resource(1, 2, 3, "host-a");
    no_accelerator.accelerators.clear();
    LS_CHECK_EQ(validate_advertisement(no_accelerator, limits).code, ErrorCode::InvalidArgument);

    ResourceAdvertisement no_descriptor;
    no_descriptor.id = ResourceId::from_value(9);
    no_descriptor.resource_class = ResourceClass::Model;
    no_descriptor.name = "model";
    no_descriptor.worker = WorkerId::from_value(2);
    no_descriptor.boot = WorkerBootId::from_value(3);
    LS_CHECK_EQ(validate_advertisement(no_descriptor, limits).code, ErrorCode::InvalidArgument);

    ResourceAdvertisement zero_slots = test::gpu_resource(1, 2, 3, "host-a");
    zero_slots.capacity.slots = 0;
    LS_CHECK_EQ(validate_advertisement(zero_slots, limits).code, ErrorCode::InvalidArgument);

    ResourceAdvertisement bad_capability = test::gpu_resource(1, 2, 3, "host-a");
    bad_capability.capabilities.entries().push_back(Capability{CapabilityId::from_value(7), "CUDA"});
    LS_CHECK_EQ(validate_advertisement(bad_capability, limits).code, ErrorCode::InvalidArgument);
}

LS_TEST(capability_set_rejects_duplicates) {
    const Limits limits;
    CapabilitySet set;
    LS_CHECK(set.add("cuda", limits).ok());
    LS_CHECK_EQ(set.add("cuda", limits).code, ErrorCode::DuplicateIdentity);
    LS_CHECK(set.contains_name("cuda"));
    LS_CHECK(!set.contains_name("rocm"));
    CapabilitySet required;
    LS_CHECK(required.add("cuda", limits).ok());
    LS_CHECK(set.contains_all(required));
}
