#include "lab_scheduler/inspection.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "lab_scheduler/canonical.hpp"

namespace lab_scheduler {
namespace {

const char* yes_no(bool value) noexcept { return value ? "yes" : "no"; }

std::string describe_domain(const TopologyDomain& domain) {
    if (domain.id.is_null()) {
        return "UNKNOWN";
    }
    std::string out = domain.id.to_string();
    if (!domain.name.empty()) {
        out += "(";
        out += domain.name;
        out += ")";
    }
    return out;
}

}  // namespace

std::string describe_capabilities(const CapabilitySet& capabilities) {
    if (capabilities.empty()) {
        return "-";
    }
    std::string out;
    const std::vector<Capability>& entries = capabilities.entries();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (i != 0) {
            out += ",";
        }
        out += entries[i].name;
    }
    return out;
}

std::string describe_topology(const TopologyDescriptor& topology) {
    if (!topology.supplied) {
        return "UNKNOWN (not supplied)";
    }
    std::string out = "host=";
    out += describe_domain(topology.host);
    out += " numa=";
    out += describe_domain(topology.numa);
    out += " root_complex=";
    out += describe_domain(topology.root_complex);
    out += " rack=";
    out += describe_domain(topology.rack);
    out += " cluster=";
    out += describe_domain(topology.cluster);
    return out;
}

std::string describe_locality(const LocalityDescriptor& locality) {
    std::string out = "datasets=";
    if (locality.local_dataset_versions.empty()) {
        out += "-";
    }
    for (std::size_t i = 0; i < locality.local_dataset_versions.size(); ++i) {
        if (i != 0) {
            out += ",";
        }
        out += locality.local_dataset_versions[i].to_string();
    }
    out += " models=";
    if (locality.resident_models.empty()) {
        out += "-";
    }
    for (std::size_t i = 0; i < locality.resident_models.size(); ++i) {
        if (i != 0) {
            out += ",";
        }
        out += locality.resident_models[i].to_string();
    }
    out += " simulators=";
    if (locality.colocated_simulators.empty()) {
        out += "-";
    }
    for (std::size_t i = 0; i < locality.colocated_simulators.size(); ++i) {
        if (i != 0) {
            out += ",";
        }
        out += locality.colocated_simulators[i].to_string();
    }
    return out;
}

std::string render_resources(const std::vector<ResourceRecord>& resources) {
    std::string out;
    out += "resources ";
    out += decimal_u64(resources.size());
    out += "\n";
    for (const ResourceRecord& resource : resources) {
        out += resource.id.to_string();
        out += " ";
        out += resource_class_name(resource.resource_class);
        out += " name='";
        out += resource.name;
        out += "'";
        out += " lifecycle=";
        out += resource_lifecycle_name(resource.lifecycle);
        out += " provenance=";
        out += provenance_name(resource.provenance);
        out += " sharing=";
        out += sharing_mode_name(resource.sharing);
        out += " generation=";
        out += decimal_u64(resource.generation.value());
        out += " capacity=";
        out += decimal_u64(resource.capacity.slots);
        out += " occupancy=";
        out += decimal_u64(resource.total_occupancy());
        out += " reserved=";
        out += decimal_u64(resource.reserved_slots);
        out += " health=";
        out += health_state_name(resource.health);
        out += " readiness=";
        out += readiness_state_name(resource.readiness);
        out += " current=";
        out += yes_no(resource.dynamic_authoritative);
        out += " worker=";
        out += resource.worker.is_null() ? std::string("-") : resource.worker.to_string();
        out += " boot=";
        out += resource.boot.is_null() ? std::string("-") : resource.boot.to_string();
        out += " epoch=";
        out += resource.validated_epoch.is_null() ? std::string("-") : resource.validated_epoch.to_string();
        out += "\n";
    }
    return out;
}

