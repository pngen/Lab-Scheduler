#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "lab_scheduler/error.hpp"
#include "lab_scheduler/limits.hpp"
#include "lab_scheduler/placement.hpp"
#include "lab_scheduler/request.hpp"
#include "lab_scheduler/resource.hpp"

namespace lab_scheduler {

// Durable scheduler state. Only durable facts are persisted: logical resource
// identities with their static capability records, placement history,
// scheduling requests, reservations, policy, and the coordinator epoch.
// Dynamic availability is deliberately *not* authoritative after a load.
struct DurableState {
    std::uint32_t format_version = 1;
    CoordinatorEpoch epoch{};
    Limits limits{};
    RankingWeights default_weights{};
    std::uint64_t id_counter = 0;
    std::vector<ResourceRecord> resources{};
    std::vector<StoredRequest> requests{};
    std::vector<PlacementRecord> placements{};
    std::vector<ReservationRecord> reservations{};
    std::string integrity_note{};
};

Status validate_durable_state(const DurableState& state);

}  // namespace lab_scheduler
