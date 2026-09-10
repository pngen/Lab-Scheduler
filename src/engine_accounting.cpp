#include "engine_internal.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "lab_scheduler/canonical.hpp"

namespace lab_scheduler {
namespace {

struct ResourceLedger {
    std::uint32_t claimed_slots = 0;
    std::uint32_t active_claims = 0;
};

std::map<ResourceId, ResourceLedger> build_ledger(const SchedulerState& state) {
    std::map<ResourceId, ResourceLedger> ledger;
    for (const auto& entry : state.reservations) {
        const ReservationRecord& reservation = entry.second;
        if (reservation.state != ReservationState::Active) {
            continue;
        }
        for (const ReservationClaim& claim : reservation.claims) {
            ResourceLedger& slot = ledger[claim.resource];
            slot.claimed_slots += claim.slots;
            slot.active_claims += 1;
        }
    }
    return ledger;
}

}  // namespace

AccountingReport SchedulerEngine::accounting() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    AccountingReport report;
    report.epoch = state_->epoch;
    report.placements_recorded = state_->placements.size();
    report.requests_recorded = state_->requests.size();
    report.reservations_recorded = state_->reservations.size();
    for (const auto& entry : state_->placements) {
        if (placement_state_is_live(entry.second.state)) {
            ++report.active_placements;
        }
    }
    for (const auto& entry : state_->reservations) {
        if (entry.second.state == ReservationState::Active) {
            ++report.active_reservations;
        }
    }
    const std::map<ResourceId, ResourceLedger> ledger = build_ledger(*state_);
    for (const auto& entry : state_->resources) {
        const ResourceRecord& resource = entry.second;
        ResourceAccounting accounting;
        accounting.resource = resource.id;
        accounting.resource_class = resource.resource_class;
        accounting.name = resource.name;
        accounting.capacity_slots = resource.capacity.slots;
        accounting.capacity_memory_bytes = resource.capacity.memory_bytes;
        accounting.advertised_occupancy = resource.advertised_occupancy;
        accounting.reserved_slots = resource.reserved_slots;
        accounting.lifecycle = resource.lifecycle;
        accounting.dynamic_authoritative = resource.dynamic_authoritative;
        accounting.generation = resource.generation.value();
        const auto found = ledger.find(resource.id);
        if (found != ledger.end()) {
            accounting.active_reservations = found->second.active_claims;
        }
        report.total_capacity_slots += resource.capacity.slots;
        report.total_reserved_slots += resource.reserved_slots;
        report.resources.push_back(std::move(accounting));
    }
    return report;
}

