# Lab Scheduler

Lab Scheduler is an open-source, vendor-neutral C++20 runtime that decides
**where experiment work may run** across heterogeneous lab resources: models,
GPUs, simulators, datasets, physical test environments, virtual environments
and the workers that host them.

Governing systems question:

> Where should this experiment run, on which resources and environment, under
> what constraints and authority, and how should placement adapt when
> availability, capability, topology, or failure changes?

The answer Lab Scheduler gives is that placement is **governed authority**, not
resource discovery. A resource being present does not make it eligible. A GPU
being idle does not make it the right GPU. A dataset being reachable does not
mean its current version or locality is acceptable. Every placement derives
from explicit current evidence, is bound to a named authority envelope, and
fails closed when that evidence is stale, revoked, or superseded.

## Systems boundary

Lab Scheduler owns:

- schedulable experiment identity and reference (experiment, experiment
  generation, trial, scheduling request identity);
- scheduling request identity and placement generation;
- resource inventory relevant to experiment placement;
- resource class, capability advertisement, health, readiness and occupancy as
  scheduling inputs;
- model-resource, GPU/accelerator, simulator, dataset, physical-environment,
  virtual-environment and worker identity where modeled;
- topology and locality as supply-side scheduling evidence;
- capacity and occupancy accounting for the scheduling surface it owns;
- reservations and leases local to this scheduling boundary;
- placement constraints (hard) and preferences (ranking);
- affinity, anti-affinity and exclusion;
- candidate-set construction, whole-placement validation, deterministic
  ranking and stable tie-breaking;
- placement decisions, assignment authority, reassignment, cancellation and
  placement-scoped preemption;
- stale placement rejection, coordinator epoch, worker boot incarnation and
  placement generation fencing;
- scheduling explanations;
- persistence, recovery and deterministic fail-closed decisions;
- real multiprocess placement, resource-loss and coordinator-restart proofs.

Lab Scheduler does **not** own:

- hypotheses, experiment branch semantics, metric interpretation, experiment
  acceptance or rollback, or experiment lineage as a whole;
- global agent lifecycle or persistent agent scheduling;
- model routing policy across general inference traffic, model serving, token
  generation, or model loading internals;
- GPU memory allocation internals or low-level accelerator partitioning;
- general-purpose cluster scheduling or global resource arbitration across all
  infrastructure;
- dataset storage, simulator internals, physical lab device control or
  low-level safety control;
- artifact promotion, research provenance across unrelated experiments, or
  production deployment;
- arbitrary workflow dependency graphs or arbitrary policy-language execution.

Adjacent boundaries:

| Adjacent runtime | What it governs | What Lab Scheduler governs instead |
| --- | --- | --- |
| Experiment Fabric | the experiment itself: hypotheses, branches, trials, artifacts, metrics, rollback, lineage | where that experiment's eligible work may run |
| Resource Broker | broader reservation and arbitration across infrastructure resources | placement of experiment work on lab resources |
| Agent Scheduler | persistent autonomous workers | worker/resource selection for one schedulable unit |
| Research Ledger | broader research provenance and accounting | placement evidence and placement accounting |
| Artifact Promotion | trust elevation of produced outputs | nothing: produced outputs are opaque to placement |
| Autonomous Foundry | higher-order populations of autonomous research/coding workers | individual placement decisions |

Lab Scheduler consumes authoritative state produced elsewhere (health,
eligibility, calibration, budgets) and never derives it. In particular it does
not reimplement Topology Fabric, NUMA Fabric, PCIe Fabric, Rack Fabric or
Cluster Fabric: topology is consumed as evidence, and a domain that was not
supplied stays UNKNOWN and can never satisfy a topology requirement.

## Architecture

    client / controller            worker / resource agents
            |                                |
            +---------- framed loopback TCP -+
                             |
                    Lab Scheduler coordinator
                    (placement authority owner)

- lab-scheduler-coordinator is a real process that owns one SchedulerEngine,
  serves the framed protocol over loopback TCP, persists durable state after
  every accepted mutation, and recovers that state (with a higher coordinator
  epoch and revoked live authority) when restarted.
- lab-scheduler-worker is a real process that owns a logical worker identity
  that survives restart and a boot identity that does not. It registers or
  republishes its resources, executes assignments under the authority envelope
  it is given, and reports completion.
- lab-scheduler-cli inspects a running coordinator or validates a persisted
  state file offline.
