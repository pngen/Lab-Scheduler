#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "lab_scheduler/identity.hpp"
#include "lab_scheduler/limits.hpp"

namespace lab_scheduler {

// Topology is consumed as scheduling evidence, never inferred. A domain that
// was not supplied stays UNKNOWN and can never satisfy a topology requirement.
enum class TopologyScope : std::uint8_t {
    Host = 0,
    NumaDomain = 1,
    RootComplex = 2,
    Rack = 3,
    Cluster = 4,
};

std::string_view topology_scope_name(TopologyScope scope) noexcept;
std::optional<TopologyScope> parse_topology_scope(std::string_view text) noexcept;

struct TopologyDomain {
    TopologyDomainId id{};
    std::string name{};

    [[nodiscard]] bool known() const noexcept { return !id.is_null(); }
    [[nodiscard]] bool matches(const TopologyDomain& other) const noexcept {
        return !id.is_null() && id == other.id;
    }
};

struct TopologyDescriptor {
    TopologyDomain host{};
    TopologyDomain numa{};
    TopologyDomain root_complex{};
    TopologyDomain rack{};
    TopologyDomain cluster{};
    // False when the advertiser supplied no topology at all: the runtime then
    // reports UNKNOWN rather than guessing co-location.
    bool supplied = false;

    [[nodiscard]] const TopologyDomain& domain(TopologyScope scope) const noexcept;
    [[nodiscard]] Status validate(const Limits& limits) const;
};

// Returns whether both domains are known and equal. Two UNKNOWN domains are
// never "the same domain".
bool topology_domains_equal(const TopologyDomain& a, const TopologyDomain& b) noexcept;

}  // namespace lab_scheduler
