#include "engine_internal.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "lab_scheduler/canonical.hpp"

namespace lab_scheduler {
namespace {

constexpr std::int64_t kUnit = 1000000;  // 1.0 in fixed point

enum class FactorScope : std::uint8_t { Additive, Cross };

// Order matches the RankingFactor enumeration.
constexpr FactorScope kFactorScope[kRankingFactorCount] = {
    FactorScope::Cross,     // Locality
    FactorScope::Additive,  // Occupancy
    FactorScope::Additive,  // ResidualCapacity
    FactorScope::Additive,  // AcceleratorFit
    FactorScope::Additive,  // ModelWarmth
    FactorScope::Cross,     // DatasetLocality
    FactorScope::Additive,  // SimulatorReadiness
    FactorScope::Cross,     // TopologyDistance
    FactorScope::Additive,  // Fragmentation
    FactorScope::Additive,  // Reuse
    FactorScope::Cross,     // Fairness
    FactorScope::Cross,     // Priority
};

std::int64_t clamp_unit(std::int64_t value) noexcept {
    if (value < 0) {
        return 0;
    }
    if (value > kUnit) {
        return kUnit;
    }
    return value;
}

int health_rank(HealthState state) noexcept {
    switch (state) {
        case HealthState::Healthy:
            return 3;
        case HealthState::Degraded:
            return 2;
        case HealthState::Unknown:
            return 1;
        case HealthState::Unhealthy:
            return 0;
    }
    return 0;
}

struct Eligibility {
    bool eligible = false;
    std::uint32_t slots = 1;
    bool exclusive = false;
    std::vector<RejectReason> reasons{};
};

void reject(Eligibility& eligibility, RejectReason reason, std::size_t maximum) {
    if (eligibility.reasons.size() < maximum) {
        eligibility.reasons.push_back(reason);
    }
}

struct CandidateResource {
    ResourceId id{};
    ResourceGeneration generation{};
    ResourceClass resource_class = ResourceClass::Worker;
    std::string name{};
    std::uint32_t slots = 1;
    bool exclusive = false;
    std::int64_t partial = 0;
    std::int64_t additive[kRankingFactorCount] = {};
};

struct RequirementPlan {
    const ResourceRequirement* requirement = nullptr;
    std::vector<CandidateResource> candidates{};
    std::uint32_t considered = 0;
    std::vector<RejectedResourceEntry> rejected{};
    std::map<RejectReason, std::uint32_t> reason_counts{};
};

std::uint64_t effective_memory_bytes(const ResourceRecord& resource) noexcept {
    std::uint64_t memory = resource.capacity.memory_bytes;
    for (const AcceleratorAttachment& attachment : resource.accelerators) {
        if (attachment.memory_bytes > memory) {
            memory = attachment.memory_bytes;
        }
    }
    return memory;
}

bool reserved_by_other_placement(const SchedulerState& state, ExperimentId experiment, ResourceId resource,
                                 ScheduleRequestId exclude_request) {
    for (const auto& entry : state.placements) {
        const PlacementRecord& placement = entry.second;
        if (placement.experiment != experiment || placement.request == exclude_request) {
            continue;
        }
        if (!placement_state_is_live(placement.state)) {
            continue;
        }
        const auto reservation = state.reservations.find(placement.reservation);
        if (reservation == state.reservations.end() || reservation->second.state != ReservationState::Active) {
            continue;
        }
        for (const ReservationClaim& claim : reservation->second.claims) {
            if (claim.resource == resource) {
                return true;
            }
        }
    }
    return false;
}

std::optional<RejectReason> class_specific_reason(const ResourceRecord& resource,
                                                  const ResourceRequirement& requirement) {
    switch (resource.resource_class) {
        case ResourceClass::Model: {
            if (!resource.model.has_value()) {
                return RejectReason::ModelMismatch;
            }
            if (requirement.model.has_value() && !(resource.model->id == *requirement.model)) {
                return RejectReason::ModelMismatch;
            }
            if (!requirement.model_version.empty() && resource.model->version != requirement.model_version) {
                return RejectReason::ModelVersionMismatch;
            }
            if (requirement.require_model_resident && !resource.model->resident) {
                return RejectReason::ModelNotResident;
            }
            return std::nullopt;
        }
        case ResourceClass::Gpu: {
            if (requirement.accelerator.has_value()) {
                const auto found = std::find_if(resource.accelerators.begin(), resource.accelerators.end(),
                                                [&](const AcceleratorAttachment& attachment) {
                                                    return attachment.id == *requirement.accelerator;
                                                });
                if (found == resource.accelerators.end()) {
                    return RejectReason::AcceleratorMismatch;
                }
            }
            return std::nullopt;
        }
        case ResourceClass::Simulator: {
            if (!resource.simulator.has_value()) {
                return RejectReason::SimulatorMismatch;
            }
            if (requirement.simulator.has_value() && !(resource.simulator->id == *requirement.simulator)) {
                return RejectReason::SimulatorMismatch;
            }
            if (!requirement.simulator_version.empty() &&
                resource.simulator->version != requirement.simulator_version) {
                return RejectReason::SimulatorMismatch;
            }
            if (!requirement.simulator_scenario.empty()) {
                const auto found = std::find(resource.simulator->scenarios.begin(),
                                             resource.simulator->scenarios.end(),
                                             requirement.simulator_scenario);
                if (found == resource.simulator->scenarios.end()) {
                    return RejectReason::Unsupported;
                }
            }
            if (requirement.require_simulator_ready && resource.readiness != ReadinessState::Ready) {
                return RejectReason::SimulatorNotReady;
            }
            return std::nullopt;
        }
        case ResourceClass::Dataset: {
            if (!resource.dataset.has_value()) {
                return RejectReason::DatasetMismatch;
            }
            if (requirement.dataset.has_value() && !(resource.dataset->id == *requirement.dataset)) {
                return RejectReason::DatasetMismatch;
            }
            if (requirement.dataset_version.has_value() &&
                !(resource.dataset->version == *requirement.dataset_version)) {
                return RejectReason::DatasetVersionMismatch;
            }
            return std::nullopt;
        }
        case ResourceClass::PhysicalEnvironment: {
            if (!resource.environment.has_value()) {
                return RejectReason::EnvironmentMismatch;
            }
            if (requirement.environment.has_value() && !(resource.environment->id == *requirement.environment)) {
                return RejectReason::EnvironmentMismatch;
            }
            if (requirement.physical_environment.has_value() &&
                !(resource.environment->physical_environment == *requirement.physical_environment)) {
                return RejectReason::EnvironmentMismatch;
            }
            if (requirement.require_current_evidence && resource.readiness != ReadinessState::Ready) {
                return RejectReason::EnvironmentNotReady;
            }
            return std::nullopt;
        }
        case ResourceClass::VirtualEnvironment: {
            if (!resource.environment.has_value()) {
                return RejectReason::EnvironmentMismatch;
            }
            if (requirement.environment.has_value() && !(resource.environment->id == *requirement.environment)) {
                return RejectReason::EnvironmentMismatch;
            }
            if (requirement.virtual_environment.has_value() &&
                !(resource.environment->virtual_environment == *requirement.virtual_environment)) {
                return RejectReason::EnvironmentMismatch;
            }
            if (!requirement.environment_digest.empty() &&
                resource.environment->image_digest != requirement.environment_digest) {
                return RejectReason::EnvironmentDigestMismatch;
            }
            if (requirement.require_current_evidence && resource.readiness != ReadinessState::Ready) {
                return RejectReason::EnvironmentNotReady;
            }
            return std::nullopt;
        }
        case ResourceClass::Worker:
            return std::nullopt;
    }
    return std::nullopt;
}

Eligibility evaluate_resource(const ScheduleRequest& request,
                              const ResourceRequirement& requirement, const ResourceRecord& resource,
                              const Limits& limits) {
    Eligibility result;
    const std::size_t maximum = limits.max_rejection_reasons_per_entry;

    if (resource.resource_class != requirement.resource_class) {
        reject(result, RejectReason::ResourceClassMismatch, maximum);
        return result;
    }
    if (!requirement.allowlist.empty() &&
        std::find(requirement.allowlist.begin(), requirement.allowlist.end(), resource.id) ==
            requirement.allowlist.end()) {
        reject(result, RejectReason::AllowlistExcluded, maximum);
    }
    if (std::find(requirement.denylist.begin(), requirement.denylist.end(), resource.id) !=
        requirement.denylist.end()) {
        reject(result, RejectReason::DenylistExcluded, maximum);
    }
    if (resource.lifecycle == ResourceLifecycle::Retired) {
        reject(result, RejectReason::ResourceRetired, maximum);
    }
    if (resource.lifecycle == ResourceLifecycle::Lost) {
        reject(result, RejectReason::WorkerIncarnationStale, maximum);
    }
    if (requirement.require_current_evidence && !resource.dynamic_authoritative) {
        reject(result,
               resource.lifecycle == ResourceLifecycle::Registered ? RejectReason::EvidenceStale
                                                                   : RejectReason::EvidenceNotAuthoritative,
               maximum);
    }
    if (!request.tenant.empty() && !resource.tenant.empty() && request.tenant != resource.tenant) {
        reject(result, RejectReason::TenantViolation, maximum);
    }
    if (!resource.capabilities.contains_all(requirement.required_capabilities)) {
        reject(result, RejectReason::CapabilityMismatch, maximum);
    }
    if (health_rank(resource.health) < health_rank(requirement.minimum_health)) {
        reject(result,
               resource.health == HealthState::Unknown ? RejectReason::HealthUnknown : RejectReason::HealthRejected,
               maximum);
    }
    if (resource.readiness == ReadinessState::Unsupported && requirement.require_current_evidence) {
        reject(result, RejectReason::Unsupported, maximum);
    }
    if (requirement.required_domain.has_value()) {
        const TopologyDomain& domain = resource.topology.domain(requirement.required_domain_scope);
        if (!(domain.id == *requirement.required_domain)) {
            reject(result, RejectReason::TopologyMismatch, maximum);
        }
    }

    const std::optional<RejectReason> specific = class_specific_reason(resource, requirement);
    if (specific.has_value()) {
        reject(result, *specific, maximum);
    }

    const std::uint32_t available = available_slots(resource);
    const bool exclusive_requested = requirement.require_exclusive || request.exclusive;
    const bool exclusive = exclusive_requested || resource.sharing == SharingMode::Exclusive;
    const std::uint32_t slots = exclusive ? resource.capacity.slots : requirement.min_slots;

    if (exclusive && resource.total_occupancy() != 0) {
        reject(result, RejectReason::ExclusivityConflict, maximum);
    }
    if (available < slots) {
        reject(result, RejectReason::CapacityExhausted, maximum);
    }
    if (requirement.min_memory_bytes > effective_memory_bytes(resource)) {
        reject(result, RejectReason::MemoryExhausted, maximum);
    }
    const std::uint64_t occupancy_after =
        static_cast<std::uint64_t>(resource.total_occupancy()) + static_cast<std::uint64_t>(slots);
    if (occupancy_after * 100ull >
        static_cast<std::uint64_t>(resource.capacity.slots) * requirement.max_occupancy_percent) {
        reject(result, RejectReason::OccupancyExceeded, maximum);
    }

    if (!result.reasons.empty()) {
        return result;
    }
    result.eligible = true;
    result.slots = slots;
    result.exclusive = exclusive;
    return result;
}

std::size_t count_live_placements(const SchedulerState& state) {
    std::size_t count = 0;
    for (const auto& entry : state.placements) {
        if (placement_state_is_live(entry.second.state)) {
            ++count;
        }
    }
    return count;
}

std::size_t count_active_reservations(const SchedulerState& state) {
    std::size_t count = 0;
    for (const auto& entry : state.reservations) {
        if (entry.second.state == ReservationState::Active) {
            ++count;
        }
    }
    return count;
}

void fill_requirement_explanations(const std::vector<RequirementPlan>& plans, Explanation& explanation) {
    explanation.requirements.clear();
    for (const RequirementPlan& plan : plans) {
        RequirementExplanation entry;
        entry.requirement_name = plan.requirement->name;
        entry.resource_class = plan.requirement->resource_class;
        entry.considered = plan.considered;
        entry.eligible = static_cast<std::uint32_t>(plan.candidates.size());
        entry.rejected = plan.rejected;
        explanation.requirements.push_back(std::move(entry));
    }
}

Decision make_no_placement_decision(Decision decision, Explanation explanation, ErrorCode code,
                                    std::string detail) {
    decision.outcome = DecisionOutcome::NoPlacement;
    decision.failure = code;
    decision.failure_detail = std::move(detail);
    explanation.outcome = DecisionOutcome::NoPlacement;
    explanation.failure = decision.failure;
    explanation.failure_detail = decision.failure_detail;
    decision.explanation = std::move(explanation);
    return decision;
}

void compute_additive_values(const SchedulerState& state, const ScheduleRequest& request,
                             const ResourceRequirement& requirement, const ResourceRecord& resource,
                             const Eligibility& eligibility, std::int64_t out[kRankingFactorCount]) {
    for (std::size_t i = 0; i < kRankingFactorCount; ++i) {
        out[i] = kUnit;
    }
    const std::uint32_t total_after = resource.total_occupancy() + eligibility.slots;

    out[static_cast<std::size_t>(RankingFactor::Occupancy)] =
        clamp_unit(kUnit - (static_cast<std::int64_t>(total_after) * kUnit) /
                              static_cast<std::int64_t>(resource.capacity.slots));

    const std::uint32_t residual =
        resource.capacity.slots > total_after ? resource.capacity.slots - total_after : 0;
    out[static_cast<std::size_t>(RankingFactor::ResidualCapacity)] =
        clamp_unit((static_cast<std::int64_t>(residual) * kUnit) /
                   static_cast<std::int64_t>(resource.capacity.slots));

    std::int64_t accelerator_fit = kUnit;
    if (resource.resource_class == ResourceClass::Gpu && requirement.min_memory_bytes > 0) {
        std::uint64_t memory = 0;
        for (const AcceleratorAttachment& attachment : resource.accelerators) {
            if (attachment.memory_bytes > memory) {
                memory = attachment.memory_bytes;
            }
        }
        if (memory >= requirement.min_memory_bytes && memory > 0) {
            const std::int64_t excess = static_cast<std::int64_t>(
                ((memory - requirement.min_memory_bytes) * static_cast<std::uint64_t>(kUnit)) / memory);
            accelerator_fit = clamp_unit(kUnit - excess);
        }
    }
    out[static_cast<std::size_t>(RankingFactor::AcceleratorFit)] = accelerator_fit;

    std::int64_t model_warmth = kUnit;
    if (resource.resource_class == ResourceClass::Model) {
        if (resource.model.has_value() && resource.model->resident) {
            model_warmth = kUnit;
        } else if (resource.readiness == ReadinessState::Ready) {
            model_warmth = kUnit / 2;
        } else {
            model_warmth = 0;
        }
    }
    out[static_cast<std::size_t>(RankingFactor::ModelWarmth)] = model_warmth;

    std::int64_t simulator_readiness = kUnit;
    if (resource.resource_class == ResourceClass::Simulator) {
        switch (resource.readiness) {
            case ReadinessState::Ready:
                simulator_readiness = kUnit;
                break;
            case ReadinessState::NotReady:
                simulator_readiness = 0;
                break;
            case ReadinessState::Unknown:
            case ReadinessState::Unsupported:
                simulator_readiness = kUnit / 4;
                break;
        }
    }
    out[static_cast<std::size_t>(RankingFactor::SimulatorReadiness)] = simulator_readiness;

    std::int64_t fragmentation = kUnit;
    if (!eligibility.exclusive) {
        const std::uint32_t residual_after =
            resource.capacity.slots > total_after ? resource.capacity.slots - total_after : 0;
        if (residual_after > 0 && residual_after < eligibility.slots) {
            fragmentation = 0;
        }
    }
    out[static_cast<std::size_t>(RankingFactor::Fragmentation)] = fragmentation;

    const bool reused = reserved_by_other_placement(state, request.experiment, resource.id, request.id);
    out[static_cast<std::size_t>(RankingFactor::Reuse)] = reused ? kUnit : 0;
}

bool domains_are_equal(const ResourceRecord& a, const ResourceRecord& b, TopologyScope scope) noexcept {
    return topology_domains_equal(a.topology.domain(scope), b.topology.domain(scope));
}

bool domains_are_disjoint(const ResourceRecord& a, const ResourceRecord& b, TopologyScope scope) noexcept {
    const TopologyDomain& left = a.topology.domain(scope);
    const TopologyDomain& right = b.topology.domain(scope);
    if (left.id.is_null() || right.id.is_null()) {
        return false;  // separation cannot be proven from UNKNOWN evidence
    }
    return !(left.id == right.id);
}

// Two eligible resources are interchangeable when they score identically on
// every factor and carry the same topology, ownership and claim shape. Only
// the identity that wins the deterministic tie-break can ever be selected, so
// exploring both would multiply the candidate space by the number of identical
// resources without changing the outcome.
std::string interchangeability_key(const CandidateResource& candidate, const ResourceRecord& resource) {
    std::string key;
    for (std::size_t f = 0; f < kRankingFactorCount; ++f) {
        key += decimal_i64(candidate.additive[f]);
        key.push_back(':');
    }
    key += decimal_u64(static_cast<std::uint64_t>(candidate.slots));
    key.push_back(':');
    key += candidate.exclusive ? "1" : "0";
    key.push_back(':');
    key += decimal_u64(resource.topology.host.id.value());
    key.push_back(':');
    key += decimal_u64(resource.topology.rack.id.value());
    key.push_back(':');
    key += decimal_u64(resource.topology.cluster.id.value());
    key.push_back(':');
    key += decimal_u64(resource.worker.value());
    key.push_back(':');
    key += decimal_u64(resource.boot.value());
    key.push_back(':');
    key += decimal_u64(resource.generation.value());
    return key;
}

std::string canonical_resource_key(const std::vector<ResourceId>& ids) {
    std::vector<std::string> parts;
    parts.reserve(ids.size());
    for (const ResourceId& id : ids) {
        parts.push_back(id.to_string());
    }
    std::sort(parts.begin(), parts.end());
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            out.push_back(',');
        }
        out += parts[i];
    }
    return out;
}