- lab-scheduler-cluster-proof drives the whole reference architecture as
  independent operating system processes, including real process death and a
  real coordinator restart.

Inside the coordinator, one coherent state machine guarded by one mutex owns
every mutation. No callback, network send, or persistence write is ever
performed under the scheduler state lock: responses are built from copies,
outbound frames are sent after the lock is released, and persistence is
serialized separately. Readers receive snapshots, never references into
mutable containers.

### Concurrency and lock discipline

- Lock order is fixed: scheduler state, then persistence, then peer write
  locks. Peer write locks are never taken while the scheduler state lock is
  held.
- No read-lock to write-lock upgrade path exists: readers take copies.
- Registration, publication, scheduling, completion, cancellation,
  reassignment, inspection and persistence never re-enter the engine.
- Connection handling runs on bounded threads (bounded by the configured
  maximum connection count), never one thread per request.
- Shutdown wakes the accept loop deterministically, closes peers so their
  threads can end, and only then joins them; no join happens while holding a
  lock a connection thread needs.

## Resource model

Heterogeneous resources are modeled explicitly, not as scalar capacity
buckets. A resource record carries:

- stable logical identity (ResourceId) and, for dynamic resources, the worker
  and boot identity of the incarnation that advertised it;
- resource class: MODEL, GPU, SIMULATOR, DATASET, PHYSICAL_ENVIRONMENT,
  VIRTUAL_ENVIRONMENT, WORKER;
- capabilities (canonical, content-addressed, order independent);
- capacity (slots, memory bytes, sessions) and advertised occupancy;
- health and readiness as consumed inputs, each with its own generation;
- topology domains (host, NUMA, root complex, rack, cluster) and locality
  (local dataset versions, resident models, co-located simulators);
- attached accelerators with real device properties when the resource is real;
- sharing mode (shared or exclusive), tenant, provenance and labels;
- a monotonic resource generation, the epoch in which its state was validated,
  and an explicit dynamic-authority flag with the reason currentness is (or is
  not) proven;
- reserved capacity mirrored from the reservation ledger.

Class specific descriptors are validated: a MODEL resource must carry a model
identity and version, a GPU resource must carry at least one accelerator, a
DATASET resource must carry dataset identity and version, and physical and
virtual environments must carry their environment identity and, for virtual
environments, an image digest.

Some resources are exclusive, some shareable, some are locality anchors, some
are capabilities rather than capacity, and some are compound. All of that is
represented explicitly so that false placement is impossible by construction.

## Scheduling request model

A scheduling request is domain neutral: it never contains scientific
semantics. It carries experiment identity and generation, an optional trial,
opaque workload and tenant labels, priority, one or more resource
requirements, affinity and anti-affinity constraints, exclusivity, an optional
reproducibility requirement, a reassignment budget, ranking weights and caller
authority/provenance labels.

A requirement may bind: resource class, required capabilities, minimum slots,
minimum memory, exclusivity, evidence currency, minimum health, maximum
occupancy, an accelerator identity, a model identity and version (and whether
the model must be resident), a simulator identity, version and scenario, a
dataset identity and exact version, an environment identity, an image digest,
a physical or virtual environment identity, a pinned topology domain, an
allowlist, a denylist, and an affinity group.

### Compound placement

An experiment may require several resource classes at once. Lab Scheduler never
picks each required resource independently, because independently valid choices
can still compose into an invalid placement:

    model family + GPU class + dataset version
    two model resources + one simulator + one dataset
    physical test rig + attached accelerator
    virtual environment + pinned runtime image
    GPU + dataset locality + simulator compatibility
    minimum GPU memory + exact model version
    same-host affinity between simulator and accelerator
    exclusive access to a physical environment

Every requirement is filtered independently (hard constraints), then whole
placements are constructed and validated as a unit (cross-requirement
constraints), then ranked.

### Hard constraints before ranking

Hard constraints are evaluated first and ranking can never override a failed
hard constraint: resource class, required capability, minimum capacity,
minimum memory, model and version, simulator version and scenario, dataset
identity and version, environment identity and digest, physical and virtual
environment identity, pinned topology domain, health threshold, current
evidence, tenant isolation, allowlist, denylist, exclusivity, occupancy
ceiling, environment readiness and current reservation compatibility.

When no valid placement exists the runtime returns a typed no-placement
outcome with an explanation of every rejection. It never selects the least bad
invalid resource. The reported failure code names the most informative
blocking condition: a resource that matches the requirement but whose evidence
is stale is reported as a staleness failure rather than as an unrelated
mismatch.

