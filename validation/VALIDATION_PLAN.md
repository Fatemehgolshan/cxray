# CXRay Validation Plan

## Purpose

This document defines a practical validation strategy for CXRay across CXL 1.1, 2.0, 3.0, and 3.1 configurations. The goal is not to find a single "golden" dataset, but to build confidence in the simulator through multiple validation layers:

- silicon-based calibration where hardware exists
- cross-paper envelope checks where exact hardware/config matches do not exist
- cross-model checks against external simulators and analytical bounds
- spec-derived validation for features that are newer than public silicon
- regression tests to preserve validated behavior over time

This is necessary because the simulator models subsystems with different fidelity levels. Some paths can be validated against measured hardware, while others can only be shown to be spec-consistent or trend-correct until public platforms become available.

## Scope

This plan covers:

- link latency and bandwidth
- credit flow control and serialization
- device controller overhead
- DRAM backend timing
- switch/fabric latency and queueing
- CXL.mem request/response behavior
- Type-2 coherence overhead modeling
- IDE/TSP overhead modeling
- RAS features in CXL 3.1
- NUMA/page migration behavior
- end-to-end trace replay accuracy

This plan does not claim full functional validation of:

- LTSSM and link training
- bit-accurate FLIT encoding and CRC/retry sequencing
- full CXL.io ordering semantics
- full DCOH snoop ordering
- attestation/SPDM/security protocol correctness
- OS/VMM software recovery flows for RAS

Those items should remain explicitly marked as out of scope in papers and reports.

## Validation Principles

1. Validate each subsystem with the strongest available oracle.
2. Avoid exact comparisons when configurations are materially mismatched.
3. Report confidence per feature, not just one aggregate error number.
4. Separate "silicon-validated" claims from "spec-consistent" claims.
5. Preserve both absolute accuracy and qualitative trends.

## Confidence Levels

Each feature should be tagged with one of the following confidence levels in reports and figures.

| Level | Meaning | Typical evidence |
|---|---|---|
| `SV` | Silicon-validated | Real hardware measurement with matched or closely mapped configuration |
| `CE` | Cross-paper envelope validated | Multiple public measurements define an expected range or trend |
| `CM` | Cross-model validated | Agreement with another simulator, DRAM model, or analytical oracle |
| `SC` | Spec-consistent | Derived from protocol/JEDEC timing and validated by invariants/trends |
| `F` | Functional only | Present for completeness, no strong timing claim |

## Validation Oracles

Use the following oracle types in order of preference.

### 1. Real hardware

Primary target for:

- unloaded latency
- bandwidth saturation
- bandwidth-latency curves
- host overhead
- CXL versus local DDR delta
- NUMA/page placement interactions

Representative sources already tracked in the repo:

- `validation/golden/paper_data.json`
- `cxl_docs/2303.15375v4.pdf`
- `other_cxl_sim_papers/CXL-DMSim_A_Full-System_CXL_Disaggregated_Memory_Simulator_With_Comprehensive_Silicon_Validation.pdf`

### 2. Cross-paper envelopes

Use when exact host CPU, lane width, DRAM generation, switch topology, or device implementation differ from the target configuration. Compare against bounded ranges and trend shape rather than a single value.

### 3. Independent models

Use:

- Ramulator2 and DRAMSim3 for DRAM timing
- analytical queueing bounds for switch ingress delay
- hand-derived FLIT and credit timing for link serialization and backpressure
- other CXL simulators for end-to-end trend checks

### 4. Specification and timing budgets

Use for:

- RS-FEC overhead
- IDE/TSP additive latency
- patrol scrub bandwidth tax
- sPPR and hPPR timing
- poison and viral state transitions

These validations support correctness of modeling assumptions, not proof of silicon accuracy.

## Subsystem Validation Matrix

