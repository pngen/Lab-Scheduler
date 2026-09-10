#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "lab_scheduler/durable.hpp"
#include "lab_scheduler/explanation.hpp"
#include "lab_scheduler/limits.hpp"
#include "lab_scheduler/placement.hpp"
#include "lab_scheduler/request.hpp"
#include "lab_scheduler/resource.hpp"

namespace lab_scheduler {

// Internal scheduler state. Declared here so that the implementation
// translation units can share it without exposing its layout as API.
struct SchedulerState;

struct EngineConfig {
    Limits limits{};
    RankingWeights default_weights{};
    std::uint64_t id_seed = 1;
    bool record_audit = true;
};

struct AuditRecord {
    std::uint64_t sequence = 0;
    std::string kind{};
    std::string detail{};
};

struct AffectedPlacement {
    PlacementId placement{};
    PlacementGeneration generation{};
    PlacementState state = PlacementState::Reserved;
    std::string detail{};
};

struct WorkerRecord {
    WorkerId id{};
    WorkerBootId boot{};
    bool connected = false;
    std::uint64_t registration_sequence = 0;
    std::string detail{};
};

struct Decision {
    DecisionId id{};
    DecisionOutcome outcome = DecisionOutcome::NoPlacement;
    ErrorCode failure = ErrorCode::Ok;
    std::string failure_detail{};
    PlacementRecord placement{};
    AuthorityEnvelope authority{};
    Explanation explanation{};
};

struct ResourceLossReport {
    Status status{};
    WorkerId worker{};
    WorkerBootId boot{};
    bool incarnation_adopted = false;
    std::vector<ResourceId> invalidated_resources{};
    std::vector<AffectedPlacement> affected_placements{};
    std::vector<ReservationId> released_reservations{};
    std::string detail{};
};

struct CompletionClaim {
    PlacementId placement{};
    PlacementGeneration placement_generation{};
    ExperimentId experiment{};
    ExperimentGeneration experiment_generation{};
    CoordinatorEpoch epoch{};
    WorkerBootId worker_boot{};
    std::vector<ResourceId> resources{};
    std::vector<ResourceGeneration> resource_generations{};
    bool succeeded = true;
    std::string detail{};
};

struct CompletionOutcome {
    Status status{};
    bool accepted = false;
    bool duplicate = false;
    PlacementId placement{};
    PlacementState state = PlacementState::Reserved;
    std::vector<ReservationId> released_reservations{};
};

struct ReassignmentReport {
    Status status{};
    PlacementId previous_placement{};
    PlacementGeneration previous_generation{};
    PlacementId new_placement{};
    PlacementGeneration new_generation{};
    Decision decision{};
    std::vector<ReservationId> revoked_reservations{};
    std::string reason{};
};

struct CancellationReport {
    Status status{};
    ScheduleRequestId request{};
    PlacementId placement{};
    PlacementState state = PlacementState::Reserved;
    std::vector<PlacementId> cancelled_placements{};
    std::vector<ReservationId> released_reservations{};
    std::string detail{};
};

struct RecoveryReport {
    Status status{};
    CoordinatorEpoch previous_epoch{};
    CoordinatorEpoch current_epoch{};
    std::vector<ResourceId> resources_requiring_revalidation{};
    std::vector<AffectedPlacement> reconciled_placements{};
    std::vector<ReservationId> revoked_reservations{};
    std::uint32_t requests_preserved = 0;
    std::uint32_t placement_history_preserved = 0;
    std::string detail{};
};

struct ResourceAccounting {
    ResourceId resource{};
    ResourceClass resource_class = ResourceClass::Worker;
    std::string name{};
    std::uint32_t capacity_slots = 0;
    std::uint64_t capacity_memory_bytes = 0;
    std::uint32_t advertised_occupancy = 0;
    std::uint32_t reserved_slots = 0;
    std::uint32_t active_reservations = 0;
    ResourceLifecycle lifecycle = ResourceLifecycle::Registered;
    bool dynamic_authoritative = false;
    std::uint64_t generation = 0;
};

struct AccountingReport {
    CoordinatorEpoch epoch{};
    std::uint64_t placements_recorded = 0;
    std::uint64_t active_placements = 0;
    std::uint64_t requests_recorded = 0;
    std::uint64_t reservations_recorded = 0;
    std::uint64_t active_reservations = 0;
    std::uint64_t total_capacity_slots = 0;
    std::uint64_t total_reserved_slots = 0;
    std::vector<ResourceAccounting> resources{};
};

struct InvariantCheck {
    std::string name{};
    bool ok = true;
    std::string detail{};
};

struct InvariantReport {
    std::vector<InvariantCheck> checks{};

