#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace lab_scheduler {

enum class ErrorCode : std::uint16_t {
    Ok = 0,
    NoPlacement,
    ResourceLost,
    StaleResource,
    StaleEpoch,
    StaleWorker,
    StalePlacement,
    StaleExperiment,
    CapacityExhausted,
    CapabilityMismatch,
    TopologyMismatch,
    ModelMismatch,
    DatasetMismatch,
    EnvironmentUnavailable,
    Unsupported,
    Cancelled,
    Preempted,
    ReassignmentRequired,
    CorruptPersistence,
    ProtocolError,
    ResourceExhausted,
    InvalidIdentity,
    DuplicateIdentity,
    NotFound,
    GenerationMismatch,
    ReservationConflict,
    ReservationNotActive,
    Unauthorized,
    DuplicateCompletion,
    InvalidArgument,
    LimitExceeded,
    HealthRejected,
    ExclusivityConflict,
    NotReady,
    TenantViolation,
    IoFailure,
    InternalError,
    ShuttingDown,
    Busy,
};

std::string_view error_code_name(ErrorCode code) noexcept;
std::optional<ErrorCode> parse_error_code(std::string_view text) noexcept;

enum class RejectReason : std::uint16_t {
    None = 0,
    ResourceClassMismatch,
    CapabilityMismatch,
    CapacityExhausted,
    MemoryExhausted,
    OccupancyExceeded,
    HealthRejected,
    HealthUnknown,
    EvidenceStale,
    EvidenceNotAuthoritative,
    ResourceRetired,
    TopologyMismatch,
    ModelMismatch,
    ModelVersionMismatch,
    ModelNotResident,
    SimulatorMismatch,
    SimulatorNotReady,
    DatasetMismatch,
    DatasetVersionMismatch,
    EnvironmentMismatch,
    EnvironmentDigestMismatch,
    EnvironmentNotReady,
    AcceleratorMismatch,
    ExclusivityConflict,
    ReservationConflict,
    TenantViolation,
    AllowlistExcluded,
    DenylistExcluded,
    AffinityViolation,
    AntiAffinityViolation,
    AlreadySelected,
    Unsupported,
    WorkerIncarnationStale,
    ShuttingDown,
};

std::string_view reject_reason_name(RejectReason reason) noexcept;
std::optional<RejectReason> parse_reject_reason(std::string_view text) noexcept;

struct Status {
    ErrorCode code = ErrorCode::Ok;
    std::string message{};

    Status() = default;
    Status(ErrorCode c, std::string msg) : code(c), message(std::move(msg)) {}

    [[nodiscard]] bool ok() const noexcept { return code == ErrorCode::Ok; }
    [[nodiscard]] std::string to_string() const;
};

Status ok_status();
Status error_status(ErrorCode code, std::string message);

template <class T>
class Result {
public:
    Result(T value) : value_(std::move(value)) {}
    Result(Status status) : status_(std::move(status)) {}

    [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
    [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
    [[nodiscard]] const Status& status() const noexcept { return status_; }
    [[nodiscard]] ErrorCode code() const noexcept { return status_.code; }
    [[nodiscard]] const std::string& message() const noexcept { return status_.message; }

    [[nodiscard]] const T& value() const noexcept { return *value_; }
    [[nodiscard]] T& value() noexcept { return *value_; }
    [[nodiscard]] T take() noexcept { return std::move(*value_); }

private:
    Status status_{};
    std::optional<T> value_{};
};

template <class T>
Result<T> make_ok(T value) {
    return Result<T>(std::move(value));
}

template <class T>
Result<T> make_error(ErrorCode code, std::string message) {
    return Result<T>(Status(code, std::move(message)));
}

}  // namespace lab_scheduler
