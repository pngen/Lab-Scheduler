#include "lab_scheduler/error.hpp"

namespace lab_scheduler {
namespace {

struct ErrorCodeName {
    ErrorCode code;
    std::string_view name;
};

constexpr ErrorCodeName kErrorNames[] = {
    {ErrorCode::Ok, "OK"},
    {ErrorCode::NoPlacement, "NO_PLACEMENT"},
    {ErrorCode::ResourceLost, "RESOURCE_LOST"},
    {ErrorCode::StaleResource, "STALE_RESOURCE"},
    {ErrorCode::StaleEpoch, "STALE_EPOCH"},
    {ErrorCode::StaleWorker, "STALE_WORKER"},
    {ErrorCode::StalePlacement, "STALE_PLACEMENT"},
    {ErrorCode::StaleExperiment, "STALE_EXPERIMENT"},
    {ErrorCode::CapacityExhausted, "CAPACITY_EXHAUSTED"},
    {ErrorCode::CapabilityMismatch, "CAPABILITY_MISMATCH"},
    {ErrorCode::TopologyMismatch, "TOPOLOGY_MISMATCH"},
    {ErrorCode::ModelMismatch, "MODEL_MISMATCH"},
    {ErrorCode::DatasetMismatch, "DATASET_MISMATCH"},
    {ErrorCode::EnvironmentUnavailable, "ENVIRONMENT_UNAVAILABLE"},
    {ErrorCode::Unsupported, "UNSUPPORTED"},
    {ErrorCode::Cancelled, "CANCELLED"},
    {ErrorCode::Preempted, "PREEMPTED"},
    {ErrorCode::ReassignmentRequired, "REASSIGNMENT_REQUIRED"},
    {ErrorCode::CorruptPersistence, "CORRUPT_PERSISTENCE"},
    {ErrorCode::ProtocolError, "PROTOCOL_ERROR"},
    {ErrorCode::ResourceExhausted, "RESOURCE_EXHAUSTED"},
    {ErrorCode::InvalidIdentity, "INVALID_IDENTITY"},
    {ErrorCode::DuplicateIdentity, "DUPLICATE_IDENTITY"},
    {ErrorCode::NotFound, "NOT_FOUND"},
    {ErrorCode::GenerationMismatch, "GENERATION_MISMATCH"},
    {ErrorCode::ReservationConflict, "RESERVATION_CONFLICT"},
    {ErrorCode::ReservationNotActive, "RESERVATION_NOT_ACTIVE"},
    {ErrorCode::Unauthorized, "UNAUTHORIZED"},
    {ErrorCode::DuplicateCompletion, "DUPLICATE_COMPLETION"},
    {ErrorCode::InvalidArgument, "INVALID_ARGUMENT"},
    {ErrorCode::LimitExceeded, "LIMIT_EXCEEDED"},
    {ErrorCode::HealthRejected, "HEALTH_REJECTED"},
    {ErrorCode::ExclusivityConflict, "EXCLUSIVITY_CONFLICT"},
    {ErrorCode::NotReady, "NOT_READY"},
    {ErrorCode::TenantViolation, "TENANT_VIOLATION"},
    {ErrorCode::IoFailure, "IO_FAILURE"},
    {ErrorCode::InternalError, "INTERNAL_ERROR"},
    {ErrorCode::ShuttingDown, "SHUTTING_DOWN"},
    {ErrorCode::Busy, "BUSY"},
};

struct RejectReasonName {
    RejectReason reason;
    std::string_view name;
};

constexpr RejectReasonName kRejectNames[] = {
    {RejectReason::None, "NONE"},
    {RejectReason::ResourceClassMismatch, "RESOURCE_CLASS_MISMATCH"},
    {RejectReason::CapabilityMismatch, "CAPABILITY_MISMATCH"},
    {RejectReason::CapacityExhausted, "CAPACITY_EXHAUSTED"},
    {RejectReason::MemoryExhausted, "MEMORY_EXHAUSTED"},
    {RejectReason::OccupancyExceeded, "OCCUPANCY_EXCEEDED"},
    {RejectReason::HealthRejected, "HEALTH_REJECTED"},
    {RejectReason::HealthUnknown, "HEALTH_UNKNOWN"},
    {RejectReason::EvidenceStale, "EVIDENCE_STALE"},
    {RejectReason::EvidenceNotAuthoritative, "EVIDENCE_NOT_AUTHORITATIVE"},
    {RejectReason::ResourceRetired, "RESOURCE_RETIRED"},
    {RejectReason::TopologyMismatch, "TOPOLOGY_MISMATCH"},
    {RejectReason::ModelMismatch, "MODEL_MISMATCH"},
    {RejectReason::ModelVersionMismatch, "MODEL_VERSION_MISMATCH"},
    {RejectReason::ModelNotResident, "MODEL_NOT_RESIDENT"},
    {RejectReason::SimulatorMismatch, "SIMULATOR_MISMATCH"},
    {RejectReason::SimulatorNotReady, "SIMULATOR_NOT_READY"},
    {RejectReason::DatasetMismatch, "DATASET_MISMATCH"},
    {RejectReason::DatasetVersionMismatch, "DATASET_VERSION_MISMATCH"},
    {RejectReason::EnvironmentMismatch, "ENVIRONMENT_MISMATCH"},
    {RejectReason::EnvironmentDigestMismatch, "ENVIRONMENT_DIGEST_MISMATCH"},
    {RejectReason::EnvironmentNotReady, "ENVIRONMENT_NOT_READY"},
    {RejectReason::AcceleratorMismatch, "ACCELERATOR_MISMATCH"},
    {RejectReason::ExclusivityConflict, "EXCLUSIVITY_CONFLICT"},
    {RejectReason::ReservationConflict, "RESERVATION_CONFLICT"},
    {RejectReason::TenantViolation, "TENANT_VIOLATION"},
    {RejectReason::AllowlistExcluded, "ALLOWLIST_EXCLUDED"},
    {RejectReason::DenylistExcluded, "DENYLIST_EXCLUDED"},
    {RejectReason::AffinityViolation, "AFFINITY_VIOLATION"},
    {RejectReason::AntiAffinityViolation, "ANTI_AFFINITY_VIOLATION"},
    {RejectReason::AlreadySelected, "ALREADY_SELECTED"},
    {RejectReason::Unsupported, "UNSUPPORTED"},
    {RejectReason::WorkerIncarnationStale, "WORKER_INCARNATION_STALE"},
    {RejectReason::ShuttingDown, "SHUTTING_DOWN"},
};

}  // namespace

std::string_view error_code_name(ErrorCode code) noexcept {
    for (const auto& entry : kErrorNames) {
        if (entry.code == code) {
            return entry.name;
        }
    }
    return "UNKNOWN";
}

std::optional<ErrorCode> parse_error_code(std::string_view text) noexcept {
    for (const auto& entry : kErrorNames) {
        if (entry.name == text) {
            return entry.code;
        }
    }
    return std::nullopt;
}

std::string_view reject_reason_name(RejectReason reason) noexcept {
    for (const auto& entry : kRejectNames) {
        if (entry.reason == reason) {
            return entry.name;
        }
    }
    return "UNKNOWN";
}

std::optional<RejectReason> parse_reject_reason(std::string_view text) noexcept {
    for (const auto& entry : kRejectNames) {
        if (entry.name == text) {
            return entry.reason;
        }
    }
    return std::nullopt;
}

Status ok_status() { return Status{}; }

Status error_status(ErrorCode code, std::string message) { return Status(code, std::move(message)); }

std::string Status::to_string() const {
    std::string out(error_code_name(code));
    if (!message.empty()) {
        out += ": ";
        out += message;
    }
    return out;
}

}  // namespace lab_scheduler