struct RankingCandidate {
    std::vector<const CandidateResource*> selection{};
    std::vector<ResourceId> resources{};
    std::int64_t raw = 0;
    std::vector<RankingFactorValue> factors{};
    std::string tie_break{};
};

std::int64_t score_candidate(const SchedulerState& state, const ScheduleRequest& request,
                             const std::vector<const CandidateResource*>& selection,
                             const std::vector<const ResourceRecord*>& records,
                             std::vector<RankingFactorValue>& factors) {
    const std::size_t n = selection.size();
    std::int64_t additive_totals[kRankingFactorCount] = {};
    for (const CandidateResource* candidate : selection) {
        for (std::size_t f = 0; f < kRankingFactorCount; ++f) {
            additive_totals[f] += candidate->additive[f];
        }
    }

    std::int64_t cross[kRankingFactorCount] = {};

    std::map<TopologyDomainId, std::uint32_t> hosts;
    std::map<TopologyDomainId, std::uint32_t> racks;
    std::map<TopologyDomainId, std::uint32_t> clusters;
    std::uint32_t unknown_hosts = 0;
    std::uint32_t unknown_racks = 0;
    std::uint32_t unknown_clusters = 0;
    std::uint32_t dominant_host_share = 0;
    for (const ResourceRecord* record : records) {
        const TopologyDomainId host = record->topology.host.id;
        if (host.is_null()) {
            ++unknown_hosts;
            dominant_host_share = std::max(dominant_host_share, 1u);
        } else {
            const std::uint32_t share = ++hosts[host];
            dominant_host_share = std::max(dominant_host_share, share);
        }
        if (record->topology.rack.id.is_null()) {
            ++unknown_racks;
        } else {
            ++racks[record->topology.rack.id];
        }
        if (record->topology.cluster.id.is_null()) {
            ++unknown_clusters;
        } else {
            ++clusters[record->topology.cluster.id];
        }
    }
    cross[static_cast<std::size_t>(RankingFactor::Locality)] =
        n > 1 ? clamp_unit((static_cast<std::int64_t>(dominant_host_share - 1) * kUnit) /
                           static_cast<std::int64_t>(n - 1))
              : kUnit;

    const std::int64_t host_count = static_cast<std::int64_t>(hosts.size() + unknown_hosts);
    const std::int64_t rack_count = static_cast<std::int64_t>(racks.size() + unknown_racks);
    const std::int64_t cluster_count = static_cast<std::int64_t>(clusters.size() + unknown_clusters);
    cross[static_cast<std::size_t>(RankingFactor::TopologyDistance)] =
        clamp_unit(kUnit - (host_count - 1) * 250000 - (rack_count - 1) * 125000 -
                   (cluster_count - 1) * 62500);

    std::int64_t dataset_locality_total = 0;
    std::uint32_t dataset_requirements = 0;
    const std::vector<ResourceRequirement>& requirements = request.requirements;
    for (std::size_t i = 0; i < n && i < requirements.size(); ++i) {
        if (requirements[i].resource_class != ResourceClass::Dataset) {
            continue;
        }
        ++dataset_requirements;
        const ResourceRecord& dataset = *records[i];
        std::int64_t value = kUnit / 4;
        bool colocated = false;
        for (std::size_t j = 0; j < n; ++j) {
            if (j == i || requirements[j].resource_class == ResourceClass::Dataset) {
                continue;
            }
            if (domains_are_equal(*records[j], dataset, TopologyScope::Host)) {
                colocated = true;
                break;
            }
        }
        if (colocated) {
            value = kUnit;
        } else if (!dataset.locality.local_dataset_versions.empty()) {
            value = kUnit / 2;
        }
        dataset_locality_total += value;
    }
    cross[static_cast<std::size_t>(RankingFactor::DatasetLocality)] =
        dataset_requirements == 0 ? kUnit : dataset_locality_total / static_cast<std::int64_t>(dataset_requirements);

    std::int64_t fairness = kUnit;
    if (!request.tenant.empty()) {
        std::uint64_t served = 0;
        const auto found = state.tenant_service.find(request.tenant);
        if (found != state.tenant_service.end()) {
            served = found->second;
        }
        std::uint64_t maximum = 1;
        for (const auto& entry : state.tenant_service) {
            maximum = std::max(maximum, entry.second);
        }
        fairness = clamp_unit(kUnit - static_cast<std::int64_t>((served * static_cast<std::uint64_t>(kUnit)) /
                                                               maximum));
    }
    cross[static_cast<std::size_t>(RankingFactor::Fairness)] = fairness;
    cross[static_cast<std::size_t>(RankingFactor::Priority)] =
        clamp_unit((static_cast<std::int64_t>(request.priority) + 1000) * kUnit / 2000);

    std::int64_t raw = 0;
    for (std::size_t f = 0; f < kRankingFactorCount; ++f) {
        const std::int32_t weight = request.weights.values[f];
        if (kFactorScope[f] == FactorScope::Additive) {
            raw += additive_totals[f] * static_cast<std::int64_t>(weight);
        } else {
            raw += static_cast<std::int64_t>(n) * cross[f] * static_cast<std::int64_t>(weight);
        }
    }

    factors.clear();
    factors.reserve(kRankingFactorCount);
    for (std::size_t f = 0; f < kRankingFactorCount; ++f) {
        RankingFactorValue entry;
        entry.name = std::string(ranking_factor_name(static_cast<RankingFactor>(f)));
        entry.weight = request.weights.values[f];
        entry.value = kFactorScope[f] == FactorScope::Additive
                          ? (n == 0 ? 0 : additive_totals[f] / static_cast<std::int64_t>(n))
                          : cross[f];
        entry.weighted = (entry.value * static_cast<std::int64_t>(entry.weight)) / 1000;
        factors.push_back(std::move(entry));
    }
    return raw;
}

