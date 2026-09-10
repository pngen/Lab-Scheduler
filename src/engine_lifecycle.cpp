#include "engine_internal.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include "lab_scheduler/canonical.hpp"

namespace lab_scheduler {

void terminate_placement(SchedulerState& state, PlacementRecord& placement, PlacementState target,
                         std::string detail) {
    if (!placement_state_is_live(placement.state)) {
        return;
    }
    placement.state = target;
    placement.detail = std::move(detail);
    placement.transition_sequence = ++state.transition_sequence;

    const auto reservation = state.reservations.find(placement.reservation);
    if (reservation != state.reservations.end()) {
        if (target == PlacementState::Preempted || target == PlacementState::ReassignmentRequired ||
            target == PlacementState::Lost) {
            revoke_reservation(state, reservation->second, placement.detail);
        } else {
            static_cast<void>(release_reservation(state, reservation->second, ReservationState::Released,
                                                  placement.detail));
        }
    }

    auto stored = state.requests.find(placement.request);
    if (stored != state.requests.end() && stored->second.current_placement == placement.id) {
        stored->second.current_placement = PlacementId{};
        switch (target) {
            case PlacementState::Completed:
                stored->second.state = RequestState::Completed;
                stored->second.detail = placement.detail;
                break;
            case PlacementState::Failed:
                stored->second.state = RequestState::Failed;
                stored->second.detail = placement.detail;
                break;
            case PlacementState::Cancelled:
                stored->second.state = RequestState::Cancelled;
                stored->second.detail = placement.detail;
                break;
            case PlacementState::Preempted:
            case PlacementState::ReassignmentRequired:
            case PlacementState::Lost:
                stored->second.state = RequestState::Submitted;
                stored->second.detail = placement.detail;
                break;
            case PlacementState::Reserved:
            case PlacementState::Assigned:
            case PlacementState::Running:
                break;
        }
    }

    UnitKey key;
    key.experiment = placement.experiment;
    key.generation = placement.experiment_generation;
    key.trial = placement.trial;
    const auto unit = state.unit_current.find(key);
    if (unit != state.unit_current.end() && unit->second == placement.id) {
        state.unit_current.erase(unit);
    }
}

Result<PlacementRecord> SchedulerEngine::mark_assigned(PlacementId placement, PlacementGeneration generation,
                                                       CoordinatorEpoch epoch, WorkerBootId worker_boot) {
    Status status = placement.validate();
    if (!status.ok()) {
        return status;
    }
    status = generation.validate();
    if (!status.ok()) {
        return status;
    }
    const std::lock_guard<std::mutex> guard(mutex_);
    status = check_epoch(*state_, epoch);
    if (!status.ok()) {
        return status;
    }
    auto found = state_->placements.find(placement);
    if (found == state_->placements.end()) {
        return Status(ErrorCode::NotFound, "no such placement: " + placement.to_string());
    }
    PlacementRecord& record = found->second;
    if (!(record.generation == generation)) {
        return Status(ErrorCode::StalePlacement, "placement generation does not match the current generation");
    }
    if (!worker_boot.is_null() && !(record.worker_boot == worker_boot)) {
        return Status(ErrorCode::StaleWorker, "placement is not bound to this worker incarnation");
    }
    if (!placement_state_is_live(record.state)) {
        return Status(record.state == PlacementState::Cancelled ? ErrorCode::Cancelled : ErrorCode::StalePlacement,
                      std::string("placement is not live: ") + std::string(placement_state_name(record.state)));
    }
    if (record.state == PlacementState::Running) {
        return Status(ErrorCode::StalePlacement, "placement is already running");
    }
    record.state = PlacementState::Assigned;
    record.detail = "assigned to worker";
    record.transition_sequence = ++state_->transition_sequence;
    push_audit(*state_, config_, "placement.assigned", placement_summary(record));
    return record;
}

Result<PlacementRecord> SchedulerEngine::mark_running(PlacementId placement, PlacementGeneration generation,
                                                      CoordinatorEpoch epoch, WorkerBootId worker_boot) {
    Status status = placement.validate();
    if (!status.ok()) {
        return status;
    }
    status = generation.validate();
    if (!status.ok()) {
        return status;
    }
    const std::lock_guard<std::mutex> guard(mutex_);
    status = check_epoch(*state_, epoch);
    if (!status.ok()) {
        return status;
    }
    auto found = state_->placements.find(placement);
    if (found == state_->placements.end()) {
        return Status(ErrorCode::NotFound, "no such placement: " + placement.to_string());
    }
    PlacementRecord& record = found->second;
    if (!(record.generation == generation)) {
        return Status(ErrorCode::StalePlacement, "placement generation does not match the current generation");
    }
    if (!worker_boot.is_null() && !(record.worker_boot == worker_boot)) {
        return Status(ErrorCode::StaleWorker, "placement is not bound to this worker incarnation");
    }
    if (!placement_state_is_live(record.state)) {
        return Status(record.state == PlacementState::Cancelled ? ErrorCode::Cancelled : ErrorCode::StalePlacement,
                      std::string("placement is not live: ") + std::string(placement_state_name(record.state)));
    }
    record.state = PlacementState::Running;
    record.detail = "running on worker";
    record.transition_sequence = ++state_->transition_sequence;
    push_audit(*state_, config_, "placement.running", placement_summary(record));
    return record;
}

CompletionOutcome SchedulerEngine::report_completion(const CompletionClaim& claim) {
    CompletionOutcome outcome;
    Status status = claim.placement.validate();
    if (!status.ok()) {
        outcome.status = status;
        return outcome;
    }
    outcome.placement = claim.placement;

    const std::lock_guard<std::mutex> guard(mutex_);
    auto found = state_->placements.find(claim.placement);
    if (found == state_->placements.end()) {
        outcome.status = Status(ErrorCode::NotFound, "no such placement: " + claim.placement.to_string());
        return outcome;
    }
    PlacementRecord& placement = found->second;

    // Authority validation order is fixed and is documented in the README:
    // epoch, experiment identity, experiment generation, placement generation,
    // lifecycle state, worker incarnation, resource identity, resource
    // generation, reservation state, duplicate completion.
    status = check_epoch(*state_, claim.epoch);
    if (!status.ok()) {
        outcome.status = status;
        outcome.state = placement.state;
        return outcome;
    }
    if (claim.experiment.is_null() || !(claim.experiment == placement.experiment)) {
        outcome.status = Status(ErrorCode::StaleExperiment, "completion names a different experiment");
        outcome.state = placement.state;
        return outcome;
    }
    if (claim.experiment_generation.is_null() ||
        !(claim.experiment_generation == placement.experiment_generation)) {
        outcome.status = Status(ErrorCode::StaleExperiment, "completion names a different experiment generation");
        outcome.state = placement.state;
        return outcome;
    }
    if (!(claim.placement_generation == placement.generation)) {
        outcome.status = Status(ErrorCode::StalePlacement, "completion names a superseded placement generation");
        outcome.state = placement.state;
        return outcome;
    }
    if (placement.completion_recorded || placement.state == PlacementState::Completed ||
        placement.state == PlacementState::Failed) {
        outcome.duplicate = true;
        outcome.status = Status(ErrorCode::DuplicateCompletion,
                                "completion for this placement generation was already recorded");
        outcome.state = placement.state;
        return outcome;
    }
    if (!placement_state_is_live(placement.state)) {
        outcome.state = placement.state;
        switch (placement.state) {
            case PlacementState::Cancelled:
                outcome.status = Status(ErrorCode::Cancelled, "placement was cancelled; completion rejected");
                break;
            case PlacementState::Preempted:
                outcome.status = Status(ErrorCode::Preempted, "placement was preempted; completion rejected");
                break;
            case PlacementState::ReassignmentRequired:
                outcome.status = Status(ErrorCode::ReassignmentRequired,
                                        "placement lost its authority; completion rejected");
                break;
            default:
                outcome.status = Status(ErrorCode::StalePlacement, "placement is no longer current");
                break;
        }
        return outcome;
    }
    if (!claim.worker_boot.is_null() && !(claim.worker_boot == placement.worker_boot)) {
        outcome.status = Status(ErrorCode::StaleWorker, "completion arrives from a stale worker incarnation");
        outcome.state = placement.state;
        return outcome;
    }
    if (!claim.resources.empty()) {
        if (claim.resources.size() != placement.resources.size()) {
            outcome.status = Status(ErrorCode::Unauthorized, "completion names the wrong resource set");
            outcome.state = placement.state;
            return outcome;
        }
        std::vector<ResourceId> claimed(claim.resources);
        std::vector<ResourceId> bound;
        bound.reserve(placement.resources.size());
        for (const SelectedResource& selected : placement.resources) {
            bound.push_back(selected.resource);
        }
        std::sort(claimed.begin(), claimed.end());
        std::sort(bound.begin(), bound.end());
        if (claimed != bound) {
            outcome.status = Status(ErrorCode::Unauthorized, "completion names the wrong resource set");
            outcome.state = placement.state;
            return outcome;
        }
    }
    if (!claim.resource_generations.empty()) {
        if (claim.resource_generations.size() != placement.resources.size()) {
            outcome.status = Status(ErrorCode::Unauthorized, "completion carries the wrong number of generations");
            outcome.state = placement.state;
            return outcome;
        }
        for (std::size_t i = 0; i < placement.resources.size(); ++i) {
            const auto resource = state_->resources.find(placement.resources[i].resource);
            if (resource == state_->resources.end()) {
                outcome.status = Status(ErrorCode::ResourceLost, "a bound resource is no longer registered");
                outcome.state = placement.state;
                return outcome;
            }
            if (!(resource->second.generation == claim.resource_generations[i])) {
                outcome.status = Status(ErrorCode::StaleResource,
                                        "completion observes a stale resource generation on " +
                                            placement.resources[i].resource.to_string());
                outcome.state = placement.state;
                return outcome;
            }
        }
    }
    const auto reservation = state_->reservations.find(placement.reservation);
    if (reservation == state_->reservations.end() ||
        reservation->second.state != ReservationState::Active) {
        outcome.status = Status(ErrorCode::ReservationNotActive,
                                "the reservation backing this placement is no longer active");
        outcome.state = placement.state;
        return outcome;
    }

    placement.completion_recorded = true;
    placement.completion_detail = claim.detail;
    const std::string detail = claim.succeeded
                                   ? std::string("completion reported by worker")
                                   : std::string("failure reported by worker");
    terminate_placement(*state_, placement, claim.succeeded ? PlacementState::Completed : PlacementState::Failed,
                        detail);
    outcome.accepted = true;
    outcome.state = placement.state;
    outcome.status = Status{};
    outcome.released_reservations.push_back(placement.reservation);
    push_audit(*state_, config_, claim.succeeded ? "placement.completed" : "placement.failed",
               placement_summary(placement));
    return outcome;
}

CancellationReport SchedulerEngine::cancel_placement(PlacementId placement, PlacementGeneration generation,
                                                     CoordinatorEpoch epoch, std::string reason) {
    CancellationReport report;
    Status status = placement.validate();
    if (!status.ok()) {
        report.status = status;
        return report;
    }
    report.placement = placement;
    const std::lock_guard<std::mutex> guard(mutex_);
    status = check_epoch(*state_, epoch);
    if (!status.ok()) {
        report.status = status;
        return report;
    }
    auto found = state_->placements.find(placement);
    if (found == state_->placements.end()) {
        report.status = Status(ErrorCode::NotFound, "no such placement: " + placement.to_string());
        return report;
    }
    PlacementRecord& record = found->second;
    if (!generation.is_null() && !(record.generation == generation)) {
        report.status = Status(ErrorCode::StalePlacement, "cancellation names a superseded placement generation");
        report.state = record.state;
        return report;
    }
    report.state = record.state;
    if (record.state == PlacementState::Cancelled) {
        report.status = Status(ErrorCode::Cancelled, "placement was already cancelled");
        report.detail = "duplicate cancellation ignored";
        return report;
    }
    if (!placement_state_is_live(record.state)) {
        report.status = Status(ErrorCode::StalePlacement,
                               std::string("placement already reached terminal state ") +
                                   std::string(placement_state_name(record.state)));
        report.detail = "cancellation had no effect";
        return report;
    }
    const ReservationId reservation = record.reservation;
    terminate_placement(*state_, record, PlacementState::Cancelled,
                        reason.empty() ? std::string("cancelled by controller") : std::move(reason));
    report.cancelled_placements.push_back(record.id);
    report.released_reservations.push_back(reservation);
    report.state = record.state;
    report.detail = record.detail;
    report.status = Status{};
    push_audit(*state_, config_, "placement.cancelled", placement_summary(record));
    return report;
}

CancellationReport SchedulerEngine::cancel_request(ScheduleRequestId request, std::string reason) {
    CancellationReport report;
    Status status = request.validate();
    if (!status.ok()) {
        report.status = status;
        return report;
    }
    report.request = request;
    const std::lock_guard<std::mutex> guard(mutex_);
    auto stored = state_->requests.find(request);
    if (stored == state_->requests.end()) {
        report.status = Status(ErrorCode::NotFound, "no such scheduling request: " + request.to_string());
        return report;
    }
    for (auto& entry : state_->placements) {
        PlacementRecord& placement = entry.second;
        if (placement.request != request || !placement_state_is_live(placement.state)) {
            continue;
        }
        const ReservationId reservation = placement.reservation;
        terminate_placement(*state_, placement, PlacementState::Cancelled,
                            reason.empty() ? std::string("request cancelled by controller") : reason);
        report.cancelled_placements.push_back(placement.id);
        report.released_reservations.push_back(reservation);
    }
    stored->second.state = RequestState::Cancelled;
    stored->second.current_placement = PlacementId{};
    stored->second.detail = reason.empty() ? std::string("cancelled by controller") : reason;
    report.detail = stored->second.detail;
    report.status = Status{};
    push_audit(*state_, config_, "request.cancelled", request.to_string() + " " + report.detail);
    return report;
}

Result<ReassignmentReport> SchedulerEngine::reassign_placement(PlacementId placement,
                                                               PlacementGeneration generation,
                                                               CoordinatorEpoch epoch, std::string reason) {
    ReassignmentReport report;
    report.reason = reason;
    Status status = placement.validate();
    if (!status.ok()) {
        return status;
    }
    const std::lock_guard<std::mutex> guard(mutex_);
    status = check_epoch(*state_, epoch);
    if (!status.ok()) {
        return status;
    }
    auto found = state_->placements.find(placement);
    if (found == state_->placements.end()) {
        return Status(ErrorCode::NotFound, "no such placement: " + placement.to_string());
    }
    PlacementRecord& record = found->second;
    if (!generation.is_null() && !(record.generation == generation)) {
        return Status(ErrorCode::StalePlacement, "reassignment names a superseded placement generation");
    }
    if (placement_state_is_live(record.state)) {
        const ReservationId reservation = record.reservation;
        terminate_placement(*state_, record, PlacementState::ReassignmentRequired,
                            reason.empty() ? std::string("reassigned by controller") : reason);
        report.revoked_reservations.push_back(reservation);
    } else if (record.state != PlacementState::ReassignmentRequired) {
        return Status(ErrorCode::StalePlacement,
                      std::string("placement cannot be reassigned from state ") +
                          std::string(placement_state_name(record.state)));
    }

    report.previous_placement = record.id;
    report.previous_generation = record.generation;

    auto stored = state_->requests.find(record.request);
    if (stored == state_->requests.end()) {
        return Status(ErrorCode::NotFound, "the placement's scheduling request is no longer recorded");
    }
    if (record.reassignment_count >= stored->second.request.max_reassignments) {
        return Status(ErrorCode::ReassignmentRequired,
                      "reassignment budget exhausted for this placement");
    }

    const PlacementGeneration next_generation =
        PlacementGeneration::from_value(record.generation.value() + 1);
    const std::uint32_t reassignment_count = record.reassignment_count + 1;
    Result<Decision> decision =
        schedule_locked(*state_, config_, ids_, stored->second, next_generation, reassignment_count);
    if (!decision.ok()) {
        return decision.status();
    }
    report.decision = decision.value();
    if (report.decision.outcome != DecisionOutcome::Placed) {
        report.status = Status(report.decision.failure,
                               "reassignment could not find a valid placement: " + report.decision.failure_detail);
        return report;
    }
    report.new_placement = report.decision.placement.id;
    report.new_generation = report.decision.placement.generation;
    report.status = Status{};
    push_audit(*state_, config_, "placement.reassigned",
               report.previous_placement.to_string() + " -> " + report.new_placement.to_string());
    return report;
}

Result<ReassignmentReport> SchedulerEngine::preempt_placement(PlacementId placement,
                                                              PlacementGeneration generation,
                                                              CoordinatorEpoch epoch, std::string reason) {
    ReassignmentReport report;
    report.reason = reason;
    Status status = placement.validate();
    if (!status.ok()) {
        return status;
    }
    const std::lock_guard<std::mutex> guard(mutex_);
    status = check_epoch(*state_, epoch);
    if (!status.ok()) {
        return status;
    }
    auto found = state_->placements.find(placement);
    if (found == state_->placements.end()) {
        return Status(ErrorCode::NotFound, "no such placement: " + placement.to_string());
    }
    PlacementRecord& record = found->second;
    if (!generation.is_null() && !(record.generation == generation)) {
        return Status(ErrorCode::StalePlacement, "preemption names a superseded placement generation");
    }
    if (!placement_state_is_live(record.state)) {
        return Status(record.state == PlacementState::Cancelled ? ErrorCode::Cancelled : ErrorCode::StalePlacement,
                      "placement is not live and cannot be preempted");
    }
    const ReservationId reservation = record.reservation;
    terminate_placement(*state_, record, PlacementState::Preempted,
                        reason.empty() ? std::string("preempted to free a constrained resource") : reason);
    report.previous_placement = record.id;
    report.previous_generation = record.generation;
    report.revoked_reservations.push_back(reservation);

    auto stored = state_->requests.find(record.request);
    if (stored == state_->requests.end()) {
        report.status = Status{};
        return report;
    }
    stored->second.state = RequestState::Submitted;
    if (!stored->second.request.allow_preemption) {
        report.status = Status{};
        return report;
    }
    if (record.reassignment_count >= stored->second.request.max_reassignments) {
        report.status = Status{};
        return report;
    }
    const PlacementGeneration next_generation = PlacementGeneration::from_value(record.generation.value() + 1);
    Result<Decision> decision = schedule_locked(*state_, config_, ids_, stored->second, next_generation,
                                                record.reassignment_count + 1);
    if (!decision.ok()) {
        return decision.status();
    }
    report.decision = decision.value();
    if (report.decision.outcome == DecisionOutcome::Placed) {
        report.new_placement = report.decision.placement.id;
        report.new_generation = report.decision.placement.generation;
    }
    report.status = Status{};
    push_audit(*state_, config_, "placement.preempted",
               placement.to_string() + " " + (reason.empty() ? std::string("preempted") : reason));
    return report;
}

}  // namespace lab_scheduler
