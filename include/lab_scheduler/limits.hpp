#pragma once

#include <cstdint>

namespace lab_scheduler {

// Every bounded surface in the runtime is declared here once so that the
// coordinator, the codec, the persistence layer, and the tests agree on the
// same limits. Nothing in the runtime allocates based on an untrusted count
// without checking it against one of these bounds first.
struct Limits {
    // Resource inventory.
    std::uint32_t max_resources = 4096;
    std::uint32_t max_capabilities_per_resource = 64;
    std::uint32_t max_accelerators_per_resource = 8;
    std::uint32_t max_local_datasets_per_resource = 32;
    std::uint32_t max_resident_models_per_resource = 32;
    std::uint32_t max_simulator_scenarios = 64;

    // Scheduling requests.
    std::uint32_t max_retained_requests = 4096;
    std::uint32_t max_active_requests = 1024;
    std::uint32_t max_requirements_per_request = 16;
    std::uint32_t max_allowlist_entries = 64;
    std::uint32_t max_denylist_entries = 64;
    std::uint32_t max_anti_affinity_pairs = 16;
    std::uint32_t max_affinity_groups = 8;

    // Candidate construction and ranking.
    std::uint32_t max_candidate_resources_per_requirement = 512;
    std::uint32_t max_candidates_evaluated = 20000;
    std::uint32_t max_candidates_ranked = 256;
    std::uint32_t max_rejected_entries_per_requirement = 16;
    std::uint32_t max_rejection_reasons_per_entry = 8;
    std::uint32_t max_ranking_factors = 16;

    // Placements, reservations, history.
    std::uint32_t max_placements = 8192;
    std::uint32_t max_active_placements = 1024;
    std::uint32_t max_reservations = 8192;
    std::uint32_t max_active_reservations = 4096;
    std::uint32_t max_placement_history_per_unit = 32;
    std::uint32_t max_audit_records = 4096;

    // Topology and locality.
    std::uint32_t max_topology_records = 4096;
    std::uint32_t max_topology_depth = 8;

    // Text.
    std::uint32_t max_string_length = 256;
    std::uint32_t max_digest_length = 128;
    std::uint32_t max_explanation_entries = 4096;

    // Transport.
    std::uint32_t max_frame_size = 1u << 20;  // 1 MiB
    std::uint32_t max_worker_connections = 64;
    std::uint32_t max_inflight_assignments = 256;

    // Persistence.
    std::uint32_t max_persistence_records = 65536;
    std::uint64_t max_persistence_bytes = 64ull << 20;  // 64 MiB

    // Scheduling retry policy on generation races (bounded, not time based).
    std::uint32_t max_placement_commit_attempts = 4;
};

}  // namespace lab_scheduler
