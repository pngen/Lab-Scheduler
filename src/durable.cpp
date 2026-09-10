#include "lab_scheduler/durable.hpp"

#include "lab_scheduler/version.hpp"

#include <map>
#include <set>
#include <string>

namespace lab_scheduler {
namespace {

template <class Id>
Status insert_unique(std::set<Id>& seen, const Id& id, std::string_view what) {
    Status status = id.validate();
    if (!status.ok()) {
        return Status(ErrorCode::CorruptPersistence,
                      std::string("persisted ") + std::string(what) + " identity is null");
    }
    if (!seen.insert(id).second) {
        return Status(ErrorCode::DuplicateIdentity,
                      std::string("persisted ") + std::string(what) + " identity is duplicated: " + id.to_string());
    }
    return Status{};
}

}  // namespace

Status validate_durable_state(const DurableState& state) {
    if (state.format_version != kPersistenceFormatVersion) {
        return Status(ErrorCode::CorruptPersistence, "unsupported durable state format version");
    }
    Status status = state.epoch.validate();
    if (!status.ok()) {
        return Status(ErrorCode::CorruptPersistence, "persisted coordinator epoch is null");
    }
    const Limits& limits = state.limits;
    if (state.resources.size() > limits.max_persistence_records ||
        state.requests.size() > limits.max_persistence_records ||
        state.placements.size() > limits.max_persistence_records ||
        state.reservations.size() > limits.max_persistence_records) {
        return Status(ErrorCode::CorruptPersistence, "persisted record count exceeds the configured bound");
    }

    std::set<ResourceId> resource_ids;
    std::map<ResourceId, std::uint32_t> capacity_slots;
    for (const ResourceRecord& resource : state.resources) {
        status = insert_unique(resource_ids, resource.id, "resource");
        if (!status.ok()) {
            return status;
        }
        status = validate_resource_record(resource, limits);
        if (!status.ok()) {
            return status;
        }
        capacity_slots.emplace(resource.id, resource.capacity.slots);
    }

    std::set<ScheduleRequestId> request_ids;
    for (const StoredRequest& stored : state.requests) {
        status = insert_unique(request_ids, stored.request.id, "scheduling request");
        if (!status.ok()) {
            return status;
        }
        status = validate_schedule_request(stored.request, limits);
        if (!status.ok()) {
            return status;
        }
    }

    std::set<PlacementId> placement_ids;
    std::map<PlacementId, const PlacementRecord*> placement_by_id;
    for (const PlacementRecord& placement : state.placements) {
        status = insert_unique(placement_ids, placement.id, "placement");
        if (!status.ok()) {
            return status;
        }
        status = placement.generation.validate();
        if (!status.ok()) {
            return Status(ErrorCode::CorruptPersistence, "persisted placement generation is null");
        }
        if (request_ids.find(placement.request) == request_ids.end()) {
            return Status(ErrorCode::CorruptPersistence,
                          "persisted placement references an unknown scheduling request");
        }
        if (placement.resources.empty()) {
            return Status(ErrorCode::CorruptPersistence, "persisted placement has no selected resources");
        }
        for (const SelectedResource& selected : placement.resources) {
            if (resource_ids.find(selected.resource) == resource_ids.end()) {
                return Status(ErrorCode::CorruptPersistence,
                              "persisted placement references an unknown resource");
            }
            status = selected.generation.validate();
            if (!status.ok()) {
                return Status(ErrorCode::CorruptPersistence, "persisted placement resource generation is null");
            }
        }
        placement_by_id.emplace(placement.id, &placement);
    }

    std::set<ReservationId> reservation_ids;
    std::map<ResourceId, std::uint64_t> claimed_slots;
    for (const ReservationRecord& reservation : state.reservations) {
        status = insert_unique(reservation_ids, reservation.id, "reservation");
        if (!status.ok()) {
            return status;
        }
        status = reservation.lease.validate();
        if (!status.ok()) {
            return Status(ErrorCode::CorruptPersistence, "persisted reservation lease identity is null");
        }
        if (reservation.claims.empty()) {
            return Status(ErrorCode::CorruptPersistence, "persisted reservation has no claims");
        }
        if (placement_by_id.find(reservation.placement) == placement_by_id.end()) {
            return Status(ErrorCode::CorruptPersistence,
                          "persisted reservation references an unknown placement");
        }
        for (const ReservationClaim& claim : reservation.claims) {
            const auto capacity = capacity_slots.find(claim.resource);
            if (capacity == capacity_slots.end()) {
                return Status(ErrorCode::CorruptPersistence,
                              "persisted reservation claims an unknown resource");
            }
            if (claim.slots == 0) {
                return Status(ErrorCode::CorruptPersistence, "persisted reservation claims zero slots");
            }
            if (reservation.state == ReservationState::Active) {
                std::uint64_t& total = claimed_slots[claim.resource];
                total += claim.slots;
                if (total > capacity->second) {
                    return Status(ErrorCode::CorruptPersistence,
                                  "persisted active reservations exceed resource capacity");
                }
            }
        }
    }

    // The reserved capacity mirrored onto a resource must equal the sum of its
    // active reservation claims. Recovery books every mirror back to zero and
    // revokes every reservation, so persisted occupancy can never be restored
    // as live capacity.
    for (const ResourceRecord& resource : state.resources) {
        std::uint64_t claimed = 0;
        const auto found = claimed_slots.find(resource.id);
        if (found != claimed_slots.end()) {
            claimed = found->second;
        }
        if (static_cast<std::uint64_t>(resource.reserved_slots) != claimed) {
            return Status(ErrorCode::CorruptPersistence,
                          "persisted reserved capacity does not match the active reservation claims");
        }
    }
    return Status{};
}

}  // namespace lab_scheduler
