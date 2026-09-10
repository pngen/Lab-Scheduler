#include "lab_scheduler/cluster.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <exception>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "lab_scheduler/canonical.hpp"
#include "lab_scheduler/inspection.hpp"
#include "lab_scheduler/lab_profiles.hpp"
#include "lab_scheduler/persistence.hpp"
#include "lab_scheduler/version.hpp"

namespace lab_scheduler {

NetworkRuntime& network_runtime() {
    static NetworkRuntime runtime;
    return runtime;
}

namespace {

// One connected peer. The write mutex serializes frames sent to the same peer
// from different coordinator threads: a controller thread answering a schedule
// request may push an assignment to a worker connection that another thread is
// reading.
struct Peer {
    TcpSocket socket{};
    mutable std::mutex write_mutex{};
    WorkerId worker{};
    WorkerBootId boot{};
    bool is_worker = false;

    Status send(MessageType type, std::uint64_t correlation, std::uint64_t epoch,
                const std::vector<std::byte>& payload) {
        const std::lock_guard<std::mutex> guard(write_mutex);
        return send_frame(socket, type, correlation, epoch, payload);
    }
};

std::string synthetic_workload_result(const AuthorityEnvelope& authority, const std::string& workload) {
    std::uint64_t hash = fnv1a64(workload);
    for (const SelectedResource& selected : authority.resources) {
        hash = mix64(hash ^ selected.resource.value());
    }
    return authority.placement.to_string() + "/" + hex_u64(hash, 16);
}

struct SubjectArgument {
    std::string verb{};
    std::string argument{};
};

SubjectArgument split_subject(const std::string& subject) {
    SubjectArgument out;
    const std::size_t space = subject.find(' ');
    if (space == std::string::npos) {
        out.verb = subject;
        return out;
    }
    out.verb = subject.substr(0, space);
    out.argument = subject.substr(space + 1);
    return out;
}

std::string inspect_subject(SchedulerEngine& engine, const std::string& subject, const Limits& limits) {
    const SubjectArgument parsed = split_subject(subject);
    if (parsed.verb == "resources") {
        return render_resources(engine.list_resources());
    }
    if (parsed.verb == "requests") {
        return render_requests(engine.list_requests());
    }
    if (parsed.verb == "placements") {
        return render_placements(engine.list_placements());
    }
    if (parsed.verb == "reservations") {
        return render_reservations(engine.list_reservations());
    }
    if (parsed.verb == "stale") {
        return render_stale_resources(engine.list_stale_resources());
    }
    if (parsed.verb == "workers") {
        return render_workers(engine.list_workers());
    }
    if (parsed.verb == "epoch") {
        return render_epoch(engine.epoch());
    }
    if (parsed.verb == "accounting") {
        return render_accounting(engine.accounting());
    }
    if (parsed.verb == "invariants") {
        return render_invariants(engine.validate_invariants());
    }
    if (parsed.verb == "audit") {
        return render_audit(engine.list_audit());
    }
    if (parsed.verb == "resource") {
        const std::optional<ResourceId> id = ResourceId::parse(parsed.argument);
        if (!id.has_value()) {
            return "error invalid resource identity: " + parsed.argument + "\n";
        }
        Result<ResourceRecord> resource = engine.get_resource(*id);
        if (!resource.ok()) {
            return "error " + resource.status().to_string() + "\n";
        }
        return render_resource_detail(resource.value());
    }
    if (parsed.verb == "request") {
        const std::optional<ScheduleRequestId> id = ScheduleRequestId::parse(parsed.argument);
        if (!id.has_value()) {
            return "error invalid scheduling request identity: " + parsed.argument + "\n";
        }
        Result<StoredRequest> stored = engine.get_request(*id);
        if (!stored.ok()) {
            return "error " + stored.status().to_string() + "\n";
        }
        return render_request_detail(stored.value());
    }
    if (parsed.verb == "placement") {
        const std::optional<PlacementId> id = PlacementId::parse(parsed.argument);
        if (!id.has_value()) {
            return "error invalid placement identity: " + parsed.argument + "\n";
        }
        Result<PlacementRecord> placement = engine.get_placement(*id);
        if (!placement.ok()) {
            return "error " + placement.status().to_string() + "\n";
        }
        ReservationRecord reservation;
        bool have_reservation = false;
        Result<ReservationRecord> found = engine.get_reservation(placement.value().reservation);
        if (found.ok()) {
            reservation = found.value();
            have_reservation = true;
        }
        return render_placement_detail(placement.value(), have_reservation ? &reservation : nullptr);
    }
    if (parsed.verb == "explain") {
        const std::optional<DecisionId> id = DecisionId::parse(parsed.argument);
        if (!id.has_value()) {
            return "error invalid decision identity: " + parsed.argument + "\n";
        }
        Result<Decision> decision = engine.get_decision(*id);
        if (!decision.ok()) {
            return "error " + decision.status().to_string() + "\n";
        }
        return render_explanation(decision.value().explanation);
    }
    if (parsed.verb == "explain-placement") {
        const std::optional<PlacementId> id = PlacementId::parse(parsed.argument);
        if (!id.has_value()) {
            return "error invalid placement identity: " + parsed.argument + "\n";
        }
        Result<PlacementRecord> placement = engine.get_placement(*id);
        if (!placement.ok()) {
            return "error " + placement.status().to_string() + "\n";
        }
        Result<Decision> decision = engine.get_decision(placement.value().decision);
        if (!decision.ok()) {
            return "error " + decision.status().to_string() + "\n";
        }
        return render_explanation(decision.value().explanation);
    }
    if (parsed.verb == "state-validate") {
        Result<DurableState> state = load_durable_state(parsed.argument, limits);
        if (!state.ok()) {
            return "error " + state.status().to_string() + "\n";
        }
        return render_durable_state_validation(state.value());
    }
    return "error unknown inspection subject: " + subject + "\n";
}

}  // namespace

struct CoordinatorServer::Impl {
    CoordinatorConfig config{};
    TcpListener listener{};
    std::atomic<bool> shutdown_requested{false};
    std::mutex peers_mutex{};
    std::vector<std::weak_ptr<Peer>> peers{};
    std::mutex threads_mutex{};
    std::vector<std::thread> threads{};
    // Persistence is serialized: two accepted mutations can be committed
    // concurrently by different connection threads, and an interleaved write
    // must never be able to become authoritative.
    std::mutex persist_mutex{};