std::string render_resource_detail(const ResourceRecord& resource) {
    std::string out;
    out += "resource ";
    out += resource.id.to_string();
    out += "\n";
    out += "  class ";
    out += resource_class_name(resource.resource_class);
    out += "\n  name ";
    out += resource.name;
    out += "\n  lifecycle ";
    out += resource_lifecycle_name(resource.lifecycle);
    out += "\n  provenance ";
    out += provenance_name(resource.provenance);
    out += "\n  sharing ";
    out += sharing_mode_name(resource.sharing);
    out += "\n  capabilities ";
    out += describe_capabilities(resource.capabilities);
    out += "\n  capacity slots=";
    out += decimal_u64(resource.capacity.slots);
    out += " memory_bytes=";
    out += decimal_u64(resource.capacity.memory_bytes);
    out += " max_sessions=";
    out += decimal_u64(resource.capacity.max_sessions);
    out += "\n  occupancy advertised=";
    out += decimal_u64(resource.advertised_occupancy);
    out += " reserved=";
    out += decimal_u64(resource.reserved_slots);
    out += "\n  health ";
    out += health_state_name(resource.health);
    if (!resource.health_detail.empty()) {
        out += " (";
        out += resource.health_detail;
        out += ")";
    }
    out += "\n  readiness ";
    out += readiness_state_name(resource.readiness);
    out += "\n  topology ";
    out += describe_topology(resource.topology);
    out += "\n  locality ";
    out += describe_locality(resource.locality);
    out += "\n  worker ";
    out += resource.worker.is_null() ? std::string("-") : resource.worker.to_string();
    out += " boot ";
    out += resource.boot.is_null() ? std::string("-") : resource.boot.to_string();
    out += "\n  generations resource=";
    out += decimal_u64(resource.generation.value());
    out += " health=";
    out += decimal_u64(resource.health_generation.value());
    out += " capacity=";
    out += decimal_u64(resource.capacity_generation.value());
    out += "\n  dynamic_authoritative ";
    out += yes_no(resource.dynamic_authoritative);
    out += "\n  authority_detail ";
    out += resource.authority_detail.empty() ? std::string("-") : resource.authority_detail;
    out += "\n  tenant ";
    out += resource.tenant.empty() ? std::string("-") : resource.tenant;
    out += "\n";
    for (const AcceleratorAttachment& accelerator : resource.accelerators) {
        out += "  accelerator ";
        out += accelerator.id.to_string();
        out += " model='";
        out += accelerator.model;
        out += "' memory_bytes=";
        out += decimal_u64(accelerator.memory_bytes);
        out += " compute_capability=";
        out += decimal_u64(accelerator.compute_capability_major);
        out += ".";
        out += decimal_u64(accelerator.compute_capability_minor);
        out += "\n";
    }
    if (resource.model.has_value()) {
        out += "  model ";
        out += resource.model->id.to_string();
        out += " version=";
        out += resource.model->version;
        out += " context_window=";
        out += decimal_u64(resource.model->context_window);
        out += " resident=";
        out += yes_no(resource.model->resident);
        out += " tool_support=";
        out += yes_no(resource.model->tool_support);
        out += "\n";
    }
    if (resource.simulator.has_value()) {
        out += "  simulator ";
        out += resource.simulator->id.to_string();
        out += " version=";
        out += resource.simulator->version;
        out += " sessions=";
        out += decimal_u64(resource.simulator->max_sessions);
        out += " scenarios=";
        for (std::size_t i = 0; i < resource.simulator->scenarios.size(); ++i) {
            if (i != 0) {
                out += ",";
            }
            out += resource.simulator->scenarios[i];
        }
        out += "\n";
    }
    if (resource.dataset.has_value()) {
        out += "  dataset ";
        out += resource.dataset->id.to_string();
        out += " version=";
        out += resource.dataset->version.to_string();
        out += " digest=";
        out += resource.dataset->digest.empty() ? std::string("-") : resource.dataset->digest;
        out += " size_bytes=";
        out += decimal_u64(resource.dataset->size_bytes);
        out += "\n";
    }
    if (resource.environment.has_value()) {
        out += "  environment ";
        out += resource.environment->id.to_string();
        out += " image_digest=";
        out += resource.environment->image_digest.empty() ? std::string("-")
                                                          : resource.environment->image_digest;
        out += " virtual=";
        out += resource.environment->virtual_environment.is_null()
                   ? std::string("-")
                   : resource.environment->virtual_environment.to_string();
        out += " physical=";
        out += resource.environment->physical_environment.is_null()
                   ? std::string("-")
                   : resource.environment->physical_environment.to_string();
        out += "\n";
    }
    return out;
}

