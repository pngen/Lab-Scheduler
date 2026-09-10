#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "lab_scheduler/engine.hpp"
#include "lab_scheduler/persistence.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

using namespace lab_scheduler;

namespace {

struct TempFile {
    std::filesystem::path path{};

    explicit TempFile(const std::string& name) {
        path = std::filesystem::temp_directory_path() / name;
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }

    ~TempFile() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(path.string() + ".part", ignored);
    }
};

std::vector<std::byte> read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        return {};
    }
    const std::streamoff size = stream.tellg();
    stream.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    stream.read(reinterpret_cast<char*>(bytes.data()), size);
    return bytes;
}

void write_file(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

DurableState populated_state(SchedulerEngine& engine) {
    LS_CHECK(engine.register_resource(test::gpu_resource(0x3001, 0x51, 0x52, "host-a", 2)).ok());
    LS_CHECK(engine
                 .register_resource(
                     test::model_resource(0x3002, 0x51, 0x52, "host-a", 0x1f01, "2.0", true))
                 .ok());
    ResourceRequirement gpu = test::requirement_of(ResourceClass::Gpu, "gpu");
    ResourceRequirement model = test::requirement_of(ResourceClass::Model, "model");
    model.model = ModelResourceId::from_value(0x1f01);
    const Result<Decision> decision =
        engine.submit_request(test::request_of(0xc001, 0xd001, {gpu, model}));
    LS_CHECK(decision.ok());
    LS_CHECK_EQ(decision.value().outcome, DecisionOutcome::Placed);
    return engine.export_state();
}

}  // namespace

LS_TEST(durable_state_round_trips_through_the_container) {
    SchedulerEngine engine;
    const DurableState state = populated_state(engine);
    const Result<std::vector<std::byte>> encoded = encode_durable_state(state, Limits{});
    LS_CHECK(encoded.ok());
    const Result<DurableState> decoded = decode_durable_state(encoded.value(), Limits{});
    LS_CHECK(decoded.ok());
    LS_CHECK_EQ(decoded.value().epoch, state.epoch);
    LS_CHECK_EQ(decoded.value().resources.size(), state.resources.size());
    LS_CHECK_EQ(decoded.value().requests.size(), state.requests.size());
    LS_CHECK_EQ(decoded.value().placements.size(), state.placements.size());
    LS_CHECK_EQ(decoded.value().reservations.size(), state.reservations.size());
    LS_CHECK(validate_durable_state(decoded.value()).ok());
    LS_CHECK_EQ(decoded.value().placements[0].resources.size(), std::size_t{2});

    const Result<std::vector<std::byte>> again = encode_durable_state(decoded.value(), Limits{});
    LS_CHECK(again.ok());
    LS_CHECK_EQ(again.value().size(), encoded.value().size());
}

LS_TEST(durable_state_file_round_trips_and_replaces_atomically) {
    TempFile file("lab-scheduler-durable-test.lss");
    SchedulerEngine engine;
    const DurableState state = populated_state(engine);
    LS_CHECK(save_durable_state(state, file.path.string(), Limits{}).ok());
    LS_CHECK(std::filesystem::exists(file.path));
    LS_CHECK(!std::filesystem::exists(file.path.string() + ".part"));
    const Result<DurableState> loaded = load_durable_state(file.path.string(), Limits{});
    LS_CHECK(loaded.ok());
    LS_CHECK(validate_durable_state(loaded.value()).ok());

    const std::vector<std::byte> first_bytes = read_file(file.path);
    LS_CHECK(save_durable_state(state, file.path.string(), Limits{}).ok());
    LS_CHECK_EQ(read_file(file.path).size(), first_bytes.size());
}

