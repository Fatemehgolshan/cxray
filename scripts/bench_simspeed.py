#!/usr/bin/env python3
"""
bench_simspeed.py — Measure CXRay simulation throughput (M req/s / MIPS).

Covers three simulation modes:
  1. Python — trace-driven (src/cxray.py), measures M req/s
  2. C++    — trace-driven (build/cxray), measures M req/s
  3. zsim   — full-system (zsim/build/opt/zsim + mt_chase2), measures MIPS

ISOLATION: all runs are strictly serial.  No background work is started between
runs.  The progress thread only polls elapsed time; it does not touch the CPU.
For maximum accuracy, run on a dedicated (idle) node.

Usage:
    python3 scripts/bench_simspeed.py [options]

    --output  <dir>       Output directory  (default: results/bench)
    --skip-python         Skip Python trace-driven benchmarks
    --skip-cpp            Skip C++ trace-driven benchmarks
    --skip-zsim           Skip zsim full-system benchmarks
    --traces  T1 T5 …     Run only traces whose label matches these substrings
    --repeat  N           Repeat each trace N times (concatenated, one run)
    --zsim-instrs  N      maxTotalInstrs per core for zsim (default: 5B)
    --cores   4 8 16      zsim core counts to sweep (default: 4 8 16 32 64)

Outputs (in <output>/ ):
    bench_simspeed_results.json
    bench_simspeed_results.csv
    bench_simspeed_summary.txt
"""

import argparse
import csv
import json
import math
import os
import re
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

try:
    import h5py
    import numpy as np
    HAS_H5PY = True
except ImportError:
    HAS_H5PY = False

# ── Paths ─────────────────────────────────────────────────────────────────────
REPO      = Path(__file__).resolve().parent.parent
SIM_PY    = REPO / "src"  / "cxray.py"
SIM_CC    = REPO / "build" / "cxray"
ZSIM_BIN  = REPO / "zsim" / "build" / "opt" / "zsim"
ZSIM_CFGS = REPO / "zsim" / "tests"
ZSIM_WL   = REPO / "zsim" / "workloads"
CFG       = REPO / "configs" / "cxl_3.1.cfg"
TRACES    = REPO / "validation" / "traces"

# ── Benchmark suite (trace-driven) ───────────────────────────────────────────
SUITE = [
    ("T1  seq-read   ",  "T1_seq_read.trace",        {}),
    ("T2  ptr-chase  ",  "T2_ptr_chase.trace",        {}),
    ("T3  rand-d1    ",  "T3_rand_depth1.trace",      {}),
    ("T3  rand-d16   ",  "T3_rand_depth16.trace",     {}),
    ("T3  rand-d64   ",  "T3_rand_depth64.trace",     {}),
    ("T6  mixed-rw   ",  "T6_mixed_rw.trace",         {}),
    ("T4  r=1        ",  "T4_seq_req1.trace",         {}),
    ("T4  r=8        ",  "T4_seq_req8.trace",         {}),
    ("T4  r=32       ",  "T4_seq_req32.trace",        {}),
    ("T7  scrub      ",  "T7_scrub_baseline.trace",   {}),
    ("T8  ppr        ",  "T8_random_ppr.trace",       {}),
    # Queued DRAM variant (heavier, exercises bank-queue logic) — Python only
    ("T4  r=8  queued",  "T4_seq_req8.trace",         {"dram.backend": "queued"}),
    ("T4  r=32 queued",  "T4_seq_req32.trace",        {"dram.backend": "queued"}),
]

# ── zsim core sweep ───────────────────────────────────────────────────────────
# config name pattern: xeon_cxl31_c{N}.cfg  (uses mt_chase2 workload)
ZSIM_CORES_DEFAULT = [4, 8, 16, 32, 64]


# ── Helpers ───────────────────────────────────────────────────────────────────

def count_requests(trace_path: Path) -> int:
    n = 0
    with open(trace_path) as f:
        for line in f:
            s = line.strip()
            if s and not s.startswith("#"):
                n += 1
    return n


def _mini_trace(src: Path, n_reqs: int, tmp_dir: Path) -> Path:
    """Return a trace containing only the first n_reqs requests from src."""
    dst = tmp_dir / f"{src.stem}_mini{n_reqs}{src.suffix}"
    if dst.exists():
        return dst
    with open(src) as f:
        body = [l for l in f if l.strip() and not l.strip().startswith("#")]
    with open(dst, "w") as out:
        out.write(f"# mini pilot {n_reqs} reqs from {src.name}\n")
        out.writelines(body[:n_reqs])
    return dst


