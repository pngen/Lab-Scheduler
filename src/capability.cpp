#include "lab_scheduler/capability.hpp"

#include <algorithm>

namespace lab_scheduler {

bool is_canonical_capability_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    for (const char c : name) {
        const bool alpha = c >= 'a' && c <= 'z';
        const bool digit = c >= '0' && c <= '9';
        const bool separator = c == '.' || c == '_' || c == '-';
        if (!alpha && !digit && !separator) {
            return false;
        }
    }
    return true;
}

Status validate_capability_name(std::string_view name) {
    if (name.empty()) {
        return Status(ErrorCode::InvalidArgument, "capability name is empty");
    }
    if (name.size() > 64) {
        return Status(ErrorCode::InvalidArgument, "capability name exceeds 64 characters");
    }
    if (!is_canonical_capability_name(name)) {
        return Status(ErrorCode::InvalidArgument,
                      "capability name is not canonical lowercase ascii: " + std::string(name));
    }
    return Status{};
}

Status CapabilitySet::add(std::string_view name, const Limits& limits) {
    Status status = validate_capability_name(name);
    if (!status.ok()) {
        return status;
    }
    return add(Capability{capability_id_from_name(name), std::string(name)}, limits);
}

Status CapabilitySet::add(const Capability& capability, const Limits& limits) {
    if (capability.id.is_null()) {
        return Status(ErrorCode::InvalidIdentity, "capability identity is null");
    }
    Status status = validate_capability_name(capability.name);
    if (!status.ok()) {
        return status;
    }
    if (capability_id_from_name(capability.name) != capability.id) {
        return Status(ErrorCode::InvalidIdentity,
                      "capability identity does not match its canonical name: " + capability.name);
    }
    if (entries_.size() >= limits.max_capabilities_per_resource) {
        return Status(ErrorCode::LimitExceeded, "capability set exceeds configured maximum");
    }
    const auto existing = std::lower_bound(
        entries_.begin(), entries_.end(), capability.id,
        [](const Capability& entry, const CapabilityId& id) { return entry.id < id; });
    if (existing != entries_.end() && existing->id == capability.id) {
        return Status(ErrorCode::DuplicateIdentity, "duplicate capability: " + capability.name);
    }
    entries_.insert(existing, capability);
    return Status{};
}

bool CapabilitySet::contains(const CapabilityId& id) const noexcept {
    const auto found = std::lower_bound(
        entries_.begin(), entries_.end(), id,
        [](const Capability& entry, const CapabilityId& value) { return entry.id < value; });
    return found != entries_.end() && found->id == id;
}

bool CapabilitySet::contains_all(const CapabilitySet& required) const noexcept {
    for (const Capability& capability : required.entries_) {
        if (!contains(capability.id)) {
            return false;
        }
    }
    return true;
}

bool CapabilitySet::contains_name(std::string_view name) const noexcept {
    return contains(capability_id_from_name(name));
}

std::vector<std::string> CapabilitySet::names() const {
    std::vector<std::string> out;
    out.reserve(entries_.size());
    for (const Capability& capability : entries_) {
        out.push_back(capability.name);
    }
    return out;
}

Status CapabilitySet::validate(const Limits& limits) const {
    if (entries_.size() > limits.max_capabilities_per_resource) {
        return Status(ErrorCode::LimitExceeded, "capability set exceeds configured maximum");
    }
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const Capability& capability = entries_[i];
        Status status = validate_capability_name(capability.name);
        if (!status.ok()) {
            return status;
        }
        if (capability.id.is_null() || capability_id_from_name(capability.name) != capability.id) {
            return Status(ErrorCode::InvalidIdentity,
                          "capability identity does not match its canonical name: " + capability.name);
        }
        if (i > 0 && !(entries_[i - 1].id < capability.id)) {
            return Status(ErrorCode::InvalidArgument, "capability set is not canonically ordered");
        }
    }
    return Status{};
}

}  // namespace lab_scheduler