| Subsystem | Current fidelity | Primary oracle | Secondary oracle | Confidence target | Acceptance target |
|---|---|---|---|---|---|
| Link serialization and bandwidth | timing-calibrated | real silicon | hand calculation | `SV` for CXL 1.1/2.0, `SC` for 3.0/3.1-specific deltas | unloaded latency error <= 10%; peak BW error <= 10% |
| Credit flow control | timing-calibrated | saturation/concurrency microbenchmarks | analytical/token model | `SV` or `CM` | saturation point within 1 thread step or <= 15% BW error |
| Host/device fixed overhead | timing-calibrated | pointer chase on hardware | paper envelopes | `SV` | unloaded latency delta <= 10% |
| DRAM backend | timing-calibrated | Ramulator2/DRAMSim3 | JEDEC timing budget | `CM` | row-hit/row-miss latency <= 5-10% |
| Switch latency | timing-approximate | vendor papers/whitepapers | analytical queueing | `CE` or `CM` | trend-correct; absolute error <= 20% where data exists |
| Switch queueing | timing-approximate | multi-stream saturation on switched systems | M/D/1 bound | `CM` | monotonic growth and queue-knee within 20% |
| CXL.mem end-to-end latency | timing-calibrated to approximate | hardware pointer chase | paper envelopes | `SV` for 1.1/2.0 paths | <= 10% on calibrated configs |
| Type-2 coherence overhead | timing-approximate | any published accelerator/coherence data | state-machine invariants | `CM` or `SC` | state transitions correct; overhead trend monotonic |
| IDE/TSP overhead | timing-approximate | spec timing budget | additive latency tests | `SC` | injected overhead matches config within 5% |
| RAS scrub/PPR/poison/viral/DBIT | timing-approximate | spec/whitepapers | invariant tests + synthetic traces | `SC` | event sequencing correct; bandwidth tax and delay in expected envelope |
| NUMA/page migration | functional | OS benchmarks on real NUMA/CXL systems | relative trend tests | `CE` or `F` | directionally correct placement/migration benefit |
| CXL.mem P2P / GIM | functional | future hardware | topology invariants | `SC` or `F` | address mapping and extra-hop penalties applied correctly |

## Validation Phases

## Phase 0: Baseline inventory and traceability

Create and maintain a validation inventory with one row per claim:

| Field | Description |
|---|---|
| `feature` | Modeled feature or path |
| `metric` | What is being compared |
| `config` | Simulator config and relevant knobs |
| `oracle_type` | Hardware, paper envelope, model, or spec |
| `source` | File, paper, or measurement run |
| `acceptance` | Numeric threshold or qualitative criterion |
| `confidence` | `SV`, `CE`, `CM`, `SC`, or `F` |
| `status` | pass, partial, fail, blocked |
| `notes` | assumptions and mapping caveats |

This inventory should live next to generated reports under `validation/reports/`.

## Phase 1: Reproducible baseline with existing assets

Use existing repo assets first:

- traces in `validation/traces/`
- current golden dataset in `validation/golden/paper_data.json`
- comparison script in `validation/compare.py`
- run script in `validation/run_cxray_validation.sh`

Initial baseline runs:

- `T1_seq_read.trace`
- `T2_ptr_chase.trace`
- `T3_rand_depth{1,2,4,8,16,32,64}.trace`
- `T4_seq_req{1,2,4,8,16,32,64,128}.trace`
- `T5_calib_{read_dominant,write_dominant,mixed}_pause*.trace`
- `T6_mixed_rw.trace`
- `T7_scrub_baseline.trace`
- `T8_random_ppr.trace`
- `T9_multihost_n{1,2,4,8,16}.trace`

Primary configs:

- `configs/cxl_1.1.cfg`
- `configs/cxl_2.0.cfg`
- `configs/cxl_3.0.cfg`
- `configs/cxl_3.1.cfg`
- `configs/cxl_3.1_gim2.cfg`
- `configs/cxl_3.0_numa2.cfg`

Deliverables:

- normalized JSON results
- comparison tables
- error-vs-golden reports
- a short narrative report identifying where results are silicon-calibrated versus only trend/spec validated

## Phase 2: Silicon calibration on shipping hardware

Target shipping systems that expose CXL Type-3 behavior. As of April 1, 2026, public evidence strongly supports calibrating against CXL 2.0-class deployments first; broad end-to-end CXL 3.1 machine availability remains limited in public sources.

### Hardware acquisition priorities

Priority order:

1. Intel Xeon 6 or recent SPR/EMR server with a real Type-3 memory expander
2. A second platform with different vendor device/controller behavior
3. A switched setup if available

Ideal minimum metadata to collect:

- host CPU model and stepping
- BIOS/firmware version
- CXL device vendor and part number
- CXL version exposed by platform/device
- PCIe generation and lane width
- device memory technology and channel count
- NUMA topology
- OS/kernel version and CXL driver stack
- page allocation policy and memory tiering configuration

