# CXRay — CXL Memory Simulator

A cycle-approximate CXL memory-subsystem simulator covering CXL 1.1 through
4.0. Two implementations of the same trace-driven engine ship here:

- `src/cxray.py` — Python reference model. Readable, annotated, extensible.
  Use it to prototype a feature or reproduce one experiment.
- `src/*.cpp` + `CMakeLists.txt` — C++ production build (`build/cxray`).
  Same logic, less interpreter overhead. Used for the speed benchmark.

Both take the same configs and traces and write the same `summary.json`.

## Stage 0 — reproduce every result (start here)

One script rebuilds all paper figures (Figs. 2–12 + Table VI), resuming
where it left off if interrupted. Run these three commands, in order:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"$(nproc)"
./reproduce.sh --smoke     # ~2 min sanity subset; must end with ALL DONE
./reproduce.sh             # full run (trace tiers take a while; T9 is large)
```

After the full run you should see, in `outputs/`:

- `fig02_calibration.png` … `fig12_multihost.png` (12 figures)
- `table6_simspeed.png` (paper-vs-reproduced side by side)
- `csv/` — one CSV per figure with every plotted point, plus
  `bw_sweep_cxray.json`

`outputs/` is gitignored build output. The committed golden copies of these
exact files live in `artifact/reference/` — byte-comparable curves; only
host-speed numbers (Table VI) legitimately differ per machine.

Reproduction scope is standalone CXRay only (no Pin, no zsim, no gem5):

- Reproduced by the script: Figs. 2–12 (all CXRay curves) plus the CXRay
  columns of Table VI.
- Shipped as recorded reference (not re-measured): the Fig. 2 CZ120
  hardware curve and the zsim/Xerxes columns of Table VI — both are
  machine-specific and would differ on any other host.
- Not reproduced (paper-only, out of scope): Tables VII–VIII, the
  GAPBS/ML application runs, and any gem5/Sniper execution path.

Faster feedback while developing:

```bash
./reproduce.sh --fig 6      # only Fig. 6 + its CSV
./reproduce.sh --help       # tiers, outputs, resume stamps
```

Docker (self-contained; internet assumed for apt only):

```bash
docker build -f docker/Dockerfile -t cxray-ae .
docker run --rm -v "$PWD/outputs:/repo/outputs" cxray-ae
```

No step above needs Pin or zsim. Those are only for the optional
Table VI host-speed cross-check (see `docker/README_PIN.md`).

## Contents

```text
cxray-public/
  src/                  C++ engine + cxray.py reference model
  configs/              18 configs: CXL 1.1/2.0/3.0/3.1/4.0, GIM, switch,
                        Type 2, NUMA, DDR5-only, bundled port
  tests/                7 C++ unit tests + pytest suites
                        (test_multihost.py, test_fig04_tier.py)
  scripts/              experiment drivers (see below)
  validation/
    traces/generate_traces.py   T1-T9 mini-benchmark generator (no traces
                                are committed; you generate them)
    parsers/            cxray/xerxes/dramsim3/ramulator2/cxlmemsim parsers
    golden/paper_data.json      published-silicon envelope for validation
    configs/cxl_2.0_baseline.json, schema/result.json
    synthetic/          trace_replay.c + trace_replay_gem5.c + Makefile
    compare.py, run_cxray_validation.sh, VALIDATION_PLAN.md
  integrations/
    gem5/               out-of-tree CXRayMem object (no gem5 patch needed)
    sniper/             DramPerfModelCXRay + stock-sniper.patch + apply.sh
  zsim/                   vendored zsim fork (GPLv2, see zsim/LICENSE) with the
                        CXRay CXLMemory model + xeon_* sweep configs +
                        workloads/mt_chase2.c (Table VI speed cross-check)
  zsim-optional/        pointer doc: what the execution tier is for and what
                        to install when (see also docker/README_PIN.md)
  real-hardware/        bandwidth-latency.csv — measured CZ120 curve used as
                        the gray reference in Fig. 2 (not generated)
  paper-data/           checked-in reference CSVs/JSON for the gray overlays
                        (hardware Fig. 2 + published Figs. 9–11 curves)
  artifact/
    plot_ae.py          paper-style renderer (CSV -> PNG)
    reference/          golden figures: the 12 repro PNGs + table PNG that
                        the current tree produces (what reviewers should see)
    work/               resume stamps (gitignored)
  artifact-evaluation/  HPCA AE submission skeleton (filled in per call)
  docker/Dockerfile     self-contained image recipe (Ubuntu 24.04)
  reproduce.sh          ONE script that rebuilds every figure (see Stage 0)
  init.sh               interactive setup: pick simulators, fetch, patch,
                        build, generate traces
