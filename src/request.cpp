#include "lab_scheduler/request.hpp"

#include <algorithm>
#include <array>
#include <set>

#include "lab_scheduler/canonical.hpp"

namespace lab_scheduler {
namespace {

struct FactorName {
    RankingFactor factor;
    std::string_view name;
};

constexpr std::array<FactorName, kRankingFactorCount> kFactorNames{{
    {RankingFactor::Locality, "locality"},
    {RankingFactor::Occupancy, "occupancy"},
    {RankingFactor::ResidualCapacity, "residual_capacity"},
    {RankingFactor::AcceleratorFit, "accelerator_fit"},
    {RankingFactor::ModelWarmth, "model_warmth"},
    {RankingFactor::DatasetLocality, "dataset_locality"},
    {RankingFactor::SimulatorReadiness, "simulator_readiness"},
    {RankingFactor::TopologyDistance, "topology_distance"},
    {RankingFactor::Fragmentation, "fragmentation"},
    {RankingFactor::Reuse, "reuse"},
    {RankingFactor::Fairness, "fairness"},
    {RankingFactor::Priority, "priority"},
}};

struct RequestStateName {
    RequestState state;
    std::string_view name;
};

constexpr std::array<RequestStateName, 6> kRequestStateNames{{
    {RequestState::Submitted, "SUBMITTED"},
    {RequestState::Placed, "PLACED"},
    {RequestState::Completed, "COMPLETED"},
    {RequestState::Failed, "FAILED"},
    {RequestState::Cancelled, "CANCELLED"},
    {RequestState::NoPlacement, "NO_PLACEMENT"},
}};

Status check_text(const std::string& value, std::string_view field, std::uint32_t maximum) {
    if (value.size() > maximum) {
        return Status(ErrorCode::InvalidArgument, std::string(field) + " exceeds the configured maximum length");
    }
    return Status{};
}

template <class Id>
Status check_sorted_unique(std::vector<Id> values, std::string_view field) {
    std::sort(values.begin(), values.end());
    for (std::size_t i = 1; i < values.size(); ++i) {
        if (values[i] == values[i - 1]) {
            return Status(ErrorCode::DuplicateIdentity,
                          std::string(field) + " contains a duplicate identity: " + values[i].to_string());
        }
    }
    return Status{};
}

}  // namespace

std::string_view ranking_factor_name(RankingFactor factor) noexcept {
    for (const auto& entry : kFactorNames) {
        if (entry.factor == factor) {
            return entry.name;
        }
    }
    return "unknown";
}

std::optional<RankingFactor> parse_ranking_factor(std::string_view text) noexcept {
    for (const auto& entry : kFactorNames) {
        if (entry.name == text) {
            return entry.factor;
        }
    }
    return std::nullopt;
}

std::string_view request_state_name(RequestState state) noexcept {
    for (const auto& entry : kRequestStateNames) {
        if (entry.state == state) {
            return entry.name;
        }
    }
    return "UNKNOWN";
}

std::optional<RequestState> parse_request_state(std::string_view text) noexcept {
    for (const auto& entry : kRequestStateNames) {
        if (entry.name == text) {
            return entry.state;
        }
    }
    return std::nullopt;
}

Status validate_schedule_request(const ScheduleRequest& request, const Limits& limits) {
    Status status = request.id.validate();
    if (!status.ok()) {
        return status;
    }
    status = request.experiment.validate();
    if (!status.ok()) {
        return status;
    }
    status = request.experiment_generation.validate();
    if (!status.ok()) {
        return status;
    }
    if (!request.trial.is_null()) {
        status = request.trial.validate();
        if (!status.ok()) {
            return status;
        }
    }
    status = check_text(request.workload, "workload", limits.max_string_length);
    if (!status.ok()) {
        return status;
    }
    status = check_text(request.tenant, "tenant", limits.max_string_length);
    if (!status.ok()) {
        return status;
    }
    status = check_text(request.caller_authority, "caller authority", limits.max_string_length);
    if (!status.ok()) {
        return status;
    }
    status = check_text(request.request_provenance, "request provenance", limits.max_string_length);
    if (!status.ok()) {
        return status;
    }
    if (request.priority < -1000 || request.priority > 1000) {
        return Status(ErrorCode::InvalidArgument, "priority must be within [-1000, 1000]");
    }
    if (request.requirements.empty()) {
        return Status(ErrorCode::InvalidArgument, "a scheduling request must declare at least one requirement");
    }
    if (request.requirements.size() > limits.max_requirements_per_request) {
        return Status(ErrorCode::LimitExceeded, "requirement count exceeds the configured maximum");
    }
    if (request.max_reassignments > 16) {
        return Status(ErrorCode::InvalidArgument, "max_reassignments must not exceed 16");
    }
    for (const RankingFactor& factor : {RankingFactor::Locality, RankingFactor::Occupancy,
                                        RankingFactor::ResidualCapacity, RankingFactor::AcceleratorFit,
                                        RankingFactor::ModelWarmth, RankingFactor::DatasetLocality,
                                        RankingFactor::SimulatorReadiness, RankingFactor::TopologyDistance,
                                        RankingFactor::Fragmentation, RankingFactor::Reuse,
                                        RankingFactor::Fairness, RankingFactor::Priority}) {
        static_cast<void>(factor);
    }
    for (std::size_t i = 0; i < kRankingFactorCount; ++i) {
        const std::int32_t weight = request.weights.values[i];
        if (weight < -1000 || weight > 1000) {
            return Status(ErrorCode::InvalidArgument, "ranking weights must be within [-1000, 1000]");
        }
    }

    std::set<std::string> requirement_names;
    std::set<std::uint32_t> affinity_groups;
    for (const ResourceRequirement& requirement : request.requirements) {
        status = check_text(requirement.name, "requirement name", limits.max_string_length);
        if (!status.ok()) {
            return status;
        }
        if (requirement.name.empty()) {
            return Status(ErrorCode::InvalidArgument, "every requirement must carry a name");
        }
        if (!requirement_names.insert(requirement.name).second) {
            return Status(ErrorCode::DuplicateIdentity, "duplicate requirement name: " + requirement.name);
        }
        status = requirement.required_capabilities.validate(limits);
        if (!status.ok()) {
            return status;
        }
        if (requirement.min_slots == 0) {
            return Status(ErrorCode::InvalidArgument,
                          "requirement '" + requirement.name + "' must request at least one slot");
        }
        if (requirement.min_slots > 0xffffu) {
            return Status(ErrorCode::InvalidArgument, "requirement slot count is implausible");
        }
        if (requirement.max_occupancy_percent == 0 || requirement.max_occupancy_percent > 100) {
            return Status(ErrorCode::InvalidArgument,
                          "max_occupancy_percent must be within [1, 100] for requirement '" + requirement.name +
                              "'");
        }
        if (requirement.allowlist.size() > limits.max_allowlist_entries ||
            requirement.denylist.size() > limits.max_denylist_entries) {
            return Status(ErrorCode::LimitExceeded, "allowlist or denylist exceeds the configured maximum");
        }
        status = check_sorted_unique(requirement.allowlist, "allowlist");
        if (!status.ok()) {
            return status;
        }
        status = check_sorted_unique(requirement.denylist, "denylist");
        if (!status.ok()) {
            return status;
        }
        for (const ResourceId& allowed : requirement.allowlist) {
            status = allowed.validate();
            if (!status.ok()) {
                return status;
            }
        }
        for (const ResourceId& denied : requirement.denylist) {
            status = denied.validate();
            if (!status.ok()) {
                return status;
            }
        }
        const std::optional<ModelResourceId> model = requirement.model;
        if (model.has_value()) {
            status = model->validate();
            if (!status.ok()) {
                return status;
            }
        }
        const std::optional<SimulatorId> simulator = requirement.simulator;
        if (simulator.has_value()) {
            status = simulator->validate();
            if (!status.ok()) {
                return status;
            }
        }
        const std::optional<DatasetId> dataset = requirement.dataset;
        if (dataset.has_value()) {
            status = dataset->validate();
            if (!status.ok()) {
                return status;
            }
        }
        const std::optional<DatasetVersionId> dataset_version = requirement.dataset_version;
        if (dataset_version.has_value()) {
            status = dataset_version->validate();
            if (!status.ok()) {
                return status;
            }
        }
        const std::optional<EnvironmentId> environment = requirement.environment;
        if (environment.has_value()) {
            status = environment->validate();
            if (!status.ok()) {
                return status;
            }
        }
        const std::optional<AcceleratorId> accelerator = requirement.accelerator;
        if (accelerator.has_value()) {
            status = accelerator->validate();
            if (!status.ok()) {
                return status;
            }
        }
        const std::optional<PhysicalEnvironmentId> physical = requirement.physical_environment;
        if (physical.has_value()) {
            status = physical->validate();
            if (!status.ok()) {
                return status;
            }
        }
        const std::optional<VirtualEnvironmentId> virtual_environment = requirement.virtual_environment;
        if (virtual_environment.has_value()) {
            status = virtual_environment->validate();
            if (!status.ok()) {
                return status;
            }
        }
        status = check_text(requirement.model_version, "model version", limits.max_digest_length);
        if (!status.ok()) {
            return status;
        }
        status = check_text(requirement.simulator_version, "simulator version", limits.max_digest_length);
        if (!status.ok()) {
            return status;
        }
        status = check_text(requirement.simulator_scenario, "simulator scenario", limits.max_string_length);
        if (!status.ok()) {
            return status;
        }
        status = check_text(requirement.environment_digest, "environment digest", limits.max_digest_length);
        if (!status.ok()) {
            return status;
        }
        const std::optional<TopologyDomainId> required_domain = requirement.required_domain;
        if (required_domain.has_value()) {
            status = required_domain->validate();
            if (!status.ok()) {
                return status;
            }
        }
        if (requirement.affinity_group > 0) {
            if (requirement.affinity_group > limits.max_affinity_groups) {
                return Status(ErrorCode::LimitExceeded, "affinity group index exceeds the configured maximum");
            }
            affinity_groups.insert(requirement.affinity_group);
        }
    }

    if (request.affinity.size() > limits.max_anti_affinity_pairs ||
        request.anti_affinity.size() > limits.max_anti_affinity_pairs) {
        return Status(ErrorCode::LimitExceeded, "affinity constraint count exceeds the configured maximum");
    }
    for (const AffinityConstraint& constraint : request.affinity) {
        if (constraint.group_a == 0 || constraint.group_b == 0) {
            return Status(ErrorCode::InvalidArgument, "affinity constraints must reference non-zero groups");
        }
        if (affinity_groups.find(constraint.group_a) == affinity_groups.end() ||
            affinity_groups.find(constraint.group_b) == affinity_groups.end()) {
            return Status(ErrorCode::InvalidArgument,
                          "affinity constraint references a group that no requirement declares");
        }
    }
    for (const AntiAffinityConstraint& constraint : request.anti_affinity) {
        if (constraint.group_a == 0 || constraint.group_b == 0) {
            return Status(ErrorCode::InvalidArgument,
                          "anti-affinity constraints must reference non-zero groups");
        }
        if (affinity_groups.find(constraint.group_a) == affinity_groups.end() ||
            affinity_groups.find(constraint.group_b) == affinity_groups.end()) {
            return Status(ErrorCode::InvalidArgument,
                          "anti-affinity constraint references a group that no requirement declares");
        }
    }
    if (request.exclusive) {
        for (const ResourceRequirement& requirement : request.requirements) {
            if (requirement.min_slots == 0) {
                return Status(ErrorCode::InvalidArgument, "exclusive requests must claim at least one slot");
            }
        }
    }
    return Status{};
}

void normalize_schedule_request(ScheduleRequest& request) {
    for (ResourceRequirement& requirement : request.requirements) {
        std::sort(requirement.allowlist.begin(), requirement.allowlist.end());
        std::sort(requirement.denylist.begin(), requirement.denylist.end());
    }
}

std::string describe_requirement(const ResourceRequirement& requirement) {
    std::string out = requirement.name;
    out += " [";
    out += resource_class_name(requirement.resource_class);
    out += " slots=";
    out += decimal_u64(requirement.min_slots);
    if (requirement.min_memory_bytes != 0) {
        out += " mem=";
        out += decimal_u64(requirement.min_memory_bytes);
    }
    if (requirement.require_exclusive) {
        out += " exclusive";
    }
    if (!requirement.required_capabilities.empty()) {
        out += " caps=";
        const std::vector<std::string> names = requirement.required_capabilities.names();
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (i != 0) {
                out += "|";
            }
            out += names[i];
        }
    }
    out += "]";
    return out;
}

}  // namespace lab_scheduler
