// Real accelerator-backed scheduling proof.
//
// The GPU is discovered through the CUDA runtime, advertised as a REAL
// resource with real device properties, selected by Lab Scheduler from
// explicit current evidence, and then actually used: memory is allocated, data
// is transferred, a kernel executes, the device synchronizes, results are
// copied back, parity is verified, and completion is reported under the
// placement authority envelope.

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "cli_support.hpp"
#include "lab_scheduler/engine.hpp"
#include "lab_scheduler/identity.hpp"
#include "lab_scheduler/inspection.hpp"
#include "lab_scheduler/process.hpp"
#include "test_support_examples.hpp"

extern "C" int lab_scheduler_cuda_device_count();
extern "C" int lab_scheduler_cuda_device_info(int device, char* name, int name_size, unsigned long long* memory,
                                              int* compute_major, int* compute_minor, int* multiprocessors);
extern "C" int lab_scheduler_cuda_vector_add(int device, int count, float* host_out, char* message,
                                             int message_size);

using namespace lab_scheduler;

int main(int argc, char** argv) {
    suppress_error_dialogs();
    static_cast<void>(argc);
    static_cast<void>(argv);

    try {
        const int device_count = lab_scheduler_cuda_device_count();
        if (device_count <= 0) {
            std::printf("cuda proof UNSUPPORTED: no CUDA device is visible to this process\n");
            return 0;
        }
        std::printf("cuda proof devices=%d\n", device_count);

        SchedulerEngine engine;
        const WorkerId worker = WorkerId::from_value(0xc001);
        const WorkerBootId boot = make_worker_boot_id(worker, 1, process_entropy_token());
        const std::string host = "cuda-host";

        std::vector<std::string> device_names;
        for (int device = 0; device < device_count; ++device) {
            char name_buffer[256] = {};
            unsigned long long memory = 0;
            int major = 0;
            int minor = 0;
            int multiprocessors = 0;
            if (lab_scheduler_cuda_device_info(device, name_buffer, sizeof(name_buffer), &memory, &major, &minor,
                                               &multiprocessors) != 0) {
                std::printf("cuda proof UNSUPPORTED: device %d properties unavailable\n", device);
                return 0;
            }
            device_names.push_back(name_buffer);
            std::printf("cuda proof device=%d name=%s memory_bytes=%llu compute_capability=%d.%d multiprocessors=%d\n",
                        device, name_buffer, memory, major, minor, multiprocessors);

            auto advertisement = examples::gpu(0xd000 + static_cast<std::uint64_t>(device), worker.value(),
                                               boot.value(), host, 2);
            advertisement.name = std::string("cuda-") + name_buffer;
            advertisement.provenance = Provenance::Real;
            advertisement.capacity.memory_bytes = memory;
            advertisement.accelerators.clear();
            AcceleratorAttachment attachment;
            attachment.id = AcceleratorId::from_value(0xd100 + static_cast<std::uint64_t>(device));
            attachment.model = name_buffer;
            attachment.memory_bytes = memory;
            attachment.compute_capability_major = static_cast<std::uint32_t>(major);
            attachment.compute_capability_minor = static_cast<std::uint32_t>(minor);
            advertisement.accelerators.push_back(attachment);
            static_cast<void>(advertisement.capabilities.add("cuda", Limits{}));
            static_cast<void>(advertisement.capabilities.add("training", Limits{}));
            const Result<ResourceRecord> registered = engine.register_resource(advertisement);
            if (!registered.ok()) {
                std::printf("cuda proof FAIL registration: %s\n", registered.status().to_string().c_str());
                return 1;
            }
        }

        ResourceRequirement accelerator = examples::requirement(ResourceClass::Gpu, "accelerator");
        accelerator.required_capabilities.add("cuda", Limits{});
        accelerator.min_memory_bytes = 4ull * 1024ull * 1024ull * 1024ull;
        accelerator.min_slots = 1;
        const Result<Decision> decision =
            engine.submit_request(examples::request(0xe001, 0xf001, {accelerator}));
        if (!decision.ok()) {
            std::printf("cuda proof FAIL scheduling: %s\n", decision.status().to_string().c_str());
            return 1;
        }
        if (decision.value().outcome != DecisionOutcome::Placed) {
            std::printf("cuda proof UNSUPPORTED: no eligible accelerator: %s\n",
                        decision.value().failure_detail.c_str());
            return 0;
        }
        const SelectedResource& selected = decision.value().placement.resources.front();
        const std::uint64_t selected_index = selected.resource.value() - 0xd000;
        std::printf("cuda proof selected=%s requirement=%s slots=%u exclusive=%s\n",
                    selected.resource.to_string().c_str(), selected.requirement_name.c_str(), selected.slots,
                    selected.exclusive ? "yes" : "no");
        if (selected_index >= device_names.size()) {
            std::printf("cuda proof FAIL selected resource is not a discovered device\n");
            return 1;
        }

        const int elements = 1 << 18;
        std::vector<float> output(static_cast<std::size_t>(elements), 0.0f);
        char message[256] = {};
        const int result = lab_scheduler_cuda_vector_add(static_cast<int>(selected_index), elements,
                                                         output.data(), message, sizeof(message));
        std::printf("cuda proof kernel elements=%d result=%d detail=%s\n", elements, result, message);
        if (result != 0) {
            std::printf("cuda proof FAIL kernel execution\n");
            return 1;
        }

        CompletionClaim claim;
        claim.placement = decision.value().placement.id;
        claim.placement_generation = decision.value().placement.generation;
        claim.experiment = decision.value().placement.experiment;
        claim.experiment_generation = decision.value().placement.experiment_generation;
        claim.epoch = engine.epoch();
        claim.worker_boot = decision.value().placement.worker_boot;
        for (const SelectedResource& entry : decision.value().placement.resources) {
            claim.resources.push_back(entry.resource);
            claim.resource_generations.push_back(entry.generation);
        }
        claim.detail = "cuda vector add verified";
        const CompletionOutcome outcome = engine.report_completion(claim);
        const AccountingReport accounting = engine.accounting();
        std::printf("cuda proof completion accepted=%s state=%s\n", outcome.accepted ? "yes" : "no",
                    std::string(placement_state_name(outcome.state)).c_str());
        std::printf("cuda proof reservations_active=%llu reserved_slots=%llu invariants=%s\n",
                    static_cast<unsigned long long>(accounting.active_reservations),
                    static_cast<unsigned long long>(accounting.total_reserved_slots),
                    engine.validate_invariants().all_ok() ? "ok" : "violated");
        std::printf("cuda proof REAL: %d device(s), scheduler-selected device %llu (%s), verified parity on %d elements\n",
                    device_count, static_cast<unsigned long long>(selected_index),
                    device_names[static_cast<std::size_t>(selected_index)].c_str(), elements);
        return outcome.accepted && accounting.active_reservations == 0 &&
                       accounting.total_reserved_slots == 0 && engine.validate_invariants().all_ok()
                   ? 0
                   : 1;
    } catch (const std::exception& error) {
        std::printf("cuda proof FAIL exception: %s\n", error.what());
        return 3;
    }
}
