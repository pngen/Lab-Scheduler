#include "lab_scheduler/net.hpp"

#include <array>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "lab_scheduler/canonical.hpp"

namespace lab_scheduler {
namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalid = INVALID_SOCKET;

int last_error() noexcept { return WSAGetLastError(); }

void close_native(NativeSocket socket) noexcept { closesocket(socket); }

Status set_reuse(NativeSocket socket) noexcept {
    BOOL enabled = TRUE;
    if (setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&enabled),
                   static_cast<int>(sizeof(enabled))) != 0) {
        return Status(ErrorCode::IoFailure, "setsockopt(SO_REUSEADDR) failed");
    }
    return Status{};
}

Status disable_nagle(NativeSocket socket) noexcept {
    BOOL enabled = TRUE;
    if (setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&enabled),
                   static_cast<int>(sizeof(enabled))) != 0) {
        return Status(ErrorCode::IoFailure, "setsockopt(TCP_NODELAY) failed");
    }
    return Status{};
}
#else
using NativeSocket = int;
constexpr NativeSocket kInvalid = -1;

int last_error() noexcept { return errno; }
void close_native(NativeSocket socket) noexcept { ::close(socket); }

Status set_reuse(NativeSocket socket) noexcept {
    int enabled = 1;
    if (setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) != 0) {
        return Status(ErrorCode::IoFailure, "setsockopt(SO_REUSEADDR) failed");
    }
    return Status{};
}

Status disable_nagle(NativeSocket socket) noexcept {
    int enabled = 1;
    if (setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) != 0) {
        return Status(ErrorCode::IoFailure, "setsockopt(TCP_NODELAY) failed");
    }
    return Status{};
}
#endif

NativeSocket to_native(std::uintptr_t handle) noexcept { return static_cast<NativeSocket>(handle); }
std::uintptr_t to_handle(NativeSocket socket) noexcept { return static_cast<std::uintptr_t>(socket); }

}  // namespace

NetworkRuntime::NetworkRuntime() {
#ifdef _WIN32
    WSADATA data{};
    static_cast<void>(WSAStartup(MAKEWORD(2, 2), &data));
#endif
}

NetworkRuntime::~NetworkRuntime() {
#ifdef _WIN32
    WSACleanup();
#endif
}

TcpSocket::TcpSocket(std::uintptr_t handle) noexcept : handle_(handle) {}

TcpSocket::~TcpSocket() { close(); }

TcpSocket::TcpSocket(TcpSocket&& other) noexcept : handle_(other.handle_) {
    other.handle_ = kInvalidSocketHandle;
}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = kInvalidSocketHandle;
    }
    return *this;
}

void TcpSocket::close() noexcept {
    if (handle_ != kInvalidSocketHandle) {
        close_native(to_native(handle_));
        handle_ = kInvalidSocketHandle;
    }
}

void TcpSocket::shutdown_both() noexcept {
    if (handle_ != kInvalidSocketHandle) {
#ifdef _WIN32
        static_cast<void>(::shutdown(to_native(handle_), SD_BOTH));
#else
        static_cast<void>(::shutdown(to_native(handle_), SHUT_RDWR));
#endif
    }
}

TcpListener::~TcpListener() { close(); }

TcpListener::TcpListener(TcpListener&& other) noexcept : handle_(other.handle_), port_(other.port_) {
    other.handle_ = kInvalidSocketHandle;
    other.port_ = 0;
}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        port_ = other.port_;
        other.handle_ = kInvalidSocketHandle;
        other.port_ = 0;
    }
    return *this;
}

void TcpListener::close() noexcept {
    if (handle_ != kInvalidSocketHandle) {
        close_native(to_native(handle_));
        handle_ = kInvalidSocketHandle;
    }
}

Status TcpListener::listen_loopback(std::uint16_t port, int backlog) {
    close();
    const NativeSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == kInvalid) {
        return Status(ErrorCode::IoFailure, "socket() failed with error " + decimal_i64(last_error()));
    }
    Status status = set_reuse(socket);
    if (!status.ok()) {
        close_native(socket);
        return status;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (::bind(socket, reinterpret_cast<sockaddr*>(&address), static_cast<int>(sizeof(address))) != 0) {
        close_native(socket);
        return Status(ErrorCode::IoFailure, "bind() failed with error " + decimal_i64(last_error()));
    }
    if (::listen(socket, backlog) != 0) {
        close_native(socket);
        return Status(ErrorCode::IoFailure, "listen() failed with error " + decimal_i64(last_error()));
    }
    sockaddr_in bound{};
#ifdef _WIN32
    int length = static_cast<int>(sizeof(bound));
#else
    socklen_t length = static_cast<socklen_t>(sizeof(bound));
#endif
    if (getsockname(socket, reinterpret_cast<sockaddr*>(&bound), &length) != 0) {
        close_native(socket);
        return Status(ErrorCode::IoFailure, "getsockname() failed with error " + decimal_i64(last_error()));
    }
    handle_ = to_handle(socket);
    port_ = ntohs(bound.sin_port);
    return Status{};
}

