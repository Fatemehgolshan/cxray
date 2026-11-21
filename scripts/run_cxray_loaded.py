#!/usr/bin/env python3
"""run_cxray_loaded.py — Fig.7 loaded-latency tier (CXRay, paced random chains).

Each core count maps to that many closed-loop requesters (one outstanding
pointer-chase miss each), reissued immediately on completion to a
uniform-random 64 B line in the requester's own 16 MB region. Unlike the
Fig.4 envelope tier (unconstrained credits, deterministic stride walk,
p99 == mean by construction), this tier keeps the configured finite credit
pools and the queued DRAM backend, so bank conflicts and bursts produce
genuine load tails: p99 > mean, widening with core count. Issue stays
bounded by the requester count, so high core counts saturate honestly
instead of diverging.

Writes (same schema the plotter expects):
  outputs: fig07_loaded.csv
  columns: version,cores,bw_GBps,mean_lat_ns,p99_lat_ns,method

method is "simulated" for every row (all versions measured directly).

Usage:
  python3 scripts/run_cxray_loaded.py --out-csv DIR [--cores 4 8 16 32 64]
      [--versions ddr 2.0 3.0 3.1 4.0] [--work DIR] [--per-chain N]
"""
import argparse
import csv
import json
import random
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

# Bursty closed-loop: each requester reissues immediately on completion
# (bounded outstanding = requester count, no open-loop divergence) but to a
# uniform-random line in its 16 MB region. Sequential strides self-organize
# into convoys (degenerate p99 == mean); random reissues restore bank/row
# burstiness, so loaded runs show genuine tails. Finite credit pools and the
# queued DRAM backend are kept at their configured defaults.
CLOSED_LOOP_PER_REQ = 10000
REGION_BYTES = 16777216  # 16 MB private region per requester


def gen_seed(path, n_chains):
    with open(path, "w") as f:
        f.write(f"# loaded tier: {n_chains} bursty closed-loop requesters\n")
        for rid in range(n_chains):
            f.write(f"0 R 0x{rid * 64 * 4096:016x} 64 {rid}\n")


def run_one(cfg, seed, outdir, per_req=CLOSED_LOOP_PER_REQ):
    outdir.mkdir(parents=True, exist_ok=True)
    cmd = [sys.executable, str(SIM), "-c", str(cfg), "-t", str(seed),
           "-o", str(outdir),
           "--set", "dram.backend=queued",
           "--set", "inject.closed_loop_per_req=%d" % per_req,
           "--set", "inject.closed_loop_random=1",
           "--set", "inject.region_bytes=%d" % REGION_BYTES]
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
    ap.add_argument("--per-req", type=int, default=CLOSED_LOOP_PER_REQ,
                    help="requests per requester (default 10000)")
    a = ap.parse_args()

    work = Path(a.work) if a.work else Path(tempfile.mkdtemp(prefix="cxray_ld_"))
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
            seed = work / f"ld_seed_r{c}.trace"
            if not seed.exists():
                gen_seed(seed, c)
            print(f"[run] {ver} {c} cores", flush=True)
            s = run_one(cfg, seed, work / f"{ver}_c{c}", per_req=a.per_req)
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

    with open(out_csv / "fig07_loaded.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["version", "cores", "bw_GBps",
                                          "mean_lat_ns", "p99_lat_ns",
                                          "method"])
        w.writeheader()
        for r in sorted(rows, key=lambda r: (r["version"], r["cores"])):
            w.writerow(r)
    print(f"wrote {out_csv / 'fig07_loaded.csv'} ({len(rows)} rows)")


if __name__ == "__main__":
    main()