std::string render_requests(const std::vector<StoredRequest>& requests) {
    std::string out;
    out += "requests ";
    out += decimal_u64(requests.size());
    out += "\n";
    for (const StoredRequest& stored : requests) {
        out += stored.request.id.to_string();
        out += " state=";
        out += request_state_name(stored.state);
        out += " experiment=";
        out += stored.request.experiment.to_string();
        out += " generation=";
        out += stored.request.experiment_generation.to_string();
        out += " trial=";
        out += stored.request.trial.is_null() ? std::string("-") : stored.request.trial.to_string();
        out += " requirements=";
        out += decimal_u64(stored.request.requirements.size());
        out += " placements=";
        out += decimal_u64(stored.placement_count);
        out += " current=";
        out += stored.current_placement.is_null() ? std::string("-") : stored.current_placement.to_string();
        out += "\n";
    }
    return out;
}

std::string render_request_detail(const StoredRequest& stored) {
    const ScheduleRequest& request = stored.request;
    std::string out;
    out += "request ";
    out += request.id.to_string();
    out += "\n  state ";
    out += request_state_name(stored.state);
    out += "\n  experiment ";
    out += request.experiment.to_string();
    out += " generation ";
    out += request.experiment_generation.to_string();
    out += "\n  trial ";
    out += request.trial.is_null() ? std::string("-") : request.trial.to_string();
    out += "\n  workload ";
    out += request.workload.empty() ? std::string("-") : request.workload;
    out += "\n  tenant ";
    out += request.tenant.empty() ? std::string("-") : request.tenant;
    out += "\n  priority ";
    out += decimal_i64(request.priority);
    out += "\n  exclusive ";
    out += yes_no(request.exclusive);
    out += " experiment_anti_affinity ";
    out += yes_no(request.experiment_anti_affinity);
    out += " allow_preemption ";
    out += yes_no(request.allow_preemption);
    out += " max_reassignments ";
    out += decimal_u64(request.max_reassignments);
    out += "\n  caller_authority ";
    out += request.caller_authority.empty() ? std::string("-") : request.caller_authority;
    out += "\n";
    for (const ResourceRequirement& requirement : request.requirements) {
        out += "  requirement ";
        out += describe_requirement(requirement);
        out += " current_evidence=";
        out += yes_no(requirement.require_current_evidence);
        out += " min_health=";
        out += health_state_name(requirement.minimum_health);
        out += " max_occupancy=";
        out += decimal_u64(requirement.max_occupancy_percent);
        out += " affinity_group=";
        out += decimal_u64(requirement.affinity_group);
        out += "\n";
    }
    for (const AffinityConstraint& constraint : request.affinity) {
        out += "  affinity group ";
        out += decimal_u64(constraint.group_a);
        out += " <-> ";
        out += decimal_u64(constraint.group_b);
        out += " scope ";
        out += topology_scope_name(constraint.scope);
        out += "\n";
    }
    for (const AntiAffinityConstraint& constraint : request.anti_affinity) {
        out += "  anti_affinity group ";
        out += decimal_u64(constraint.group_a);
        out += " <-> ";
        out += decimal_u64(constraint.group_b);
        out += " scope ";
        out += topology_scope_name(constraint.scope);
        out += "\n";
    }
    return out;
}

std::string render_placements(const std::vector<PlacementRecord>& placements) {
    std::string out;
    out += "placements ";
    out += decimal_u64(placements.size());
    out += "\n";
    for (const PlacementRecord& placement : placements) {
        out += placement.id.to_string();
        out += " generation=";
        out += decimal_u64(placement.generation.value());
        out += " state=";
        out += placement_state_name(placement.state);
        out += " request=";
        out += placement.request.to_string();
        out += " experiment=";
        out += placement.experiment.to_string();
        out += " epoch=";
        out += placement.epoch.to_string();
        out += " worker=";
        out += placement.worker.is_null() ? std::string("-") : placement.worker.to_string();
        out += " boot=";
        out += placement.worker_boot.is_null() ? std::string("-") : placement.worker_boot.to_string();
        out += " reservation=";
        out += placement.reservation.is_null() ? std::string("-") : placement.reservation.to_string();
        out += " resources=";
        for (std::size_t i = 0; i < placement.resources.size(); ++i) {
            if (i != 0) {
                out += ",";
            }
            out += placement.resources[i].resource.to_string();
        }
        out += " reassignments=";
        out += decimal_u64(placement.reassignment_count);
        out += "\n";
    }
    return out;
}