### Ranking

Ranking runs only over placements that already satisfy every hard constraint.
Twelve named factors are reported individually, never collapsed into one
opaque score:

| Factor | Meaning |
| --- | --- |
| locality | how concentrated the placement is on one host |
| occupancy | resulting occupancy of the selected resources |
| residual_capacity | headroom left after the placement |
| accelerator_fit | closeness of the accelerator to the requested memory |
| model_warmth | model residency and readiness |
| dataset_locality | dataset co-location with the compute resources |
| simulator_readiness | simulator readiness state |
| topology_distance | spread across hosts, racks and clusters |
| fragmentation | whether the placement splits residual capacity |
| reuse | reuse of resources already reserved by the same experiment |
| fairness | service count of the requesting tenant |
| priority | request priority |

Scores are computed in exact integer fixed point; no floating point takes part
in a placement decision. Ties are broken by the canonical ascending resource
identity vector, so identical state and policy always produce an identical
placement, independent of registration order.

Candidate sets are bounded and the search is pruned with an admissible bound.
Candidates that are interchangeable on every ranking factor and every
constraint (same score, same topology domains, same ownership, same claim
shape) collapse to the single identity that would win the tie-break, because
exploring the others cannot change the outcome. When the exploration bound
truncates the search, the explanation says so explicitly.

### Placement authority

A placement is bound to an authority envelope:

    ScheduleRequestId, ExperimentId, ExperimentGeneration,
    PlacementId, PlacementGeneration, CoordinatorEpoch,
    LeaseId, ReservationId, WorkerId, WorkerBootId, and the selected
    resources with the resource generation observed at selection time

Before a placement result or completion can mutate state, authority is
validated in a fixed order: coordinator epoch, experiment identity, experiment
generation, placement generation, lifecycle state, worker boot incarnation,
resource identity, resource generation, reservation state, duplicate
completion. Anything stale is rejected with a specific code (STALE_EPOCH,
STALE_EXPERIMENT, STALE_PLACEMENT, STALE_WORKER, STALE_RESOURCE,
RESERVATION_NOT_ACTIVE, DUPLICATE_COMPLETION, CANCELLED, PREEMPTED,
REASSIGNMENT_REQUIRED).

Exactly one placement may be current for one schedulable unit (experiment,
experiment generation, trial). Duplicate delivery is harmless: a repeated
completion is detected and rejected without mutating state or releasing
capacity twice.
## Reservations and accounting

A committed placement owns a reservation with an explicit lease identity,
a claim per selected resource (slots, exclusivity, generation at acquire), and
a lifecycle state (ACTIVE, RELEASED, REVOKED). Reservations never expire by
wall clock: lifetime is an explicit state transition, which keeps correctness
independent of timing and testable without timeouts.

Accounting invariants, verified by the runtime and asserted by the tests:

- capacity is never negative and never exceeds the configured total;
- a resource's mirrored reserved capacity always equals the sum of its active
  claims;
- an exclusive resource has at most one active claim;
- cancellation releases exactly once;
- failure releases or quarantines exactly once;
- reassignment does not duplicate reservations;
- stale completion cannot release current capacity;
- stale worker traffic cannot mutate capacity;
- coordinator restart does not double count prior occupancy;
- duplicate reservation release is rejected deterministically and is harmless;
- every live placement references an active reservation with a matching
  generation, and every active reservation backs a live placement.

Reservations are committed under the same lock that produced the decision, so
a resource generation can never change between eligibility and commit.

## Resource loss, reassignment, cancellation, preemption

**Resource loss.** When a worker process dies the coordinator observes the end
of its connection and revokes everything that incarnation held: its resources
stop being authoritative, its generations advance, its health and readiness
fall back to UNKNOWN, live placements that reference those resources become
REASSIGNMENT_REQUIRED, and their reservations are revoked exactly once. A
logical resource keeps its identity; its boot incarnation does not. A fresh
registration with a new boot identity is required before the resource becomes
eligible again, and a repeated loss report is idempotent.

**Reassignment.** Reassignment creates a fresh placement identity with an
incremented placement generation, revokes the previous placement, preserves
placement history, keeps the experiment identity, binds fresh resource
evidence, and prevents the old completion from ever committing. Reassignment
is not an in-place resource edit, and it is bounded by the request's
reassignment budget.