```

Not shipped here: paper sources, generated outputs/build dirs, and
superseded placeholder figure scripts. `validation/adapters/gem5_cxl/` was
dropped because it hardcoded a conda path; use `integrations/gem5/`.

## Requirements

- CMake >= 3.14, a C++17 compiler (GCC 9+), Python >= 3.8, numpy,
  matplotlib (plots only), pytest (Python suites).
- Optional: libconfig++ (auto-detected; a built-in key=value parser is used
  when absent). Set `CONDA_PREFIX` instead of editing paths.
- Optional backends, off by default: DRAMSim3 (`-DDRAMSIM3_DIR=...`),
  Ramulator2 (`-DRAMULATOR2_DIR=...`).

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
python3 -m pytest tests/ -q
```

Or run `./init.sh` and answer which integrations you want.

## Traces and the mini-benchmark suite (T1-T9)

One line per request: `<timestamp_ns> <R|W> <0xaddr> <size> <req_id>`.

```bash
python3 validation/traces/generate_traces.py --output validation/traces
python3 validation/traces/generate_traces.py --trace T2 --output /tmp/tr
```

| Trace | Pattern | What it isolates |
|-------|---------|------------------|
| T1 | sequential, 16 req, stride-64 | peak bandwidth |
| T2 | pointer chase, 1 req, 300 ns gap | protocol floor (Fig. 3) |
| T3 | pointer chase, depth 1/4/16/64 | MLP scaling (Fig. 5) |
| T4 | cacheline-stride, r requesters | row-buffer loss (Fig. 6) |
| T5 | sequential shared-interleave, rate sweep | BW-latency calibration (Fig. 2) |
| T6 | mixed read/write | Type 2 vs Type 3 (Fig. 11) |
| T7 | sequential at 80% BW | scrub-interference baseline (RAS) |
| T8 | random + PPR marker | PPR event impact (RAS) |
| T9 | multihost shared | fabric sharing (Fig. 12) |

Single run:

```bash
./build/cxray --config configs/cxl_2.0.cfg \
  --trace validation/traces/T2_ptr_chase.trace \
  --output /tmp/cxray_out --tag t2_demo
# or: python3 src/cxray.py -c configs/cxl_2.0.cfg -t <trace> -o /tmp/out
```

Per-run scalar override: `--set link.credits.m2s_req=8`.

## Configs

All 18 files in `configs/` are supported. Highlights:

- `cxl_1.1.cfg`, `cxl_2.0.cfg` — Gen5 baselines, no FEC.
- `cxl_3.0.cfg`, `cxl_3.1.cfg` — Gen6 + RS-FEC; 3.1 adds RAS + IDE.
- `cxl_4.0.cfg`, `cxl_4.0_bundled2.cfg` — Gen7 128 GT/s, 2-port option.
- `cxl_3.1_gim[24].cfg`, `cxl_4.0_gim[24].cfg` — global interleave.
- `cxl_3.1_switch[12]hop.cfg`, `cxl_4.0_switch[12]hop.cfg` — fabric hops.
- `cxl_3.1_type2.cfg`, `cxl_4.0_type2.cfg` — accelerator device.
- `cxl_3.0_numa2.cfg` — 2-socket UPI penalty. `ddr5_only.cfg` — no CXL path.

