#include "engine_internal.hpp"

#include <algorithm>
#include <utility>

#include "lab_scheduler/canonical.hpp"

namespace lab_scheduler {
namespace {

void copy_advertisement(const ResourceAdvertisement& source, ResourceRecord& target) {
    target.id = source.id;
    target.resource_class = source.resource_class;
    target.name = source.name;
    target.worker = source.worker;
    target.boot = source.boot;
    target.provenance = source.provenance;
    target.sharing = source.sharing;
    target.capabilities = source.capabilities;
    target.capacity = source.capacity;
    target.advertised_occupancy = source.current_occupancy;
    target.health = source.health;
    target.health_detail = source.health_detail;
    target.readiness = source.readiness;
    target.topology = source.topology;
    target.locality = source.locality;
    target.accelerators = source.accelerators;
    target.model = source.model;
    target.simulator = source.simulator;
    target.dataset = source.dataset;
    target.environment = source.environment;
    target.tenant = source.tenant;
    target.labels = source.labels;
}

bool capacity_matches(const ResourceCapacity& a, const ResourceCapacity& b) {
    return a.slots == b.slots && a.memory_bytes == b.memory_bytes && a.max_sessions == b.max_sessions;
}

}  // namespace

Result<ResourceRecord> SchedulerEngine::register_resource(const ResourceAdvertisement& advertisement) {
    ResourceAdvertisement normalized = advertisement;
    Status status = validate_advertisement(normalized, config_.limits);
    if (!status.ok()) {
        return status;
    }
    normalize_advertisement(normalized);

    const std::lock_guard<std::mutex> guard(mutex_);
    if (state_->shutting_down) {
        return Status(ErrorCode::ShuttingDown, "coordinator is shutting down");
    }
    if (state_->resources.size() >= config_.limits.max_resources) {
        return Status(ErrorCode::ResourceExhausted, "resource registry has reached its configured bound");
    }
    if (state_->resources.find(normalized.id) != state_->resources.end()) {
        return Status(ErrorCode::DuplicateIdentity,
                      "resource is already registered: " + normalized.id.to_string());
    }
    if (!normalized.worker.is_null()) {
        static_cast<void>(adopt_worker_incarnation(*state_, config_, normalized.worker, normalized.boot,
                                                   "resource registration"));
    }

    ResourceRecord record;
    copy_advertisement(normalized, record);
    record.lifecycle = ResourceLifecycle::Current;
    record.generation = ResourceGeneration::from_value(1);
    record.health_generation = HealthGeneration::from_value(1);
    record.capacity_generation = CapacityGeneration::from_value(1);
    record.state_sequence = 1;
    record.validated_epoch = state_->epoch;
    record.dynamic_authoritative = true;
    record.authority_detail = "registered in coordinator epoch " + state_->epoch.to_string();
    record.reserved_slots = 0;

    status = validate_resource_record(record, config_.limits);
    if (!status.ok()) {
        return status;
    }
    const ResourceId id = record.id;
    auto inserted = state_->resources.emplace(id, std::move(record));
    push_audit(*state_, config_, "resource.registered", resource_summary(inserted.first->second));
    return inserted.first->second;
}

Result<ResourceRecord> SchedulerEngine::publish_resource_state(const ResourceAdvertisement& advertisement) {
    ResourceAdvertisement normalized = advertisement;
    Status status = validate_advertisement(normalized, config_.limits);
    if (!status.ok()) {
        return status;
    }
    normalize_advertisement(normalized);

    const std::lock_guard<std::mutex> guard(mutex_);
    if (state_->shutting_down) {
        return Status(ErrorCode::ShuttingDown, "coordinator is shutting down");
    }
    auto found = state_->resources.find(normalized.id);
    if (found == state_->resources.end()) {
        return Status(ErrorCode::NotFound, "unknown resource: " + normalized.id.to_string());
    }
    ResourceRecord& record = found->second;

    if (record.worker != normalized.worker) {
        return Status(ErrorCode::Unauthorized,
                      "resource " + record.id.to_string() + " is owned by a different worker");
    }
    if (record.lifecycle == ResourceLifecycle::Retired) {
        return Status(ErrorCode::Unauthorized,
                      "resource is retired and no longer accepts advertisements: " + record.id.to_string());
    }
    if (!record.worker.is_null() && !(record.boot == normalized.boot)) {
        // A publication from a different incarnation is only accepted after the
        // incarnation change has revoked the previous incarnation's authority.
        static_cast<void>(adopt_worker_incarnation(*state_, config_, normalized.worker, normalized.boot,
                                                   "resource state publication"));
        found = state_->resources.find(normalized.id);
        if (found == state_->resources.end()) {
            return Status(ErrorCode::InternalError, "resource disappeared during incarnation adoption");
        }
    }

    ResourceRecord& live = found->second;
    if (live.resource_class != normalized.resource_class) {
        return Status(ErrorCode::InvalidArgument, "resource class cannot change after registration");
    }
    if (normalized.capacity.slots < live.reserved_slots) {
        return Status(ErrorCode::CapacityExhausted,
                      "advertised capacity would invalidate active reservations on " + live.id.to_string());
    }
    if (static_cast<std::uint64_t>(normalized.current_occupancy) + live.reserved_slots >
        normalized.capacity.slots) {
        return Status(ErrorCode::CapacityExhausted,
                      "advertised occupancy plus active reservations exceeds capacity on " + live.id.to_string());
    }

    const ResourceCapacity previous_capacity = live.capacity;
    const HealthState previous_health = live.health;
    const ReadinessState previous_readiness = live.readiness;

    copy_advertisement(normalized, live);
    live.lifecycle = ResourceLifecycle::Current;
    live.generation = ResourceGeneration::from_value(live.generation.value() + 1);
    live.state_sequence += 1;
    live.validated_epoch = state_->epoch;
    live.dynamic_authoritative = true;
    live.authority_detail = "state published in coordinator epoch " + state_->epoch.to_string();
    if (!capacity_matches(previous_capacity, live.capacity)) {
        live.capacity_generation = CapacityGeneration::from_value(live.capacity_generation.value() + 1);
    }
    if (previous_health != live.health || previous_readiness != live.readiness) {
        live.health_generation = HealthGeneration::from_value(live.health_generation.value() + 1);
    }

    status = validate_resource_record(live, config_.limits);
    if (!status.ok()) {
        return status;
    }
    push_audit(*state_, config_, "resource.published", resource_summary(live));
    return live;
}

Result<ResourceRecord> SchedulerEngine::retire_resource(ResourceId resource,
                                                        ResourceGeneration expected_generation) {
    Status status = resource.validate();
    if (!status.ok()) {
        return status;
    }
    const std::lock_guard<std::mutex> guard(mutex_);
    auto found = state_->resources.find(resource);
    if (found == state_->resources.end()) {
        return Status(ErrorCode::NotFound, "unknown resource: " + resource.to_string());
    }
    ResourceRecord& record = found->second;
    if (!expected_generation.is_null() && !(record.generation == expected_generation)) {
        return Status(ErrorCode::StaleResource,
                      "resource generation " + expected_generation.to_string() +
                          " is not the current generation " + record.generation.to_string());
    }
    if (record.reserved_slots != 0) {
        return Status(ErrorCode::ReservationConflict,
                      "resource still holds reserved capacity: " + record.id.to_string());
    }
    record.lifecycle = ResourceLifecycle::Retired;
    record.dynamic_authoritative = false;
    record.generation = ResourceGeneration::from_value(record.generation.value() + 1);
    record.validated_epoch = CoordinatorEpoch{};
    record.authority_detail = "retired";
    push_audit(*state_, config_, "resource.retired", resource_summary(record));
    return record;
}

}  // namespace lab_scheduler
