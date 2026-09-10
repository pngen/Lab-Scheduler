#include "lab_scheduler/process.hpp"

#include <array>
#include <cstring>
#include <string>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#include "lab_scheduler/canonical.hpp"

namespace lab_scheduler {

struct ChildProcess::Impl {
#ifdef _WIN32
    HANDLE process = nullptr;
    HANDLE read_pipe = nullptr;
    std::string buffer{};
#else
    pid_t pid = -1;
    int read_fd = -1;
    std::string buffer{};
#endif
};

ChildProcess::ChildProcess() noexcept = default;

ChildProcess::~ChildProcess() { close(); }

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : impl_(std::move(other.impl_)), process_id_(other.process_id_) {
    other.process_id_ = 0;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
    if (this != &other) {
        close();
        impl_ = std::move(other.impl_);
        process_id_ = other.process_id_;
        other.process_id_ = 0;
    }
    return *this;
}

std::uint64_t current_process_id() noexcept {
#ifdef _WIN32
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

Result<ChildProcess> ChildProcess::spawn(const std::string& program,
                                         const std::vector<std::string>& arguments) {
#ifdef _WIN32
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &attributes, 0)) {
        return Status(ErrorCode::IoFailure, "CreatePipe failed");
    }
    if (!SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        return Status(ErrorCode::IoFailure, "SetHandleInformation failed");
    }

    std::string command_line = "\"" + program + "\"";
    for (const std::string& argument : arguments) {
        command_line += " \"";
        command_line += argument;
        command_line += "\"";
    }
    std::vector<char> mutable_line(command_line.begin(), command_line.end());
    mutable_line.push_back('\0');

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    PROCESS_INFORMATION information{};
    const DWORD flags = CREATE_NO_WINDOW;
    if (!CreateProcessA(nullptr, mutable_line.data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr,
                        &startup, &information)) {
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        return Status(ErrorCode::IoFailure, "CreateProcess failed for " + program);
    }
    CloseHandle(write_pipe);
    CloseHandle(information.hThread);

    ChildProcess child;
    child.impl_ = std::make_unique<Impl>();
    child.impl_->process = information.hProcess;
    child.impl_->read_pipe = read_pipe;
    child.process_id_ = static_cast<std::uint64_t>(information.dwProcessId);
    return child;
#else
    int pipe_fds[2] = {-1, -1};
    if (pipe(pipe_fds) != 0) {
        return Status(ErrorCode::IoFailure, "pipe failed");
    }
    std::vector<std::string> storage;
    storage.push_back(program);
    for (const std::string& argument : arguments) {
        storage.push_back(argument);
    }
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (std::string& entry : storage) {
        argv.push_back(entry.data());
    }
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
    pid_t pid = -1;
    const int rc = posix_spawn(&pid, program.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipe_fds[1]);
    if (rc != 0) {
        close(pipe_fds[0]);
        return Status(ErrorCode::IoFailure, "posix_spawn failed");
    }
    ChildProcess child;
    child.impl_ = std::make_unique<Impl>();
    child.impl_->pid = pid;
    child.impl_->read_fd = pipe_fds[0];
    child.process_id_ = static_cast<std::uint64_t>(pid);
    return child;
#endif
}

Result<std::string> ChildProcess::read_line() {
    if (impl_ == nullptr) {
        return Status(ErrorCode::IoFailure, "child process is not open");
    }
    for (;;) {
        const std::size_t newline = impl_->buffer.find('\n');
        if (newline != std::string::npos) {
            std::string line = impl_->buffer.substr(0, newline);
            impl_->buffer.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            return line;
        }
        std::array<char, 512> chunk{};
#ifdef _WIN32
        DWORD read = 0;
        const BOOL ok = ReadFile(impl_->read_pipe, chunk.data(), static_cast<DWORD>(chunk.size()), &read,
                                 nullptr);
        if (!ok || read == 0) {
            return Status(ErrorCode::IoFailure, "child output stream ended");
        }
#else
        const ssize_t read = ::read(impl_->read_fd, chunk.data(), chunk.size());
        if (read <= 0) {
            return Status(ErrorCode::IoFailure, "child output stream ended");
        }
#endif
        impl_->buffer.append(chunk.data(), static_cast<std::size_t>(read));
    }
}

Status ChildProcess::terminate() {
    if (impl_ == nullptr) {
        return Status(ErrorCode::IoFailure, "child process is not open");
    }
#ifdef _WIN32
    if (!TerminateProcess(impl_->process, 1)) {
        return Status(ErrorCode::IoFailure, "TerminateProcess failed");
    }
    return Status{};
#else
    if (kill(impl_->pid, SIGKILL) != 0) {
        return Status(ErrorCode::IoFailure, "kill failed");
    }
    return Status{};
#endif
}

bool ChildProcess::running() {
    if (impl_ == nullptr) {
        return false;
    }
#ifdef _WIN32
    return WaitForSingleObject(impl_->process, 0) == WAIT_TIMEOUT;
#else
    int status = 0;
    const pid_t result = waitpid(impl_->pid, &status, WNOHANG);
    return result == 0;
#endif
}

Result<int> ChildProcess::wait() {
    if (impl_ == nullptr) {
        return Status(ErrorCode::IoFailure, "child process is not open");
    }
#ifdef _WIN32
    const DWORD result = WaitForSingleObject(impl_->process, INFINITE);
    if (result != WAIT_OBJECT_0) {
        return Status(ErrorCode::IoFailure, "WaitForSingleObject failed");
    }
    DWORD code = 0;
    if (!GetExitCodeProcess(impl_->process, &code)) {
        return Status(ErrorCode::IoFailure, "GetExitCodeProcess failed");
    }
    return static_cast<int>(code);
#else
    int status = 0;
    if (waitpid(impl_->pid, &status, 0) < 0) {
        return Status(ErrorCode::IoFailure, "waitpid failed");
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return 1;
#endif
}

void ChildProcess::close() noexcept {
    if (impl_ == nullptr) {
        return;
    }
#ifdef _WIN32
    if (impl_->read_pipe != nullptr) {
        CloseHandle(impl_->read_pipe);
        impl_->read_pipe = nullptr;
    }
    if (impl_->process != nullptr) {
        CloseHandle(impl_->process);
        impl_->process = nullptr;
    }
#else
    if (impl_->read_fd >= 0) {
        ::close(impl_->read_fd);
        impl_->read_fd = -1;
    }
#endif
    impl_.reset();
    process_id_ = 0;
}

}  // namespace lab_scheduler
