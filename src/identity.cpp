#include "lab_scheduler/identity.hpp"

#include <atomic>
#include <chrono>
#include <random>
#include <thread>

namespace lab_scheduler {
namespace {

std::uint64_t hash_name(std::string_view name) noexcept {
    const std::string lowered = to_lower_ascii(name);
    std::uint64_t hash = fnv1a64(lowered);
    hash = mix64(hash);
    if (hash == 0) {
        hash = 1;
    }
    return hash;
}

}  // namespace

CapabilityId capability_id_from_name(std::string_view name) noexcept {
    return CapabilityId::from_value(hash_name(name));
}

TopologyDomainId topology_domain_from_name(std::string_view name) noexcept {
    return TopologyDomainId::from_value(hash_name(name));
}

std::uint64_t process_entropy_token() noexcept {
    static const std::uint64_t token = []() noexcept {
        std::random_device device;
        std::uint64_t mixed = mix64(static_cast<std::uint64_t>(device()));
        mixed ^= mix64(static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        mixed ^= mix64(static_cast<std::uint64_t>(
            std::hash<std::thread::id>{}(std::this_thread::get_id())));
        static std::atomic<std::uint64_t> sequence{0};
        mixed ^= mix64(sequence.fetch_add(1) + 1);
        return mixed == 0 ? 1 : mixed;
    }();
    return token;
}

WorkerBootId make_worker_boot_id(WorkerId worker, std::uint64_t boot_counter,
                                 std::uint64_t process_token) noexcept {
    std::uint64_t value = mix64(worker.value() ^ (boot_counter * 0x9e3779b97f4a7c15ull));
    value = mix64(value ^ process_token);
    if (value == 0) {
        value = 1;
    }
    return WorkerBootId::from_value(value);
}

}  // namespace lab_scheduler
