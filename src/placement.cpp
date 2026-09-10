#include "lab_scheduler/placement.hpp"

#include <array>

namespace lab_scheduler {
namespace {

struct StateName {
    PlacementState state;
    std::string_view name;
};

constexpr std::array<StateName, 9> kStateNames{{
    {PlacementState::Reserved, "RESERVED"},
    {PlacementState::Assigned, "ASSIGNED"},
    {PlacementState::Running, "RUNNING"},
    {PlacementState::Completed, "COMPLETED"},
    {PlacementState::Failed, "FAILED"},
    {PlacementState::Cancelled, "CANCELLED"},
    {PlacementState::Preempted, "PREEMPTED"},
    {PlacementState::ReassignmentRequired, "REASSIGNMENT_REQUIRED"},
    {PlacementState::Lost, "LOST"},
}};

struct ReservationStateName {
    ReservationState state;
    std::string_view name;
};

constexpr std::array<ReservationStateName, 3> kReservationStateNames{{
    {ReservationState::Active, "ACTIVE"},
    {ReservationState::Released, "RELEASED"},
    {ReservationState::Revoked, "REVOKED"},
}};

}  // namespace

std::string_view placement_state_name(PlacementState state) noexcept {
    for (const auto& entry : kStateNames) {
        if (entry.state == state) {
            return entry.name;
        }
    }
    return "UNKNOWN";
}

std::optional<PlacementState> parse_placement_state(std::string_view text) noexcept {
    for (const auto& entry : kStateNames) {
        if (entry.name == text) {
            return entry.state;
        }
    }
    return std::nullopt;
}

bool placement_state_is_terminal(PlacementState state) noexcept {
    switch (state) {
        case PlacementState::Completed:
        case PlacementState::Failed:
        case PlacementState::Cancelled:
        case PlacementState::Preempted:
        case PlacementState::ReassignmentRequired:
        case PlacementState::Lost:
            return true;
        case PlacementState::Reserved:
        case PlacementState::Assigned:
        case PlacementState::Running:
            return false;
    }
    return true;
}

bool placement_state_is_current(PlacementState state) noexcept {
    return !placement_state_is_terminal(state);
}

std::string_view reservation_state_name(ReservationState state) noexcept {
    for (const auto& entry : kReservationStateNames) {
        if (entry.state == state) {
            return entry.name;
        }
    }
    return "UNKNOWN";
}

std::optional<ReservationState> parse_reservation_state(std::string_view text) noexcept {
    for (const auto& entry : kReservationStateNames) {
        if (entry.name == text) {
            return entry.state;
        }
    }
    return std::nullopt;
}

}  // namespace lab_scheduler
