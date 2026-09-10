// Real multiprocess proof for Lab Scheduler.
//
// Every process here is a real operating system process: a coordinator, three
// worker/resource agents, and this driver. The proof exercises compound
// placement, hard filtering, deterministic ranking, reservations, completion,
// cancellation, exclusivity, real worker death with reincarnation, real
// coordinator death with recovery, stale-authority replay rejection, malformed
// frame rejection, and capacity accounting return to baseline.
//
// Output is one deterministic line per check:
//   check PASS <name>
//   check FAIL <name> :: <detail>
// followed by:
//   proof result PASS|FAIL checks=<n> failures=<n>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "cli_support.hpp"
#include "lab_scheduler/canonical.hpp"
#include "lab_scheduler/cluster.hpp"
#include "lab_scheduler/identity.hpp"
#include "lab_scheduler/inspection.hpp"
#include "lab_scheduler/lab_profiles.hpp"
#include "lab_scheduler/persistence.hpp"
#include "lab_scheduler/process.hpp"
#include "lab_scheduler/protocol.hpp"

using namespace lab_scheduler;

namespace {

struct Proof {
    int checks = 0;
    int failures = 0;

    void expect(bool ok, const std::string& name, const std::string& detail = std::string()) {
        ++checks;
        if (ok) {
            std::printf("check PASS %s\n", name.c_str());
        } else {
            ++failures;
            std::printf("check FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : " :: ", detail.c_str());
        }
        std::fflush(stdout);
    }
};

struct CoordinatorHandle {
    ChildProcess process{};
    std::uint16_t port = 0;
    CoordinatorEpoch epoch{};
    bool recovered = false;
    CoordinatorEpoch previous_epoch{};
    std::uint32_t revalidation = 0;
    std::uint32_t reconciled = 0;
    std::uint32_t revoked = 0;
    std::uint32_t preserved_requests = 0;
    std::uint32_t preserved_history = 0;
};

struct WorkerHandle {
    ChildProcess process{};
    WorkerBootId boot{};
    WorkerId worker{};
};

bool read_until(ChildProcess& child, const std::string& prefix, std::string& line_out, std::string& error) {
    std::string seen;
    for (int attempt = 0; attempt < 128; ++attempt) {
        Result<std::string> line = child.read_line();
        if (!line.ok()) {
            error = "child output ended: " + line.status().to_string() + "; saw:" + seen;
            return false;
        }
        if (line.value().rfind(prefix, 0) == 0) {
            line_out = line.value();
            return true;
        }
        if (seen.size() < 512) {
            seen += " | ";
            seen += line.value();
        }
    }
    error = "expected line '" + prefix + "' was not produced; saw:" + seen;
    return false;
}

bool parse_field(const std::string& line, const std::string& key, std::string& value) {
    const std::size_t position = line.find(key);
    if (position == std::string::npos) {
        return false;
    }
    std::size_t start = position + key.size();
    std::size_t end = line.find(' ', start);
    if (end == std::string::npos) {
        end = line.size();
    }
    value = line.substr(start, end - start);
    return !value.empty();
}

bool parse_u32_field(const std::string& line, const std::string& key, std::uint32_t& value) {
    std::string text;
    if (!parse_field(line, key, text)) {
        return false;
    }
    return parse_decimal_u32(text, value);
}

bool start_coordinator(const std::string& executable, const std::string& state_path,
                       CoordinatorHandle& handle, std::string& error) {
    std::vector<std::string> arguments{"--port", "0"};
    if (!state_path.empty()) {
        arguments.push_back("--state");
        arguments.push_back(state_path);
    }
    Result<ChildProcess> child = ChildProcess::spawn(executable, arguments);
    if (!child.ok()) {
        error = child.status().to_string();
        return false;
    }
    handle.process = child.take();
    std::string line;
    if (!read_until(handle.process, "coordinator listening", line, error)) {
        if (!handle.process.running()) {
            for (int attempt = 0; attempt < 64; ++attempt) {
                Result<std::string> extra = handle.process.read_line();
                if (!extra.ok()) {
                    break;
                }
                error += " | coordinator said: " + extra.value();
            }
        }
        return false;
    }
    std::uint32_t port = 0;
    if (!parse_u32_field(line, "port=", port)) {
        error = "cannot parse bound port from: " + line;
        return false;
    }
    handle.port = static_cast<std::uint16_t>(port);
    if (!read_until(handle.process, "coordinator epoch=", line, error)) {
        return false;
    }
    const std::optional<CoordinatorEpoch> epoch =
        CoordinatorEpoch::parse(line.substr(std::string("coordinator epoch=").size()));
    if (!epoch.has_value()) {
        error = "cannot parse coordinator epoch from: " + line;
        return false;
    }
    handle.epoch = *epoch;
    if (!read_until(handle.process, "coordinator recovered", line, error)) {
        return false;
    }
    handle.recovered = line.rfind("coordinator recovered=no", 0) != 0;
    if (handle.recovered) {
        std::string text;
        if (parse_field(line, "previous_epoch=", text)) {
            const std::optional<CoordinatorEpoch> previous = CoordinatorEpoch::parse(text);
            if (previous.has_value()) {
                handle.previous_epoch = *previous;
            }
        }
        static_cast<void>(parse_u32_field(line, "revalidation=", handle.revalidation));
        static_cast<void>(parse_u32_field(line, "reconciled=", handle.reconciled));
        static_cast<void>(parse_u32_field(line, "revoked=", handle.revoked));
        static_cast<void>(parse_u32_field(line, "requests=", handle.preserved_requests));
        static_cast<void>(parse_u32_field(line, "history=", handle.preserved_history));
    }
    return true;
}

bool start_worker(const std::string& executable, std::uint16_t port, const std::string& profile,
                  std::uint32_t boot_counter, bool hold, WorkerHandle& handle, std::string& error) {
    std::vector<std::string> arguments{"--port", decimal_u64(port), "--profile", profile, "--boot",
                                       decimal_u64(boot_counter)};
    if (hold) {
        arguments.push_back("--hold");
    }
    Result<ChildProcess> child = ChildProcess::spawn(executable, arguments);
    if (!child.ok()) {
        error = child.status().to_string();
        return false;
    }
    handle.process = child.take();
    std::string line;
    if (!read_until(handle.process, "worker id=", line, error)) {
        return false;
    }
    std::string boot_text;
    if (!parse_field(line, "boot=", boot_text)) {
        error = "cannot parse worker boot identity from: " + line;
        return false;
    }
    const std::optional<WorkerBootId> boot = WorkerBootId::parse(boot_text);
    if (!boot.has_value()) {
        error = "invalid worker boot identity: " + boot_text;
        return false;
    }
    handle.boot = *boot;
    std::string worker_text;
    if (parse_field(line, "id=", worker_text)) {
        const std::optional<WorkerId> worker = WorkerId::parse(worker_text);
        if (worker.has_value()) {
            handle.worker = *worker;
        }
    }
    if (!read_until(handle.process, "worker ready", line, error)) {
        return false;
    }
    return true;
}

bool inspect(CoordinatorClient& client, const std::string& subject, std::string& text) {
    return client.call_inspect(subject, text).ok();
}

// Bounded polling: each attempt is a real round trip and returns immediately.
bool poll_until(CoordinatorClient& client, const std::string& subject, const std::string& needle,
                std::uint32_t attempts, std::string& last) {
    for (std::uint32_t attempt = 0; attempt < attempts; ++attempt) {
        if (!inspect(client, subject, last)) {
            return false;
        }
        if (last.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

ResourceRequirement make_requirement(const std::string& name, ResourceClass resource_class) {
    ResourceRequirement requirement;
    requirement.name = name;
    requirement.resource_class = resource_class;
    requirement.min_slots = 1;
    return requirement;
}

ScheduleRequest make_alpha_request(ScheduleRequestId id, ExperimentId experiment) {
    ScheduleRequest request;
    request.id = id;
    request.experiment = experiment;
    request.experiment_generation = ExperimentGeneration::from_value(1);
    request.workload = "alpha-compound";
    request.tenant = "lab-tenant";
    request.caller_authority = "cluster-proof";

    ResourceRequirement model = make_requirement("model", ResourceClass::Model);
    model.required_capabilities.add("inference", Limits{});
    model.model = ModelResourceId::from_value(kTrainingModelId);
    model.model_version = "1.4.0";
    model.require_model_resident = true;

    ResourceRequirement accelerator = make_requirement("accelerator", ResourceClass::Gpu);
    accelerator.required_capabilities.add("cuda", Limits{});
    accelerator.required_capabilities.add("training", Limits{});
    accelerator.min_memory_bytes = 8ull * 1024ull * 1024ull * 1024ull;
    accelerator.affinity_group = 1;

    ResourceRequirement corpus = make_requirement("corpus", ResourceClass::Dataset);
    corpus.required_capabilities.add("dataset.read", Limits{});
    corpus.dataset = DatasetId::from_value(kCorpusDatasetId);
    corpus.dataset_version = DatasetVersionId::from_value(kCorpusDatasetVersion1);
    corpus.affinity_group = 1;

    request.requirements = {model, accelerator, corpus};
    AffinityConstraint same_host;
    same_host.group_a = 1;
    same_host.group_b = 1;
    same_host.scope = TopologyScope::Host;
    request.affinity.push_back(same_host);
    return request;
}

ScheduleRequest make_beta_request(ScheduleRequestId id, ExperimentId experiment) {
    ScheduleRequest request;
    request.id = id;
    request.experiment = experiment;
    request.experiment_generation = ExperimentGeneration::from_value(1);
    request.workload = "beta-compound";
    request.tenant = "lab-tenant";
    request.caller_authority = "cluster-proof";

    ResourceRequirement accelerator = make_requirement("accelerator", ResourceClass::Gpu);
    accelerator.required_capabilities.add("cuda", Limits{});
    accelerator.required_capabilities.add("simulation", Limits{});

    ResourceRequirement simulator = make_requirement("simulator", ResourceClass::Simulator);
    simulator.required_capabilities.add("sim.grid", Limits{});
    simulator.simulator = SimulatorId::from_value(kGridSimulatorId);
    simulator.simulator_version = "2.3.1";
    simulator.simulator_scenario = "grid-steady";
    simulator.affinity_group = 2;

    ResourceRequirement corpus = make_requirement("corpus", ResourceClass::Dataset);
    corpus.dataset = DatasetId::from_value(kCorpusDatasetId);
    corpus.dataset_version = DatasetVersionId::from_value(kCorpusDatasetVersion2);
    corpus.affinity_group = 2;

    request.requirements = {accelerator, simulator, corpus};
    AffinityConstraint same_host;
    same_host.group_a = 2;
    same_host.group_b = 2;
    same_host.scope = TopologyScope::Host;
    request.affinity.push_back(same_host);
    return request;
}

ScheduleRequest make_rig_request(ScheduleRequestId id, ExperimentId experiment) {
    ScheduleRequest request;
    request.id = id;
    request.experiment = experiment;
    request.experiment_generation = ExperimentGeneration::from_value(1);
    request.workload = "physical-rig";
    request.tenant = "lab-tenant";
    request.caller_authority = "cluster-proof";

    ResourceRequirement rig = make_requirement("rig", ResourceClass::PhysicalEnvironment);
    rig.required_capabilities.add("rig.robotics", Limits{});
    rig.require_exclusive = true;
    rig.physical_environment = PhysicalEnvironmentId::from_value(kRoboticsPhysicalEnvironmentId);
    request.requirements = {rig};
    request.exclusive = true;
    return request;
}

ScheduleRequest make_environment_request(ScheduleRequestId id, ExperimentId experiment) {
    ScheduleRequest request;
    request.id = id;
    request.experiment = experiment;
    request.experiment_generation = ExperimentGeneration::from_value(1);
    request.workload = "virtual-environment";
    request.tenant = "lab-tenant";
    request.caller_authority = "cluster-proof";

    ResourceRequirement environment = make_requirement("environment", ResourceClass::VirtualEnvironment);
    environment.required_capabilities.add("runtime.container", Limits{});
    environment.environment = EnvironmentId::from_value(kRoboticsEnvironmentId);
    environment.environment_digest = "sha256:gamma-runtime-1";
    request.requirements = {environment};
    return request;
}

CompletionClaim claim_from(const DecisionMessage& decision) {
    CompletionClaim claim;
    claim.placement = decision.authority.placement;
    claim.placement_generation = decision.authority.placement_generation;
    claim.experiment = decision.authority.experiment;
    claim.experiment_generation = decision.authority.experiment_generation;
    claim.epoch = decision.authority.epoch;
    claim.worker_boot = decision.authority.worker_boot;
    for (const SelectedResource& selected : decision.authority.resources) {
        claim.resources.push_back(selected.resource);
        claim.resource_generations.push_back(selected.generation);
    }
    claim.succeeded = true;
    claim.detail = "replayed by the multiprocess proof";
    return claim;
}

std::string resource_list(const PlacementRecord& placement) {
    std::string out;
    for (std::size_t i = 0; i < placement.resources.size(); ++i) {
        if (i != 0) {
            out += ",";
        }
        out += placement.resources[i].resource.to_string();
    }
    return out;
}

}  // namespace
int main(int argc, char** argv) {
    suppress_error_dialogs();
    const std::vector<std::string> arguments(argv + 1, argv + argc);
    const std::string bin_dir = option_value(arguments, "--bin", std::string("."));
#ifdef _WIN32
    const std::string suffix = ".exe";
    const std::string separator = "\\";
#else
    const std::string suffix;
    const std::string separator = "/";
#endif
    const std::string coordinator_exe = bin_dir + separator + "lab-scheduler-coordinator" + suffix;
    const std::string worker_exe = bin_dir + separator + "lab-scheduler-worker" + suffix;

    Proof proof;
    std::string error;

    const std::filesystem::path scratch =
        std::filesystem::temp_directory_path() /
        ("lab-scheduler-proof-" + decimal_u64(current_process_id()));
    std::error_code ignored;
    std::filesystem::remove_all(scratch, ignored);
    std::filesystem::create_directories(scratch, ignored);
    const std::string state_path = (scratch / "coordinator-state.lss").string();

    std::printf("proof scratch=%s\n", scratch.string().c_str());
    std::fflush(stdout);

    CoordinatorHandle coordinator;
    WorkerHandle alpha;
    WorkerHandle beta;
    WorkerHandle gamma;

    auto drain = [&](ChildProcess& child, const char* prefix) {
        if (child.running()) {
            return;
        }
        for (int attempt = 0; attempt < 256; ++attempt) {
            Result<std::string> line = child.read_line();
            if (!line.ok()) {
                break;
            }
            std::printf("%s%s\n", prefix, line.value().c_str());
        }
        std::fflush(stdout);
    };

    auto cleanup = [&]() {
        drain(coordinator.process, "coordinator output: ");
        static_cast<void>(alpha.process.terminate());
        static_cast<void>(beta.process.terminate());
        static_cast<void>(gamma.process.terminate());
        static_cast<void>(coordinator.process.terminate());
        alpha.process.close();
        beta.process.close();
        gamma.process.close();
        coordinator.process.close();
    };

    if (!start_coordinator(coordinator_exe, state_path, coordinator, error)) {
        proof.expect(false, "coordinator_start", error);
        cleanup();
        std::filesystem::remove_all(scratch, ignored);
        std::printf("proof result FAIL checks=%d failures=%d\n", proof.checks, proof.failures);
        return 1;
    }
    proof.expect(true, "coordinator_start");
    proof.expect(coordinator.epoch.value() == 1, "coordinator_fresh_epoch_is_one");

    proof.expect(start_worker(worker_exe, coordinator.port, "alpha", 1, false, alpha, error), "worker_alpha_start",
                 error);
    proof.expect(start_worker(worker_exe, coordinator.port, "beta", 1, false, beta, error), "worker_beta_start",
                 error);
    proof.expect(start_worker(worker_exe, coordinator.port, "gamma", 1, true, gamma, error), "worker_gamma_start",
                 error);

    Result<CoordinatorClient> connected = CoordinatorClient::connect(coordinator.port);
    if (!connected.ok()) {
        proof.expect(false, "client_connect", connected.status().to_string());
        cleanup();
        std::filesystem::remove_all(scratch, ignored);
        std::printf("proof result FAIL checks=%d failures=%d\n", proof.checks, proof.failures);
        return 1;
    }
    CoordinatorClient client = connected.take();

    const ExperimentId experiment_alpha = ExperimentId::from_value(0x5001);
    const ExperimentId experiment_alpha_again = ExperimentId::from_value(0x5002);
    const ExperimentId experiment_beta = ExperimentId::from_value(0x5003);
    const ExperimentId experiment_beta_restart = ExperimentId::from_value(0x5004);
    const ExperimentId experiment_environment = ExperimentId::from_value(0x5005);
    const ExperimentId experiment_rig = ExperimentId::from_value(0x5006);
    const ExperimentId experiment_rig_conflict = ExperimentId::from_value(0x5007);
    const ExperimentId experiment_recovery = ExperimentId::from_value(0x5008);
    const ExperimentId experiment_after_restart = ExperimentId::from_value(0x5009);

    // --- Section 1: compound placement, hard filtering, determinism ---------
    ScheduleMessage first;
    first.request = make_alpha_request(ScheduleRequestId::from_value(0x6001), experiment_alpha);
    DecisionMessage decision_first;
    proof.expect(client.call_schedule(first, decision_first).ok(), "schedule_alpha_compound_transport");
    proof.expect(decision_first.outcome == DecisionOutcome::Placed, "schedule_alpha_compound_placed",
                 decision_first.failure_detail + " | " + decision_first.explanation);
    {
        const std::string selected = resource_list(decision_first.placement);
        proof.expect(decision_first.placement.resources.size() == 3, "compound_placement_selected_three_resources",
                     selected);
        bool gpu = false;
        bool model = false;
        bool dataset = false;
        for (const SelectedResource& entry : decision_first.placement.resources) {
            if (entry.resource.value() == kAlphaGpuResource) {
                gpu = true;
            }
            if (entry.resource.value() == kAlphaModelResource) {
                model = true;
            }
            if (entry.resource.value() == kAlphaDatasetResource) {
                dataset = true;
            }
        }
        proof.expect(gpu && model && dataset, "compound_placement_uses_alpha_resources", selected);
        proof.expect(decision_first.explanation.find("DATASET_VERSION_MISMATCH") != std::string::npos,
                     "hard_filter_rejects_wrong_dataset_version");
        proof.expect(decision_first.explanation.find("residual_capacity") != std::string::npos,
                     "explanation_exposes_named_ranking_factors");
    }
    {
        std::string text;
        const std::string subject = "placement " + decision_first.placement.id.to_string();
        proof.expect(poll_until(client, subject, "COMPLETED", 256, text),
                     "alpha_assignment_completed_in_worker_process", text);
    }
    {
        ScheduleMessage second;
        second.request = make_alpha_request(ScheduleRequestId::from_value(0x6002), experiment_alpha_again);
        DecisionMessage decision_second;
        proof.expect(client.call_schedule(second, decision_second).ok(), "schedule_alpha_again_transport");
        proof.expect(decision_second.outcome == DecisionOutcome::Placed, "schedule_alpha_again_placed",
                     decision_second.failure_detail);
        proof.expect(resource_list(decision_second.placement) == resource_list(decision_first.placement),
                     "ranking_is_deterministic_for_identical_state",
                     resource_list(decision_second.placement) + " vs " + resource_list(decision_first.placement));
        std::string text;
        const std::string subject = "placement " + decision_second.placement.id.to_string();
        proof.expect(poll_until(client, subject, "COMPLETED", 256, text),
                     "second_alpha_assignment_completed", text);
    }
    {
        std::string accounting_text;
        proof.expect(inspect(client, "accounting", accounting_text), "accounting_inspect");
        proof.expect(accounting_text.find("active_placements=0") != std::string::npos,
                     "accounting_returns_to_baseline_after_completion", accounting_text);
        proof.expect(accounting_text.find("total_reserved=0") != std::string::npos,
                     "reservations_released_after_completion", accounting_text);
    }

    // --- Section 2: real worker death, reincarnation, stale replay ----------
    ScheduleMessage held_beta;
    held_beta.request = make_beta_request(ScheduleRequestId::from_value(0x6010), experiment_beta);
    const std::string held_beta_subject = "placement ";  // completed below
    static_cast<void>(held_beta_subject);

    proof.expect(beta.process.terminate().ok(), "terminate_idle_beta_incarnation");
    static_cast<void>(beta.process.wait());
    beta.process.close();
    proof.expect(start_worker(worker_exe, coordinator.port, "beta", 2, true, beta, error),
                 "worker_beta_restart_hold", error);
    const WorkerBootId beta_boot_before_death = beta.boot;
    const WorkerBootId beta_boot_original = beta_boot_before_death;

    DecisionMessage decision_beta;
    proof.expect(client.call_schedule(held_beta, decision_beta).ok(), "schedule_beta_compound_transport");
    proof.expect(decision_beta.outcome == DecisionOutcome::Placed, "schedule_beta_compound_placed",
                 decision_beta.failure_detail + " | " + decision_beta.explanation);
    proof.expect(decision_beta.placement.worker_boot == beta_boot_before_death,
                 "placement_bound_to_current_worker_incarnation");
    {
        std::string text;
        const std::string subject = "placement " + decision_beta.placement.id.to_string();
        proof.expect(poll_until(client, subject, "RUNNING", 256, text),
                     "held_placement_running_in_worker_process", text);
    }
    const CompletionClaim stale_claim = claim_from(decision_beta);
    proof.expect(beta.process.terminate().ok(), "terminate_beta_process_while_placement_is_live");
    static_cast<void>(beta.process.wait());
    beta.process.close();
    {
        std::string text;
        const std::string subject = "placement " + decision_beta.placement.id.to_string();
        proof.expect(poll_until(client, subject, "REASSIGNMENT_REQUIRED", 256, text),
                     "worker_death_marks_placement_reassignment_required", text);
        proof.expect(text.find("REVOKED") != std::string::npos,
                     "worker_death_revokes_reservation", text);
    }
    {
        std::string text;
        proof.expect(inspect(client, "stale", text), "stale_inspect");
        proof.expect(text.find(ResourceId::from_value(kBetaGpuResource).to_string()) != std::string::npos,
                     "dead_incarnation_resources_are_not_current", text);
    }
    {
        CompletionAckMessage ack;
        proof.expect(client.call_completion(stale_claim, ack).ok(), "replay_dead_incarnation_completion_transport");
        proof.expect(!ack.accepted, "dead_incarnation_completion_rejected");
        proof.expect(ack.code == ErrorCode::ReassignmentRequired || ack.code == ErrorCode::StaleWorker ||
                         ack.code == ErrorCode::StaleResource || ack.code == ErrorCode::StalePlacement,
                     "dead_incarnation_completion_error_code",
                     std::string(error_code_name(ack.code)));
    }
    {
        ScheduleMessage after_death;
        after_death.request = make_beta_request(ScheduleRequestId::from_value(0x6011), experiment_beta_restart);
        DecisionMessage decision_after_death;
        proof.expect(client.call_schedule(after_death, decision_after_death).ok(),
                     "schedule_after_worker_death_transport");
        proof.expect(decision_after_death.outcome == DecisionOutcome::NoPlacement,
                     "stale_evidence_prevents_placement", decision_after_death.failure_detail);
        proof.expect(decision_after_death.failure == ErrorCode::StaleResource,
                     "stale_evidence_failure_code_is_stale_resource",
                     std::string(error_code_name(decision_after_death.failure)) + " | " +
                         decision_after_death.explanation);
    }
    proof.expect(start_worker(worker_exe, coordinator.port, "beta", 3, false, beta, error),
                 "worker_beta_reincarnation", error);
    proof.expect(!(beta.boot == beta_boot_original), "worker_boot_identity_changes_on_restart");
    {
        ScheduleMessage after_reincarnation;
        after_reincarnation.request =
            make_beta_request(ScheduleRequestId::from_value(0x6012), experiment_beta_restart);
        DecisionMessage decision_after_reincarnation;
        proof.expect(client.call_schedule(after_reincarnation, decision_after_reincarnation).ok(),
                     "schedule_after_reincarnation_transport");
        proof.expect(decision_after_reincarnation.outcome == DecisionOutcome::Placed,
                     "reincarnated_worker_resources_are_eligible_again",
                     decision_after_reincarnation.failure_detail);
        std::string text;
        const std::string subject = "placement " + decision_after_reincarnation.placement.id.to_string();
        proof.expect(poll_until(client, subject, "COMPLETED", 256, text),
                     "reincarnated_worker_completes_fresh_placement", text);
        CompletionAckMessage ack;
        proof.expect(client.call_completion(stale_claim, ack).ok(), "replay_old_placement_completion_transport");
        proof.expect(!ack.accepted, "old_placement_cannot_commit_after_reassignment");
        CompletionAckMessage duplicate;
        proof.expect(client.call_completion(claim_from(decision_after_reincarnation), duplicate).ok(),
                     "duplicate_completion_transport");
        proof.expect(duplicate.duplicate, "duplicate_completion_is_harmless_and_detected");
    }

    // --- Section 3: cancellation semantics ----------------------------------
    {
        ScheduleMessage environment;
        environment.request =
            make_environment_request(ScheduleRequestId::from_value(0x6020), experiment_environment);
        DecisionMessage decision_environment;
        proof.expect(client.call_schedule(environment, decision_environment).ok(),
                     "schedule_virtual_environment_transport");
        proof.expect(decision_environment.outcome == DecisionOutcome::Placed, "virtual_environment_placed",
                     decision_environment.failure_detail);
        std::string text;
        const std::string subject = "placement " + decision_environment.placement.id.to_string();
        proof.expect(poll_until(client, subject, "RUNNING", 256, text), "virtual_environment_running", text);

        CancelPlacementMessage cancel;
        cancel.placement = decision_environment.placement.id;
        cancel.generation = decision_environment.placement.generation;
        cancel.epoch = decision_environment.authority.epoch;
        cancel.reason = "cancelled by the multiprocess proof";
        LifecycleAckMessage ack;
        proof.expect(client.call_cancel_placement(cancel, ack).ok(), "cancel_placement_transport");
        proof.expect(ack.state == PlacementState::Cancelled, "cancel_placement_reaches_cancelled_state");
        LifecycleAckMessage again;
        proof.expect(client.call_cancel_placement(cancel, again).ok(), "duplicate_cancel_transport");
        proof.expect(again.code == ErrorCode::Cancelled, "duplicate_cancellation_is_deterministic");
        CompletionAckMessage late;
        proof.expect(client.call_completion(claim_from(decision_environment), late).ok(),
                     "late_completion_transport");
        proof.expect(!late.accepted && late.code == ErrorCode::Cancelled,
                     "cancelled_placement_cannot_complete_later");
        std::string accounting_text;
        proof.expect(inspect(client, "accounting", accounting_text), "accounting_after_cancellation");
        proof.expect(accounting_text.find("total_reserved=0") != std::string::npos,
                     "cancellation_releases_capacity_exactly_once", accounting_text);
    }

    // --- Section 4: exclusive physical environment (SYNTHETIC rig) ----------
    DecisionMessage rig_decision;
    {
        ScheduleMessage rig;
        rig.request = make_rig_request(ScheduleRequestId::from_value(0x6030), experiment_rig);
        proof.expect(client.call_schedule(rig, rig_decision).ok(), "schedule_physical_rig_transport");
        proof.expect(rig_decision.outcome == DecisionOutcome::Placed, "synthetic_physical_rig_placed",
                     rig_decision.failure_detail + " | " + rig_decision.explanation);
        std::string text;
        const std::string subject = "placement " + rig_decision.placement.id.to_string();
        proof.expect(poll_until(client, subject, "RUNNING", 256, text), "physical_rig_running_held", text);
    }
    {
        ScheduleMessage conflicting;
        conflicting.request =
            make_rig_request(ScheduleRequestId::from_value(0x6031), experiment_rig_conflict);
        DecisionMessage decision_conflict;
        proof.expect(client.call_schedule(conflicting, decision_conflict).ok(), "schedule_rig_conflict_transport");
        proof.expect(decision_conflict.outcome == DecisionOutcome::NoPlacement,
                     "exclusive_rig_cannot_be_double_booked", decision_conflict.failure_detail);
        proof.expect(decision_conflict.failure == ErrorCode::ExclusivityConflict ||
                         decision_conflict.failure == ErrorCode::CapacityExhausted,
                     "exclusive_conflict_failure_code",
                     std::string(error_code_name(decision_conflict.failure)) + " | " +
                         decision_conflict.explanation);
    }

    // --- Section 5: real coordinator death and recovery ---------------------
    {
        StateAckMessage saved;
        proof.expect(client.call_save(state_path, saved).ok() && saved.code == ErrorCode::Ok,
                     "explicit_state_save_succeeds", saved.detail);
    }
    const CoordinatorEpoch pre_restart_epoch = coordinator.epoch;
    const CompletionClaim pre_restart_claim = claim_from(rig_decision);
    proof.expect(coordinator.process.terminate().ok(), "terminate_coordinator_process");
    static_cast<void>(coordinator.process.wait());
    coordinator.process.close();
    static_cast<void>(gamma.process.wait());
    gamma.process.close();

    if (!start_coordinator(coordinator_exe, state_path, coordinator, error)) {
        proof.expect(false, "coordinator_restart", error);
        cleanup();
        std::filesystem::remove_all(scratch, ignored);
        std::printf("proof result FAIL checks=%d failures=%d\n", proof.checks, proof.failures);
        return 1;
    }
    proof.expect(true, "coordinator_restart");
    proof.expect(coordinator.recovered, "restart_detects_persisted_state");
    proof.expect(coordinator.previous_epoch == pre_restart_epoch, "restart_preserves_previous_epoch");
    proof.expect(coordinator.epoch.value() > pre_restart_epoch.value(), "restart_advances_coordinator_epoch");
    proof.expect(coordinator.revalidation > 0, "restart_requires_resource_revalidation");
    proof.expect(coordinator.reconciled > 0, "restart_reconciles_in_flight_placements");
    proof.expect(coordinator.revoked > 0, "restart_revokes_prior_reservations");
    proof.expect(coordinator.preserved_history > 0, "restart_preserves_placement_history");

    Result<CoordinatorClient> reconnected = CoordinatorClient::connect(coordinator.port);
    if (!reconnected.ok()) {
        proof.expect(false, "client_reconnect_after_restart", reconnected.status().to_string());
        cleanup();
        std::filesystem::remove_all(scratch, ignored);
        std::printf("proof result FAIL checks=%d failures=%d\n", proof.checks, proof.failures);
        return 1;
    }
    client = reconnected.take();

    {
        std::string text;
        const std::string subject = "placement " + rig_decision.placement.id.to_string();
        proof.expect(poll_until(client, subject, "REASSIGNMENT_REQUIRED", 8, text),
                     "pre_restart_placement_reconciled", text);
        std::string stale_text;
        proof.expect(inspect(client, "stale", stale_text), "stale_inspect_after_restart");
        proof.expect(stale_text.find(ResourceId::from_value(kBetaGpuResource).to_string()) != std::string::npos,
                     "restored_resources_are_not_current_until_revalidated", stale_text);
        proof.expect(stale_text.find(ResourceId::from_value(kAlphaGpuResource).to_string()) != std::string::npos,
                     "all_worker_resources_require_revalidation_after_restart");
    }
    {
        CompletionAckMessage ack;
        proof.expect(client.call_completion(pre_restart_claim, ack).ok(), "replay_pre_restart_completion_transport");
        proof.expect(!ack.accepted && ack.code == ErrorCode::StaleEpoch,
                     "pre_restart_traffic_rejected_as_stale_epoch",
                     std::string(error_code_name(ack.code)));
    }
    {
        ScheduleMessage before_revalidation;
        before_revalidation.request =
            make_alpha_request(ScheduleRequestId::from_value(0x6040), experiment_recovery);
        DecisionMessage decision;
        proof.expect(client.call_schedule(before_revalidation, decision).ok(),
                     "schedule_before_revalidation_transport");
        proof.expect(decision.outcome == DecisionOutcome::NoPlacement,
                     "no_placement_before_workers_revalidate", decision.failure_detail);
    }

    proof.expect(start_worker(worker_exe, coordinator.port, "alpha", 2, false, alpha, error),
                 "worker_alpha_reregistered_after_restart", error);
    proof.expect(start_worker(worker_exe, coordinator.port, "beta", 4, false, beta, error),
                 "worker_beta_reregistered_after_restart", error);
    proof.expect(start_worker(worker_exe, coordinator.port, "gamma", 2, false, gamma, error),
                 "worker_gamma_reregistered_after_restart", error);
    {
        ScheduleMessage after_restart;
        after_restart.request =
            make_alpha_request(ScheduleRequestId::from_value(0x6041), experiment_after_restart);
        DecisionMessage decision;
        proof.expect(client.call_schedule(after_restart, decision).ok(), "schedule_after_recovery_transport");
        proof.expect(decision.outcome == DecisionOutcome::Placed, "fresh_work_placed_after_recovery",
                     decision.failure_detail);
        std::string text;
        const std::string subject = "placement " + decision.placement.id.to_string();
        proof.expect(poll_until(client, subject, "COMPLETED", 256, text),
                     "fresh_work_completed_after_recovery", text);
        CompletionAckMessage ack;
        proof.expect(client.call_completion(pre_restart_claim, ack).ok(), "replay_pre_restart_again_transport");
        proof.expect(!ack.accepted, "pre_restart_authority_never_commits");
    }
    {
        std::string accounting_text;
        proof.expect(inspect(client, "accounting", accounting_text), "accounting_after_recovery");
        proof.expect(accounting_text.find("active_placements=0") != std::string::npos,
                     "no_active_placements_after_recovery", accounting_text);
        proof.expect(accounting_text.find("total_reserved=0") != std::string::npos,
                     "capacity_accounting_clean_after_recovery", accounting_text);
        std::string invariants;
        proof.expect(inspect(client, "invariants", invariants), "invariants_inspect");
        proof.expect(invariants.find("invariants OK") != std::string::npos, "resource_invariants_hold", invariants);
    }

    // --- Section 6: frame hardening at the process boundary -----------------
    {
        auto expect_rejected = [&](const std::string& name, const std::vector<std::byte>& frame) {
            Result<TcpSocket> raw = connect_loopback(coordinator.port);
            if (!raw.ok()) {
                proof.expect(false, name, raw.status().to_string());
                return;
            }
            static_cast<void>(send_all(raw.value(), std::span<const std::byte>(frame.data(), frame.size())));
            raw.value().shutdown_both();
            raw.value().close();
            std::string text;
            proof.expect(inspect(client, "epoch", text), name + "_coordinator_still_serves");
        };

        std::vector<std::byte> empty_payload;
        const Result<std::vector<std::byte>> valid =
            encode_frame(MessageType::Inspect, 1, 0, std::span<const std::byte>(empty_payload.data(), 0));
        if (valid.ok()) {
            std::vector<std::byte> unknown = valid.value();
            unknown[8] = static_cast<std::byte>(0xff);
            unknown[9] = static_cast<std::byte>(0x7f);
            expect_rejected("unknown_message_type_rejected", unknown);

            std::vector<std::byte> corrupt = valid.value();
            if (corrupt.size() > 40) {
                corrupt[corrupt.size() - 1] = static_cast<std::byte>(
                    std::to_integer<std::uint8_t>(corrupt.back()) ^ 0x5a);
            }
            expect_rejected("checksum_mismatch_rejected", corrupt);

            std::vector<std::byte> oversized(4, std::byte{0});
            oversized[0] = static_cast<std::byte>(0xff);
            oversized[1] = static_cast<std::byte>(0xff);
            oversized[2] = static_cast<std::byte>(0xff);
            oversized[3] = static_cast<std::byte>(0x7f);
            expect_rejected("oversized_declared_length_rejected", oversized);

            std::vector<std::byte> truncated(valid.value().begin(), valid.value().begin() + 10);
            expect_rejected("truncated_frame_rejected", truncated);

            std::vector<std::byte> trailing = valid.value();
            trailing.push_back(std::byte{0x00});
            expect_rejected("trailing_bytes_rejected", trailing);
        } else {
            proof.expect(false, "frame_encode_for_hardening", valid.status().to_string());
        }
    }

    // --- Section 7: persisted state validation and closure accounting -------
    {
        StateAckMessage saved;
        proof.expect(client.call_save(state_path, saved).ok() && saved.code == ErrorCode::Ok,
                     "final_state_save_succeeds", saved.detail);
        Result<DurableState> durable = load_durable_state(state_path, Limits{});
        proof.expect(durable.ok(), "persisted_state_reloads", durable.ok() ? "" : durable.status().to_string());
        if (durable.ok()) {
            proof.expect(validate_durable_state(durable.value()).ok(), "persisted_state_validates");
            proof.expect(!durable.value().placements.empty(), "persisted_state_keeps_placement_history");
        }
        std::string text;
        proof.expect(inspect(client, "epoch", text), "final_epoch_inspect");
        std::string accounting_text;
        proof.expect(inspect(client, "accounting", accounting_text), "final_accounting_inspect");
        proof.expect(accounting_text.find("total_reserved=0") != std::string::npos,
                     "final_capacity_baseline_restored", accounting_text);
    }

    cleanup();
    std::filesystem::remove_all(scratch, ignored);
    proof.expect(!std::filesystem::exists(scratch), "proof_scratch_removed");

    std::printf("proof result %s checks=%d failures=%d\n", proof.failures == 0 ? "PASS" : "FAIL", proof.checks,
                proof.failures);
    std::fflush(stdout);
    return proof.failures == 0 ? 0 : 1;
}