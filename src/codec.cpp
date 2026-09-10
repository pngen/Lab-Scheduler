#include "lab_scheduler/codec.hpp"

#include <string>

namespace lab_scheduler {

void ByteWriter::u8(std::uint8_t value) { buffer_.push_back(static_cast<std::byte>(value)); }

void ByteWriter::u16(std::uint16_t value) {
    u8(static_cast<std::uint8_t>(value & 0xffu));
    u8(static_cast<std::uint8_t>((value >> 8) & 0xffu));
}

void ByteWriter::u32(std::uint32_t value) {
    u16(static_cast<std::uint16_t>(value & 0xffffu));
    u16(static_cast<std::uint16_t>((value >> 16) & 0xffffu));
}

void ByteWriter::u64(std::uint64_t value) {
    u32(static_cast<std::uint32_t>(value & 0xffffffffull));
    u32(static_cast<std::uint32_t>((value >> 32) & 0xffffffffull));
}

void ByteWriter::boolean(bool value) { u8(value ? 1u : 0u); }

void ByteWriter::raw(std::span<const std::byte> bytes) {
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

void ByteWriter::str(std::string_view text) {
    u32(static_cast<std::uint32_t>(text.size()));
    buffer_.insert(buffer_.end(), reinterpret_cast<const std::byte*>(text.data()),
                   reinterpret_cast<const std::byte*>(text.data()) + text.size());
}

ByteReader::ByteReader(std::span<const std::byte> bytes, ErrorCode failure) noexcept
    : bytes_(bytes), failure_(failure) {}

Status ByteReader::fail(std::string_view what) const {
    std::string message = "decoding failed at offset ";
    message += std::to_string(offset_);
    message += ": ";
    message += what;
    return Status(failure_, std::move(message));
}

Status ByteReader::u8(std::uint8_t& out) {
    if (remaining() < 1) {
        return fail("truncated u8");
    }
    out = std::to_integer<std::uint8_t>(bytes_[offset_]);
    ++offset_;
    return Status{};
}

Status ByteReader::u16(std::uint16_t& out) {
    std::uint8_t lo = 0;
    std::uint8_t hi = 0;
    Status status = u8(lo);
    if (!status.ok()) {
        return status;
    }
    status = u8(hi);
    if (!status.ok()) {
        return status;
    }
    out = static_cast<std::uint16_t>(static_cast<std::uint16_t>(lo) |
                                     static_cast<std::uint16_t>(static_cast<std::uint16_t>(hi) << 8));
    return Status{};
}

Status ByteReader::u32(std::uint32_t& out) {
    std::uint16_t lo = 0;
    std::uint16_t hi = 0;
    Status status = u16(lo);
    if (!status.ok()) {
        return status;
    }
    status = u16(hi);
    if (!status.ok()) {
        return status;
    }
    out = static_cast<std::uint32_t>(static_cast<std::uint32_t>(lo) |
                                     (static_cast<std::uint32_t>(hi) << 16));
    return Status{};
}

Status ByteReader::u64(std::uint64_t& out) {
    std::uint32_t lo = 0;
    std::uint32_t hi = 0;
    Status status = u32(lo);
    if (!status.ok()) {
        return status;
    }
    status = u32(hi);
    if (!status.ok()) {
        return status;
    }
    out = static_cast<std::uint64_t>(lo) | (static_cast<std::uint64_t>(hi) << 32);
    return Status{};
}

Status ByteReader::boolean(bool& out) {
    std::uint8_t value = 0;
    Status status = u8(value);
    if (!status.ok()) {
        return status;
    }
    if (value > 1) {
        return fail("non-canonical boolean");
    }
    out = value != 0;
    return Status{};
}

Status ByteReader::raw(std::size_t count, std::span<const std::byte>& out) {
    if (count > remaining()) {
        return fail("truncated byte range");
    }
    out = bytes_.subspan(offset_, count);
    offset_ += count;
    return Status{};
}

Status ByteReader::str(std::string& out, std::uint32_t max_length) {
    std::uint32_t length = 0;
    Status status = u32(length);
    if (!status.ok()) {
        return status;
    }
    if (length > max_length) {
        return fail("string length exceeds configured bound");
    }
    if (length > remaining()) {
        return fail("truncated string");
    }
    out.assign(reinterpret_cast<const char*>(bytes_.data() + offset_), length);
    offset_ += length;
    return Status{};
}

Status ByteReader::count(std::uint32_t& out, std::uint32_t max_count) {
    std::uint32_t value = 0;
    Status status = u32(value);
    if (!status.ok()) {
        return status;
    }
    if (value > max_count) {
        return fail("record count exceeds configured bound");
    }
    out = value;
    return Status{};
}

Status ByteReader::skip(std::size_t count) {
    if (count > remaining()) {
        return fail("truncated skip");
    }
    offset_ += count;
    return Status{};
}

Status ByteReader::require_end() const {
    if (!at_end()) {
        return fail("trailing bytes after payload");
    }
    return Status{};
}

}  // namespace lab_scheduler
