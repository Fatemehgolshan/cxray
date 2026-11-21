#!/usr/bin/env python3
"""
Generate BW-sweep calibration traces, run CXRay (cxl_2.0.cfg),
and write paper/figures/bw_sweep_cxray.json.

W traffic: deterministic (D/D/1) sweep across nreq=1 interval and nreq=2..7.
  Produces the flat-then-sharp write BW-latency curve matching hardware.

R and M traffic: simple deterministic nreq sweep (D/D/1 reference).
  The figure script (build_paper_figures.py) uses the M/D/1 analytical formula
  for R and M panels, so these simulation points serve only as a reference.
  Stochastic (Poisson) arrivals at high utilisation are unstable in the
  trace-driven credit-pool model due to credit feedback loops, so the M/D/1
  formula is evaluated analytically rather than by simulation for R/M.
"""

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np

RNG = np.random.default_rng(42)

REPO      = Path(__file__).resolve().parents[1]
CONFIG    = REPO / "configs" / "cxl_2.0.cfg"
CXRAY     = REPO / "src" / "cxray.py"
# Overridable so public-tree / artifact runs need no paper/ directory.
OUT_JSON  = Path(os.environ.get("CXRAY_CALIB_JSON",
                                REPO / "paper" / "figures" / "bw_sweep_cxray.json"))
TRACE_DIR = Path("/tmp/cxray_calib_traces")
RUN_DIR   = Path("/tmp/cxray_calib_runs")

BUF_SIZE  = 0x10000000   # 256 MB
CACHELINE = 64
TOTAL_REQS = 100_000

# --set overrides applied to every simulation run
# - queued DRAM backend: bank serialisation → latency rise at high BW
# - 256 credits each: lets us drive BW to DRAM saturation (~27 GB/s)
SIM_OVERRIDES = [
    "dram.backend=queued",
    "link.credits.m2s_req=256",
    "link.credits.m2s_rwd=256",
    # DDR5 has bank groups: 75% of ACT-to-ACT pairs are cross-group (tRRD_S≈2.5 ns),
    # 25% are same-group (tRRD_L≈5 ns). Effective value ≈ 0.75×2.5 + 0.25×5 = 3.125 ns.
    # Using 5 ns (per-rank conservative) caps throughput at 25.6 GB/s; 3.125 ns lifts
    # that to ~41 GB/s, letting bank capacity be the true bottleneck.
    "dram.tRRD_L_ns=3.125",
]

# ---------------------------------------------------------------------------
# Sweep configurations
# ---------------------------------------------------------------------------

# Interval sweep (nreq=1, deterministic): establishes unloaded base latency for W.
INTERVAL_SWEEP_W_NS = [50000, 10000, 2000, 500, 100, 25, 14, 7, 5, 3]

# Deterministic multi-requester sweep for W: flat latency until saturation (D/D/1).
# Hardware write BW-lat is flat-then-sharp, matching D/D/1 behaviour.
NREQ_PAIRS_W = [
    (2,  14),   #  9.1 GB/s
    (3,  14),   # 13.7 GB/s
    (4,  14),   # 18.3 GB/s
    (4,  12),   # 21.3 GB/s
    (5,  14),   # 22.9 GB/s
    (6,  16),   # 24.0 GB/s
    (5,  13),   # 24.6 GB/s
    (6,  15),   # 25.6 GB/s — last stable point before write capacity ~25.1 GB/s
    (6,  14),   # 27.4 GB/s — above write cap; filtered by MAX_LAT_NS
    (7,  16),   # 28.0 GB/s
    (7,  15),   # 29.9 GB/s
]

# Deterministic nreq sweep for R and M (D/D/1 reference, not used by figure).
NREQ_RANGE_RM    = list(range(1, 8))   # nreq = 1 .. 7
INTER_ISSUE_RM_NS = 14                 # ns inter-issue per requester

# Discard runaway-unstable points (allow gently-rising latency near saturation)
MAX_LAT_NS = 8_000


# ---------------------------------------------------------------------------
# Trace generators
# ---------------------------------------------------------------------------

def _op(traffic: str, k: int) -> str:
    if traffic == "R":  return "R"
    if traffic == "W":  return "W"
    return "R" if k % 2 == 0 else "W"


def write_trace_interval(path: Path, traffic: str, interval_ns: int) -> None:
    addr = 0
    with open(path, "w") as f:
        f.write(f"# Calib: {traffic} nreq=1 interval={interval_ns}ns\n")
        ts = 0
        for k in range(TOTAL_REQS):
            f.write(f"{ts} {_op(traffic, k)} 0x{addr:016x} {CACHELINE} 0\n")
            addr = (addr + CACHELINE) % BUF_SIZE
            ts += interval_ns


