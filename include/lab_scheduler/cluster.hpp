#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "lab_scheduler/engine.hpp"
#include "lab_scheduler/error.hpp"
#include "lab_scheduler/limits.hpp"
#include "lab_scheduler/net.hpp"
#include "lab_scheduler/protocol.hpp"
#include "lab_scheduler/resource.hpp"

namespace lab_scheduler {

NetworkRuntime& network_runtime();

struct CoordinatorConfig {
    std::uint16_t port = 0;  // 0 selects an ephemeral loopback port
    std::string state_path{};  // empty disables persistence
    Limits limits{};
    bool persist_on_mutation = true;
    std::size_t max_connections = 32;
    std::string coordinator_name = "lab-scheduler-coordinator";
};

struct CoordinatorStatus {
    std::uint16_t port = 0;
    CoordinatorEpoch epoch{};
    bool recovered = false;
    RecoveryReport recovery{};
};

// The reference coordinator: a real process that owns an Engine and serves
// framed loopback TCP. Every accepted mutation is persisted, so placement
// history and reservations survive a hard kill of the process.
class CoordinatorServer {
public:
    CoordinatorServer();
    ~CoordinatorServer();

    CoordinatorServer(const CoordinatorServer&) = delete;
    CoordinatorServer& operator=(const CoordinatorServer&) = delete;

    Result<CoordinatorStatus> start(const CoordinatorConfig& config);
    Status run();
    void request_shutdown();
    Status save_state();
    [[nodiscard]] Status save_state(const std::string& path);

    [[nodiscard]] SchedulerEngine& engine() { return *engine_; }
    [[nodiscard]] const SchedulerEngine& engine() const { return *engine_; }
    [[nodiscard]] CoordinatorEpoch epoch() const { return engine_->epoch(); }
    [[nodiscard]] std::uint16_t port() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_{};
    std::unique_ptr<SchedulerEngine> engine_{};
    std::string state_path_{};
    bool persist_on_mutation_ = true;
};

// Blocking request/response client for the coordinator protocol.
class CoordinatorClient {
public:
    CoordinatorClient() = default;
    ~CoordinatorClient();

    CoordinatorClient(const CoordinatorClient&) = delete;
    CoordinatorClient& operator=(const CoordinatorClient&) = delete;
    CoordinatorClient(CoordinatorClient&&) = default;
    CoordinatorClient& operator=(CoordinatorClient&&) = default;

    static Result<CoordinatorClient> connect(std::uint16_t port, Limits limits = Limits{},
                                             const std::string& host = "127.0.0.1");

    Status call_hello(const HelloMessage& message, HelloAckMessage& reply);
    Status call_register(const ResourceMessage& message, ResourceAckMessage& reply);
    Status call_publish(const ResourceMessage& message, ResourceAckMessage& reply);
    Status call_retire(const RetireMessage& message, ResourceAckMessage& reply);
    Status call_schedule(const ScheduleMessage& message, DecisionMessage& reply);
    Status call_assign_ack(const AuthorityMessage& message, LifecycleAckMessage& reply);
    Status call_running(const AuthorityMessage& message, LifecycleAckMessage& reply);
    Status call_completion(const CompletionClaim& claim, CompletionAckMessage& reply);
    Status call_cancel_placement(const CancelPlacementMessage& message, LifecycleAckMessage& reply);
    Status call_cancel_request(const CancelRequestMessage& message, LifecycleAckMessage& reply);
    Status call_reassign(const CancelPlacementMessage& message, LifecycleAckMessage& reply);
    Status call_preempt(const CancelPlacementMessage& message, LifecycleAckMessage& reply);
    Status call_disconnect_worker(const DisconnectWorkerMessage& message, StateAckMessage& reply);
    Status call_inspect(const std::string& subject, std::string& text);
    Status call_save(const std::string& path, StateAckMessage& reply);
    Status call_shutdown(StateAckMessage& reply);

    Status send_frame(MessageType type, const std::vector<std::byte>& payload);
    Result<FrameView> receive(std::vector<std::byte>& storage);
    Result<std::vector<std::byte>> transact(MessageType request_type, const std::vector<std::byte>& payload,
                                            std::vector<std::byte>& storage);
    [[nodiscard]] std::uint64_t next_correlation() noexcept { return ++correlation_; }
    [[nodiscard]] bool connected() const noexcept { return socket_.valid(); }
    [[nodiscard]] TcpSocket& socket() noexcept { return socket_; }
    void close() noexcept { socket_.close(); }

private:
    TcpSocket socket_{};
    Limits limits_{};
    std::uint64_t correlation_ = 0;
    std::vector<std::byte> storage_{};
};

struct WorkerConfig {
    WorkerId worker{};
    WorkerBootId boot{};
    std::string profile{};
    std::uint16_t port = 0;
    std::uint32_t current_occupancy = 0;
    Limits limits{};
    std::string authority = "lab-scheduler-worker";
    // When set the worker acknowledges assignments and reports RUNNING but
    // does not report completion. The multiprocess proof uses this to hold a
    // live placement across a real process death.
    bool hold_assignments = false;
};

// Executes an assignment. The default task is a deterministic synthetic
// workload; the CUDA proof replaces it with real accelerator work.
using WorkerTask = std::function<Status(const AuthorityEnvelope&, std::string& detail)>;

class WorkerAgent {
public:
    WorkerAgent() = default;
    ~WorkerAgent();

    WorkerAgent(const WorkerAgent&) = delete;
    WorkerAgent& operator=(const WorkerAgent&) = delete;

    Result<HelloAckMessage> start(const WorkerConfig& config, WorkerTask task = WorkerTask{});
    Status run();
    void request_exit() noexcept { exit_requested_ = true; }

    [[nodiscard]] const std::vector<ResourceId>& resource_ids() const noexcept { return resource_ids_; }
    [[nodiscard]] const CoordinatorClient& client() const noexcept { return client_; }
    [[nodiscard]] std::uint32_t completed_assignments() const noexcept { return completed_assignments_; }
    [[nodiscard]] std::uint32_t rejected_assignments() const noexcept { return rejected_assignments_; }

private:
    Status handle_assignment(const AssignMessage& message);

    CoordinatorClient client_{};
    std::vector<ResourceId> resource_ids_{};
    WorkerTask task_{};
    WorkerId worker_{};
    WorkerBootId boot_{};
    Limits limits_{};
    bool exit_requested_ = false;
    bool hold_assignments_ = false;
    std::uint32_t completed_assignments_ = 0;
    std::uint32_t rejected_assignments_ = 0;
    std::vector<std::byte> storage_{};
};

}  // namespace lab_scheduler