## Experiment scripts

| Script | Runs | Writes (default) |
|--------|------|------------------|
| `scripts/gen_calibration_traces.py` | T5 sweep on `cxl_2.0.cfg` | `bw_sweep_cxray.json`-style output |
| `scripts/run_cxray_multicore.py` | Fig.4 envelope sweep (V4 recipe: stride walk, unconstrained credits) | `fig04_bw_concurrency.csv` |
| `scripts/run_cxray_loaded.py` | Fig.7 loaded sweep (bursty closed-loop: random reissues, finite pools, queued DRAM) | `fig07_loaded.csv` |
| `scripts/run_feature_experiments.py` | E1 credit, E2 switch, E3 GIM, E4 Type2 | `credit_pool_sweep.csv`, `switch_sweep.csv`, `gim_sweep.csv`, `type2_vs_type3.csv` |
| `scripts/run_cxl40_experiments.py` | V1-V4 + E1b-E5b + bundled port | `results/cxray/` stats + macro values |
| `scripts/run_zsim_figs.py` | zsim xeon sweep (execution cross-check) | `fig04_bw_concurrency.csv`, `fig07_loaded.csv` (same schema, independent tier) |
| `scripts/bench_simspeed.py` | Python vs C++ Mreq/s; optional zsim MIPS | `results/bench/bench_simspeed_results.{csv,json}` |
| `scripts/build_paper_figures.py` | vector PDFs from golden data | paper PDFs (needs `paper/` tree; the artifact uses `artifact/plot_ae.py` instead) |

`reproduce.sh` is the supported entry point; the scripts above are the
building blocks it calls. Figs. 4/7 are produced by `run_cxray_multicore.py`;
`run_zsim_figs.py` provides the independent execution-driven cross-check.

## Integrations

All three are optional. Trace-driven CXRay works without any of them.

- **gem5 (copy, no patch).** gem5 v24.1.0.3 (`b9da2bfe1e21`). The checkout is
  unmodified (`stock-gem5.patch` is an empty `git diff`). Build:

  ```bash
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
  cd <gem5> && scons EXTRAS=<cxray>/integrations/gem5 build/X86/gem5.opt -j16
  ```

  Select `--mem-type=CXRayMem`; set `CXRAY_CONFIG` and
  `CXRAY_ATOMIC_LATENCY_NS` (default 100). See `integrations/gem5/README.md`.

- **Sniper (patch).** Sniper commit `56505e42fd98` (v8.1). Apply:

  ```bash
  bash integrations/sniper/apply.sh <sniper_root> <cxray_build_dir>
  cd <sniper_root> && make -j8
  ```

  Build CXRay with the same compiler and `-D_GLIBCXX_USE_CXX11_ABI=0`.
  Run with `perf_model/dram/type=cxray`,
  `perf_model/dram/cxray_config=<cfg>`. `stock-sniper.patch` is the full
  diff; `sniper.patch` + `sniper-fixes.patch` are the split form that
  `apply.sh` uses. See `integrations/sniper/README.md`.

- **zsim (vendored fork; out of reproduction scope).** The `zsim/` tree
  ships the fork used for the paper (GPLv2, keep `zsim/LICENSE`): the
  CXLMemory model is self-contained (no CXRay headers needed), driven by
  `zsim/tests/xeon_*` configs and `zsim/workloads/mt_chase2.c`. Building
  needs Pin 2.14 (Intel EULA forbids redistribution, and Intel blocks
  direct download — see `docker/README_PIN.md`). Native build:
  `PINPATH=<pin> LIBCONFIGPATH=/usr HDF5PATH=<hdf5> scons -j8` in `zsim/`.
  `reproduce.sh` always runs the speed benchmark with `--skip-zsim`
  (CXRay tiers only); drop the flag to re-measure the zsim MIPS column on
  your own host.

