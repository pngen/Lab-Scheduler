#pragma once

// Internal engine layout shared by the engine translation units. Nothing in
// this header is installed or part of the public API.

#include <compare>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "lab_scheduler/engine.hpp"

namespace lab_scheduler {

// One logical schedulable unit: an experiment generation, an optional trial,
// and the scheduling request that produced the placement. Exactly one
// placement may be current for a unit unless the policy permits replication.
struct UnitKey {
    ExperimentId experiment{};
    ExperimentGeneration generation{};
    TrialId trial{};

    friend auto operator<=>(const UnitKey&, const UnitKey&) noexcept = default;
};

struct SchedulerState {
    CoordinatorEpoch epoch{};
    bool shutting_down = false;

    std::map<WorkerId, WorkerRecord> workers{};
    std::map<ResourceId, ResourceRecord> resources{};
    std::map<ScheduleRequestId, StoredRequest> requests{};
    std::map<PlacementId, PlacementRecord> placements{};
    std::map<ReservationId, ReservationRecord> reservations{};
    std::map<DecisionId, Explanation> decisions{};
    std::map<UnitKey, PlacementId> unit_current{};
    std::map<std::string, std::uint64_t> tenant_service{};

    std::vector<AuditRecord> audit{};
    std::uint64_t audit_sequence = 0;
    std::uint64_t request_sequence = 0;
    std::uint64_t placement_sequence = 0;
    std::uint64_t transition_sequence = 0;
    std::uint64_t reservation_sequence = 0;
    std::uint64_t worker_sequence = 0;
};

void push_audit(SchedulerState& state, const EngineConfig& config, std::string kind, std::string detail);

[[nodiscard]] std::uint32_t available_slots(const ResourceRecord& resource) noexcept;
[[nodiscard]] bool placement_state_is_live(PlacementState state) noexcept;

// Releases a reservation exactly once. Returns false when the reservation was
// already released or revoked, which makes duplicate release harmless.
bool release_reservation(SchedulerState& state, ReservationRecord& reservation, ReservationState target,
                         std::string detail);
void revoke_reservation(SchedulerState& state, ReservationRecord& reservation, std::string detail);

// Revokes every authority a worker incarnation holds: its resources stop
// being authoritative, live placements referencing them become
// REASSIGNMENT_REQUIRED, and their reservations are revoked exactly once.
[[nodiscard]] ResourceLossReport invalidate_worker_incarnation(SchedulerState& state,
                                                               const EngineConfig& config, WorkerId worker,
                                                               WorkerBootId boot, std::string detail);

// Adopts (worker, boot) as the current incarnation, invalidating any previous
// incarnation first. This is the only path that can make a worker's dynamic
// state authoritative.
[[nodiscard]] ResourceLossReport adopt_worker_incarnation(SchedulerState& state, const EngineConfig& config,
                                                          WorkerId worker, WorkerBootId boot,
                                                          std::string detail);

[[nodiscard]] Status check_epoch(const SchedulerState& state, CoordinatorEpoch epoch);
[[nodiscard]] std::string resource_summary(const ResourceRecord& resource);
[[nodiscard]] std::string placement_summary(const PlacementRecord& placement);

// Runs candidate construction, hard filtering, whole-placement validation,
// ranking, and the reservation commit for one stored request. The caller must
// already hold the scheduler mutex.
Result<Decision> schedule_locked(SchedulerState& state, const EngineConfig& config, IdGenerator& ids,
                                 StoredRequest& stored, PlacementGeneration forced_generation,
                                 std::uint32_t reassignment_count);

// Transitions a live placement to a terminal state, revoking its reservation
// exactly once. The caller must already hold the scheduler mutex.
void terminate_placement(SchedulerState& state, PlacementRecord& placement, PlacementState target,
                         std::string detail);

}  // namespace lab_scheduler