std::int64_t cross_bound(const ScheduleRequest& request, std::size_t n) {
    std::int64_t bound = 0;
    for (std::size_t f = 0; f < kRankingFactorCount; ++f) {
        if (kFactorScope[f] != FactorScope::Cross) {
            continue;
        }
        const std::int32_t weight = request.weights.values[f];
        if (weight > 0) {
            bound += static_cast<std::int64_t>(n) * static_cast<std::int64_t>(weight) * kUnit;
        }
    }
    return bound;
}

bool validate_selection(const SchedulerState& state, const ScheduleRequest& request,
                        const std::vector<const ResourceRecord*>& records,
                        const std::vector<const CandidateResource*>& selection, RejectReason& failure) {
    const std::size_t n = records.size();
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i + 1; j < n; ++j) {
            if (records[i]->id == records[j]->id) {
                failure = RejectReason::AlreadySelected;
                return false;
            }
        }
    }
    const std::vector<ResourceRequirement>& requirements = request.requirements;
    for (const AffinityConstraint& constraint : request.affinity) {
        for (std::size_t i = 0; i < n && i < requirements.size(); ++i) {
            if (requirements[i].affinity_group != constraint.group_a) {
                continue;
            }
            for (std::size_t j = 0; j < n && j < requirements.size(); ++j) {
                if (requirements[j].affinity_group != constraint.group_b) {
                    continue;
                }
                if (!domains_are_equal(*records[i], *records[j], constraint.scope)) {
                    failure = RejectReason::AffinityViolation;
                    return false;
                }
            }
        }
    }
    for (const AntiAffinityConstraint& constraint : request.anti_affinity) {
        for (std::size_t i = 0; i < n && i < requirements.size(); ++i) {
            if (requirements[i].affinity_group != constraint.group_a) {
                continue;
            }
            for (std::size_t j = 0; j < n && j < requirements.size(); ++j) {
                if (requirements[j].affinity_group != constraint.group_b) {
                    continue;
                }
                if (!domains_are_disjoint(*records[i], *records[j], constraint.scope)) {
                    failure = RejectReason::AntiAffinityViolation;
                    return false;
                }
            }
        }
    }
    if (request.experiment_anti_affinity) {
        for (const ResourceRecord* record : records) {
            if (reserved_by_other_placement(state, request.experiment, record->id, request.id)) {
                failure = RejectReason::AntiAffinityViolation;
                return false;
            }
        }
    }
    if (request.exclusive) {
        for (const CandidateResource* candidate : selection) {
            if (!candidate->exclusive) {
                failure = RejectReason::ExclusivityConflict;
                return false;
            }
        }
    }
    return true;
}

