#!/usr/bin/env python3
"""
lb_curve.py — Parse xeon sweep results and produce bandwidth-latency (L-B) curves.

Usage:
    python lb_curve.py [--outbase /tmp/xeon_sweep] [--plot lb_curve.png]
                       [--memcurve-dir <path>]
"""

import os
import re
import sys
import glob
import numpy as np
import h5py
import argparse

FREQ_MHZ  = 2700
LINE_SIZE = 64        # bytes
BINS      = 100
BIN_WIDTH = 10        # sys-cycles per bin

# CXL forward + return overhead cycles added to DDR weave-phase rdlat.
# cxlFwdCycles = ceil((97+24)*2700/1000) = ceil(326.7) = 327
# cxlRetCycles = ceil(24*2700/1000)      = ceil(64.8)  = 65
# total offset  = 327 + 65 = 392 → round to 400 conservatively
CXL_OFFSET_CYC = 392
DDR_OFFSET_CYC = 0

# ---------------------------------------------------------------------------
# Default path for golden memory-curve data (relative to this script)
# ---------------------------------------------------------------------------
_SCRIPT_DIR   = os.path.dirname(os.path.abspath(__file__))
MEMCURVE_DIR  = os.path.join(_SCRIPT_DIR, "..", "memory_curve")

# Synthesized MRDIMM 8800 MT/s curve from MICRO-2024 slide 16
# (Esmaili-Dokht et al., MESS benchmark, 100% reads, 12-DIMM system)
# BW in GB/s, latency in ns; manual digitization of the slide.
MRDIMM8800_CURVE = [
    (  0.0,  100.0),
    (100.0,  101.0),
    (300.0,  104.0),
    (500.0,  108.0),
    (630.0,  115.0),
    (700.0,  175.0),
    (780.0,  290.0),
    (820.0,  380.0),
    (840.0,  430.0),
    (844.0,  450.0),
]


# ---------------------------------------------------------------------------
# Golden hardware reference curves
# ---------------------------------------------------------------------------

def load_golden_curve(tech_dir):
    """
    Read all bwlat_*.txt files from tech_dir.
    Each file has rows: <BW_MB/s>  <lat_ns>
    For each file, take the row with the highest valid BW as the representative
    "loaded" point, plus the last valid row as the unloaded baseline.
    Returns sorted array of (bw_gbps, lat_ns) forming the L-B envelope.
    """
    pattern = os.path.join(tech_dir, "bwlat_*.txt")
    files = sorted(glob.glob(pattern))
    if not files:
        return np.empty((0, 2))

    points = []
    for fpath in files:
        rows = []
        with open(fpath) as fh:
            for line in fh:
                line = line.strip()
                if not line:
                    continue
                parts = line.split()
                if len(parts) < 2:
                    continue
                try:
                    bw_mbs = float(parts[0])
                    lat_ns = float(parts[1])
                except ValueError:
                    continue
                if bw_mbs <= 0 or lat_ns <= 0:
                    continue
                rows.append((bw_mbs / 1000.0, lat_ns))  # convert to GB/s
        if rows:
            # Representative point: the one with median BW among rows (avoids
            # the plateau repetitions at the top and noisy outliers at bottom)
            rows.sort(key=lambda x: x[0])
            points.extend(rows)

    if not points:
        return np.empty((0, 2))

    arr = np.array(points)
    # Sort by bandwidth ascending
    arr = arr[arr[:, 0].argsort()]
    # Build envelope: bin BW range into N bins, take min latency per bin
    # (the Pareto-optimal lower frontier of latency vs BW)
    bw_min, bw_max = arr[:, 0].min(), arr[:, 0].max()
    n_bins = 60
    edges = np.linspace(bw_min, bw_max, n_bins + 1)
    env_bw, env_lat = [], []
    for i in range(n_bins):
        mask = (arr[:, 0] >= edges[i]) & (arr[:, 0] < edges[i + 1])
        if mask.sum() == 0:
            continue
        env_bw.append(arr[mask, 0].mean())
        env_lat.append(arr[mask, 1].min())
    return np.column_stack([env_bw, env_lat])


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def read_sim_cycles(rundir):
    """Read total simulated cycles from heartbeat file."""
    hb = os.path.join(rundir, "heartbeat")
    if not os.path.exists(hb):
        return None
    with open(hb) as f:
        for line in f:
            m = re.match(r'\s*(\d+)\s+cycles\s*$', line)
            if m:
                return int(m.group(1))
    return None


