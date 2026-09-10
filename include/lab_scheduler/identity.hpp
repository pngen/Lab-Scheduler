#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "lab_scheduler/canonical.hpp"
#include "lab_scheduler/error.hpp"

namespace lab_scheduler {

// Every identity in the runtime is a distinct strong type with a distinct
// canonical text form: "<prefix>-<16 lowercase hex digits>". The prefix makes
// cross-identity confusion (for example persisting a PlacementId where a
// ReservationId belongs) detectable at the transport and persistence
// boundaries instead of silently accepted as an integer. Value zero is the
// reserved null identity and never parses.
template <class Tag>
class StrongId {
public:
    using tag_type = Tag;
    using value_type = std::uint64_t;

    constexpr StrongId() noexcept = default;
    explicit constexpr StrongId(std::uint64_t value) noexcept : value_(value) {}

    [[nodiscard]] static constexpr StrongId from_value(std::uint64_t value) noexcept {
        return StrongId(value);
    }

    [[nodiscard]] static constexpr std::string_view prefix() noexcept { return Tag::kPrefix; }
    [[nodiscard]] static constexpr std::string_view type_name() noexcept { return Tag::kName; }

    [[nodiscard]] static std::optional<StrongId> parse(std::string_view text) noexcept {
        constexpr std::string_view prefix_text = Tag::kPrefix;
        constexpr std::size_t digits = 16;
        if (text.size() != prefix_text.size() + 1 + digits) {
            return std::nullopt;
        }
        if (text.substr(0, prefix_text.size()) != prefix_text) {
            return std::nullopt;
        }
        if (text[prefix_text.size()] != '-') {
            return std::nullopt;
        }
        std::uint64_t value = 0;
        if (!hex_u64_decode(text.substr(prefix_text.size() + 1), static_cast<unsigned>(digits), value)) {
            return std::nullopt;
        }
        if (value == 0) {
            return std::nullopt;  // null identity is never canonical
        }
        return StrongId(value);
    }

    [[nodiscard]] std::string to_string() const {
        std::string out(Tag::kPrefix);
        out.push_back('-');
        out += hex_u64(value_, 16);
        return out;
    }

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool is_null() const noexcept { return value_ == 0; }

    void clear() noexcept { value_ = 0; }

    [[nodiscard]] Status validate() const {
        if (value_ == 0) {
            std::string message = "null ";
            message += Tag::kName;
            return Status(ErrorCode::InvalidIdentity, std::move(message));
        }
        return Status{};
    }

    friend constexpr bool operator==(const StrongId&, const StrongId&) noexcept = default;
    friend constexpr auto operator<=>(const StrongId&, const StrongId&) noexcept = default;

private:
    std::uint64_t value_ = 0;
};

#define LS_DECLARE_ID(TypeName, PrefixLiteral, NameLiteral) \
    struct TypeName##Tag {                                  \
        static constexpr std::string_view kPrefix = PrefixLiteral; \
        static constexpr std::string_view kName = NameLiteral;     \
    };                                                      \
    using TypeName = StrongId<TypeName##Tag>

// Scheduling domain identities.
LS_DECLARE_ID(ScheduleRequestId, "sreq", "ScheduleRequestId");
LS_DECLARE_ID(ExperimentId, "exp", "ExperimentId");
LS_DECLARE_ID(ExperimentGeneration, "expg", "ExperimentGeneration");
LS_DECLARE_ID(TrialId, "trial", "TrialId");
LS_DECLARE_ID(DecisionId, "dec", "DecisionId");
LS_DECLARE_ID(PlacementId, "plc", "PlacementId");
LS_DECLARE_ID(PlacementGeneration, "plcg", "PlacementGeneration");
LS_DECLARE_ID(CoordinatorEpoch, "epoch", "CoordinatorEpoch");

// Resource identities.
LS_DECLARE_ID(ResourceId, "res", "ResourceId");
LS_DECLARE_ID(ResourceGeneration, "resg", "ResourceGeneration");
LS_DECLARE_ID(AcceleratorId, "acc", "AcceleratorId");
LS_DECLARE_ID(ModelResourceId, "mdl", "ModelResourceId");
LS_DECLARE_ID(SimulatorId, "sim", "SimulatorId");
LS_DECLARE_ID(DatasetId, "ds", "DatasetId");
LS_DECLARE_ID(DatasetVersionId, "dsv", "DatasetVersionId");
LS_DECLARE_ID(EnvironmentId, "env", "EnvironmentId");
LS_DECLARE_ID(PhysicalEnvironmentId, "phys", "PhysicalEnvironmentId");
LS_DECLARE_ID(VirtualEnvironmentId, "virt", "VirtualEnvironmentId");
LS_DECLARE_ID(CapabilityId, "cap", "CapabilityId");
LS_DECLARE_ID(TopologyDomainId, "topo", "TopologyDomainId");
LS_DECLARE_ID(HealthGeneration, "hgen", "HealthGeneration");
LS_DECLARE_ID(CapacityGeneration, "cgen", "CapacityGeneration");

// Worker / incarnation identities.
LS_DECLARE_ID(WorkerId, "wrk", "WorkerId");
LS_DECLARE_ID(WorkerBootId, "boot", "WorkerBootId");

// Reservation identities.
LS_DECLARE_ID(ReservationId, "rsv", "ReservationId");
LS_DECLARE_ID(LeaseId, "lease", "LeaseId");

#undef LS_DECLARE_ID

// Deterministic, monotonic identity source. Tests seed it so that generated
// scenarios are reproducible; production code seeds it from process entropy.
class IdGenerator {
public:
    explicit IdGenerator(std::uint64_t seed = 1) noexcept : counter_(seed) {}

    [[nodiscard]] std::uint64_t next_value() noexcept {
        ++counter_;
        return counter_;
    }

    template <class Id>
    [[nodiscard]] Id next() noexcept {
        return Id::from_value(next_value());
    }

    // Keeps the generator strictly ahead of any identity observed from
    // persisted state or the wire.
    void observe(std::uint64_t value) noexcept {
        if (value > counter_) {
            counter_ = value;
        }
    }

    void reset(std::uint64_t seed) noexcept { counter_ = seed; }

    [[nodiscard]] std::uint64_t counter() const noexcept { return counter_; }

private:
    std::uint64_t counter_ = 0;
};

// Capability identities are content addressed: the identity of a capability is
// the hash of its canonical lowercase name, so two processes that advertise
// the same capability text always agree on the identity, and a mistyped
// capability can never silently match.
[[nodiscard]] CapabilityId capability_id_from_name(std::string_view name) noexcept;
[[nodiscard]] TopologyDomainId topology_domain_from_name(std::string_view name) noexcept;

// A boot identity must differ on every process incarnation of the same logical
// worker. It mixes the logical worker identity, a caller supplied boot counter,
// and a process entropy token, and can never collide with the null identity.
[[nodiscard]] WorkerBootId make_worker_boot_id(WorkerId worker, std::uint64_t boot_counter,
                                               std::uint64_t process_token) noexcept;

// Process entropy token: stable for the lifetime of a process, distinct
// between processes on the same host.
[[nodiscard]] std::uint64_t process_entropy_token() noexcept;

}  // namespace lab_scheduler
