#include "lab_scheduler/protocol.hpp"

#include <array>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "lab_scheduler/canonical.hpp"
#include "lab_scheduler/codec.hpp"
#include "lab_scheduler/version.hpp"
#include "record_codec.hpp"

namespace lab_scheduler {
namespace {

struct MessageTypeName {
    MessageType type;
    std::string_view name;
};

constexpr std::array<MessageTypeName, 26> kMessageTypeNames{{
    {MessageType::Hello, "HELLO"},
    {MessageType::HelloAck, "HELLO_ACK"},
    {MessageType::RegisterResource, "REGISTER_RESOURCE"},
    {MessageType::PublishState, "PUBLISH_STATE"},
    {MessageType::RetireResource, "RETIRE_RESOURCE"},
    {MessageType::ResourceAck, "RESOURCE_ACK"},
    {MessageType::ScheduleRequest, "SCHEDULE_REQUEST"},
    {MessageType::Decision, "DECISION"},
    {MessageType::Assign, "ASSIGN"},
    {MessageType::AssignAck, "ASSIGN_ACK"},
    {MessageType::Running, "RUNNING"},
    {MessageType::Completion, "COMPLETION"},
    {MessageType::CompletionAck, "COMPLETION_ACK"},
    {MessageType::CancelPlacement, "CANCEL_PLACEMENT"},
    {MessageType::CancelRequest, "CANCEL_REQUEST"},
    {MessageType::Reassign, "REASSIGN"},
    {MessageType::Preempt, "PREEMPT"},
    {MessageType::LifecycleAck, "LIFECYCLE_ACK"},
    {MessageType::Inspect, "INSPECT"},
    {MessageType::InspectReply, "INSPECT_REPLY"},
    {MessageType::SaveState, "SAVE_STATE"},
    {MessageType::StateAck, "STATE_ACK"},
    {MessageType::Shutdown, "SHUTDOWN"},
    {MessageType::ShutdownAck, "SHUTDOWN_ACK"},
    {MessageType::Error, "ERROR"},
    {MessageType::DisconnectWorker, "DISCONNECT_WORKER"},
}};

Status read_outcome(ByteReader& reader, DecisionOutcome& outcome) {
    std::uint8_t raw = 0;
    Status status = reader.u8(raw);
    if (!status.ok()) {
        return status;
    }
    if (raw > 1) {
        return Status(ErrorCode::ProtocolError, "unknown decision outcome in payload");
    }
    outcome = static_cast<DecisionOutcome>(raw);
    return Status{};
}

Status read_error_code(ByteReader& reader, ErrorCode& code) {
    std::uint16_t raw = 0;
    Status status = reader.u16(raw);
    if (!status.ok()) {
        return status;
    }
    const ErrorCode parsed = static_cast<ErrorCode>(raw);
    if (error_code_name(parsed) == std::string_view("UNKNOWN")) {
        return Status(ErrorCode::ProtocolError, "unknown error code in payload");
    }
    code = parsed;
    return Status{};
}

Status read_placement_state(ByteReader& reader, PlacementState& state) {
    std::uint8_t raw = 0;
    Status status = reader.u8(raw);
    if (!status.ok()) {
        return status;
    }
    if (raw > static_cast<std::uint8_t>(PlacementState::Lost)) {
        return Status(ErrorCode::ProtocolError, "unknown placement state in payload");
    }
    state = static_cast<PlacementState>(raw);
    return Status{};
}

Status read_resource_lifecycle(ByteReader& reader, ResourceLifecycle& lifecycle) {
    std::uint8_t raw = 0;
    Status status = reader.u8(raw);
    if (!status.ok()) {
        return status;
    }
    if (raw > static_cast<std::uint8_t>(ResourceLifecycle::Lost)) {
        return Status(ErrorCode::ProtocolError, "unknown resource lifecycle in payload");
    }
    lifecycle = static_cast<ResourceLifecycle>(raw);
    return Status{};
}

Result<ByteReader> make_reader(std::span<const std::byte> payload) {
    return ByteReader(payload, ErrorCode::ProtocolError);
}

}  // namespace

std::string_view message_type_name(MessageType type) noexcept {
    for (const auto& entry : kMessageTypeNames) {
        if (entry.type == type) {
            return entry.name;
        }
    }
    return "UNKNOWN";
}

std::optional<MessageType> parse_message_type(std::string_view text) noexcept {
    for (const auto& entry : kMessageTypeNames) {
        if (entry.name == text) {
            return entry.type;
        }
    }
    return std::nullopt;
}

Result<std::vector<std::byte>> encode_frame(MessageType type, std::uint64_t correlation_id,
                                            std::uint64_t coordinator_epoch,
                                            std::span<const std::byte> payload) {
    if (payload.size() + kFrameHeaderSize > std::numeric_limits<std::uint32_t>::max()) {
        return Status(ErrorCode::ProtocolError, "frame payload is too large to encode");
    }
    std::vector<std::byte> frame;
    frame.reserve(kFrameHeaderSize + payload.size());
    ByteWriter writer;
    writer.u32(static_cast<std::uint32_t>(kFrameHeaderFields + payload.size()));
    writer.u32(kProtocolVersion);
    writer.u16(static_cast<std::uint16_t>(type));
    writer.u16(kFrameFlagsNone);
    writer.u64(correlation_id);
    writer.u64(coordinator_epoch);
    writer.u32(static_cast<std::uint32_t>(payload.size()));
    writer.u32(0);  // checksum placeholder
    const std::span<const std::byte> header = writer.span();
    frame.insert(frame.end(), header.begin(), header.end());
    frame.insert(frame.end(), payload.begin(), payload.end());

    // The checksum covers every header field after the length field plus the
    // payload; the checksum field itself is excluded.
    const std::span<const std::byte> header_prefix(frame.data() + 4, kFrameHeaderFields - 4);
    std::uint64_t hash = fnv1a64(header_prefix);
    if (!payload.empty()) {
        hash = fnv1a64_continue(hash, payload);
    }
    const std::uint32_t checksum = static_cast<std::uint32_t>(hash & 0xffffffffull);
    frame[32] = static_cast<std::byte>(checksum & 0xffu);
    frame[33] = static_cast<std::byte>((checksum >> 8) & 0xffu);
    frame[34] = static_cast<std::byte>((checksum >> 16) & 0xffu);
    frame[35] = static_cast<std::byte>((checksum >> 24) & 0xffu);
    return frame;
}

Result<FrameView> decode_frame(std::span<const std::byte> frame, const Limits& limits) {
    if (frame.size() < kFrameHeaderSize) {
        return Status(ErrorCode::ProtocolError, "frame is shorter than the header");
    }
    ByteReader reader(frame, ErrorCode::ProtocolError);
    std::uint32_t frame_length = 0;
    Status status = reader.u32(frame_length);
    if (!status.ok()) {
        return status;
    }
    if (frame_length != frame.size() - 4) {
        return Status(ErrorCode::ProtocolError, "frame length does not match the delivered bytes");
    }
    if (frame_length + 4 > limits.max_frame_size) {
        return Status(ErrorCode::ProtocolError, "frame exceeds the configured maximum size");
    }
    std::uint32_t protocol_version = 0;
    status = reader.u32(protocol_version);
    if (!status.ok()) {
        return status;
    }
    if (protocol_version != kProtocolVersion) {
        return Status(ErrorCode::ProtocolError, "unsupported protocol version");
    }
    std::uint16_t raw_type = 0;
    status = reader.u16(raw_type);
    if (!status.ok()) {
        return status;
    }
    bool known_type = false;
    for (const auto& entry : kMessageTypeNames) {
        if (static_cast<std::uint16_t>(entry.type) == raw_type) {
            known_type = true;
            break;
        }
    }
    if (!known_type) {
        return Status(ErrorCode::ProtocolError, "unknown message type");
    }
    const MessageType type = static_cast<MessageType>(raw_type);
    std::uint16_t flags = 0;
    status = reader.u16(flags);
    if (!status.ok()) {
        return status;
    }
    if (flags != kFrameFlagsNone) {
        return Status(ErrorCode::ProtocolError, "frame flags must be zero");
    }
    std::uint64_t correlation_id = 0;
    status = reader.u64(correlation_id);
    if (!status.ok()) {
        return status;
    }
    std::uint64_t coordinator_epoch = 0;
    status = reader.u64(coordinator_epoch);
    if (!status.ok()) {
        return status;
    }
    std::uint32_t payload_length = 0;
    status = reader.u32(payload_length);
    if (!status.ok()) {
        return status;
    }
    if (payload_length != frame_length - kFrameHeaderFields) {
        return Status(ErrorCode::ProtocolError, "declared payload length does not match the frame");
    }
    std::uint32_t checksum = 0;
    status = reader.u32(checksum);
    if (!status.ok()) {
        return status;
    }
    const std::span<const std::byte> header_prefix(frame.data() + 4, kFrameHeaderFields - 4);
    std::uint64_t hash = fnv1a64(header_prefix);
    if (payload_length > 0) {
        hash = fnv1a64_continue(hash, frame.subspan(kFrameHeaderSize, payload_length));
    }
    const std::uint32_t expected = static_cast<std::uint32_t>(hash & 0xffffffffull);
    if (checksum != expected) {
        return Status(ErrorCode::ProtocolError, "frame checksum does not match");
    }

    FrameView view;
    view.type = type;
    view.correlation_id = correlation_id;
    view.coordinator_epoch = coordinator_epoch;
    view.payload = frame.subspan(kFrameHeaderSize);
    return view;
}

namespace {

Status read_hello(ByteReader& reader, HelloMessage& message, const Limits& limits) {
    Status status = record_codec::read_id(reader, message.worker);
    if (!status.ok()) {
        return status;
    }
    status = record_codec::read_id(reader, message.boot);
    if (!status.ok()) {
        return status;
    }
    return record_codec::read_string(reader, message.authority, limits.max_string_length);
}

Status read_hello_ack(ByteReader& reader, HelloAckMessage& message, const Limits& limits) {
    Status status = record_codec::read_id(reader, message.epoch);
    if (!status.ok()) {
        return status;
    }
    return record_codec::read_string(reader, message.coordinator, limits.max_string_length);
}

Status read_resource_message(ByteReader& reader, ResourceMessage& message, const Limits& limits) {
    return record_codec::read_advertisement(reader, message.advertisement, limits);
}

Status read_retire(ByteReader& reader, RetireMessage& message, const Limits& limits) {
    static_cast<void>(limits);
    Status status = record_codec::read_id(reader, message.resource);
    if (!status.ok()) {
        return status;
    }
    return record_codec::read_id(reader, message.generation);
}

Status read_resource_ack(ByteReader& reader, ResourceAckMessage& message, const Limits& limits) {
    Status status = record_codec::read_id(reader, message.resource);
    if (!status.ok()) {
        return status;
    }
    status = record_codec::read_id(reader, message.generation);
    if (!status.ok()) {
        return status;
    }
    status = read_resource_lifecycle(reader, message.lifecycle);
    if (!status.ok()) {
        return status;
    }
    status = read_error_code(reader, message.code);
    if (!status.ok()) {
        return status;
    }
    return record_codec::read_string(reader, message.detail, limits.max_string_length);
}

Status read_schedule(ByteReader& reader, ScheduleMessage& message, const Limits& limits) {
    return record_codec::read_request(reader, message.request, limits);
}

Status read_decision(ByteReader& reader, DecisionMessage& message, const Limits& limits) {
    Status status = record_codec::read_id(reader, message.decision);
    if (!status.ok()) {
        return status;
    }
    status = read_outcome(reader, message.outcome);
    if (!status.ok()) {
        return status;
    }
    status = read_error_code(reader, message.failure);
    if (!status.ok()) {
        return status;
    }
    status = record_codec::read_string(reader, message.failure_detail, limits.max_string_length);
    if (!status.ok()) {
        return status;
    }
    status = record_codec::read_placement(reader, message.placement, limits);
    if (!status.ok()) {
        return status;
    }
    status = record_codec::read_authority_envelope(reader, message.authority, limits);
    if (!status.ok()) {
        return status;
    }
    return reader.str(message.explanation, limits.max_explanation_entries * limits.max_string_length);
}

Status read_assign(ByteReader& reader, AssignMessage& message, const Limits& limits) {
    Status status = record_codec::read_authority_envelope(reader, message.authority, limits);
    if (!status.ok()) {
        return status;
    }
    return record_codec::read_string(reader, message.workload, limits.max_string_length);
}

Status read_authority(ByteReader& reader, AuthorityMessage& message, const Limits& limits) {
    static_cast<void>(limits);
    Status status = record_codec::read_id(reader, message.placement);
    if (!status.ok()) {
        return status;
    }
    status = record_codec::read_id(reader, message.generation);
    if (!status.ok()) {
        return status;
    }
    status = record_codec::read_id(reader, message.epoch);
    if (!status.ok()) {
        return status;
    }
    return record_codec::read_id(reader, message.worker_boot);
}

Status read_completion(ByteReader& reader, CompletionClaim& claim, const Limits& limits) {
    return lab_scheduler::record_codec::read_completion_claim(reader, claim, limits);
}

Status read_completion_ack(ByteReader& reader, CompletionAckMessage& message, const Limits& limits) {
    Status status = record_codec::read_id(reader, message.placement);
    if (!status.ok()) {
        return status;
    }
    status = read_placement_state(reader, message.state);
    if (!status.ok()) {
        return status;
    }
    status = reader.boolean(message.accepted);
    if (!status.ok()) {
        return status;
    }
    status = reader.boolean(message.duplicate);
    if (!status.ok()) {
        return status;
    }
    status = read_error_code(reader, message.code);
    if (!status.ok()) {
        return status;
    }
    status = record_codec::read_string(reader, message.detail, limits.max_string_length);
    if (!status.ok()) {
        return status;
    }
    return reader.u32(message.released_reservations);
}

Status read_cancel_placement(ByteReader& reader, CancelPlacementMessage& message, const Limits& limits) {
    Status status = record_codec::read_id(reader, message.placement);
    if (!status.ok()) {
        return status;
    }
    status = record_codec::read_id(reader, message.generation);
    if (!status.ok()) {
        return status;
    }
    status = record_codec::read_id(reader, message.epoch);
    if (!status.ok()) {
        return status;
    }
    return record_codec::read_string(reader, message.reason, limits.max_string_length);
}

Status read_cancel_request(ByteReader& reader, CancelRequestMessage& message, const Limits& limits) {
    Status status = record_codec::read_id(reader, message.request);
    if (!status.ok()) {
        return status;
    }
    return record_codec::read_string(reader, message.reason, limits.max_string_length);
}

Status read_lifecycle_ack(ByteReader& reader, LifecycleAckMessage& message, const Limits& limits) {
    Status status = record_codec::read_id(reader, message.placement);
    if (!status.ok()) {
        return status;
    }
    status = record_codec::read_id(reader, message.generation);
    if (!status.ok()) {
        return status;
    }
    status = read_placement_state(reader, message.state);
    if (!status.ok()) {
        return status;
    }
    status = read_error_code(reader, message.code);
    if (!status.ok()) {
        return status;
    }
    status = record_codec::read_string(reader, message.detail, limits.max_string_length);
    if (!status.ok()) {
        return status;
    }
    status = reader.u32(message.released_reservations);
    if (!status.ok()) {
        return status;
    }
    return reader.u32(message.affected);
}

Status read_inspect(ByteReader& reader, InspectMessage& message, const Limits& limits) {
    return record_codec::read_string(reader, message.subject, limits.max_string_length);
}

Status read_inspect_reply(ByteReader& reader, InspectReplyMessage& message, const Limits& limits) {
    Status status = record_codec::read_string(reader, message.subject, limits.max_string_length);
    if (!status.ok()) {
        return status;
    }
    return reader.str(message.text, limits.max_explanation_entries * limits.max_string_length);
}

Status read_error_message(ByteReader& reader, ErrorMessage& message, const Limits& limits) {
    Status status = read_error_code(reader, message.code);
    if (!status.ok()) {
        return status;
    }
    return record_codec::read_string(reader, message.message, limits.max_string_length);
}

Status read_save_state(ByteReader& reader, SaveStateMessage& message, const Limits& limits) {
    return record_codec::read_string(reader, message.path, limits.max_string_length);
}

Status read_state_ack(ByteReader& reader, StateAckMessage& message, const Limits& limits) {
    Status status = record_codec::read_string(reader, message.path, limits.max_string_length);
    if (!status.ok()) {
        return status;
    }
    status = record_codec::read_id(reader, message.epoch);
    if (!status.ok()) {
        return status;
    }
    status = reader.u32(message.resources);
    if (!status.ok()) {
        return status;
    }
    status = reader.u32(message.requests);
    if (!status.ok()) {
        return status;
    }
    status = reader.u32(message.placements);
    if (!status.ok()) {
        return status;
    }
    status = reader.u32(message.reservations);
    if (!status.ok()) {
        return status;
    }
    status = read_error_code(reader, message.code);
    if (!status.ok()) {
        return status;
    }
    return record_codec::read_string(reader, message.detail, limits.max_string_length);
}

Status read_disconnect_worker(ByteReader& reader, DisconnectWorkerMessage& message, const Limits& limits) {
    Status status = record_codec::read_id(reader, message.worker);
    if (!status.ok()) {
        return status;
    }
    status = record_codec::read_id(reader, message.boot);
    if (!status.ok()) {
        return status;
    }
    return record_codec::read_string(reader, message.detail, limits.max_string_length);
}

}  // namespace

Result<std::vector<std::byte>> encode_hello(const HelloMessage& message, const Limits& limits) {
    static_cast<void>(limits);
    ByteWriter writer;
    record_codec::write_id(writer, message.worker);
    record_codec::write_id(writer, message.boot);
    writer.str(message.authority);
    return std::vector<std::byte>(writer.data().begin(), writer.data().end());
}

Result<std::vector<std::byte>> encode_hello_ack(const HelloAckMessage& message, const Limits& limits) {
    static_cast<void>(limits);
    ByteWriter writer;
    record_codec::write_id(writer, message.epoch);
    writer.str(message.coordinator);
    return std::vector<std::byte>(writer.data().begin(), writer.data().end());
}

Result<std::vector<std::byte>> encode_resource_message(const ResourceMessage& message, const Limits& limits) {
    ByteWriter writer;
    record_codec::write_advertisement(writer, message.advertisement, limits);
    return std::vector<std::byte>(writer.data().begin(), writer.data().end());
}

Result<std::vector<std::byte>> encode_retire(const RetireMessage& message, const Limits& limits) {
    static_cast<void>(limits);
    ByteWriter writer;
    record_codec::write_id(writer, message.resource);
    record_codec::write_id(writer, message.generation);
    return std::vector<std::byte>(writer.data().begin(), writer.data().end());
}

Result<std::vector<std::byte>> encode_resource_ack(const ResourceAckMessage& message, const Limits& limits) {
    static_cast<void>(limits);
    ByteWriter writer;
    record_codec::write_id(writer, message.resource);
    record_codec::write_id(writer, message.generation);
    writer.u8(static_cast<std::uint8_t>(message.lifecycle));
    writer.u16(static_cast<std::uint16_t>(message.code));
    writer.str(message.detail);
    return std::vector<std::byte>(writer.data().begin(), writer.data().end());
}

Result<std::vector<std::byte>> encode_schedule(const ScheduleMessage& message, const Limits& limits) {
    ByteWriter writer;
    record_codec::write_request(writer, message.request, limits);
    return std::vector<std::byte>(writer.data().begin(), writer.data().end());
}

Result<std::vector<std::byte>> encode_decision(const DecisionMessage& message, const Limits& limits) {
    static_cast<void>(limits);
    ByteWriter writer;
    record_codec::write_id(writer, message.decision);
    writer.u8(static_cast<std::uint8_t>(message.outcome));
    writer.u16(static_cast<std::uint16_t>(message.failure));
    writer.str(message.failure_detail);
    record_codec::write_placement(writer, message.placement);
    record_codec::write_authority_envelope(writer, message.authority);
    writer.str(message.explanation);
    return std::vector<std::byte>(writer.data().begin(), writer.data().end());
}

Result<std::vector<std::byte>> encode_assign(const AssignMessage& message, const Limits& limits) {
    static_cast<void>(limits);
    ByteWriter writer;
    record_codec::write_authority_envelope(writer, message.authority);
    writer.str(message.workload);
    return std::vector<std::byte>(writer.data().begin(), writer.data().end());
}

Result<std::vector<std::byte>> encode_authority(const AuthorityMessage& message, const Limits& limits) {
    static_cast<void>(limits);
    ByteWriter writer;
    record_codec::write_id(writer, message.placement);
    record_codec::write_id(writer, message.generation);
    record_codec::write_id(writer, message.epoch);
    record_codec::write_id(writer, message.worker_boot);
    return std::vector<std::byte>(writer.data().begin(), writer.data().end());
}

Result<std::vector<std::byte>> encode_completion(const CompletionClaim& claim, const Limits& limits) {
    ByteWriter writer;
    record_codec::write_completion_claim(writer, claim);
    static_cast<void>(limits);
    return std::vector<std::byte>(writer.data().begin(), writer.data().end());
}

Result<std::vector<std::byte>> encode_completion_ack(const CompletionAckMessage& message, const Limits& limits) {
    static_cast<void>(limits);
    ByteWriter writer;
    record_codec::write_id(writer, message.placement);
    writer.u8(static_cast<std::uint8_t>(message.state));
    writer.boolean(message.accepted);
    writer.boolean(message.duplicate);
    writer.u16(static_cast<std::uint16_t>(message.code));
    writer.str(message.detail);
    writer.u32(message.released_reservations);
    return std::vector<std::byte>(writer.data().begin(), writer.data().end());
}

Result<std::vector<std::byte>> encode_cancel_placement(const CancelPlacementMessage& message,
                                                       const Limits& limits) {
    static_cast<void>(limits);
    ByteWriter writer;
    record_codec::write_id(writer, message.placement);
    record_codec::write_id(writer, message.generation);
    record_codec::write_id(writer, message.epoch);
    writer.str(message.reason);
    return std::vector<std::byte>(writer.data().begin(), writer.data().end());
}

Result<std::vector<std::byte>> encode_cancel_request(const CancelRequestMessage& message, const Limits& limits) {
    static_cast<void>(limits);
    ByteWriter writer;
    record_codec::write_id(writer, message.request);
    writer.str(message.reason);
    return std::vector<std::byte>(writer.data().begin(), writer.data().end());
}

Result<std::vector<std::byte>> encode_lifecycle_ack(const LifecycleAckMessage& message, const Limits& limits) {
    static_cast<void>(limits);
    ByteWriter writer;
    record_codec::write_id(writer, message.placement);
    record_codec::write_id(writer, message.generation);
    writer.u8(static_cast<std::uint8_t>(message.state));
    writer.u16(static_cast<std::uint16_t>(message.code));
    writer.str(message.detail);
    writer.u32(message.released_reservations);
    writer.u32(message.affected);
    return std::vector<std::byte>(writer.data().begin(), writer.data().end());
}

Result<std::vector<std::byte>> encode_inspect(const InspectMessage& message, const Limits& limits) {
    static_cast<void>(limits);
    ByteWriter writer;
    writer.str(message.subject);
    return std::vector<std::byte>(writer.data().begin(), writer.data().end());
}

Result<std::vector<std::byte>> encode_inspect_reply(const InspectReplyMessage& message, const Limits& limits) {
    static_cast<void>(limits);
    ByteWriter writer;
    writer.str(message.subject);
    writer.str(message.text);
    return std::vector<std::byte>(writer.data().begin(), writer.data().end());
}

Result<std::vector<std::byte>> encode_error_message(const ErrorMessage& message, const Limits& limits) {
    static_cast<void>(limits);
    ByteWriter writer;
    writer.u16(static_cast<std::uint16_t>(message.code));
    writer.str(message.message);
    return std::vector<std::byte>(writer.data().begin(), writer.data().end());
}

Result<std::vector<std::byte>> encode_save_state(const SaveStateMessage& message, const Limits& limits) {
    static_cast<void>(limits);
    ByteWriter writer;
    writer.str(message.path);
    return std::vector<std::byte>(writer.data().begin(), writer.data().end());
}

Result<std::vector<std::byte>> encode_state_ack(const StateAckMessage& message, const Limits& limits) {
    static_cast<void>(limits);
    ByteWriter writer;
    writer.str(message.path);
    record_codec::write_id(writer, message.epoch);
    writer.u32(message.resources);
    writer.u32(message.requests);
    writer.u32(message.placements);
    writer.u32(message.reservations);
    writer.u16(static_cast<std::uint16_t>(message.code));
    writer.str(message.detail);
    return std::vector<std::byte>(writer.data().begin(), writer.data().end());
}

Result<std::vector<std::byte>> encode_disconnect_worker(const DisconnectWorkerMessage& message,
                                                        const Limits& limits) {
    static_cast<void>(limits);
    ByteWriter writer;
    record_codec::write_id(writer, message.worker);
    record_codec::write_id(writer, message.boot);
    writer.str(message.detail);
    return std::vector<std::byte>(writer.data().begin(), writer.data().end());
}

template <class Message, class ReadFn>
Result<Message> decode_payload(std::span<const std::byte> payload, const Limits& limits, ReadFn read) {
    ByteReader reader(payload, ErrorCode::ProtocolError);
    Message message;
    Status status = read(reader, message, limits);
    if (!status.ok()) {
        return status;
    }
    status = reader.require_end();
    if (!status.ok()) {
        return status;
    }
    return message;
}

Result<HelloMessage> decode_hello(std::span<const std::byte> payload, const Limits& limits) {
    return decode_payload<HelloMessage>(payload, limits, read_hello);
}

Result<HelloAckMessage> decode_hello_ack(std::span<const std::byte> payload, const Limits& limits) {
    return decode_payload<HelloAckMessage>(payload, limits, read_hello_ack);
}

Result<ResourceMessage> decode_resource_message(std::span<const std::byte> payload, const Limits& limits) {
    return decode_payload<ResourceMessage>(payload, limits, read_resource_message);
}

Result<RetireMessage> decode_retire(std::span<const std::byte> payload, const Limits& limits) {
    return decode_payload<RetireMessage>(payload, limits, read_retire);
}

Result<ResourceAckMessage> decode_resource_ack(std::span<const std::byte> payload, const Limits& limits) {
    return decode_payload<ResourceAckMessage>(payload, limits, read_resource_ack);
}

Result<ScheduleMessage> decode_schedule(std::span<const std::byte> payload, const Limits& limits) {
    return decode_payload<ScheduleMessage>(payload, limits, read_schedule);
}

Result<DecisionMessage> decode_decision(std::span<const std::byte> payload, const Limits& limits) {
    return decode_payload<DecisionMessage>(payload, limits, read_decision);
}

Result<AssignMessage> decode_assign(std::span<const std::byte> payload, const Limits& limits) {
    return decode_payload<AssignMessage>(payload, limits, read_assign);
}

Result<AuthorityMessage> decode_authority(std::span<const std::byte> payload, const Limits& limits) {
    return decode_payload<AuthorityMessage>(payload, limits, read_authority);
}

Result<CompletionAckMessage> decode_completion_ack(std::span<const std::byte> payload, const Limits& limits) {
    return decode_payload<CompletionAckMessage>(payload, limits, read_completion_ack);
}

Result<CompletionClaim> decode_completion(std::span<const std::byte> payload, const Limits& limits) {
    return decode_payload<CompletionClaim>(payload, limits, read_completion);
}

Result<CancelPlacementMessage> decode_cancel_placement(std::span<const std::byte> payload, const Limits& limits) {
    return decode_payload<CancelPlacementMessage>(payload, limits, read_cancel_placement);
}

Result<CancelRequestMessage> decode_cancel_request(std::span<const std::byte> payload, const Limits& limits) {
    return decode_payload<CancelRequestMessage>(payload, limits, read_cancel_request);
}

Result<LifecycleAckMessage> decode_lifecycle_ack(std::span<const std::byte> payload, const Limits& limits) {
    return decode_payload<LifecycleAckMessage>(payload, limits, read_lifecycle_ack);
}

Result<InspectMessage> decode_inspect(std::span<const std::byte> payload, const Limits& limits) {
    return decode_payload<InspectMessage>(payload, limits, read_inspect);
}

Result<InspectReplyMessage> decode_inspect_reply(std::span<const std::byte> payload, const Limits& limits) {
    return decode_payload<InspectReplyMessage>(payload, limits, read_inspect_reply);
}

Result<ErrorMessage> decode_error_message(std::span<const std::byte> payload, const Limits& limits) {
    return decode_payload<ErrorMessage>(payload, limits, read_error_message);
}

Result<SaveStateMessage> decode_save_state(std::span<const std::byte> payload, const Limits& limits) {
    return decode_payload<SaveStateMessage>(payload, limits, read_save_state);
}

Result<StateAckMessage> decode_state_ack(std::span<const std::byte> payload, const Limits& limits) {
    return decode_payload<StateAckMessage>(payload, limits, read_state_ack);
}

Result<DisconnectWorkerMessage> decode_disconnect_worker(std::span<const std::byte> payload, const Limits& limits) {
    return decode_payload<DisconnectWorkerMessage>(payload, limits, read_disconnect_worker);
}

}  // namespace lab_scheduler
