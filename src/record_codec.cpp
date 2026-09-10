#include "record_codec.hpp"

#include <string>
#include <utility>
#include <vector>

#include "lab_scheduler/canonical.hpp"
#include "lab_scheduler/codec.hpp"
#include "lab_scheduler/version.hpp"

namespace lab_scheduler {
namespace record_codec {


void write_string(ByteWriter& writer, const std::string& text) { writer.str(text); }

Status read_string(ByteReader& reader, std::string& text, std::uint32_t maximum) {
    return reader.str(text, maximum);
}

void write_enum(ByteWriter& writer, std::uint8_t value) { writer.u8(value); }

template <class Enum>
Status read_enum(ByteReader& reader, Enum& value, std::uint8_t maximum) {
    std::uint8_t raw = 0;
    Status status = reader.u8(raw);
    if (!status.ok()) {
        return status;
    }
    if (raw > maximum) {
        return Status(ErrorCode::CorruptPersistence, "persisted enum value is out of range");
    }
    value = static_cast<Enum>(raw);
    return Status{};
}

void write_limits(ByteWriter& writer, const Limits& limits) {
    writer.u32(limits.max_resources);
    writer.u32(limits.max_capabilities_per_resource);
    writer.u32(limits.max_accelerators_per_resource);
    writer.u32(limits.max_local_datasets_per_resource);
    writer.u32(limits.max_resident_models_per_resource);
    writer.u32(limits.max_simulator_scenarios);
    writer.u32(limits.max_retained_requests);
    writer.u32(limits.max_active_requests);
    writer.u32(limits.max_requirements_per_request);
    writer.u32(limits.max_allowlist_entries);
    writer.u32(limits.max_denylist_entries);
    writer.u32(limits.max_anti_affinity_pairs);
    writer.u32(limits.max_affinity_groups);
    writer.u32(limits.max_candidate_resources_per_requirement);
    writer.u32(limits.max_candidates_evaluated);
    writer.u32(limits.max_candidates_ranked);
    writer.u32(limits.max_rejected_entries_per_requirement);
    writer.u32(limits.max_rejection_reasons_per_entry);
    writer.u32(limits.max_ranking_factors);
    writer.u32(limits.max_placements);
    writer.u32(limits.max_active_placements);
    writer.u32(limits.max_reservations);
    writer.u32(limits.max_active_reservations);
    writer.u32(limits.max_placement_history_per_unit);
    writer.u32(limits.max_audit_records);
    writer.u32(limits.max_topology_records);
    writer.u32(limits.max_topology_depth);
    writer.u32(limits.max_string_length);
    writer.u32(limits.max_digest_length);
    writer.u32(limits.max_explanation_entries);
    writer.u32(limits.max_frame_size);
    writer.u32(limits.max_worker_connections);
    writer.u32(limits.max_inflight_assignments);
    writer.u32(limits.max_persistence_records);
    writer.u64(limits.max_persistence_bytes);
    writer.u32(limits.max_placement_commit_attempts);
}

Status read_limits(ByteReader& reader, Limits& limits) {
    std::uint32_t values[34] = {};
    for (std::uint32_t& value : values) {
        Status status = reader.u32(value);
        if (!status.ok()) {
            return status;
        }
    }
    std::size_t index = 0;
    limits.max_resources = values[index++];
    limits.max_capabilities_per_resource = values[index++];
    limits.max_accelerators_per_resource = values[index++];
    limits.max_local_datasets_per_resource = values[index++];
    limits.max_resident_models_per_resource = values[index++];
    limits.max_simulator_scenarios = values[index++];
    limits.max_retained_requests = values[index++];
    limits.max_active_requests = values[index++];
    limits.max_requirements_per_request = values[index++];
    limits.max_allowlist_entries = values[index++];
    limits.max_denylist_entries = values[index++];
    limits.max_anti_affinity_pairs = values[index++];
    limits.max_affinity_groups = values[index++];
    limits.max_candidate_resources_per_requirement = values[index++];
    limits.max_candidates_evaluated = values[index++];
    limits.max_candidates_ranked = values[index++];
    limits.max_rejected_entries_per_requirement = values[index++];
    limits.max_rejection_reasons_per_entry = values[index++];
    limits.max_ranking_factors = values[index++];
    limits.max_placements = values[index++];
    limits.max_active_placements = values[index++];
    limits.max_reservations = values[index++];
    limits.max_active_reservations = values[index++];
    limits.max_placement_history_per_unit = values[index++];
    limits.max_audit_records = values[index++];
    limits.max_topology_records = values[index++];
    limits.max_topology_depth = values[index++];
    limits.max_string_length = values[index++];
    limits.max_digest_length = values[index++];
    limits.max_explanation_entries = values[index++];
    limits.max_frame_size = values[index++];
    limits.max_worker_connections = values[index++];
    limits.max_inflight_assignments = values[index++];
    limits.max_persistence_records = values[index++];
    Status status = reader.u64(limits.max_persistence_bytes);
    if (!status.ok()) {
        return status;
    }
    return reader.u32(limits.max_placement_commit_attempts);
}

void write_weights(ByteWriter& writer, const RankingWeights& weights) {
    for (std::size_t i = 0; i < kRankingFactorCount; ++i) {
        writer.u32(static_cast<std::uint32_t>(weights.values[i]));
    }
}

Status read_weights(ByteReader& reader, RankingWeights& weights) {
    for (std::size_t i = 0; i < kRankingFactorCount; ++i) {
        std::uint32_t raw = 0;
        Status status = reader.u32(raw);
        if (!status.ok()) {
            return status;
        }
        const std::int32_t value = static_cast<std::int32_t>(raw);
        if (value < -1000 || value > 1000) {
            return Status(ErrorCode::CorruptPersistence, "persisted ranking weight is out of range");
        }
        weights.values[i] = value;
    }
    return Status{};
}

void write_topology_domain(ByteWriter& writer, const TopologyDomain& domain) {
    write_id(writer, domain.id);
    write_string(writer, domain.name);
}

Status read_topology_domain(ByteReader& reader, TopologyDomain& domain, std::uint32_t maximum) {
    Status status = read_id(reader, domain.id);
    if (!status.ok()) {
        return status;
    }
    return read_string(reader, domain.name, maximum);
}

void write_topology(ByteWriter& writer, const TopologyDescriptor& topology) {
    write_topology_domain(writer, topology.host);
    write_topology_domain(writer, topology.numa);
    write_topology_domain(writer, topology.root_complex);
    write_topology_domain(writer, topology.rack);
    write_topology_domain(writer, topology.cluster);
    writer.boolean(topology.supplied);
}

Status read_topology(ByteReader& reader, TopologyDescriptor& topology, std::uint32_t maximum) {
    Status status = read_topology_domain(reader, topology.host, maximum);
    if (!status.ok()) {
        return status;
    }
    status = read_topology_domain(reader, topology.numa, maximum);
    if (!status.ok()) {
        return status;
    }
    status = read_topology_domain(reader, topology.root_complex, maximum);
    if (!status.ok()) {
        return status;
    }
    status = read_topology_domain(reader, topology.rack, maximum);
    if (!status.ok()) {
        return status;
    }
    status = read_topology_domain(reader, topology.cluster, maximum);
    if (!status.ok()) {
        return status;
    }
    return reader.boolean(topology.supplied);
}

void write_capabilities(ByteWriter& writer, const CapabilitySet& capabilities) {
    writer.u32(static_cast<std::uint32_t>(capabilities.size()));
    for (const Capability& capability : capabilities.entries()) {
        write_id(writer, capability.id);
        write_string(writer, capability.name);
    }
}

Status read_capabilities(ByteReader& reader, CapabilitySet& capabilities, const Limits& limits) {
    std::uint32_t count = 0;
    Status status = reader.count(count, limits.max_capabilities_per_resource);
    if (!status.ok()) {
        return status;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        Capability capability;
        status = read_id(reader, capability.id);
        if (!status.ok()) {
            return status;
        }
        status = read_string(reader, capability.name, 64);
        if (!status.ok()) {
            return status;
        }
        if (!(capability_id_from_name(capability.name) == capability.id)) {
            return Status(ErrorCode::CorruptPersistence, "persisted capability identity does not match its name");
        }
        status = capabilities.add(capability, limits);
        if (!status.ok()) {
            return status;
        }
    }
    return Status{};
}

void write_accelerators(ByteWriter& writer, const std::vector<AcceleratorAttachment>& accelerators) {
    writer.u32(static_cast<std::uint32_t>(accelerators.size()));
    for (const AcceleratorAttachment& accelerator : accelerators) {
        write_id(writer, accelerator.id);
        write_string(writer, accelerator.model);
        writer.u64(accelerator.memory_bytes);
        writer.u32(accelerator.compute_capability_major);
        writer.u32(accelerator.compute_capability_minor);
        write_id(writer, accelerator.root_complex);
    }
}

Status read_accelerators(ByteReader& reader, std::vector<AcceleratorAttachment>& accelerators,
                         const Limits& limits) {
    std::uint32_t count = 0;
    Status status = reader.count(count, limits.max_accelerators_per_resource);
    if (!status.ok()) {
        return status;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        AcceleratorAttachment accelerator;
        status = read_id(reader, accelerator.id);
        if (!status.ok()) {
            return status;
        }
        status = read_string(reader, accelerator.model, limits.max_string_length);
        if (!status.ok()) {
            return status;
        }
        status = reader.u64(accelerator.memory_bytes);
        if (!status.ok()) {
            return status;
        }
        status = reader.u32(accelerator.compute_capability_major);
        if (!status.ok()) {
            return status;
        }
        status = reader.u32(accelerator.compute_capability_minor);
        if (!status.ok()) {
            return status;
        }
        status = read_id(reader, accelerator.root_complex);
        if (!status.ok()) {
            return status;
        }
        accelerators.push_back(std::move(accelerator));
    }
    return Status{};
}

template <class Id>
void write_id_list(ByteWriter& writer, const std::vector<Id>& ids) {
    writer.u32(static_cast<std::uint32_t>(ids.size()));
    for (const Id& id : ids) {
        write_id(writer, id);
    }
}

template <class Id>
Status read_id_list(ByteReader& reader, std::vector<Id>& ids, std::uint32_t maximum) {
    std::uint32_t count = 0;
    Status status = reader.count(count, maximum);
    if (!status.ok()) {
        return status;
    }
    Id previous{};
    bool first = true;
    for (std::uint32_t i = 0; i < count; ++i) {
        Id id{};
        status = read_id(reader, id);
        if (!status.ok()) {
            return status;
        }
        if (!first && !(previous < id)) {
            return Status(ErrorCode::CorruptPersistence, "persisted identity list is not canonically ordered");
        }
        first = false;
        previous = id;
        ids.push_back(id);
    }
    return Status{};
}

void write_string_list(ByteWriter& writer, const std::vector<std::string>& values) {
    writer.u32(static_cast<std::uint32_t>(values.size()));
    for (const std::string& value : values) {
        write_string(writer, value);
    }
}

Status read_string_list(ByteReader& reader, std::vector<std::string>& values, std::uint32_t maximum,
                        std::uint32_t maximum_length) {
    std::uint32_t count = 0;
    Status status = reader.count(count, maximum);
    if (!status.ok()) {
        return status;
    }
    std::string previous;
    bool first = true;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::string value;
        status = read_string(reader, value, maximum_length);
        if (!status.ok()) {
            return status;
        }
        if (!first && !(previous < value)) {
            return Status(ErrorCode::CorruptPersistence, "persisted string list is not canonically ordered");
        }
        first = false;
        previous = value;
        values.push_back(std::move(value));
    }
    return Status{};
}

void write_locality(ByteWriter& writer, const LocalityDescriptor& locality, const Limits& limits) {
    static_cast<void>(limits);
    write_id_list(writer, locality.local_dataset_versions);
    write_id_list(writer, locality.resident_models);
    write_id_list(writer, locality.colocated_simulators);
}

Status read_locality(ByteReader& reader, LocalityDescriptor& locality, const Limits& limits) {
    Status status = read_id_list(reader, locality.local_dataset_versions, limits.max_local_datasets_per_resource);
    if (!status.ok()) {
        return status;
    }
    status = read_id_list(reader, locality.resident_models, limits.max_resident_models_per_resource);
    if (!status.ok()) {
        return status;
    }
    return read_id_list(reader, locality.colocated_simulators, limits.max_simulator_scenarios);
}

void write_resource(ByteWriter& writer, const ResourceRecord& resource, const Limits& limits) {
    write_id(writer, resource.id);
    write_enum(writer, static_cast<std::uint8_t>(resource.resource_class));
    write_string(writer, resource.name);
    write_id(writer, resource.worker);
    write_id(writer, resource.boot);
    write_enum(writer, static_cast<std::uint8_t>(resource.provenance));
    write_enum(writer, static_cast<std::uint8_t>(resource.sharing));
    write_capabilities(writer, resource.capabilities);
    writer.u32(resource.capacity.slots);
    writer.u64(resource.capacity.memory_bytes);
    writer.u32(resource.capacity.max_sessions);
    writer.u32(resource.advertised_occupancy);
    write_enum(writer, static_cast<std::uint8_t>(resource.health));
    write_string(writer, resource.health_detail);
    write_enum(writer, static_cast<std::uint8_t>(resource.readiness));
    write_topology(writer, resource.topology);
    write_locality(writer, resource.locality, limits);
    write_accelerators(writer, resource.accelerators);
    writer.boolean(resource.model.has_value());
    if (resource.model.has_value()) {
        write_id(writer, resource.model->id);
        write_string(writer, resource.model->version);
        writer.u32(resource.model->context_window);
        writer.boolean(resource.model->tool_support);
        writer.boolean(resource.model->resident);
        write_capabilities(writer, resource.model->modalities);
    }
    writer.boolean(resource.simulator.has_value());
    if (resource.simulator.has_value()) {
        write_id(writer, resource.simulator->id);
        write_string(writer, resource.simulator->version);
        write_string_list(writer, resource.simulator->scenarios);
        writer.u32(resource.simulator->max_sessions);
    }
    writer.boolean(resource.dataset.has_value());
    if (resource.dataset.has_value()) {
        write_id(writer, resource.dataset->id);
        write_id(writer, resource.dataset->version);
        write_string(writer, resource.dataset->digest);
        writer.u64(resource.dataset->size_bytes);
    }
    writer.boolean(resource.environment.has_value());
    if (resource.environment.has_value()) {
        write_id(writer, resource.environment->id);
        write_string(writer, resource.environment->image_digest);
        write_id(writer, resource.environment->virtual_environment);
        write_id(writer, resource.environment->physical_environment);
        write_string(writer, resource.environment->eligibility_token);
    }
    write_string(writer, resource.tenant);
    write_string_list(writer, resource.labels);
    write_enum(writer, static_cast<std::uint8_t>(resource.lifecycle));
    write_id(writer, resource.generation);
    write_id(writer, resource.health_generation);
    write_id(writer, resource.capacity_generation);
    writer.u64(resource.state_sequence);
    write_id(writer, resource.validated_epoch);
    writer.boolean(resource.dynamic_authoritative);
    write_string(writer, resource.authority_detail);
    writer.u32(resource.reserved_slots);
}

Status read_resource(ByteReader& reader, ResourceRecord& resource, const Limits& limits) {
    Status status = read_id(reader, resource.id);
    if (!status.ok()) {
        return status;
    }
    status = read_enum(reader, resource.resource_class, static_cast<std::uint8_t>(ResourceClass::Worker));
    if (!status.ok()) {
        return status;
    }
    status = read_string(reader, resource.name, limits.max_string_length);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, resource.worker);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, resource.boot);
    if (!status.ok()) {
        return status;
    }
    status = read_enum(reader, resource.provenance, static_cast<std::uint8_t>(Provenance::Real));
    if (!status.ok()) {
        return status;
    }
    status = read_enum(reader, resource.sharing, static_cast<std::uint8_t>(SharingMode::Exclusive));
    if (!status.ok()) {
        return status;
    }
    status = read_capabilities(reader, resource.capabilities, limits);
    if (!status.ok()) {
        return status;
    }
    status = reader.u32(resource.capacity.slots);
    if (!status.ok()) {
        return status;
    }
    status = reader.u64(resource.capacity.memory_bytes);
    if (!status.ok()) {
        return status;
    }
    status = reader.u32(resource.capacity.max_sessions);
    if (!status.ok()) {
        return status;
    }
    status = reader.u32(resource.advertised_occupancy);
    if (!status.ok()) {
        return status;
    }
    status = read_enum(reader, resource.health, static_cast<std::uint8_t>(HealthState::Unhealthy));
    if (!status.ok()) {
        return status;
    }
    status = read_string(reader, resource.health_detail, limits.max_string_length);
    if (!status.ok()) {
        return status;
    }
    status = read_enum(reader, resource.readiness, static_cast<std::uint8_t>(ReadinessState::Unsupported));
    if (!status.ok()) {
        return status;
    }
    status = read_topology(reader, resource.topology, limits.max_string_length);
    if (!status.ok()) {
        return status;
    }
    status = read_locality(reader, resource.locality, limits);
    if (!status.ok()) {
        return status;
    }
    status = read_accelerators(reader, resource.accelerators, limits);
    if (!status.ok()) {
        return status;
    }
    bool present = false;
    status = reader.boolean(present);
    if (!status.ok()) {
        return status;
    }
    if (present) {
        ModelDescriptor model;
        status = read_id(reader, model.id);
        if (!status.ok()) {
            return status;
        }
        status = read_string(reader, model.version, limits.max_digest_length);
        if (!status.ok()) {
            return status;
        }
        status = reader.u32(model.context_window);
        if (!status.ok()) {
            return status;
        }
        status = reader.boolean(model.tool_support);
        if (!status.ok()) {
            return status;
        }
        status = reader.boolean(model.resident);
        if (!status.ok()) {
            return status;
        }
        status = read_capabilities(reader, model.modalities, limits);
        if (!status.ok()) {
            return status;
        }
        resource.model = std::move(model);
    }
    status = reader.boolean(present);
    if (!status.ok()) {
        return status;
    }
    if (present) {
        SimulatorDescriptor simulator;
        status = read_id(reader, simulator.id);
        if (!status.ok()) {
            return status;
        }
        status = read_string(reader, simulator.version, limits.max_digest_length);
        if (!status.ok()) {
            return status;
        }
        status = read_string_list(reader, simulator.scenarios, limits.max_simulator_scenarios,
                                  limits.max_string_length);
        if (!status.ok()) {
            return status;
        }
        status = reader.u32(simulator.max_sessions);
        if (!status.ok()) {
            return status;
        }
        resource.simulator = std::move(simulator);
    }
    status = reader.boolean(present);
    if (!status.ok()) {
        return status;
    }
    if (present) {
        DatasetDescriptor dataset;
        status = read_id(reader, dataset.id);
        if (!status.ok()) {
            return status;
        }
        status = read_id(reader, dataset.version);
        if (!status.ok()) {
            return status;
        }
        status = read_string(reader, dataset.digest, limits.max_digest_length);
        if (!status.ok()) {
            return status;
        }
        status = reader.u64(dataset.size_bytes);
        if (!status.ok()) {
            return status;
        }
        resource.dataset = std::move(dataset);
    }
    status = reader.boolean(present);
    if (!status.ok()) {
        return status;
    }
    if (present) {
        EnvironmentDescriptor environment;
        status = read_id(reader, environment.id);
        if (!status.ok()) {
            return status;
        }
        status = read_string(reader, environment.image_digest, limits.max_digest_length);
        if (!status.ok()) {
            return status;
        }
        status = read_id(reader, environment.virtual_environment);
        if (!status.ok()) {
            return status;
        }
        status = read_id(reader, environment.physical_environment);
        if (!status.ok()) {
            return status;
        }
        status = read_string(reader, environment.eligibility_token, limits.max_string_length);
        if (!status.ok()) {
            return status;
        }
        resource.environment = std::move(environment);
    }
    status = read_string(reader, resource.tenant, limits.max_string_length);
    if (!status.ok()) {
        return status;
    }
    status = read_string_list(reader, resource.labels, limits.max_capabilities_per_resource,
                              limits.max_string_length);
    if (!status.ok()) {
        return status;
    }
    status = read_enum(reader, resource.lifecycle, static_cast<std::uint8_t>(ResourceLifecycle::Lost));
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, resource.generation);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, resource.health_generation);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, resource.capacity_generation);
    if (!status.ok()) {
        return status;
    }
    status = reader.u64(resource.state_sequence);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, resource.validated_epoch);
    if (!status.ok()) {
        return status;
    }
    status = reader.boolean(resource.dynamic_authoritative);
    if (!status.ok()) {
        return status;
    }
    status = read_string(reader, resource.authority_detail, limits.max_string_length);
    if (!status.ok()) {
        return status;
    }
    return reader.u32(resource.reserved_slots);
}

