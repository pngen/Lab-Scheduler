#include <atomic>
#include <thread>
#include <vector>

#include "lab_scheduler/engine.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

using namespace lab_scheduler;

namespace {

// Concurrent phases are launched together and joined: no sleeps, no timeouts.
struct ThreadGroup {
    std::vector<std::thread> threads{};

    void add(void (*function)(SchedulerEngine&, std::uint32_t), SchedulerEngine& engine,
             std::uint32_t index) {
        threads.emplace_back(function, std::ref(engine), index);
    }

    void join() {
        for (std::thread& thread : threads) {
            thread.join();
        }
        threads.clear();
    }
};

void register_worker_resources(SchedulerEngine& engine, std::uint32_t index) {
    const std::uint64_t worker = 0x100 + index;
    const std::uint64_t boot = 0x200 + index;
    const std::string host = "host-" + decimal_u64(index);
    for (std::uint32_t resource = 0; resource < 4; ++resource) {
        const std::uint64_t id = 0x5000 + index * 16 + resource;
        if (resource % 2 == 0) {
            static_cast<void>(engine.register_resource(test::gpu_resource(id, worker, boot, host, 4)));
        } else {
            static_cast<void>(
                engine.register_resource(test::model_resource(id, worker, boot, host, 0x1f01, "1.0", true)));
        }
    }
}

void submit_requests(SchedulerEngine& engine, std::uint32_t index) {
    for (std::uint32_t attempt = 0; attempt < 8; ++attempt) {
        ResourceRequirement gpu = test::requirement_of(ResourceClass::Gpu, "gpu");
        const std::uint64_t request_id = 0x7000 + index * 16 + attempt;
        const std::uint64_t experiment_id = 0x8000 + index * 16 + attempt;
        static_cast<void>(engine.submit_request(test::request_of(request_id, experiment_id, {gpu})));
    }
}

void publish_state(SchedulerEngine& engine, std::uint32_t index) {
    const std::uint64_t worker = 0x100 + index;
    const std::uint64_t boot = 0x200 + index;
    const std::string host = "host-" + decimal_u64(index);
    for (std::uint32_t round = 0; round < 4; ++round) {
        for (std::uint32_t resource = 0; resource < 4; ++resource) {
            const std::uint64_t id = 0x5000 + index * 16 + resource;
            ResourceAdvertisement advertisement =
                resource % 2 == 0 ? test::gpu_resource(id, worker, boot, host, 4)
                                  : test::model_resource(id, worker, boot, host, 0x1f01, "1.0", true);
            advertisement.current_occupancy = round % 2;
            static_cast<void>(engine.publish_resource_state(advertisement));
        }
    }
}

void inspect_and_validate(SchedulerEngine& engine, std::uint32_t index) {
    static_cast<void>(index);
    for (std::uint32_t round = 0; round < 16; ++round) {
        const SchedulerSnapshot snapshot = engine.snapshot();
        static_cast<void>(snapshot.resources.size());
        static_cast<void>(engine.accounting());
        static_cast<void>(engine.list_placements());
        static_cast<void>(engine.list_stale_resources());
    }
}

}  // namespace

