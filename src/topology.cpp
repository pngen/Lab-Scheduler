#include "lab_scheduler/topology.hpp"

#include <array>

namespace lab_scheduler {
namespace {

struct ScopeName {
    TopologyScope scope;
    std::string_view name;
};

constexpr std::array<ScopeName, 5> kScopeNames{{
    {TopologyScope::Host, "HOST"},
    {TopologyScope::NumaDomain, "NUMA"},
    {TopologyScope::RootComplex, "ROOT_COMPLEX"},
    {TopologyScope::Rack, "RACK"},
    {TopologyScope::Cluster, "CLUSTER"},
}};

}  // namespace

std::string_view topology_scope_name(TopologyScope scope) noexcept {
    for (const auto& entry : kScopeNames) {
        if (entry.scope == scope) {
            return entry.name;
        }
    }
    return "UNKNOWN_SCOPE";
}

std::optional<TopologyScope> parse_topology_scope(std::string_view text) noexcept {
    for (const auto& entry : kScopeNames) {
        if (entry.name == text) {
            return entry.scope;
        }
    }
    return std::nullopt;
}

const TopologyDomain& TopologyDescriptor::domain(TopologyScope scope) const noexcept {
    switch (scope) {
        case TopologyScope::Host:
            return host;
        case TopologyScope::NumaDomain:
            return numa;
        case TopologyScope::RootComplex:
            return root_complex;
        case TopologyScope::Rack:
            return rack;
        case TopologyScope::Cluster:
            return cluster;
    }
    return host;
}

Status TopologyDescriptor::validate(const Limits& limits) const {
    const std::array<const TopologyDomain*, 5> domains{{&host, &numa, &root_complex, &rack, &cluster}};
    for (const TopologyDomain* domain : domains) {
        if (domain->name.size() > limits.max_string_length) {
            return Status(ErrorCode::InvalidArgument, "topology domain name exceeds maximum length");
        }
        if (domain->id.is_null() && !domain->name.empty()) {
            return Status(ErrorCode::InvalidArgument, "topology domain name supplied without a domain identity");
        }
    }
    return Status{};
}

bool topology_domains_equal(const TopologyDomain& a, const TopologyDomain& b) noexcept {
    return !a.id.is_null() && a.id == b.id;
}

}  // namespace lab_scheduler
