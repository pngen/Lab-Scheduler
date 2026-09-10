#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lab_scheduler/capability.hpp"
#include "lab_scheduler/error.hpp"
#include "lab_scheduler/identity.hpp"
#include "lab_scheduler/limits.hpp"
#include "lab_scheduler/resource.hpp"
#include "lab_scheduler/topology.hpp"

namespace lab_scheduler {

// One placement may satisfy several requirements of different resource classes
// at once. The scheduler never picks each required resource independently,
// because independently valid choices can still compose into an invalid
// placement.
struct ResourceRequirement {
    std::string name{};  // label used in explanations
    ResourceClass resource_class = ResourceClass::Worker;
    CapabilitySet required_capabilities{};
    std::uint32_t min_slots = 1;
    std::uint64_t min_memory_bytes = 0;
    bool require_exclusive = false;
    bool require_current_evidence = true;
    HealthState minimum_health = HealthState::Healthy;
    std::uint32_t max_occupancy_percent = 100;

    std::optional<AcceleratorId> accelerator{};
    std::optional<ModelResourceId> model{};
    std::string model_version{};
    bool require_model_resident = false;
    std::optional<SimulatorId> simulator{};
    std::string simulator_version{};
    std::string simulator_scenario{};
    bool require_simulator_ready = true;
    std::optional<DatasetId> dataset{};
    std::optional<DatasetVersionId> dataset_version{};
    std::optional<EnvironmentId> environment{};
    std::string environment_digest{};
    std::optional<PhysicalEnvironmentId> physical_environment{};
    std::optional<VirtualEnvironmentId> virtual_environment{};

    // A requirement may pin the resource to a specific topology domain. A
    // domain that is not supplied by the advertiser is UNKNOWN and never
    // satisfies this constraint.
    std::optional<TopologyDomainId> required_domain{};
    TopologyScope required_domain_scope = TopologyScope::Host;

    std::vector<ResourceId> allowlist{};  // empty means "no allowlist restriction"
    std::vector<ResourceId> denylist{};
    std::uint32_t affinity_group = 0;  // 0 means "not part of an affinity group"
};

struct AffinityConstraint {
    std::uint32_t group_a = 0;
    std::uint32_t group_b = 0;
    TopologyScope scope = TopologyScope::Host;
};

struct AntiAffinityConstraint {
    std::uint32_t group_a = 0;
    std::uint32_t group_b = 0;
    TopologyScope scope = TopologyScope::Host;
};

// Named ranking factors. Placement never reduces to one opaque score: the
// explanation reports every factor, its weight, and its weighted contribution.
enum class RankingFactor : std::uint8_t {
    Locality = 0,
    Occupancy = 1,
    ResidualCapacity = 2,
    AcceleratorFit = 3,
    ModelWarmth = 4,
    DatasetLocality = 5,
    SimulatorReadiness = 6,
    TopologyDistance = 7,
    Fragmentation = 8,
    Reuse = 9,
    Fairness = 10,
    Priority = 11,
};

inline constexpr std::size_t kRankingFactorCount = 12;

std::string_view ranking_factor_name(RankingFactor factor) noexcept;
std::optional<RankingFactor> parse_ranking_factor(std::string_view text) noexcept;

// Weights are per-mille and may be negative (a negative weight makes a factor
// act as a penalty).
struct RankingWeights {
    std::int32_t values[kRankingFactorCount] = {100, 100, 60, 80, 80, 90, 70, 50, 40, 30, 20, 0};

    [[nodiscard]] std::int32_t weight(RankingFactor factor) const noexcept {
        return values[static_cast<std::size_t>(factor)];
    }
    void set_weight(RankingFactor factor, std::int32_t value) noexcept {
        values[static_cast<std::size_t>(factor)] = value;
    }
};

struct ScheduleRequest {
    ScheduleRequestId id{};
    ExperimentId experiment{};
    ExperimentGeneration experiment_generation{};
    TrialId trial{};  // optional, may be null
    std::string workload{};
    std::string tenant{};
    std::int32_t priority = 0;  // -1000..1000
    std::vector<ResourceRequirement> requirements{};
    std::vector<AffinityConstraint> affinity{};
    std::vector<AntiAffinityConstraint> anti_affinity{};
    // When set, no resource already reserved by another current placement of
    // the same experiment generation may be selected.
    bool experiment_anti_affinity = false;
    bool exclusive = false;
    bool reproducibility_required = false;
    bool allow_preemption = false;
    std::uint32_t max_reassignments = 3;
    RankingWeights weights{};
    std::string caller_authority{};
    std::string request_provenance{};
};

enum class RequestState : std::uint8_t {
    Submitted = 0,
    Placed = 1,
    Completed = 2,
    Failed = 3,
    Cancelled = 4,
    NoPlacement = 5,
};

std::string_view request_state_name(RequestState state) noexcept;
std::optional<RequestState> parse_request_state(std::string_view text) noexcept;

// The scheduler's own record for a submitted request. Cancellation and
// placement history are properties of the scheduler's view, not of the
// caller supplied request value.
struct StoredRequest {
    ScheduleRequest request{};
    RequestState state = RequestState::Submitted;
    std::uint64_t submitted_sequence = 0;
    PlacementId current_placement{};
    std::uint32_t placement_count = 0;
    std::string detail{};
};

Status validate_schedule_request(const ScheduleRequest& request, const Limits& limits);
void normalize_schedule_request(ScheduleRequest& request);

// Human readable requirement summary used by the CLI and explanations.
std::string describe_requirement(const ResourceRequirement& requirement);

}  // namespace lab_scheduler