void write_requirement(ByteWriter& writer, const ResourceRequirement& requirement, const Limits& limits) {
    write_string(writer, requirement.name);
    write_enum(writer, static_cast<std::uint8_t>(requirement.resource_class));
    write_capabilities(writer, requirement.required_capabilities);
    writer.u32(requirement.min_slots);
    writer.u64(requirement.min_memory_bytes);
    writer.boolean(requirement.require_exclusive);
    writer.boolean(requirement.require_current_evidence);
    write_enum(writer, static_cast<std::uint8_t>(requirement.minimum_health));
    writer.u32(requirement.max_occupancy_percent);
    writer.boolean(requirement.accelerator.has_value());
    if (requirement.accelerator.has_value()) {
        write_id(writer, *requirement.accelerator);
    }
    writer.boolean(requirement.model.has_value());
    if (requirement.model.has_value()) {
        write_id(writer, *requirement.model);
    }
    write_string(writer, requirement.model_version);
    writer.boolean(requirement.require_model_resident);
    writer.boolean(requirement.simulator.has_value());
    if (requirement.simulator.has_value()) {
        write_id(writer, *requirement.simulator);
    }
    write_string(writer, requirement.simulator_version);
    write_string(writer, requirement.simulator_scenario);
    writer.boolean(requirement.require_simulator_ready);
    writer.boolean(requirement.dataset.has_value());
    if (requirement.dataset.has_value()) {
        write_id(writer, *requirement.dataset);
    }
    writer.boolean(requirement.dataset_version.has_value());
    if (requirement.dataset_version.has_value()) {
        write_id(writer, *requirement.dataset_version);
    }
    writer.boolean(requirement.environment.has_value());
    if (requirement.environment.has_value()) {
        write_id(writer, *requirement.environment);
    }
    write_string(writer, requirement.environment_digest);
    writer.boolean(requirement.physical_environment.has_value());
    if (requirement.physical_environment.has_value()) {
        write_id(writer, *requirement.physical_environment);
    }
    writer.boolean(requirement.virtual_environment.has_value());
    if (requirement.virtual_environment.has_value()) {
        write_id(writer, *requirement.virtual_environment);
    }
    writer.boolean(requirement.required_domain.has_value());
    if (requirement.required_domain.has_value()) {
        write_id(writer, *requirement.required_domain);
    }
    write_enum(writer, static_cast<std::uint8_t>(requirement.required_domain_scope));
    write_id_list(writer, requirement.allowlist);
    write_id_list(writer, requirement.denylist);
    writer.u32(requirement.affinity_group);
    static_cast<void>(limits);
}

