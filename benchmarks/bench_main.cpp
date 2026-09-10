// Lab Scheduler benchmarks.
//
// Every measurement is time to *completed* scheduling work: a placement that
// was validated, ranked, committed, reserved, and (where applicable)
// completed. Enqueue-only cost is never reported as a placement.

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "cli_support.hpp"
#include "lab_scheduler/engine.hpp"
#include "lab_scheduler/inspection.hpp"
#include "lab_scheduler/lab_profiles.hpp"
#include "lab_scheduler/persistence.hpp"
#include "test_support_examples.hpp"

using namespace lab_scheduler;

namespace {

using Clock = std::chrono::steady_clock;

struct Measurement {
    std::string name{};
    std::uint64_t operations = 0;
    double milliseconds = 0.0;

    [[nodiscard]] double per_second() const {
        return milliseconds <= 0.0 ? 0.0 : static_cast<double>(operations) * 1000.0 / milliseconds;
    }
    [[nodiscard]] double micros_per_operation() const {
        return operations == 0 ? 0.0 : (milliseconds * 1000.0) / static_cast<double>(operations);
    }
};

std::vector<Measurement> g_measurements{};

void record(const std::string& name, std::uint64_t operations, double milliseconds) {
    g_measurements.push_back(Measurement{name, operations, milliseconds});
}

double elapsed_ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

struct BenchResources {
    std::vector<ResourceAdvertisement> advertisements{};
    std::uint32_t workers = 8;
    std::uint32_t per_worker = 16;
    std::uint32_t hosts = 4;

    BenchResources() {
        for (std::uint32_t worker = 0; worker < workers; ++worker) {
            const std::uint64_t worker_id = 0x100 + worker;
            const std::uint64_t boot = 0x200 + worker;
            const std::string host = "bench-host-" + decimal_u64(worker % hosts);
            for (std::uint32_t index = 0; index < per_worker; ++index) {
                const std::uint64_t id = 0x10000 + worker * 64 + index;
                switch (index % 4) {
                    case 0:
                        advertisements.push_back(
                            examples::gpu(id, worker_id, boot, host, 4));
                        break;
                    case 1:
                        advertisements.push_back(
                            examples::model(id, worker_id, boot, host, 0x1f01, "1.0", true));
                        break;
                    case 2:
                        advertisements.push_back(
                            examples::dataset(id, worker_id, boot, host, 0x1f02, 0x2f01));
                        break;
                    default:
                        advertisements.push_back(
                            examples::simulator(id, worker_id, boot, host, 0x1f03, "1.0"));
                        break;
                }
            }
        }
    }

