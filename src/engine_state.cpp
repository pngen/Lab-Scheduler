#include "engine_internal.hpp"

#include <algorithm>
#include <utility>

#include "lab_scheduler/canonical.hpp"
#include "lab_scheduler/version.hpp"

namespace lab_scheduler {

void push_audit(SchedulerState& state, const EngineConfig& config, std::string kind, std::string detail) {
    if (!config.record_audit) {
        return;
    }
    ++state.audit_sequence;
    AuditRecord record;
    record.sequence = state.audit_sequence;
    record.kind = std::move(kind);
    record.detail = std::move(detail);
    state.audit.push_back(std::move(record));
    const std::size_t maximum = config.limits.max_audit_records;
    if (state.audit.size() > maximum) {
        const std::size_t excess = state.audit.size() - maximum;
        state.audit.erase(state.audit.begin(), state.audit.begin() + static_cast<std::ptrdiff_t>(excess));
    }
}

std::uint32_t available_slots(const ResourceRecord& resource) noexcept {
    const std::uint32_t used = resource.advertised_occupancy + resource.reserved_slots;
    if (used >= resource.capacity.slots) {
        return 0;
    }
    return resource.capacity.slots - used;
}

bool placement_state_is_live(PlacementState state) noexcept {
    switch (state) {
        case PlacementState::Reserved:
        case PlacementState::Assigned:
        case PlacementState::Running:
            return true;
        case PlacementState::Completed:
        case PlacementState::Failed:
        case PlacementState::Cancelled:
        case PlacementState::Preempted:
        case PlacementState::ReassignmentRequired:
        case PlacementState::Lost:
            return false;
    }
    return false;
}

bool release_reservation(SchedulerState& state, ReservationRecord& reservation, ReservationState target,
                         std::string detail) {
    if (reservation.state != ReservationState::Active) {
        return false;
    }
    for (const ReservationClaim& claim : reservation.claims) {
        const auto found = state.resources.find(claim.resource);
        if (found == state.resources.end()) {
            continue;
        }
        ResourceRecord& resource = found->second;
        if (resource.reserved_slots >= claim.slots) {
            resource.reserved_slots -= claim.slots;
        } else {
            resource.reserved_slots = 0;
        }
    }
    reservation.state = target;
    reservation.detail = std::move(detail);
    return true;
}

void revoke_reservation(SchedulerState& state, ReservationRecord& reservation, std::string detail) {
    static_cast<void>(release_reservation(state, reservation, ReservationState::Revoked, std::move(detail)));
}

Status check_epoch(const SchedulerState& state, CoordinatorEpoch epoch) {
    Status status = epoch.validate();
    if (!status.ok()) {
        return status;
    }
    if (!(epoch == state.epoch)) {
        std::string message = "coordinator epoch ";
        message += epoch.to_string();
        message += " is not the current epoch ";
        message += state.epoch.to_string();
        return Status(ErrorCode::StaleEpoch, std::move(message));
    }
    return Status{};
}

std::string resource_summary(const ResourceRecord& resource) {
    std::string out = resource.id.to_string();
    out += " ";
    out += resource_class_name(resource.resource_class);
    out += " '";
    out += resource.name;
    out += "' gen=";
    out += decimal_u64(resource.generation.value());
    out += " cap=";
    out += decimal_u64(resource.capacity.slots);
    out += " occ=";
    out += decimal_u64(resource.total_occupancy());
    out += " ";
    out += health_state_name(resource.health);
    out += " ";
    out += readiness_state_name(resource.readiness);
    out += resource.dynamic_authoritative ? " authoritative" : " NOT_AUTHORITATIVE";
    return out;
}

std::string placement_summary(const PlacementRecord& placement) {
    std::string out = placement.id.to_string();
    out += " gen=";
    out += decimal_u64(placement.generation.value());
    out += " ";
    out += placement_state_name(placement.state);
    out += " request=";
    out += placement.request.to_string();
    out += " resources=";
    for (std::size_t i = 0; i < placement.resources.size(); ++i) {
        if (i != 0) {
            out += ",";
        }
        out += placement.resources[i].resource.to_string();
    }
    return out;
}

ResourceLossReport invalidate_worker_incarnation(SchedulerState& state, const EngineConfig& config, WorkerId worker,
                                                 WorkerBootId boot, std::string detail) {
    ResourceLossReport report;
    report.worker = worker;
    report.boot = boot;
    report.detail = std::move(detail);

    std::vector<ResourceId> affected;
    for (auto& entry : state.resources) {
        ResourceRecord& resource = entry.second;
        if (resource.worker != worker || resource.boot != boot) {
            continue;
        }
        if (resource.dynamic_authoritative || resource.lifecycle == ResourceLifecycle::Current) {
            resource.dynamic_authoritative = false;
            resource.lifecycle = ResourceLifecycle::Lost;
            resource.health = HealthState::Unknown;
            resource.readiness = ReadinessState::Unknown;
            resource.health_detail = "incarnation lost";
            resource.generation = ResourceGeneration::from_value(resource.generation.value() + 1);
            resource.health_generation = HealthGeneration::from_value(resource.health_generation.value() + 1);
            resource.capacity_generation = CapacityGeneration::from_value(resource.capacity_generation.value() + 1);
            resource.validated_epoch = CoordinatorEpoch{};
            resource.authority_detail = "incarnation lost; fresh registration required";
            report.invalidated_resources.push_back(resource.id);
            affected.push_back(resource.id);
        }
    }

    for (auto& entry : state.placements) {
        PlacementRecord& placement = entry.second;
        if (!placement_state_is_live(placement.state)) {
            continue;
        }
        bool touches = placement.worker == worker && placement.worker_boot == boot;
        if (!touches) {
            for (const SelectedResource& selected : placement.resources) {
                if (std::find(affected.begin(), affected.end(), selected.resource) != affected.end()) {
                    touches = true;
                    break;
                }
            }
        }
        if (!touches) {
            continue;
        }
        const ReservationId reservation_id = placement.reservation;
        terminate_placement(state, placement, PlacementState::ReassignmentRequired,
                            "resource incarnation lost; reassignment required");
        AffectedPlacement affected_placement;
        affected_placement.placement = placement.id;
        affected_placement.generation = placement.generation;
        affected_placement.state = placement.state;
        affected_placement.detail = placement.detail;
        report.affected_placements.push_back(affected_placement);

        const auto reservation = state.reservations.find(reservation_id);
        if (reservation != state.reservations.end() && reservation->second.state == ReservationState::Revoked) {
            report.released_reservations.push_back(reservation_id);
        }
    }

    auto worker_entry = state.workers.find(worker);
    if (worker_entry != state.workers.end() && worker_entry->second.boot == boot) {
        worker_entry->second.connected = false;
        worker_entry->second.detail = report.detail;
    }

    push_audit(state, config, "worker.incarnation_invalidated",
                     worker.to_string() + " boot=" + boot.to_string() + " " + report.detail);
    report.status = Status{};
    return report;
}

ResourceLossReport adopt_worker_incarnation(SchedulerState& state, const EngineConfig& config, WorkerId worker,
                                            WorkerBootId boot, std::string detail) {
    ResourceLossReport report;
    auto found = state.workers.find(worker);
    if (found == state.workers.end()) {
        WorkerRecord record;
        record.id = worker;
        record.boot = boot;
        record.connected = true;
        ++state.worker_sequence;
        record.registration_sequence = state.worker_sequence;
        record.detail = "first incarnation observed by this coordinator";
        state.workers.emplace(worker, record);
        push_audit(state, config, "worker.connected",
                         worker.to_string() + " boot=" + boot.to_string() + " (first incarnation)");
        report.worker = worker;
        report.boot = boot;
        report.incarnation_adopted = true;
        report.detail = "first incarnation observed by this coordinator";
        return report;
    }
    if (found->second.boot == boot) {
        found->second.connected = true;
        found->second.detail = detail;
        report.worker = worker;
        report.boot = boot;
        report.detail = std::move(detail);
        return report;
    }

    const WorkerBootId previous = found->second.boot;
    report = invalidate_worker_incarnation(state, config, worker, previous, detail);
    report.incarnation_adopted = true;
    found = state.workers.find(worker);
    found->second.boot = boot;
    found->second.connected = true;
    ++state.worker_sequence;
    found->second.registration_sequence = state.worker_sequence;
    found->second.detail = "new incarnation adopted";
    push_audit(state, config, "worker.reincarnated",
                     worker.to_string() + " boot=" + boot.to_string() + " replaced boot=" + previous.to_string());
    return report;
}

SchedulerEngine::SchedulerEngine(EngineConfig config)
    : config_(std::move(config)), ids_(config_.id_seed), state_(std::make_unique<SchedulerState>()) {
    state_->epoch = CoordinatorEpoch::from_value(1);
}

SchedulerEngine::~SchedulerEngine() = default;

CoordinatorEpoch SchedulerEngine::epoch() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return state_->epoch;
}