def _pilot_seconds(cmd_fn, trace_path: Path, base_n: int,
                   tmp_dir: Path) -> float:
    """Run cmd_fn on a pilot sub-trace, return estimated wall time for
    one full pass of base_n requests.  cmd_fn(trace_path, outdir) → returncode.
    Pilot size = 10% of trace, capped at [5000, 100000], to keep startup
    overhead below ~5% of pilot wall time."""
    PILOT = max(5_000, min(base_n // 10, 100_000))
    mini  = _mini_trace(trace_path, PILOT, tmp_dir)
    odir  = tmp_dir / f"pilot_{trace_path.stem}_{id(cmd_fn)}"
    odir.mkdir(parents=True, exist_ok=True)
    t0 = time.monotonic()
    cmd_fn(mini, odir)
    elapsed = max(time.monotonic() - t0, 1e-3)
    # scale up to full trace
    return elapsed * base_n / PILOT


def _repeat_trace(src: Path, n: int, tmp_dir: Path) -> Path:
    """Return a path to a trace that repeats *src* exactly *n* times.
    If n == 1 the original file is returned unchanged (no copy made).
    Comment lines from the source are dropped in the repeated copy so the
    simulator does not see N copies of the header."""
    if n == 1:
        return src
    dst = tmp_dir / f"{src.stem}_x{n}{src.suffix}"
    with open(src) as f:
        body = [line for line in f
                if line.strip() and not line.strip().startswith("#")]
    with open(dst, "w") as out:
        out.write(f"# repeated x{n} from {src.name}\n")
        for _ in range(n):
            out.writelines(body)
    return dst


def _progress_watcher(label: str, done_event: threading.Event,
                      interval: float = 2.0) -> None:
    """Print elapsed time every `interval` seconds until done_event is set.
    Runs in a daemon thread; does not perform any I/O or CPU work beyond printing."""
    start = time.monotonic()
    while not done_event.wait(timeout=interval):
        elapsed = time.monotonic() - start
        print(f"  [{label}]  running …  elapsed {elapsed:6.1f}s", flush=True)
    elapsed = time.monotonic() - start
    print(f"  [{label}]  done       elapsed {elapsed:6.1f}s", flush=True)


# ── Python trace-driven ───────────────────────────────────────────────────────

def run_py(label: str, trace_path: Path, outdir: Path,
           overrides: dict, total_reqs: int) -> dict | None:
    cmd = [sys.executable, str(SIM_PY),
           "-c", str(CFG), "-t", str(trace_path), "-o", str(outdir)]
    for k, v in overrides.items():
        cmd += ["--set", f"{k}={v}"]

    stdout_f = outdir / "sim.log"
    stderr_f = outdir / "sim.err"
    done = threading.Event()
    watcher = threading.Thread(target=_progress_watcher,
                               args=(label.strip(), done), daemon=True)
    watcher.start()
    t0 = time.monotonic()
    with open(stdout_f, "w") as fout, open(stderr_f, "w") as ferr:
        proc = subprocess.run(cmd, stdout=fout, stderr=ferr)
    wall = time.monotonic() - t0
    done.set(); watcher.join()

    if proc.returncode != 0:
        err  = stderr_f.read_text()[-300:].strip() if stderr_f.exists() else ""
        log  = stdout_f.read_text()[-300:].strip() if stdout_f.exists() else ""
        msg  = err or log or "(no output — likely OOM-killed)"
        print(f"  [ERROR] {label.strip()} (rc={proc.returncode}): {msg}", flush=True)
        return None

    summary_path = outdir / "summary.json"
    if not summary_path.exists():
        print(f"  [ERROR] {label.strip()}: no summary.json", flush=True)
        return None

    s = json.loads(summary_path.read_text())
    reported_wall = s.get("wall_clock_s", wall)
    n_req  = s.get("total_requests", total_reqs)
    speed  = n_req / max(reported_wall, 1e-9) / 1e6

    return {
        "mode":           "python",
        "label":          label.strip(),
        "trace":          trace_path.name,
        "overrides":      overrides,
        "total_requests": n_req,
        "wall_clock_s":   round(reported_wall, 3),
        "speed_Mreqs":    round(speed, 2),
        "mean_lat_ns":    round(s["latency_ns"]["all_mean"], 1),
    }


# ── C++ trace-driven ──────────────────────────────────────────────────────────

def run_cc(label: str, trace_path: Path, outdir: Path,
           total_reqs: int) -> dict | None:
    cmd = [str(SIM_CC),
           "--config", str(CFG), "--trace", str(trace_path),
           "--output", str(outdir), "--tag", "bench"]

    stdout_f = outdir / "sim.log"
    stderr_f = outdir / "sim.err"
    done = threading.Event()
    watcher = threading.Thread(target=_progress_watcher,
                               args=(label.strip() + " [C++]", done), daemon=True)
    watcher.start()
    t0 = time.monotonic()
    with open(stdout_f, "w") as fout, open(stderr_f, "w") as ferr:
        proc = subprocess.run(cmd, stdout=fout, stderr=ferr)
    wall = time.monotonic() - t0
    done.set(); watcher.join()

    if proc.returncode != 0:
        err = stderr_f.read_text()[-300:].strip() if stderr_f.exists() else ""
        log = stdout_f.read_text()[-300:].strip() if stdout_f.exists() else ""
        msg = err or log or "(no output — likely OOM-killed)"
        print(f"  [ERROR] {label.strip()} C++ (rc={proc.returncode}): {msg}", flush=True)
        return None

    # C++ writes <tag>_stats.json
    for fname in ("bench_stats.json", "bench_summary.json", "summary.json"):
        p = outdir / fname
        if p.exists():
            s = json.loads(p.read_text())
            reported_wall = s.get("wall_clock_s", wall)
            n_req = s.get("total_requests", total_reqs)
            speed = n_req / max(reported_wall, 1e-9) / 1e6
            lat = (s.get("latency_ns", {}).get("all_mean")
                   or s.get("mean_latency_ns", 0.0))
            return {
                "mode":           "cpp",
                "label":          label.strip(),
                "trace":          trace_path.name,
                "overrides":      {},
                "total_requests": n_req,
                "wall_clock_s":   round(reported_wall, 3),
                "speed_Mreqs":    round(speed, 2),
                "mean_lat_ns":    round(float(lat), 1),
            }

    # Fallback: estimate from wall clock
    speed = total_reqs / max(wall, 1e-9) / 1e6
    return {
        "mode": "cpp", "label": label.strip(), "trace": trace_path.name,
        "overrides": {}, "total_requests": total_reqs,
        "wall_clock_s": round(wall, 3), "speed_Mreqs": round(speed, 2),
        "mean_lat_ns": 0.0,
    }


# ── zsim full-system ──────────────────────────────────────────────────────────

def _ensure_mt_chase2() -> Path | None:
    """Return path to mt_chase2 binary, compiling it if necessary."""
    binary = Path("/tmp/mt_chase2")
    if binary.exists():
        return binary
    src = ZSIM_WL / "mt_chase2.c"
    if not src.exists():
        print(f"  [WARN] mt_chase2.c not found at {src}", flush=True)
        return None
    print(f"  Compiling {src} → {binary} …", flush=True)
    r = subprocess.run(
        ["gcc", "-O2", "-fopenmp", "-o", str(binary), str(src)],
        capture_output=True, text=True)
    if r.returncode != 0:
        print(f"  [ERROR] compile failed: {r.stderr}", flush=True)
        return None
    print(f"  Compiled {binary}", flush=True)
    return binary


def _parse_zsim_mips(run_dir: Path, wall_clock_s: float,
                     n_cores: int) -> tuple[float, int]:
    """Return (MIPS, total_instrs) from zsim-ev.h5 in run_dir.
    Falls back to wall_clock_s estimate if h5py is unavailable."""
    h5_path = run_dir / "zsim-ev.h5"
    if not h5_path.exists():
        h5_path = run_dir / "zsim.h5"
    if HAS_H5PY and h5_path.exists():
        try:
            with h5py.File(h5_path, "r") as f:
                stats = f["stats"][()]
            # Accumulate instrs across all phases and cores
            total_instrs = int(stats["root"]["c"]["instrs"].sum())
            mips = total_instrs / max(wall_clock_s, 1e-9) / 1e6
            return round(mips, 2), total_instrs
        except Exception as e:
            print(f"  [WARN] h5py parse failed: {e}", flush=True)

    # Fallback: parse zsim.out for "Phase X" lines or procInstrs
    out_path = run_dir / "zsim.out"
    if out_path.exists():
        text = out_path.read_text()
        # Look for "Simulated X instrs" or similar
        m = re.search(r"(\d+)\s+instrs?", text, re.IGNORECASE)
        if m:
            total_instrs = int(m.group(1))
            return round(total_instrs / max(wall_clock_s, 1e-9) / 1e6, 2), total_instrs

    # Last resort: use maxTotalInstrs from config as estimate
    return 0.0, 0


def _patch_zsim_cfg(src: Path, max_instrs: int, tmp_dir: Path) -> Path:
    """Write a copy of *src* with maxTotalInstrs replaced by *max_instrs*.
    Returns *src* unchanged if max_instrs <= 0."""
    if max_instrs <= 0:
        return src
    text = src.read_text()
    patched = re.sub(
        r"maxTotalInstrs\s*=\s*[\dLl]+\s*;",
        f"maxTotalInstrs = {max_instrs}L;",
        text,
    )
    dst = tmp_dir / f"{src.stem}_instrs{max_instrs}{src.suffix}"
    dst.write_text(patched)
    return dst


def run_zsim(n_cores: int, run_dir: Path, max_instrs: int = 0) -> dict | None:
    cfg_name = f"xeon_cxl31_c{n_cores}.cfg"
    cfg_path = ZSIM_CFGS / cfg_name
    if not cfg_path.exists():
        print(f"  [SKIP] zsim config not found: {cfg_path}", flush=True)
        return None

    # Ensure workload binary
    wl_bin = _ensure_mt_chase2()
    if wl_bin is None:
        return None

    # Optionally override instruction count
    if max_instrs > 0:
        cfg_path = _patch_zsim_cfg(cfg_path, max_instrs, run_dir.parent)

    # Run zsim from run_dir so zsim-ev.h5 / zsim.out land there
    run_dir.mkdir(parents=True, exist_ok=True)
    label = f"zsim  CXL3.1  c={n_cores:>2}"

    stdout_f = run_dir / "zsim.log"
    stderr_f = run_dir / "zsim.err"
    done = threading.Event()
    watcher = threading.Thread(target=_progress_watcher,
                               args=(label, done, 5.0), daemon=True)
    watcher.start()
    t0 = time.monotonic()
    with open(stdout_f, "w") as fout, open(stderr_f, "w") as ferr:
        proc = subprocess.run(
            [str(ZSIM_BIN), str(cfg_path)],
            stdout=fout, stderr=ferr, cwd=str(run_dir))
    wall = time.monotonic() - t0
    done.set(); watcher.join()

    if proc.returncode != 0:
        tail = stderr_f.read_text()[-400:] if stderr_f.exists() else ""
        print(f"  [ERROR] zsim c={n_cores}: {tail}", flush=True)
        return None

    mips, total_instrs = _parse_zsim_mips(run_dir, wall, n_cores)
    if mips == 0.0 and total_instrs == 0:
        # Use maxTotalInstrs from config as fallback
        if max_instrs > 0:
            total_instrs = max_instrs * n_cores
        else:
            cfg_text = cfg_path.read_text()
            m = re.search(r"maxTotalInstrs\s*=\s*(\d+)", cfg_text)
            total_instrs = int(m.group(1)) * n_cores if m else 500_000_000 * n_cores
        mips = round(total_instrs / max(wall, 1e-9) / 1e6, 2)

    return {
        "mode":           "zsim",
        "label":          label.strip(),
        "trace":          f"mt_chase2 c={n_cores}",
        "overrides":      {"cores": n_cores},
        "total_requests": total_instrs,  # instructions
        "wall_clock_s":   round(wall, 3),
        "speed_Mreqs":    mips,          # MIPS for zsim
        "mean_lat_ns":    0.0,
    }


# ── Output ────────────────────────────────────────────────────────────────────

def fmt_row(r: dict) -> str:
    unit = "MIPS   " if r["mode"] == "zsim" else "M req/s"
    ovr  = ",".join(f"{k}={v}" for k, v in r["overrides"].items()
                    if k != "cores") or "-"
    lat  = f"  lat {r['mean_lat_ns']:>8.1f} ns" if r["mean_lat_ns"] else ""
    return (f"  {r['mode']:6s}  {r['label']:<22s}  "
            f"{r['total_requests']:>12,}  "
            f"{r['wall_clock_s']:>8.2f}s  "
            f"{r['speed_Mreqs']:>8.2f} {unit}{lat}"
            + (f"  ovr: {ovr}" if ovr != "-" else ""))


def write_results(results: list, out_dir: Path,
                  target_s: int = 0, fixed_repeat: int = 0,
                  zsim_instrs: int = 0) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)

    out_dir.joinpath("bench_simspeed_results.json").write_text(
        json.dumps(results, indent=2))

    fields = ["mode", "label", "trace", "total_requests",
              "wall_clock_s", "speed_Mreqs", "mean_lat_ns", "overrides"]
    with open(out_dir / "bench_simspeed_results.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore",
                           lineterminator="\n")
        w.writeheader()
        for r in results:
            row = dict(r); row["overrides"] = json.dumps(r["overrides"])
            w.writerow(row)

    notes = ["  all runs executed serially for isolation"]
    if fixed_repeat > 0:
        notes.append(f"  trace repeat: fixed x{fixed_repeat}")
    elif target_s > 0:
        notes.append(f"  target wall-clock per run: {target_s}s (auto-calibrated repeat)")
    if zsim_instrs > 0:
        il = f"{zsim_instrs/1e9:.1f}B" if zsim_instrs >= 1e9 else f"{zsim_instrs/1e6:.0f}M"
        notes.append(f"  zsim maxTotalInstrs/core: {il}")
    lines = [
        "CXRay Simulation Speed Benchmark",
        "=" * 82,
    ] + [f"NOTE: {n.strip()}" for n in notes] + [""]

    for mode, heading, unit in [
        ("python", "Python — trace-driven (M req/s)",   "M req/s"),
        ("cpp",    "C++    — trace-driven (M req/s)",   "M req/s"),
        ("zsim",   "zsim   — full-system   (MIPS)",     "MIPS   "),
    ]:
        rows = [r for r in results if r["mode"] == mode]
        if not rows:
            continue
        lines.append(f"── {heading} {'─'*(55-len(heading))}")
        for r in rows:
            lines.append(fmt_row(r))
        speeds = [r["speed_Mreqs"] for r in rows]
        lines.append(f"  → min {min(speeds):.2f}  max {max(speeds):.2f}  "
                     f"median {sorted(speeds)[len(speeds)//2]:.2f}  {unit}")
        lines.append("")

    # C++ vs Python speedup
    py_map = {r["trace"] + str(r["overrides"]): r["speed_Mreqs"]
              for r in results if r["mode"] == "python" and not r["overrides"]}
    cc_rows = [r for r in results if r["mode"] == "cpp"]
    if py_map and cc_rows:
        lines.append("── Speedup C++ vs Python ───────────────────────────────────────")
        for r in cc_rows:
            key = r["trace"] + str(r["overrides"])
            if key in py_map and py_map[key] > 0:
                lines.append(f"  {r['label']:<24s}  {r['speed_Mreqs'] / py_map[key]:.2f}×")
        lines.append("")

    summary = "\n".join(lines)
    print("\n" + summary)
    out_dir.joinpath("bench_simspeed_summary.txt").write_text(summary + "\n")
    print(f"Results saved to {out_dir}/")
    for fname in ("bench_simspeed_results.json",
                  "bench_simspeed_results.csv",
                  "bench_simspeed_summary.txt"):
        print(f"  {fname}")


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--output",       default=str(REPO / "results" / "bench"))
    ap.add_argument("--skip-python",  action="store_true")
    ap.add_argument("--skip-cpp",     action="store_true")
    ap.add_argument("--skip-zsim",    action="store_true")
    ap.add_argument("--traces",       nargs="+", metavar="LABEL",
                    help="Run only traces whose label contains these substrings")
    ap.add_argument("--target-seconds", type=int, default=600, metavar="S",
                    help="Target wall-clock seconds per run (default: 600 = 10 min). "
                         "A 2000-req pilot determines the repeat factor automatically. "
                         "Pass 0 to disable auto-repeat.")
    ap.add_argument("--repeat",       type=int, default=0, metavar="N",
                    help="Override: repeat each trace exactly N times (skips "
                         "auto-calibration).  0 = use --target-seconds instead.")
    ap.add_argument("--zsim-instrs",  type=int, default=5_000_000_000, metavar="N",
                    help="maxTotalInstrs per core for zsim runs.  Default: 5B.  "
                         "Set higher (e.g. 10B) for more stable throughput measurement. "
                         "Pass 0 to use the value already in the config file.")
    ap.add_argument("--cores",        nargs="+", type=int,
                    default=ZSIM_CORES_DEFAULT,
                    metavar="N", help="zsim core counts (default: 4 8 16 32 64)")
    args = ap.parse_args()

    out_dir       = Path(args.output)
    tmpdir        = Path(tempfile.mkdtemp(prefix="cxray_bench_"))
    fixed_repeat  = args.repeat          # 0 = auto
    target_s      = args.target_seconds  # 0 = no auto-repeat

    # ── Preflight checks ─────────────────────────────────────────────────────
    print("CXRay Simulation Speed Benchmark")
    print("NOTE: runs are strictly serial — start on an idle node for accuracy.")
    notes = []
    if fixed_repeat > 0:
        notes.append(f"trace repeat: fixed x{fixed_repeat} (auto-calibration disabled)")
    elif target_s > 0:
        notes.append(f"target wall-clock per run: {target_s}s — repeat factor auto-calibrated")
    zsim_instrs = args.zsim_instrs
    if zsim_instrs > 0:
        il = f"{zsim_instrs/1e9:.1f}B" if zsim_instrs >= 1e9 else f"{zsim_instrs/1e6:.0f}M"
        notes.append(f"zsim maxTotalInstrs per core: {il}")
    for n in notes:
        print(f"NOTE: {n}")
    print()

    py_ok   = SIM_PY.exists()
    cc_ok   = SIM_CC.exists()
    zsim_ok = ZSIM_BIN.exists()
    if not py_ok:   print(f"[WARN] Python sim not found at {SIM_PY} — skipping Python runs")
    if not cc_ok:   print(f"[WARN] C++ binary not found at {SIM_CC} — skipping")
    if not zsim_ok: print(f"[WARN] zsim binary not found at {ZSIM_BIN} — skipping")
    if not HAS_H5PY and zsim_ok:
        print("[WARN] h5py not installed — zsim MIPS will be estimated from wall clock")

    # ── Filter trace suite ───────────────────────────────────────────────────
    suite = SUITE
    if args.traces:
        suite = [(l, t, o) for l, t, o in SUITE
                 if any(f in l for f in args.traces)]

    # ── Pre-count requests ───────────────────────────────────────────────────
    print("Pre-counting requests in trace files …")
    req_cache: dict[str, int] = {}   # base counts (single-pass)
    for _, tname, _ in suite:
        tpath = TRACES / tname
        if not tpath.exists():
            continue
        if tname not in req_cache:
            base = count_requests(tpath)
            req_cache[tname] = base
            print(f"  {tname}: {base:,}")

    # ── Count total runs for progress display ────────────────────────────────
    n_py   = 0 if (args.skip_python or not py_ok) else sum(1 for _, t, _ in suite if (TRACES/t).exists())
    n_cc   = 0 if (args.skip_cpp or not cc_ok) else sum(
                 1 for _, t, o in suite if (TRACES/t).exists() and not o)
    n_zsim = 0 if (args.skip_zsim or not zsim_ok) else sum(
                 1 for c in args.cores
                 if (ZSIM_CFGS / f"xeon_cxl31_c{c}.cfg").exists())
    total_runs = n_py + n_cc + n_zsim
    run_idx    = 0

    results: list[dict] = []

    # ── Python ───────────────────────────────────────────────────────────────
    if not args.skip_python and py_ok:
        print(f"\n{'='*60}\nPython trace-driven\n{'='*60}")
        for lbl, tname, ovr in suite:
            tpath = TRACES / tname
            if not tpath.exists(): continue
            run_idx += 1
            base_n = req_cache.get(tname, 0)

            # Python stores every latency value in memory — repeating the trace
            # causes OOM for large traces.  Always run exactly one pass.
            repeat = fixed_repeat if fixed_repeat > 0 else 1

            n         = base_n * repeat
            tpath_eff = _repeat_trace(tpath, repeat, tmpdir)
            ovr_str   = (" [" + ",".join(f"{k}={v}" for k,v in ovr.items()) + "]"
                         if ovr else "")
            print(f"\n[{run_idx}/{total_runs}] Python  {lbl.strip()}{ovr_str}"
                  f"  ({n:,} reqs ×{repeat})")
            odir = tmpdir / f"py_{lbl.strip().replace(' ','_')}_{tname}"
            odir.mkdir(parents=True, exist_ok=True)
            r = run_py(lbl, tpath_eff, odir, ovr, n)
            if r:
                results.append(r)
                print(f"  → {r['speed_Mreqs']:.2f} M req/s  "
                      f"({r['wall_clock_s']:.2f}s  lat {r['mean_lat_ns']:.1f} ns)")

    # ── C++ ──────────────────────────────────────────────────────────────────
    if not args.skip_cpp and cc_ok:
        print(f"\n{'='*60}\nC++ standalone binary\n{'='*60}")
        for lbl, tname, ovr in suite:
            if ovr: continue   # C++ binary doesn't support --set overrides
            tpath = TRACES / tname
            if not tpath.exists(): continue
            run_idx += 1
            base_n = req_cache.get(tname, 0)

            # ── auto-calibrate repeat ────────────────────────────────────────
            if fixed_repeat > 0:
                repeat = fixed_repeat
            elif target_s > 0:
                def _cc_cmd(tp, od):
                    cmd = [str(SIM_CC), "--config", str(CFG),
                           "--trace", str(tp), "--output", str(od), "--tag", "bench"]
                    with open(od / "p.log", "w") as f:
                        subprocess.run(cmd, stdout=f, stderr=f)
                est = _pilot_seconds(_cc_cmd, tpath, base_n, tmpdir)
                repeat = max(1, math.ceil(target_s / est))
                print(f"  pilot: ~{est:.1f}s/pass → ×{repeat} "
                      f"(~{est*repeat:.0f}s)", flush=True)
            else:
                repeat = 1

            n         = base_n * repeat
            tpath_eff = _repeat_trace(tpath, repeat, tmpdir)
            print(f"\n[{run_idx}/{total_runs}] C++    {lbl.strip()}  "
                  f"({n:,} reqs ×{repeat})")
            odir = tmpdir / f"cc_{lbl.strip().replace(' ','_')}_{tname}"
            odir.mkdir(parents=True, exist_ok=True)
            r = run_cc(lbl, tpath_eff, odir, n)
            if r:
                results.append(r)
                print(f"  → {r['speed_Mreqs']:.2f} M req/s  "
                      f"({r['wall_clock_s']:.2f}s  lat {r['mean_lat_ns']:.1f} ns)")

    # ── zsim full-system ─────────────────────────────────────────────────────
    if not args.skip_zsim and zsim_ok:
        zsim_instrs = args.zsim_instrs
        instrs_label = (f"{zsim_instrs/1e9:.1f}B" if zsim_instrs >= 1e9
                        else f"{zsim_instrs/1e6:.0f}M") if zsim_instrs > 0 else "cfg default"
        print(f"\n{'='*60}\nzsim full-system (mt_chase2, CXL 3.1)  [{instrs_label} instrs/core]\n{'='*60}")
        for c in args.cores:
            cfg_path = ZSIM_CFGS / f"xeon_cxl31_c{c}.cfg"
            if not cfg_path.exists(): continue
            run_idx += 1
            total_i = (zsim_instrs * c) if zsim_instrs > 0 else 0
            i_str = f"{total_i/1e9:.1f}B" if total_i >= 1e9 else (f"{total_i/1e6:.0f}M" if total_i else "?")
            print(f"\n[{run_idx}/{total_runs}] zsim  cores={c}  "
                  f"({instrs_label} instrs/core → {i_str} total)")
            run_dir = tmpdir / f"zsim_c{c}"
            r = run_zsim(c, run_dir, max_instrs=zsim_instrs)
            if r:
                results.append(r)
                print(f"  → {r['speed_Mreqs']:.2f} MIPS  "
                      f"({r['wall_clock_s']:.2f}s  "
                      f"{r['total_requests']:,} instrs)")

    if not results:
        print("\n[ERROR] No results collected.", file=sys.stderr); sys.exit(1)

    write_results(results, out_dir, target_s=target_s,
                  fixed_repeat=fixed_repeat, zsim_instrs=zsim_instrs)


if __name__ == "__main__":
    main()