    void register_peer(const std::shared_ptr<Peer>& peer) {
        const std::lock_guard<std::mutex> guard(peers_mutex);
        peers.erase(std::remove_if(peers.begin(), peers.end(),
                                   [](const std::weak_ptr<Peer>& entry) { return entry.expired(); }),
                    peers.end());
        peers.push_back(peer);
    }

    std::shared_ptr<Peer> find_worker_peer(WorkerId worker) {
        const std::lock_guard<std::mutex> guard(peers_mutex);
        for (const std::weak_ptr<Peer>& entry : peers) {
            const std::shared_ptr<Peer> peer = entry.lock();
            if (peer && peer->is_worker && peer->worker == worker) {
                return peer;
            }
        }
        return nullptr;
    }

    std::vector<std::shared_ptr<Peer>> worker_peers() {
        std::vector<std::shared_ptr<Peer>> out;
        const std::lock_guard<std::mutex> guard(peers_mutex);
        for (const std::weak_ptr<Peer>& entry : peers) {
            const std::shared_ptr<Peer> peer = entry.lock();
            if (peer && peer->is_worker) {
                out.push_back(peer);
            }
        }
        return out;
    }
};

CoordinatorServer::CoordinatorServer() : impl_(std::make_unique<Impl>()) {}

CoordinatorServer::~CoordinatorServer() {
    request_shutdown();
    if (impl_ && impl_->listener.valid()) {
        impl_->listener.close();
    }
    if (impl_) {
        const std::lock_guard<std::mutex> guard(impl_->threads_mutex);
        for (std::thread& thread : impl_->threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }
}

std::uint16_t CoordinatorServer::port() const noexcept {
    return impl_ ? impl_->listener.bound_port() : 0;
}

Result<CoordinatorStatus> CoordinatorServer::start(const CoordinatorConfig& config) {
    EngineConfig engine_config;
    engine_config.limits = config.limits;
    persist_on_mutation_ = config.persist_on_mutation;
    state_path_ = config.state_path;
    engine_ = std::make_unique<SchedulerEngine>(engine_config);

    CoordinatorStatus status;
    if (!config.state_path.empty()) {
        const std::ifstream existing(config.state_path, std::ios::binary);
        if (existing.good()) {
            Result<DurableState> durable = load_durable_state(config.state_path, config.limits);
            if (!durable.ok()) {
                return durable.status();
            }
            Result<RecoveryReport> recovery = engine_->import_state(durable.value());
            if (!recovery.ok()) {
                return recovery.status();
            }
            status.recovered = true;
            status.recovery = recovery.value();
        }
    }

    static_cast<void>(network_runtime());
    impl_->config = config;
    Status listen_status = impl_->listener.listen_loopback(config.port, 64);
    if (!listen_status.ok()) {
        return listen_status;
    }
    status.port = impl_->listener.bound_port();
    status.epoch = engine_->epoch();
    return status;
}

Status CoordinatorServer::save_state() { return save_state(state_path_); }

Status CoordinatorServer::save_state(const std::string& path) {
    if (path.empty()) {
        return Status(ErrorCode::InvalidArgument, "no persistence path configured");
    }
    const DurableState state = engine_->export_state();
    const std::lock_guard<std::mutex> guard(impl_->persist_mutex);
    return save_durable_state(state, path, impl_->config.limits);
}

void CoordinatorServer::request_shutdown() {
    if (!impl_) {
        return;
    }
    const bool already_requested = impl_->shutdown_requested.exchange(true);
    if (already_requested || !impl_->listener.valid()) {
        return;
    }
    // A blocked accept() is woken by connecting to our own listener and
    // dropping the connection: this is deterministic and does not depend on
    // the platform closing a socket out from under another thread.
    Result<TcpSocket> wake = connect_loopback(impl_->listener.bound_port());
    if (wake.ok()) {
        wake.value().close();
    }
}

Status CoordinatorServer::run() {
    if (!impl_ || !impl_->listener.valid()) {
        if (impl_ && impl_->shutdown_requested.load()) {
            return Status{};
        }
        return Status(ErrorCode::InvalidArgument, "coordinator is not listening");
    }

    while (!impl_->shutdown_requested.load()) {
        std::string peer_text;
        Result<TcpSocket> accepted = impl_->listener.accept_one(peer_text);
        if (!accepted.ok()) {
            if (impl_->shutdown_requested.load()) {
                break;
            }
            continue;
        }
        auto peer = std::make_shared<Peer>();
        peer->socket = accepted.take();
        impl_->register_peer(peer);

        {
            const std::lock_guard<std::mutex> guard(impl_->threads_mutex);
            impl_->threads.emplace_back([this, peer]() {
                SchedulerEngine& engine = *engine_;
                const Limits& limits = impl_->config.limits;
                std::vector<std::byte> storage;
                bool announced_worker = false;

                try {
                    for (;;) {
                        Result<FrameView> frame = receive_frame(peer->socket, limits, storage);
                        if (!frame.ok()) {
                            break;
                        }
                        const FrameView view = frame.value();
                        const std::uint64_t correlation = view.correlation_id;

                        auto reply_error = [&](ErrorCode code, const std::string& message) {
                            ErrorMessage error;
                            error.code = code;
                            error.message = message;
                            Result<std::vector<std::byte>> payload = encode_error_message(error, limits);
                            if (payload.ok()) {
                                static_cast<void>(peer->send(MessageType::Error, correlation,
                                                             engine.epoch().value(), payload.value()));
                            }
                        };

                        auto push_assignment = [&](const Decision& decision, const std::string& workload) {
                            if (decision.outcome != DecisionOutcome::Placed ||
                                decision.placement.worker.is_null()) {
                                return;
                            }
                            const std::shared_ptr<Peer> worker_peer =
                                impl_->find_worker_peer(decision.placement.worker);
                            if (!worker_peer) {
                                return;
                            }
                            AssignMessage assignment;
                            assignment.authority = decision.authority;
                            assignment.workload = workload;
                            Result<std::vector<std::byte>> payload = encode_assign(assignment, limits);
                            if (payload.ok()) {
                                static_cast<void>(worker_peer->send(MessageType::Assign, correlation,
                                                                    engine.epoch().value(), payload.value()));
                            }
                        };

                        bool mutated = false;
                        switch (view.type) {
                            case MessageType::Hello: {
                                Result<HelloMessage> hello = decode_hello(view.payload, limits);
                                if (!hello.ok()) {
                                    reply_error(hello.code(), hello.message());
                                    break;
                                }
                                static_cast<void>(
                                    engine.worker_connected(hello.value().worker, hello.value().boot));
                                peer->worker = hello.value().worker;
                                peer->boot = hello.value().boot;
                                peer->is_worker = true;
                                announced_worker = true;
                                HelloAckMessage ack;
                                ack.epoch = engine.epoch();
                                ack.coordinator = impl_->config.coordinator_name;
                                Result<std::vector<std::byte>> payload = encode_hello_ack(ack, limits);
                                if (payload.ok()) {
                                    static_cast<void>(peer->send(MessageType::HelloAck, correlation,
                                                                 engine.epoch().value(), payload.value()));
                                }
                                break;
                            }
                            case MessageType::RegisterResource:
                            case MessageType::PublishState: {
                                Result<ResourceMessage> message = decode_resource_message(view.payload, limits);
                                if (!message.ok()) {
                                    reply_error(message.code(), message.message());
                                    break;
                                }
                                Result<ResourceRecord> result =
                                    view.type == MessageType::RegisterResource
                                        ? engine.register_resource(message.value().advertisement)
                                        : engine.publish_resource_state(message.value().advertisement);
                                ResourceAckMessage ack;
                                ack.resource = message.value().advertisement.id;
                                if (result.ok()) {
                                    ack.generation = result.value().generation;
                                    ack.lifecycle = result.value().lifecycle;
                                    mutated = true;
                                } else {
                                    ack.code = result.code();
                                    ack.detail = result.message();
                                }
                                Result<std::vector<std::byte>> payload = encode_resource_ack(ack, limits);
                                if (payload.ok()) {
                                    static_cast<void>(peer->send(MessageType::ResourceAck, correlation,
                                                                 engine.epoch().value(), payload.value()));
                                }
                                break;
                            }
                            case MessageType::RetireResource: {
                                Result<RetireMessage> message = decode_retire(view.payload, limits);
                                if (!message.ok()) {
                                    reply_error(message.code(), message.message());
                                    break;
                                }
                                Result<ResourceRecord> result = engine.retire_resource(
                                    message.value().resource, message.value().generation);
                                ResourceAckMessage ack;
                                ack.resource = message.value().resource;
                                if (result.ok()) {
                                    ack.generation = result.value().generation;
                                    ack.lifecycle = result.value().lifecycle;
                                    mutated = true;
                                } else {
                                    ack.code = result.code();
                                    ack.detail = result.message();
                                }
                                Result<std::vector<std::byte>> payload = encode_resource_ack(ack, limits);
                                if (payload.ok()) {
                                    static_cast<void>(peer->send(MessageType::ResourceAck, correlation,
                                                                 engine.epoch().value(), payload.value()));
                                }
                                break;
                            }
                            case MessageType::ScheduleRequest: {
                                Result<ScheduleMessage> message = decode_schedule(view.payload, limits);
                                if (!message.ok()) {
                                    reply_error(message.code(), message.message());
                                    break;
                                }
                                Result<Decision> decision = engine.submit_request(message.value().request);
                                DecisionMessage reply;
                                if (decision.ok()) {
                                    mutated = true;
                                    const Decision& value = decision.value();
                                    reply.decision = value.id;
                                    reply.outcome = value.outcome;
                                    reply.failure = value.failure;
                                    reply.failure_detail = value.failure_detail;
                                    reply.placement = value.placement;
                                    reply.authority = value.authority;
                                    reply.explanation = render_explanation(value.explanation);
                                } else {
                                    reply.outcome = DecisionOutcome::NoPlacement;
                                    reply.failure = decision.code();
                                    reply.failure_detail = decision.message();
                                }
                                Result<std::vector<std::byte>> payload = encode_decision(reply, limits);
                                if (payload.ok()) {
                                    static_cast<void>(peer->send(MessageType::Decision, correlation,
                                                                 engine.epoch().value(), payload.value()));
                                }
                                if (decision.ok()) {
                                    push_assignment(decision.value(), message.value().request.workload);
                                }
                                break;
                            }
                            case MessageType::AssignAck:
                            case MessageType::Running: {
                                Result<AuthorityMessage> message = decode_authority(view.payload, limits);
                                if (!message.ok()) {
                                    reply_error(message.code(), message.message());
                                    break;
                                }
                                Result<PlacementRecord> result =
                                    view.type == MessageType::AssignAck
                                        ? engine.mark_assigned(message.value().placement,
                                                               message.value().generation,
                                                               message.value().epoch,
                                                               message.value().worker_boot)
                                        : engine.mark_running(message.value().placement,
                                                              message.value().generation,
                                                              message.value().epoch,
                                                              message.value().worker_boot);
                                LifecycleAckMessage ack;
                                ack.placement = message.value().placement;
                                ack.generation = message.value().generation;
                                if (result.ok()) {
                                    ack.state = result.value().state;
                                    mutated = true;
                                } else {
                                    ack.code = result.code();
                                    ack.detail = result.message();
                                }
                                Result<std::vector<std::byte>> payload = encode_lifecycle_ack(ack, limits);
                                if (payload.ok()) {
                                    static_cast<void>(peer->send(MessageType::LifecycleAck, correlation,
                                                                 engine.epoch().value(), payload.value()));
                                }
                                break;
                            }
                            case MessageType::Completion: {
                                Result<CompletionClaim> claim = decode_completion(view.payload, limits);
                                if (!claim.ok()) {
                                    reply_error(claim.code(), claim.message());
                                    break;
                                }
                                const CompletionOutcome outcome = engine.report_completion(claim.value());
                                CompletionAckMessage ack;
                                ack.placement = claim.value().placement;
                                ack.state = outcome.state;
                                ack.accepted = outcome.accepted;
                                ack.duplicate = outcome.duplicate;
                                ack.code = outcome.status.code;
                                ack.detail = outcome.status.message;
                                ack.released_reservations =
                                    static_cast<std::uint32_t>(outcome.released_reservations.size());
                                mutated = outcome.accepted;
                                Result<std::vector<std::byte>> payload = encode_completion_ack(ack, limits);
                                if (payload.ok()) {
                                    static_cast<void>(peer->send(MessageType::CompletionAck, correlation,
                                                                 engine.epoch().value(), payload.value()));
                                }
                                break;
                            }
                            case MessageType::CancelPlacement: {
                                Result<CancelPlacementMessage> message =
                                    decode_cancel_placement(view.payload, limits);
                                if (!message.ok()) {
                                    reply_error(message.code(), message.message());
                                    break;
                                }
                                const CancellationReport report = engine.cancel_placement(
                                    message.value().placement, message.value().generation,
                                    message.value().epoch, message.value().reason);
                                LifecycleAckMessage ack;
                                ack.placement = message.value().placement;
                                ack.generation = message.value().generation;
                                ack.state = report.state;
                                ack.code = report.status.code;
                                ack.detail = report.status.ok() ? report.detail : report.status.message;
                                ack.released_reservations =
                                    static_cast<std::uint32_t>(report.released_reservations.size());
                                mutated = report.status.ok();
                                Result<std::vector<std::byte>> payload = encode_lifecycle_ack(ack, limits);
                                if (payload.ok()) {
                                    static_cast<void>(peer->send(MessageType::LifecycleAck, correlation,
                                                                 engine.epoch().value(), payload.value()));
                                }
                                break;
                            }
                            case MessageType::CancelRequest: {
                                Result<CancelRequestMessage> message =
                                    decode_cancel_request(view.payload, limits);
                                if (!message.ok()) {
                                    reply_error(message.code(), message.message());
                                    break;
                                }
                                const CancellationReport report =
                                    engine.cancel_request(message.value().request, message.value().reason);
                                LifecycleAckMessage ack;
                                ack.code = report.status.code;
                                ack.detail = report.status.ok() ? report.detail : report.status.message;
                                ack.released_reservations =
                                    static_cast<std::uint32_t>(report.released_reservations.size());
                                ack.affected = static_cast<std::uint32_t>(report.cancelled_placements.size());
                                mutated = report.status.ok();
                                Result<std::vector<std::byte>> payload = encode_lifecycle_ack(ack, limits);
                                if (payload.ok()) {
                                    static_cast<void>(peer->send(MessageType::LifecycleAck, correlation,
                                                                 engine.epoch().value(), payload.value()));
                                }
                                break;
                            }
                            case MessageType::Reassign:
                            case MessageType::Preempt: {
                                Result<CancelPlacementMessage> message =
                                    decode_cancel_placement(view.payload, limits);
                                if (!message.ok()) {
                                    reply_error(message.code(), message.message());
                                    break;
                                }
                                Result<ReassignmentReport> report =
                                    view.type == MessageType::Reassign
                                        ? engine.reassign_placement(message.value().placement,
                                                                    message.value().generation,
                                                                    message.value().epoch,
                                                                    message.value().reason)
                                        : engine.preempt_placement(message.value().placement,
                                                                   message.value().generation,
                                                                   message.value().epoch,
                                                                   message.value().reason);
                                LifecycleAckMessage ack;
                                ack.placement = message.value().placement;
                                ack.generation = message.value().generation;
                                if (report.ok()) {
                                    mutated = true;
                                    ack.code = report.value().status.code;
                                    ack.detail = report.value().status.ok()
                                                     ? std::string("replacement scheduled")
                                                     : report.value().status.message;
                                    ack.released_reservations = static_cast<std::uint32_t>(
                                        report.value().revoked_reservations.size());
                                    if (!report.value().new_placement.is_null()) {
                                        ack.placement = report.value().new_placement;
                                        ack.generation = report.value().new_generation;
                                        ack.affected = 1;
                                    }
                                } else {
                                    ack.code = report.code();
                                    ack.detail = report.message();
                                }
                                Result<std::vector<std::byte>> payload = encode_lifecycle_ack(ack, limits);
                                if (payload.ok()) {
                                    static_cast<void>(peer->send(MessageType::LifecycleAck, correlation,
                                                                 engine.epoch().value(), payload.value()));
                                }
                                if (report.ok() && report.value().status.ok()) {
                                    push_assignment(report.value().decision, std::string());
                                }
                                break;
                            }
                            case MessageType::Inspect: {
                                Result<InspectMessage> message = decode_inspect(view.payload, limits);
                                if (!message.ok()) {
                                    reply_error(message.code(), message.message());
                                    break;
                                }
                                InspectReplyMessage reply;
                                reply.subject = message.value().subject;
                                reply.text = inspect_subject(engine, message.value().subject,
                                                             impl_->config.limits);
                                Result<std::vector<std::byte>> payload = encode_inspect_reply(reply, limits);
                                if (payload.ok()) {
                                    static_cast<void>(peer->send(MessageType::InspectReply, correlation,
                                                                 engine.epoch().value(), payload.value()));
                                }
                                break;
                            }
                            case MessageType::SaveState: {
                                Result<SaveStateMessage> message = decode_save_state(view.payload, limits);
                                if (!message.ok()) {
                                    reply_error(message.code(), message.message());
                                    break;
                                }
                                const std::string path = message.value().path.empty()
                                                             ? impl_->config.state_path
                                                             : message.value().path;
                                const Status saved = save_state(path);
                                StateAckMessage ack;
                                ack.path = path;
                                ack.epoch = engine.epoch();
                                const AccountingReport accounting = engine.accounting();
                                ack.resources = static_cast<std::uint32_t>(accounting.resources.size());
                                ack.requests = static_cast<std::uint32_t>(accounting.requests_recorded);
                                ack.placements = static_cast<std::uint32_t>(accounting.placements_recorded);
                                ack.reservations = static_cast<std::uint32_t>(accounting.reservations_recorded);
                                ack.code = saved.code;
                                ack.detail = saved.message;
                                Result<std::vector<std::byte>> payload = encode_state_ack(ack, limits);
                                if (payload.ok()) {
                                    static_cast<void>(peer->send(MessageType::StateAck, correlation,
                                                                 engine.epoch().value(), payload.value()));
                                }
                                break;
                            }
                            case MessageType::DisconnectWorker: {
                                Result<DisconnectWorkerMessage> message =
                                    decode_disconnect_worker(view.payload, limits);
                                if (!message.ok()) {
                                    reply_error(message.code(), message.message());
                                    break;
                                }
                                const ResourceLossReport report = engine.mark_worker_lost(
                                    message.value().worker, message.value().boot, message.value().detail);
                                StateAckMessage ack;
                                ack.epoch = engine.epoch();
                                ack.resources = static_cast<std::uint32_t>(report.invalidated_resources.size());
                                ack.placements = static_cast<std::uint32_t>(report.affected_placements.size());
                                ack.reservations =
                                    static_cast<std::uint32_t>(report.released_reservations.size());
                                ack.code = report.status.code;
                                ack.detail = report.status.ok() ? report.detail : report.status.message;
                                mutated = report.status.ok();
                                Result<std::vector<std::byte>> payload = encode_state_ack(ack, limits);
                                if (payload.ok()) {
                                    static_cast<void>(peer->send(MessageType::StateAck, correlation,
                                                                 engine.epoch().value(), payload.value()));
                                }
                                break;
                            }
                            case MessageType::Shutdown: {
                                StateAckMessage ack;
                                ack.epoch = engine.epoch();
                                ack.detail = "coordinator shutting down";
                                Result<std::vector<std::byte>> payload = encode_state_ack(ack, limits);
                                if (payload.ok()) {
                                    static_cast<void>(peer->send(MessageType::ShutdownAck, correlation,
                                                                 engine.epoch().value(), payload.value()));
                                }
                                for (const std::shared_ptr<Peer>& worker_peer : impl_->worker_peers()) {
                                    StateAckMessage worker_ack;
                                    worker_ack.detail = "coordinator shutting down";
                                    Result<std::vector<std::byte>> worker_payload =
                                        encode_state_ack(worker_ack, limits);
                                    if (worker_payload.ok()) {
                                        static_cast<void>(worker_peer->send(MessageType::Shutdown, 0,
                                                                            engine.epoch().value(),
                                                                            worker_payload.value()));
                                    }
                                }
                                request_shutdown();
                                break;
                            }
                            default:
                                reply_error(ErrorCode::ProtocolError,
                                            "message type " + std::string(message_type_name(view.type)) +
                                                " is not accepted by the coordinator on this connection");
                                break;
                        }
                        if (mutated && impl_->config.persist_on_mutation &&
                            !impl_->config.state_path.empty()) {
                            static_cast<void>(save_state());
                        }
                    }

                    if (announced_worker && !peer->worker.is_null() && !peer->boot.is_null()) {
                        static_cast<void>(engine.mark_worker_lost(peer->worker, peer->boot,
                                                                  "worker connection ended"));
                        if (impl_->config.persist_on_mutation && !impl_->config.state_path.empty()) {
                            static_cast<void>(save_state());
                        }
                    }
                    peer->socket.close();
                } catch (const std::exception& error) {
                    std::fprintf(stderr, "coordinator connection failed: %s\n", error.what());
                    std::fflush(stderr);
                    peer->socket.close();
                } catch (...) {
                    std::fprintf(stderr, "coordinator connection failed: unknown exception\n");
                    std::fflush(stderr);
                    peer->socket.close();
                }
            });
        }
    }

    // Shutdown: stop accepting, then close every peer so its thread can end.
    impl_->listener.close();
    for (const std::shared_ptr<Peer>& peer : impl_->worker_peers()) {
        peer->socket.shutdown_both();
        peer->socket.close();
    }
    {
        const std::lock_guard<std::mutex> guard(impl_->threads_mutex);
        for (std::thread& thread : impl_->threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        impl_->threads.clear();
    }
    if (!impl_->config.state_path.empty()) {
        static_cast<void>(save_state());
    }
    return Status{};
}

CoordinatorClient::~CoordinatorClient() { close(); }

Result<CoordinatorClient> CoordinatorClient::connect(std::uint16_t port, Limits limits,
                                                     const std::string& host) {
    static_cast<void>(network_runtime());
    Result<TcpSocket> socket = connect_loopback(port, host);
    if (!socket.ok()) {
        return socket.status();
    }
    CoordinatorClient client;
    client.socket_ = socket.take();
    client.limits_ = limits;
    return client;
}

Status CoordinatorClient::send_frame(MessageType type, const std::vector<std::byte>& payload) {
    return lab_scheduler::send_frame(socket_, type, next_correlation(), 0, payload);
}

Result<FrameView> CoordinatorClient::receive(std::vector<std::byte>& storage) {
    return receive_frame(socket_, limits_, storage);
}

Result<std::vector<std::byte>> CoordinatorClient::transact(MessageType request_type,
                                                           const std::vector<std::byte>& payload,
                                                           std::vector<std::byte>& storage) {
    Status status = send_frame(request_type, payload);
    if (!status.ok()) {
        return status;
    }
    Result<FrameView> frame = receive(storage);
    if (!frame.ok()) {
        return frame.status();
    }
    return std::vector<std::byte>(frame.value().payload.begin(), frame.value().payload.end());
}

Status CoordinatorClient::call_hello(const HelloMessage& message, HelloAckMessage& reply) {
    Result<std::vector<std::byte>> payload = encode_hello(message, limits_);
    if (!payload.ok()) {
        return payload.status();
    }
    Result<std::vector<std::byte>> response =
        transact(MessageType::Hello, payload.value(), storage_);
    if (!response.ok()) {
        return response.status();
    }
    Result<HelloAckMessage> decoded = decode_hello_ack(response.value(), limits_);
    if (!decoded.ok()) {
        return decoded.status();
    }
    reply = decoded.take();
    return Status{};
}

Status CoordinatorClient::call_register(const ResourceMessage& message, ResourceAckMessage& reply) {
    Result<std::vector<std::byte>> payload = encode_resource_message(message, limits_);
    if (!payload.ok()) {
        return payload.status();
    }
    Result<std::vector<std::byte>> response =
        transact(MessageType::RegisterResource, payload.value(), storage_);
    if (!response.ok()) {
        return response.status();
    }
    Result<ResourceAckMessage> decoded = decode_resource_ack(response.value(), limits_);
    if (!decoded.ok()) {
        return decoded.status();
    }
    reply = decoded.take();
    return Status{};
}

Status CoordinatorClient::call_publish(const ResourceMessage& message, ResourceAckMessage& reply) {
    Result<std::vector<std::byte>> payload = encode_resource_message(message, limits_);
    if (!payload.ok()) {
        return payload.status();
    }
    Result<std::vector<std::byte>> response =
        transact(MessageType::PublishState, payload.value(), storage_);
    if (!response.ok()) {
        return response.status();
    }
    Result<ResourceAckMessage> decoded = decode_resource_ack(response.value(), limits_);
    if (!decoded.ok()) {
        return decoded.status();
    }
    reply = decoded.take();
    return Status{};
}

Status CoordinatorClient::call_retire(const RetireMessage& message, ResourceAckMessage& reply) {
    Result<std::vector<std::byte>> payload = encode_retire(message, limits_);
    if (!payload.ok()) {
        return payload.status();
    }
    Result<std::vector<std::byte>> response =
        transact(MessageType::RetireResource, payload.value(), storage_);
    if (!response.ok()) {
        return response.status();
    }
    Result<ResourceAckMessage> decoded = decode_resource_ack(response.value(), limits_);
    if (!decoded.ok()) {
        return decoded.status();
    }
    reply = decoded.take();
    return Status{};
}

Status CoordinatorClient::call_schedule(const ScheduleMessage& message, DecisionMessage& reply) {
    Result<std::vector<std::byte>> payload = encode_schedule(message, limits_);
    if (!payload.ok()) {
        return payload.status();
    }
    Result<std::vector<std::byte>> response =
        transact(MessageType::ScheduleRequest, payload.value(), storage_);
    if (!response.ok()) {
        return response.status();
    }
    Result<DecisionMessage> decoded = decode_decision(response.value(), limits_);
    if (!decoded.ok()) {
        return decoded.status();
    }
    reply = decoded.take();
    return Status{};
}

Status CoordinatorClient::call_assign_ack(const AuthorityMessage& message, LifecycleAckMessage& reply) {
    Result<std::vector<std::byte>> payload = encode_authority(message, limits_);
    if (!payload.ok()) {
        return payload.status();
    }
    Result<std::vector<std::byte>> response =
        transact(MessageType::AssignAck, payload.value(), storage_);
    if (!response.ok()) {
        return response.status();
    }
    Result<LifecycleAckMessage> decoded = decode_lifecycle_ack(response.value(), limits_);
    if (!decoded.ok()) {
        return decoded.status();
    }
    reply = decoded.take();
    return Status{};
}

Status CoordinatorClient::call_running(const AuthorityMessage& message, LifecycleAckMessage& reply) {
    Result<std::vector<std::byte>> payload = encode_authority(message, limits_);
    if (!payload.ok()) {
        return payload.status();
    }
    Result<std::vector<std::byte>> response = transact(MessageType::Running, payload.value(), storage_);
    if (!response.ok()) {
        return response.status();
    }
    Result<LifecycleAckMessage> decoded = decode_lifecycle_ack(response.value(), limits_);
    if (!decoded.ok()) {
        return decoded.status();
    }
    reply = decoded.take();
    return Status{};
}

Status CoordinatorClient::call_completion(const CompletionClaim& claim, CompletionAckMessage& reply) {
    Result<std::vector<std::byte>> payload = encode_completion(claim, limits_);
    if (!payload.ok()) {
        return payload.status();
    }
    Result<std::vector<std::byte>> response =
        transact(MessageType::Completion, payload.value(), storage_);
    if (!response.ok()) {
        return response.status();
    }
    Result<CompletionAckMessage> decoded = decode_completion_ack(response.value(), limits_);
    if (!decoded.ok()) {
        return decoded.status();
    }
    reply = decoded.take();
    return Status{};
}

Status CoordinatorClient::call_cancel_placement(const CancelPlacementMessage& message,
                                                LifecycleAckMessage& reply) {
    Result<std::vector<std::byte>> payload = encode_cancel_placement(message, limits_);
    if (!payload.ok()) {
        return payload.status();
    }
    Result<std::vector<std::byte>> response =
        transact(MessageType::CancelPlacement, payload.value(), storage_);
    if (!response.ok()) {
        return response.status();
    }
    Result<LifecycleAckMessage> decoded = decode_lifecycle_ack(response.value(), limits_);
    if (!decoded.ok()) {
        return decoded.status();
    }
    reply = decoded.take();
    return Status{};
}

Status CoordinatorClient::call_cancel_request(const CancelRequestMessage& message,
                                              LifecycleAckMessage& reply) {
    Result<std::vector<std::byte>> payload = encode_cancel_request(message, limits_);
    if (!payload.ok()) {
        return payload.status();
    }
    Result<std::vector<std::byte>> response =
        transact(MessageType::CancelRequest, payload.value(), storage_);
    if (!response.ok()) {
        return response.status();
    }
    Result<LifecycleAckMessage> decoded = decode_lifecycle_ack(response.value(), limits_);
    if (!decoded.ok()) {
        return decoded.status();
    }
    reply = decoded.take();
    return Status{};
}

Status CoordinatorClient::call_reassign(const CancelPlacementMessage& message, LifecycleAckMessage& reply) {
    Result<std::vector<std::byte>> payload = encode_cancel_placement(message, limits_);
    if (!payload.ok()) {
        return payload.status();
    }
    Result<std::vector<std::byte>> response = transact(MessageType::Reassign, payload.value(), storage_);
    if (!response.ok()) {
        return response.status();
    }
    Result<LifecycleAckMessage> decoded = decode_lifecycle_ack(response.value(), limits_);
    if (!decoded.ok()) {
        return decoded.status();
    }
    reply = decoded.take();
    return Status{};
}

Status CoordinatorClient::call_preempt(const CancelPlacementMessage& message, LifecycleAckMessage& reply) {
    Result<std::vector<std::byte>> payload = encode_cancel_placement(message, limits_);
    if (!payload.ok()) {
        return payload.status();
    }
    Result<std::vector<std::byte>> response = transact(MessageType::Preempt, payload.value(), storage_);
    if (!response.ok()) {
        return response.status();
    }
    Result<LifecycleAckMessage> decoded = decode_lifecycle_ack(response.value(), limits_);
    if (!decoded.ok()) {
        return decoded.status();
    }
    reply = decoded.take();
    return Status{};
}

Status CoordinatorClient::call_disconnect_worker(const DisconnectWorkerMessage& message,
                                                 StateAckMessage& reply) {
    Result<std::vector<std::byte>> payload = encode_disconnect_worker(message, limits_);
    if (!payload.ok()) {
        return payload.status();
    }
    Result<std::vector<std::byte>> response =
        transact(MessageType::DisconnectWorker, payload.value(), storage_);
    if (!response.ok()) {
        return response.status();
    }
    Result<StateAckMessage> decoded = decode_state_ack(response.value(), limits_);
    if (!decoded.ok()) {
        return decoded.status();
    }
    reply = decoded.take();
    return Status{};
}

Status CoordinatorClient::call_inspect(const std::string& subject, std::string& text) {
    InspectMessage message;
    message.subject = subject;
    Result<std::vector<std::byte>> payload = encode_inspect(message, limits_);
    if (!payload.ok()) {
        return payload.status();
    }
    Result<std::vector<std::byte>> response = transact(MessageType::Inspect, payload.value(), storage_);
    if (!response.ok()) {
        return response.status();
    }
    Result<InspectReplyMessage> decoded = decode_inspect_reply(response.value(), limits_);
    if (!decoded.ok()) {
        return decoded.status();
    }
    text = decoded.value().text;
    return Status{};
}

Status CoordinatorClient::call_save(const std::string& path, StateAckMessage& reply) {
    SaveStateMessage message;
    message.path = path;
    Result<std::vector<std::byte>> payload = encode_save_state(message, limits_);
    if (!payload.ok()) {
        return payload.status();
    }
    Result<std::vector<std::byte>> response = transact(MessageType::SaveState, payload.value(), storage_);
    if (!response.ok()) {
        return response.status();
    }
    Result<StateAckMessage> decoded = decode_state_ack(response.value(), limits_);
    if (!decoded.ok()) {
        return decoded.status();
    }
    reply = decoded.take();
    return Status{};
}

Status CoordinatorClient::call_shutdown(StateAckMessage& reply) {
    const std::vector<std::byte> payload;
    Result<std::vector<std::byte>> response = transact(MessageType::Shutdown, payload, storage_);
    if (!response.ok()) {
        return response.status();
    }
    Result<StateAckMessage> decoded = decode_state_ack(response.value(), limits_);
    if (!decoded.ok()) {
        return decoded.status();
    }
    reply = decoded.take();
    return Status{};
}

WorkerAgent::~WorkerAgent() = default;

Result<HelloAckMessage> WorkerAgent::start(const WorkerConfig& config, WorkerTask task) {
    limits_ = config.limits;
    worker_ = config.worker;
    boot_ = config.boot;
    hold_assignments_ = config.hold_assignments;
    task_ = std::move(task);

    Result<CoordinatorClient> client = CoordinatorClient::connect(config.port, config.limits);
    if (!client.ok()) {
        return client.status();
    }
    client_ = client.take();

    HelloMessage hello;
    hello.worker = config.worker;
    hello.boot = config.boot;
    hello.authority = config.authority;
    HelloAckMessage ack;
    Status status = client_.call_hello(hello, ack);
    if (!status.ok()) {
        return status;
    }

    Result<ProfileBlueprint> blueprint = make_profile(config.profile, config.boot, config.current_occupancy);
    if (!blueprint.ok()) {
        return blueprint.status();
    }
    for (const ResourceAdvertisement& advertisement : blueprint.value().resources) {
        ResourceMessage message;
        message.advertisement = advertisement;
        ResourceAckMessage resource_ack;
        status = client_.call_register(message, resource_ack);
        if (!status.ok()) {
            return status;
        }
        if (resource_ack.code == ErrorCode::DuplicateIdentity) {
            // The logical resource already exists in this coordinator: the new
            // incarnation publishes current state instead of re-registering the
            // identity, and the coordinator revokes whatever authority the
            // previous incarnation still held.
            status = client_.call_publish(message, resource_ack);
            if (!status.ok()) {
                return status;
            }
        }
        if (resource_ack.code != ErrorCode::Ok) {
            return Status(resource_ack.code, "resource advertisement rejected: " + resource_ack.detail);
        }
        resource_ids_.push_back(advertisement.id);
    }
    return ack;
}

Status WorkerAgent::handle_assignment(const AssignMessage& message) {
    AuthorityMessage authority;
    authority.placement = message.authority.placement;
    authority.generation = message.authority.placement_generation;
    authority.epoch = message.authority.epoch;
    authority.worker_boot = message.authority.worker_boot;
    LifecycleAckMessage reply;
    Status status = client_.call_assign_ack(authority, reply);
    if (!status.ok()) {
        return status;
    }
    if (reply.code != ErrorCode::Ok) {
        ++rejected_assignments_;
        return Status(reply.code, "assignment rejected: " + reply.detail);
    }
    status = client_.call_running(authority, reply);
    if (!status.ok()) {
        return status;
    }

    if (hold_assignments_) {
        return Status{};
    }

    std::string detail;
    Status executed{};
    if (task_) {
        executed = task_(message.authority, detail);
    } else {
        detail = synthetic_workload_result(message.authority, message.workload);
    }

    CompletionClaim claim;
    claim.placement = message.authority.placement;
    claim.placement_generation = message.authority.placement_generation;
    claim.experiment = message.authority.experiment;
    claim.experiment_generation = message.authority.experiment_generation;
    claim.epoch = message.authority.epoch;
    claim.worker_boot = boot_;
    for (const SelectedResource& selected : message.authority.resources) {
        claim.resources.push_back(selected.resource);
        claim.resource_generations.push_back(selected.generation);
    }
    claim.succeeded = executed.ok();
    claim.detail = detail;
    CompletionAckMessage completion;
    status = client_.call_completion(claim, completion);
    if (!status.ok()) {
        return status;
    }
    if (completion.accepted) {
        ++completed_assignments_;
    } else {
        ++rejected_assignments_;
    }
    return Status{};
}

Status WorkerAgent::run() {
    while (!exit_requested_) {
        Result<FrameView> frame = receive_frame(client_.socket(), limits_, storage_);
        if (!frame.ok()) {
            return frame.status();
        }
        const FrameView view = frame.value();
        switch (view.type) {
            case MessageType::Assign: {
                Result<AssignMessage> message = decode_assign(view.payload, limits_);
                if (!message.ok()) {
                    return message.status();
                }
                static_cast<void>(handle_assignment(message.value()));
                break;
            }
            case MessageType::Shutdown:
                exit_requested_ = true;
                break;
            case MessageType::Error: {
                Result<ErrorMessage> error = decode_error_message(view.payload, limits_);
                if (error.ok()) {
                    return Status(error.value().code, error.value().message);
                }
                return Status(ErrorCode::ProtocolError, "coordinator reported an undecodable error");
            }
            default:
                break;
        }
    }
    return Status{};
}

}  // namespace lab_scheduler