Result<TcpSocket> TcpListener::accept_one(std::string& peer) {
    if (handle_ == kInvalidSocketHandle) {
        return Status(ErrorCode::IoFailure, "listener is not bound");
    }
    sockaddr_in address{};
#ifdef _WIN32
    int length = static_cast<int>(sizeof(address));
#else
    socklen_t length = static_cast<socklen_t>(sizeof(address));
#endif
    const NativeSocket accepted =
        ::accept(to_native(handle_), reinterpret_cast<sockaddr*>(&address), &length);
    if (accepted == kInvalid) {
        return Status(ErrorCode::IoFailure, "accept() failed with error " + decimal_i64(last_error()));
    }
    static_cast<void>(disable_nagle(accepted));
    std::array<char, 32> buffer{};
    const char* text = inet_ntop(AF_INET, &address.sin_addr, buffer.data(),
                                 static_cast<socklen_t>(buffer.size()));
    peer = text != nullptr ? std::string(text) : std::string("?");
    peer += ":";
    peer += decimal_u64(ntohs(address.sin_port));
    return TcpSocket(to_handle(accepted));
}

Result<TcpSocket> connect_loopback(std::uint16_t port, const std::string& host) {
    const NativeSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == kInvalid) {
        return Status(ErrorCode::IoFailure, "socket() failed with error " + decimal_i64(last_error()));
    }
    static_cast<void>(disable_nagle(socket));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        close_native(socket);
        return Status(ErrorCode::InvalidArgument, "invalid loopback address: " + host);
    }
    if (::connect(socket, reinterpret_cast<sockaddr*>(&address), static_cast<int>(sizeof(address))) != 0) {
        const int code = last_error();
        close_native(socket);
        return Status(ErrorCode::IoFailure, "connect() failed with error " + decimal_i64(code));
    }
    return TcpSocket(to_handle(socket));
}

Status send_all(TcpSocket& socket, std::span<const std::byte> bytes) {
    if (!socket.valid()) {
        return Status(ErrorCode::IoFailure, "cannot send on a closed socket");
    }
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const std::size_t remaining = bytes.size() - offset;
#ifdef _WIN32
        const int chunk = static_cast<int>(remaining > 1u << 20 ? 1u << 20 : remaining);
        const int sent = ::send(to_native(socket.handle()),
                                reinterpret_cast<const char*>(bytes.data() + offset), chunk, 0);
#else
        const ssize_t sent = ::send(to_native(socket.handle()),
                                    reinterpret_cast<const char*>(bytes.data() + offset),
                                    static_cast<std::size_t>(remaining), 0);
#endif
        if (sent <= 0) {
            return Status(ErrorCode::IoFailure, "send() failed with error " + decimal_i64(last_error()));
        }
        offset += static_cast<std::size_t>(sent);
    }
    return Status{};
}

Result<std::vector<std::byte>> receive_exact(TcpSocket& socket, std::size_t count) {
    std::vector<std::byte> buffer(count);
    std::size_t offset = 0;
    while (offset < count) {
        const std::size_t remaining = count - offset;
#ifdef _WIN32
        const int chunk = static_cast<int>(remaining > 1u << 20 ? 1u << 20 : remaining);
        const int received = ::recv(to_native(socket.handle()),
                                    reinterpret_cast<char*>(buffer.data() + offset), chunk, 0);
#else
        const ssize_t received = ::recv(to_native(socket.handle()),
                                        reinterpret_cast<char*>(buffer.data() + offset),
                                        static_cast<std::size_t>(remaining), 0);
#endif
        if (received == 0) {
            return Status(ErrorCode::IoFailure, "peer closed the connection");
        }
        if (received < 0) {
            return Status(ErrorCode::IoFailure, "recv() failed with error " + decimal_i64(last_error()));
        }
        offset += static_cast<std::size_t>(received);
    }
    return buffer;
}

Result<FrameView> receive_frame(TcpSocket& socket, const Limits& limits, std::vector<std::byte>& storage) {
    constexpr std::size_t kLengthFieldSize = 4;
    Result<std::vector<std::byte>> length_bytes = receive_exact(socket, kLengthFieldSize);
    if (!length_bytes.ok()) {
        return length_bytes.status();
    }
    const std::vector<std::byte>& prefix = length_bytes.value();
    std::uint32_t frame_length = 0;
    frame_length |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(prefix[0]));
    frame_length |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(prefix[1])) << 8;
    frame_length |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(prefix[2])) << 16;
    frame_length |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(prefix[3])) << 24;
    if (frame_length == 0 || frame_length + kLengthFieldSize > limits.max_frame_size) {
        return Status(ErrorCode::ProtocolError, "declared frame length is outside the configured bound");
    }
    Result<std::vector<std::byte>> remainder = receive_exact(socket, frame_length);
    if (!remainder.ok()) {
        return remainder.status();
    }
    storage.clear();
    storage.reserve(kLengthFieldSize + frame_length);
    storage.insert(storage.end(), prefix.begin(), prefix.end());
    storage.insert(storage.end(), remainder.value().begin(), remainder.value().end());
    return decode_frame(std::span<const std::byte>(storage.data(), storage.size()), limits);
}

Status send_frame(TcpSocket& socket, MessageType type, std::uint64_t correlation_id,
                  std::uint64_t coordinator_epoch, std::span<const std::byte> payload) {
    Result<std::vector<std::byte>> frame =
        encode_frame(type, correlation_id, coordinator_epoch, payload);
    if (!frame.ok()) {
        return frame.status();
    }
    const std::vector<std::byte>& bytes = frame.value();
    return send_all(socket, std::span<const std::byte>(bytes.data(), bytes.size()));
}

}  // namespace lab_scheduler
