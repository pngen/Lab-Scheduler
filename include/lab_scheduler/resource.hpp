#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lab_scheduler/capability.hpp"
#include "lab_scheduler/error.hpp"
#include "lab_scheduler/identity.hpp"
#include "lab_scheduler/limits.hpp"
#include "lab_scheduler/topology.hpp"

namespace lab_scheduler {

// Lab Scheduler models heterogeneous lab resources explicitly. A resource is
// not a scalar capacity bucket: some resources are exclusive, some shareable,
// some are locality anchors, some are capabilities rather than capacity, and
// some are compound.
enum class ResourceClass : std::uint8_t {
    Model = 0,
    Gpu = 1,
    Simulator = 2,
    Dataset = 3,
    PhysicalEnvironment = 4,
    VirtualEnvironment = 5,
    Worker = 6,
};

std::string_view resource_class_name(ResourceClass value) noexcept;
std::optional<ResourceClass> parse_resource_class(std::string_view text) noexcept;

enum class SharingMode : std::uint8_t { Shared = 0, Exclusive = 1 };
std::string_view sharing_mode_name(SharingMode value) noexcept;
std::optional<SharingMode> parse_sharing_mode(std::string_view text) noexcept;

enum class HealthState : std::uint8_t { Unknown = 0, Healthy = 1, Degraded = 2, Unhealthy = 3 };
std::string_view health_state_name(HealthState value) noexcept;
std::optional<HealthState> parse_health_state(std::string_view text) noexcept;

enum class ReadinessState : std::uint8_t { Unknown = 0, Ready = 1, NotReady = 2, Unsupported = 3 };
std::string_view readiness_state_name(ReadinessState value) noexcept;
std::optional<ReadinessState> parse_readiness_state(std::string_view text) noexcept;

// REAL / SYNTHETIC / UNSUPPORTED provenance is carried on every resource so
// that proofs never silently upgrade a synthetic resource into real hardware.
enum class Provenance : std::uint8_t { Unsupported = 0, Synthetic = 1, Real = 2 };
std::string_view provenance_name(Provenance value) noexcept;
std::optional<Provenance> parse_provenance(std::string_view text) noexcept;

enum class ResourceLifecycle : std::uint8_t { Registered = 0, Current = 1, Retired = 2, Lost = 3 };
std::string_view resource_lifecycle_name(ResourceLifecycle value) noexcept;
std::optional<ResourceLifecycle> parse_resource_lifecycle(std::string_view text) noexcept;

struct ResourceCapacity {
    std::uint32_t slots = 1;
    std::uint64_t memory_bytes = 0;
    std::uint32_t max_sessions = 1;
};

struct AcceleratorAttachment {
    AcceleratorId id{};
    std::string model{};
    std::uint64_t memory_bytes = 0;
    std::uint32_t compute_capability_major = 0;
    std::uint32_t compute_capability_minor = 0;
    TopologyDomainId root_complex{};
};

struct ModelDescriptor {
    ModelResourceId id{};
    std::string version{};
    std::uint32_t context_window = 0;
    bool tool_support = false;
    bool resident = false;
    CapabilitySet modalities{};
};

struct SimulatorDescriptor {
    SimulatorId id{};
    std::string version{};
    std::vector<std::string> scenarios{};  // sorted, unique
    std::uint32_t max_sessions = 1;
};

struct DatasetDescriptor {
    DatasetId id{};
    DatasetVersionId version{};
    std::string digest{};
    std::uint64_t size_bytes = 0;
};

struct EnvironmentDescriptor {
    EnvironmentId id{};
    std::string image_digest{};
    VirtualEnvironmentId virtual_environment{};
    PhysicalEnvironmentId physical_environment{};
    // Opaque, externally authoritative eligibility input (for example a safety
    // or calibration state). Lab Scheduler consumes it and never derives it.
    std::string eligibility_token{};
};

struct LocalityDescriptor {
    // Sorted, unique. Locality is only ever what an advertiser supplied.
    std::vector<DatasetVersionId> local_dataset_versions{};
    std::vector<ModelResourceId> resident_models{};
    std::vector<SimulatorId> colocated_simulators{};
};

// Everything a resource publishes about itself. Registration and every later
// state publication use this same shape, so there is exactly one resource
// description in the runtime.
struct ResourceAdvertisement {
    ResourceId id{};
    ResourceClass resource_class = ResourceClass::Worker;
    std::string name{};
    WorkerId worker{};      // null for coordinator-owned static resources
    WorkerBootId boot{};    // incarnation that produced this advertisement
    Provenance provenance = Provenance::Synthetic;
    SharingMode sharing = SharingMode::Shared;
    CapabilitySet capabilities{};
    ResourceCapacity capacity{};
    std::uint32_t current_occupancy = 0;
    HealthState health = HealthState::Unknown;
    std::string health_detail{};
    ReadinessState readiness = ReadinessState::Unknown;
    TopologyDescriptor topology{};
    LocalityDescriptor locality{};
    std::vector<AcceleratorAttachment> accelerators{};
    std::optional<ModelDescriptor> model{};
    std::optional<SimulatorDescriptor> simulator{};
    std::optional<DatasetDescriptor> dataset{};
    std::optional<EnvironmentDescriptor> environment{};
    std::string tenant{};
    std::vector<std::string> labels{};  // secondary metadata only, never a constraint
};

// The scheduler's live view of one resource. Dynamic fields are only
// authoritative while dynamic_authoritative is true and the advertising
// incarnation is still the current one.
struct ResourceRecord {
    ResourceId id{};
    ResourceClass resource_class = ResourceClass::Worker;
    std::string name{};
    WorkerId worker{};
    WorkerBootId boot{};
    Provenance provenance = Provenance::Synthetic;
    SharingMode sharing = SharingMode::Shared;
    CapabilitySet capabilities{};
    ResourceCapacity capacity{};
    std::uint32_t advertised_occupancy = 0;
    HealthState health = HealthState::Unknown;
    std::string health_detail{};
    ReadinessState readiness = ReadinessState::Unknown;
    TopologyDescriptor topology{};
    LocalityDescriptor locality{};
    std::vector<AcceleratorAttachment> accelerators{};
    std::optional<ModelDescriptor> model{};
    std::optional<SimulatorDescriptor> simulator{};
    std::optional<DatasetDescriptor> dataset{};
    std::optional<EnvironmentDescriptor> environment{};
    std::string tenant{};
    std::vector<std::string> labels{};

