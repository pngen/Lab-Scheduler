#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "lab_scheduler/error.hpp"
#include "lab_scheduler/identity.hpp"
#include "lab_scheduler/placement.hpp"
#include "lab_scheduler/request.hpp"

namespace lab_scheduler {

enum class DecisionOutcome : std::uint8_t { Placed = 0, NoPlacement = 1 };

std::string_view decision_outcome_name(DecisionOutcome outcome) noexcept;
std::optional<DecisionOutcome> parse_decision_outcome(std::string_view text) noexcept;

struct RejectedResourceEntry {
    ResourceId resource{};
    std::string name{};
    std::vector<RejectReason> reasons{};
};

struct RequirementExplanation {
    std::string requirement_name{};
    ResourceClass resource_class = ResourceClass::Worker;
    std::uint32_t considered = 0;   // resources of this class examined
    std::uint32_t eligible = 0;     // resources that passed every hard constraint
    std::vector<RejectedResourceEntry> rejected{};
};

struct RankingFactorValue {
    std::string name{};
    std::int32_t weight = 0;
    std::int64_t value = 0;      // fixed point, 1e6 == 1.0
    std::int64_t weighted = 0;   // value * weight / 1000
};

struct CandidateExplanation {
    std::vector<ResourceId> resources{};  // canonical ascending order
    std::int64_t score = 0;
    std::vector<RankingFactorValue> factors{};
    bool selected = false;
    std::string tie_break{};
};

// A placement decision is only useful if it explains itself. The explanation
// distinguishes every failure category instead of reporting "no capacity", and
// its ordering is deterministic so that two runs over the same state produce
// byte identical text.
struct Explanation {
    DecisionId decision{};
    ScheduleRequestId request{};
    ExperimentId experiment{};
    ExperimentGeneration experiment_generation{};
    DecisionOutcome outcome = DecisionOutcome::NoPlacement;
    ErrorCode failure = ErrorCode::Ok;
    std::string failure_detail{};
    std::vector<RequirementExplanation> requirements{};
    std::vector<CandidateExplanation> candidates{};
    std::vector<SelectedResource> selected{};
    std::vector<std::string> notes{};
};

}  // namespace lab_scheduler
