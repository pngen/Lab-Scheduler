#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "lab_scheduler/engine.hpp"
#include "lab_scheduler/error.hpp"
#include "lab_scheduler/limits.hpp"
#include "lab_scheduler/placement.hpp"
#include "lab_scheduler/request.hpp"
#include "lab_scheduler/resource.hpp"

namespace lab_scheduler {

// Framed loopback transport. Every frame carries:
//   frame_length      u32   bytes after this field
//   protocol_version  u32   must equal kProtocolVersion
//   message_type      u16   must be a known type
//   flags             u16   must be zero
//   correlation_id    u64
//   coordinator_epoch u64
//   payload_length    u32   must equal frame_length - 32
//   checksum          u32   low 32 bits of FNV-1a 64 over the header fields
//                           after the length field followed by the payload
//                           (the checksum field itself is excluded)
//   payload           payload_length bytes
//
// A frame is rejected when it is truncated, oversized, carries an unknown
// message type, carries non-zero flags, carries a payload length that does not
// match, fails its checksum, or leaves trailing bytes in the payload.
enum class MessageType : std::uint16_t {
    Hello = 1,
    HelloAck = 2,
    RegisterResource = 3,
    PublishState = 4,
    RetireResource = 5,
    ResourceAck = 6,
    ScheduleRequest = 7,
    Decision = 8,
    Assign = 9,
    AssignAck = 10,
    Running = 11,
    Completion = 12,
    CompletionAck = 13,
    CancelPlacement = 14,
    CancelRequest = 15,
    Reassign = 16,
    Preempt = 17,
    LifecycleAck = 18,
    Inspect = 19,
    InspectReply = 20,
    SaveState = 21,
    StateAck = 22,
    Shutdown = 23,
    ShutdownAck = 24,
    Error = 25,
    DisconnectWorker = 26,
};

std::string_view message_type_name(MessageType type) noexcept;
std::optional<MessageType> parse_message_type(std::string_view text) noexcept;

inline constexpr std::uint32_t kFrameHeaderSize = 36;
inline constexpr std::uint32_t kFrameHeaderFields = 32;
inline constexpr std::uint16_t kFrameFlagsNone = 0;

struct FrameView {
    MessageType type = MessageType::Hello;
    std::uint64_t correlation_id = 0;
    std::uint64_t coordinator_epoch = 0;
    std::span<const std::byte> payload{};
};

Result<std::vector<std::byte>> encode_frame(MessageType type, std::uint64_t correlation_id,
                                            std::uint64_t coordinator_epoch,
                                            std::span<const std::byte> payload);
Result<FrameView> decode_frame(std::span<const std::byte> frame, const Limits& limits);

// --- message payloads -------------------------------------------------------
struct HelloMessage {
    WorkerId worker{};
    WorkerBootId boot{};
    std::string authority{};
};

struct HelloAckMessage {
    CoordinatorEpoch epoch{};
    std::string coordinator{};
};

struct ResourceMessage {
    ResourceAdvertisement advertisement{};
};

struct RetireMessage {
    ResourceId resource{};
    ResourceGeneration generation{};
};

struct ResourceAckMessage {
    ResourceId resource{};
    ResourceGeneration generation{};
    ResourceLifecycle lifecycle = ResourceLifecycle::Registered;
    ErrorCode code = ErrorCode::Ok;
    std::string detail{};
};

struct ScheduleMessage {
    ScheduleRequest request{};
};

struct DecisionMessage {
    DecisionId decision{};
    DecisionOutcome outcome = DecisionOutcome::NoPlacement;
    ErrorCode failure = ErrorCode::Ok;
    std::string failure_detail{};
    PlacementRecord placement{};
    AuthorityEnvelope authority{};
    std::string explanation{};
};

struct AssignMessage {
    AuthorityEnvelope authority{};
    std::string workload{};
};

struct AuthorityMessage {
    PlacementId placement{};
    PlacementGeneration generation{};
    CoordinatorEpoch epoch{};
    WorkerBootId worker_boot{};
};

struct CompletionAckMessage {
    PlacementId placement{};
    PlacementState state = PlacementState::Reserved;
    bool accepted = false;
    bool duplicate = false;
    ErrorCode code = ErrorCode::Ok;
    std::string detail{};
    std::uint32_t released_reservations = 0;
};

struct CancelPlacementMessage {
    PlacementId placement{};
    PlacementGeneration generation{};
    CoordinatorEpoch epoch{};
    std::string reason{};
};

struct CancelRequestMessage {
    ScheduleRequestId request{};
    std::string reason{};
};

struct LifecycleAckMessage {
    PlacementId placement{};
    PlacementGeneration generation{};
    PlacementState state = PlacementState::Reserved;
    ErrorCode code = ErrorCode::Ok;
    std::string detail{};
    std::uint32_t released_reservations = 0;
    std::uint32_t affected = 0;
};

struct InspectMessage {
    std::string subject{};
};

struct InspectReplyMessage {
    std::string subject{};
    std::string text{};
};

struct ErrorMessage {
    ErrorCode code = ErrorCode::Ok;
    std::string message{};
};

struct SaveStateMessage {
    std::string path{};
};

struct StateAckMessage {
    std::string path{};
    CoordinatorEpoch epoch{};
    std::uint32_t resources = 0;
    std::uint32_t requests = 0;
    std::uint32_t placements = 0;
    std::uint32_t reservations = 0;
    ErrorCode code = ErrorCode::Ok;
    std::string detail{};
};

struct DisconnectWorkerMessage {
    WorkerId worker{};
    WorkerBootId boot{};
    std::string detail{};
};

Result<std::vector<std::byte>> encode_hello(const HelloMessage& message, const Limits& limits);
Result<HelloMessage> decode_hello(std::span<const std::byte> payload, const Limits& limits);
Result<std::vector<std::byte>> encode_hello_ack(const HelloAckMessage& message, const Limits& limits);
Result<HelloAckMessage> decode_hello_ack(std::span<const std::byte> payload, const Limits& limits);

Result<std::vector<std::byte>> encode_resource_message(const ResourceMessage& message, const Limits& limits);
Result<ResourceMessage> decode_resource_message(std::span<const std::byte> payload, const Limits& limits);
Result<std::vector<std::byte>> encode_retire(const RetireMessage& message, const Limits& limits);
Result<RetireMessage> decode_retire(std::span<const std::byte> payload, const Limits& limits);
Result<std::vector<std::byte>> encode_resource_ack(const ResourceAckMessage& message, const Limits& limits);
Result<ResourceAckMessage> decode_resource_ack(std::span<const std::byte> payload, const Limits& limits);

Result<std::vector<std::byte>> encode_schedule(const ScheduleMessage& message, const Limits& limits);
Result<ScheduleMessage> decode_schedule(std::span<const std::byte> payload, const Limits& limits);
Result<std::vector<std::byte>> encode_decision(const DecisionMessage& message, const Limits& limits);
Result<DecisionMessage> decode_decision(std::span<const std::byte> payload, const Limits& limits);

Result<std::vector<std::byte>> encode_assign(const AssignMessage& message, const Limits& limits);
Result<AssignMessage> decode_assign(std::span<const std::byte> payload, const Limits& limits);
Result<std::vector<std::byte>> encode_authority(const AuthorityMessage& message, const Limits& limits);
Result<AuthorityMessage> decode_authority(std::span<const std::byte> payload, const Limits& limits);

Result<std::vector<std::byte>> encode_completion(const CompletionClaim& claim, const Limits& limits);
Result<CompletionClaim> decode_completion(std::span<const std::byte> payload, const Limits& limits);
Result<std::vector<std::byte>> encode_completion_ack(const CompletionAckMessage& message, const Limits& limits);
Result<CompletionAckMessage> decode_completion_ack(std::span<const std::byte> payload, const Limits& limits);

Result<std::vector<std::byte>> encode_cancel_placement(const CancelPlacementMessage& message,
                                                       const Limits& limits);
Result<CancelPlacementMessage> decode_cancel_placement(std::span<const std::byte> payload, const Limits& limits);
Result<std::vector<std::byte>> encode_cancel_request(const CancelRequestMessage& message, const Limits& limits);
Result<CancelRequestMessage> decode_cancel_request(std::span<const std::byte> payload, const Limits& limits);
Result<std::vector<std::byte>> encode_lifecycle_ack(const LifecycleAckMessage& message, const Limits& limits);
Result<LifecycleAckMessage> decode_lifecycle_ack(std::span<const std::byte> payload, const Limits& limits);

Result<std::vector<std::byte>> encode_inspect(const InspectMessage& message, const Limits& limits);
Result<InspectMessage> decode_inspect(std::span<const std::byte> payload, const Limits& limits);
Result<std::vector<std::byte>> encode_inspect_reply(const InspectReplyMessage& message, const Limits& limits);
Result<InspectReplyMessage> decode_inspect_reply(std::span<const std::byte> payload, const Limits& limits);

Result<std::vector<std::byte>> encode_error_message(const ErrorMessage& message, const Limits& limits);
Result<ErrorMessage> decode_error_message(std::span<const std::byte> payload, const Limits& limits);
Result<std::vector<std::byte>> encode_save_state(const SaveStateMessage& message, const Limits& limits);
Result<SaveStateMessage> decode_save_state(std::span<const std::byte> payload, const Limits& limits);
Result<std::vector<std::byte>> encode_state_ack(const StateAckMessage& message, const Limits& limits);
Result<StateAckMessage> decode_state_ack(std::span<const std::byte> payload, const Limits& limits);
Result<std::vector<std::byte>> encode_disconnect_worker(const DisconnectWorkerMessage& message,
                                                        const Limits& limits);
Result<DisconnectWorkerMessage> decode_disconnect_worker(std::span<const std::byte> payload,
                                                         const Limits& limits);

}  // namespace lab_scheduler
