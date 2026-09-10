#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "lab_scheduler/engine.hpp"
#include "lab_scheduler/inspection.hpp"
#include "lab_scheduler/persistence.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

using namespace lab_scheduler;

namespace {

struct RandomLab {
    SchedulerEngine engine{};
    test::Rng rng;
    std::vector<ResourceAdvertisement> advertisements{};
    std::vector<WorkerBootId> boots{};

    RandomLab(std::uint64_t seed, std::uint32_t worker_count, std::uint32_t resources_per_worker)
        : rng(seed) {
        for (std::uint32_t worker = 0; worker < worker_count; ++worker) {
            const std::uint64_t worker_id = 0x1000 + worker;
            const WorkerBootId boot = WorkerBootId::from_value(0x2000 + worker);
            boots.push_back(boot);
            const std::string host = "prop-host-" + decimal_u64(worker);
            for (std::uint32_t resource = 0; resource < resources_per_worker; ++resource) {
                const std::uint64_t id = 0x3000 + worker * 32 + resource;
                ResourceAdvertisement advertisement;
                switch (resource % 4) {
                    case 0:
                        advertisement = test::gpu_resource(id, worker_id, boot.value(), host,
                                                           1 + rng.u32(4), rng.chance(1, 4));
                        break;
                    case 1:
                        advertisement = test::model_resource(id, worker_id, boot.value(), host,
                                                             0x1f01 + rng.range(2),
                                                             rng.chance(1, 2) ? "1.0" : "2.0",
                                                             rng.chance(3, 4));
                        break;
                    case 2:
                        advertisement = test::dataset_resource(id, worker_id, boot.value(), host, 0x1f05,
                                                               1 + rng.range(2));
                        break;
                    default:
                        advertisement = test::simulator_resource(id, worker_id, boot.value(), host, 0x1f06,
                                                                 "3.0");
                        break;
                }
                advertisement.health = rng.chance(6, 7) ? HealthState::Healthy : HealthState::Degraded;
                advertisement.current_occupancy = rng.u32(advertisement.capacity.slots);
                advertisements.push_back(advertisement);
            }
        }
    }

    void register_all_in_random_order() {
        std::vector<ResourceAdvertisement> shuffled = advertisements;
        for (std::size_t i = shuffled.size(); i > 1; --i) {
            std::swap(shuffled[i - 1], shuffled[rng.range(i)]);
        }
        for (const ResourceAdvertisement& advertisement : shuffled) {
            const Result<ResourceRecord> result = engine.register_resource(advertisement);
            LS_CHECK(result.ok() || result.code() == ErrorCode::DuplicateIdentity);
        }
    }

    ScheduleRequest random_request(std::uint64_t id) {
        ScheduleRequest request = test::request_of(id, 0x7000 + (id % 64), {});
        const std::uint32_t requirement_count = 1 + rng.u32(3);
        for (std::uint32_t index = 0; index < requirement_count; ++index) {
            const std::uint32_t kind = rng.u32(4);
            ResourceRequirement requirement;
            switch (kind) {
                case 0:
                    requirement = test::requirement_of(ResourceClass::Gpu, "gpu" + decimal_u64(index));
                    if (rng.chance(1, 3)) {
                        requirement.required_capabilities.add("cuda", Limits{});
                    }
                    if (rng.chance(1, 4)) {
                        requirement.require_exclusive = true;
                    }
                    break;
                case 1:
                    requirement = test::requirement_of(ResourceClass::Model, "model" + decimal_u64(index));
                    if (rng.chance(1, 2)) {
                        requirement.model = ModelResourceId::from_value(0x1f01 + rng.range(2));
                    }
                    if (rng.chance(1, 4)) {
                        requirement.model_version = "1.0";
                    }
                    break;
                case 2:
                    requirement = test::requirement_of(ResourceClass::Dataset, "dataset" + decimal_u64(index));
                    requirement.dataset = DatasetId::from_value(0x1f05);
                    if (rng.chance(1, 2)) {
                        requirement.dataset_version = DatasetVersionId::from_value(1 + rng.range(2));
                    }
                    break;
                default:
                    requirement = test::requirement_of(ResourceClass::Simulator, "sim" + decimal_u64(index));
                    requirement.simulator = SimulatorId::from_value(0x1f06);
                    break;
            }
            requirement.max_occupancy_percent = 50 + rng.u32(51);
            if (rng.chance(1, 5)) {
                requirement.require_current_evidence = false;
            }
            request.requirements.push_back(requirement);
        }
        request.experiment_anti_affinity = rng.chance(1, 6);
        return request;
    }