Status SchedulerEngine::adopt_epoch(CoordinatorEpoch epoch) {
    Status status = epoch.validate();
    if (!status.ok()) {
        return status;
    }
    const std::lock_guard<std::mutex> guard(mutex_);
    if (epoch.value() <= state_->epoch.value()) {
        std::string message = "refusing to adopt epoch ";
        message += epoch.to_string();
        message += " that does not advance the current epoch ";
        message += state_->epoch.to_string();
        return Status(ErrorCode::GenerationMismatch, std::move(message));
    }
    state_->epoch = epoch;
    push_audit(*state_, config_, "epoch.adopted", epoch.to_string());
    return Status{};
}

Status SchedulerEngine::begin_shutdown() {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (state_->shutting_down) {
        return Status(ErrorCode::ShuttingDown, "coordinator is already shutting down");
    }
    state_->shutting_down = true;
    push_audit(*state_, config_, "coordinator.shutdown", "scheduler entered shutdown");
    return Status{};
}

bool SchedulerEngine::shutting_down() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return state_->shutting_down;
}

std::vector<WorkerRecord> SchedulerEngine::list_workers() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    std::vector<WorkerRecord> out;
    out.reserve(state_->workers.size());
    for (const auto& entry : state_->workers) {
        out.push_back(entry.second);
    }
    return out;
}

