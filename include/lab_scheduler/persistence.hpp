#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "lab_scheduler/durable.hpp"
#include "lab_scheduler/error.hpp"
#include "lab_scheduler/limits.hpp"

namespace lab_scheduler {

// Container layout (little endian):
//   magic         8 bytes  "LSCHED01"
//   format        u32      container format version
//   payload_len   u64      bytes of payload that follow
//   checksum      u64      FNV-1a 64 over the payload
//   payload       payload_len bytes of bounded, self describing records
//
// The decoder rejects a bad magic, an unsupported format, an impossible
// payload length, a checksum mismatch, a truncated payload, any count above
// the configured bound, an impossible enum value, a duplicate identity, an
// invalid reference, and trailing bytes after the payload.
inline constexpr std::size_t kPersistenceHeaderSize = 28;
inline constexpr char kPersistenceMagic[8] = {'L', 'S', 'C', 'H', 'E', 'D', '0', '1'};

Result<std::vector<std::byte>> encode_durable_state(const DurableState& state, const Limits& limits);
Result<DurableState> decode_durable_state(std::span<const std::byte> bytes, const Limits& limits);

// Writes the container through a temporary file in the same directory and
// replaces the destination atomically, so a partial write can never become
// authoritative.
Status save_durable_state(const DurableState& state, const std::string& path, const Limits& limits,
                          bool flush_to_disk = true);
Result<DurableState> load_durable_state(const std::string& path, const Limits& limits);

}  // namespace lab_scheduler