template <class Id>
Status read_optional(ByteReader& reader, std::optional<Id>& target) {
    bool present = false;
    Status status = reader.boolean(present);
    if (!status.ok()) {
        return status;
    }
    if (present) {
        Id value{};
        status = read_id(reader, value);
        if (!status.ok()) {
            return status;
        }
        target = value;
    }
    return Status{};
}

Status read_requirement(ByteReader& reader, ResourceRequirement& requirement, const Limits& limits) {
    Status status = read_string(reader, requirement.name, limits.max_string_length);
    if (!status.ok()) {
        return status;
    }
    status = read_enum(reader, requirement.resource_class, static_cast<std::uint8_t>(ResourceClass::Worker));
    if (!status.ok()) {
        return status;
    }
    status = read_capabilities(reader, requirement.required_capabilities, limits);
    if (!status.ok()) {
        return status;
    }
    status = reader.u32(requirement.min_slots);
    if (!status.ok()) {
        return status;
    }
    status = reader.u64(requirement.min_memory_bytes);
    if (!status.ok()) {
        return status;
    }
    status = reader.boolean(requirement.require_exclusive);
    if (!status.ok()) {
        return status;
    }
    status = reader.boolean(requirement.require_current_evidence);
    if (!status.ok()) {
        return status;
    }
    status = read_enum(reader, requirement.minimum_health, static_cast<std::uint8_t>(HealthState::Unhealthy));
    if (!status.ok()) {
        return status;
    }
    status = reader.u32(requirement.max_occupancy_percent);
    if (!status.ok()) {
        return status;
    }
    status = read_optional(reader, requirement.accelerator);
    if (!status.ok()) {
        return status;
    }
    status = read_optional(reader, requirement.model);
    if (!status.ok()) {
        return status;
    }
    status = read_string(reader, requirement.model_version, limits.max_digest_length);
    if (!status.ok()) {
        return status;
    }
    status = reader.boolean(requirement.require_model_resident);
    if (!status.ok()) {
        return status;
    }
    status = read_optional(reader, requirement.simulator);
    if (!status.ok()) {
        return status;
    }
    status = read_string(reader, requirement.simulator_version, limits.max_digest_length);
    if (!status.ok()) {
        return status;
    }
    status = read_string(reader, requirement.simulator_scenario, limits.max_string_length);
    if (!status.ok()) {
        return status;
    }
    status = reader.boolean(requirement.require_simulator_ready);
    if (!status.ok()) {
        return status;
    }
    status = read_optional(reader, requirement.dataset);
    if (!status.ok()) {
        return status;
    }
    status = read_optional(reader, requirement.dataset_version);
    if (!status.ok()) {
        return status;
    }
    status = read_optional(reader, requirement.environment);
    if (!status.ok()) {
        return status;
    }
    status = read_string(reader, requirement.environment_digest, limits.max_digest_length);
    if (!status.ok()) {
        return status;
    }
    status = read_optional(reader, requirement.physical_environment);
    if (!status.ok()) {
        return status;
    }
    status = read_optional(reader, requirement.virtual_environment);
    if (!status.ok()) {
        return status;
    }
    status = read_optional(reader, requirement.required_domain);
    if (!status.ok()) {
        return status;
    }
    status = read_enum(reader, requirement.required_domain_scope,
                       static_cast<std::uint8_t>(TopologyScope::Cluster));
    if (!status.ok()) {
        return status;
    }
    status = read_id_list(reader, requirement.allowlist, limits.max_allowlist_entries);
    if (!status.ok()) {
        return status;
    }
    status = read_id_list(reader, requirement.denylist, limits.max_denylist_entries);
    if (!status.ok()) {
        return status;
    }
    return reader.u32(requirement.affinity_group);
}

