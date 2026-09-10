#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "lab_scheduler/error.hpp"
#include "lab_scheduler/limits.hpp"

namespace lab_scheduler {

// Little-endian, fixed width primitives plus bounded, length-prefixed
// strings. Both the wire protocol and the persistence container use this
// codec so that a single hardened decoder serves every untrusted byte range.
class ByteWriter {
public:
    void u8(std::uint8_t value);
    void u16(std::uint16_t value);
    void u32(std::uint32_t value);
    void u64(std::uint64_t value);
    void boolean(bool value);
    void raw(std::span<const std::byte> bytes);
    void str(std::string_view text);

    [[nodiscard]] const std::vector<std::byte>& data() const noexcept { return buffer_; }
    [[nodiscard]] std::vector<std::byte>& data() noexcept { return buffer_; }
    [[nodiscard]] std::span<const std::byte> span() const noexcept {
        return std::span<const std::byte>(buffer_.data(), buffer_.size());
    }
    [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }

private:
    std::vector<std::byte> buffer_;
};

class ByteReader {
public:
    explicit ByteReader(std::span<const std::byte> bytes,
                        ErrorCode failure = ErrorCode::ProtocolError) noexcept;

    Status u8(std::uint8_t& out);
    Status u16(std::uint16_t& out);
    Status u32(std::uint32_t& out);
    Status u64(std::uint64_t& out);
    Status boolean(bool& out);
    Status raw(std::size_t count, std::span<const std::byte>& out);
    Status str(std::string& out, std::uint32_t max_length);
    Status count(std::uint32_t& out, std::uint32_t max_count);
    Status skip(std::size_t count);
    Status require_end() const;

    [[nodiscard]] bool at_end() const noexcept { return offset_ == bytes_.size(); }
    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - offset_; }
    [[nodiscard]] std::size_t position() const noexcept { return offset_; }
    [[nodiscard]] ErrorCode failure_code() const noexcept { return failure_; }

private:
    Status fail(std::string_view what) const;

    std::span<const std::byte> bytes_;
    std::size_t offset_ = 0;
    ErrorCode failure_;
};

}  // namespace lab_scheduler
