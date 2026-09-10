#pragma once

// Internal record encoders shared by the persistence container and the wire
// protocol. Both boundaries therefore validate exactly the same bytes with the
// same bounds, and a record that survives persistence also survives transport.

#include <cstdint>
#include <string>

#include "lab_scheduler/codec.hpp"
#include "lab_scheduler/durable.hpp"
#include "lab_scheduler/engine.hpp"
#include "lab_scheduler/limits.hpp"
#include "lab_scheduler/placement.hpp"
#include "lab_scheduler/request.hpp"
#include "lab_scheduler/resource.hpp"

namespace lab_scheduler {
namespace record_codec {

template <class Id>
void write_id(ByteWriter& writer, const Id& id) {
    writer.u64(id.value());
}

template <class Id>
Status read_id(ByteReader& reader, Id& id) {
    std::uint64_t value = 0;
    Status status = reader.u64(value);
    if (!status.ok()) {
        return status;
    }
    id = Id::from_value(value);
    return Status{};
}

Status read_string(ByteReader& reader, std::string& text, std::uint32_t maximum);

void write_limits(ByteWriter& writer, const Limits& limits);
Status read_limits(ByteReader& reader, Limits& limits);
void write_weights(ByteWriter& writer, const RankingWeights& weights);
Status read_weights(ByteReader& reader, RankingWeights& weights);

void write_resource(ByteWriter& writer, const ResourceRecord& resource, const Limits& limits);
Status read_resource(ByteReader& reader, ResourceRecord& resource, const Limits& limits);
void write_advertisement(ByteWriter& writer, const ResourceAdvertisement& advertisement, const Limits& limits);
Status read_advertisement(ByteReader& reader, ResourceAdvertisement& advertisement, const Limits& limits);
ResourceRecord advertisement_to_record(const ResourceAdvertisement& advertisement);
ResourceAdvertisement record_to_advertisement(const ResourceRecord& record);

void write_request(ByteWriter& writer, const ScheduleRequest& request, const Limits& limits);
Status read_request(ByteReader& reader, ScheduleRequest& request, const Limits& limits);
void write_stored_request(ByteWriter& writer, const StoredRequest& stored, const Limits& limits);
Status read_stored_request(ByteReader& reader, StoredRequest& stored, const Limits& limits);

void write_selected(ByteWriter& writer, const SelectedResource& selected);
Status read_selected(ByteReader& reader, SelectedResource& selected, const Limits& limits);
void write_placement(ByteWriter& writer, const PlacementRecord& placement);
Status read_placement(ByteReader& reader, PlacementRecord& placement, const Limits& limits);
void write_reservation(ByteWriter& writer, const ReservationRecord& reservation);
Status read_reservation(ByteReader& reader, ReservationRecord& reservation, const Limits& limits);

void write_authority_envelope(ByteWriter& writer, const AuthorityEnvelope& authority);
Status read_authority_envelope(ByteReader& reader, AuthorityEnvelope& authority, const Limits& limits);
void write_completion_claim(ByteWriter& writer, const CompletionClaim& claim);
Status read_completion_claim(ByteReader& reader, CompletionClaim& claim, const Limits& limits);

}  // namespace record_codec
}  // namespace lab_scheduler
