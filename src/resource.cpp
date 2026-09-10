#include "lab_scheduler/resource.hpp"

#include <algorithm>
#include <array>

namespace lab_scheduler {
namespace {

struct ClassName {
    ResourceClass value;
    std::string_view name;
};

constexpr std::array<ClassName, 7> kClassNames{{
    {ResourceClass::Model, "MODEL"},
    {ResourceClass::Gpu, "GPU"},
    {ResourceClass::Simulator, "SIMULATOR"},
    {ResourceClass::Dataset, "DATASET"},
    {ResourceClass::PhysicalEnvironment, "PHYSICAL_ENVIRONMENT"},
    {ResourceClass::VirtualEnvironment, "VIRTUAL_ENVIRONMENT"},
    {ResourceClass::Worker, "WORKER"},
}};

Status check_text(const std::string& value, std::string_view field, std::uint32_t maximum,
                  bool allow_empty = true) {
    if (!allow_empty && value.empty()) {
        std::string message(field);
        message += " must not be empty";
        return Status(ErrorCode::InvalidArgument, std::move(message));
    }
    if (value.size() > maximum) {
        std::string message(field);
        message += " exceeds the configured maximum length";
        return Status(ErrorCode::InvalidArgument, std::move(message));
    }
    return Status{};
}

template <class T>
Status check_unique_sorted(std::vector<T>& values, std::string_view field) {
    std::sort(values.begin(), values.end());
    for (std::size_t i = 1; i < values.size(); ++i) {
        if (values[i] == values[i - 1]) {
            std::string message(field);
            message += " contains a duplicate identity";
            return Status(ErrorCode::DuplicateIdentity, std::move(message));
        }
    }
    return Status{};
}

}  // namespace

std::string_view resource_class_name(ResourceClass value) noexcept {
    for (const auto& entry : kClassNames) {
        if (entry.value == value) {
            return entry.name;
        }
    }
    return "UNKNOWN_CLASS";
}

std::optional<ResourceClass> parse_resource_class(std::string_view text) noexcept {
    for (const auto& entry : kClassNames) {
        if (entry.name == text) {
            return entry.value;
        }
    }
    return std::nullopt;
}

std::string_view sharing_mode_name(SharingMode value) noexcept {
    return value == SharingMode::Exclusive ? std::string_view("EXCLUSIVE") : std::string_view("SHARED");
}

std::optional<SharingMode> parse_sharing_mode(std::string_view text) noexcept {
    if (text == "SHARED") {
        return SharingMode::Shared;
    }
    if (text == "EXCLUSIVE") {
        return SharingMode::Exclusive;
    }
    return std::nullopt;
}

std::string_view health_state_name(HealthState value) noexcept {
    switch (value) {
        case HealthState::Unknown:
            return "UNKNOWN";
        case HealthState::Healthy:
            return "HEALTHY";
        case HealthState::Degraded:
            return "DEGRADED";
        case HealthState::Unhealthy:
            return "UNHEALTHY";
    }
    return "UNKNOWN";
}

std::optional<HealthState> parse_health_state(std::string_view text) noexcept {
    if (text == "UNKNOWN") {
        return HealthState::Unknown;
    }
    if (text == "HEALTHY") {
        return HealthState::Healthy;
    }
    if (text == "DEGRADED") {
        return HealthState::Degraded;
    }
    if (text == "UNHEALTHY") {
        return HealthState::Unhealthy;
    }
    return std::nullopt;
}

std::string_view readiness_state_name(ReadinessState value) noexcept {
    switch (value) {
        case ReadinessState::Unknown:
            return "UNKNOWN";
        case ReadinessState::Ready:
            return "READY";
        case ReadinessState::NotReady:
            return "NOT_READY";
        case ReadinessState::Unsupported:
            return "UNSUPPORTED";
    }
    return "UNKNOWN";
}

std::optional<ReadinessState> parse_readiness_state(std::string_view text) noexcept {
    if (text == "UNKNOWN") {
        return ReadinessState::Unknown;
    }
    if (text == "READY") {
        return ReadinessState::Ready;
    }
    if (text == "NOT_READY") {
        return ReadinessState::NotReady;
    }
    if (text == "UNSUPPORTED") {
        return ReadinessState::Unsupported;
    }
    return std::nullopt;
}

std::string_view provenance_name(Provenance value) noexcept {
    switch (value) {
        case Provenance::Unsupported:
            return "UNSUPPORTED";
        case Provenance::Synthetic:
            return "SYNTHETIC";
        case Provenance::Real:
            return "REAL";
    }
    return "UNSUPPORTED";
}

std::optional<Provenance> parse_provenance(std::string_view text) noexcept {
    if (text == "UNSUPPORTED") {
        return Provenance::Unsupported;
    }
    if (text == "SYNTHETIC") {
        return Provenance::Synthetic;
    }
    if (text == "REAL") {
        return Provenance::Real;
    }
    return std::nullopt;
}

std::string_view resource_lifecycle_name(ResourceLifecycle value) noexcept {
    switch (value) {
        case ResourceLifecycle::Registered:
            return "REGISTERED";
        case ResourceLifecycle::Current:
            return "CURRENT";
        case ResourceLifecycle::Retired:
            return "RETIRED";
        case ResourceLifecycle::Lost:
            return "LOST";
    }
    return "REGISTERED";
}

std::optional<ResourceLifecycle> parse_resource_lifecycle(std::string_view text) noexcept {
    if (text == "REGISTERED") {
        return ResourceLifecycle::Registered;
    }
    if (text == "CURRENT") {
        return ResourceLifecycle::Current;
    }
    if (text == "RETIRED") {
        return ResourceLifecycle::Retired;
    }
    if (text == "LOST") {
        return ResourceLifecycle::Lost;
    }
    return std::nullopt;
}

Status validate_advertisement(const ResourceAdvertisement& advertisement, const Limits& limits) {
    Status status = advertisement.id.validate();
    if (!status.ok()) {
        return status;
    }
    status = check_text(advertisement.name, "resource name", limits.max_string_length, false);
    if (!status.ok()) {
        return status;
    }
    status = check_text(advertisement.health_detail, "health detail", limits.max_string_length);
    if (!status.ok()) {
        return status;
    }
    status = check_text(advertisement.tenant, "tenant", limits.max_string_length);
    if (!status.ok()) {
        return status;
    }
    if (!advertisement.worker.is_null() && advertisement.boot.is_null()) {
        return Status(ErrorCode::InvalidIdentity,
                      "worker owned resource must carry the boot identity of its incarnation");
    }
    if (advertisement.worker.is_null() && !advertisement.boot.is_null()) {
        return Status(ErrorCode::InvalidIdentity,
                      "boot identity supplied for a resource that has no worker owner");
    }
    status = advertisement.capabilities.validate(limits);
    if (!status.ok()) {
        return status;
    }
    if (advertisement.capacity.slots == 0) {
        return Status(ErrorCode::InvalidArgument, "resource capacity must declare at least one slot");
    }
    if (advertisement.capacity.max_sessions == 0) {
        return Status(ErrorCode::InvalidArgument, "resource max_sessions must be at least one");
    }
    if (advertisement.current_occupancy > advertisement.capacity.slots) {
        return Status(ErrorCode::InvalidArgument, "advertised occupancy exceeds advertised capacity");
    }
    status = advertisement.topology.validate(limits);
    if (!status.ok()) {
        return status;
    }
    if (advertisement.accelerators.size() > limits.max_accelerators_per_resource) {
        return Status(ErrorCode::LimitExceeded, "accelerator count exceeds configured maximum");
    }
    for (const AcceleratorAttachment& attachment : advertisement.accelerators) {
        status = attachment.id.validate();
        if (!status.ok()) {
            return status;
        }
        status = check_text(attachment.model, "accelerator model", limits.max_string_length);
        if (!status.ok()) {
            return status;
        }
    }
    if (advertisement.locality.local_dataset_versions.size() > limits.max_local_datasets_per_resource) {
        return Status(ErrorCode::LimitExceeded, "local dataset count exceeds configured maximum");
    }
    if (advertisement.locality.resident_models.size() > limits.max_resident_models_per_resource) {
        return Status(ErrorCode::LimitExceeded, "resident model count exceeds configured maximum");
    }
    if (advertisement.locality.colocated_simulators.size() > limits.max_simulator_scenarios) {
        return Status(ErrorCode::LimitExceeded, "co-located simulator count exceeds configured maximum");
    }
    for (const DatasetVersionId& version : advertisement.locality.local_dataset_versions) {
        status = version.validate();
        if (!status.ok()) {
            return status;
        }
    }
    for (const ModelResourceId& model : advertisement.locality.resident_models) {
        status = model.validate();
        if (!status.ok()) {
            return status;
        }
    }
    for (const SimulatorId& simulator : advertisement.locality.colocated_simulators) {
        status = simulator.validate();
        if (!status.ok()) {
            return status;
        }
    }
    if (advertisement.labels.size() > limits.max_capabilities_per_resource) {
        return Status(ErrorCode::LimitExceeded, "label count exceeds configured maximum");
    }
    for (const std::string& label : advertisement.labels) {
        status = check_text(label, "label", limits.max_string_length);
        if (!status.ok()) {
            return status;
        }
    }

    switch (advertisement.resource_class) {
        case ResourceClass::Model: {
            if (!advertisement.model.has_value()) {
                return Status(ErrorCode::InvalidArgument, "MODEL resource must carry a model descriptor");
            }
            status = advertisement.model->id.validate();
            if (!status.ok()) {
                return status;
            }
            status = check_text(advertisement.model->version, "model version", limits.max_digest_length, false);
            if (!status.ok()) {
                return status;
            }
            status = advertisement.model->modalities.validate(limits);
            if (!status.ok()) {
                return status;
            }
            break;
        }
        case ResourceClass::Gpu: {
            if (advertisement.accelerators.empty()) {
                return Status(ErrorCode::InvalidArgument,
                              "GPU resource must carry at least one accelerator attachment");
            }
            break;
        }
        case ResourceClass::Simulator: {
            if (!advertisement.simulator.has_value()) {
                return Status(ErrorCode::InvalidArgument,
                              "SIMULATOR resource must carry a simulator descriptor");
            }
            status = advertisement.simulator->id.validate();
            if (!status.ok()) {
                return status;
            }
            status = check_text(advertisement.simulator->version, "simulator version", limits.max_digest_length,
                                false);
            if (!status.ok()) {
                return status;
            }
            if (advertisement.simulator->scenarios.size() > limits.max_simulator_scenarios) {
                return Status(ErrorCode::LimitExceeded, "simulator scenario count exceeds configured maximum");
            }
            for (const std::string& scenario : advertisement.simulator->scenarios) {
                status = check_text(scenario, "simulator scenario", limits.max_string_length, false);
                if (!status.ok()) {
                    return status;
                }
            }
            if (advertisement.simulator->max_sessions == 0 ||
                advertisement.simulator->max_sessions > advertisement.capacity.max_sessions) {
                return Status(ErrorCode::InvalidArgument,
                              "simulator session limit is inconsistent with resource capacity");
            }
            break;
        }
        case ResourceClass::Dataset: {
            if (!advertisement.dataset.has_value()) {
                return Status(ErrorCode::InvalidArgument, "DATASET resource must carry a dataset descriptor");
            }
            status = advertisement.dataset->id.validate();
            if (!status.ok()) {
                return status;
            }
            status = advertisement.dataset->version.validate();
            if (!status.ok()) {
                return status;
            }
            status = check_text(advertisement.dataset->digest, "dataset digest", limits.max_digest_length);
            if (!status.ok()) {
                return status;
            }
            break;
        }
        case ResourceClass::PhysicalEnvironment: {
            if (!advertisement.environment.has_value()) {
                return Status(ErrorCode::InvalidArgument,
                              "PHYSICAL_ENVIRONMENT resource must carry an environment descriptor");
            }
            status = advertisement.environment->id.validate();
            if (!status.ok()) {
                return status;
            }
            status = advertisement.environment->physical_environment.validate();
            if (!status.ok()) {
                return status;
            }
            status = check_text(advertisement.environment->eligibility_token, "eligibility token",
                                limits.max_string_length);
            if (!status.ok()) {
                return status;
            }
            break;
        }
        case ResourceClass::VirtualEnvironment: {
            if (!advertisement.environment.has_value()) {
                return Status(ErrorCode::InvalidArgument,
                              "VIRTUAL_ENVIRONMENT resource must carry an environment descriptor");
            }
            status = advertisement.environment->id.validate();
            if (!status.ok()) {
                return status;
            }
            status = advertisement.environment->virtual_environment.validate();
            if (!status.ok()) {
                return status;
            }
            status = check_text(advertisement.environment->image_digest, "environment image digest",
                                limits.max_digest_length, false);
            if (!status.ok()) {
                return status;
            }
            break;
        }
        case ResourceClass::Worker:
            break;
    }
    return Status{};
}

Status validate_resource_record(const ResourceRecord& record, const Limits& limits) {
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
    Status status = validate_advertisement(advertisement, limits);
    if (!status.ok()) {
        return status;
    }
    if (record.lifecycle == ResourceLifecycle::Retired) {
        if (record.reserved_slots != 0) {
            return Status(ErrorCode::ReservationConflict, "retired resource still holds reserved capacity");
        }
    }
    if (record.total_occupancy() > record.capacity.slots) {
        return Status(ErrorCode::CapacityExhausted, "resource occupancy exceeds declared capacity");
    }
    return Status{};
}

void normalize_advertisement(ResourceAdvertisement& advertisement) {
    std::sort(advertisement.accelerators.begin(), advertisement.accelerators.end(),
              [](const AcceleratorAttachment& a, const AcceleratorAttachment& b) { return a.id < b.id; });
    std::sort(advertisement.locality.local_dataset_versions.begin(),
              advertisement.locality.local_dataset_versions.end());
    std::sort(advertisement.locality.resident_models.begin(), advertisement.locality.resident_models.end());
    std::sort(advertisement.locality.colocated_simulators.begin(),
              advertisement.locality.colocated_simulators.end());
    std::sort(advertisement.labels.begin(), advertisement.labels.end());
    if (advertisement.simulator.has_value()) {
        std::sort(advertisement.simulator->scenarios.begin(), advertisement.simulator->scenarios.end());
    }
}

}  // namespace lab_scheduler
