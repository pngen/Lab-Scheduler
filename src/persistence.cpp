#include "lab_scheduler/persistence.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "lab_scheduler/canonical.hpp"
#include "lab_scheduler/codec.hpp"
#include "lab_scheduler/version.hpp"
#include "record_codec.hpp"

namespace lab_scheduler {

Result<std::vector<std::byte>> encode_durable_state(const DurableState& state, const Limits& limits) {
    ByteWriter payload;
    payload.u32(kPersistenceFormatVersion);
    record_codec::write_id(payload, state.epoch);
    record_codec::write_limits(payload, state.limits);
    record_codec::write_weights(payload, state.default_weights);
    payload.u64(state.id_counter);
    payload.str(std::string(kProductName) + " " + std::string(kVersionString));

    payload.u32(static_cast<std::uint32_t>(state.resources.size()));
    for (const ResourceRecord& resource : state.resources) {
        record_codec::write_resource(payload, resource, limits);
    }
    payload.u32(static_cast<std::uint32_t>(state.requests.size()));
    for (const StoredRequest& stored : state.requests) {
        record_codec::write_stored_request(payload, stored, limits);
    }
    payload.u32(static_cast<std::uint32_t>(state.placements.size()));
    for (const PlacementRecord& placement : state.placements) {
        record_codec::write_placement(payload, placement);
    }
    payload.u32(static_cast<std::uint32_t>(state.reservations.size()));
    for (const ReservationRecord& reservation : state.reservations) {
        record_codec::write_reservation(payload, reservation);
    }

    const std::span<const std::byte> body = payload.span();
    std::vector<std::byte> container;
    container.reserve(kPersistenceHeaderSize + body.size());
    for (const char c : kPersistenceMagic) {
        container.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
    ByteWriter header;
    header.u32(kPersistenceFormatVersion);
    header.u64(static_cast<std::uint64_t>(body.size()));
    header.u64(fnv1a64(body));
    const std::span<const std::byte> header_bytes = header.span();
    container.insert(container.end(), header_bytes.begin(), header_bytes.end());
    container.insert(container.end(), body.begin(), body.end());
    return container;
}

Result<DurableState> decode_durable_state(std::span<const std::byte> bytes, const Limits& limits) {
    if (bytes.size() < kPersistenceHeaderSize) {
        return Status(ErrorCode::CorruptPersistence, "persisted container is shorter than its header");
    }
    for (std::size_t i = 0; i < sizeof(kPersistenceMagic); ++i) {
        const auto expected = static_cast<std::byte>(static_cast<unsigned char>(kPersistenceMagic[i]));
        if (bytes[i] != expected) {
            return Status(ErrorCode::CorruptPersistence, "persisted container magic does not match");
        }
    }
    ByteReader header(bytes.subspan(sizeof(kPersistenceMagic)), ErrorCode::CorruptPersistence);
    std::uint32_t format = 0;
    Status status = header.u32(format);
    if (!status.ok()) {
        return status;
    }
    if (format != kPersistenceFormatVersion) {
        return Status(ErrorCode::CorruptPersistence, "unsupported persisted container format version");
    }
    std::uint64_t payload_length = 0;
    status = header.u64(payload_length);
    if (!status.ok()) {
        return status;
    }
    std::uint64_t checksum = 0;
    status = header.u64(checksum);
    if (!status.ok()) {
        return status;
    }
    if (payload_length == 0 || payload_length > limits.max_persistence_bytes) {
        return Status(ErrorCode::CorruptPersistence, "persisted payload length is implausible");
    }
    const std::size_t available = bytes.size() - kPersistenceHeaderSize;
    if (payload_length > available) {
        return Status(ErrorCode::CorruptPersistence, "persisted payload is truncated");
    }
    if (payload_length != available) {
        return Status(ErrorCode::CorruptPersistence, "persisted container carries trailing bytes");
    }
    const std::span<const std::byte> body =
        bytes.subspan(kPersistenceHeaderSize, static_cast<std::size_t>(payload_length));
    if (fnv1a64(body) != checksum) {
        return Status(ErrorCode::CorruptPersistence, "persisted payload checksum does not match");
    }

    ByteReader reader(body, ErrorCode::CorruptPersistence);
    DurableState state;
    std::uint32_t inner_format = 0;
    status = reader.u32(inner_format);
    if (!status.ok()) {
        return status;
    }
    if (inner_format != kPersistenceFormatVersion) {
        return Status(ErrorCode::CorruptPersistence, "unsupported payload format version");
    }
    status = record_codec::read_id(reader, state.epoch);
    if (!status.ok()) {
        return status;
    }
    status = record_codec::read_limits(reader, state.limits);
    if (!status.ok()) {
        return status;
    }
    status = record_codec::read_weights(reader, state.default_weights);
    if (!status.ok()) {
        return status;
    }
    status = reader.u64(state.id_counter);
    if (!status.ok()) {
        return status;
    }
    status = record_codec::read_string(reader, state.integrity_note, limits.max_string_length);
    if (!status.ok()) {
        return status;
    }
    state.format_version = inner_format;

    std::uint32_t resource_count = 0;
    status = reader.count(resource_count, limits.max_persistence_records);
    if (!status.ok()) {
        return status;
    }
    for (std::uint32_t i = 0; i < resource_count; ++i) {
        ResourceRecord resource;
        status = record_codec::read_resource(reader, resource, limits);
        if (!status.ok()) {
            return status;
        }
        state.resources.push_back(std::move(resource));
    }
    std::uint32_t request_count = 0;
    status = reader.count(request_count, limits.max_persistence_records);
    if (!status.ok()) {
        return status;
    }
    for (std::uint32_t i = 0; i < request_count; ++i) {
        StoredRequest stored;
        status = record_codec::read_stored_request(reader, stored, limits);
        if (!status.ok()) {
            return status;
        }
        state.requests.push_back(std::move(stored));
    }
    std::uint32_t placement_count = 0;
    status = reader.count(placement_count, limits.max_persistence_records);
    if (!status.ok()) {
        return status;
    }
    for (std::uint32_t i = 0; i < placement_count; ++i) {
        PlacementRecord placement;
        status = record_codec::read_placement(reader, placement, limits);
        if (!status.ok()) {
            return status;
        }
        state.placements.push_back(std::move(placement));
    }
    std::uint32_t reservation_count = 0;
    status = reader.count(reservation_count, limits.max_persistence_records);
    if (!status.ok()) {
        return status;
    }
    for (std::uint32_t i = 0; i < reservation_count; ++i) {
        ReservationRecord reservation;
        status = record_codec::read_reservation(reader, reservation, limits);
        if (!status.ok()) {
            return status;
        }
        state.reservations.push_back(std::move(reservation));
    }
    status = reader.require_end();
    if (!status.ok()) {
        return status;
    }
    return state;
}

Status save_durable_state(const DurableState& state, const std::string& path, const Limits& limits,
                          bool flush_to_disk) {
    Result<std::vector<std::byte>> encoded = encode_durable_state(state, limits);
    if (!encoded.ok()) {
        return encoded.status();
    }
    const std::string temporary = path + ".part";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            return Status(ErrorCode::IoFailure, "cannot open persistence file for writing: " + temporary);
        }
        const std::vector<std::byte>& bytes = encoded.value();
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        stream.flush();
        if (!stream) {
            return Status(ErrorCode::IoFailure, "failed to write persistence file: " + temporary);
        }
    }
    if (flush_to_disk) {
        std::FILE* file = nullptr;
        if (fopen_s(&file, temporary.c_str(), "rb") == 0 && file != nullptr) {
            static_cast<void>(std::fseek(file, 0, SEEK_END));
            static_cast<void>(std::fflush(file));
            std::fclose(file);
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        error.clear();
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            return Status(ErrorCode::IoFailure, "failed to replace persistence file: " + path);
        }
    }
    return Status{};
}

Result<DurableState> load_durable_state(const std::string& path, const Limits& limits) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        return Status(ErrorCode::IoFailure, "cannot open persistence file: " + path);
    }
    const std::streamoff size = stream.tellg();
    if (size < 0) {
        return Status(ErrorCode::IoFailure, "cannot determine persistence file size: " + path);
    }
    if (static_cast<std::uint64_t>(size) > limits.max_persistence_bytes + kPersistenceHeaderSize) {
        return Status(ErrorCode::CorruptPersistence, "persistence file exceeds the configured bound");
    }
    stream.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (size > 0) {
        stream.read(reinterpret_cast<char*>(bytes.data()), size);
        if (!stream) {
            return Status(ErrorCode::IoFailure, "failed to read persistence file: " + path);
        }
    }
    return decode_durable_state(std::span<const std::byte>(bytes.data(), bytes.size()), limits);
}

}  // namespace lab_scheduler
