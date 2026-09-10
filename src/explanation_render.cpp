#include "lab_scheduler/canonical.hpp"
#include "lab_scheduler/engine.hpp"

namespace lab_scheduler {

std::string_view decision_outcome_name(DecisionOutcome outcome) noexcept {
    return outcome == DecisionOutcome::Placed ? std::string_view("PLACED") : std::string_view("NO_PLACEMENT");
}

std::optional<DecisionOutcome> parse_decision_outcome(std::string_view text) noexcept {
    if (text == "PLACED") {
        return DecisionOutcome::Placed;
    }
    if (text == "NO_PLACEMENT") {
        return DecisionOutcome::NoPlacement;
    }
    return std::nullopt;
}

std::string render_explanation(const Explanation& explanation) {
    std::string out;
    out += "decision ";
    out += explanation.decision.to_string();
    out += "\nrequest ";
    out += explanation.request.to_string();
    out += "\nexperiment ";
    out += explanation.experiment.to_string();
    out += " generation ";
    out += explanation.experiment_generation.to_string();
    out += "\noutcome ";
    out += decision_outcome_name(explanation.outcome);
    out += "\nfailure ";
    out += error_code_name(explanation.failure);
    if (!explanation.failure_detail.empty()) {
        out += " (";
        out += explanation.failure_detail;
        out += ")";
    }
    out += "\n";

    for (const RequirementExplanation& requirement : explanation.requirements) {
        out += "requirement ";
        out += requirement.requirement_name;
        out += " class=";
        out += resource_class_name(requirement.resource_class);
        out += " considered=";
        out += decimal_u64(requirement.considered);
        out += " eligible=";
        out += decimal_u64(requirement.eligible);
        out += "\n";
        for (const RejectedResourceEntry& rejected : requirement.rejected) {
            out += "  rejected ";
            out += rejected.resource.to_string();
            out += " '";
            out += rejected.name;
            out += "'";
            for (const RejectReason reason : rejected.reasons) {
                out += " ";
                out += reject_reason_name(reason);
            }
            out += "\n";
        }
    }

    out += "selected";
    for (const SelectedResource& selected : explanation.selected) {
        out += " ";
        out += selected.resource.to_string();
        out += "(slots=";
        out += decimal_u64(selected.slots);
        out += selected.exclusive ? ",exclusive)" : ")";
    }
    out += "\n";

    for (const CandidateExplanation& candidate : explanation.candidates) {
        out += candidate.selected ? "candidate selected" : "candidate";
        out += " score=";
        out += decimal_i64(candidate.score);
        out += " resources=";
        for (std::size_t i = 0; i < candidate.resources.size(); ++i) {
            if (i != 0) {
                out += ",";
            }
            out += candidate.resources[i].to_string();
        }
        out += "\n";
        for (const RankingFactorValue& factor : candidate.factors) {
            out += "  factor ";
            out += factor.name;
            out += " weight=";
            out += decimal_i64(factor.weight);
            out += " value=";
            out += decimal_i64(factor.value);
            out += " weighted=";
            out += decimal_i64(factor.weighted);
            out += "\n";
        }
    }

    for (const std::string& note : explanation.notes) {
        out += "note ";
        out += note;
        out += "\n";
    }
    return out;
}

}  // namespace lab_scheduler