### Hardware benchmark suite

Run the following on the real system and mirror the same access patterns in simulation.

#### Latency

- single-thread pointer chase
- pointer chase with varying dependency depth
- random load latency under no contention
- local DDR versus CXL delta

#### Bandwidth

- sequential read
- sequential write
- mixed read/write ratios
- multi-thread saturation sweep
- socket-local versus remote-socket access if applicable

#### Contention and queueing

- increasing outstanding requests
- increasing threads/cores
- mixed background traffic plus latency-sensitive foreground stream

#### NUMA and tiering

- page placement with `numactl`
- migration under pressure
- tiering/promotion if kernel support exists

#### RAS and maintenance traffic

Where possible:

- scrub-enabled versus scrub-disabled steady-state measurements
- error injection if platform tooling supports it

Realistically, many RAS features will remain spec-derived until vendor tooling is available.

### Acceptance targets for silicon calibration

For matched or near-matched hardware:

- unloaded latency: <= 10% error
- bandwidth peak: <= 10% error
- concurrency knee/saturation point: within 1 discrete step or <= 15%
- BW-latency curve: same knee region and monotonic shape; pointwise median error <= 15%

For mismatched hardware:

- compare only after remapping config and recording deviations
- report range agreement instead of exact error

## Phase 3: Cross-paper envelope validation

This phase is required because most public CXL data does not match the simulator configuration exactly.

### Normalization rules

When mapping paper data to simulation, record:

- lane width mismatch
- PCIe generation mismatch
- DRAM generation and speed mismatch
- host CPU family mismatch
- controller/device implementation mismatch
- switch presence/absence mismatch

Then classify the comparison:

- `direct`: close enough for error calculation
- `envelope`: suitable only for range/trend comparison
- `qualitative`: only trend statement is defensible

### Envelope metrics

Use envelopes for:

- unloaded latency ranges
- peak bandwidth ranges
- bandwidth fraction of local DRAM
- latency growth under load
- thread count at bandwidth saturation

Never present an envelope match as a point-accurate calibration.

## Phase 4: Cross-model validation

### DRAM

Use Ramulator2 and DRAMSim3 backends to validate:

- row-hit versus row-miss latency
- bank conflicts
- queue occupancy trends
- impact of DDR4 versus DDR5 speed changes

Acceptance:

- single-access and row-locality cases within 5-10%
- same ranking across DRAM organizations

### Link and credits

Construct hand-checkable scenarios:

- one request, one response, no contention
- multiple FLITs with exact serializer occupancy
- credit-starved request stream
- retry probability set to zero versus elevated stress setting

Acceptance:

- simulated latency equals derived timing budget within rounding
- no impossible throughput above physical serialization limit
- no negative or non-monotonic stall behavior

### Other simulators

Use adapters and parsers under `validation/adapters/` and `validation/parsers/` to compare:

- CXRay
- gem5-CXL
- CXLMemSim
- Xerxes
- DRAMSim3
- Ramulator2

Goal:

- preserve trend alignment under the same synthetic traces
- identify where differences come from model scope rather than implementation bugs

## Phase 5: CXL 3.1 feature validation without public silicon

This phase is necessary because hardware for all CXL 3.1 features is not broadly available.

### RS-FEC

Validate as additive link latency with no effect on peak bandwidth except where modeled serialization/credit interaction causes second-order effects.

Checks:

- enabling FEC increases unloaded latency by the configured decode budget
- bandwidth ceiling remains consistent with physical link model

### IDE/TSP

Validate as additive per-transaction or periodic overhead according to the config.

Checks:

- enabling IDE adds configured latency
- key rotation introduces periodic perturbations at configured volume thresholds
- disabling the feature removes those deltas cleanly

### RAS features

Validate each with synthetic traces:

- patrol scrub reduces effective memory bandwidth under overlap
- sPPR and hPPR inject repair latencies according to timing profile
- poison and viral transitions follow legal state progression
- DBIT changes bad-row handling only in relevant cases

Checks:

- timing and state sequence follow the intended model
- no unrelated paths regress when RAS is enabled

Confidence for these features should remain `SC` until real measurement exists.

## Benchmark-to-Feature Mapping

