// Independent consumer of the installed Lab Scheduler package.
//
// It registers a resource, submits a compound scheduling request, prints the
// decision, cancels the placement, and verifies that capacity accounting
// returns to baseline. It uses only installed public headers and the exported
// namespaced target.

#include <cstdio>
#include <string>
#include <vector>

#include <lab_scheduler/engine.hpp>
#include <lab_scheduler/identity.hpp>
#include <lab_scheduler/inspection.hpp>
#include <lab_scheduler/persistence.hpp>
#include <lab_scheduler/request.hpp>
#include <lab_scheduler/resource.hpp>
#include <lab_scheduler/version.hpp>

using namespace lab_scheduler;

int main() {
    std::printf("consumer lab scheduler version=%s\n", std::string(kVersionString).c_str());

    SchedulerEngine engine;
    const WorkerId worker = WorkerId::from_value(0x77);
    const WorkerBootId boot = WorkerBootId::from_value(0x88);

    ResourceAdvertisement gpu;
    gpu.id = ResourceId::from_value(0x9001);
    gpu.resource_class = ResourceClass::Gpu;
    gpu.name = "consumer-gpu";
    gpu.worker = worker;
    gpu.boot = boot;
    gpu.provenance = Provenance::Synthetic;
    gpu.health = HealthState::Healthy;
    gpu.readiness = ReadinessState::Ready;
    gpu.capacity.slots = 2;
    gpu.capacity.max_sessions = 2;
    gpu.topology.supplied = true;
    gpu.topology.host.id = topology_domain_from_name("consumer-host");
    AcceleratorAttachment accelerator;
    accelerator.id = AcceleratorId::from_value(0x9002);
    accelerator.model = "consumer-accelerator";
    accelerator.memory_bytes = 8ull * 1024ull * 1024ull * 1024ull;
    gpu.accelerators.push_back(accelerator);
    static_cast<void>(gpu.capabilities.add("cuda", Limits{}));

    ResourceAdvertisement dataset;
    dataset.id = ResourceId::from_value(0x9003);
    dataset.resource_class = ResourceClass::Dataset;
    dataset.name = "consumer-dataset";
    dataset.worker = worker;
    dataset.boot = boot;
    dataset.provenance = Provenance::Synthetic;
    dataset.health = HealthState::Healthy;
    dataset.readiness = ReadinessState::Ready;
    dataset.capacity.slots = 2;
    dataset.capacity.max_sessions = 2;
    dataset.topology.supplied = true;
    dataset.topology.host.id = topology_domain_from_name("consumer-host");
    DatasetDescriptor dataset_descriptor;
    dataset_descriptor.id = DatasetId::from_value(0x9004);
    dataset_descriptor.version = DatasetVersionId::from_value(0x9005);
    dataset.dataset = dataset_descriptor;

    if (!engine.register_resource(gpu).ok() || !engine.register_resource(dataset).ok()) {
        std::printf("consumer FAIL registration\n");
        return 1;
    }

    ScheduleRequest request;
    request.id = ScheduleRequestId::from_value(0xa001);
    request.experiment = ExperimentId::from_value(0xb001);
    request.experiment_generation = ExperimentGeneration::from_value(1);
    request.workload = "consumer-workload";
    request.tenant = "consumer-tenant";
    ResourceRequirement accelerator_requirement;
    accelerator_requirement.name = "accelerator";
    accelerator_requirement.resource_class = ResourceClass::Gpu;
    accelerator_requirement.min_slots = 1;
    accelerator_requirement.min_memory_bytes = 4ull * 1024ull * 1024ull * 1024ull;
    static_cast<void>(accelerator_requirement.required_capabilities.add("cuda", Limits{}));
    ResourceRequirement dataset_requirement;
    dataset_requirement.name = "corpus";
    dataset_requirement.resource_class = ResourceClass::Dataset;
    dataset_requirement.min_slots = 1;
    dataset_requirement.dataset = DatasetId::from_value(0x9004);
    dataset_requirement.dataset_version = DatasetVersionId::from_value(0x9005);
    request.requirements = {accelerator_requirement, dataset_requirement};

    const Result<Decision> decision = engine.submit_request(request);
    if (!decision.ok()) {
        std::printf("consumer FAIL submit: %s\n", decision.status().to_string().c_str());
        return 1;
    }
    if (decision.value().outcome != DecisionOutcome::Placed) {
        std::printf("consumer FAIL placement: %s\n", decision.value().failure_detail.c_str());
        return 1;
    }
    std::printf("consumer placed=%zu resources\n", decision.value().placement.resources.size());
    std::printf("%s", render_explanation(decision.value().explanation).c_str());

    const CancellationReport cancelled =
        engine.cancel_placement(decision.value().placement.id, decision.value().placement.generation,
                                engine.epoch(), "consumer release");
    if (!cancelled.status.ok()) {
        std::printf("consumer FAIL cancel: %s\n", cancelled.status.to_string().c_str());
        return 1;
    }
    const AccountingReport accounting = engine.accounting();
    std::printf("consumer reservations_active=%llu reserved_slots=%llu invariants=%s\n",
                static_cast<unsigned long long>(accounting.active_reservations),
                static_cast<unsigned long long>(accounting.total_reserved_slots),
                engine.validate_invariants().all_ok() ? "ok" : "violated");

    Result<std::vector<std::byte>> encoded = encode_durable_state(engine.export_state(), Limits{});
    if (!encoded.ok()) {
        std::printf("consumer FAIL encode\n");
        return 1;
    }
    const Result<DurableState> decoded = decode_durable_state(encoded.value(), Limits{});
    if (!decoded.ok() || !validate_durable_state(decoded.value()).ok()) {
        std::printf("consumer FAIL persistence round trip\n");
        return 1;
    }
    std::printf("consumer persistence bytes=%zu\n", encoded.value().size());
    if (accounting.active_reservations != 0 || accounting.total_reserved_slots != 0 ||
        !engine.validate_invariants().all_ok()) {
        std::printf("consumer FAIL accounting\n");
        return 1;
    }
    std::printf("consumer result PASS\n");
    return 0;
}