std::vector<AuditRecord> SchedulerEngine::list_audit() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return state_->audit;
}

SchedulerSnapshot SchedulerEngine::snapshot() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    SchedulerSnapshot snapshot_out;
    snapshot_out.epoch = state_->epoch;
    snapshot_out.shutting_down = state_->shutting_down;
    for (const auto& entry : state_->workers) {
        snapshot_out.workers.push_back(entry.second);
    }
    for (const auto& entry : state_->resources) {
        snapshot_out.resources.push_back(entry.second);
    }
    for (const auto& entry : state_->requests) {
        snapshot_out.requests.push_back(entry.second);
    }
    for (const auto& entry : state_->placements) {
        snapshot_out.placements.push_back(entry.second);
    }
    for (const auto& entry : state_->reservations) {
        snapshot_out.reservations.push_back(entry.second);
    }
    snapshot_out.audit = state_->audit;
    return snapshot_out;
}

std::vector<ResourceRecord> SchedulerEngine::list_resources() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    std::vector<ResourceRecord> out;
    out.reserve(state_->resources.size());
    for (const auto& entry : state_->resources) {
        out.push_back(entry.second);
    }
    return out;
}

std::vector<ResourceRecord> SchedulerEngine::list_stale_resources() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    std::vector<ResourceRecord> out;
    for (const auto& entry : state_->resources) {
        const ResourceRecord& resource = entry.second;
        if (!resource.dynamic_authoritative || resource.lifecycle == ResourceLifecycle::Lost ||
            resource.lifecycle == ResourceLifecycle::Registered) {
            out.push_back(resource);
        }
    }
    return out;
}

std::vector<StoredRequest> SchedulerEngine::list_requests() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    std::vector<StoredRequest> out;
    out.reserve(state_->requests.size());
    for (const auto& entry : state_->requests) {
        out.push_back(entry.second);
    }
    return out;
}

std::vector<PlacementRecord> SchedulerEngine::list_placements() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    std::vector<PlacementRecord> out;
    out.reserve(state_->placements.size());
    for (const auto& entry : state_->placements) {
        out.push_back(entry.second);
    }
    return out;
}

std::vector<PlacementRecord> SchedulerEngine::list_placements_for_experiment(ExperimentId experiment) const {
    const std::lock_guard<std::mutex> guard(mutex_);
    std::vector<PlacementRecord> out;
    for (const auto& entry : state_->placements) {
        if (entry.second.experiment == experiment) {
            out.push_back(entry.second);
        }
    }
    return out;
}

std::vector<ReservationRecord> SchedulerEngine::list_reservations() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    std::vector<ReservationRecord> out;
    out.reserve(state_->reservations.size());
    for (const auto& entry : state_->reservations) {
        out.push_back(entry.second);
    }
    return out;
}

Result<ResourceRecord> SchedulerEngine::get_resource(ResourceId resource) const {
    Status status = resource.validate();
    if (!status.ok()) {
        return status;
    }
    const std::lock_guard<std::mutex> guard(mutex_);
    const auto found = state_->resources.find(resource);
    if (found == state_->resources.end()) {
        return Status(ErrorCode::NotFound, "no such resource: " + resource.to_string());
    }
    return found->second;
}