    [[nodiscard]] bool all_ok() const noexcept {
        for (const InvariantCheck& check : checks) {
            if (!check.ok) {
                return false;
            }
        }
        return true;
    }
};

struct SchedulerSnapshot {
    CoordinatorEpoch epoch{};
    bool shutting_down = false;
    std::vector<WorkerRecord> workers{};
    std::vector<ResourceRecord> resources{};
    std::vector<StoredRequest> requests{};
    std::vector<PlacementRecord> placements{};
    std::vector<ReservationRecord> reservations{};
    std::vector<AuditRecord> audit{};
};

// The scheduler engine owns placement authority. It is deliberately a single
// coherent state machine guarded by one mutex: every mutation happens while
// holding that lock, no callback, network send, or persistence write is ever
// performed under it, and readers receive copies rather than references.
class SchedulerEngine {
public:
    explicit SchedulerEngine(EngineConfig config = EngineConfig{});
    ~SchedulerEngine();

    SchedulerEngine(const SchedulerEngine&) = delete;
    SchedulerEngine& operator=(const SchedulerEngine&) = delete;
    SchedulerEngine(SchedulerEngine&&) = delete;
    SchedulerEngine& operator=(SchedulerEngine&&) = delete;

    [[nodiscard]] const Limits& limits() const noexcept { return config_.limits; }

    // --- coordinator epoch and lifecycle -------------------------------------
    [[nodiscard]] CoordinatorEpoch epoch() const;
    Status adopt_epoch(CoordinatorEpoch epoch);
    Status begin_shutdown();
    [[nodiscard]] bool shutting_down() const;

    // --- worker incarnation tracking ----------------------------------------
    ResourceLossReport worker_connected(WorkerId worker, WorkerBootId boot);
    ResourceLossReport worker_disconnected(WorkerId worker, WorkerBootId boot, std::string detail);
    ResourceLossReport mark_worker_lost(WorkerId worker, WorkerBootId boot, std::string detail);
    [[nodiscard]] std::vector<WorkerRecord> list_workers() const;

    // --- resources -----------------------------------------------------------
    Result<ResourceRecord> register_resource(const ResourceAdvertisement& advertisement);
    Result<ResourceRecord> publish_resource_state(const ResourceAdvertisement& advertisement);
    Result<ResourceRecord> retire_resource(ResourceId resource, ResourceGeneration expected_generation);
    Result<ResourceRecord> get_resource(ResourceId resource) const;
    [[nodiscard]] std::vector<ResourceRecord> list_resources() const;
    [[nodiscard]] std::vector<ResourceRecord> list_stale_resources() const;

    // --- scheduling ----------------------------------------------------------
    Result<Decision> submit_request(ScheduleRequest request);
    Result<Decision> get_decision(DecisionId decision) const;
    Result<StoredRequest> get_request(ScheduleRequestId request) const;
    [[nodiscard]] std::vector<StoredRequest> list_requests() const;

    // --- placement lifecycle -------------------------------------------------
    Result<PlacementRecord> get_placement(PlacementId placement) const;
    Result<PlacementRecord> mark_assigned(PlacementId placement, PlacementGeneration generation,
                                          CoordinatorEpoch epoch, WorkerBootId worker_boot);
    Result<PlacementRecord> mark_running(PlacementId placement, PlacementGeneration generation,
                                         CoordinatorEpoch epoch, WorkerBootId worker_boot);
    CompletionOutcome report_completion(const CompletionClaim& claim);
    Result<ReassignmentReport> reassign_placement(PlacementId placement, PlacementGeneration generation,
                                                  CoordinatorEpoch epoch, std::string reason);
    Result<ReassignmentReport> preempt_placement(PlacementId placement, PlacementGeneration generation,
                                                 CoordinatorEpoch epoch, std::string reason);
    CancellationReport cancel_placement(PlacementId placement, PlacementGeneration generation,
                                        CoordinatorEpoch epoch, std::string reason);
    CancellationReport cancel_request(ScheduleRequestId request, std::string reason);
    [[nodiscard]] std::vector<PlacementRecord> list_placements() const;
    [[nodiscard]] std::vector<PlacementRecord> list_placements_for_experiment(ExperimentId experiment) const;

    // --- reservations and accounting ----------------------------------------
    Result<ReservationRecord> get_reservation(ReservationId reservation) const;
    [[nodiscard]] std::vector<ReservationRecord> list_reservations() const;
    [[nodiscard]] AccountingReport accounting() const;
    [[nodiscard]] InvariantReport validate_invariants() const;

    // --- inspection ----------------------------------------------------------
    [[nodiscard]] SchedulerSnapshot snapshot() const;
    [[nodiscard]] std::vector<AuditRecord> list_audit() const;

    // --- persistence and recovery -------------------------------------------
    [[nodiscard]] DurableState export_state() const;
    Result<RecoveryReport> import_state(const DurableState& state);

private:
    EngineConfig config_;
    IdGenerator ids_;
    mutable std::mutex mutex_;
    std::unique_ptr<SchedulerState> state_;
};

std::string render_explanation(const Explanation& explanation);

}  // namespace lab_scheduler