void write_request(ByteWriter& writer, const ScheduleRequest& request, const Limits& limits) {
    write_id(writer, request.id);
    write_id(writer, request.experiment);
    write_id(writer, request.experiment_generation);
    write_id(writer, request.trial);
    write_string(writer, request.workload);
    write_string(writer, request.tenant);
    writer.u32(static_cast<std::uint32_t>(request.priority + 2000));
    writer.u32(static_cast<std::uint32_t>(request.requirements.size()));
    for (const ResourceRequirement& requirement : request.requirements) {
        write_requirement(writer, requirement, limits);
    }
    writer.u32(static_cast<std::uint32_t>(request.affinity.size()));
    for (const AffinityConstraint& constraint : request.affinity) {
        writer.u32(constraint.group_a);
        writer.u32(constraint.group_b);
        write_enum(writer, static_cast<std::uint8_t>(constraint.scope));
    }
    writer.u32(static_cast<std::uint32_t>(request.anti_affinity.size()));
    for (const AntiAffinityConstraint& constraint : request.anti_affinity) {
        writer.u32(constraint.group_a);
        writer.u32(constraint.group_b);
        write_enum(writer, static_cast<std::uint8_t>(constraint.scope));
    }
    writer.boolean(request.experiment_anti_affinity);
    writer.boolean(request.exclusive);
    writer.boolean(request.reproducibility_required);
    writer.boolean(request.allow_preemption);
    writer.u32(request.max_reassignments);
    write_weights(writer, request.weights);
    write_string(writer, request.caller_authority);
    write_string(writer, request.request_provenance);
}