struct SearchOutcome {
    std::vector<RankingCandidate> candidates{};
    std::map<RejectReason, std::uint32_t> cross_rejections{};
    std::uint32_t evaluations = 0;
    bool truncated = false;
};

bool candidate_is_better(const RankingCandidate& left, const RankingCandidate& right) {
    if (left.raw != right.raw) {
        return left.raw > right.raw;
    }
    return left.tie_break < right.tie_break;
}

void search_recursive(std::size_t index, std::int64_t partial, const std::vector<std::int64_t>& remaining_max,
                      const SchedulerState& state, const ScheduleRequest& request, const Limits& limits,
                      const std::vector<RequirementPlan>& plans, std::vector<const CandidateResource*>& current,
                      SearchOutcome& outcome, std::int64_t bound, std::int64_t& best_raw) {
    if (outcome.truncated) {
        return;
    }
    if (outcome.evaluations >= limits.max_candidates_evaluated) {
        outcome.truncated = true;
        return;
    }
    if (index == plans.size()) {
        ++outcome.evaluations;
        std::vector<const ResourceRecord*> records;
        records.reserve(current.size());
        for (const CandidateResource* candidate : current) {
            const auto found = state.resources.find(candidate->id);
            if (found == state.resources.end()) {
                return;
            }
            records.push_back(&found->second);
        }
        RejectReason failure = RejectReason::None;
        if (!validate_selection(state, request, records, current, failure)) {
            outcome.cross_rejections[failure] += 1;
            return;
        }
        RankingCandidate candidate;
        candidate.selection = current;
        candidate.resources.reserve(current.size());
        for (const CandidateResource* selected : current) {
            candidate.resources.push_back(selected->id);
        }
        std::sort(candidate.resources.begin(), candidate.resources.end());
        candidate.raw = score_candidate(state, request, current, records, candidate.factors);
        candidate.tie_break = canonical_resource_key(candidate.resources);
        if (candidate.raw > best_raw) {
            best_raw = candidate.raw;
        }
        if (outcome.candidates.size() < limits.max_candidates_ranked) {
            outcome.candidates.push_back(std::move(candidate));
            return;
        }
        auto worst = outcome.candidates.begin();
        for (auto it = outcome.candidates.begin(); it != outcome.candidates.end(); ++it) {
            if (candidate_is_better(*worst, *it)) {
                worst = it;
            }
        }
        if (candidate_is_better(candidate, *worst)) {
            *worst = std::move(candidate);
        }
        return;
    }
    for (const CandidateResource& candidate : plans[index].candidates) {
        if (partial + candidate.partial + remaining_max[index + 1] + bound < best_raw) {
            break;  // candidates are ordered by descending partial score
        }
        current.push_back(&candidate);
        search_recursive(index + 1, partial + candidate.partial, remaining_max, state, request, limits, plans,
                         current, outcome, bound, best_raw);
        current.pop_back();
        if (outcome.truncated) {
            return;
        }
    }
}