    void assert_core_invariants(const char* context) {
        const InvariantReport report = engine.validate_invariants();
        LS_CHECK_MESSAGE(report.all_ok(), std::string(context) + " :: " + render_invariants(report));
        const AccountingReport accounting = engine.accounting();
        for (const ResourceAccounting& resource : accounting.resources) {
            std::uint64_t used = static_cast<std::uint64_t>(resource.advertised_occupancy) +
                                 resource.reserved_slots;
            LS_CHECK_MESSAGE(used <= resource.capacity_slots, context);
        }
    }
};

}  // namespace

LS_TEST(randomized_scheduling_preserves_invariants) {
    for (std::uint64_t seed = 1; seed <= 6; ++seed) {
        RandomLab lab(seed, 4, 6);
        lab.register_all_in_random_order();
        const CoordinatorEpoch initial_epoch = lab.engine.epoch();
        for (std::uint32_t step = 0; step < 24; ++step) {
            const ScheduleRequest request = lab.random_request(0x4000 + seed * 64 + step);
            const Result<Decision> decision = lab.engine.submit_request(request);
            LS_CHECK(decision.ok() || decision.code() == ErrorCode::Busy);
            if (!decision.ok()) {
                continue;
            }
            LS_CHECK(decision.value().outcome == DecisionOutcome::Placed ||
                     decision.value().outcome == DecisionOutcome::NoPlacement);
            if (decision.value().outcome == DecisionOutcome::Placed) {
                const PlacementRecord& placement = decision.value().placement;
                for (const SelectedResource& selected : placement.resources) {
                    const Result<ResourceRecord> resource = lab.engine.get_resource(selected.resource);
                    LS_CHECK_MESSAGE(resource.ok(), "selected resource must exist");
                    if (resource.ok()) {
                        LS_CHECK_MESSAGE(!resource.value().id.is_null(), "selected identity must be valid");
                    }
                }
                if (step % 3 == 0) {
                    static_cast<void>(lab.engine.cancel_placement(placement.id, placement.generation,
                                                                  lab.engine.epoch(), "property cancel"));
                }
            }
            if (step % 7 == 5) {
                const std::uint32_t worker = lab.rng.u32(4);
                static_cast<void>(lab.engine.mark_worker_lost(
                    WorkerId::from_value(0x1000 + worker), lab.boots[worker], "property loss"));
            }
            if (step % 11 == 9) {
                const std::uint32_t worker = lab.rng.u32(4);
                const WorkerBootId fresh = WorkerBootId::from_value(0x9000 + step);
                lab.boots[worker] = fresh;
                static_cast<void>(
                    lab.engine.worker_connected(WorkerId::from_value(0x1000 + worker), fresh));
                for (ResourceAdvertisement advertisement : lab.advertisements) {
                    if (advertisement.worker.value() == 0x1000 + worker) {
                        advertisement.boot = fresh;
                        static_cast<void>(lab.engine.publish_resource_state(advertisement));
                    }
                }
            }
            LS_CHECK(lab.engine.epoch().value() >= initial_epoch.value());
            lab.assert_core_invariants("randomized step");
        }
        lab.assert_core_invariants("randomized scenario end");
    }
}