Status read_request(ByteReader& reader, ScheduleRequest& request, const Limits& limits) {
    Status status = read_id(reader, request.id);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, request.experiment);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, request.experiment_generation);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, request.trial);
    if (!status.ok()) {
        return status;
    }
    status = read_string(reader, request.workload, limits.max_string_length);
    if (!status.ok()) {
        return status;
    }
    status = read_string(reader, request.tenant, limits.max_string_length);
    if (!status.ok()) {
        return status;
    }
    std::uint32_t priority_raw = 0;
    status = reader.u32(priority_raw);
    if (!status.ok()) {
        return status;
    }
    if (priority_raw < 1000 || priority_raw > 3000) {
        return Status(ErrorCode::CorruptPersistence, "persisted priority is out of range");
    }
    request.priority = static_cast<std::int32_t>(priority_raw) - 2000;
    std::uint32_t requirement_count = 0;
    status = reader.count(requirement_count, limits.max_requirements_per_request);
    if (!status.ok()) {
        return status;
    }
    for (std::uint32_t i = 0; i < requirement_count; ++i) {
        ResourceRequirement requirement;
        status = read_requirement(reader, requirement, limits);
        if (!status.ok()) {
            return status;
        }
        request.requirements.push_back(std::move(requirement));
    }
    std::uint32_t affinity_count = 0;
    status = reader.count(affinity_count, limits.max_anti_affinity_pairs);
    if (!status.ok()) {
        return status;
    }
    for (std::uint32_t i = 0; i < affinity_count; ++i) {
        AffinityConstraint constraint;
        status = reader.u32(constraint.group_a);
        if (!status.ok()) {
            return status;
        }
        status = reader.u32(constraint.group_b);
        if (!status.ok()) {
            return status;
        }
        status = read_enum(reader, constraint.scope, static_cast<std::uint8_t>(TopologyScope::Cluster));
        if (!status.ok()) {
            return status;
        }
        request.affinity.push_back(constraint);
    }
    std::uint32_t anti_count = 0;
    status = reader.count(anti_count, limits.max_anti_affinity_pairs);
    if (!status.ok()) {
        return status;
    }
    for (std::uint32_t i = 0; i < anti_count; ++i) {
        AntiAffinityConstraint constraint;
        status = reader.u32(constraint.group_a);
        if (!status.ok()) {
            return status;
        }
        status = reader.u32(constraint.group_b);
        if (!status.ok()) {
            return status;
        }
        status = read_enum(reader, constraint.scope, static_cast<std::uint8_t>(TopologyScope::Cluster));
        if (!status.ok()) {
            return status;
        }
        request.anti_affinity.push_back(constraint);
    }
    status = reader.boolean(request.experiment_anti_affinity);
    if (!status.ok()) {
        return status;
    }
    status = reader.boolean(request.exclusive);
    if (!status.ok()) {
        return status;
    }
    status = reader.boolean(request.reproducibility_required);
    if (!status.ok()) {
        return status;
    }
    status = reader.boolean(request.allow_preemption);
    if (!status.ok()) {
        return status;
    }
    status = reader.u32(request.max_reassignments);
    if (!status.ok()) {
        return status;
    }
    if (request.max_reassignments > 16) {
        return Status(ErrorCode::CorruptPersistence, "persisted reassignment budget is implausible");
    }
    status = read_weights(reader, request.weights);
    if (!status.ok()) {
        return status;
    }
    status = read_string(reader, request.caller_authority, limits.max_string_length);
    if (!status.ok()) {
        return status;
    }
    return read_string(reader, request.request_provenance, limits.max_string_length);
}