ErrorCode map_reject_reason(RejectReason reason) {
    switch (reason) {
        case RejectReason::None:
        case RejectReason::ResourceClassMismatch:
        case RejectReason::AllowlistExcluded:
        case RejectReason::DenylistExcluded:
        case RejectReason::AlreadySelected:
        case RejectReason::AntiAffinityViolation:
            return ErrorCode::NoPlacement;
        case RejectReason::CapabilityMismatch:
            return ErrorCode::CapabilityMismatch;
        case RejectReason::CapacityExhausted:
        case RejectReason::MemoryExhausted:
        case RejectReason::OccupancyExceeded:
            return ErrorCode::CapacityExhausted;
        case RejectReason::HealthRejected:
        case RejectReason::HealthUnknown:
            return ErrorCode::HealthRejected;
        case RejectReason::EvidenceStale:
        case RejectReason::EvidenceNotAuthoritative:
        case RejectReason::ResourceRetired:
        case RejectReason::WorkerIncarnationStale:
            return ErrorCode::StaleResource;
        case RejectReason::TopologyMismatch:
        case RejectReason::AffinityViolation:
            return ErrorCode::TopologyMismatch;
        case RejectReason::ModelMismatch:
        case RejectReason::ModelVersionMismatch:
        case RejectReason::ModelNotResident:
            return ErrorCode::ModelMismatch;
        case RejectReason::SimulatorMismatch:
        case RejectReason::AcceleratorMismatch:
        case RejectReason::Unsupported:
            return ErrorCode::Unsupported;
        case RejectReason::SimulatorNotReady:
        case RejectReason::EnvironmentMismatch:
        case RejectReason::EnvironmentDigestMismatch:
        case RejectReason::EnvironmentNotReady:
            return ErrorCode::EnvironmentUnavailable;
        case RejectReason::DatasetMismatch:
        case RejectReason::DatasetVersionMismatch:
            return ErrorCode::DatasetMismatch;
        case RejectReason::ExclusivityConflict:
            return ErrorCode::ExclusivityConflict;
        case RejectReason::ReservationConflict:
            return ErrorCode::ReservationConflict;
        case RejectReason::TenantViolation:
            return ErrorCode::TenantViolation;
        case RejectReason::ShuttingDown:
            return ErrorCode::ShuttingDown;
    }
    return ErrorCode::NoPlacement;
}