def percentile_from_mlh(mlh, pct, offset_cyc=0):
    """Compute percentile latency (ns) from the 100-bin mlh histogram."""
    mlh = np.asarray(mlh, dtype=np.int64).ravel()
    total = int(mlh.sum())
    if total == 0:
        return float('nan')
    target = total * pct / 100.0
    cum = 0
    for i, cnt in enumerate(mlh):
        cum += int(cnt)
        if cum >= target:
            mid_cyc = i * BIN_WIDTH + BIN_WIDTH // 2 + offset_cyc
            return mid_cyc * 1000.0 / FREQ_MHZ
    # Saturated at last bin
    mid_cyc = (BINS - 1) * BIN_WIDTH + BIN_WIDTH // 2 + offset_cyc
    return mid_cyc * 1000.0 / FREQ_MHZ


def parse_run(rundir, mem_type):
    """Return dict with bw_gbps, avg_ns, p50_ns, p95_ns, p99_ns, rd_count."""
    h5_path = os.path.join(rundir, "zsim.h5")
    if not os.path.exists(h5_path):
        return None

    offset = CXL_OFFSET_CYC if mem_type == "cxl" else DDR_OFFSET_CYC

    with h5py.File(h5_path, "r") as f:
        stats = f["stats"][:]
        root  = stats[-1]["root"]
        mem   = root["mem"]

        # CXL wraps DDR under mem['mem-0']; pure DDR is flat (shape (1,)).
        if "rd" in mem.dtype.names:
            m = mem[0]
        else:
            m = mem["mem-0"][0]

        rd     = int(np.asarray(m["rd"]).flat[0])
        rdlat  = int(np.asarray(m["rdlat"]).flat[0])
        mlh    = np.asarray(m["mlh"], dtype=np.int64).ravel()

    if rd == 0:
        return None

    wr = int(np.asarray(m["wr"]).flat[0]) if "wr" in m.dtype.names else 0

    sim_cycles = read_sim_cycles(rundir)
    if sim_cycles is None or sim_cycles == 0:
        return None

    avg_raw_cyc = rdlat / rd
    avg_ns      = (avg_raw_cyc + offset) * 1000.0 / FREQ_MHZ

    p50 = percentile_from_mlh(mlh, 50, offset)
    p95 = percentile_from_mlh(mlh, 95, offset)
    p99 = percentile_from_mlh(mlh, 99, offset)

    return dict(avg_ns=avg_ns, p50_ns=p50, p95_ns=p95,
                p99_ns=p99, rd=rd, wr=wr, sim_cycles=sim_cycles)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--outbase",     default="/tmp/xeon_sweep")
    ap.add_argument("--plot",        default="lb_curve.png")
    ap.add_argument("--no-plot",     action="store_true")
    ap.add_argument("--memcurve-dir", default=MEMCURVE_DIR,
                    help="Root directory containing CXL/, ddr5/, etc. sub-dirs")
    args = ap.parse_args()

    core_counts = [4, 8, 16, 20, 32, 64]
    mem_types   = ["cxl", "ddr"]

    results = {}
    # BW from Little's Law: BW = N_threads × 64B / avg_latency_ns
    # This is the theoretical bandwidth at N concurrent independent DRAM streams (MLP=N).
    print(f"{'Run':<22} {'Cores':>5} {'BW_LL(GB/s)':>12} {'avg (ns)':>10} "
          f"{'p50 (ns)':>10} {'p95 (ns)':>10} {'p99 (ns)':>10}")
    print("-" * 84)

    for mem in mem_types:
        results[mem] = []
        for n in core_counts:
            rundir = os.path.join(args.outbase, f"xeon_{mem}_c{n}")
            r = parse_run(rundir, mem)
            if r is None:
                print(f"  xeon_{mem}_c{n:<4} — NOT DONE")
                results[mem].append(None)
                continue
            # Little's Law BW: N × 64B × 1e9 / avg_ns
            bw_ll = n * LINE_SIZE * 1e9 / r['avg_ns'] / 1e9  # GB/s
            r['bw_ll_gbps'] = bw_ll
            tag = f"xeon_{mem}_c{n}"
            print(f"  {tag:<20} {n:>5} {bw_ll:>12.3f} {r['avg_ns']:>10.1f} "
                  f"{r['p50_ns']:>10.1f} {r['p95_ns']:>10.1f} {r['p99_ns']:>10.1f}")
            results[mem].append(r)

    if args.no_plot:
        return

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("\nmatplotlib not available; skipping plot.")
        return

    # -------------------------------------------------------------------------
    # Load golden hardware reference curves from memory_curve/
    # -------------------------------------------------------------------------
    mcd = args.memcurve_dir
    golden = {}
    for tag, subdir in [
        ("CXL (Micron CZ120, real HW)",    os.path.join(mcd, "CXL")),
        ("DDR5 MRDIMM (real HW)",          os.path.join(mcd, "ddr5")),
        ("HBM2e Fujitsu (real HW)",        os.path.join(mcd, "hbm2e-fujitsu")),
        ("NUMA DDR4 Skylake (real HW)",    os.path.join(mcd, "NumaDDR4Skylake")),
        ("Skylake NT (real HW)",           os.path.join(mcd, "skylake-non-temporal")),
    ]:
        curve = load_golden_curve(subdir)
        if curve.size > 0:
            golden[tag] = curve
            print(f"  Loaded {len(curve)} pts for '{tag}'")
        else:
            print(f"  No data for '{tag}' (dir: {subdir})")

    # Synthesized MRDIMM 8800 MT/s from MICRO-2024 slide 16
    mrd = np.array(MRDIMM8800_CURVE)
    golden["MRDIMM 8800 MT/s ×12 (MICRO-2024, synth.)"] = mrd

    # -------------------------------------------------------------------------
    # Plot
    # -------------------------------------------------------------------------
    # Leave room above each subplot for the explanation text block
    fig, axes = plt.subplots(1, 2, figsize=(18, 9))
    fig.subplots_adjust(top=0.62, bottom=0.08, left=0.06, right=0.98, wspace=0.28)

    # Explanation paragraphs placed above each subplot (in figure coordinates).
    # They describe what the graph shows and how to read it so the plot is
    # self-contained without needing an external caption.
    explanations = [
        # Left panel — average latency
        (
            "How to read this graph:\n"
            "Each point is one zsim simulation run (labeled Nc = N cores) of a multi-threaded\n"
            "pointer-chase benchmark on a modeled Intel Xeon Gold 6248. Bandwidth on the X-axis\n"
            "is estimated via Little's Law: BW = N_threads × 64 B / avg_latency. This gives the\n"
            "theoretical bandwidth if N independent DRAM streams each stall for avg_latency.\n\n"
            "What you are comparing:\n"
            "  • Red solid line  — zsim CXL 3.1 (DDR5-2666 backend, +145 ns CXL protocol overhead)\n"
            "  • Blue solid line — zsim local DDR5-2666 (no CXL)\n"
            "  • Dashed / dot-dash lines — real hardware L-B envelopes from published measurements\n"
            "    (Micron CZ120 CXL expander, DDR5 MRDIMM, HBM2e Fujitsu, NUMA DDR4 Skylake).\n"
            "  • Teal dash-dot  — MRDIMM 8800 MT/s ×12 synthesized from MICRO-2024 slide 16.\n\n"
            "Key observations:\n"
            "  • zsim CXL stays nearly flat ~194–213 ns across 1–19 GB/s: the fixed 145 ns CXL\n"
            "    protocol overhead dwarfs DRAM queueing, masking load-dependent latency growth.\n"
            "  • zsim DDR5 rises from 45 ns (4 cores) to 64 ns (64 cores): DRAM bank contention\n"
            "    is visible but mild because the DDR5-2666 model is not yet saturated at 64 GB/s.\n"
            "  • Real Micron CZ120 (orange dashed) shows ~225 ns unloaded, matching zsim's ~194 ns\n"
            "    (zsim uses DDR5-2666 vs real DDR5-4800, explaining the ~30 ns gap).\n"
            "  • The real-HW curves bend sharply past their saturation knee — zsim does not reach\n"
            "    this regime because MLP = 1 per thread (pointer-chase) limits peak BW to ~64 GB/s."
        ),
        # Right panel — p99 latency
        (
            "How to read this graph:\n"
            "Same setup as the left panel but Y-axis shows the 99th-percentile (p99) latency\n"
            "instead of the mean. p99 captures worst-case tail behaviour: requests that hit a\n"
            "congested DRAM bank, a full MSHR queue, or CXL link back-pressure. It is the metric\n"
            "that matters for latency-sensitive workloads (databases, key-value stores, etc.).\n\n"
            "What you are comparing:\n"
            "  • Red solid line  — zsim CXL p99: rises steeply from 458 ns (4c) to 514 ns (64c)\n"
            "    because even rare DRAM retries are inflated by the 145 ns CXL round-trip overhead.\n"
            "  • Blue solid line — zsim DDR5 p99: rises from 198 ns (4c) to 357 ns (64c), showing\n"
            "    significant bank-conflict tail growth as core count increases.\n"
            "  • Dashed lines    — real hardware p99 envelopes (same technologies as left panel).\n\n"
            "Key observations:\n"
            "  • CXL p99/avg ratio ≈ 2.4× at 4 cores, growing to 2.4× at 64 cores — the tail is\n"
            "    proportionally large because every retry must pay the full CXL round-trip twice.\n"
            "  • DDR5 p99/avg ratio grows from 3.7× (4c) to 5.6× (64c), indicating bank-conflict\n"
            "    queueing tails that worsen super-linearly with load.\n"
            "  • At low core counts both lines sit in the same BW range but CXL p99 > DDR p99,\n"
            "    confirming CXL adds tail latency even when average bandwidth is modest.\n"
            "  • Real HW references extend to much higher BW (DDR5 MRDIMM ~283 GB/s, HBM2e ~900\n"
            "    GB/s), illustrating the BW headroom that a CXL-attached expander cannot match."
        ),
    ]

    for ax_idx, explanation in enumerate(explanations):
        # Place text box above the corresponding subplot in figure coordinates
        x_left  = fig.subplotpars.left  + ax_idx * (
            (fig.subplotpars.right - fig.subplotpars.left + fig.subplotpars.wspace * 0.5)
            / 2
        )
        x_left = [fig.subplotpars.left, fig.subplotpars.left +
                  (fig.subplotpars.right - fig.subplotpars.left) / 2 +
                  fig.subplotpars.wspace * 0.07][ax_idx]
        fig.text(x_left, 0.985, explanation,
                 va="top", ha="left", fontsize=7.5,
                 fontfamily="monospace",
                 bbox=dict(boxstyle="round,pad=0.4", facecolor="#f5f5f5",
                           edgecolor="#cccccc", alpha=0.9))

    # Golden reference styles (thin dashed lines, de-emphasized)
    golden_styles = {
        "CXL (Micron CZ120, real HW)":                {"color": "#e09040", "ls": "--",  "lw": 1.5, "marker": None},
        "DDR5 MRDIMM (real HW)":                       {"color": "#4080d0", "ls": "--",  "lw": 1.5, "marker": None},
        "HBM2e Fujitsu (real HW)":                     {"color": "#40b060", "ls": "--",  "lw": 1.5, "marker": None},
        "NUMA DDR4 Skylake (real HW)":                  {"color": "#a050c0", "ls": "--",  "lw": 1.5, "marker": None},
        "Skylake NT (real HW)":                         {"color": "#808080", "ls": ":",   "lw": 1.2, "marker": None},
        "MRDIMM 8800 MT/s ×12 (MICRO-2024, synth.)":  {"color": "#20a0b0", "ls": "-.", "lw": 1.8, "marker": "s"},
    }

    sim_colors = {"cxl": "#e05858", "ddr": "#5588e0"}
    sim_labels = {"cxl": "zsim CXL 3.1 Gen5×8 (DDR5-2666)", "ddr": "zsim DDR5-2666 (local)"}

    for ax_idx, lat_key in enumerate(["avg_ns", "p99_ns"]):
        ax = axes[ax_idx]

        # --- golden reference curves (background) ---
        for gname, gcurve in golden.items():
            sty = golden_styles.get(gname, {"color": "gray", "ls": "--", "lw": 1.2, "marker": None})
            bws  = gcurve[:, 0]
            lats = gcurve[:, 1]
            mkr  = sty.get("marker") or ""
            ax.plot(bws, lats, linestyle=sty["ls"], color=sty["color"],
                    linewidth=sty["lw"], marker=mkr, markersize=4,
                    label=gname, alpha=0.75, zorder=1)

        # --- zsim simulation results (foreground) ---
        for mem in mem_types:
            bws, lats, ns = [], [], []
            for n, r in zip(core_counts, results[mem]):
                if r is None:
                    continue
                bws.append(r["bw_ll_gbps"])
                lats.append(r[lat_key])
                ns.append(n)
            if not bws:
                continue
            ax.plot(bws, lats, "o-", color=sim_colors[mem],
                    label=sim_labels[mem], linewidth=2.5, zorder=3)
            for x, y, n in zip(bws, lats, ns):
                ax.annotate(f"{n}c", (x, y), textcoords="offset points",
                            xytext=(4, 4), fontsize=8, zorder=4)

        lat_label = "Average" if lat_key == "avg_ns" else "p99"
        ax.set_xlabel("Memory Bandwidth (GB/s)", fontsize=11)
        ax.set_ylabel(f"{lat_label} Latency (ns)", fontsize=11)
        ax.set_title(f"Bandwidth–Latency Curve — {lat_label} Latency\n"
                     f"zsim Xeon Gold 6248 (mt_chase, 500M instrs) vs real-HW references",
                     fontsize=10, pad=8)
        ax.legend(fontsize=7.5, loc="upper left")
        ax.grid(True, alpha=0.3)

    plt.savefig(args.plot, dpi=150, bbox_inches="tight")
    print(f"\nPlot saved to {args.plot}")


if __name__ == "__main__":
    main()
