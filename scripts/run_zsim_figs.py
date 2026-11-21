#!/usr/bin/env python3
"""run_zsim_figs.py — execution-driven cross-check tier via zsim+CXRay.

NOTE: Figs 4/7 are produced by scripts/run_cxray_multicore.py (closed-loop
tier, which reproduces the paper); this script is the independent
execution-driven cross-check (Table VI sim-speed context). It builds
mt_chase2 + zsim (needs PINPATH + scons + libconfig/hdf5 dev libs), runs the
xeon_* core-count sweep (cores 4/8/16/32/64 × DDR/CXL-2.0/3.0/3.1), parses
zsim.h5 (instrumented CXL counters + demand-read histograms; DDR uses
generic rd/wr/rdlat/wrlat mem counters), and writes:

  outputs: fig04_bw_concurrency.csv  (version,cores,bw_GBps,mean_lat_ns,p99_lat_ns,method)
           fig07_loaded.csv          (version,cores,mean_lat_ns,p99_lat_ns,method)

CXL 4.0 rows are derived exactly like the paper (lat_40 = lat_31 - 22 ns floor
delta; bw_40 = cores*64B/lat_40 Little's-law; p99_40 = p99_20 + 2 ns) and are
labeled method=derived. DDR p99 is reported only when the h5 exposes a usable
histogram (--dump-fields to inspect a DDR h5).

BW definition (all versions): completed demand bytes / simulated seconds,
  sim_seconds = max core cycles / frequency. This is the Little's-law envelope
  the paper's concurrency section reasons about.

Usage:
  python3 scripts/run_zsim_figs.py --out-csv DIR [--cores 4 8 16 32 64]
      [--versions ddr 2.0 3.0 3.1] [--skip-build] [--dump-fields FILE.h5]
Requires: PINPATH env, scons, gcc, python3-h5py (or pip h5py).
"""
import argparse
import csv
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import h5py
import numpy as np

REPO = Path(__file__).resolve().parents[1]
ZSIM = REPO / "zsim"
TESTS = ZSIM / "tests"
WORKLOAD_SRC = ZSIM / "workloads" / "mt_chase2.c"

CFG = {
    "ddr": "xeon_ddr_c{N}.cfg",
    "2.0": "xeon_cxl_c{N}.cfg",
    "3.0": "xeon_cxl30_c{N}.cfg",
    "3.1": "xeon_cxl31_c{N}.cfg",
}
FLOOR_DELTA_31_40 = 22.0  # CXL 3.1 floor 169 ns -> CXL 4.0 floor 147 ns

HIST_BIN_NS = 8.0
FINE_BINS, COARSE_NS, COARSE_BINS, TAIL_NS = 500, 128.0, 500, 2000.0


def bin_upper(i):
    if i < FINE_BINS:
        return (i + 1) * HIST_BIN_NS
    if i < FINE_BINS + COARSE_BINS:
        return FINE_BINS * HIST_BIN_NS + (i - FINE_BINS + 1) * COARSE_NS
    return (FINE_BINS * HIST_BIN_NS + COARSE_BINS * COARSE_NS
            + (i - FINE_BINS - COARSE_BINS + 1) * TAIL_NS)


def p_from_hist(hist, q):
    tot = int(np.sum(hist))
    if tot == 0:
        return None
    target, cum = q * tot, 0
    for i, c in enumerate(hist):
        cum += int(c)
        if cum >= target:
            return None if i == len(hist) - 1 else bin_upper(i)
    return None


def find_freq(out_cfg):
    try:
        m = re.search(r"frequency\s*=\s*(\d+)", Path(out_cfg).read_text())
        if m:
            return int(m.group(1))
    except OSError:
        pass
    return 2700


