#include "lab_scheduler/canonical.hpp"

#include <array>
#include <limits>

namespace lab_scheduler {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

constexpr int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

}  // namespace

std::uint64_t fnv1a64_continue(std::uint64_t seed, std::span<const std::byte> bytes) noexcept {
    std::uint64_t hash = seed;
    for (const std::byte b : bytes) {
        hash ^= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(b));
        hash *= kFnvPrime;
    }
    return hash;
}

std::uint64_t fnv1a64(std::span<const std::byte> bytes) noexcept {
    return fnv1a64_continue(kFnvOffsetBasis, bytes);
}

std::uint64_t fnv1a64(std::string_view text) noexcept {
    return fnv1a64_continue(kFnvOffsetBasis,
                            std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                                       text.size()));
}

std::uint64_t mix64(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}

std::string hex_u64(std::uint64_t value, unsigned width) {
    std::string out;
    out.resize(width, '0');
    for (unsigned i = 0; i < width; ++i) {
        const unsigned shift = static_cast<unsigned>((width - 1u - i) * 4u);
        const unsigned nibble = static_cast<unsigned>((value >> shift) & 0xfull);
        out[i] = kHexDigits[nibble];
    }
    return out;
}

bool hex_u64_decode(std::string_view text, unsigned width, std::uint64_t& out) noexcept {
    if (text.size() != width) {
        return false;
    }
    std::uint64_t value = 0;
    for (const char c : text) {
        const int digit = hex_value(c);
        if (digit < 0) {
            return false;
        }
        value = (value << 4) | static_cast<std::uint64_t>(digit);
    }
    out = value;
    return true;
}

bool is_lower_hex(std::string_view text) noexcept {
    if (text.empty()) {
        return false;
    }
    for (const char c : text) {
        if (hex_value(c) < 0) {
            return false;
        }
    }
    return true;
}

std::string to_lower_ascii(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

bool equals_ascii_case_insensitive(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = static_cast<char>(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = static_cast<char>(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

std::string decimal_u64(std::uint64_t value) {
    if (value == 0) {
        return std::string("0");
    }
    std::array<char, 32> buffer{};
    std::size_t index = buffer.size();
    while (value > 0) {
        --index;
        buffer[index] = static_cast<char>('0' + static_cast<char>(value % 10));
        value /= 10;
    }
    return std::string(buffer.data() + index, buffer.size() - index);
}

std::string decimal_i64(std::int64_t value) {
    if (value < 0) {
        const std::uint64_t magnitude = static_cast<std::uint64_t>(-(value + 1)) + 1ull;
        return std::string("-") + decimal_u64(magnitude);
    }
    return decimal_u64(static_cast<std::uint64_t>(value));
}

bool parse_decimal_u64(std::string_view text, std::uint64_t& out) noexcept {
    if (text.empty() || text.size() > 20) {
        return false;
    }
    if (text.size() > 1 && text[0] == '0') {
        return false;  // non-canonical form
    }
    std::uint64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

bool parse_decimal_u32(std::string_view text, std::uint32_t& out) noexcept {
    std::uint64_t wide = 0;
    if (!parse_decimal_u64(text, wide) || wide > 0xffffffffull) {
        return false;
    }
    out = static_cast<std::uint32_t>(wide);
    return true;
}

bool parse_decimal_i64(std::string_view text, std::int64_t& out) noexcept {
    if (text.empty()) {
        return false;
    }
    bool negative = false;
    std::string_view digits = text;
    if (text[0] == '-') {
        negative = true;
        digits = text.substr(1);
    }
    std::uint64_t magnitude = 0;
    if (!parse_decimal_u64(digits, magnitude)) {
        return false;
    }
    if (negative) {
        if (magnitude > 0x8000000000000000ull) {
            return false;
        }
        if (magnitude == 0x8000000000000000ull) {
            out = std::numeric_limits<std::int64_t>::min();
            return true;
        }
        out = -static_cast<std::int64_t>(magnitude);
        return true;
    }
    if (magnitude > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return false;
    }
    out = static_cast<std::int64_t>(magnitude);
    return true;
}

}  // namespace lab_scheduler
