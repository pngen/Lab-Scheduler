#include <algorithm>
#include <vector>

#include "lab_scheduler/protocol.hpp"
#include "lab_scheduler/cluster.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

using namespace lab_scheduler;

namespace {

std::vector<std::byte> frame_bytes(MessageType type, std::uint64_t correlation,
                                   const std::vector<std::byte>& payload) {
    Result<std::vector<std::byte>> frame = encode_frame(type, correlation, 0, payload);
    LS_CHECK(frame.ok());
    return frame.ok() ? frame.value() : std::vector<std::byte>{};
}

}  // namespace

LS_TEST(frames_round_trip_and_validate) {
    const std::vector<std::byte> payload{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
    const std::vector<std::byte> frame = frame_bytes(MessageType::Inspect, 42, payload);
    LS_CHECK_EQ(frame.size(), std::size_t{kFrameHeaderSize} + payload.size());
    const Result<FrameView> decoded = decode_frame(frame, Limits{});
    LS_CHECK(decoded.ok());
    LS_CHECK_EQ(decoded.value().type, MessageType::Inspect);
    LS_CHECK_EQ(decoded.value().correlation_id, std::uint64_t{42});
    LS_CHECK_EQ(decoded.value().payload.size(), payload.size());
}

LS_TEST(malformed_frames_are_rejected) {
    const std::vector<std::byte> payload{std::byte{0x09}};
    const std::vector<std::byte> good = frame_bytes(MessageType::Inspect, 1, payload);

    std::vector<std::byte> unknown_type = good;
    unknown_type[8] = static_cast<std::byte>(0xff);
    unknown_type[9] = static_cast<std::byte>(0x7f);
    LS_CHECK(!decode_frame(unknown_type, Limits{}).ok());

    std::vector<std::byte> zero_type = good;
    zero_type[8] = std::byte{0x00};
    zero_type[9] = std::byte{0x00};
    LS_CHECK(!decode_frame(zero_type, Limits{}).ok());

    std::vector<std::byte> bad_flags = good;
    bad_flags[10] = std::byte{0x01};
    LS_CHECK(!decode_frame(bad_flags, Limits{}).ok());

    std::vector<std::byte> bad_version = good;
    bad_version[4] = std::byte{0x09};
    LS_CHECK(!decode_frame(bad_version, Limits{}).ok());

    std::vector<std::byte> bad_length = good;
    bad_length[0] = static_cast<std::byte>(std::to_integer<std::uint8_t>(bad_length[0]) + 1);
    LS_CHECK(!decode_frame(bad_length, Limits{}).ok());

    std::vector<std::byte> bad_payload_length = good;
    bad_payload_length[28] = static_cast<std::byte>(std::to_integer<std::uint8_t>(bad_payload_length[28]) + 1);
    LS_CHECK(!decode_frame(bad_payload_length, Limits{}).ok());

    std::vector<std::byte> bad_checksum = good;
    bad_checksum[36] = static_cast<std::byte>(std::to_integer<std::uint8_t>(bad_checksum[36]) ^ 0xff);
    LS_CHECK(!decode_frame(bad_checksum, Limits{}).ok());

    std::vector<std::byte> truncated(good.begin(), good.end() - 1);
    LS_CHECK(!decode_frame(truncated, Limits{}).ok());

    Limits small;
    small.max_frame_size = 16;
    LS_CHECK(!decode_frame(good, small).ok());
}

LS_TEST(message_payloads_round_trip) {
    const Limits limits;

    HelloMessage hello;
    hello.worker = WorkerId::from_value(0x61);
    hello.boot = WorkerBootId::from_value(0x62);
    hello.authority = "worker";
    Result<std::vector<std::byte>> encoded = encode_hello(hello, limits);
    LS_CHECK(encoded.ok());
    Result<HelloMessage> decoded = decode_hello(encoded.value(), limits);
    LS_CHECK(decoded.ok());
    LS_CHECK_EQ(decoded.value().worker, hello.worker);
    LS_CHECK_EQ(decoded.value().authority, hello.authority);

    ScheduleMessage schedule;
    schedule.request = test::request_of(0xe001, 0xf001, {test::requirement_of(ResourceClass::Gpu, "gpu")});
    encoded = encode_schedule(schedule, limits);
    LS_CHECK(encoded.ok());
    Result<ScheduleMessage> decoded_schedule = decode_schedule(encoded.value(), limits);
    LS_CHECK(decoded_schedule.ok());
    LS_CHECK_EQ(decoded_schedule.value().request.id, schedule.request.id);
    LS_CHECK_EQ(decoded_schedule.value().request.requirements.size(), std::size_t{1});

    CompletionClaim claim;
    claim.placement = PlacementId::from_value(0x71);
    claim.placement_generation = PlacementGeneration::from_value(1);
    claim.experiment = ExperimentId::from_value(0x72);
    claim.experiment_generation = ExperimentGeneration::from_value(1);
    claim.epoch = CoordinatorEpoch::from_value(1);
    claim.worker_boot = WorkerBootId::from_value(0x73);
    claim.resources.push_back(ResourceId::from_value(0x74));
    claim.resource_generations.push_back(ResourceGeneration::from_value(2));
    claim.detail = "done";
    encoded = encode_completion(claim, limits);
    LS_CHECK(encoded.ok());
    Result<CompletionClaim> decoded_claim = decode_completion(encoded.value(), limits);
    LS_CHECK(decoded_claim.ok());
    LS_CHECK_EQ(decoded_claim.value().placement, claim.placement);
    LS_CHECK_EQ(decoded_claim.value().resources.size(), std::size_t{1});
    LS_CHECK_EQ(decoded_claim.value().resource_generations[0], claim.resource_generations[0]);
}

LS_TEST(payload_decoders_reject_trailing_and_truncated_bytes) {
    const Limits limits;
    HelloMessage hello;
    hello.worker = WorkerId::from_value(0x81);
    hello.boot = WorkerBootId::from_value(0x82);
    Result<std::vector<std::byte>> encoded = encode_hello(hello, limits);
    LS_CHECK(encoded.ok());

    std::vector<std::byte> trailing = encoded.value();
    trailing.push_back(std::byte{0x00});
    LS_CHECK_EQ(decode_hello(trailing, limits).status().code, ErrorCode::ProtocolError);

    std::vector<std::byte> truncated(encoded.value().begin(), encoded.value().end() - 3);
    LS_CHECK(!decode_hello(truncated, limits).ok());

    std::vector<std::byte> empty;
    LS_CHECK(!decode_hello(empty, limits).ok());
}

LS_TEST(message_type_names_are_stable) {
    for (std::uint16_t value = 1; value <= 26; ++value) {
        const MessageType type = static_cast<MessageType>(value);
        const std::string_view name = message_type_name(type);
        LS_CHECK(name != std::string_view("UNKNOWN"));
        const std::optional<MessageType> parsed = parse_message_type(name);
        LS_CHECK(parsed.has_value());
        LS_CHECK_EQ(*parsed, type);
    }
    LS_CHECK_EQ(message_type_name(static_cast<MessageType>(999)), std::string_view("UNKNOWN"));
}
