#include <algorithm>
#include <vector>

#include "lab_scheduler/engine.hpp"
#include "lab_scheduler/inspection.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

using namespace lab_scheduler;

namespace {

constexpr std::uint64_t kWorkerA = 0x21;
constexpr std::uint64_t kWorkerB = 0x22;
constexpr std::uint64_t kBootA = 0x31;
constexpr std::uint64_t kBootB = 0x32;

struct LabFixture {
    SchedulerEngine engine{};
    ResourceAdvertisement gpu_a = test::gpu_resource(0x1001, kWorkerA, kBootA, "host-a", 2);
    ResourceAdvertisement gpu_b = test::gpu_resource(0x1002, kWorkerB, kBootB, "host-b", 1, true);
    ResourceAdvertisement model_a = test::model_resource(0x1003, kWorkerA, kBootA, "host-a", 0x1f01, "1.0", true);
    ResourceAdvertisement dataset_a =
        test::dataset_resource(0x1004, kWorkerA, kBootA, "host-a", 0x1f02, 0x2f01);
    ResourceAdvertisement dataset_b =
        test::dataset_resource(0x1005, kWorkerB, kBootB, "host-b", 0x1f02, 0x2f02);
    ResourceAdvertisement simulator_b =
        test::simulator_resource(0x1006, kWorkerB, kBootB, "host-b", 0x1f03, "1.2");

    LabFixture() {
        const Status status = test::register_advertisements(
            engine, {gpu_a, gpu_b, model_a, dataset_a, dataset_b, simulator_b});
        LS_CHECK(status.ok());
    }
};

}  // namespace

LS_TEST(hard_constraints_precede_ranking) {
    LabFixture fixture;
    ResourceRequirement gpu = test::requirement_of(ResourceClass::Gpu, "gpu");
    LS_CHECK(gpu.required_capabilities.add("missing-capability", Limits{}).ok());
    ScheduleRequest request = test::request_of(0x9001, 0x8001, {gpu});
    const Result<Decision> decision = fixture.engine.submit_request(request);
    LS_CHECK(decision.ok());
    LS_CHECK_EQ(decision.value().outcome, DecisionOutcome::NoPlacement);
    LS_CHECK_EQ(decision.value().failure, ErrorCode::CapabilityMismatch);
    LS_CHECK_EQ(decision.value().explanation.requirements.size(), std::size_t{1});
    LS_CHECK_EQ(decision.value().explanation.requirements[0].eligible, std::uint32_t{0});
    LS_CHECK_EQ(decision.value().explanation.requirements[0].rejected.size(), std::size_t{2});
}

LS_TEST(compound_placement_binds_dataset_version_and_affinity) {
    LabFixture fixture;
    ResourceRequirement gpu = test::requirement_of(ResourceClass::Gpu, "gpu");
    gpu.affinity_group = 1;
    ResourceRequirement dataset = test::requirement_of(ResourceClass::Dataset, "dataset");
    dataset.dataset = DatasetId::from_value(0x1f02);
    dataset.dataset_version = DatasetVersionId::from_value(0x2f01);
    dataset.affinity_group = 1;
    AffinityConstraint affinity;
    affinity.group_a = 1;
    affinity.group_b = 1;
    affinity.scope = TopologyScope::Host;
    ScheduleRequest request = test::request_of(0x9002, 0x8002, {gpu, dataset});
    request.affinity.push_back(affinity);
    const Result<Decision> decision = fixture.engine.submit_request(request);
    LS_CHECK(decision.ok());
    LS_CHECK_EQ(decision.value().outcome, DecisionOutcome::Placed);
    LS_CHECK_EQ(decision.value().placement.resources.size(), std::size_t{2});
    for (const SelectedResource& selected : decision.value().placement.resources) {
        LS_CHECK(selected.resource.value() == 0x1001 || selected.resource.value() == 0x1004);
    }
}

