#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "lab_scheduler/error.hpp"
#include "lab_scheduler/identity.hpp"
#include "lab_scheduler/resource.hpp"

namespace lab_scheduler {

// Reference lab profiles used by the coordinator/worker architecture, the
// examples, and the multiprocess proof. Resource identities are fixed so that
// the same logical resource keeps its identity across worker restarts, while
// the worker boot identity changes on every process incarnation.
//
// Provenance is explicit and honest:
//   REAL      the resource represents something that exists on the host that
//             advertises it (a real GPU discovered through CUDA, real local
//             files, a real process).
//   SYNTHETIC the resource is a lab resource the runtime models but does not
//             physically own (simulators, model endpoints, datasets, physical
//             test rigs).
//   UNSUPPORTED the surface exists but is not available in this environment.
struct ProfileBlueprint {
    std::string name{};
    WorkerId worker{};
    WorkerBootId boot{};
    std::vector<ResourceAdvertisement> resources{};
};

// Well known reference resource identities.
inline constexpr std::uint64_t kAlphaGpuResource = 0x1001;
inline constexpr std::uint64_t kAlphaModelResource = 0x1002;
inline constexpr std::uint64_t kAlphaDatasetResource = 0x1003;
inline constexpr std::uint64_t kBetaGpuResource = 0x2001;
inline constexpr std::uint64_t kBetaSimulatorResource = 0x2002;
inline constexpr std::uint64_t kBetaDatasetResource = 0x2003;
inline constexpr std::uint64_t kGammaVirtualEnvironmentResource = 0x3001;
inline constexpr std::uint64_t kGammaPhysicalRigResource = 0x3002;

inline constexpr std::uint64_t kAlphaWorkerId = 0x11;
inline constexpr std::uint64_t kBetaWorkerId = 0x12;
inline constexpr std::uint64_t kGammaWorkerId = 0x13;

inline constexpr std::uint64_t kTrainingModelId = 0x1f01;
inline constexpr std::uint64_t kGridSimulatorId = 0x1f02;
inline constexpr std::uint64_t kCorpusDatasetId = 0x1f03;
inline constexpr std::uint64_t kCorpusDatasetVersion1 = 0x2f01;
inline constexpr std::uint64_t kCorpusDatasetVersion2 = 0x2f02;
inline constexpr std::uint64_t kRoboticsEnvironmentId = 0x1f04;
inline constexpr std::uint64_t kRoboticsVirtualEnvironmentId = 0x3f01;
inline constexpr std::uint64_t kRoboticsPhysicalEnvironmentId = 0x3f02;

std::vector<std::string> profile_names();

// Builds the resource advertisement set for one reference profile. The
// occupancy argument lets a restarted worker publish a different current
// occupancy than the one it registered with.
Result<ProfileBlueprint> make_profile(const std::string& name, WorkerBootId boot,
                                      std::uint32_t current_occupancy = 0);

}  // namespace lab_scheduler