InvariantReport SchedulerEngine::validate_invariants() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    InvariantReport report;
    const std::map<ResourceId, ResourceLedger> ledger = build_ledger(*state_);

    {
        InvariantCheck check;
        check.name = "capacity_never_negative_or_over_committed";
        for (const auto& entry : state_->resources) {
            const ResourceRecord& resource = entry.second;
            const std::uint64_t occupancy = static_cast<std::uint64_t>(resource.advertised_occupancy) +
                                            resource.reserved_slots;
            if (occupancy > resource.capacity.slots) {
                check.ok = false;
                check.detail += resource.id.to_string() + " occupancy " + decimal_u64(occupancy) +
                                " exceeds capacity " + decimal_u64(resource.capacity.slots) + "; ";
            }
        }
        report.checks.push_back(std::move(check));
    }
    {
        InvariantCheck check;
        check.name = "reserved_slots_match_active_claims";
        for (const auto& entry : state_->resources) {
            const ResourceRecord& resource = entry.second;
            std::uint32_t claimed = 0;
            const auto found = ledger.find(resource.id);
            if (found != ledger.end()) {
                claimed = found->second.claimed_slots;
            }
            if (claimed != resource.reserved_slots) {
                check.ok = false;
                check.detail += resource.id.to_string() + " mirrored " + decimal_u64(resource.reserved_slots) +
                                " != ledger " + decimal_u64(claimed) + "; ";
            }
        }
        report.checks.push_back(std::move(check));
    }
    {
        InvariantCheck check;
        check.name = "exclusive_resources_have_at_most_one_claim";
        for (const auto& entry : state_->resources) {
            const ResourceRecord& resource = entry.second;
            const auto found = ledger.find(resource.id);
            const std::uint32_t claims = found == ledger.end() ? 0 : found->second.active_claims;
            if (resource.sharing == SharingMode::Exclusive && claims > 1) {
                check.ok = false;
                check.detail += resource.id.to_string() + " has " + decimal_u64(claims) + " active claims; ";
            }
            if (claims > 1 && found != ledger.end() &&
                found->second.claimed_slots > resource.capacity.slots) {
                check.ok = false;
                check.detail += resource.id.to_string() + " over claimed; ";
            }
        }
        report.checks.push_back(std::move(check));
    }
    {
        InvariantCheck check;
        check.name = "live_placements_reference_active_reservations";
        for (const auto& entry : state_->placements) {
            const PlacementRecord& placement = entry.second;
            if (!placement_state_is_live(placement.state)) {
                continue;
            }
            const auto reservation = state_->reservations.find(placement.reservation);
            if (reservation == state_->reservations.end()) {
                check.ok = false;
                check.detail += placement.id.to_string() + " references a missing reservation; ";
                continue;
            }
            if (reservation->second.state != ReservationState::Active) {
                check.ok = false;
                check.detail += placement.id.to_string() + " references a non-active reservation; ";
            }
            if (!(reservation->second.placement == placement.id) ||
                !(reservation->second.placement_generation == placement.generation)) {
                check.ok = false;
                check.detail += placement.id.to_string() + " reservation binding mismatch; ";
            }
        }
        report.checks.push_back(std::move(check));
    }
    {
        InvariantCheck check;
        check.name = "active_reservations_reference_live_placements";
        for (const auto& entry : state_->reservations) {
            const ReservationRecord& reservation = entry.second;
            if (reservation.state != ReservationState::Active) {
                continue;
            }
            const auto placement = state_->placements.find(reservation.placement);
            if (placement == state_->placements.end()) {
                check.ok = false;
                check.detail += reservation.id.to_string() + " references a missing placement; ";
                continue;
            }
            if (!placement_state_is_live(placement->second.state)) {
                check.ok = false;
                check.detail += reservation.id.to_string() + " backs a non-live placement; ";
            }
            if (!(placement->second.generation == reservation.placement_generation)) {
                check.ok = false;
                check.detail += reservation.id.to_string() + " generation mismatch; ";
            }
            if (reservation.claims.empty()) {
                check.ok = false;
                check.detail += reservation.id.to_string() + " has no claims; ";
            }
        }
        report.checks.push_back(std::move(check));
    }
    {
        InvariantCheck check;
        check.name = "placements_reference_registered_resources_and_requests";
        for (const auto& entry : state_->placements) {
            const PlacementRecord& placement = entry.second;
            if (placement.resources.empty()) {
                check.ok = false;
                check.detail += placement.id.to_string() + " selects no resources; ";
            }
            if (state_->requests.find(placement.request) == state_->requests.end()) {
                check.ok = false;
                check.detail += placement.id.to_string() + " references an unknown request; ";
            }
            std::vector<ResourceId> ids;
            for (const SelectedResource& selected : placement.resources) {
                ids.push_back(selected.resource);
                if (state_->resources.find(selected.resource) == state_->resources.end()) {
                    check.ok = false;
                    check.detail += placement.id.to_string() + " references an unknown resource; ";
                }
                if (selected.slots == 0) {
                    check.ok = false;
                    check.detail += placement.id.to_string() + " claims zero slots; ";
                }
            }
            std::sort(ids.begin(), ids.end());
            if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) {
                check.ok = false;
                check.detail += placement.id.to_string() + " selects a resource twice; ";
            }
        }
        report.checks.push_back(std::move(check));
    }
    {
        InvariantCheck check;
        check.name = "one_current_placement_per_schedulable_unit";
        for (const auto& entry : state_->unit_current) {
            const auto placement = state_->placements.find(entry.second);
            if (placement == state_->placements.end()) {
                check.ok = false;
                check.detail += "unit map references a missing placement; ";
                continue;
            }
            if (!placement_state_is_live(placement->second.state)) {
                check.ok = false;
                check.detail += "unit map references a non-live placement; ";
            }
        }
        report.checks.push_back(std::move(check));
    }
    {
        InvariantCheck check;
        check.name = "resource_counters_are_monotonic_and_bounded";
        std::uint64_t claimed_total = 0;
        std::uint64_t capacity_total = 0;
        for (const auto& entry : state_->resources) {
            const ResourceRecord& resource = entry.second;
            if (resource.generation.is_null() || resource.health_generation.is_null() ||
                resource.capacity_generation.is_null()) {
                check.ok = false;
                check.detail += resource.id.to_string() + " has a null generation; ";
            }
            if (resource.lifecycle == ResourceLifecycle::Retired && resource.reserved_slots != 0) {
                check.ok = false;
                check.detail += resource.id.to_string() + " is retired but still reserved; ";
            }
            claimed_total += resource.reserved_slots;
            capacity_total += resource.capacity.slots;
        }
        if (claimed_total > capacity_total) {
            check.ok = false;
            check.detail += "aggregate reserved capacity exceeds aggregate capacity; ";
        }
        report.checks.push_back(std::move(check));
    }
    {
        InvariantCheck check;
        check.name = "stale_authority_cannot_hold_reserved_capacity";
        for (const auto& entry : state_->resources) {
            const ResourceRecord& resource = entry.second;
            if (!resource.dynamic_authoritative && resource.reserved_slots != 0) {
                check.ok = false;
                check.detail += resource.id.to_string() +
                                " holds reserved capacity while its evidence is not authoritative; ";
            }
        }
        report.checks.push_back(std::move(check));
    }
    return report;
}

}  // namespace lab_scheduler