void write_stored_request(ByteWriter& writer, const StoredRequest& stored, const Limits& limits) {
    write_request(writer, stored.request, limits);
    write_enum(writer, static_cast<std::uint8_t>(stored.state));
    writer.u64(stored.submitted_sequence);
    write_id(writer, stored.current_placement);
    writer.u32(stored.placement_count);
    write_string(writer, stored.detail);
}

Status read_stored_request(ByteReader& reader, StoredRequest& stored, const Limits& limits) {
    Status status = read_request(reader, stored.request, limits);
    if (!status.ok()) {
        return status;
    }
    status = read_enum(reader, stored.state, static_cast<std::uint8_t>(RequestState::NoPlacement));
    if (!status.ok()) {
        return status;
    }
    status = reader.u64(stored.submitted_sequence);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, stored.current_placement);
    if (!status.ok()) {
        return status;
    }
    status = reader.u32(stored.placement_count);
    if (!status.ok()) {
        return status;
    }
    return read_string(reader, stored.detail, limits.max_string_length);
}

void write_selected(ByteWriter& writer, const SelectedResource& selected) {
    write_id(writer, selected.resource);
    write_enum(writer, static_cast<std::uint8_t>(selected.resource_class));
    write_id(writer, selected.generation);
    writer.u32(selected.slots);
    writer.boolean(selected.exclusive);
    write_string(writer, selected.requirement_name);
    write_id(writer, selected.worker);
    write_id(writer, selected.worker_boot);
    write_string(writer, selected.resource_name);
}

