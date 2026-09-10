#pragma once

// Fixture builders shared by the test translation units.

#include <string>
#include <vector>

#include "lab_scheduler/engine.hpp"
#include "lab_scheduler/identity.hpp"
#include "lab_scheduler/request.hpp"
#include "lab_scheduler/resource.hpp"

namespace lab_scheduler {
namespace test {

inline Limits default_limits() { return Limits{}; }

inline ResourceAdvertisement base_resource(ResourceId id, ResourceClass resource_class, std::string name,
                                           WorkerId worker, WorkerBootId boot, const std::string& host) {
    ResourceAdvertisement advertisement;
    advertisement.id = id;
    advertisement.resource_class = resource_class;
    advertisement.name = std::move(name);
    advertisement.worker = worker;
    advertisement.boot = boot;
    advertisement.provenance = Provenance::Synthetic;
    advertisement.sharing = SharingMode::Shared;
    advertisement.health = HealthState::Healthy;
    advertisement.readiness = ReadinessState::Ready;
    advertisement.topology.supplied = true;
    advertisement.topology.host.id = topology_domain_from_name(host);
    advertisement.topology.host.name = host;
    advertisement.capacity.slots = 1;
    advertisement.capacity.max_sessions = 1;
    return advertisement;
}

inline ResourceAdvertisement gpu_resource(std::uint64_t id, std::uint64_t worker, std::uint64_t boot,
                                          const std::string& host, std::uint32_t slots = 2,
                                          bool exclusive = false) {
    ResourceAdvertisement advertisement =
        base_resource(ResourceId::from_value(id), ResourceClass::Gpu, "gpu-" + decimal_u64(id),
                      WorkerId::from_value(worker), WorkerBootId::from_value(boot), host);
    advertisement.capacity.slots = slots;
    advertisement.capacity.max_sessions = slots;
    advertisement.sharing = exclusive ? SharingMode::Exclusive : SharingMode::Shared;
    AcceleratorAttachment accelerator;
    accelerator.id = AcceleratorId::from_value(id + 0x100);
    accelerator.model = "test-accelerator";
    accelerator.memory_bytes = 16ull * 1024ull * 1024ull * 1024ull;
    advertisement.accelerators.push_back(accelerator);
    static_cast<void>(advertisement.capabilities.add("cuda", Limits{}));
    return advertisement;
}

inline ResourceAdvertisement model_resource(std::uint64_t id, std::uint64_t worker, std::uint64_t boot,
                                            const std::string& host, std::uint64_t model_id,
                                            const std::string& version, bool resident) {
    ResourceAdvertisement advertisement =
        base_resource(ResourceId::from_value(id), ResourceClass::Model, "model-" + decimal_u64(id),
                      WorkerId::from_value(worker), WorkerBootId::from_value(boot), host);
    advertisement.capacity.slots = 4;
    advertisement.capacity.max_sessions = 4;
    ModelDescriptor descriptor;
    descriptor.id = ModelResourceId::from_value(model_id);
    descriptor.version = version;
    descriptor.resident = resident;
    descriptor.context_window = 8192;
    advertisement.model = descriptor;
    static_cast<void>(advertisement.capabilities.add("inference", Limits{}));
    return advertisement;
}

inline ResourceAdvertisement dataset_resource(std::uint64_t id, std::uint64_t worker, std::uint64_t boot,
                                              const std::string& host, std::uint64_t dataset_id,
                                              std::uint64_t version) {
    ResourceAdvertisement advertisement =
        base_resource(ResourceId::from_value(id), ResourceClass::Dataset, "dataset-" + decimal_u64(id),
                      WorkerId::from_value(worker), WorkerBootId::from_value(boot), host);
    advertisement.capacity.slots = 2;
    advertisement.capacity.max_sessions = 2;
    DatasetDescriptor descriptor;
    descriptor.id = DatasetId::from_value(dataset_id);
    descriptor.version = DatasetVersionId::from_value(version);
    descriptor.digest = "sha256:test";
    advertisement.dataset = descriptor;
    advertisement.locality.local_dataset_versions.push_back(descriptor.version);
    static_cast<void>(advertisement.capabilities.add("dataset.read", Limits{}));
    return advertisement;
}

inline ResourceAdvertisement simulator_resource(std::uint64_t id, std::uint64_t worker, std::uint64_t boot,
                                                const std::string& host, std::uint64_t simulator_id,
                                                const std::string& version) {
    ResourceAdvertisement advertisement =
        base_resource(ResourceId::from_value(id), ResourceClass::Simulator, "sim-" + decimal_u64(id),
                      WorkerId::from_value(worker), WorkerBootId::from_value(boot), host);
    advertisement.capacity.slots = 2;
    advertisement.capacity.max_sessions = 2;
    SimulatorDescriptor descriptor;
    descriptor.id = SimulatorId::from_value(simulator_id);
    descriptor.version = version;
    descriptor.scenarios = {"steady"};
    descriptor.max_sessions = 2;
    advertisement.simulator = descriptor;
    static_cast<void>(advertisement.capabilities.add("sim", Limits{}));
    return advertisement;
}

inline ResourceAdvertisement virtual_environment_resource(std::uint64_t id, std::uint64_t worker,
                                                          std::uint64_t boot, const std::string& host,
                                                          std::uint64_t environment_id,
                                                          const std::string& digest) {
    ResourceAdvertisement advertisement =
        base_resource(ResourceId::from_value(id), ResourceClass::VirtualEnvironment,
                      "venv-" + decimal_u64(id), WorkerId::from_value(worker), WorkerBootId::from_value(boot),
                      host);
    advertisement.capacity.slots = 4;
    advertisement.capacity.max_sessions = 4;
    EnvironmentDescriptor descriptor;
    descriptor.id = EnvironmentId::from_value(environment_id);
    descriptor.image_digest = digest;
    descriptor.virtual_environment = VirtualEnvironmentId::from_value(id + 0x40);
    advertisement.environment = descriptor;
    return advertisement;
}

inline ResourceAdvertisement physical_rig_resource(std::uint64_t id, std::uint64_t worker, std::uint64_t boot,
                                                   const std::string& host, std::uint64_t environment_id) {
    ResourceAdvertisement advertisement =
        base_resource(ResourceId::from_value(id), ResourceClass::PhysicalEnvironment,
                      "rig-" + decimal_u64(id), WorkerId::from_value(worker), WorkerBootId::from_value(boot),
                      host);
    advertisement.capacity.slots = 1;
    advertisement.capacity.max_sessions = 1;
    advertisement.sharing = SharingMode::Exclusive;
    EnvironmentDescriptor descriptor;
    descriptor.id = EnvironmentId::from_value(environment_id);
    descriptor.physical_environment = PhysicalEnvironmentId::from_value(id + 0x80);
    descriptor.eligibility_token = "calibrated";
    advertisement.environment = descriptor;
    return advertisement;
}

inline ResourceRequirement requirement_of(ResourceClass resource_class, std::string name) {
    ResourceRequirement requirement;
    requirement.name = std::move(name);
    requirement.resource_class = resource_class;
    requirement.min_slots = 1;
    return requirement;
}

inline ScheduleRequest request_of(std::uint64_t id, std::uint64_t experiment,
                                  std::vector<ResourceRequirement> requirements) {
    ScheduleRequest request;
    request.id = ScheduleRequestId::from_value(id);
    request.experiment = ExperimentId::from_value(experiment);
    request.experiment_generation = ExperimentGeneration::from_value(1);
    request.workload = "test-workload";
    request.tenant = "test-tenant";
    request.requirements = std::move(requirements);
    return request;
}

inline Status register_advertisements(SchedulerEngine& engine,
                                      const std::vector<ResourceAdvertisement>& advertisements) {
    for (const ResourceAdvertisement& advertisement : advertisements) {
        Result<ResourceRecord> result = engine.register_resource(advertisement);
        if (!result.ok()) {
            return result.status();
        }
    }
    return Status{};
}

}  // namespace test
}  // namespace lab_scheduler