// When several rejection reasons are equally frequent, the reported failure
// names the most informative blocking condition rather than an arbitrary one.
// A resource that matches the requirement but whose evidence is stale is a
// closer miss than a resource that could never satisfy it, and it is the one
// an operator can act on.
int reason_priority(RejectReason reason) noexcept {
    switch (reason) {
        case RejectReason::WorkerIncarnationStale:
            return 0;
        case RejectReason::EvidenceNotAuthoritative:
            return 1;
        case RejectReason::EvidenceStale:
            return 2;
        case RejectReason::ResourceRetired:
            return 3;
        case RejectReason::ExclusivityConflict:
            return 4;
        case RejectReason::ReservationConflict:
            return 5;
        case RejectReason::CapacityExhausted:
            return 6;
        case RejectReason::MemoryExhausted:
            return 7;
        case RejectReason::OccupancyExceeded:
            return 8;
        case RejectReason::HealthRejected:
            return 9;
        case RejectReason::HealthUnknown:
            return 10;
        case RejectReason::EnvironmentNotReady:
            return 12;
        case RejectReason::SimulatorNotReady:
            return 13;
        case RejectReason::TopologyMismatch:
            return 14;
        case RejectReason::AffinityViolation:
            return 15;
        case RejectReason::AntiAffinityViolation:
            return 16;
        case RejectReason::TenantViolation:
            return 17;
        case RejectReason::ModelMismatch:
        case RejectReason::ModelVersionMismatch:
        case RejectReason::ModelNotResident:
            return 18;
        case RejectReason::SimulatorMismatch:
            return 19;
        case RejectReason::DatasetMismatch:
        case RejectReason::DatasetVersionMismatch:
            return 20;
        case RejectReason::EnvironmentMismatch:
        case RejectReason::EnvironmentDigestMismatch:
            return 21;
        case RejectReason::AcceleratorMismatch:
            return 22;
        case RejectReason::CapabilityMismatch:
            return 23;
        case RejectReason::ResourceClassMismatch:
        case RejectReason::AllowlistExcluded:
        case RejectReason::DenylistExcluded:
        case RejectReason::AlreadySelected:
        case RejectReason::Unsupported:
        case RejectReason::ShuttingDown:
        case RejectReason::None:
            return 24;
    }
    return 24;
}

RejectReason dominant_reason(const std::map<RejectReason, std::uint32_t>& counts) {
    RejectReason best = RejectReason::None;
    std::uint32_t best_count = 0;
    int best_priority = 0;
    for (const auto& entry : counts) {
        const int priority = reason_priority(entry.first);
        const bool better = entry.second > best_count ||
                            (entry.second == best_count && best != RejectReason::None &&
                             (priority < best_priority ||
                              (priority == best_priority && entry.first < best)));
        if (better) {
            best = entry.first;
            best_count = entry.second;
            best_priority = priority;
        }
    }
    return best;
}
}  // namespace