LS_TEST(concurrent_registration_never_duplicates_a_logical_resource) {
    SchedulerEngine engine;
    std::atomic<int> successes{0};
    std::vector<std::thread> threads;
    for (int thread_index = 0; thread_index < 8; ++thread_index) {
        threads.emplace_back([&engine, &successes]() {
            for (int attempt = 0; attempt < 16; ++attempt) {
                const std::uint64_t id = 0x6000 + static_cast<std::uint64_t>(attempt % 4);
                const Result<ResourceRecord> result = engine.register_resource(
                    test::gpu_resource(id, 0x300 + static_cast<std::uint64_t>(attempt % 4),
                                       0x400 + static_cast<std::uint64_t>(attempt % 4), "host-contended", 4));
                if (result.ok()) {
                    successes.fetch_add(1);
                } else {
                    LS_CHECK_EQ(result.code(), ErrorCode::DuplicateIdentity);
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    LS_CHECK_EQ(successes.load(), 4);
    LS_CHECK_EQ(engine.list_resources().size(), std::size_t{4});
    LS_CHECK(engine.validate_invariants().all_ok());
}

LS_TEST(concurrent_phases_keep_the_engine_coherent) {
    SchedulerEngine engine;
    const std::uint64_t workers = 6;
    for (std::uint64_t worker = 0; worker < workers; ++worker) {
        register_worker_resources(engine, static_cast<std::uint32_t>(worker));
    }
    ThreadGroup group;
    for (std::uint32_t index = 0; index < 4; ++index) {
        group.add(publish_state, engine, index);
        group.add(submit_requests, engine, index + 1);
        group.add(inspect_and_validate, engine, index);
        group.add(register_worker_resources, engine, static_cast<std::uint32_t>(workers - 1));
    }
    group.join();

    LS_CHECK(engine.validate_invariants().all_ok());
    const AccountingReport accounting = engine.accounting();
    LS_CHECK(accounting.total_reserved_slots <= accounting.total_capacity_slots);
    for (const ResourceAccounting& resource : accounting.resources) {
        LS_CHECK(resource.reserved_slots <= resource.capacity_slots);
    }
}

LS_TEST(completion_and_cancellation_race_has_exactly_one_winner) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        SchedulerEngine engine;
        LS_CHECK(engine.register_resource(test::gpu_resource(0x9101, 0x91, 0x92, "host-race", 4)).ok());
        ResourceRequirement gpu = test::requirement_of(ResourceClass::Gpu, "gpu");
        const Result<Decision> decision =
            engine.submit_request(test::request_of(0x9200 + static_cast<std::uint64_t>(attempt),
                                                   0x9300 + static_cast<std::uint64_t>(attempt), {gpu}));
        LS_CHECK(decision.ok());
        LS_CHECK_EQ(decision.value().outcome, DecisionOutcome::Placed);

        CompletionClaim claim;
        claim.placement = decision.value().placement.id;
        claim.placement_generation = decision.value().placement.generation;
        claim.experiment = decision.value().placement.experiment;
        claim.experiment_generation = decision.value().placement.experiment_generation;
        claim.epoch = engine.epoch();
        claim.worker_boot = decision.value().placement.worker_boot;
        for (const SelectedResource& selected : decision.value().placement.resources) {
            claim.resources.push_back(selected.resource);
            claim.resource_generations.push_back(selected.generation);
        }

        std::atomic<int> accepted{0};
        std::atomic<int> released{0};
        std::vector<std::thread> threads;
        threads.emplace_back([&engine, &claim, &accepted, &released]() {
            const CompletionOutcome outcome = engine.report_completion(claim);
            if (outcome.accepted) {
                accepted.fetch_add(1);
            }
            released.fetch_add(static_cast<int>(outcome.released_reservations.size()));
        });
        threads.emplace_back([&engine, &decision, &released]() {
            const CancellationReport report = engine.cancel_placement(
                decision.value().placement.id, decision.value().placement.generation, engine.epoch(), "race");
            released.fetch_add(static_cast<int>(report.released_reservations.size()));
        });
        for (std::thread& thread : threads) {
            thread.join();
        }

        LS_CHECK_EQ(released.load(), 1);
        LS_CHECK(accepted.load() <= 1);
        LS_CHECK_EQ(engine.accounting().total_reserved_slots, std::uint64_t{0});
        LS_CHECK(engine.validate_invariants().all_ok());
    }
}

LS_TEST(resource_loss_and_completion_race_stays_consistent) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        SchedulerEngine engine;
        LS_CHECK(engine.register_resource(test::gpu_resource(0x9501, 0x95, 0x96, "host-loss", 4)).ok());
        ResourceRequirement gpu = test::requirement_of(ResourceClass::Gpu, "gpu");
        const Result<Decision> decision =
            engine.submit_request(test::request_of(0x9600 + static_cast<std::uint64_t>(attempt),
                                                   0x9700 + static_cast<std::uint64_t>(attempt), {gpu}));
        LS_CHECK(decision.ok());
        CompletionClaim claim;
        claim.placement = decision.value().placement.id;
        claim.placement_generation = decision.value().placement.generation;
        claim.experiment = decision.value().placement.experiment;
        claim.experiment_generation = decision.value().placement.experiment_generation;
        claim.epoch = engine.epoch();
        claim.worker_boot = decision.value().placement.worker_boot;
        for (const SelectedResource& selected : decision.value().placement.resources) {
            claim.resources.push_back(selected.resource);
            claim.resource_generations.push_back(selected.generation);
        }

        std::atomic<int> releases{0};
        std::vector<std::thread> threads;
        threads.emplace_back([&engine, &claim, &releases]() {
            const CompletionOutcome outcome = engine.report_completion(claim);
            releases.fetch_add(static_cast<int>(outcome.released_reservations.size()));
        });
        threads.emplace_back([&engine, &releases]() {
            const ResourceLossReport loss = engine.mark_worker_lost(WorkerId::from_value(0x95),
                                                                    WorkerBootId::from_value(0x96),
                                                                    "race loss");
            releases.fetch_add(static_cast<int>(loss.released_reservations.size()));
        });
        for (std::thread& thread : threads) {
            thread.join();
        }
        LS_CHECK_EQ(releases.load(), 1);
        LS_CHECK_EQ(engine.accounting().total_reserved_slots, std::uint64_t{0});
        LS_CHECK(engine.validate_invariants().all_ok());
    }
}

LS_TEST(shutdown_does_not_strand_authority) {
    SchedulerEngine engine;
    LS_CHECK(engine.register_resource(test::gpu_resource(0x9901, 0x99, 0x9a, "host-shutdown", 2)).ok());
    ResourceRequirement gpu = test::requirement_of(ResourceClass::Gpu, "gpu");
    LS_CHECK(engine.submit_request(test::request_of(0x9b01, 0x9c01, {gpu})).ok());
    LS_CHECK(engine.begin_shutdown().ok());
    LS_CHECK_EQ(engine.begin_shutdown().code, ErrorCode::ShuttingDown);
    LS_CHECK(!engine.submit_request(test::request_of(0x9b02, 0x9c02, {gpu})).ok());
    LS_CHECK_EQ(engine.submit_request(test::request_of(0x9b03, 0x9c03, {gpu})).code(),
                ErrorCode::ShuttingDown);
    LS_CHECK(engine.validate_invariants().all_ok());
    LS_CHECK(engine.shutting_down());
}
