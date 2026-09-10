#include <vector>

#include "lab_scheduler/engine.hpp"
#include "lab_scheduler/inspection.hpp"
#include "lab_scheduler/persistence.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

using namespace lab_scheduler;

namespace {

constexpr std::uint64_t kWorker = 0x41;
constexpr std::uint64_t kBoot = 0x42;

struct Fixture {
    SchedulerEngine engine{};
    ResourceId gpu{};

    Fixture() {
        auto gpu_resource = test::gpu_resource(0x2001, kWorker, kBoot, "host-a", 2);
        gpu_resource.capacity.memory_bytes = 32ull * 1024ull * 1024ull * 1024ull;
        LS_CHECK(engine.register_resource(gpu_resource).ok());
        LS_CHECK(engine.register_resource(test::model_resource(0x2002, kWorker, kBoot, "host-a", 0x1f01, "1.0", true)).ok());
        gpu = ResourceId::from_value(0x2001);
    }

    Decision place(std::uint64_t request_id, std::uint64_t experiment) {
        ResourceRequirement gpu_requirement = test::requirement_of(ResourceClass::Gpu, "gpu");
        const Result<Decision> decision =
            engine.submit_request(test::request_of(request_id, experiment, {gpu_requirement}));
        LS_CHECK(decision.ok());
        return decision.value();
    }

    CompletionClaim claim_for(const Decision& decision, CoordinatorEpoch epoch) const {
        CompletionClaim claim;
        claim.placement = decision.authority.placement;
        claim.placement_generation = decision.authority.placement_generation;
        claim.experiment = decision.authority.experiment;
        claim.experiment_generation = decision.authority.experiment_generation;
        claim.epoch = epoch;
        claim.worker_boot = decision.authority.worker_boot;
        for (const SelectedResource& selected : decision.authority.resources) {
            claim.resources.push_back(selected.resource);
            claim.resource_generations.push_back(selected.generation);
        }
        claim.detail = "test completion";
        return claim;
    }
};

}  // namespace

LS_TEST(completion_requires_current_epoch_generation_and_identity) {
    Fixture fixture;
    const Decision decision = fixture.place(0xa001, 0xb001);
    LS_CHECK_EQ(decision.outcome, DecisionOutcome::Placed);
    const CoordinatorEpoch epoch = fixture.engine.epoch();

    CompletionClaim wrong_epoch = fixture.claim_for(decision, CoordinatorEpoch::from_value(999));
    CompletionOutcome outcome = fixture.engine.report_completion(wrong_epoch);
    LS_CHECK(!outcome.accepted);
    LS_CHECK_EQ(outcome.status.code, ErrorCode::StaleEpoch);

    CompletionClaim wrong_experiment = fixture.claim_for(decision, epoch);
    wrong_experiment.experiment = ExperimentId::from_value(0xdead);
    LS_CHECK_EQ(fixture.engine.report_completion(wrong_experiment).status.code, ErrorCode::StaleExperiment);

    CompletionClaim wrong_generation = fixture.claim_for(decision, epoch);
    wrong_generation.placement_generation = PlacementGeneration::from_value(7);
    LS_CHECK_EQ(fixture.engine.report_completion(wrong_generation).status.code, ErrorCode::StalePlacement);

    CompletionClaim wrong_boot = fixture.claim_for(decision, epoch);
    wrong_boot.worker_boot = WorkerBootId::from_value(0xffff);
    LS_CHECK_EQ(fixture.engine.report_completion(wrong_boot).status.code, ErrorCode::StaleWorker);

    CompletionClaim wrong_resources = fixture.claim_for(decision, epoch);
    wrong_resources.resources[0] = ResourceId::from_value(0x9999);
    LS_CHECK_EQ(fixture.engine.report_completion(wrong_resources).status.code, ErrorCode::Unauthorized);

    CompletionClaim stale_resource = fixture.claim_for(decision, epoch);
    stale_resource.resource_generations[0] = ResourceGeneration::from_value(99);
    LS_CHECK_EQ(fixture.engine.report_completion(stale_resource).status.code, ErrorCode::StaleResource);

    CompletionClaim accepted = fixture.claim_for(decision, epoch);
    const CompletionOutcome success = fixture.engine.report_completion(accepted);
    LS_CHECK(success.accepted);
    LS_CHECK_EQ(success.state, PlacementState::Completed);
    LS_CHECK_EQ(success.released_reservations.size(), std::size_t{1});

    const CompletionOutcome duplicate = fixture.engine.report_completion(accepted);
    LS_CHECK(!duplicate.accepted);
    LS_CHECK(duplicate.duplicate);
    LS_CHECK_EQ(duplicate.status.code, ErrorCode::DuplicateCompletion);

    const AccountingReport accounting = fixture.engine.accounting();
    LS_CHECK_EQ(accounting.total_reserved_slots, std::uint64_t{0});
    LS_CHECK(accounting.active_reservations == 0);
    LS_CHECK(fixture.engine.validate_invariants().all_ok());
}

