// Lab Scheduler worker/resource agent process.
//
// A worker owns a logical identity that survives restart and a boot identity
// that does not. It registers its resources, publishes their current state,
// executes assignments under the authority envelope it is given, and reports
// completion.

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "cli_support.hpp"
#include "lab_scheduler/cluster.hpp"
#include "lab_scheduler/identity.hpp"
#include "lab_scheduler/lab_profiles.hpp"
#include "lab_scheduler/process.hpp"

int main(int argc, char** argv) {
    lab_scheduler::suppress_error_dialogs();
    std::vector<std::string> arguments(argv + 1, argv + argc);

    if (lab_scheduler::has_flag(arguments, "--help") || lab_scheduler::has_flag(arguments, "-h")) {
        std::printf("usage: lab-scheduler-worker --port <port> --profile <alpha|beta|gamma>\n");
        std::printf("       [--boot <counter>] [--occupancy <n>] [--authority <text>] [--hold]\n");
        return 0;
    }

    try {
        const lab_scheduler::Result<std::uint32_t> port =
            lab_scheduler::option_u32(arguments, "--port", 0);
        if (!port.ok() || port.value() == 0) {
            std::printf("error a coordinator port is required\n");
            return 2;
        }
        const std::string profile = lab_scheduler::option_value(arguments, "--profile");
        if (profile.empty()) {
            std::printf("error a profile is required\n");
            return 2;
        }
        const lab_scheduler::Result<std::uint32_t> boot_counter =
            lab_scheduler::option_u32(arguments, "--boot", 1);
        if (!boot_counter.ok()) {
            std::printf("error %s\n", boot_counter.status().to_string().c_str());
            return 2;
        }
        const lab_scheduler::Result<std::uint32_t> occupancy =
            lab_scheduler::option_u32(arguments, "--occupancy", 0);
        if (!occupancy.ok()) {
            std::printf("error %s\n", occupancy.status().to_string().c_str());
            return 2;
        }

        lab_scheduler::WorkerConfig config;
        config.port = static_cast<std::uint16_t>(port.value());
        config.profile = profile;
        config.current_occupancy = occupancy.value();
        config.authority = lab_scheduler::option_value(arguments, "--authority", "lab-scheduler-worker");
        config.hold_assignments = lab_scheduler::has_flag(arguments, "--hold");

        if (profile == "alpha") {
            config.worker = lab_scheduler::WorkerId::from_value(lab_scheduler::kAlphaWorkerId);
        } else if (profile == "beta") {
            config.worker = lab_scheduler::WorkerId::from_value(lab_scheduler::kBetaWorkerId);
        } else if (profile == "gamma") {
            config.worker = lab_scheduler::WorkerId::from_value(lab_scheduler::kGammaWorkerId);
        } else {
            std::printf("error unknown profile %s\n", profile.c_str());
            return 2;
        }
        config.boot = lab_scheduler::make_worker_boot_id(config.worker, boot_counter.value(),
                                                         lab_scheduler::process_entropy_token());

        std::printf("worker id=%s boot=%s profile=%s pid=%llu\n", config.worker.to_string().c_str(),
                    config.boot.to_string().c_str(), profile.c_str(),
                    static_cast<unsigned long long>(lab_scheduler::current_process_id()));
        std::fflush(stdout);

        lab_scheduler::WorkerAgent agent;
        lab_scheduler::Result<lab_scheduler::HelloAckMessage> hello = agent.start(config);
        if (!hello.ok()) {
            std::printf("error %s\n", hello.status().to_string().c_str());
            return 2;
        }
        for (const lab_scheduler::ResourceId& resource : agent.resource_ids()) {
            std::printf("worker registered resource=%s\n", resource.to_string().c_str());
        }
        std::printf("worker ready epoch=%s\n", hello.value().epoch.to_string().c_str());
        std::fflush(stdout);

        const lab_scheduler::Status served = agent.run();
        std::printf("worker stopped completed=%u rejected=%u code=%s\n",
                    static_cast<unsigned>(agent.completed_assignments()),
                    static_cast<unsigned>(agent.rejected_assignments()),
                    lab_scheduler::error_code_name(served.code).data());
        std::fflush(stdout);
        if (agent.completed_assignments() > 0 && served.ok()) {
            return 0;
        }
        return served.ok() ? 0 : 4;
    } catch (const std::exception& error) {
        std::printf("error unhandled exception: %s\n", error.what());
        return 3;
    }
}