def parse_h5(h5path):
    """Return dict with mean/p99/BW or Nones. Handles CXL + generic DDR mem."""
    out = {"mean_lat_ns": None, "p99_lat_ns": None, "bw_GBps": None,
           "sim_seconds": None, "freq_mhz": None}
    with h5py.File(h5path, "r") as f:
        root = f["stats"]["root"]
        rec = root[root.shape[0] - 1]
        cores = rec["c"]
        cycles = int(np.max(cores["cycles"]))
        freq = out["freq_mhz"] = find_freq(
            os.path.join(os.path.dirname(os.path.abspath(h5path)), "out.cfg")) or 2700
        ghz = freq / 1000.0
        out["sim_seconds"] = cycles / (freq * 1e6)
        mem = rec["mem"]
        mem0 = mem[0] if mem.shape else mem
        names = list(mem0.dtype.names)

        def sub_with(*keys):
            if all(k in names for k in keys):
                return mem0, ""
            for n in names:
                sub = mem0[n]
                if (hasattr(sub, "dtype") and sub.dtype.names
                        and all(k in sub.dtype.names for k in keys)):
                    return sub, n
            return None, None

        # Instrumented CXL path (demand histograms)
        cx, _ = sub_with("cxlReqs")
        if cx is not None:
            reqs = np.array(cx["cxlReqs"], dtype=np.float64)
            lat = np.array(cx["cxlLat"], dtype=np.float64)
            dem_n = float(reqs[0] + reqs[1])
            dem_l = float(lat[0] + lat[1])
            if dem_n > 0:
                out["mean_lat_ns"] = dem_l / dem_n / ghz
                out["bw_GBps"] = dem_n * 64 / out["sim_seconds"] / 1e9
            h = np.array(cx["cxlHistDemandRd"], dtype=np.float64)
            out["p99_lat_ns"] = p_from_hist(h, 0.99)
            return out
        # Generic DDR path (rd/wr + rdlat/wrlat cycle counters)
        blk, _ = sub_with("rd", "wr", "rdlat", "wrlat")
        if blk is not None:
            rd, wr = int(blk["rd"]), int(blk["wr"])
            if rd + wr > 0:
                out["mean_lat_ns"] = (int(blk["rdlat"]) + int(blk["wrlat"])) / (rd + wr) / ghz
                out["bw_GBps"] = (rd + wr) * 64 / out["sim_seconds"] / 1e9
            for n in (list(blk.dtype.names) if hasattr(blk, "dtype") else []):
                if "hist" in n.lower():
                    try:
                        h = np.array(blk[n], dtype=np.float64).ravel()
                        if h.size > 8 and h.sum() > 0:
                            out["p99_lat_ns"] = p_from_hist(h, 0.99)
                            break
                    except Exception:
                        continue
            return out
    return out


def dump_fields(h5path):
    with h5py.File(h5path, "r") as f:
        def walk(name, obj):
            if hasattr(obj, "dtype") and obj.dtype.names:
                print(f"{name}: {list(obj.dtype.names)[:30]}")
        f["stats"]["root"].visititems(walk)


def build_all(work):
    pin = os.environ.get("PINPATH")
    if not pin or not Path(pin).exists():
        sys.exit("PINPATH not set (or missing). See zsim/README.md + docker/README_PIN.md.")
    mt = work / "mt_chase2"
    print(f"[build] mt_chase2 from {WORKLOAD_SRC}")
    subprocess.run(["gcc", "-O2", "-fopenmp", "-o", str(mt), str(WORKLOAD_SRC)],
                   check=True)
    print("[build] zsim via scons (this takes a while)")
    subprocess.run(["scons", "-j8"], cwd=str(ZSIM), check=True)
    zbin = ZSIM / "build" / "opt" / "zsim"
    if not zbin.exists():
        sys.exit(f"zsim binary missing after build: {zbin}")
    return mt, zbin


def stage_cfg(cfg, outdir, cores):
    """Copy cfg to outdir, bumping the HOST-side GM heap for large core
    counts (gmMBytes only sizes zsim's own allocator; the simulated machine
    is unchanged). 4 GB overflows at >=16 OOO cores."""
    outdir.mkdir(parents=True, exist_ok=True)
    heap = max(4096, cores * 512)
    text = re.sub(r"gmMBytes\s*=\s*\d+;", f"gmMBytes = {heap};",
                  Path(cfg).read_text())
    staged = outdir / cfg.name
    staged.write_text(text)
    return staged


