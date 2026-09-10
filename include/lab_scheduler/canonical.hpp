#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace lab_scheduler {

// FNV-1a 64 is used for both frame integrity and persistence integrity. It is
// deliberately simple, dependency-free, and identical on every platform, so a
// persisted file or frame produced by one build validates in another.
inline constexpr std::uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ull;
inline constexpr std::uint64_t kFnvPrime = 0x100000001b3ull;

std::uint64_t fnv1a64_continue(std::uint64_t seed, std::span<const std::byte> bytes) noexcept;
std::uint64_t fnv1a64(std::span<const std::byte> bytes) noexcept;
std::uint64_t fnv1a64(std::string_view text) noexcept;

// Deterministic 64-bit mixing used for stable hash ordering (never for
// cryptographic purposes).
std::uint64_t mix64(std::uint64_t value) noexcept;

std::string hex_u64(std::uint64_t value, unsigned width);
bool hex_u64_decode(std::string_view text, unsigned width, std::uint64_t& out) noexcept;

bool is_lower_hex(std::string_view text) noexcept;
std::string to_lower_ascii(std::string_view text);
bool equals_ascii_case_insensitive(std::string_view a, std::string_view b) noexcept;

// Canonical, locale independent integer text. Parsing rejects leading '+',
// leading zeros (other than "0" itself), empty input, and overflow.
std::string decimal_u64(std::uint64_t value);
std::string decimal_i64(std::int64_t value);
bool parse_decimal_u64(std::string_view text, std::uint64_t& out) noexcept;
bool parse_decimal_u32(std::string_view text, std::uint32_t& out) noexcept;
bool parse_decimal_i64(std::string_view text, std::int64_t& out) noexcept;

}  // namespace lab_scheduler
