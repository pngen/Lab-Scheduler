#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "lab_scheduler/error.hpp"
#include "lab_scheduler/identity.hpp"
#include "lab_scheduler/limits.hpp"
#include "lab_scheduler/resource.hpp"

namespace lab_scheduler {

struct SelectedResource {
    ResourceId resource{};
    ResourceClass resource_class = ResourceClass::Worker;
    ResourceGeneration generation{};  // generation that was current at selection time
    std::uint32_t slots = 0;
    bool exclusive = false;
    std::string requirement_name{};
    WorkerId worker{};
    WorkerBootId worker_boot{};
    std::string resource_name{};
};

// The authority envelope is the complete set of facts a placement is bound to.
// Every later message that claims to act on the placement is checked against
// this envelope in a fixed order before it can mutate scheduler state.
struct AuthorityEnvelope {
    ScheduleRequestId request{};
    ExperimentId experiment{};
    ExperimentGeneration experiment_generation{};
    PlacementId placement{};
    PlacementGeneration placement_generation{};
    CoordinatorEpoch epoch{};
    LeaseId lease{};
    ReservationId reservation{};
    WorkerId worker{};
    WorkerBootId worker_boot{};
    std::vector<SelectedResource> resources{};
};

enum class PlacementState : std::uint8_t {
    Reserved = 0,
    Assigned = 1,
    Running = 2,
    Completed = 3,
    Failed = 4,
    Cancelled = 5,
    Preempted = 6,
    ReassignmentRequired = 7,
    Lost = 8,
};

std::string_view placement_state_name(PlacementState state) noexcept;
std::optional<PlacementState> parse_placement_state(std::string_view text) noexcept;
bool placement_state_is_terminal(PlacementState state) noexcept;
bool placement_state_is_current(PlacementState state) noexcept;

struct PlacementRecord {
    PlacementId id{};
    PlacementGeneration generation{};
    ScheduleRequestId request{};
    DecisionId decision{};
    ExperimentId experiment{};
    ExperimentGeneration experiment_generation{};
    TrialId trial{};
    CoordinatorEpoch epoch{};  // coordinator epoch that issued this generation
    std::vector<SelectedResource> resources{};
    ReservationId reservation{};
    LeaseId lease{};
    WorkerId worker{};
    WorkerBootId worker_boot{};
    PlacementState state = PlacementState::Reserved;
    std::uint32_t reassignment_count = 0;
    bool completion_recorded = false;
    std::string detail{};
    std::uint64_t transition_sequence = 0;  // monotonic transition counter
    std::string completion_detail{};
};

enum class ReservationState : std::uint8_t { Active = 0, Released = 1, Revoked = 2 };

std::string_view reservation_state_name(ReservationState state) noexcept;
std::optional<ReservationState> parse_reservation_state(std::string_view text) noexcept;

struct ReservationClaim {
    ResourceId resource{};
    std::uint32_t slots = 0;
    bool exclusive = false;
    ResourceGeneration generation_at_acquire{};
};

struct ReservationRecord {
    ReservationId id{};
    LeaseId lease{};
    PlacementId placement{};
    PlacementGeneration placement_generation{};
    ScheduleRequestId request{};
    ExperimentId experiment{};
    ExperimentGeneration experiment_generation{};
    CoordinatorEpoch epoch{};
    std::vector<ReservationClaim> claims{};
    ReservationState state = ReservationState::Active;
    std::uint64_t acquire_sequence = 0;
    std::string detail{};
};

}  // namespace lab_scheduler