Result<Decision> schedule_locked(SchedulerState& state, const EngineConfig& config, IdGenerator& ids,
                                 StoredRequest& stored, PlacementGeneration forced_generation,
                                 std::uint32_t reassignment_count) {
    const ScheduleRequest& request = stored.request;
    const Limits& limits = config.limits;

    Decision decision;
    decision.id = ids.next<DecisionId>();
    Explanation explanation;
    explanation.decision = decision.id;
    explanation.request = request.id;
    explanation.experiment = request.experiment;
    explanation.experiment_generation = request.experiment_generation;

    if (state.shutting_down) {
        explanation.notes.push_back("coordinator is shutting down");
    }

    // --- 1. hard filtering: build the eligible set for every requirement -----
    std::vector<RequirementPlan> plans;
    plans.reserve(request.requirements.size());
    bool satisfiable = true;
    std::size_t unsatisfied_index = plans.max_size();

    for (const ResourceRequirement& requirement : request.requirements) {
        RequirementPlan plan;
        plan.requirement = &requirement;
        for (const auto& entry : state.resources) {
            const ResourceRecord& resource = entry.second;
            if (resource.resource_class != requirement.resource_class) {
                continue;
            }
            ++plan.considered;
            Eligibility eligibility = evaluate_resource(request, requirement, resource, limits);
            if (!eligibility.eligible) {
                for (const RejectReason reason : eligibility.reasons) {
                    plan.reason_counts[reason] += 1;
                }
                if (plan.rejected.size() < limits.max_rejected_entries_per_requirement) {
                    RejectedResourceEntry rejected;
                    rejected.resource = resource.id;
                    rejected.name = resource.name;
                    rejected.reasons = eligibility.reasons;
                    plan.rejected.push_back(std::move(rejected));
                }
                continue;
            }
            CandidateResource candidate;
            candidate.id = resource.id;
            candidate.generation = resource.generation;
            candidate.resource_class = resource.resource_class;
            candidate.name = resource.name;
            candidate.slots = eligibility.slots;
            candidate.exclusive = eligibility.exclusive;
            compute_additive_values(state, request, requirement, resource, eligibility, candidate.additive);
            std::int64_t partial = 0;
            for (std::size_t f = 0; f < kRankingFactorCount; ++f) {
                if (kFactorScope[f] == FactorScope::Additive) {
                    partial += candidate.additive[f] * static_cast<std::int64_t>(request.weights.values[f]);
                }
            }
            candidate.partial = partial;
            plan.candidates.push_back(std::move(candidate));
        }
        std::sort(plan.candidates.begin(), plan.candidates.end(),
                  [](const CandidateResource& a, const CandidateResource& b) {
                      if (a.partial != b.partial) {
                          return a.partial > b.partial;
                      }
                      return a.id < b.id;
                  });
        {
            std::set<std::string> seen;
            std::vector<CandidateResource> distinct;
            distinct.reserve(plan.candidates.size());
            for (CandidateResource& candidate : plan.candidates) {
                const auto resource = state.resources.find(candidate.id);
                if (resource == state.resources.end()) {
                    continue;
                }
                if (seen.insert(interchangeability_key(candidate, resource->second)).second) {
                    distinct.push_back(std::move(candidate));
                }
            }
            plan.candidates = std::move(distinct);
        }
        if (plan.candidates.size() > limits.max_candidate_resources_per_requirement) {
            plan.candidates.resize(limits.max_candidate_resources_per_requirement);
            explanation.notes.push_back("eligible set for requirement '" + requirement.name +
                                        "' was truncated to the configured candidate bound");
        }
        if (plan.candidates.empty()) {
            satisfiable = false;
            if (unsatisfied_index == plans.max_size()) {
                unsatisfied_index = plans.size();
            }
        }
        plans.push_back(std::move(plan));
    }
    fill_requirement_explanations(plans, explanation);

    if (!satisfiable && unsatisfied_index < plans.size()) {
        const RequirementPlan& blocked = plans[unsatisfied_index];
        const RejectReason reason = dominant_reason(blocked.reason_counts);
        const ErrorCode code = map_reject_reason(reason);
        std::string detail = "requirement '" + blocked.requirement->name +
                             "' has no eligible resource: " + std::string(reject_reason_name(reason));
        if (blocked.considered == 0) {
            detail += " (no resource of class " +
                      std::string(resource_class_name(blocked.requirement->resource_class)) +
                      " is registered)";
        }
        decision = make_no_placement_decision(std::move(decision), std::move(explanation), code,
                                              std::move(detail));
        state.decisions.emplace(decision.id, decision.explanation);
        stored.state = RequestState::NoPlacement;
        stored.current_placement = PlacementId{};
        stored.detail = decision.failure_detail;
        return decision;
    }

    // --- 2. whole-placement construction and validation ----------------------
    std::vector<std::int64_t> remaining_max(plans.size() + 1, 0);
    for (std::size_t i = plans.size(); i-- > 0;) {
        const std::int64_t front = plans[i].candidates.empty() ? 0 : plans[i].candidates.front().partial;
        remaining_max[i] = remaining_max[i + 1] + front;
    }
    const std::int64_t bound = cross_bound(request, plans.size());

    SearchOutcome outcome;
    std::vector<const CandidateResource*> current;
    std::int64_t best_raw = std::numeric_limits<std::int64_t>::min();
    search_recursive(0, 0, remaining_max, state, request, limits, plans, current, outcome, bound, best_raw);

    if (outcome.truncated) {
        explanation.notes.push_back("candidate exploration stopped at the configured evaluation bound");
    }
    for (const auto& entry : outcome.cross_rejections) {
        explanation.notes.push_back("cross-constraint rejections: " + std::string(reject_reason_name(entry.first)) +
                                    "=" + decimal_u64(entry.second));
    }
    std::sort(explanation.notes.begin(), explanation.notes.end());

    if (outcome.candidates.empty()) {
        RejectReason reason = dominant_reason(outcome.cross_rejections);
        const ErrorCode code = map_reject_reason(reason);
        std::string detail = "no combination of eligible resources satisfies the compound placement constraints";
        if (reason != RejectReason::None) {
            detail += ": " + std::string(reject_reason_name(reason));
        }
        decision = make_no_placement_decision(std::move(decision), std::move(explanation), code,
                                              std::move(detail));
        state.decisions.emplace(decision.id, decision.explanation);
        stored.state = RequestState::NoPlacement;
        stored.current_placement = PlacementId{};
        stored.detail = decision.failure_detail;
        return decision;
    }

    std::sort(outcome.candidates.begin(), outcome.candidates.end(), candidate_is_better);
    const RankingCandidate& best = outcome.candidates.front();
    const std::size_t requirement_count = plans.size();

    // --- 3. commit -----------------------------------------------------------
    if (state.placements.size() >= limits.max_placements) {
        return Status(ErrorCode::ResourceExhausted, "placement log has reached its configured bound");
    }
    if (state.reservations.size() >= limits.max_reservations) {
        return Status(ErrorCode::ResourceExhausted, "reservation ledger has reached its configured bound");
    }
    if (count_live_placements(state) >= limits.max_active_placements) {
        return Status(ErrorCode::ResourceExhausted, "active placement bound reached");
    }
    if (count_active_reservations(state) >= limits.max_active_reservations) {
        return Status(ErrorCode::ResourceExhausted, "active reservation bound reached");
    }

    PlacementRecord placement;
    placement.id = ids.next<PlacementId>();
    placement.generation =
        forced_generation.is_null() ? PlacementGeneration::from_value(1) : forced_generation;
    placement.request = request.id;
    placement.decision = decision.id;
    placement.experiment = request.experiment;
    placement.experiment_generation = request.experiment_generation;
    placement.trial = request.trial;
    placement.epoch = state.epoch;
    placement.reassignment_count = reassignment_count;
    placement.state = PlacementState::Reserved;
    placement.transition_sequence = ++state.transition_sequence;

    ReservationRecord reservation;
    reservation.id = ids.next<ReservationId>();
    reservation.lease = ids.next<LeaseId>();
    reservation.placement = placement.id;
    reservation.placement_generation = placement.generation;
    reservation.request = request.id;
    reservation.experiment = request.experiment;
    reservation.experiment_generation = request.experiment_generation;
    reservation.epoch = state.epoch;
    reservation.state = ReservationState::Active;
    reservation.acquire_sequence = ++state.reservation_sequence;
    reservation.detail = "committed with placement " + placement.id.to_string();

    for (std::size_t i = 0; i < best.selection.size(); ++i) {
        const CandidateResource& candidate = *best.selection[i];
        auto found = state.resources.find(candidate.id);
        if (found == state.resources.end()) {
            return Status(ErrorCode::InternalError, "selected resource disappeared before commit");
        }
        ResourceRecord& resource = found->second;
        SelectedResource selected;
        selected.resource = resource.id;
        selected.resource_class = resource.resource_class;
        selected.generation = resource.generation;
        selected.slots = candidate.slots;
        selected.exclusive = candidate.exclusive;
        selected.requirement_name = request.requirements[i].name;
        selected.worker = resource.worker;
        selected.worker_boot = resource.boot;
        selected.resource_name = resource.name;
        placement.resources.push_back(std::move(selected));

        ReservationClaim claim;
        claim.resource = resource.id;
        claim.slots = candidate.slots;
        claim.exclusive = candidate.exclusive;
        claim.generation_at_acquire = resource.generation;
        reservation.claims.push_back(claim);

        if (resource.reserved_slots > resource.capacity.slots - candidate.slots) {
            return Status(ErrorCode::CapacityExhausted,
                          "reservation would exceed capacity on " + resource.id.to_string());
        }
        resource.reserved_slots += candidate.slots;
    }

    for (const SelectedResource& selected : placement.resources) {
        if (selected.resource_class == ResourceClass::Dataset) {
            continue;
        }
        if (!selected.worker.is_null()) {
            placement.worker = selected.worker;
            placement.worker_boot = selected.worker_boot;
            break;
        }
    }
    if (placement.worker.is_null()) {
        for (const SelectedResource& selected : placement.resources) {
            if (!selected.worker.is_null()) {
                placement.worker = selected.worker;
                placement.worker_boot = selected.worker_boot;
                break;
            }
        }
    }

    placement.reservation = reservation.id;
    placement.lease = reservation.lease;
    placement.detail = "reserved and awaiting assignment";

    AuthorityEnvelope authority;
    authority.request = request.id;
    authority.experiment = request.experiment;
    authority.experiment_generation = request.experiment_generation;
    authority.placement = placement.id;
    authority.placement_generation = placement.generation;
    authority.epoch = state.epoch;
    authority.lease = reservation.lease;
    authority.reservation = reservation.id;
    authority.worker = placement.worker;
    authority.worker_boot = placement.worker_boot;
    authority.resources = placement.resources;

    state.reservations.emplace(reservation.id, reservation);
    state.placements.emplace(placement.id, placement);
    UnitKey key;
    key.experiment = request.experiment;
    key.generation = request.experiment_generation;
    key.trial = request.trial;
    state.unit_current[key] = placement.id;
    stored.state = RequestState::Placed;
    stored.current_placement = placement.id;
    stored.placement_count += 1;
    stored.detail = "placed";
    ++state.placement_sequence;
    if (!request.tenant.empty()) {
        state.tenant_service[request.tenant] += 1;
    }

    explanation.outcome = DecisionOutcome::Placed;
    explanation.failure = ErrorCode::Ok;
    explanation.failure_detail.clear();
    explanation.selected = placement.resources;
    explanation.candidates.clear();
    const std::int64_t display_divisor =
        static_cast<std::int64_t>(1000 * (requirement_count == 0 ? 1 : requirement_count));
    for (std::size_t i = 0; i < outcome.candidates.size(); ++i) {
        CandidateExplanation entry;
        entry.resources = outcome.candidates[i].resources;
        entry.score = outcome.candidates[i].raw / display_divisor;
        entry.selected = i == 0;
        entry.tie_break = outcome.candidates[i].tie_break;
        if (i == 0) {
            entry.factors = outcome.candidates[i].factors;
        }
        explanation.candidates.push_back(std::move(entry));
        if (explanation.candidates.size() >= limits.max_explanation_entries) {
            break;
        }
    }

    decision.outcome = DecisionOutcome::Placed;
    decision.failure = ErrorCode::Ok;
    decision.placement = placement;
    decision.authority = authority;
    decision.explanation = explanation;
    state.decisions.emplace(decision.id, explanation);
    push_audit(state, config, "placement.committed", placement_summary(placement));
    return decision;
}