`./init.sh` automates all three: it asks which simulators you want, clones
the pinned commits, applies patches, and builds.

## Reproducing the paper (artifact evaluation)

```bash
./reproduce.sh              # full run, resumes where it was left off
./reproduce.sh --smoke      # fast sanity subset
./reproduce.sh --fig 6      # only Fig. 6 + its CSV
./reproduce.sh --help       # tiers, outputs, resume stamps
```

Outputs: `outputs/fig02_calibration.png` … `outputs/fig12_multihost.png`,
`outputs/table6_simspeed.png`, plus one CSV per figure in `outputs/csv/`.
Rerunning is safe: each stage leaves a stamp in `artifact/work/` and skipped
stages are not repeated. No figure needs zsim or Pin. Table VI is reported
side-by-side (paper | reproduced | %Δ); its MIPS/speed columns are
host-dependent and re-recorded on each machine rather than asserted equal.
Methodology notes: Fig. 5 uses direct T3 replay at d=1/4/16 and a
simple-backend closed-loop depth-64 seed; Fig. 6 uses closed-loop
per-requester seeds with the queued DRAM backend. Figs. 4/7 use the CXRay
closed-loop multicore tier (`scripts/run_cxray_multicore.py`), which is what
the paper's numbers come from. `scripts/run_zsim_figs.py` keeps the older
zsim execution tier for users; it is outside the reproduction scope.

Docker (self-contained, internet assumed for apt only):

```bash
docker build -f docker/Dockerfile -t cxray-ae .
docker run --rm -v "$PWD/outputs:/repo/outputs" cxray-ae
docker run --rm cxray-ae ./reproduce.sh --smoke
```

## Validation

```bash
python3 validation/traces/generate_traces.py --output validation/traces
bash validation/run_cxray_validation.sh   # builds, runs T1-T9, compare.py
python3 -m pytest tests/ -q
```

Targets in `validation/VALIDATION_PLAN.md`: unloaded latency, flat-regime
MAPE, loaded-regime envelope vs `validation/golden/paper_data.json`.

## Known limits

- Single host / single fabric domain. No multi-root topologies.
- Switch queueing, DDR backend, RAS, MESO/BI, TSP/IDE: timing-approximate;
  link serialization and credit flow: timing-calibrated. See fidelity matrix
  in the old README history.
- Absolute Mreq/s and MIPS move with the host; the artifact compares curve
  shapes within `VALIDATION_PLAN` tolerances, not exact speeds.
- Under uniform closed-loop injection the latency distribution collapses
  (p99 ≈ mean); tails need arrival burstiness (see the paper's Fig. 7
  discussion). N=4 multihost sits at the DL stability boundary: sub-percent
  service-rate differences decide the regime.

## Reproduction methodology notes

- Fig. 2 MAPE badges are flat-regime only (HW points within base + 35 ns
  define the flat BW range), matching the paper's stated definition.
- Fig. 4/7 use the closed-loop multicore tier (one outstanding miss per
  core, unconstrained credits, default DRAM backend) — the paper's V4
  methodology. CXL 4.0 is simulated directly (the paper derived it
  analytically and agrees to 0.1 ns).
- Fig. 5 d=64 uses a simple-backend closed-loop seed (the knee point);
  d=1/4/16 and DDR d=64 use direct T3 replay.
- Fig. 6/8 use closed-loop per-requester seeds with the queued DRAM backend;
  the T4 row-buffer-destruction knee needs finite bank queues.
- Fig. 10 shows the E3B credit-saturated scenario (device-count invariant);
  absolute E3B latencies are model-version sensitive (current code ~3.9–4.1
  µs for 3.1; the invariant shape is the claim).
- Absolute Mreq/s and MIPS move with the host; the artifact compares curve
  shapes and ratios, not exact speeds.

## License

MIT for CXRay proper (see `LICENSE`). The `zsim/` subtree is the paper's
zsim fork under GPLv2 (see `zsim/LICENSE`); that license applies to files
under `zsim/` only.
