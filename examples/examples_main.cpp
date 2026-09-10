// Compact Lab Scheduler examples.
//
// Each example is a self contained scenario over an in-process scheduler and
// prints deterministic text. The distributed examples (real multiprocess
// placement, worker death, coordinator restart) live in
// lab-scheduler-cluster-proof because they need real OS processes.

#include <cstdio>
#include <string>
#include <vector>

#include "cli_support.hpp"
#include "lab_scheduler/engine.hpp"
#include "lab_scheduler/inspection.hpp"
#include "lab_scheduler/lab_profiles.hpp"
#include "test_support_examples.hpp"

using namespace lab_scheduler;

namespace {

void print_decision(const char* title, const Decision& decision) {
    std::printf("example %s outcome=%s failure=%s\n", title,
                std::string(decision_outcome_name(decision.outcome)).c_str(),
                std::string(error_code_name(decision.failure)).c_str());
    if (decision.outcome == DecisionOutcome::Placed) {
        std::printf("  placement %s generation=%llu\n", decision.placement.id.to_string().c_str(),
                    static_cast<unsigned long long>(decision.placement.generation.value()));
        for (const SelectedResource& selected : decision.placement.resources) {
            std::printf("  selected %s class=%s requirement=%s slots=%u\n",
                        selected.resource.to_string().c_str(),
                        std::string(resource_class_name(selected.resource_class)).c_str(),
                        selected.requirement_name.c_str(), selected.slots);
        }
    } else {
        std::printf("  reason %s\n", decision.failure_detail.c_str());
    }
    std::fflush(stdout);
}

int example_model_gpu() {
    SchedulerEngine engine;
    std::printf("example model-gpu: a model endpoint and an accelerator must be placed together\n");
    static_cast<void>(engine.register_resource(examples::gpu(0x1001, 0x11, 0x12, "host-a", 2)));
    static_cast<void>(engine.register_resource(examples::model(0x1002, 0x11, 0x12, "host-a", 0x1f01, "1.0", true)));
    ResourceRequirement gpu = examples::requirement(ResourceClass::Gpu, "accelerator");
    gpu.min_memory_bytes = 8ull * 1024ull * 1024ull * 1024ull;
    ResourceRequirement model = examples::requirement(ResourceClass::Model, "model");
    model.model = ModelResourceId::from_value(0x1f01);
    model.model_version = "1.0";
    const Result<Decision> decision =
        engine.submit_request(examples::request(0x2001, 0x3001, {gpu, model}));
    if (!decision.ok()) {
        std::printf("error %s\n", decision.status().to_string().c_str());
        return 1;
    }
    print_decision("model-gpu", decision.value());
    return 0;
}

int example_simulator_dataset() {
    SchedulerEngine engine;
    std::printf("example simulator-dataset: a simulator session and an exact dataset version\n");
    static_cast<void>(engine.register_resource(examples::simulator(0x1101, 0x21, 0x22, "host-b", 0x1f02, "2.0")));
    static_cast<void>(engine.register_resource(examples::dataset(0x1102, 0x21, 0x22, "host-b", 0x1f03, 0x2f01)));
    ResourceRequirement simulator = examples::requirement(ResourceClass::Simulator, "simulator");
    simulator.simulator = SimulatorId::from_value(0x1f02);
    simulator.simulator_version = "2.0";
    ResourceRequirement dataset = examples::requirement(ResourceClass::Dataset, "dataset");
    dataset.dataset = DatasetId::from_value(0x1f03);
    dataset.dataset_version = DatasetVersionId::from_value(0x2f01);
    const Result<Decision> decision =
        engine.submit_request(examples::request(0x2002, 0x3002, {simulator, dataset}));
    if (!decision.ok()) {
        return 1;
    }
    print_decision("simulator-dataset", decision.value());

    ResourceRequirement wrong_version = examples::requirement(ResourceClass::Dataset, "dataset");
    wrong_version.dataset = DatasetId::from_value(0x1f03);
    wrong_version.dataset_version = DatasetVersionId::from_value(0x2f09);
    const Result<Decision> rejected =
        engine.submit_request(examples::request(0x2003, 0x3003, {wrong_version}));
    if (rejected.ok()) {
        print_decision("simulator-dataset-wrong-version", rejected.value());
    }
    return 0;
}

int example_dataset_locality() {
    SchedulerEngine engine;
    std::printf("example dataset-locality: two hosts hold different dataset versions\n");
    static_cast<void>(engine.register_resource(examples::gpu(0x1201, 0x31, 0x32, "host-c", 2)));
    static_cast<void>(engine.register_resource(examples::dataset(0x1202, 0x31, 0x32, "host-c", 0x1f04, 0x2f01)));
    static_cast<void>(engine.register_resource(examples::gpu(0x1203, 0x33, 0x34, "host-d", 2)));
    static_cast<void>(engine.register_resource(examples::dataset(0x1204, 0x33, 0x34, "host-d", 0x1f04, 0x2f02)));
    ResourceRequirement gpu = examples::requirement(ResourceClass::Gpu, "accelerator");
    gpu.affinity_group = 1;
    ResourceRequirement dataset = examples::requirement(ResourceClass::Dataset, "corpus");
    dataset.dataset = DatasetId::from_value(0x1f04);
    dataset.dataset_version = DatasetVersionId::from_value(0x2f02);
    dataset.affinity_group = 1;
    AffinityConstraint affinity;
    affinity.group_a = 1;
    affinity.group_b = 1;
    affinity.scope = TopologyScope::Host;
    ScheduleRequest request = examples::request(0x2004, 0x3004, {gpu, dataset});
    request.affinity.push_back(affinity);
    const Result<Decision> decision = engine.submit_request(request);
    if (!decision.ok()) {
        return 1;
    }
    print_decision("dataset-locality", decision.value());
    return 0;
}

int example_topology_aware() {
    SchedulerEngine engine;
    std::printf("example topology-aware: pinned topology domain versus preferred locality\n");
    auto pinned = examples::gpu(0x1301, 0x41, 0x42, "host-e", 2);
    static_cast<void>(engine.register_resource(pinned));
    static_cast<void>(engine.register_resource(examples::gpu(0x1302, 0x43, 0x44, "host-f", 4)));
    ResourceRequirement requirement = examples::requirement(ResourceClass::Gpu, "accelerator");
    requirement.required_domain = topology_domain_from_name("host-f");
    requirement.required_domain_scope = TopologyScope::Host;
    const Result<Decision> decision =
        engine.submit_request(examples::request(0x2005, 0x3005, {requirement}));
    if (!decision.ok()) {
        return 1;
    }
    print_decision("topology-aware", decision.value());
    return 0;
}

int example_exclusive_physical() {
    SchedulerEngine engine;
    std::printf("example exclusive-physical: a SYNTHETIC robotics rig is scheduled exclusively\n");
    static_cast<void>(engine.register_resource(examples::physical_rig(0x1401, 0x51, 0x52, "host-g", 0x1f05)));
    ResourceRequirement rig = examples::requirement(ResourceClass::PhysicalEnvironment, "rig");
    rig.require_exclusive = true;
    rig.physical_environment = PhysicalEnvironmentId::from_value(0x1501);
    ScheduleRequest first = examples::request(0x2006, 0x3006, {rig});
    first.exclusive = true;
    const Result<Decision> placed = engine.submit_request(first);
    if (placed.ok()) {
        print_decision("exclusive-physical-first", placed.value());
    }
    ScheduleRequest second = examples::request(0x2007, 0x3007, {rig});
    second.exclusive = true;
    const Result<Decision> conflict = engine.submit_request(second);
    if (conflict.ok()) {
        print_decision("exclusive-physical-second", conflict.value());
    }
    return 0;
}

int example_virtual_environment() {
    SchedulerEngine engine;
    std::printf("example virtual-environment: a pinned runtime image digest\n");
    static_cast<void>(engine.register_resource(
        examples::virtual_environment(0x1501, 0x61, 0x62, "host-h", 0x1f06, "sha256:runtime-7")));
    ResourceRequirement environment = examples::requirement(ResourceClass::VirtualEnvironment, "environment");
    environment.environment = EnvironmentId::from_value(0x1f06);
    environment.environment_digest = "sha256:runtime-7";
    const Result<Decision> decision =
        engine.submit_request(examples::request(0x2008, 0x3008, {environment}));
    if (!decision.ok()) {
        return 1;
    }
    print_decision("virtual-environment", decision.value());

    environment.environment_digest = "sha256:runtime-8";
    const Result<Decision> wrong_digest =
        engine.submit_request(examples::request(0x2009, 0x3009, {environment}));
    if (wrong_digest.ok()) {
        print_decision("virtual-environment-wrong-digest", wrong_digest.value());
    }
    return 0;
}

int example_no_placement() {
    SchedulerEngine engine;
    std::printf("example no-placement: every rejection category is explained\n");
    static_cast<void>(engine.register_resource(examples::gpu(0x1601, 0x71, 0x72, "host-i", 1)));
    ResourceRequirement requirement = examples::requirement(ResourceClass::Gpu, "accelerator");
    requirement.required_capabilities.add("absent-capability", Limits{});
    const Result<Decision> decision =
        engine.submit_request(examples::request(0x200a, 0x300a, {requirement}));
    if (!decision.ok()) {
        return 1;
    }
    print_decision("no-placement", decision.value());
    std::printf("%s", render_explanation(decision.value().explanation).c_str());
    return 0;
}

int example_cancellation() {
    SchedulerEngine engine;
    std::printf("example cancellation: authority is revoked and capacity returns to baseline\n");
    static_cast<void>(engine.register_resource(examples::gpu(0x1701, 0x81, 0x82, "host-j", 2)));
    ResourceRequirement gpu = examples::requirement(ResourceClass::Gpu, "accelerator");
    const Result<Decision> decision =
        engine.submit_request(examples::request(0x200b, 0x300b, {gpu}));
    if (!decision.ok() || decision.value().outcome != DecisionOutcome::Placed) {
        return 1;
    }
    const CancellationReport cancelled = engine.cancel_placement(
        decision.value().placement.id, decision.value().placement.generation, engine.epoch(), "operator cancel");
    std::printf("example cancellation state=%s released=%zu\n",
                std::string(placement_state_name(cancelled.state)).c_str(),
                cancelled.released_reservations.size());
    std::printf("%s", render_accounting(engine.accounting()).c_str());
    return 0;
}

int example_resource_loss() {
    SchedulerEngine engine;
    std::printf("example resource-loss: incarnation loss invalidates evidence and placements\n");
    static_cast<void>(engine.register_resource(examples::gpu(0x1801, 0x91, 0x92, "host-k", 2)));
    ResourceRequirement gpu = examples::requirement(ResourceClass::Gpu, "accelerator");
    const Result<Decision> decision = engine.submit_request(examples::request(0x200c, 0x300c, {gpu}));
    if (!decision.ok() || decision.value().outcome != DecisionOutcome::Placed) {
        return 1;
    }
    const ResourceLossReport loss =
        engine.mark_worker_lost(WorkerId::from_value(0x91), WorkerBootId::from_value(0x92), "process died");
    std::printf("example resource-loss invalidated=%zu affected=%zu revoked=%zu\n",
                loss.invalidated_resources.size(), loss.affected_placements.size(),
                loss.released_reservations.size());
    std::printf("%s", render_stale_resources(engine.list_stale_resources()).c_str());
    const Result<Decision> again = engine.submit_request(examples::request(0x200d, 0x300d, {gpu}));
    if (again.ok()) {
        print_decision("resource-loss-retry", again.value());
    }
    return 0;
}

int example_coordinator_restart() {
    SchedulerEngine origin;
    std::printf("example coordinator-restart: epoch advances and prior authority is revoked\n");
    static_cast<void>(origin.register_resource(examples::gpu(0x1901, 0xa1, 0xa2, "host-l", 2)));
    ResourceRequirement gpu = examples::requirement(ResourceClass::Gpu, "accelerator");
    const Result<Decision> decision = origin.submit_request(examples::request(0x200e, 0x300e, {gpu}));
    if (!decision.ok() || decision.value().outcome != DecisionOutcome::Placed) {
        return 1;
    }
    SchedulerEngine restarted;
    const Result<RecoveryReport> recovery = restarted.import_state(origin.export_state());
    if (!recovery.ok()) {
        return 1;
    }
    std::printf("example coordinator-restart previous_epoch=%s current_epoch=%s revalidation=%zu reconciled=%zu revoked=%zu\n",
                recovery.value().previous_epoch.to_string().c_str(),
                recovery.value().current_epoch.to_string().c_str(),
                recovery.value().resources_requiring_revalidation.size(),
                recovery.value().reconciled_placements.size(),
                recovery.value().revoked_reservations.size());
    CompletionClaim claim;
    claim.placement = decision.value().placement.id;
    claim.placement_generation = decision.value().placement.generation;
    claim.experiment = decision.value().placement.experiment;
    claim.experiment_generation = decision.value().placement.experiment_generation;
    claim.epoch = origin.epoch();
    const CompletionOutcome outcome = restarted.report_completion(claim);
    std::printf("example coordinator-restart replayed_completion=%s\n",
                std::string(error_code_name(outcome.status.code)).c_str());
    return 0;
}

int example_explanation() {
    SchedulerEngine engine;
    std::printf("example explanation: deterministic named ranking factors\n");
    static_cast<void>(engine.register_resource(examples::gpu(0x1a01, 0xb1, 0xb2, "host-m", 2)));
    static_cast<void>(engine.register_resource(examples::gpu(0x1a02, 0xb3, 0xb4, "host-n", 8)));
    ResourceRequirement gpu = examples::requirement(ResourceClass::Gpu, "accelerator");
    const Result<Decision> decision = engine.submit_request(examples::request(0x200f, 0x300f, {gpu}));
    if (!decision.ok()) {
        return 1;
    }
    std::printf("%s", render_explanation(decision.value().explanation).c_str());
    return 0;
}

struct ExampleEntry {
    const char* name;
    int (*function)();
};

const ExampleEntry kExamples[] = {
    {"model-gpu", example_model_gpu},
    {"simulator-dataset", example_simulator_dataset},
    {"dataset-locality", example_dataset_locality},
    {"topology-aware", example_topology_aware},
    {"exclusive-physical", example_exclusive_physical},
    {"virtual-environment", example_virtual_environment},
    {"no-placement", example_no_placement},
    {"cancellation", example_cancellation},
    {"resource-loss", example_resource_loss},
    {"coordinator-restart", example_coordinator_restart},
    {"explanation", example_explanation},
};

}  // namespace

int main(int argc, char** argv) {
    suppress_error_dialogs();
    const std::vector<std::string> arguments(argv + 1, argv + argc);
    if (arguments.empty() || arguments[0] == "--help") {
        std::printf("usage: lab-scheduler-examples <name>\n");
        for (const ExampleEntry& entry : kExamples) {
            std::printf("  %s\n", entry.name);
        }
        return 0;
    }
    for (const ExampleEntry& entry : kExamples) {
        if (arguments[0] == entry.name) {
            return entry.function();
        }
    }
    std::printf("error unknown example: %s\n", arguments[0].c_str());
    return 2;
}