std::string render_placement_detail(const PlacementRecord& placement, const ReservationRecord* reservation) {
    std::string out;
    out += "placement ";
    out += placement.id.to_string();
    out += "\n  generation ";
    out += decimal_u64(placement.generation.value());
    out += "\n  state ";
    out += placement_state_name(placement.state);
    out += "\n  request ";
    out += placement.request.to_string();
    out += "\n  decision ";
    out += placement.decision.to_string();
    out += "\n  experiment ";
    out += placement.experiment.to_string();
    out += " generation ";
    out += placement.experiment_generation.to_string();
    out += "\n  coordinator_epoch ";
    out += placement.epoch.to_string();
    out += "\n  worker ";
    out += placement.worker.is_null() ? std::string("-") : placement.worker.to_string();
    out += " boot ";
    out += placement.worker_boot.is_null() ? std::string("-") : placement.worker_boot.to_string();
    out += "\n  reassignments ";
    out += decimal_u64(placement.reassignment_count);
    out += "\n  completion_recorded ";
    out += yes_no(placement.completion_recorded);
    out += "\n  detail ";
    out += placement.detail.empty() ? std::string("-") : placement.detail;
    out += "\n";
    for (const SelectedResource& selected : placement.resources) {
        out += "  selected ";
        out += selected.resource.to_string();
        out += " class=";
        out += resource_class_name(selected.resource_class);
        out += " requirement='";
        out += selected.requirement_name;
        out += "' slots=";
        out += decimal_u64(selected.slots);
        out += " exclusive=";
        out += yes_no(selected.exclusive);
        out += " generation=";
        out += decimal_u64(selected.generation.value());
        out += " worker=";
        out += selected.worker.is_null() ? std::string("-") : selected.worker.to_string();
        out += "\n";
    }
    if (reservation != nullptr) {
        out += "  reservation ";
        out += reservation->id.to_string();
        out += " state=";
        out += reservation_state_name(reservation->state);
        out += " lease=";
        out += reservation->lease.to_string();
        out += "\n";
        for (const ReservationClaim& claim : reservation->claims) {
            out += "    claim ";
            out += claim.resource.to_string();
            out += " slots=";
            out += decimal_u64(claim.slots);
            out += " exclusive=";
            out += yes_no(claim.exclusive);
            out += " generation_at_acquire=";
            out += decimal_u64(claim.generation_at_acquire.value());
            out += "\n";
        }
    }
    return out;
}

std::string render_reservations(const std::vector<ReservationRecord>& reservations) {
    std::string out;
    out += "reservations ";
    out += decimal_u64(reservations.size());
    out += "\n";
    for (const ReservationRecord& reservation : reservations) {
        out += reservation.id.to_string();
        out += " state=";
        out += reservation_state_name(reservation.state);
        out += " placement=";
        out += reservation.placement.to_string();
        out += " generation=";
        out += decimal_u64(reservation.placement_generation.value());
        out += " epoch=";
        out += reservation.epoch.to_string();
        out += " claims=";
        out += decimal_u64(reservation.claims.size());
        std::uint64_t slots = 0;
        for (const ReservationClaim& claim : reservation.claims) {
            slots += claim.slots;
        }
        out += " slots=";
        out += decimal_u64(slots);
        out += "\n";
    }
    return out;
}

std::string render_stale_resources(const std::vector<ResourceRecord>& resources) {
    std::string out;
    out += "stale_resources ";
    out += decimal_u64(resources.size());
    out += "\n";
    for (const ResourceRecord& resource : resources) {
        out += resource.id.to_string();
        out += " lifecycle=";
        out += resource_lifecycle_name(resource.lifecycle);
        out += " current=";
        out += yes_no(resource.dynamic_authoritative);
        out += " reason=";
        out += resource.authority_detail.empty() ? std::string("-") : resource.authority_detail;
        out += "\n";
    }
    return out;
}