**Cancellation.** Cancelling a placement or a request stops new placement,
revokes live authority, releases owned reservations exactly once, rejects late
completion, preserves history, and leaves capacity accounting valid. A second
cancellation of the same placement is deterministic and changes nothing.

**Preemption.** Preemption exists only within this placement boundary:
a placement may be revoked to free a constrained or exclusive lab resource.
Preempted work becomes PREEMPTED or REASSIGNMENT_REQUIRED, prior placement
authority is fenced, and a replacement placement is scheduled only when the
request permits it. Lab Scheduler does not implement infrastructure-wide
preemption economics.

## Topology and locality

Topology is supply-side evidence: host, NUMA domain, root complex, rack and
cluster domains, plus dataset locality, model residency and simulator
co-location. Affinity and anti-affinity constraints are expressed over these
domains. UNKNOWN and UNSUPPORTED are explicit states: two UNKNOWN domains are
never "the same domain", so affinity fails closed, and two UNKNOWN domains are
never proven disjoint, so anti-affinity also fails closed. Locality that was
not measured or supplied is never inferred.

## Model, GPU, simulator, dataset and environment semantics

- **Model.** A model resource represents a local model process, a serving
  endpoint, a model instance or a capability slot. Requirements may bind model
  identity, version, residency and capabilities. Lab Scheduler decides whether
  and where an experiment may run given model-resource requirements; it does
  not route general inference traffic.
- **GPU.** A GPU resource carries accelerator identity, memory and compute
  capability. Requirements may bind accelerator identity, minimum memory and
  capabilities. Placement decides which GPU, from current evidence.
- **Simulator.** A simulator is an explicit schedulable resource with identity,
  version, supported scenarios, session capacity, readiness, host and, when
  process backed, an incarnation.
- **Dataset.** A dataset is a schedulable dependency with identity, exact
  version, optional content digest, locality and access capability. A placement
  that requires version X never silently accepts version Y.
- **Physical environment.** Physical test environments (robotics cell,
  instrument bench, hardware rack, vehicle rig, sensor environment,
  accelerator validation machine) may require exclusive access, readiness,
  calibration generation, attached devices and an opaque eligibility token that
  Lab Scheduler consumes and never derives. Lab Scheduler does not implement
  physical device safety control.
- **Virtual environment.** Containers, VMs, sandboxes and runtime images are
  modeled with identity, version or digest, capabilities, readiness and host
  compatibility. Lab Scheduler does not build a container runtime or VM
  manager.

## Explanations

Every decision is explainable. An explanation contains the scheduling request
and experiment generation, the required resource classes, the resources
considered per requirement, the exact hard-constraint rejections per resource,
the eligible set size, the ranked candidates with every named factor, its
weight and its weighted contribution, the stable tie-breaker, the selected
resources with their generations, the authority envelope, and the final
outcome. NO_PLACEMENT is explained with its category: no matching capability,
stale evidence, insufficient capacity, topology mismatch, model mismatch,
dataset mismatch, environment unavailable, health rejection, exclusivity
conflict, reservation conflict, unsupported requirement.

Ordering is deterministic: identical state produces byte identical output.

## Persistence and recovery

Durable state is written in a versioned container with a magic header, an
explicit payload length, an integrity checksum over the payload, and a bounded
self-describing payload. The decoder rejects a bad magic, an unsupported
format version, an implausible payload length, a truncated payload, a checksum
mismatch, trailing bytes, a count above the configured bound, an impossible
enum value, a non-canonical boolean, duplicate identities, invalid references,
inconsistent capacity mirrors and absurd string lengths. Writes go through a
temporary file in the same directory and an atomic replace, so a partial write
can never become authoritative, and concurrent saves are serialized.

Persisted state contains the coordinator epoch, policy, id counter, logical
resource identities with their static capability records, scheduling requests,
placement history and the reservation ledger. It deliberately does not restore
dynamic availability as current.

On coordinator restart:

- the coordinator epoch is advanced;
- every worker-owned resource becomes non-authoritative with the reason
  recorded, and must be revalidated by a fresh advertisement;
- live placements are reconciled to REASSIGNMENT_REQUIRED;
- every active reservation is revoked and every capacity mirror is booked back
  to zero, so prior occupancy can never be double counted;
- placement and request history is preserved;
- pre-restart traffic is rejected as STALE_EPOCH;
- work is scheduled again only after resources are authoritative again.

## Real multiprocess proof

