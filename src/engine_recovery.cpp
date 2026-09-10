#include "engine_internal.hpp"

#include <algorithm>
#include <utility>

#include "lab_scheduler/canonical.hpp"

namespace lab_scheduler {
namespace {

struct IdFloor {
    std::uint64_t value = 0;

    template <class Id>
    void observe(const Id& id) noexcept {
        if (id.value() > value) {
            value = id.value();
        }
    }
};

}  // namespace

Result<RecoveryReport> SchedulerEngine::import_state(const DurableState& durable) {
    Status status = validate_durable_state(durable);
    if (!status.ok()) {
        return status;
    }

    const std::lock_guard<std::mutex> guard(mutex_);
    if (!state_->resources.empty() || !state_->requests.empty() || !state_->placements.empty() ||
        !state_->reservations.empty()) {
        return Status(ErrorCode::Busy, "import_state requires an empty scheduler state");
    }

    RecoveryReport report;
    report.previous_epoch = durable.epoch;
    report.current_epoch = CoordinatorEpoch::from_value(durable.epoch.value() + 1);
    state_->epoch = report.current_epoch;

    IdFloor floor;
    floor.value = durable.id_counter;

    // 1. Rebuild the resource inventory. Static capability records survive;
    //    dynamic availability does not, because a new coordinator cannot prove
    //    that an advertisement produced by the previous coordinator is still
    //    current.
    for (const ResourceRecord& persisted : durable.resources) {
        ResourceRecord record = persisted;
        record.reserved_slots = 0;
        record.generation = ResourceGeneration::from_value(record.generation.value() + 1);
        record.validated_epoch = CoordinatorEpoch{};
        floor.observe(record.id);
        if (record.worker.is_null()) {
            if (record.lifecycle != ResourceLifecycle::Retired) {
                record.lifecycle = ResourceLifecycle::Current;
            }
            record.dynamic_authoritative = record.lifecycle == ResourceLifecycle::Current;
            record.validated_epoch = state_->epoch;
            record.authority_detail = "coordinator owned static resource restored as current";
        } else {
            if (record.lifecycle != ResourceLifecycle::Retired) {
                record.lifecycle = ResourceLifecycle::Registered;
            }
            record.dynamic_authoritative = false;
            record.authority_detail =
                "dynamic state is not authoritative until the owning worker re-registers after coordinator restart";
            report.resources_requiring_revalidation.push_back(record.id);

            auto worker = state_->workers.find(record.worker);
            if (worker == state_->workers.end()) {
                WorkerRecord worker_record;
                worker_record.id = record.worker;
                worker_record.boot = record.boot;
                worker_record.connected = false;
                ++state_->worker_sequence;
                worker_record.registration_sequence = state_->worker_sequence;
                worker_record.detail = "restored from durable state; incarnation is not connected";
                floor.observe(record.worker);
                floor.observe(record.boot);
                state_->workers.emplace(record.worker, worker_record);
            }
        }
        state_->resources.emplace(record.id, std::move(record));
    }

    // 2. Restore scheduling requests. A request whose placement authority was
    //    revoked is returned to SUBMITTED so it can be scheduled again; its
    //    history is preserved.
    for (const StoredRequest& persisted : durable.requests) {
        StoredRequest stored = persisted;
        floor.observe(stored.request.id);
        floor.observe(stored.request.experiment);
        floor.observe(stored.request.experiment_generation);
        floor.observe(stored.request.trial);
        if (stored.state == RequestState::Placed) {
            stored.state = RequestState::Submitted;
            stored.detail = "placement authority revoked by coordinator restart";
        }
        stored.current_placement = PlacementId{};
        state_->requests.emplace(stored.request.id, std::move(stored));
        ++report.requests_preserved;
    }

    // 3. Restore placement history. Live placements are reconciled: their
    //    authority does not survive the coordinator restart.
    for (const PlacementRecord& persisted : durable.placements) {
        PlacementRecord placement = persisted;
        floor.observe(placement.id);
        floor.observe(placement.request);
        floor.observe(placement.decision);
        floor.observe(placement.reservation);
        floor.observe(placement.lease);
        if (placement_state_is_live(placement.state)) {
            placement.state = PlacementState::ReassignmentRequired;
            placement.detail = "coordinator restart revoked placement authority; reassignment required";
            ++state_->transition_sequence;
            placement.transition_sequence = state_->transition_sequence;
            AffectedPlacement affected;
            affected.placement = placement.id;
            affected.generation = placement.generation;
            affected.state = placement.state;
            affected.detail = placement.detail;
            report.reconciled_placements.push_back(affected);
        }
        state_->placements.emplace(placement.id, std::move(placement));
        ++report.placement_history_preserved;
    }

    // 4. Restore reservation ledger. No reservation survives a coordinator
    //    restart as active, and every claim is booked back to zero because the
    //    resources were rebuilt without reserved capacity. This is what keeps
    //    capacity accounting from double counting prior occupancy.
    for (const ReservationRecord& persisted : durable.reservations) {
        ReservationRecord reservation = persisted;
        floor.observe(reservation.id);
        floor.observe(reservation.lease);
        floor.observe(reservation.placement);
        if (reservation.state == ReservationState::Active) {
            reservation.state = ReservationState::Revoked;
            reservation.detail = "revoked by coordinator restart";
            report.revoked_reservations.push_back(reservation.id);
        }
        state_->reservations.emplace(reservation.id, std::move(reservation));
    }

    ids_.observe(floor.value + 1);
    report.status = Status{};
    report.detail = "durable state restored; coordinator epoch advanced and prior live authority revoked";
    push_audit(*state_, config_, "coordinator.recovered",
                       report.previous_epoch.to_string() + " -> " + report.current_epoch.to_string());
    return report;
}

}  // namespace lab_scheduler