LS_TEST(whole_placement_rejects_incompatible_combination) {
    LabFixture fixture;
    // Two accelerators on one host, but the request demands host separation:
    // every individual resource is eligible and the combination is not.
    LS_CHECK(fixture.engine.register_resource(test::gpu_resource(0x1010, kWorkerA, kBootA, "host-a", 1)).ok());
    ResourceRequirement left = test::requirement_of(ResourceClass::Gpu, "left");
    left.affinity_group = 1;
    left.allowlist.push_back(ResourceId::from_value(0x1001));
    ResourceRequirement right = test::requirement_of(ResourceClass::Gpu, "right");
    right.affinity_group = 2;
    right.allowlist.push_back(ResourceId::from_value(0x1010));
    AntiAffinityConstraint anti;
    anti.group_a = 1;
    anti.group_b = 2;
    anti.scope = TopologyScope::Host;
    ScheduleRequest request = test::request_of(0x9003, 0x8003, {left, right});
    request.anti_affinity.push_back(anti);
    const Result<Decision> decision = fixture.engine.submit_request(request);
    LS_CHECK(decision.ok());
    LS_CHECK_EQ(decision.value().outcome, DecisionOutcome::NoPlacement);
    LS_CHECK_EQ(decision.value().explanation.requirements[0].eligible, std::uint32_t{1});
    LS_CHECK_EQ(decision.value().explanation.requirements[1].eligible, std::uint32_t{1});
    LS_CHECK(!decision.value().explanation.notes.empty());
    LS_CHECK(decision.value().explanation.notes[0].find("ANTI_AFFINITY_VIOLATION") != std::string::npos);
}

LS_TEST(affinity_never_infers_unknown_topology) {
    LabFixture fixture;
    ResourceAdvertisement unknown_topology = test::gpu_resource(0x1007, kWorkerB, kBootB, "host-b", 4);
    unknown_topology.topology.supplied = false;
    unknown_topology.topology.host = TopologyDomain{};
    LS_CHECK(fixture.engine.register_resource(unknown_topology).ok());

    ResourceRequirement first = test::requirement_of(ResourceClass::Gpu, "first");
    first.affinity_group = 1;
    ResourceRequirement second = test::requirement_of(ResourceClass::Gpu, "second");
    second.affinity_group = 1;
    second.allowlist.push_back(ResourceId::from_value(0x1007));
    first.allowlist.push_back(ResourceId::from_value(0x1001));
    AffinityConstraint affinity;
    affinity.group_a = 1;
    affinity.group_b = 1;
    affinity.scope = TopologyScope::Host;
    ScheduleRequest request = test::request_of(0x9004, 0x8004, {first, second});
    request.affinity.push_back(affinity);
    const Result<Decision> decision = fixture.engine.submit_request(request);
    LS_CHECK(decision.ok());
    LS_CHECK_EQ(decision.value().outcome, DecisionOutcome::NoPlacement);
}

LS_TEST(ranking_is_deterministic_and_tie_broken_by_identity) {
    LabFixture fixture;
    ResourceRequirement gpu = test::requirement_of(ResourceClass::Gpu, "gpu");
    ScheduleRequest first_request = test::request_of(0x9005, 0x8005, {gpu});
    ScheduleRequest second_request = test::request_of(0x9006, 0x8006, {gpu});
    const Result<Decision> first = fixture.engine.submit_request(first_request);
    const Result<Decision> second = fixture.engine.submit_request(second_request);
    LS_CHECK(first.ok() && second.ok());
    LS_CHECK_EQ(first.value().placement.resources.size(), second.value().placement.resources.size());
    for (std::size_t i = 0; i < first.value().placement.resources.size(); ++i) {
        LS_CHECK_EQ(first.value().placement.resources[i].resource,
                    second.value().placement.resources[i].resource);
    }
    LS_CHECK_EQ(first.value().explanation.candidates.front().tie_break,
                second.value().explanation.candidates.front().tie_break);
}