def write_trace_nreq(path: Path, traffic: str, nreq: int,
                     inter_issue_ns: int = 14) -> None:
    """Independent per-requester timelines; shared interleaved address space.

    For mixed traffic each requester tracks a per-bank flip flag so that
    every bank alternates R→W→R→W on successive visits.  With stride=nreq*CL
    each bank is owned by exactly one requester, so the per-requester flip is
    also the global flip.  This gives each bank an average service time of
    (t_conflict + t_conflict+tWR)/2 = 74.2 ns → mixed bank capacity ≈ 27.6 GB/s.
    """
    stride = nreq * CACHELINE
    reqs_per_req = max(1, TOTAL_REQS // nreq)
    entries = []
    for rid in range(nreq):
        ts   = rid          # 1 ns stagger
        addr = rid * CACHELINE
        bank_flip: dict[int, bool] = {}   # per-bank: False=next R, True=next W
        for k in range(reqs_per_req):
            if traffic == "M":
                bank = (addr // CACHELINE) % 32
                write_next = bank_flip.get(bank, False)
                op = "W" if write_next else "R"
                bank_flip[bank] = not write_next
            else:
                op = _op(traffic, k)
            entries.append((ts, op, addr, CACHELINE, rid))
            addr = (addr + stride) % BUF_SIZE
            ts += inter_issue_ns
    entries.sort(key=lambda e: e[0])
    with open(path, "w") as f:
        f.write(f"# Calib: {traffic} nreq={nreq} inter_issue={inter_issue_ns}ns\n")
        for ts, op, addr, size, rid in entries:
            f.write(f"{ts} {op} 0x{addr:016x} {size} {rid}\n")


# ---------------------------------------------------------------------------
# Simulation runner
# ---------------------------------------------------------------------------

def run_sim(trace_path: Path, run_dir: Path) -> dict | None:
    run_dir.mkdir(parents=True, exist_ok=True)
    summary_path = run_dir / "summary.json"
    if summary_path.exists():
        with open(summary_path) as f:
            return json.load(f)

    cmd = [sys.executable, str(CXRAY),
           "-c", str(CONFIG), "-t", str(trace_path), "-o", str(run_dir)]
    for kv in SIM_OVERRIDES:
        cmd += ["--set", kv]

    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    if proc.returncode != 0:
        print(f"\n    STDERR: {proc.stderr[-400:]}", file=sys.stderr)
        return None
    if not summary_path.exists():
        return None
    with open(summary_path) as f:
        return json.load(f)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    # Wipe old cached runs so overrides take effect
    if RUN_DIR.exists():
        shutil.rmtree(RUN_DIR)
    TRACE_DIR.mkdir(parents=True, exist_ok=True)
    RUN_DIR.mkdir(parents=True, exist_ok=True)

    all_results: dict[str, list[dict]] = {"R": [], "W": [], "M": []}

    for traffic in ["R", "W", "M"]:
        print(f"\n=== Traffic: {traffic} ===")

        if traffic == "W":
            # Writes: deterministic arrivals (D/D/1) — flat then sharp, matching hardware.
            for interval_ns in INTERVAL_SWEEP_W_NS:
                trace_path = TRACE_DIR / f"calib_W_int{interval_ns}.trace"
                run_dir    = RUN_DIR   / f"W_int{interval_ns}"
                label      = f"nreq=1,int={interval_ns}ns"
                if not trace_path.exists():
                    write_trace_interval(trace_path, "W", interval_ns)
                print(f"  {label:<30s} ", end="", flush=True)
                _record(run_sim(trace_path, run_dir), label, all_results["W"])
            for nreq, inter_ns in NREQ_PAIRS_W:
                trace_path = TRACE_DIR / f"calib_W_nreq{nreq}_int{inter_ns}.trace"
                run_dir    = RUN_DIR   / f"W_nreq{nreq}_int{inter_ns}"
                label      = f"nreq={nreq},int={inter_ns}ns"
                if not trace_path.exists():
                    write_trace_nreq(trace_path, "W", nreq, inter_ns)
                print(f"  {label:<30s} ", end="", flush=True)
                _record(run_sim(trace_path, run_dir), label, all_results["W"])
        else:
            # R/M: deterministic nreq sweep. D/D/1 reference data only;
            # the figure uses M/D/1 analytical curves for these panels.
            for nreq in NREQ_RANGE_RM:
                trace_path = TRACE_DIR / f"calib_{traffic}_nreq{nreq}_int{INTER_ISSUE_RM_NS}.trace"
                run_dir    = RUN_DIR   / f"{traffic}_nreq{nreq}_int{INTER_ISSUE_RM_NS}"
                label      = f"nreq={nreq},int={INTER_ISSUE_RM_NS}ns"
                if not trace_path.exists():
                    write_trace_nreq(trace_path, traffic, nreq, INTER_ISSUE_RM_NS)
                print(f"  {label:<30s} ", end="", flush=True)
                _record(run_sim(trace_path, run_dir), label, all_results[traffic])

        all_results[traffic].sort(key=lambda x: x["bw_GBps"])

    print("\n--- Summary ---")
    for t in ["R", "W", "M"]:
        pts = all_results[t]
        if pts:
            lo, hi = pts[0], pts[-1]
            print(f"  {t}: {len(pts)} pts | "
                  f"low {lo['bw_GBps']:.2f} GB/s {lo['mean_lat_ns']:.0f} ns | "
                  f"high {hi['bw_GBps']:.2f} GB/s {hi['mean_lat_ns']:.0f} ns")

    OUT_JSON.parent.mkdir(parents=True, exist_ok=True)
    with open(OUT_JSON, "w") as f:
        json.dump(all_results, f, indent=2)
    print(f"Wrote {OUT_JSON}")


def _record(summary: dict | None, label: str, out: list) -> None:
    if summary is None:
        print("FAILED")
        return
    mean_lat = summary["latency_ns"]["all_mean"]
    p99_lat  = summary["latency_ns"]["all_p99"]
    bw       = summary["bandwidth_GBps"]["total_avg"]
    hit_rate = summary["dram_model"]["row_buffer_hit_rate"]
    print(f"BW={bw:6.2f} GB/s  lat={mean_lat:8.1f} ns  row_hit={hit_rate:.3f}")
    if mean_lat < MAX_LAT_NS:
        out.append({
            "label":       label,
            "bw_GBps":     round(bw,       4),
            "mean_lat_ns": round(mean_lat,  2),
            "p99_lat_ns":  round(p99_lat,   2),
        })


if __name__ == "__main__":
    main()
