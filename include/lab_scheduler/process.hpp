#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "lab_scheduler/error.hpp"

namespace lab_scheduler {

// Real operating system processes are used for every distributed claim: the
// multiprocess proof starts the coordinator and the workers as independent
// processes, kills a worker with real process termination, and restarts the
// coordinator as a new process. Nothing here simulates death with a flag.
class ChildProcess {
public:
    ChildProcess() noexcept;
    ~ChildProcess();

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ChildProcess(ChildProcess&& other) noexcept;
    ChildProcess& operator=(ChildProcess&& other) noexcept;

    // Starts a process with stdout and stderr merged into one pipe and no
    // console window.
    static Result<ChildProcess> spawn(const std::string& program, const std::vector<std::string>& arguments);

    // Blocking read of one line. Returns an empty optional when the stream
    // ends. No timeout: the caller reads exactly the lines the child promises.
    Result<std::string> read_line();

    // Terminates the process with real OS termination (TerminateProcess on
    // Windows, SIGKILL elsewhere).
    Status terminate();

    [[nodiscard]] bool running();
    Result<int> wait();
    void close() noexcept;
    [[nodiscard]] std::uint64_t process_id() const noexcept { return process_id_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_{};
    std::uint64_t process_id_ = 0;
};

// Current process identifier.
std::uint64_t current_process_id() noexcept;

}  // namespace lab_scheduler
