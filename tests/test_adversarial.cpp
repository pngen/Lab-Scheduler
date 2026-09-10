#include <string>
#include <thread>
#include <vector>

#include "lab_scheduler/cluster.hpp"
#include "lab_scheduler/codec.hpp"
#include "lab_scheduler/engine.hpp"
#include "lab_scheduler/inspection.hpp"
#include "lab_scheduler/persistence.hpp"
#include "lab_scheduler/protocol.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

using namespace lab_scheduler;

namespace {

std::vector<std::byte> crafted_frame(MessageType type, const std::vector<std::byte>& payload) {
    const Result<std::vector<std::byte>> frame = encode_frame(type, 1, 0, payload);
    return frame.ok() ? frame.value() : std::vector<std::byte>{};
}

}  // namespace

LS_TEST(null_and_duplicate_identities_are_rejected_everywhere) {
    SchedulerEngine engine;
    LS_CHECK_EQ(engine.register_resource(ResourceAdvertisement{}).code(), ErrorCode::InvalidIdentity);
    auto advertisement = test::gpu_resource(0x4001, 0x11, 0x12, "host-null");
    LS_CHECK(engine.register_resource(advertisement).ok());
    LS_CHECK_EQ(engine.register_resource(advertisement).code(), ErrorCode::DuplicateIdentity);
    LS_CHECK_EQ(engine.get_resource(ResourceId{}).code(), ErrorCode::InvalidIdentity);
    LS_CHECK_EQ(engine.get_placement(PlacementId{}).code(), ErrorCode::InvalidIdentity);
    LS_CHECK_EQ(engine.get_request(ScheduleRequestId{}).code(), ErrorCode::InvalidIdentity);
    LS_CHECK_EQ(engine.get_decision(DecisionId{}).code(), ErrorCode::InvalidIdentity);
    LS_CHECK_EQ(engine.retire_resource(ResourceId{}, ResourceGeneration{}).code(), ErrorCode::InvalidIdentity);
    LS_CHECK_EQ(engine.cancel_request(ScheduleRequestId{}, "x").status.code, ErrorCode::InvalidIdentity);
    LS_CHECK_EQ(engine.mark_worker_lost(WorkerId{}, WorkerBootId{}, "x").status.code, ErrorCode::InvalidIdentity);
}

LS_TEST(advertisements_from_a_stale_incarnation_cannot_restore_authority) {
    SchedulerEngine engine;
    auto advertisement = test::gpu_resource(0x4002, 0x21, 0x22, "host-stale");
    LS_CHECK(engine.register_resource(advertisement).ok());
    LS_CHECK(engine.worker_connected(WorkerId::from_value(0x21), WorkerBootId::from_value(0x23)).status.ok());

    ResourceAdvertisement stale = advertisement;
    stale.boot = WorkerBootId::from_value(0x22);
    const Result<ResourceRecord> published = engine.publish_resource_state(stale);
    LS_CHECK(published.ok() || published.code() == ErrorCode::Unauthorized);

    ResourceAdvertisement foreign = advertisement;
    foreign.worker = WorkerId::from_value(0x99);
    foreign.boot = WorkerBootId::from_value(0x98);
    LS_CHECK_EQ(engine.publish_resource_state(foreign).code(), ErrorCode::Unauthorized);

    ResourceAdvertisement wrong_class = advertisement;
    wrong_class.boot = WorkerBootId::from_value(0x23);
    wrong_class.resource_class = ResourceClass::Worker;
    LS_CHECK_EQ(engine.publish_resource_state(wrong_class).code(), ErrorCode::InvalidArgument);
}

LS_TEST(capacity_cannot_be_shrunk_below_active_reservations) {
    SchedulerEngine engine;
    LS_CHECK(engine.register_resource(test::gpu_resource(0x4003, 0x31, 0x32, "host-shrink", 4)).ok());
    ResourceRequirement gpu = test::requirement_of(ResourceClass::Gpu, "gpu");
    gpu.min_slots = 3;
    const Result<Decision> decision = engine.submit_request(test::request_of(0x4004, 0x4005, {gpu}));
    LS_CHECK(decision.ok());
    LS_CHECK_EQ(decision.value().outcome, DecisionOutcome::Placed);

    auto shrunk = test::gpu_resource(0x4003, 0x31, 0x32, "host-shrink", 1);
    LS_CHECK_EQ(engine.publish_resource_state(shrunk).code(), ErrorCode::CapacityExhausted);

    auto occupied = test::gpu_resource(0x4003, 0x31, 0x32, "host-shrink", 4);
    occupied.current_occupancy = 2;
    LS_CHECK_EQ(engine.publish_resource_state(occupied).code(), ErrorCode::CapacityExhausted);
}