Result<Decision> SchedulerEngine::submit_request(ScheduleRequest request) {
    normalize_schedule_request(request);
    Status status = validate_schedule_request(request, config_.limits);
    if (!status.ok()) {
        return status;
    }
    const std::lock_guard<std::mutex> guard(mutex_);
    if (state_->shutting_down) {
        return Status(ErrorCode::ShuttingDown, "coordinator is shutting down: scheduling requests are rejected");
    }
    if (state_->requests.size() >= config_.limits.max_retained_requests) {
        return Status(ErrorCode::ResourceExhausted, "scheduling request log has reached its configured bound");
    }
    if (state_->requests.find(request.id) != state_->requests.end()) {
        return Status(ErrorCode::DuplicateIdentity,
                      "duplicate scheduling request identity: " + request.id.to_string());
    }
    std::size_t active_requests = 0;
    for (const auto& entry : state_->requests) {
        if (entry.second.state == RequestState::Submitted || entry.second.state == RequestState::Placed) {
            ++active_requests;
        }
    }
    if (active_requests >= config_.limits.max_active_requests) {
        return Status(ErrorCode::ResourceExhausted, "active scheduling request bound reached");
    }

    UnitKey key;
    key.experiment = request.experiment;
    key.generation = request.experiment_generation;
    key.trial = request.trial;
    const auto existing_unit = state_->unit_current.find(key);
    if (existing_unit != state_->unit_current.end()) {
        const auto placement = state_->placements.find(existing_unit->second);
        if (placement != state_->placements.end() && placement_state_is_live(placement->second.state)) {
            return Status(ErrorCode::Busy,
                          "a placement is already current for this scheduling unit: " +
                              placement->second.id.to_string());
        }
    }

    StoredRequest stored;
    stored.request = std::move(request);
    stored.state = RequestState::Submitted;
    stored.submitted_sequence = ++state_->request_sequence;
    stored.detail = "submitted";
    auto inserted = state_->requests.emplace(stored.request.id, std::move(stored));
    push_audit(*state_, config_, "request.submitted",
               inserted.first->second.request.id.to_string() + " experiment=" +
                   inserted.first->second.request.experiment.to_string());
    return schedule_locked(*state_, config_, ids_, inserted.first->second, PlacementGeneration{}, 0);
}


}  // namespace lab_scheduler