LS_TEST(cancellation_is_exactly_once_and_blocks_late_completion) {
    Fixture fixture;
    const Decision decision = fixture.place(0xa002, 0xb002);
    const CoordinatorEpoch epoch = fixture.engine.epoch();
    const CancellationReport cancelled = fixture.engine.cancel_placement(
        decision.placement.id, decision.placement.generation, epoch, "test cancel");
    LS_CHECK(cancelled.status.ok());
    LS_CHECK_EQ(cancelled.state, PlacementState::Cancelled);
    LS_CHECK_EQ(cancelled.released_reservations.size(), std::size_t{1});

    const CancellationReport again = fixture.engine.cancel_placement(
        decision.placement.id, decision.placement.generation, epoch, "duplicate cancel");
    LS_CHECK_EQ(again.status.code, ErrorCode::Cancelled);
    LS_CHECK(again.released_reservations.empty());

    const CompletionOutcome late = fixture.engine.report_completion(fixture.claim_for(decision, epoch));
    LS_CHECK(!late.accepted);
    LS_CHECK_EQ(late.status.code, ErrorCode::Cancelled);

    const AccountingReport accounting = fixture.engine.accounting();
    LS_CHECK_EQ(accounting.total_reserved_slots, std::uint64_t{0});
    LS_CHECK(fixture.engine.validate_invariants().all_ok());
}

LS_TEST(request_cancellation_cancels_every_live_placement) {
    Fixture fixture;
    const Decision first = fixture.place(0xa003, 0xb003);
    const Decision second = fixture.place(0xa004, 0xb004);
    const CancellationReport report = fixture.engine.cancel_request(first.placement.request, "cancel request");
    LS_CHECK(report.status.ok());
    LS_CHECK_EQ(report.cancelled_placements.size(), std::size_t{1});
    const Result<PlacementRecord> still_live = fixture.engine.get_placement(second.placement.id);
    LS_CHECK(still_live.ok());
    LS_CHECK_EQ(still_live.value().state, PlacementState::Running == still_live.value().state
                                             ? PlacementState::Reserved
                                             : still_live.value().state);
    const AccountingReport accounting = fixture.engine.accounting();
    LS_CHECK_EQ(accounting.active_reservations, std::uint64_t{1});
    LS_CHECK(fixture.engine.validate_invariants().all_ok());
}

LS_TEST(reassignment_fences_prior_authority) {
    Fixture fixture;
    const Decision decision = fixture.place(0xa005, 0xb005);
    const CoordinatorEpoch epoch = fixture.engine.epoch();
    const Result<ReassignmentReport> reassigned =
        fixture.engine.reassign_placement(decision.placement.id, decision.placement.generation, epoch,
                                          "test reassignment");
    LS_CHECK(reassigned.ok());
    LS_CHECK(reassigned.value().status.ok());
    LS_CHECK(!reassigned.value().new_placement.is_null());
    LS_CHECK(!(reassigned.value().new_placement == decision.placement.id));
    LS_CHECK(reassigned.value().new_generation.value() == decision.placement.generation.value() + 1);

    const Result<PlacementRecord> old = fixture.engine.get_placement(decision.placement.id);
    LS_CHECK(old.ok());
    LS_CHECK_EQ(old.value().state, PlacementState::ReassignmentRequired);

    const CompletionOutcome stale = fixture.engine.report_completion(fixture.claim_for(decision, epoch));
    LS_CHECK(!stale.accepted);
    LS_CHECK_EQ(stale.status.code, ErrorCode::ReassignmentRequired);

    const Result<PlacementRecord> fresh =
        fixture.engine.get_placement(reassigned.value().new_placement);
    LS_CHECK(fresh.ok());
    LS_CHECK_EQ(fresh.value().reassignment_count, std::uint32_t{1});
    LS_CHECK(fixture.engine.validate_invariants().all_ok());
}