Result<StoredRequest> SchedulerEngine::get_request(ScheduleRequestId request) const {
    Status status = request.validate();
    if (!status.ok()) {
        return status;
    }
    const std::lock_guard<std::mutex> guard(mutex_);
    const auto found = state_->requests.find(request);
    if (found == state_->requests.end()) {
        return Status(ErrorCode::NotFound, "no such scheduling request: " + request.to_string());
    }
    return found->second;
}

Result<Decision> SchedulerEngine::get_decision(DecisionId decision) const {
    Status status = decision.validate();
    if (!status.ok()) {
        return status;
    }
    const std::lock_guard<std::mutex> guard(mutex_);
    const auto found = state_->decisions.find(decision);
    if (found == state_->decisions.end()) {
        return Status(ErrorCode::NotFound, "no such decision: " + decision.to_string());
    }
    Decision out;
    out.id = decision;
    out.outcome = found->second.outcome;
    out.failure = found->second.failure;
    out.failure_detail = found->second.failure_detail;
    out.explanation = found->second;
    if (out.outcome == DecisionOutcome::Placed && !out.explanation.selected.empty()) {
        for (const auto& entry : state_->placements) {
            if (entry.second.decision == decision) {
                out.placement = entry.second;
                break;
            }
        }
    }
    return out;
}

Result<PlacementRecord> SchedulerEngine::get_placement(PlacementId placement) const {
    Status status = placement.validate();
    if (!status.ok()) {
        return status;
    }
    const std::lock_guard<std::mutex> guard(mutex_);
    const auto found = state_->placements.find(placement);
    if (found == state_->placements.end()) {
        return Status(ErrorCode::NotFound, "no such placement: " + placement.to_string());
    }
    return found->second;
}

Result<ReservationRecord> SchedulerEngine::get_reservation(ReservationId reservation) const {
    Status status = reservation.validate();
    if (!status.ok()) {
        return status;
    }
    const std::lock_guard<std::mutex> guard(mutex_);
    const auto found = state_->reservations.find(reservation);
    if (found == state_->reservations.end()) {
        return Status(ErrorCode::NotFound, "no such reservation: " + reservation.to_string());
    }
    return found->second;
}

DurableState SchedulerEngine::export_state() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    DurableState out;
    out.format_version = kPersistenceFormatVersion;
    out.epoch = state_->epoch;
    out.limits = config_.limits;
    out.default_weights = config_.default_weights;
    out.id_counter = ids_.counter();
    for (const auto& entry : state_->resources) {
        out.resources.push_back(entry.second);
    }
    for (const auto& entry : state_->requests) {
        out.requests.push_back(entry.second);
    }
    for (const auto& entry : state_->placements) {
        out.placements.push_back(entry.second);
    }
    for (const auto& entry : state_->reservations) {
        out.reservations.push_back(entry.second);
    }
    return out;
}

ResourceLossReport SchedulerEngine::worker_connected(WorkerId worker, WorkerBootId boot) {
    ResourceLossReport report;
    Status status = worker.validate();
    if (!status.ok()) {
        report.status = status;
        return report;
    }
    status = boot.validate();
    if (!status.ok()) {
        report.status = status;
        return report;
    }
    const std::lock_guard<std::mutex> guard(mutex_);
    return adopt_worker_incarnation(*state_, config_, worker, boot, "worker connected");
}

ResourceLossReport SchedulerEngine::worker_disconnected(WorkerId worker, WorkerBootId boot,
                                                        std::string detail) {
    return mark_worker_lost(worker, boot, std::move(detail));
}

ResourceLossReport SchedulerEngine::mark_worker_lost(WorkerId worker, WorkerBootId boot, std::string detail) {
    ResourceLossReport report;
    Status status = worker.validate();
    if (!status.ok()) {
        report.status = status;
        return report;
    }
    status = boot.validate();
    if (!status.ok()) {
        report.status = status;
        return report;
    }
    const std::lock_guard<std::mutex> guard(mutex_);
    const auto found = state_->workers.find(worker);
    if (found == state_->workers.end()) {
        report.status = Status(ErrorCode::NotFound, "unknown worker: " + worker.to_string());
        return report;
    }
    if (!(found->second.boot == boot)) {
        report.status = Status(ErrorCode::StaleWorker,
                               "worker " + worker.to_string() + " boot=" + boot.to_string() +
                                   " is not the current incarnation");
        return report;
    }
    report = invalidate_worker_incarnation(*state_, config_, worker, boot, std::move(detail));
    return report;
}

}  // namespace lab_scheduler
