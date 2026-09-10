#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace lab_scheduler {

inline constexpr int kVersionMajor = 1;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 0;
inline constexpr std::string_view kProductName = "Lab Scheduler";
inline constexpr std::string_view kVersionString = "1.0.0";

// Wire protocol version carried by every frame header.
inline constexpr std::uint32_t kProtocolVersion = 1;
// Persistence container version written into every state file header.
inline constexpr std::uint32_t kPersistenceFormatVersion = 1;

std::string version_banner();
std::string version_string();

}  // namespace lab_scheduler