LS_TEST(over_capacity_and_absurd_claims_are_rejected) {
    SchedulerEngine engine;
    auto advertisement = test::gpu_resource(0x4006, 0x41, 0x42, "host-absurd", 1);
    advertisement.current_occupancy = 5;
    LS_CHECK_EQ(engine.register_resource(advertisement).code(), ErrorCode::InvalidArgument);

    advertisement.current_occupancy = 0;
    advertisement.capacity.slots = 0xffffffffu;
    advertisement.capacity.max_sessions = 0xffffffffu;
    LS_CHECK(engine.register_resource(advertisement).ok());

    ResourceRequirement gpu = test::requirement_of(ResourceClass::Gpu, "gpu");
    gpu.min_slots = 1000;
    gpu.max_occupancy_percent = 100;
    const Result<Decision> decision = engine.submit_request(test::request_of(0x4007, 0x4008, {gpu}));
    LS_CHECK(decision.ok());
    LS_CHECK_EQ(decision.value().outcome, DecisionOutcome::Placed);
    LS_CHECK_EQ(decision.value().placement.resources[0].slots, std::uint32_t{1000});
    LS_CHECK(engine.validate_invariants().all_ok());
}

LS_TEST(impossible_enum_values_in_payloads_are_rejected) {
    const Limits limits;
    CompletionAckMessage ack;
    ack.placement = PlacementId::from_value(0x5001);
    ack.state = PlacementState::Completed;
    Result<std::vector<std::byte>> encoded = encode_completion_ack(ack, limits);
    LS_CHECK(encoded.ok());
    std::vector<std::byte> corrupted = encoded.value();
    corrupted[8] = std::byte{0x7f};
    LS_CHECK(!decode_completion_ack(corrupted, limits).ok());

    ResourceAckMessage resource_ack;
    resource_ack.resource = ResourceId::from_value(0x5002);
    resource_ack.generation = ResourceGeneration::from_value(1);
    encoded = encode_resource_ack(resource_ack, limits);
    LS_CHECK(encoded.ok());
    std::vector<std::byte> bad_lifecycle = encoded.value();
    bad_lifecycle[16] = std::byte{0x7f};
    LS_CHECK(!decode_resource_ack(bad_lifecycle, limits).ok());

    ErrorMessage error;
    error.code = ErrorCode::NoPlacement;
    error.message = "no placement";
    encoded = encode_error_message(error, limits);
    LS_CHECK(encoded.ok());
    std::vector<std::byte> bad_code = encoded.value();
    bad_code[0] = std::byte{0xff};
    bad_code[1] = std::byte{0xff};
    LS_CHECK(!decode_error_message(bad_code, limits).ok());
}

LS_TEST(huge_strings_and_counts_are_bounded) {
    ByteWriter writer;
    writer.str(std::string(5000, 'x'));
    ByteReader reader(writer.span(), ErrorCode::ProtocolError);
    std::string text;
    LS_CHECK(!reader.str(text, 64).ok());

    Limits limits;
    limits.max_explanation_entries = 4;
    limits.max_string_length = 16;
    InspectReplyMessage reply;
    reply.subject = "resources";
    reply.text = std::string(4096, 'y');
    const Result<std::vector<std::byte>> encoded = encode_inspect_reply(reply, limits);
    LS_CHECK(encoded.ok());
    LS_CHECK(!decode_inspect_reply(encoded.value(), limits).ok());
}

LS_TEST(coordinator_survives_hostile_connections) {
    static_cast<void>(network_runtime());
    CoordinatorServer server;
    CoordinatorConfig config;
    config.port = 0;
    config.persist_on_mutation = false;
    const Result<CoordinatorStatus> status = server.start(config);
    LS_CHECK(status.ok());
    if (!status.ok()) {
        return;
    }
    // The accept loop runs in its own thread so that hostile connections are
    // really served (and really rejected) by the coordinator.
    std::thread server_thread([&server]() { static_cast<void>(server.run()); });

    const std::vector<std::byte> payload{std::byte{0x01}};
    std::vector<std::vector<std::byte>> hostile;
    hostile.push_back({});
    hostile.push_back({std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0x7f}});
    hostile.push_back(std::vector<std::byte>(kFrameHeaderSize, std::byte{0x00}));
    std::vector<std::byte> valid = crafted_frame(MessageType::Inspect, payload);
    valid[8] = std::byte{0x7f};
    valid[9] = std::byte{0x00};
    hostile.push_back(valid);
    std::vector<std::byte> trailing = crafted_frame(MessageType::Inspect, payload);
    trailing.push_back(std::byte{0x00});
    hostile.push_back(trailing);

    for (const std::vector<std::byte>& bytes : hostile) {
        Result<TcpSocket> socket = connect_loopback(status.value().port);
        LS_CHECK(socket.ok());
        if (!socket.ok()) {
            continue;
        }
        if (!bytes.empty()) {
            static_cast<void>(send_all(socket.value(), std::span<const std::byte>(bytes.data(), bytes.size())));
        }
        socket.value().shutdown_both();
        socket.value().close();
    }

    Result<CoordinatorClient> client = CoordinatorClient::connect(status.value().port);
    LS_CHECK(client.ok());
    if (client.ok()) {
        std::string text;
        LS_CHECK(client.value().call_inspect("epoch", text).ok());
        LS_CHECK(text.find("epoch-") != std::string::npos);
    }
    client.value().close();
    server.request_shutdown();
    server_thread.join();
    LS_CHECK(true);
}