| Benchmark or trace | Primary purpose | Features exercised |
|---|---|---|
| `T1_seq_read` | peak read bandwidth and steady-state link/DRAM behavior | serializer, DRAM bandwidth, link BW |
| `T2_ptr_chase` | unloaded latency | host/device overhead, link latency, DRAM first-hit latency |
| `T3_rand_depth*` | memory-level parallelism sensitivity | outstanding requests, queueing, row locality |
| `T4_seq_req*` | concurrency/BW sweep | credit pools, serialization, saturation knee |
| `T5_calib_*` | hardware calibration | shared interleaved addr, varying rate, 3 R/W ratios |
| `T6_mixed_rw` | mixed traffic behavior | read/write balance, DRAM scheduling |
| `T7_scrub_baseline` | maintenance overhead | scrub bandwidth tax, background interference |
| `T8_random_ppr` | repair event injection | PPR timing and event handling |
| `T9_multihost_n*` | multi-host sharing | switch downstream-link utilization, host contention |

Additional hardware benchmarks to add outside the current trace set:

- STREAM or STREAM-like kernels
- Intel MLC or equivalent latency/BW probes
- NUMA placement tests
- tiering/migration microbenchmarks
- switch-topology multi-initiator stress cases

## Configuration Mapping Procedure

Before comparing simulator output to any external data, create a one-page mapping sheet with:

- external source identifier
- simulator config used
- every parameter copied directly
- every parameter approximated
- every parameter unknown
- rationale for each approximation

Typical parameters to map:

- CXL version
- PCIe generation
- lane width
- flit size and payload
- host port, physical link, and device port latencies
- controller latency
- DRAM type, speed, and topology
- switch hop count and latency
- IDE/TSP and RAS enablement
- NUMA/socket placement

If more than three high-impact parameters are unknown, downgrade the comparison to `envelope` or `qualitative`.

## Reporting Requirements

Every validation report should include:

- exact repo commit
- simulator config file
- trace or benchmark identity
- oracle identity
- comparison type: direct, envelope, or qualitative
- confidence level: `SV`, `CE`, `CM`, `SC`, or `F`
- measured error or qualitative outcome
- caveats and mismatch notes

Recommended summary tables:

1. Feature confidence table
2. Golden-data comparison table
3. Open validation gaps table

## Recommended Near-Term Execution Plan

### Step 1

Stabilize and rerun the existing `validation/` flow for current configs and traces. Generate baseline reports from `validation/compare.py`.

### Step 2

Clean up `validation/golden/paper_data.json` into two views:

- `direct_calibration_points`
- `envelope_reference_points`

This prevents accidental point-accuracy claims on mismatched systems.

### Step 3

Acquire one real Type-3 CXL platform and run a minimal benchmark suite:

- pointer chase
- sequential BW
- thread/concurrency sweep
- local-versus-CXL comparison

Use that platform to calibrate the common host-link-device-memory path.

### Step 4

Validate DRAM behavior against Ramulator2/DRAMSim3 under the same synthetic traces.

### Step 5

For CXL 3.1-only features, publish claims as spec-derived and trend-validated unless direct silicon data becomes available.

## Exit Criteria

The simulator can be described as "validated" for a given feature only if:

- the feature has an assigned confidence level
- the comparison method is documented
- the acceptance target is met or deviations are explained
- the result is reproducible from files committed in the repo

The simulator should not be described as fully silicon-validated for CXL 3.1 end-to-end behavior until actual 3.1-capable systems are measured.

## Open Gaps

Current likely gaps to track explicitly:

- limited public silicon data for CXL 3.0/3.1 feature paths
- scarce public switched-fabric queueing measurements
- weak public data for Type-2 coherence timing
- limited tooling for exercising RAS recovery flows on hardware
- configuration ambiguity in many published papers and whitepapers

These gaps do not block useful validation, but they do limit the strength of some claims.

## Immediate Repo Actions

Short-term improvements that follow directly from this plan:

- add a machine-readable validation inventory under `validation/reports/`
- separate direct versus envelope golden data in `validation/golden/`
- add a report template that emits confidence levels per feature
- add synthetic checks for IDE/TSP and more explicit credit-stall tests
- document hardware benchmark procedures under `validation/`

## Bottom Line

The recommended path is:

- calibrate the common CXL memory path on real Type-3 hardware now
- use cross-paper envelopes where configs differ
- use cross-model and analytical checks for subcomponents
- treat CXL 3.1-specific features as spec-consistent unless measured on real systems

This yields a defensible validation story without overstating what current public hardware can prove.
