// Lab Scheduler coordinator process.
//
// The coordinator owns scheduling authority. It listens on loopback TCP,
// serves the framed protocol, persists its durable state after every accepted
// mutation, and recovers that state (with a higher coordinator epoch and
// revoked live authority) when it is restarted.

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "cli_support.hpp"
#include "lab_scheduler/cluster.hpp"
#include "lab_scheduler/inspection.hpp"
#include "lab_scheduler/version.hpp"

int main(int argc, char** argv) {
    lab_scheduler::suppress_error_dialogs();
    std::vector<std::string> arguments(argv + 1, argv + argc);

    if (lab_scheduler::has_flag(arguments, "--help") || lab_scheduler::has_flag(arguments, "-h")) {
        std::printf("usage: lab-scheduler-coordinator --port <port> [--state <path>] [--no-persist] [--quiet]\n");
        std::printf("       --port 0 selects an ephemeral loopback port\n");
        return 0;
    }

    try {
        const lab_scheduler::Result<std::uint32_t> port =
            lab_scheduler::option_u32(arguments, "--port", 0);
        if (!port.ok()) {
            std::printf("error %s\n", port.status().to_string().c_str());
            return 2;
        }
        lab_scheduler::CoordinatorConfig config;
        config.port = static_cast<std::uint16_t>(port.value());
        config.state_path = lab_scheduler::option_value(arguments, "--state");
        config.persist_on_mutation = !lab_scheduler::has_flag(arguments, "--no-persist");
        const bool quiet = lab_scheduler::has_flag(arguments, "--quiet");

        lab_scheduler::CoordinatorServer server;
        lab_scheduler::Result<lab_scheduler::CoordinatorStatus> status = server.start(config);
        if (!status.ok()) {
            std::printf("error %s\n", status.status().to_string().c_str());
            return 2;
        }
        std::printf("coordinator listening port=%u\n", static_cast<unsigned>(status.value().port));
        std::printf("coordinator epoch=%s\n", status.value().epoch.to_string().c_str());
        if (status.value().recovered) {
            const lab_scheduler::RecoveryReport& recovery = status.value().recovery;
            std::printf("coordinator recovered previous_epoch=%s current_epoch=%s revalidation=%u reconciled=%u revoked=%u requests=%u history=%u\n",
                        recovery.previous_epoch.to_string().c_str(),
                        recovery.current_epoch.to_string().c_str(),
                        static_cast<unsigned>(recovery.resources_requiring_revalidation.size()),
                        static_cast<unsigned>(recovery.reconciled_placements.size()),
                        static_cast<unsigned>(recovery.revoked_reservations.size()),
                        static_cast<unsigned>(recovery.requests_preserved),
                        static_cast<unsigned>(recovery.placement_history_preserved));
        } else {
            std::printf("coordinator recovered=no\n");
        }
        std::fflush(stdout);

        const lab_scheduler::Status ran = server.run();
        if (!quiet) {
            std::printf("coordinator stopped code=%s\n", lab_scheduler::error_code_name(ran.code).data());
        }
        return ran.ok() ? 0 : 1;
    } catch (const std::exception& error) {
        std::printf("error unhandled exception: %s\n", error.what());
        return 3;
    }
}