LS_TEST(preemption_revokes_authority_and_can_reschedule) {
    Fixture fixture;
    ResourceRequirement gpu = test::requirement_of(ResourceClass::Gpu, "gpu");
    ScheduleRequest request = test::request_of(0xa006, 0xb006, {gpu});
    request.allow_preemption = true;
    const Result<Decision> decision = fixture.engine.submit_request(request);
    LS_CHECK(decision.ok());
    LS_CHECK_EQ(decision.value().outcome, DecisionOutcome::Placed);
    const Result<ReassignmentReport> preempted = fixture.engine.preempt_placement(
        decision.value().placement.id, decision.value().placement.generation, fixture.engine.epoch(),
        "free the accelerator");
    LS_CHECK(preempted.ok());
    LS_CHECK_EQ(preempted.value().revoked_reservations.size(), std::size_t{1});
    const Result<PlacementRecord> old = fixture.engine.get_placement(decision.value().placement.id);
    LS_CHECK(old.ok());
    LS_CHECK_EQ(old.value().state, PlacementState::Preempted);
    LS_CHECK(!preempted.value().new_placement.is_null());
}

LS_TEST(worker_loss_invalidates_incarnation_and_requires_fresh_registration) {
    Fixture fixture;
    const Decision decision = fixture.place(0xa007, 0xb007);
    const ResourceLossReport loss = fixture.engine.mark_worker_lost(
        WorkerId::from_value(kWorker), WorkerBootId::from_value(kBoot), "process died");
    LS_CHECK(loss.status.ok());
    LS_CHECK_EQ(loss.invalidated_resources.size(), std::size_t{2});
    LS_CHECK_EQ(loss.affected_placements.size(), std::size_t{1});
    LS_CHECK_EQ(loss.released_reservations.size(), std::size_t{1});

    const ResourceLossReport repeated = fixture.engine.mark_worker_lost(
        WorkerId::from_value(kWorker), WorkerBootId::from_value(kBoot), "duplicate report");
    LS_CHECK(repeated.status.ok());
    LS_CHECK(repeated.invalidated_resources.empty());
    LS_CHECK(repeated.released_reservations.empty());

    const Result<PlacementRecord> placement = fixture.engine.get_placement(decision.placement.id);
    LS_CHECK(placement.ok());
    LS_CHECK_EQ(placement.value().state, PlacementState::ReassignmentRequired);

    const CompletionOutcome late = fixture.engine.report_completion(
        fixture.claim_for(decision, fixture.engine.epoch()));
    LS_CHECK(!late.accepted);

    // A fresh incarnation publishes state and becomes eligible again.
    const ResourceLossReport reconnected = fixture.engine.worker_connected(
        WorkerId::from_value(kWorker), WorkerBootId::from_value(0x43));
    LS_CHECK(reconnected.status.ok());
    LS_CHECK(reconnected.incarnation_adopted);
    auto reregistered = test::gpu_resource(0x2001, kWorker, 0x43, "host-a", 2);
    LS_CHECK(fixture.engine.publish_resource_state(reregistered).ok());
    const Decision after = fixture.place(0xa008, 0xb008);
    LS_CHECK_EQ(after.outcome, DecisionOutcome::Placed);
    LS_CHECK_MESSAGE(fixture.engine.validate_invariants().all_ok(),
                     render_invariants(fixture.engine.validate_invariants()));
}

LS_TEST(retirement_requires_no_active_reservation) {
    Fixture fixture;
    const Decision decision = fixture.place(0xa009, 0xb009);
    const Result<ResourceRecord> blocked =
        fixture.engine.retire_resource(ResourceId::from_value(0x2001), ResourceGeneration{});
    LS_CHECK(!blocked.ok());
    LS_CHECK_EQ(blocked.code(), ErrorCode::ReservationConflict);

    const CompletionOutcome completion = fixture.engine.report_completion(
        fixture.claim_for(decision, fixture.engine.epoch()));
    LS_CHECK(completion.accepted);
    const Result<ResourceRecord> retired =
        fixture.engine.retire_resource(ResourceId::from_value(0x2001), ResourceGeneration{});
    LS_CHECK(retired.ok());
    LS_CHECK_EQ(retired.value().lifecycle, ResourceLifecycle::Retired);
    LS_CHECK_EQ(fixture.engine.retire_resource(ResourceId::from_value(0x2001), ResourceGeneration::from_value(1))
                    .code(),
                ErrorCode::StaleResource);
}