lab-scheduler-cluster-proof starts a real coordinator process and three real
worker processes (alpha: accelerator + model + dataset version 1, beta:
exclusive accelerator + simulator + dataset version 2, gamma: virtual
environment + synthetic physical test rig), drives them over loopback TCP, and
asserts 106 checks:

- a compound placement binds an accelerator, a model and one exact dataset
  version on one host, with same-host affinity;
- the wrong dataset version and the wrong capability appear as exact hard
  constraint rejections in the explanation;
- named ranking factors are exposed in the explanation;
- ranking is deterministic for identical state across two different requests;
- capacity accounting returns to baseline after completion;
- an idle worker process is terminated and restarted with a fresh boot
  identity;
- a live placement is held by a worker process that is then killed with real
  OS process termination: the coordinator revokes the incarnation, marks the
  placement REASSIGNMENT_REQUIRED, revokes the reservation, lists the resources
  as not current, and rejects a replayed completion from the dead incarnation;
- a request that could only be satisfied by the dead incarnation's resources
  fails with STALE_RESOURCE;
- the same logical worker restarts with a new boot identity, re-registers,
  becomes eligible, is placed, and completes a fresh placement, while the old
  placement still cannot commit;
- a duplicate completion is detected as harmless;
- cancellation releases capacity exactly once and rejects a late completion;
- an exclusive synthetic physical rig cannot be double booked;
- the coordinator process is killed with a live held placement; a fresh
  coordinator process recovers the persisted state, advances the epoch,
  preserves history, requires resource revalidation, reconciles the placement,
  rejects the pre-restart completion as STALE_EPOCH, refuses to schedule until
  workers re-register, and then completes fresh work with clean accounting;
- five malformed frame families (unknown message type, checksum mismatch,
  oversized declared length, truncated frame, trailing bytes) are rejected
  while the coordinator keeps serving;
- persisted state reloads and validates, and the proof scratch directory is
  removed.

## Provenance: REAL, SYNTHETIC, UNSUPPORTED

- **REAL** is used for actual operating system processes, real loopback TCP,
  real filesystem persistence, actual CUDA device discovery, real CUDA
  execution and actual local files.
- **SYNTHETIC** is used for model endpoints, simulators, dataset metadata,
  physical test environments, synthetic capacity and topology, and synthetic
  experiment workloads. The reference lab profiles are synthetic.
- **UNSUPPORTED** is used for surfaces that are not available in this
  environment. When no CUDA device is visible the accelerator proof prints
  UNSUPPORTED and exits successfully; the core release does not depend on it.

No claim in this repository describes robotics hardware, physical lab
equipment, multi-node clusters, RDMA, NVLink, NVSwitch, MIG, SmartNIC/DPU,
cloud schedulers, external model APIs or production Kubernetes integration,
because none of those were exercised.

## CUDA proof

When a CUDA toolkit and a visible device are present, lab-scheduler-cuda-proof:

1. discovers the real device through the CUDA runtime and reads its real
   properties (name, total memory, compute capability, multiprocessor count);
2. advertises it as a REAL GPU resource with those properties;
3. submits an experiment request requiring CUDA capability and real memory;
4. takes the placement the scheduler chose from that evidence (the device is
   never hard-wired);
5. allocates device memory, transfers input, launches a vector-add kernel,
   synchronizes, copies results back and verifies exact parity;
6. reports completion under the placement authority envelope;
7. verifies that reservations and capacity return to baseline and that the
   runtime invariants hold.

Measured on this machine: NVIDIA GeForce RTX 5090, 34162016256 bytes of device
memory, compute capability 12.0, 170 multiprocessors; the scheduler selected
device 0, the kernel verified parity on 262144 elements, completion was
accepted, and reservations returned to zero.

## Examples

lab-scheduler-examples runs one named scenario per invocation and prints
deterministic text:

| Example | Shows |
| --- | --- |
| model-gpu | compound placement of a model endpoint and an accelerator |
| simulator-dataset | simulator plus exact dataset version, and the rejection of a wrong version |
| dataset-locality | same-host affinity between accelerator and dataset version |
| topology-aware | a pinned topology domain selecting a specific host |
| exclusive-physical | synthetic physical rig scheduled exclusively, then a conflict |
| virtual-environment | pinned runtime image digest, and a digest mismatch |
| no-placement | full explanation of every rejection category |
| cancellation | authority revocation and capacity return to baseline |
| resource-loss | incarnation loss invalidating evidence and placements |
| coordinator-restart | epoch advance and stale-authority rejection |
| explanation | named ranking factors for a decision |