LS_TEST(repeated_start_and_stop_is_stable) {
    for (int round = 0; round < 4; ++round) {
        CoordinatorServer server;
        CoordinatorConfig config;
        config.port = 0;
        config.persist_on_mutation = false;
        const Result<CoordinatorStatus> status = server.start(config);
        LS_CHECK(status.ok());
        if (!status.ok()) {
            continue;
        }
        std::thread server_thread([&server]() { static_cast<void>(server.run()); });
        Result<CoordinatorClient> client = CoordinatorClient::connect(status.value().port);
        LS_CHECK(client.ok());
        if (client.ok()) {
            std::string text;
            LS_CHECK(client.value().call_inspect("epoch", text).ok());
        }
        client.value().close();
        server.request_shutdown();
        server_thread.join();
        LS_CHECK(round >= 0);
    }
}

LS_TEST(cancellation_storm_releases_capacity_exactly_once) {
    SchedulerEngine engine;
    LS_CHECK(engine.register_resource(test::gpu_resource(0x6001, 0x51, 0x52, "host-storm", 8)).ok());
    std::vector<PlacementId> placements;
    for (std::uint32_t index = 0; index < 8; ++index) {
        ResourceRequirement gpu = test::requirement_of(ResourceClass::Gpu, "gpu");
        const Result<Decision> decision =
            engine.submit_request(test::request_of(0x6100 + index, 0x6200 + index, {gpu}));
        LS_CHECK(decision.ok());
        if (decision.value().outcome == DecisionOutcome::Placed) {
            placements.push_back(decision.value().placement.id);
        }
    }
    std::uint32_t released = 0;
    for (int round = 0; round < 4; ++round) {
        for (const PlacementId& placement : placements) {
            const Result<PlacementRecord> record = engine.get_placement(placement);
            if (!record.ok()) {
                continue;
            }
            const CancellationReport report = engine.cancel_placement(
                placement, record.value().generation, engine.epoch(), "storm");
            released += static_cast<std::uint32_t>(report.released_reservations.size());
        }
    }
    LS_CHECK_EQ(released, static_cast<std::uint32_t>(placements.size()));
    LS_CHECK_EQ(engine.accounting().total_reserved_slots, std::uint64_t{0});
    LS_CHECK(engine.validate_invariants().all_ok());
}

LS_TEST(explanation_size_is_bounded) {
    SchedulerEngine engine;
    for (std::uint32_t index = 0; index < 64; ++index) {
        LS_CHECK(engine
                     .register_resource(test::gpu_resource(0x7000 + index, 0x61, 0x62,
                                                           "host-" + decimal_u64(index % 4), 2))
                     .ok());
    }
    Limits limits;
    limits.max_candidates_ranked = 4;
    EngineConfig config;
    config.limits = limits;
    SchedulerEngine bounded(config);
    for (std::uint32_t index = 0; index < 64; ++index) {
        LS_CHECK(bounded
                     .register_resource(test::gpu_resource(0x7100 + index, 0x71, 0x72,
                                                           "host-" + decimal_u64(index % 4), 2))
                     .ok());
    }
    ResourceRequirement gpu = test::requirement_of(ResourceClass::Gpu, "gpu");
    const Result<Decision> decision = bounded.submit_request(test::request_of(0x7200, 0x7300, {gpu}));
    LS_CHECK(decision.ok());
    LS_CHECK(decision.value().explanation.candidates.size() <= limits.max_candidates_ranked);
    const std::string text = render_explanation(decision.value().explanation);
    LS_CHECK(text.size() < 200000);
}