Status read_selected(ByteReader& reader, SelectedResource& selected, const Limits& limits) {
    Status status = read_id(reader, selected.resource);
    if (!status.ok()) {
        return status;
    }
    status = read_enum(reader, selected.resource_class, static_cast<std::uint8_t>(ResourceClass::Worker));
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, selected.generation);
    if (!status.ok()) {
        return status;
    }
    status = reader.u32(selected.slots);
    if (!status.ok()) {
        return status;
    }
    status = reader.boolean(selected.exclusive);
    if (!status.ok()) {
        return status;
    }
    status = read_string(reader, selected.requirement_name, limits.max_string_length);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, selected.worker);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, selected.worker_boot);
    if (!status.ok()) {
        return status;
    }
    return read_string(reader, selected.resource_name, limits.max_string_length);
}

void write_placement(ByteWriter& writer, const PlacementRecord& placement) {
    write_id(writer, placement.id);
    write_id(writer, placement.generation);
    write_id(writer, placement.request);
    write_id(writer, placement.decision);
    write_id(writer, placement.experiment);
    write_id(writer, placement.experiment_generation);
    write_id(writer, placement.trial);
    write_id(writer, placement.epoch);
    writer.u32(static_cast<std::uint32_t>(placement.resources.size()));
    for (const SelectedResource& selected : placement.resources) {
        write_selected(writer, selected);
    }
    write_id(writer, placement.reservation);
    write_id(writer, placement.lease);
    write_id(writer, placement.worker);
    write_id(writer, placement.worker_boot);
    write_enum(writer, static_cast<std::uint8_t>(placement.state));
    writer.u32(placement.reassignment_count);
    writer.boolean(placement.completion_recorded);
    write_string(writer, placement.detail);
    writer.u64(placement.transition_sequence);
    write_string(writer, placement.completion_detail);
}

Status read_placement(ByteReader& reader, PlacementRecord& placement, const Limits& limits) {
    Status status = read_id(reader, placement.id);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, placement.generation);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, placement.request);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, placement.decision);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, placement.experiment);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, placement.experiment_generation);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, placement.trial);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, placement.epoch);
    if (!status.ok()) {
        return status;
    }
    std::uint32_t count = 0;
    status = reader.count(count, limits.max_requirements_per_request);
    if (!status.ok()) {
        return status;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        SelectedResource selected;
        status = read_selected(reader, selected, limits);
        if (!status.ok()) {
            return status;
        }
        placement.resources.push_back(std::move(selected));
    }
    status = read_id(reader, placement.reservation);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, placement.lease);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, placement.worker);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, placement.worker_boot);
    if (!status.ok()) {
        return status;
    }
    status = read_enum(reader, placement.state, static_cast<std::uint8_t>(PlacementState::Lost));
    if (!status.ok()) {
        return status;
    }
    status = reader.u32(placement.reassignment_count);
    if (!status.ok()) {
        return status;
    }
    if (placement.reassignment_count > 16) {
        return Status(ErrorCode::CorruptPersistence, "persisted reassignment count is implausible");
    }
    status = reader.boolean(placement.completion_recorded);
    if (!status.ok()) {
        return status;
    }
    status = read_string(reader, placement.detail, limits.max_string_length);
    if (!status.ok()) {
        return status;
    }
    status = reader.u64(placement.transition_sequence);
    if (!status.ok()) {
        return status;
    }
    return read_string(reader, placement.completion_detail, limits.max_string_length);
}

void write_reservation(ByteWriter& writer, const ReservationRecord& reservation) {
    write_id(writer, reservation.id);
    write_id(writer, reservation.lease);
    write_id(writer, reservation.placement);
    write_id(writer, reservation.placement_generation);
    write_id(writer, reservation.request);
    write_id(writer, reservation.experiment);
    write_id(writer, reservation.experiment_generation);
    write_id(writer, reservation.epoch);
    writer.u32(static_cast<std::uint32_t>(reservation.claims.size()));
    for (const ReservationClaim& claim : reservation.claims) {
        write_id(writer, claim.resource);
        writer.u32(claim.slots);
        writer.boolean(claim.exclusive);
        write_id(writer, claim.generation_at_acquire);
    }
    write_enum(writer, static_cast<std::uint8_t>(reservation.state));
    writer.u64(reservation.acquire_sequence);
    write_string(writer, reservation.detail);
}

Status read_reservation(ByteReader& reader, ReservationRecord& reservation, const Limits& limits) {
    Status status = read_id(reader, reservation.id);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, reservation.lease);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, reservation.placement);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, reservation.placement_generation);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, reservation.request);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, reservation.experiment);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, reservation.experiment_generation);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, reservation.epoch);
    if (!status.ok()) {
        return status;
    }
    std::uint32_t count = 0;
    status = reader.count(count, limits.max_requirements_per_request);
    if (!status.ok()) {
        return status;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        ReservationClaim claim;
        status = read_id(reader, claim.resource);
        if (!status.ok()) {
            return status;
        }
        status = reader.u32(claim.slots);
        if (!status.ok()) {
            return status;
        }
        status = reader.boolean(claim.exclusive);
        if (!status.ok()) {
            return status;
        }
        status = read_id(reader, claim.generation_at_acquire);
        if (!status.ok()) {
            return status;
        }
        reservation.claims.push_back(claim);
    }
    status = read_enum(reader, reservation.state, static_cast<std::uint8_t>(ReservationState::Revoked));
    if (!status.ok()) {
        return status;
    }
    status = reader.u64(reservation.acquire_sequence);
    if (!status.ok()) {
        return status;
    }
    return read_string(reader, reservation.detail, limits.max_string_length);
}