The "most available resource loses" and "seemingly valid resource is rejected
because its evidence is stale" cases are covered by no-placement,
resource-loss and the multiprocess proof; compound placement across several
resource classes is covered by model-gpu, simulator-dataset and the
multiprocess proof.

## Benchmarks

lab-scheduler-bench measures completed scheduling work, not enqueue cost, and
reports operations, total time, throughput and microseconds per operation at
128 registered resources (8 workers, 4 topology domains) with 64 single
requirement placements and 32 compound placements of four requirements.

Representative measurement from this machine (Windows, x64 MSVC Release):

    resource_registration           128 ops      628375.1/s      1.591 us/op
    resource_state_publication      512 ops      895261.4/s      1.117 us/op
    single_requirement_placement     64 ops       37503.7/s     26.664 us/op
    completed_placements             64 ops       35868.4/s     27.880 us/op
    compound_placement               32 ops         127.8/s   7823.834 us/op
    completed_placements             32 ops         127.8/s   7825.716 us/op
    explanation_generation           32 ops      458452.7/s      2.181 us/op
    snapshot_and_accounting_read    128 ops       13136.6/s     76.123 us/op
    persistence_save_and_load        16 ops        1060.4/s    943.013 us/op

Compound placement improved by 5.2x (40.8 ms to 7.8 ms per completed
placement) once interchangeable candidates stopped multiplying the search
space. Correctness is never weakened for throughput: the benchmark asserts
that every counted placement was validated, ranked, committed and completed.

## Build, test, install

Requirements: CMake 3.20 or newer, a C++20 compiler (MSVC 19.3x, GCC 12+,
Clang 15+), and a network stack for the reference architecture. CUDA is
optional.

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --config Release
    ctest --test-dir build -C Release --output-on-failure

Options: LAB_SCHEDULER_BUILD_TESTS, LAB_SCHEDULER_BUILD_EXAMPLES,
LAB_SCHEDULER_BUILD_BENCHMARKS, LAB_SCHEDULER_BUILD_TOOLS,
LAB_SCHEDULER_BUILD_CUDA_PROOF, LAB_SCHEDULER_ENABLE_ADDRESS_SANITIZER.

Every first-party target is compiled with strict warnings as errors
(/W4 /WX on MSVC, -Wall -Wextra -Wpedantic -Werror elsewhere). There is no
suppression list.

The test suite contains no timeouts, no watchdogs and no sleeps: a hanging
test is a defect, not a flake. Concurrency phases are launched together and
joined, races are resolved by the runtime's own ordering rules, and
asynchronous work is observed by polling state that returns immediately.

Installing exports a CMake package with namespaced targets:

    cmake --install build --prefix <prefix>
    find_package(LabScheduler REQUIRED)
    target_link_libraries(consumer PRIVATE LabScheduler::lab_scheduler)

## Inspection CLI

    lab-scheduler-cli --port <port> resources
    lab-scheduler-cli --port <port> resource <res-id>
    lab-scheduler-cli --port <port> requests | request <sreq-id>
    lab-scheduler-cli --port <port> placements | placement <plc-id>
    lab-scheduler-cli --port <port> explain <dec-id>
    lab-scheduler-cli --port <port> explain-placement <plc-id>
    lab-scheduler-cli --port <port> reservations | stale | workers | epoch
    lab-scheduler-cli --port <port> accounting | invariants | audit
    lab-scheduler-cli --port <port> save <path>
    lab-scheduler-cli --state <path> validate-state

Output ordering is deterministic: inventories are rendered in identity order
with a fixed field order.

## Known limitations

- The reference topology is a single host per worker profile; multi-node,
  RDMA, NVLink and NVSwitch behaviour is not modeled or proven here.
- Lab profiles, simulators, model endpoints, datasets and physical test
  environments are SYNTHETIC. Real hardware proof covers local CUDA devices
  only.
- Physical test environments consume an opaque eligibility token; the runtime
  does not implement device safety control.
- Decision explanations are retained in the coordinator's memory; placement,
  request and reservation history is durable, and an explanation is not
  replayed after a restart.
- Candidate exploration is bounded; when the bound truncates the search the
  explanation records it, and the selected placement is the best of the
  explored set, not a proven global optimum.
- Preemption is placement-scoped by design and has no economic policy.
- The POSIX transport and process paths are implemented but the verified
  closure in this repository was produced on Windows with MSVC; the CUDA proof
  was produced on a single local RTX 5090.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