LS_TEST(exclusive_selection_claims_whole_resource_and_blocks_second_claim) {
    LabFixture fixture;
    ResourceRequirement gpu = test::requirement_of(ResourceClass::Gpu, "gpu");
    gpu.allowlist.push_back(ResourceId::from_value(0x1002));
    gpu.require_exclusive = true;
    ScheduleRequest first_request = test::request_of(0x9007, 0x8007, {gpu});
    const Result<Decision> first = fixture.engine.submit_request(first_request);
    LS_CHECK(first.ok());
    LS_CHECK_EQ(first.value().outcome, DecisionOutcome::Placed);
    LS_CHECK_EQ(first.value().placement.resources[0].slots, std::uint32_t{1});
    LS_CHECK(first.value().placement.resources[0].exclusive);
    ScheduleRequest second_request = test::request_of(0x9008, 0x8008, {gpu});
    const Result<Decision> second = fixture.engine.submit_request(second_request);
    LS_CHECK(second.ok());
    LS_CHECK_EQ(second.value().outcome, DecisionOutcome::NoPlacement);
    LS_CHECK_EQ(second.value().failure, ErrorCode::ExclusivityConflict);
}

LS_TEST(stale_evidence_is_rejected_unless_explicitly_waived) {
    // A coordinator restart is the only path that leaves a resource registered
    // but not authoritative: the evidence exists, and it is not current.
    SchedulerEngine origin;
    LS_CHECK(origin.register_resource(test::gpu_resource(0x1001, kWorkerA, kBootA, "host-a", 2)).ok());
    SchedulerEngine engine;
    const Result<RecoveryReport> recovery = engine.import_state(origin.export_state());
    LS_CHECK(recovery.ok());
    LS_CHECK(!engine.list_stale_resources().empty());

    ResourceRequirement gpu = test::requirement_of(ResourceClass::Gpu, "gpu");
    gpu.allowlist.push_back(ResourceId::from_value(0x1001));
    ScheduleRequest strict_request = test::request_of(0x9009, 0x8009, {gpu});
    const Result<Decision> strict = engine.submit_request(strict_request);
    LS_CHECK(strict.ok());
    LS_CHECK_EQ(strict.value().outcome, DecisionOutcome::NoPlacement);
    LS_CHECK_EQ(strict.value().failure, ErrorCode::StaleResource);

    ResourceRequirement relaxed = test::requirement_of(ResourceClass::Gpu, "gpu");
    relaxed.allowlist.push_back(ResourceId::from_value(0x1001));
    relaxed.require_current_evidence = false;
    relaxed.minimum_health = HealthState::Unknown;
    ScheduleRequest relaxed_request = test::request_of(0x900a, 0x800a, {relaxed});
    const Result<Decision> relaxed_decision = engine.submit_request(relaxed_request);
    LS_CHECK(relaxed_decision.ok());
    LS_CHECK_EQ(relaxed_decision.value().outcome, DecisionOutcome::Placed);
}

LS_TEST(duplicate_requests_and_unit_slots_are_rejected) {
    LabFixture fixture;
    ResourceRequirement gpu = test::requirement_of(ResourceClass::Gpu, "gpu");
    ScheduleRequest request = test::request_of(0x900b, 0x800b, {gpu});
    LS_CHECK(fixture.engine.submit_request(request).ok());
    const Result<Decision> duplicate = fixture.engine.submit_request(request);
    LS_CHECK(!duplicate.ok());
    LS_CHECK_EQ(duplicate.code(), ErrorCode::DuplicateIdentity);

    ScheduleRequest same_unit = test::request_of(0x900c, 0x800b, {gpu});
    same_unit.trial = TrialId::from_value(1);
    ScheduleRequest same_unit_again = test::request_of(0x900d, 0x800b, {gpu});
    same_unit_again.trial = TrialId::from_value(1);
    LS_CHECK(fixture.engine.submit_request(same_unit).ok());
    const Result<Decision> unit_conflict = fixture.engine.submit_request(same_unit_again);
    LS_CHECK(!unit_conflict.ok());
    LS_CHECK_EQ(unit_conflict.code(), ErrorCode::Busy);
}