void write_authority_envelope(ByteWriter& writer, const AuthorityEnvelope& authority) {
    write_id(writer, authority.request);
    write_id(writer, authority.experiment);
    write_id(writer, authority.experiment_generation);
    write_id(writer, authority.placement);
    write_id(writer, authority.placement_generation);
    write_id(writer, authority.epoch);
    write_id(writer, authority.lease);
    write_id(writer, authority.reservation);
    write_id(writer, authority.worker);
    write_id(writer, authority.worker_boot);
    writer.u32(static_cast<std::uint32_t>(authority.resources.size()));
    for (const SelectedResource& selected : authority.resources) {
        write_selected(writer, selected);
    }
}

Status read_authority_envelope(ByteReader& reader, AuthorityEnvelope& authority, const Limits& limits) {
    Status status = read_id(reader, authority.request);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, authority.experiment);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, authority.experiment_generation);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, authority.placement);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, authority.placement_generation);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, authority.epoch);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, authority.lease);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, authority.reservation);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, authority.worker);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, authority.worker_boot);
    if (!status.ok()) {
        return status;
    }
    std::uint32_t count = 0;
    status = reader.count(count, limits.max_requirements_per_request);
    if (!status.ok()) {
        return status;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        SelectedResource selected;
        status = read_selected(reader, selected, limits);
        if (!status.ok()) {
            return status;
        }
        authority.resources.push_back(std::move(selected));
    }
    return Status{};
}

void write_completion_claim(ByteWriter& writer, const CompletionClaim& claim) {
    write_id(writer, claim.placement);
    write_id(writer, claim.placement_generation);
    write_id(writer, claim.experiment);
    write_id(writer, claim.experiment_generation);
    write_id(writer, claim.epoch);
    write_id(writer, claim.worker_boot);
    writer.u32(static_cast<std::uint32_t>(claim.resources.size()));
    for (const ResourceId& resource : claim.resources) {
        write_id(writer, resource);
    }
    writer.u32(static_cast<std::uint32_t>(claim.resource_generations.size()));
    for (const ResourceGeneration& generation : claim.resource_generations) {
        write_id(writer, generation);
    }
    writer.boolean(claim.succeeded);
    write_string(writer, claim.detail);
}

Status read_completion_claim(ByteReader& reader, CompletionClaim& claim, const Limits& limits) {
    Status status = read_id(reader, claim.placement);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, claim.placement_generation);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, claim.experiment);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, claim.experiment_generation);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, claim.epoch);
    if (!status.ok()) {
        return status;
    }
    status = read_id(reader, claim.worker_boot);
    if (!status.ok()) {
        return status;
    }
    std::uint32_t count = 0;
    status = reader.count(count, limits.max_requirements_per_request);
    if (!status.ok()) {
        return status;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        ResourceId resource{};
        status = read_id(reader, resource);
        if (!status.ok()) {
            return status;
        }
        claim.resources.push_back(resource);
    }
    status = reader.count(count, limits.max_requirements_per_request);
    if (!status.ok()) {
        return status;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        ResourceGeneration generation{};
        status = read_id(reader, generation);
        if (!status.ok()) {
            return status;
        }
        claim.resource_generations.push_back(generation);
    }
    status = reader.boolean(claim.succeeded);
    if (!status.ok()) {
        return status;
    }
    return read_string(reader, claim.detail, limits.max_string_length);
}

ResourceRecord advertisement_to_record(const ResourceAdvertisement& advertisement) {
    ResourceRecord record;
    record.id = advertisement.id;
    record.resource_class = advertisement.resource_class;
    record.name = advertisement.name;
    record.worker = advertisement.worker;
    record.boot = advertisement.boot;
    record.provenance = advertisement.provenance;
    record.sharing = advertisement.sharing;
    record.capabilities = advertisement.capabilities;
    record.capacity = advertisement.capacity;
    record.advertised_occupancy = advertisement.current_occupancy;
    record.health = advertisement.health;
    record.health_detail = advertisement.health_detail;
    record.readiness = advertisement.readiness;
    record.topology = advertisement.topology;
    record.locality = advertisement.locality;
    record.accelerators = advertisement.accelerators;
    record.model = advertisement.model;
    record.simulator = advertisement.simulator;
    record.dataset = advertisement.dataset;
    record.environment = advertisement.environment;
    record.tenant = advertisement.tenant;
    record.labels = advertisement.labels;
    return record;
}

ResourceAdvertisement record_to_advertisement(const ResourceRecord& record) {
    ResourceAdvertisement advertisement;
    advertisement.id = record.id;
    advertisement.resource_class = record.resource_class;
    advertisement.name = record.name;
    advertisement.worker = record.worker;
    advertisement.boot = record.boot;
    advertisement.provenance = record.provenance;
    advertisement.sharing = record.sharing;
    advertisement.capabilities = record.capabilities;
    advertisement.capacity = record.capacity;
    advertisement.current_occupancy = record.advertised_occupancy;
    advertisement.health = record.health;
    advertisement.health_detail = record.health_detail;
    advertisement.readiness = record.readiness;
    advertisement.topology = record.topology;
    advertisement.locality = record.locality;
    advertisement.accelerators = record.accelerators;
    advertisement.model = record.model;
    advertisement.simulator = record.simulator;
    advertisement.dataset = record.dataset;
    advertisement.environment = record.environment;
    advertisement.tenant = record.tenant;
    advertisement.labels = record.labels;
    return advertisement;
}

void write_advertisement(ByteWriter& writer, const ResourceAdvertisement& advertisement, const Limits& limits) {
    write_resource(writer, advertisement_to_record(advertisement), limits);
}

Status read_advertisement(ByteReader& reader, ResourceAdvertisement& advertisement, const Limits& limits) {
    ResourceRecord record;
    Status status = read_resource(reader, record, limits);
    if (!status.ok()) {
        return status;
    }
    advertisement = record_to_advertisement(record);
    return Status{};
}

}  // namespace record_codec
}  // namespace lab_scheduler