def run_one(zbin, cfg, outdir, mt, cores):
    outdir.mkdir(parents=True, exist_ok=True)
    cfg = stage_cfg(cfg, outdir, cores)
    shutil.copyfile(mt, "/tmp/mt_chase2")
    r = subprocess.run([str(zbin), str(cfg)], cwd=str(outdir),
                       capture_output=True, text=True)
    (outdir / "zsim.log").write_text(r.stdout[-20000:] + "\n" + r.stderr[-5000:])
    if r.returncode != 0:
        print(f"  [FAIL] {cfg.name} rc={r.returncode} (see {outdir}/zsim.log)")
        return None
    h5s = list(outdir.glob("zsim.h5"))
    if not h5s:
        print(f"  [FAIL] {cfg.name}: no zsim.h5")
        return None
    return parse_h5(h5s[0])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-csv", required=True)
    ap.add_argument("--cores", nargs="+", type=int, default=[4, 8, 16, 32, 64])
    ap.add_argument("--versions", nargs="+", default=["ddr", "2.0", "3.0", "3.1"])
    ap.add_argument("--skip-build", action="store_true")
    ap.add_argument("--work", default=None)
    ap.add_argument("--dump-fields", default=None)
    a = ap.parse_args()
    if a.dump_fields:
        return dump_fields(a.dump_fields)

    work = Path(a.work) if a.work else Path(tempfile.mkdtemp(prefix="cxray_zsim_"))
    work.mkdir(parents=True, exist_ok=True)
    out_csv = Path(a.out_csv)
    out_csv.mkdir(parents=True, exist_ok=True)

    if a.skip_build:
        zbin = ZSIM / "build" / "opt" / "zsim"
        mt = work / "mt_chase2"
        if not zbin.exists():
            sys.exit("zsim binary missing; drop --skip-build")
        if not mt.exists():
            # Static: Pin 2.14 crashes on dynamically linked glibc >= 2.34
            # apps (loader interference); static binaries run clean.
            subprocess.run(["gcc", "-O2", "-fopenmp", "-static", "-o", str(mt),
                            str(WORKLOAD_SRC)], check=True)
    else:
        mt, zbin = build_all(work)

    rows = []
    for ver in a.versions:
        for c in a.cores:
            cfg = TESTS / CFG[ver].format(N=c)
            if not cfg.exists():
                print(f"  [SKIP] missing cfg {cfg.name}")
                continue
            d = work / f"{ver}_c{c}"
            print(f"[run] {ver} {c} cores", flush=True)
            st = run_one(zbin, cfg, d, mt, c)
            if st is None or st["mean_lat_ns"] is None:
                print(f"  [WARN] no stats for {ver} c{c}")
                continue
            rows.append({"version": ver, "cores": c,
                         "bw_GBps": round(st["bw_GBps"] or 0, 3),
                         "mean_lat_ns": round(st["mean_lat_ns"], 1),
                         "p99_lat_ns": (round(st["p99_lat_ns"], 1)
                                        if st["p99_lat_ns"] else ""),
                         "method": "measured"})
            print(f"  mean={st['mean_lat_ns']:.1f} ns p99={st['p99_lat_ns']} "
                  f"bw={st['bw_GBps']:.3f} GB/s")

    # CXL 4.0 derived exactly like the paper
    have31 = {r["cores"]: r for r in rows if r["version"] == "3.1"}
    have20 = {r["cores"]: r for r in rows if r["version"] == "2.0"}
    if have31 and "4.0" not in a.versions:
        for c in sorted(have31):
            m31, p31 = have31[c]["mean_lat_ns"], have31[c]["p99_lat_ns"]
            m40 = round(m31 - FLOOR_DELTA_31_40, 1)
            bw40 = round(c * 64 / m40, 3)
            p20 = (have20.get(c) or {}).get("p99_lat_ns") or ""
            p40 = round(float(p20) + 2.0, 1) if p20 != "" else ""
            rows.append({"version": "4.0", "cores": c, "bw_GBps": bw40,
                         "mean_lat_ns": m40, "p99_lat_ns": p40,
                         "method": "derived"})
            print(f"  [derived] 4.0 c{c}: mean={m40} bw={bw40}")

    for name in ["fig04_bw_concurrency.csv", "fig07_loaded.csv"]:
        with open(out_csv / name, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=["version", "cores", "bw_GBps",
                                              "mean_lat_ns", "p99_lat_ns", "method"])
            w.writeheader()
            for r in sorted(rows, key=lambda r: (r["version"], r["cores"])):
                w.writerow(r)
        print(f"wrote {out_csv / name} ({len(rows)} rows)")


if __name__ == "__main__":
    main()