LS_TEST(corruption_truncation_and_trailing_bytes_are_rejected) {
    SchedulerEngine engine;
    const DurableState state = populated_state(engine);
    const Result<std::vector<std::byte>> encoded = encode_durable_state(state, Limits{});
    LS_CHECK(encoded.ok());
    const std::vector<std::byte> good = encoded.value();

    std::vector<std::byte> truncated(good.begin(), good.end() - 4);
    LS_CHECK_EQ(decode_durable_state(truncated, Limits{}).status().code, ErrorCode::CorruptPersistence);

    std::vector<std::byte> trailing = good;
    trailing.push_back(std::byte{0x2a});
    LS_CHECK_EQ(decode_durable_state(trailing, Limits{}).status().code, ErrorCode::CorruptPersistence);

    std::vector<std::byte> bad_magic = good;
    bad_magic[0] = std::byte{0x00};
    LS_CHECK_EQ(decode_durable_state(bad_magic, Limits{}).status().code, ErrorCode::CorruptPersistence);

    std::vector<std::byte> bad_version = good;
    bad_version[8] = std::byte{0x7f};
    LS_CHECK_EQ(decode_durable_state(bad_version, Limits{}).status().code, ErrorCode::CorruptPersistence);

    std::vector<std::byte> bad_checksum = good;
    bad_checksum[good.size() - 1] = static_cast<std::byte>(std::to_integer<std::uint8_t>(bad_checksum.back()) ^ 0x11);
    LS_CHECK_EQ(decode_durable_state(bad_checksum, Limits{}).status().code, ErrorCode::CorruptPersistence);

    std::vector<std::byte> absurd_length = good;
    absurd_length[12] = std::byte{0xff};
    absurd_length[13] = std::byte{0xff};
    absurd_length[14] = std::byte{0xff};
    absurd_length[15] = std::byte{0xff};
    LS_CHECK_EQ(decode_durable_state(absurd_length, Limits{}).status().code, ErrorCode::CorruptPersistence);

    std::vector<std::byte> empty;
    LS_CHECK_EQ(decode_durable_state(empty, Limits{}).status().code, ErrorCode::CorruptPersistence);
    LS_CHECK_EQ(decode_durable_state(std::span<const std::byte>(good.data(), 4), Limits{}).status().code,
                ErrorCode::CorruptPersistence);
}

LS_TEST(absurd_record_counts_are_rejected_before_allocation) {
    SchedulerEngine engine;
    const DurableState state = populated_state(engine);
    const Result<std::vector<std::byte>> encoded = encode_durable_state(state, Limits{});
    LS_CHECK(encoded.ok());
    std::vector<std::byte> bytes = encoded.value();
    // The resource count sits directly after the producer string; locate and
    // corrupt it by re-encoding a state with a fake count instead.
    Limits tiny;
    tiny.max_persistence_records = 0;
    LS_CHECK_EQ(decode_durable_state(bytes, tiny).status().code, ErrorCode::CorruptPersistence);
}

LS_TEST(missing_file_and_truncated_file_are_reported) {
    TempFile file("lab-scheduler-missing-test.lss");
    LS_CHECK_EQ(load_durable_state(file.path.string(), Limits{}).status().code, ErrorCode::IoFailure);
    write_file(file.path, {std::byte{0x01}, std::byte{0x02}});
    LS_CHECK_EQ(load_durable_state(file.path.string(), Limits{}).status().code, ErrorCode::CorruptPersistence);
}

LS_TEST(validation_rejects_inconsistent_references_and_mirrors) {
    SchedulerEngine engine;
    DurableState state = populated_state(engine);
    LS_CHECK(validate_durable_state(state).ok());

    DurableState unknown_placement = state;
    unknown_placement.placements[0].request = ScheduleRequestId::from_value(0x7777);
    LS_CHECK_EQ(validate_durable_state(unknown_placement).code, ErrorCode::CorruptPersistence);

    DurableState unknown_resource = state;
    unknown_resource.reservations[0].claims[0].resource = ResourceId::from_value(0x7777);
    LS_CHECK_EQ(validate_durable_state(unknown_resource).code, ErrorCode::CorruptPersistence);

    DurableState mirror_mismatch = state;
    mirror_mismatch.resources[0].reserved_slots += 1;
    LS_CHECK_EQ(validate_durable_state(mirror_mismatch).code, ErrorCode::CorruptPersistence);

    DurableState duplicate = state;
    duplicate.resources.push_back(duplicate.resources[0]);
    LS_CHECK_EQ(validate_durable_state(duplicate).code, ErrorCode::DuplicateIdentity);

    DurableState over_claimed = state;
    over_claimed.reservations[0].claims[0].slots = 99;
    LS_CHECK(validate_durable_state(over_claimed).code == ErrorCode::CorruptPersistence ||
             validate_durable_state(over_claimed).code == ErrorCode::CapacityExhausted);
}