LS_TEST(ranking_is_insertion_order_independent) {
    std::vector<ResourceAdvertisement> pool;
    for (std::uint64_t id = 0; id < 8; ++id) {
        pool.push_back(test::gpu_resource(0xa000 + id, 0xb0 + (id % 2), 0xc0 + (id % 2),
                                          "host-" + decimal_u64(id % 2), 4));
    }
    std::vector<ResourceId> reference;
    for (std::uint64_t seed = 1; seed <= 8; ++seed) {
        SchedulerEngine engine;
        std::vector<ResourceAdvertisement> order = pool;
        test::Rng rng(seed);
        for (std::size_t i = order.size(); i > 1; --i) {
            std::swap(order[i - 1], order[rng.range(i)]);
        }
        for (const ResourceAdvertisement& advertisement : order) {
            LS_CHECK(engine.register_resource(advertisement).ok());
        }
        ResourceRequirement gpu = test::requirement_of(ResourceClass::Gpu, "gpu");
        const Result<Decision> decision = engine.submit_request(test::request_of(0xd000 + seed, 0xe000, {gpu}));
        LS_CHECK(decision.ok());
        LS_CHECK_EQ(decision.value().outcome, DecisionOutcome::Placed);
        std::vector<ResourceId> selected;
        for (const SelectedResource& entry : decision.value().placement.resources) {
            selected.push_back(entry.resource);
        }
        if (reference.empty()) {
            reference = selected;
        } else {
            LS_CHECK_EQ(selected, reference);
        }
    }
}

LS_TEST(exclusive_resources_are_never_double_booked_under_random_load) {
    test::Rng rng(0xabcdu);
    for (int round = 0; round < 24; ++round) {
        SchedulerEngine engine;
        const std::uint32_t slots = 1 + rng.u32(3);
        LS_CHECK(engine.register_resource(test::gpu_resource(0xf001, 0xf1, 0xf2, "host-exclusive", slots, true)).ok());
        std::vector<Decision> placed;
        for (std::uint32_t attempt = 0; attempt < 6; ++attempt) {
            ResourceRequirement gpu = test::requirement_of(ResourceClass::Gpu, "gpu");
            const Result<Decision> decision =
                engine.submit_request(test::request_of(0xf100 + attempt, 0xf200 + attempt, {gpu}));
            LS_CHECK(decision.ok());
            if (decision.value().outcome == DecisionOutcome::Placed) {
                placed.push_back(decision.value());
            }
        }
        LS_CHECK_EQ(placed.size(), std::size_t{1});
        LS_CHECK(engine.validate_invariants().all_ok());
        for (const Decision& decision : placed) {
            const CancellationReport report = engine.cancel_placement(
                decision.placement.id, decision.placement.generation, engine.epoch(), "release exclusive");
            LS_CHECK(report.status.ok());
        }
        LS_CHECK_EQ(engine.accounting().total_reserved_slots, std::uint64_t{0});
    }
}

LS_TEST(recovered_state_is_never_silently_current) {
    for (std::uint64_t seed = 1; seed <= 4; ++seed) {
        RandomLab lab(seed, 3, 4);
        lab.register_all_in_random_order();
        for (std::uint32_t step = 0; step < 8; ++step) {
            const ScheduleRequest request = lab.random_request(0x1100 + seed * 16 + step);
            static_cast<void>(lab.engine.submit_request(request));
        }
        const DurableState durable = lab.engine.export_state();
        LS_CHECK(validate_durable_state(durable).ok());

        SchedulerEngine recovered;
        const Result<RecoveryReport> recovery = recovered.import_state(durable);
        LS_CHECK(recovery.ok());
        for (const ResourceRecord& resource : recovered.list_resources()) {
            if (!resource.worker.is_null()) {
                LS_CHECK(!resource.dynamic_authoritative);
            }
        }
        for (const PlacementRecord& placement : recovered.list_placements()) {
            LS_CHECK(placement_state_is_terminal(placement.state));
        }
        LS_CHECK_EQ(recovered.accounting().total_reserved_slots, std::uint64_t{0});
        LS_CHECK(recovered.validate_invariants().all_ok());
    }
}
