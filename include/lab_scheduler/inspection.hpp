#pragma once

#include <string>
#include <vector>

#include "lab_scheduler/durable.hpp"
#include "lab_scheduler/engine.hpp"
#include "lab_scheduler/placement.hpp"
#include "lab_scheduler/request.hpp"
#include "lab_scheduler/resource.hpp"

namespace lab_scheduler {

// Deterministic text rendering for the inspection CLI. Every renderer walks a
// canonically ordered input (the engine keeps its inventories in identity
// order) and prints a fixed field order, so two runs over the same state
// produce byte identical output.
std::string render_resources(const std::vector<ResourceRecord>& resources);
std::string render_resource_detail(const ResourceRecord& resource);
std::string render_requests(const std::vector<StoredRequest>& requests);
std::string render_request_detail(const StoredRequest& stored);
std::string render_placements(const std::vector<PlacementRecord>& placements);
std::string render_placement_detail(const PlacementRecord& placement, const ReservationRecord* reservation);
std::string render_reservations(const std::vector<ReservationRecord>& reservations);
std::string render_stale_resources(const std::vector<ResourceRecord>& resources);
std::string render_workers(const std::vector<WorkerRecord>& workers);
std::string render_accounting(const AccountingReport& report);
std::string render_invariants(const InvariantReport& report);
std::string render_audit(const std::vector<AuditRecord>& audit);
std::string render_epoch(CoordinatorEpoch epoch);
std::string render_durable_state_validation(const DurableState& state);

std::string describe_capabilities(const CapabilitySet& capabilities);
std::string describe_topology(const TopologyDescriptor& topology);
std::string describe_locality(const LocalityDescriptor& locality);

}  // namespace lab_scheduler