    ResourceLifecycle lifecycle = ResourceLifecycle::Registered;
    ResourceGeneration generation{};        // monotonic, bumped on every accepted change
    HealthGeneration health_generation{};
    CapacityGeneration capacity_generation{};
    std::uint64_t state_sequence = 0;       // accepted publication counter for this resource
    CoordinatorEpoch validated_epoch{};     // epoch in which the dynamic state was validated
    bool dynamic_authoritative = false;     // false => currentness is not proven
    std::string authority_detail{};         // why currentness is (or is not) proven

    // Reserved capacity held by current placements. Owned by the reservation
    // ledger, mirrored here for accounting checks.
    std::uint32_t reserved_slots = 0;

    [[nodiscard]] bool is_compound() const noexcept {
        return resource_class == ResourceClass::Gpu || resource_class == ResourceClass::Worker;
    }
    [[nodiscard]] std::uint32_t total_occupancy() const noexcept {
        return advertised_occupancy + reserved_slots;
    }
};

Status validate_advertisement(const ResourceAdvertisement& advertisement, const Limits& limits);
Status validate_resource_record(const ResourceRecord& record, const Limits& limits);

// Canonical, order independent normalization applied after validation.
void normalize_advertisement(ResourceAdvertisement& advertisement);

}  // namespace lab_scheduler