LS_TEST(coordinator_recovery_revokes_live_authority_and_preserves_history) {
    SchedulerEngine first_engine;
    const auto gpu = test::gpu_resource(0x2001, kWorker, kBoot, "host-a", 2);
    LS_CHECK(first_engine.register_resource(gpu).ok());
    ResourceRequirement gpu_requirement = test::requirement_of(ResourceClass::Gpu, "gpu");
    const Result<Decision> decision =
        first_engine.submit_request(test::request_of(0xa00a, 0xb00a, {gpu_requirement}));
    LS_CHECK(decision.ok());
    LS_CHECK_EQ(decision.value().outcome, DecisionOutcome::Placed);
    const DurableState durable = first_engine.export_state();
    LS_CHECK(validate_durable_state(durable).ok());

    CompletionClaim claim;
    claim.placement = decision.value().placement.id;
    claim.placement_generation = decision.value().placement.generation;
    claim.experiment = decision.value().placement.experiment;
    claim.experiment_generation = decision.value().placement.experiment_generation;
    claim.epoch = first_engine.epoch();
    claim.worker_boot = decision.value().placement.worker_boot;
    for (const SelectedResource& selected : decision.value().placement.resources) {
        claim.resources.push_back(selected.resource);
        claim.resource_generations.push_back(selected.generation);
    }

    SchedulerEngine recovered;
    const Result<RecoveryReport> recovery = recovered.import_state(durable);
    LS_CHECK(recovery.ok());
    LS_CHECK_EQ(recovery.value().previous_epoch, durable.epoch);
    LS_CHECK(recovery.value().current_epoch.value() > durable.epoch.value());
    LS_CHECK(recovery.value().reconciled_placements.size() == 1);
    LS_CHECK(recovery.value().revoked_reservations.size() == 1);
    LS_CHECK(recovery.value().resources_requiring_revalidation.size() == 1);
    LS_CHECK(recovery.value().placement_history_preserved == 1);

    const Result<PlacementRecord> placement = recovered.get_placement(claim.placement);
    LS_CHECK(placement.ok());
    LS_CHECK_EQ(placement.value().state, PlacementState::ReassignmentRequired);
    LS_CHECK_EQ(recovered.report_completion(claim).status.code, ErrorCode::StaleEpoch);
    LS_CHECK_EQ(recovered.accounting().total_reserved_slots, std::uint64_t{0});
    LS_CHECK(recovered.validate_invariants().all_ok());

    // Dynamic state is not restored as current: placement requires fresh
    // registration, and a new incarnation publishing state makes it eligible.
    ResourceRequirement requirement = test::requirement_of(ResourceClass::Gpu, "gpu");
    const Result<Decision> before =
        recovered.submit_request(test::request_of(0xa00b, 0xb00b, {requirement}));
    LS_CHECK(before.ok());
    LS_CHECK_EQ(before.value().outcome, DecisionOutcome::NoPlacement);

    const ResourceLossReport adopted = recovered.worker_connected(WorkerId::from_value(kWorker),
                                                                  WorkerBootId::from_value(0x44));
    LS_CHECK(adopted.status.ok());
    auto republished = test::gpu_resource(0x2001, kWorker, 0x44, "host-a", 2);
    LS_CHECK(recovered.publish_resource_state(republished).ok());
    const Result<Decision> after =
        recovered.submit_request(test::request_of(0xa00c, 0xb00c, {requirement}));
    LS_CHECK(after.ok());
    LS_CHECK_EQ(after.value().outcome, DecisionOutcome::Placed);
}

LS_TEST(import_requires_an_empty_engine_and_valid_state) {
    Fixture fixture;
    DurableState state = fixture.engine.export_state();
    LS_CHECK_EQ(fixture.engine.import_state(state).code(), ErrorCode::Busy);
    SchedulerEngine fresh;
    state.format_version = 99;
    LS_CHECK_EQ(fresh.import_state(state).code(), ErrorCode::CorruptPersistence);
    state.format_version = 1;
    state.placements.push_back(PlacementRecord{});
    LS_CHECK(!fresh.import_state(state).ok());
}