LS_TEST(request_validation_rejects_malformed_requests) {
    LabFixture fixture;
    ResourceRequirement gpu = test::requirement_of(ResourceClass::Gpu, "gpu");
    gpu.min_slots = 0;
    ScheduleRequest invalid = test::request_of(0x900e, 0x800e, {gpu});
    const Result<Decision> decision = fixture.engine.submit_request(invalid);
    LS_CHECK(!decision.ok());
    LS_CHECK_EQ(decision.code(), ErrorCode::InvalidArgument);

    ScheduleRequest no_requirements = test::request_of(0x900f, 0x800f, {});
    LS_CHECK_EQ(fixture.engine.submit_request(no_requirements).code(), ErrorCode::InvalidArgument);

    ScheduleRequest duplicate_names = test::request_of(
        0x9010, 0x8010,
        {test::requirement_of(ResourceClass::Gpu, "same"), test::requirement_of(ResourceClass::Gpu, "same")});
    LS_CHECK_EQ(fixture.engine.submit_request(duplicate_names).code(), ErrorCode::DuplicateIdentity);

    ScheduleRequest unknown_group = test::request_of(0x9011, 0x8011, {gpu});
    unknown_group.requirements[0].min_slots = 1;
    AffinityConstraint bad;
    bad.group_a = 5;
    bad.group_b = 5;
    unknown_group.affinity.push_back(bad);
    LS_CHECK_EQ(fixture.engine.submit_request(unknown_group).code(), ErrorCode::InvalidArgument);
}

LS_TEST(explanations_are_byte_identical_for_identical_state) {
    LabFixture fixture;
    ResourceRequirement gpu = test::requirement_of(ResourceClass::Gpu, "gpu");
    const Result<Decision> first = fixture.engine.submit_request(test::request_of(0x9012, 0x8012, {gpu}));
    LS_CHECK(first.ok());
    const Result<Decision> second = fixture.engine.submit_request(test::request_of(0x9013, 0x8013, {gpu}));
    LS_CHECK(second.ok());
    Explanation first_explanation = first.value().explanation;
    Explanation second_explanation = second.value().explanation;
    first_explanation.decision = DecisionId{};
    first_explanation.request = ScheduleRequestId{};
    first_explanation.experiment = ExperimentId{};
    second_explanation.decision = DecisionId{};
    second_explanation.request = ScheduleRequestId{};
    second_explanation.experiment = ExperimentId{};
    second_explanation.selected.clear();
    first_explanation.selected.clear();
    first_explanation.candidates.clear();
    second_explanation.candidates.clear();
    LS_CHECK_EQ(render_explanation(first_explanation), render_explanation(second_explanation));
}

LS_TEST(no_placement_explains_every_requirement) {
    LabFixture fixture;
    ResourceRequirement gpu = test::requirement_of(ResourceClass::Gpu, "gpu");
    gpu.required_capabilities.add("absent", Limits{});
    ResourceRequirement model = test::requirement_of(ResourceClass::Model, "model");
    model.model = ModelResourceId::from_value(0x1f01);
    model.model_version = "9.9";
    ScheduleRequest request = test::request_of(0x9014, 0x8014, {gpu, model});
    const Result<Decision> decision = fixture.engine.submit_request(request);
    LS_CHECK(decision.ok());
    LS_CHECK_EQ(decision.value().outcome, DecisionOutcome::NoPlacement);
    const std::string text = render_explanation(decision.value().explanation);
    LS_CHECK(text.find("CAPABILITY_MISMATCH") != std::string::npos);
    LS_CHECK(text.find("requirement model") != std::string::npos);
}
