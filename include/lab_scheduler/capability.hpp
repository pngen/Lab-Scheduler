#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "lab_scheduler/error.hpp"
#include "lab_scheduler/identity.hpp"
#include "lab_scheduler/limits.hpp"

namespace lab_scheduler {

// Canonical capability names are lowercase ASCII: [a-z0-9] plus '.', '_' and
// '-'. A name that is not already canonical is rejected instead of normalized,
// so two advertisements that look different never silently compare equal.
bool is_canonical_capability_name(std::string_view name) noexcept;
Status validate_capability_name(std::string_view name);

struct Capability {
    CapabilityId id{};
    std::string name{};

    friend bool operator==(const Capability&, const Capability&) noexcept = default;
};

// Ordered, duplicate rejecting capability set.
class CapabilitySet {
public:
    Status add(std::string_view name, const Limits& limits);
    Status add(const Capability& capability, const Limits& limits);

    [[nodiscard]] bool contains(const CapabilityId& id) const noexcept;
    [[nodiscard]] bool contains_all(const CapabilitySet& required) const noexcept;
    [[nodiscard]] bool contains_name(std::string_view name) const noexcept;
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    // Entries are kept sorted by CapabilityId, which makes iteration order
    // independent of advertisement order.
    [[nodiscard]] const std::vector<Capability>& entries() const noexcept { return entries_; }
    [[nodiscard]] std::vector<Capability>& entries() noexcept { return entries_; }

    [[nodiscard]] std::vector<std::string> names() const;
    [[nodiscard]] Status validate(const Limits& limits) const;

private:
    std::vector<Capability> entries_{};
};

}  // namespace lab_scheduler
