#!/usr/bin/env python3
"""run_cxray_multicore.py — Fig.4 closed-loop envelope tier (CXRay).

Each core count maps to that many closed-loop requesters (one outstanding
pointer-chase miss each, stride 64 B, 16 MB regions, unconstrained credits,
default/simple DRAM backend). This is the V4 methodology behind the paper's
Fig.4 envelope macros (scripts/run_cxl40_experiments.py exp_multicore).

Closed-loop uniform injection is deterministic, so p99 == mean here; the
paper's Fig.7 loaded tails come from the bursty tier instead
(scripts/run_cxray_loaded.py). zsim+Pin remains the Table VI
simulation-speed cross-check.

Writes (same schema the plotter expects):
  outputs: fig04_bw_concurrency.csv
  columns: version,cores,bw_GBps,mean_lat_ns,p99_lat_ns,method

method is "simulated" for every row.

Usage:
  python3 scripts/run_cxray_multicore.py --out-csv DIR [--cores 4 8 16 32 64]
      [--versions ddr 2.0 3.0 3.1 4.0] [--work DIR]
"""
import argparse
import csv
import json
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
SIM = REPO / "src" / "cxray.py"
CFGDIR = REPO / "configs"

CFG = {
    "ddr": "ddr5_only.cfg",
    "2.0": "cxl_2.0.cfg",
    "3.0": "cxl_3.0.cfg",
    "3.1": "cxl_3.1.cfg",
    "4.0": "cxl_4.0.cfg",
}

# V4 recipe: per-requester closed loop, no think time, unconstrained
# credits (Little's-law envelope), default/simple DRAM backend.
OVERRIDES = [
    "inject.closed_loop_per_req=10000",
    "inject.stride_bytes=64",
    "inject.region_bytes=16777216",
    "link.credits.m2s_req=0",
    "link.credits.m2s_rwd=0",
    "link.credits.s2m_drs=0",
    "link.credits.s2m_ndr=0",
]


def run_one(cfg, seed, outdir):
    outdir.mkdir(parents=True, exist_ok=True)
    cmd = [sys.executable, str(SIM), "-c", str(cfg), "-t", str(seed),
           "-o", str(outdir)]
    for ov in OVERRIDES:
        cmd += ["--set", ov]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"  [FAIL] {cfg.name}: {r.stderr[-300:]}")
        return None
    with open(outdir / "summary.json") as f:
        return json.load(f)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-csv", required=True)
    ap.add_argument("--cores", nargs="+", type=int, default=[4, 8, 16, 32, 64])
    ap.add_argument("--versions", nargs="+",
                    default=["ddr", "2.0", "3.0", "3.1", "4.0"])
    ap.add_argument("--work", default=None)
    a = ap.parse_args()

    work = Path(a.work) if a.work else Path(tempfile.mkdtemp(prefix="cxray_mc_"))
    work.mkdir(parents=True, exist_ok=True)
    out_csv = Path(a.out_csv)
    out_csv.mkdir(parents=True, exist_ok=True)

    rows = []
    for ver in a.versions:
        cfg = CFGDIR / CFG[ver]
        if not cfg.exists():
            print(f"  [SKIP] missing cfg {cfg.name}")
            continue
        for c in a.cores:
            seed = work / f"mc_seed_r{c}.trace"
            if not seed.exists():
                with open(seed, "w") as f:
                    for rid in range(c):
                        f.write(f"0 R 0x{rid * 64 * 4096:016x} 64 {rid}\n")
            print(f"[run] {ver} {c} cores", flush=True)
            s = run_one(cfg, seed, work / f"{ver}_c{c}")
            if s is None:
                continue
            lat = s["latency_ns"]
            mean = round(lat["all_mean"], 1)
            p99 = round(lat["all_p99"], 1)
            bw = round(s["bandwidth_GBps"]["total_avg"], 3)
            rows.append({"version": ver, "cores": c, "bw_GBps": bw,
                         "mean_lat_ns": mean, "p99_lat_ns": p99,
                         "method": "simulated"})
            print(f"  mean={mean} ns p99={p99} bw={bw} GB/s")

    # Envelope only: Fig.7 loaded latencies come from run_cxray_loaded.py.
    with open(out_csv / "fig04_bw_concurrency.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["version", "cores", "bw_GBps",
                                          "mean_lat_ns", "p99_lat_ns",
                                          "method"])
        w.writeheader()
        for r in sorted(rows, key=lambda r: (r["version"], r["cores"])):
            w.writerow(r)
    print(f"wrote {out_csv / 'fig04_bw_concurrency.csv'} ({len(rows)} rows)")


if __name__ == "__main__":
    main()