    [[nodiscard]] std::size_t size() const { return advertisements.size(); }
};

ScheduleRequest bench_request(std::uint64_t index, bool compound) {
    ResourceRequirement gpu = examples::requirement(ResourceClass::Gpu, "accelerator");
    gpu.min_slots = 1;
    if (!compound) {
        return examples::request(0x40000 + index, 0x50000 + index, {gpu});
    }
    ResourceRequirement model = examples::requirement(ResourceClass::Model, "model");
    model.model = ModelResourceId::from_value(0x1f01);
    ResourceRequirement dataset = examples::requirement(ResourceClass::Dataset, "corpus");
    dataset.dataset = DatasetId::from_value(0x1f02);
    dataset.dataset_version = DatasetVersionId::from_value(0x2f01);
    ResourceRequirement simulator = examples::requirement(ResourceClass::Simulator, "simulator");
    simulator.simulator = SimulatorId::from_value(0x1f03);
    return examples::request(0x40000 + index, 0x50000 + index, {gpu, model, dataset, simulator});
}

void bench_registration(const BenchResources& resources) {
    SchedulerEngine engine;
    const Clock::time_point start = Clock::now();
    for (const ResourceAdvertisement& advertisement : resources.advertisements) {
        static_cast<void>(engine.register_resource(advertisement));
    }
    record("resource_registration", resources.size(), elapsed_ms(start));
}

void bench_publication(const BenchResources& resources) {
    SchedulerEngine engine;
    for (const ResourceAdvertisement& advertisement : resources.advertisements) {
        static_cast<void>(engine.register_resource(advertisement));
    }
    const std::uint32_t rounds = 4;
    const Clock::time_point start = Clock::now();
    for (std::uint32_t round = 0; round < rounds; ++round) {
        for (ResourceAdvertisement advertisement : resources.advertisements) {
            advertisement.current_occupancy = (round + advertisement.capacity.slots) % advertisement.capacity.slots;
            static_cast<void>(engine.publish_resource_state(advertisement));
        }
    }
    record("resource_state_publication", static_cast<std::uint64_t>(resources.size()) * rounds,
           elapsed_ms(start));
}

void bench_scheduling(const BenchResources& resources, bool compound, const char* name,
                      std::uint32_t placements) {
    SchedulerEngine engine;
    for (const ResourceAdvertisement& advertisement : resources.advertisements) {
        static_cast<void>(engine.register_resource(advertisement));
    }
    std::vector<Decision> decisions;
    decisions.reserve(placements);
    const Clock::time_point start = Clock::now();
    for (std::uint32_t index = 0; index < placements; ++index) {
        const Result<Decision> decision = engine.submit_request(bench_request(index, compound));
        if (!decision.ok() || decision.value().outcome != DecisionOutcome::Placed) {
            std::printf("benchmark %s stopped early at request %u\n", name, index);
            break;
        }
        decisions.push_back(decision.value());
    }
    const double scheduled = elapsed_ms(start);
    record(name, decisions.size(), scheduled);

    std::uint64_t completed = 0;
    const Clock::time_point completion_start = Clock::now();
    for (const Decision& decision : decisions) {
        CompletionClaim claim;
        claim.placement = decision.placement.id;
        claim.placement_generation = decision.placement.generation;
        claim.experiment = decision.placement.experiment;
        claim.experiment_generation = decision.placement.experiment_generation;
        claim.epoch = engine.epoch();
        claim.worker_boot = decision.placement.worker_boot;
        for (const SelectedResource& selected : decision.placement.resources) {
            claim.resources.push_back(selected.resource);
            claim.resource_generations.push_back(selected.generation);
        }
        const CompletionOutcome outcome = engine.report_completion(claim);
        if (outcome.accepted) {
            ++completed;
        }
    }
    record("completed_placements", completed, scheduled + elapsed_ms(completion_start));
}

void bench_reservation_commit(const BenchResources& resources, std::uint32_t placements) {
    SchedulerEngine engine;
    for (const ResourceAdvertisement& advertisement : resources.advertisements) {
        static_cast<void>(engine.register_resource(advertisement));
    }
    const Clock::time_point start = Clock::now();
    for (std::uint32_t index = 0; index < placements; ++index) {
        static_cast<void>(engine.submit_request(bench_request(index, false)));
    }
    record("reservation_commit", placements, elapsed_ms(start));
}

void bench_explanation(const BenchResources& resources, std::uint32_t count) {
    SchedulerEngine engine;
    for (const ResourceAdvertisement& advertisement : resources.advertisements) {
        static_cast<void>(engine.register_resource(advertisement));
    }
    std::vector<Explanation> explanations;
    for (std::uint32_t index = 0; index < count; ++index) {
        const Result<Decision> decision = engine.submit_request(bench_request(index, false));
        if (decision.ok()) {
            explanations.push_back(decision.value().explanation);
        }
    }
    const Clock::time_point start = Clock::now();
    std::size_t characters = 0;
    for (const Explanation& explanation : explanations) {
        characters += render_explanation(explanation).size();
    }
    record("explanation_generation", explanations.size(), elapsed_ms(start));
    std::printf("benchmark explanation characters=%zu\n", characters);
}

void bench_snapshot(const BenchResources& resources, std::uint32_t reads) {
    SchedulerEngine engine;
    for (const ResourceAdvertisement& advertisement : resources.advertisements) {
        static_cast<void>(engine.register_resource(advertisement));
    }
    const Clock::time_point start = Clock::now();
    std::size_t total = 0;
    for (std::uint32_t index = 0; index < reads; ++index) {
        total += engine.snapshot().resources.size();
        total += engine.accounting().resources.size();
    }
    record("snapshot_and_accounting_read", static_cast<std::uint64_t>(reads) * 2, elapsed_ms(start));
    std::printf("benchmark snapshot rows=%zu\n", total);
}

void bench_persistence(const BenchResources& resources, std::uint32_t rounds) {
    SchedulerEngine engine;
    for (const ResourceAdvertisement& advertisement : resources.advertisements) {
        static_cast<void>(engine.register_resource(advertisement));
    }
    for (std::uint32_t index = 0; index < 64; ++index) {
        static_cast<void>(engine.submit_request(bench_request(index, false)));
    }
    const DurableState state = engine.export_state();
    const Clock::time_point start = Clock::now();
    std::size_t bytes = 0;
    for (std::uint32_t index = 0; index < rounds; ++index) {
        const Result<std::vector<std::byte>> encoded = encode_durable_state(state, Limits{});
        if (!encoded.ok()) {
            break;
        }
        bytes = encoded.value().size();
        const Result<DurableState> decoded = decode_durable_state(encoded.value(), Limits{});
        if (!decoded.ok()) {
            break;
        }
    }
    record("persistence_save_and_load", rounds, elapsed_ms(start));
    std::printf("benchmark persistence bytes=%zu\n", bytes);
}

}  // namespace

int main(int argc, char** argv) {
    suppress_error_dialogs();
    const std::vector<std::string> arguments(argv + 1, argv + argc);
    BenchResources resources;
    std::printf("benchmark resources=%zu workers=%u hosts=%u\n", resources.size(), resources.workers,
                resources.hosts);

    bench_registration(resources);
    bench_publication(resources);
    bench_reservation_commit(resources, 64);
    bench_scheduling(resources, false, "single_requirement_placement", 64);
    bench_scheduling(resources, true, "compound_placement", 32);
    bench_explanation(resources, 32);
    bench_snapshot(resources, 64);
    bench_persistence(resources, 16);

    std::printf("benchmark results\n");
    for (const Measurement& measurement : g_measurements) {
        std::printf("  %-32s operations=%llu total_ms=%.3f per_second=%.1f micros_per_op=%.3f\n",
                    measurement.name.c_str(), static_cast<unsigned long long>(measurement.operations),
                    measurement.milliseconds, measurement.per_second(), measurement.micros_per_operation());
    }
    std::fflush(stdout);
    return 0;
}
