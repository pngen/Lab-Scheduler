// Lab Scheduler inspection CLI.
//
// Two modes:
//   lab-scheduler-cli --port <port> <subject> [argument]
//   lab-scheduler-cli --state <path> validate-state
//
// Output ordering is deterministic: the coordinator renders inventories in
// identity order and this tool prints the reply verbatim.

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "cli_support.hpp"
#include "lab_scheduler/cluster.hpp"
#include "lab_scheduler/inspection.hpp"
#include "lab_scheduler/persistence.hpp"

int main(int argc, char** argv) {
    lab_scheduler::suppress_error_dialogs();
    std::vector<std::string> arguments(argv + 1, argv + argc);

    if (arguments.empty() || lab_scheduler::has_flag(arguments, "--help") ||
        lab_scheduler::has_flag(arguments, "-h")) {
        std::printf("usage: lab-scheduler-cli --port <port> <subject> [argument]\n");
        std::printf("       subjects: resources requests placements reservations stale workers epoch accounting invariants audit\n");
        std::printf("                 resource <id> request <id> placement <id> explain <decision> explain-placement <placement>\n");
        std::printf("                 save <path> state-validate <path> shutdown\n");
        std::printf("       lab-scheduler-cli --state <path> validate-state\n");
        return arguments.empty() ? 2 : 0;
    }

    try {
        const std::vector<std::string> valued_options{"--port", "--state"};
        const std::vector<std::string> positional =
            lab_scheduler::positional_arguments(arguments, valued_options);
        if (positional.empty()) {
            std::printf("error no inspection subject given\n");
            return 2;
        }

        lab_scheduler::Limits limits;
        const std::string state_path = lab_scheduler::option_value(arguments, "--state");

        if (positional[0] == "validate-state") {
            const std::string path =
                positional.size() > 1 ? positional[1] : state_path;
            if (path.empty()) {
                std::printf("error validate-state requires a persistence path\n");
                return 2;
            }
            lab_scheduler::Result<lab_scheduler::DurableState> state =
                lab_scheduler::load_durable_state(path, limits);
            if (!state.ok()) {
                std::printf("durable_state INVALID\n  error %s\n", state.status().to_string().c_str());
                return 1;
            }
            std::printf("%s", lab_scheduler::render_durable_state_validation(state.value()).c_str());
            return lab_scheduler::validate_durable_state(state.value()).ok() ? 0 : 1;
        }

        const lab_scheduler::Result<std::uint32_t> port =
            lab_scheduler::option_u32(arguments, "--port", 0);
        if (!port.ok() || port.value() == 0) {
            std::printf("error a coordinator port is required for live inspection\n");
            return 2;
        }
        lab_scheduler::Result<lab_scheduler::CoordinatorClient> client =
            lab_scheduler::CoordinatorClient::connect(static_cast<std::uint16_t>(port.value()), limits);
        if (!client.ok()) {
            std::printf("error %s\n", client.status().to_string().c_str());
            return 1;
        }

        std::string subject = positional[0];
        if (positional.size() > 1) {
            subject += " ";
            subject += positional[1];
        }

        if (positional[0] == "shutdown") {
            lab_scheduler::StateAckMessage ack;
            const lab_scheduler::Status status = client.value().call_shutdown(ack);
            if (!status.ok()) {
                std::printf("error %s\n", status.to_string().c_str());
                return 1;
            }
            std::printf("shutdown epoch=%s detail=%s\n", ack.epoch.to_string().c_str(), ack.detail.c_str());
            return 0;
        }
        if (positional[0] == "save") {
            lab_scheduler::StateAckMessage ack;
            const std::string path = positional.size() > 1 ? positional[1] : state_path;
            const lab_scheduler::Status status = client.value().call_save(path, ack);
            if (!status.ok() || ack.code != lab_scheduler::ErrorCode::Ok) {
                std::printf("error %s\n", status.ok() ? ack.detail.c_str() : status.to_string().c_str());
                return 1;
            }
            std::printf("state path=%s epoch=%s resources=%u requests=%u placements=%u reservations=%u\n",
                        ack.path.c_str(), ack.epoch.to_string().c_str(), ack.resources, ack.requests,
                        ack.placements, ack.reservations);
            return 0;
        }

        std::string text;
        const lab_scheduler::Status status = client.value().call_inspect(subject, text);
        if (!status.ok()) {
            std::printf("error %s\n", status.to_string().c_str());
            return 1;
        }
        std::printf("%s", text.c_str());
        return 0;
    } catch (const std::exception& error) {
        std::printf("error unhandled exception: %s\n", error.what());
        return 3;
    }
}