std::string render_workers(const std::vector<WorkerRecord>& workers) {
    std::string out;
    out += "workers ";
    out += decimal_u64(workers.size());
    out += "\n";
    for (const WorkerRecord& worker : workers) {
        out += worker.id.to_string();
        out += " boot=";
        out += worker.boot.is_null() ? std::string("-") : worker.boot.to_string();
        out += " connected=";
        out += yes_no(worker.connected);
        out += " registrations=";
        out += decimal_u64(worker.registration_sequence);
        out += " detail=";
        out += worker.detail.empty() ? std::string("-") : worker.detail;
        out += "\n";
    }
    return out;
}

std::string render_accounting(const AccountingReport& report) {
    std::string out;
    out += "accounting epoch=";
    out += report.epoch.to_string();
    out += " placements_recorded=";
    out += decimal_u64(report.placements_recorded);
    out += " active_placements=";
    out += decimal_u64(report.active_placements);
    out += " requests_recorded=";
    out += decimal_u64(report.requests_recorded);
    out += " reservations_recorded=";
    out += decimal_u64(report.reservations_recorded);
    out += " active_reservations=";
    out += decimal_u64(report.active_reservations);
    out += " total_capacity=";
    out += decimal_u64(report.total_capacity_slots);
    out += " total_reserved=";
    out += decimal_u64(report.total_reserved_slots);
    out += "\n";
    for (const ResourceAccounting& resource : report.resources) {
        out += "  ";
        out += resource.resource.to_string();
        out += " class=";
        out += resource_class_name(resource.resource_class);
        out += " capacity=";
        out += decimal_u64(resource.capacity_slots);
        out += " advertised=";
        out += decimal_u64(resource.advertised_occupancy);
        out += " reserved=";
        out += decimal_u64(resource.reserved_slots);
        out += " active_reservations=";
        out += decimal_u64(resource.active_reservations);
        out += " current=";
        out += yes_no(resource.dynamic_authoritative);
        out += "\n";
    }
    return out;
}

std::string render_invariants(const InvariantReport& report) {
    std::string out;
    out += "invariants ";
    out += report.all_ok() ? "OK" : "VIOLATED";
    out += " checks=";
    out += decimal_u64(report.checks.size());
    out += "\n";
    for (const InvariantCheck& check : report.checks) {
        out += "  ";
        out += check.ok ? "ok " : "FAILED ";
        out += check.name;
        if (!check.detail.empty()) {
            out += " :: ";
            out += check.detail;
        }
        out += "\n";
    }
    return out;
}

std::string render_audit(const std::vector<AuditRecord>& audit) {
    std::string out;
    out += "audit ";
    out += decimal_u64(audit.size());
    out += "\n";
    for (const AuditRecord& record : audit) {
        out += decimal_u64(record.sequence);
        out += " ";
        out += record.kind;
        out += " ";
        out += record.detail;
        out += "\n";
    }
    return out;
}

std::string render_epoch(CoordinatorEpoch epoch) {
    std::string out = "epoch ";
    out += epoch.to_string();
    out += "\n";
    return out;
}

std::string render_durable_state_validation(const DurableState& state) {
    std::string out;
    const Status status = validate_durable_state(state);
    out += "durable_state ";
    out += status.ok() ? "VALID" : "INVALID";
    out += "\n  format_version ";
    out += decimal_u64(state.format_version);
    out += "\n  epoch ";
    out += state.epoch.is_null() ? std::string("-") : state.epoch.to_string();
    out += "\n  resources ";
    out += decimal_u64(state.resources.size());
    out += "\n  requests ";
    out += decimal_u64(state.requests.size());
    out += "\n  placements ";
    out += decimal_u64(state.placements.size());
    out += "\n  reservations ";
    out += decimal_u64(state.reservations.size());
    out += "\n  integrity_note ";
    out += state.integrity_note.empty() ? std::string("-") : state.integrity_note;
    if (!status.ok()) {
        out += "\n  error ";
        out += status.to_string();
    }
    out += "\n";
    return out;
}

}  // namespace lab_scheduler
