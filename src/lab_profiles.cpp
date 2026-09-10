#include "lab_scheduler/lab_profiles.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "lab_scheduler/canonical.hpp"
#include "lab_scheduler/identity.hpp"

namespace lab_scheduler {
namespace {

CapabilitySet capability_set(std::initializer_list<std::string_view> names, const Limits& limits) {
    CapabilitySet set;
    for (const std::string_view name : names) {
        static_cast<void>(set.add(name, limits));
    }
    return set;
}

ResourceAdvertisement base_advertisement(ResourceId id, ResourceClass resource_class, std::string name,
                                         WorkerId worker, WorkerBootId boot, TopologyDomainId host) {
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
    advertisement.topology.host.id = host;
    advertisement.topology.host.name = host.to_string();
    advertisement.current_occupancy = 0;
    return advertisement;
}

}  // namespace

std::vector<std::string> profile_names() { return {"alpha", "beta", "gamma"}; }

Result<ProfileBlueprint> make_profile(const std::string& name, WorkerBootId boot,
                                      std::uint32_t current_occupancy) {
    Status status = boot.validate();
    if (!status.ok()) {
        return status;
    }
    const Limits limits;
    ProfileBlueprint blueprint;
    blueprint.name = name;
    blueprint.boot = boot;

    if (name == "alpha") {
        const WorkerId worker = WorkerId::from_value(kAlphaWorkerId);
        const TopologyDomainId host = topology_domain_from_name("lab-host-alpha");
        blueprint.worker = worker;

        ResourceAdvertisement gpu = base_advertisement(ResourceId::from_value(kAlphaGpuResource),
                                                       ResourceClass::Gpu, "alpha-gpu-0", worker, boot, host);
        gpu.capabilities = capability_set({"cuda", "gpu.memory.24gb", "training", "inference"}, limits);
        gpu.capacity.slots = 2;
        gpu.capacity.memory_bytes = 24ull * 1024ull * 1024ull * 1024ull;
        gpu.capacity.max_sessions = 2;
        gpu.current_occupancy = current_occupancy;
        gpu.sharing = SharingMode::Shared;
        AcceleratorAttachment accelerator;
        accelerator.id = AcceleratorId::from_value(0x1a01);
        accelerator.model = "synthetic-accelerator-alpha";
        accelerator.memory_bytes = 24ull * 1024ull * 1024ull * 1024ull;
        accelerator.compute_capability_major = 9;
        accelerator.compute_capability_minor = 0;
        gpu.accelerators.push_back(accelerator);
        blueprint.resources.push_back(gpu);

        ResourceAdvertisement model = base_advertisement(ResourceId::from_value(kAlphaModelResource),
                                                         ResourceClass::Model, "alpha-model-endpoint", worker,
                                                         boot, host);
        model.capabilities = capability_set({"inference", "model.chat"}, limits);
        model.capacity.slots = 4;
        model.capacity.max_sessions = 4;
        ModelDescriptor descriptor;
        descriptor.id = ModelResourceId::from_value(kTrainingModelId);
        descriptor.version = "1.4.0";
        descriptor.context_window = 32768;
        descriptor.tool_support = true;
        descriptor.resident = true;
        descriptor.modalities = capability_set({"text"}, limits);
        model.model = descriptor;
        blueprint.resources.push_back(model);

        ResourceAdvertisement dataset = base_advertisement(ResourceId::from_value(kAlphaDatasetResource),
                                                           ResourceClass::Dataset, "alpha-corpus-v1", worker, boot,
                                                           host);
        dataset.capabilities = capability_set({"dataset.read"}, limits);
        dataset.capacity.slots = 4;
        dataset.capacity.max_sessions = 4;
        DatasetDescriptor dataset_descriptor;
        dataset_descriptor.id = DatasetId::from_value(kCorpusDatasetId);
        dataset_descriptor.version = DatasetVersionId::from_value(kCorpusDatasetVersion1);
        dataset_descriptor.digest = "sha256:alpha-corpus-v1";
        dataset_descriptor.size_bytes = 64ull * 1024ull * 1024ull;
        dataset.dataset = dataset_descriptor;
        dataset.locality.local_dataset_versions.push_back(dataset_descriptor.version);
        blueprint.resources.push_back(dataset);

        // The model is resident on the same host as the accelerator: locality is
        // supplied, never inferred by the scheduler.
        blueprint.resources[1].locality.resident_models.push_back(ModelResourceId::from_value(kTrainingModelId));
        return blueprint;
    }

    if (name == "beta") {
        const WorkerId worker = WorkerId::from_value(kBetaWorkerId);
        const TopologyDomainId host = topology_domain_from_name("lab-host-beta");
        blueprint.worker = worker;

        ResourceAdvertisement gpu = base_advertisement(ResourceId::from_value(kBetaGpuResource),
                                                       ResourceClass::Gpu, "beta-gpu-0", worker, boot, host);
        gpu.capabilities = capability_set({"cuda", "gpu.memory.16gb", "simulation"}, limits);
        gpu.capacity.slots = 1;
        gpu.capacity.memory_bytes = 16ull * 1024ull * 1024ull * 1024ull;
        gpu.capacity.max_sessions = 1;
        gpu.current_occupancy = current_occupancy;
        gpu.sharing = SharingMode::Exclusive;
        AcceleratorAttachment accelerator;
        accelerator.id = AcceleratorId::from_value(0x1a02);
        accelerator.model = "synthetic-accelerator-beta";
        accelerator.memory_bytes = 16ull * 1024ull * 1024ull * 1024ull;
        accelerator.compute_capability_major = 8;
        accelerator.compute_capability_minor = 6;
        gpu.accelerators.push_back(accelerator);
        blueprint.resources.push_back(gpu);

        ResourceAdvertisement simulator = base_advertisement(ResourceId::from_value(kBetaSimulatorResource),
                                                             ResourceClass::Simulator, "beta-grid-simulator",
                                                             worker, boot, host);
        simulator.capabilities = capability_set({"simulation", "sim.grid"}, limits);
        simulator.capacity.slots = 2;
        simulator.capacity.max_sessions = 2;
        SimulatorDescriptor simulator_descriptor;
        simulator_descriptor.id = SimulatorId::from_value(kGridSimulatorId);
        simulator_descriptor.version = "2.3.1";
        simulator_descriptor.scenarios = {"grid-fault", "grid-steady"};
        simulator_descriptor.max_sessions = 2;
        simulator.simulator = simulator_descriptor;
        simulator.locality.colocated_simulators.push_back(simulator_descriptor.id);
        blueprint.resources.push_back(simulator);

        ResourceAdvertisement dataset = base_advertisement(ResourceId::from_value(kBetaDatasetResource),
                                                           ResourceClass::Dataset, "beta-corpus-v2", worker, boot,
                                                           host);
        dataset.capabilities = capability_set({"dataset.read"}, limits);
        dataset.capacity.slots = 2;
        dataset.capacity.max_sessions = 2;
        DatasetDescriptor dataset_descriptor;
        dataset_descriptor.id = DatasetId::from_value(kCorpusDatasetId);
        dataset_descriptor.version = DatasetVersionId::from_value(kCorpusDatasetVersion2);
        dataset_descriptor.digest = "sha256:beta-corpus-v2";
        dataset_descriptor.size_bytes = 96ull * 1024ull * 1024ull;
        dataset.dataset = dataset_descriptor;
        dataset.locality.local_dataset_versions.push_back(dataset_descriptor.version);
        blueprint.resources.push_back(dataset);
        return blueprint;
    }

    if (name == "gamma") {
        const WorkerId worker = WorkerId::from_value(kGammaWorkerId);
        const TopologyDomainId host = topology_domain_from_name("lab-host-gamma");
        blueprint.worker = worker;

        ResourceAdvertisement environment =
            base_advertisement(ResourceId::from_value(kGammaVirtualEnvironmentResource),
                               ResourceClass::VirtualEnvironment, "gamma-runtime-image", worker, boot, host);
        environment.capabilities = capability_set({"runtime.container", "runtime.image.pinned"}, limits);
        environment.capacity.slots = 4;
        environment.capacity.max_sessions = 4;
        environment.current_occupancy = current_occupancy;
        EnvironmentDescriptor environment_descriptor;
        environment_descriptor.id = EnvironmentId::from_value(kRoboticsEnvironmentId);
        environment_descriptor.image_digest = "sha256:gamma-runtime-1";
        environment_descriptor.virtual_environment =
            VirtualEnvironmentId::from_value(kRoboticsVirtualEnvironmentId);
        environment.environment = environment_descriptor;
        blueprint.resources.push_back(environment);

        ResourceAdvertisement rig = base_advertisement(ResourceId::from_value(kGammaPhysicalRigResource),
                                                       ResourceClass::PhysicalEnvironment, "gamma-robotics-rig",
                                                       worker, boot, host);
        rig.capabilities = capability_set({"rig.robotics", "rig.calibrated"}, limits);
        rig.capacity.slots = 1;
        rig.capacity.max_sessions = 1;
        rig.sharing = SharingMode::Exclusive;
        rig.provenance = Provenance::Synthetic;
        EnvironmentDescriptor rig_descriptor;
        rig_descriptor.id = EnvironmentId::from_value(kRoboticsEnvironmentId);
        rig_descriptor.physical_environment =
            PhysicalEnvironmentId::from_value(kRoboticsPhysicalEnvironmentId);
        rig_descriptor.eligibility_token = "calibrated";
        rig.environment = rig_descriptor;
        blueprint.resources.push_back(rig);
        return blueprint;
    }

    return Status(ErrorCode::NotFound, "unknown lab profile: " + name);
}

}  // namespace lab_scheduler
