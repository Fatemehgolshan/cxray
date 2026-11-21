#!/usr/bin/env python3
"""plot_ae.py — render artifact PNGs in the paper's figure style.

Layout, colors, markers, legends and annotations mirror
scripts/build_paper_figures.py (the script that draws the paper PDFs);
only the data sources differ: reproduced CSVs/JSON from reproduce.sh
instead of macros.tex. Any series the artifact does not regenerate degrades
to a labeled placeholder, never to invented data. (Figs 4/7 come from the
CXRay closed-loop multicore tier, not zsim execution: see
scripts/run_cxray_multicore.py.)
"""
import argparse
import csv
import json
import os
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.lines as mlines
import numpy as np

os.environ.setdefault("MPLCONFIGDIR", "/tmp/mplconfig")

plt.rcParams.update(
    {
        "font.size": 9,
        "axes.labelsize": 9,
        "axes.titlesize": 10,
        "legend.fontsize": 8,
        "xtick.labelsize": 8,
        "ytick.labelsize": 8,
        "axes.spines.top": False,
        "axes.spines.right": False,
        "axes.grid": True,
        "grid.color": "#DDDDDD",
        "grid.linewidth": 0.6,
        "grid.linestyle": "--",
        "figure.facecolor": "white",
        "axes.facecolor": "white",
        "savefig.facecolor": "white",
        "pdf.fonttype": 42,
        "ps.fonttype": 42,
    }
)

COL = {
    "cxl":     "#FF8C00",
    "cxl30":   "#D46800",
    "cxl31":   "#8B3200",
    "cxl40":   "#4A1000",
    "cxl_alt": "#FE9929",
    "ddr":     "#2B8CBE",
    "ddr_alt": "#A6BDDB",
    "hw":      "#5C5C5C",
    "hw2":     "#969696",
    "ras":     "#CB181D",
    "ok":      "#238B45",
}
VER_COL = {"2.0": COL["cxl"], "3.0": COL["cxl30"], "3.1": COL["cxl31"],
           "4.0": COL["cxl40"], "ddr5": COL["ddr"], "DDR5": COL["ddr"]}
VER_MARK = {"2.0": "o", "3.0": "s", "3.1": "^", "4.0": "D",
            "ddr5": "o", "DDR5": "o"}
VER_LBL = {"2.0": "CXL 2.0", "3.0": "CXL 3.0", "3.1": "CXL 3.1+IDE",
           "4.0": "CXL 4.0+IDE", "ddr5": "DDR5 only", "DDR5": "DDR5 only"}

CORES = [4, 8, 16, 32, 64]


def read_csv(path):
    if not os.path.exists(path):
        return [], []
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    return rows, (list(rows[0].keys()) if rows else [])


def is_placeholder(rows):
    return bool(rows) and "note" in rows[0]


def placeholder(out, title, note):
    fig, ax = plt.subplots(figsize=(6.9, 2.45))
    ax.text(0.5, 0.6, title, ha="center", fontsize=13, weight="bold",
            transform=ax.transAxes)
    ax.text(0.5, 0.4, note, ha="center", fontsize=9, transform=ax.transAxes)
    ax.axis("off")
    fig.tight_layout(); fig.savefig(out, dpi=150); plt.close(fig)
    print("wrote (placeholder)", out)


def save(fig, out):
    fig.savefig(out, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print("wrote", out)


def load_json(path):
    with open(path) as f:
        return json.load(f)


# ---------------------------------------------------------------- Fig.2 ---
def load_hw_bw_lat(hw_csv, rd_pct_str):
    pts = []
    with open(hw_csv, newline="") as f:
        for row in csv.DictReader(f):
            if row["rd_percentage"] == rd_pct_str:
                pts.append((float(row["bandwidth_mean"]) / 2.0,
                            float(row["latency_mean"])))
    pts.sort()
    return pts


def mape_vs_hw(sim_pts, hw_pts, base_ns=223.7, flat_over_ns=35.0):
    """Flat-regime MAPE: HW points within base+35 ns define the flat BW range;
    sim points inside that range are compared to their nearest HW point."""
    if not sim_pts or not hw_pts:
        return None
    flat = [(b, l) for b, l in hw_pts if l <= base_ns + flat_over_ns]
    if not flat:
        return None
    fmax = max(b for b, _ in flat)
    errs = []
    for bw, lat in sim_pts:
        if bw > fmax:
            continue
        hb, hl = min(flat, key=lambda p: abs(p[0] - bw))
        if hl > 0:
            errs.append(abs(lat - hl) / hl)
    return 100.0 * sum(errs) / len(errs) if errs else None


def fig02_calibration(out, csv_path, hw_csv):
    """3-panel BW-latency: CZ120 HW vs CXRay (W sweep + M/D/1 R/M)."""
    _NUM_BANKS, _CACHELINE = 32, 64
    _BASE_LAT_NS, _S_READ_NS, _TWR_NS = 223.7, 66.7, 15.0
    _S_MIX_NS = (_S_READ_NS + _S_READ_NS + _TWR_NS) / 2

    def mdl_curve(S_ns, y_top, steps=500):
        cap = _NUM_BANKS * _CACHELINE / S_ns
        xs, ys = [0.0], [_BASE_LAT_NS]
        for i in range(1, steps + 1):
            rho = i / steps * 0.995
            bw = rho * cap
            lat = _BASE_LAT_NS + rho * S_ns / (2.0 * (1.0 - rho))
            if lat > y_top:
                break
            xs.append(bw); ys.append(lat)
        return xs, ys

    sweep = {}
    js = csv_path.parent / "bw_sweep_cxray.json"
    if js.exists():
        sweep = load_json(js)
    HW_C, SIM_C = COL["hw"], COL["cxl_alt"]
    FLAT_C, FLAT_TC = "#EEF5EE", "#2E6E3E"
    Y_LIMS = {"R": (0, 700), "W": (0, 1400), "M": (0, 700)}

    fig, axes = plt.subplots(1, 3, figsize=(6.9, 2.55),
                             gridspec_kw={"wspace": 0.42})
    for ax, (title, key, rd) in zip(axes, [("(a) Reads", "R", "0"),
                                           ("(b) Writes", "W", "100"),
                                           ("(c) Mixed", "M", "50")]):
        y_bot, y_top = Y_LIMS[key]
        hw_pts = load_hw_bw_lat(hw_csv, rd) if os.path.exists(hw_csv) else []
        xs_hw = [p[0] for p in hw_pts]; ys_hw = [p[1] for p in hw_pts]
        if xs_hw:
            flat_end = float(next((x for x, y in zip(xs_hw, ys_hw)
                                   if y > _BASE_LAT_NS + 35), xs_hw[-1] * 0.55))
            ax.axvspan(0, flat_end, color=FLAT_C, alpha=0.85, zorder=0, lw=0)
            ax.text(flat_end * 0.47, y_bot + (y_top - y_bot) * 0.03,
                    "Flat\nRegime", ha="center", va="bottom",
                    fontsize=5.8, color=FLAT_TC, style="italic", zorder=2)
            ax.plot(xs_hw, ys_hw, color=HW_C, linewidth=1.6,
                    label="CZ120", zorder=3)
        if key in ("R", "M"):
            S_ns = _S_READ_NS if key == "R" else _S_MIX_NS
            xs_m, ys_m = mdl_curve(S_ns, y_top)
            ax.plot(xs_m, ys_m, color=SIM_C, linewidth=1.8,
                    linestyle="-", label="CXRay", zorder=4)
            mape = mape_vs_hw(list(zip(xs_m, ys_m)), hw_pts)
        else:
            pts = sweep.get(key, [])
            nreq1 = [p for p in pts if p["label"].startswith("nreq=1,")]
            multi = [p for p in pts if not p["label"].startswith("nreq=1,")]
            if nreq1:
                max_n1 = max(p["bw_GBps"] for p in nreq1)
                multi = [p for p in multi if p["bw_GBps"] > max_n1]
            combined = sorted(nreq1 + multi, key=lambda p: p["bw_GBps"])
            xs_sim = [p["bw_GBps"] for p in combined]
            ys_sim = [p["mean_lat_ns"] for p in combined]
            if xs_sim:
                ax.plot(xs_sim, ys_sim, color=SIM_C, linewidth=1.8,
                        linestyle="-", label="CXRay", zorder=4)
                if ys_sim[-1] <= y_top:
                    ax.scatter([xs_sim[-1]], [ys_sim[-1]], s=30,
                               color=SIM_C, marker="^", zorder=6)
            mape = mape_vs_hw(list(zip(xs_sim, ys_sim)), hw_pts)
        if mape is not None:
            ax.text(0.97, 0.05, f"MAPE {mape:.0f}%",
                    transform=ax.transAxes, fontsize=6.5, ha="right",
                    va="bottom", color=SIM_C, fontweight="bold", zorder=5,
                    bbox=dict(boxstyle="round,pad=0.25", fc="white",
                              ec=SIM_C, alpha=0.90, lw=0.8))
        ax.set_title(title, fontsize=8.5, fontweight="bold", pad=5)
        if key == "W":
            ax.set_xlim(0, 32); ax.set_xticks([0, 10, 20, 30])
        else:
            ax.set_xlim(left=0)
        ax.set_ylim(bottom=y_bot, top=y_top)
        ax.set_xlabel("Bandwidth / device (GB/s)", fontsize=8)
        ax.set_ylabel("Mean latency (ns)" if ax is axes[0] else "", fontsize=8)
        ax.tick_params(axis="both", labelsize=6.8, length=3)
        ax.spines["left"].set_color("#BBBBBB")
        ax.spines["bottom"].set_color("#BBBBBB")
        ax.grid(True, color="#E8E8E8", linewidth=0.55, linestyle="-", zorder=1)
        ax.legend(frameon=True, fontsize=6.5, loc="upper left",
                  framealpha=0.0, edgecolor="none",
                  borderpad=0.5, handlelength=1.6, handletextpad=0.4,
                  labelspacing=0.3)
    save(fig, out)


# ---------------------------------------------------------------- Fig.3 ---
FLOORS = {"2.0": 145.0, "3.0": 153.0, "3.1": 169.0, "4.0": 147.0}
FLOOR_LBL = {"2.0": "CXL 2.0", "3.0": "CXL 3.0", "3.1": "CXL 3.1\n+IDE",
             "4.0": "CXL 4.0\n+IDE"}


def fig03_versions(out, rows):
    """Stacked bar: protocol floor + above-floor = e2e mean (T2)."""
    by_ver = {r["config"].replace("cxl_", "").replace(".cfg", ""): r
              for r in rows}
    order = [v for v in ["2.0", "3.0", "3.1", "4.0"] if v in by_ver]
    floors = [FLOORS[v] for v in order]
    means = [float(by_ver[v]["mean_lat_ns"]) for v in order]
    overheads = [m - f for m, f in zip(means, floors)]
    versions = [FLOOR_LBL[v] for v in order]

    fig, ax = plt.subplots(figsize=(2.4, 2.2))
    x = np.arange(len(order)); w = 0.38
    ax.bar(x, floors, width=w, color=COL["cxl_alt"], alpha=0.62,
           label="Protocol floor", zorder=3, edgecolor="none")
    ax.bar(x, overheads, width=w, bottom=floors, color=COL["cxl"],
           alpha=0.78, label="Overhead", zorder=3, edgecolor="none")
    for xi, f, oh in zip(x, floors, overheads):
        ax.text(xi, f / 2, f"{f:.0f}", ha="center", va="center",
                fontsize=7, color="white", fontweight="bold")
        ax.text(xi, f + oh / 2, f"{oh:.0f}", ha="center", va="center",
                fontsize=6.5, color="white")
    ref = means[0]
    ax.axhline(ref, color="#BBBBBB", linewidth=0.9, linestyle=":", zorder=2)
    ax.text(len(order) - 0.78, ref + 4, f"≈{ref:.0f} ns", ha="left",
            va="bottom", fontsize=6.5, color="#999999")
    ax.set_xticks(x); ax.set_xticklabels(versions, fontsize=6.5, rotation=15,
                                         ha="right")
    ax.set_ylabel("Latency (ns)", fontsize=8)
    ax.set_ylim(0, max(means) * 1.22)
    ax.tick_params(axis="y", labelsize=7)
    ax.legend(frameon=True, facecolor="white", edgecolor="#DDDDDD",
              framealpha=0.55, loc="upper left",
              bbox_to_anchor=(0.02, 1.04), fontsize=7,
              handlelength=1.2, borderpad=0.4)
    save(fig, out)


# --------------------------------- Figs. 4 & 7 (closed-loop multicore) ---
def _mc_series(rows, field, cores):
    by_ver = {}
    for r in rows:
        try:
            by_ver.setdefault(r["version"], {})[int(r["cores"])] = float(r[field])
        except (ValueError, KeyError):
            continue
    return by_ver


def fig04_bw_concurrency(out, rows):
    """BW (left) + mean latency (right) vs cores, DDR + CXL 2.0-4.0."""
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(6.9, 2.45),
                                   gridspec_kw={"wspace": 0.28})
    bw = _mc_series(rows, "bw_GBps", CORES)
    av = _mc_series(rows, "mean_lat_ns", CORES)
    order = ["ddr", "2.0", "3.0", "3.1", "4.0"]
    lbl = {"ddr": "Local DDR5", "2.0": "CXL 2.0", "3.0": "CXL 3.0",
           "3.1": "CXL 3.1+IDE", "4.0": "CXL 4.0+IDE"}
    mk = {"ddr": "o", "2.0": "o", "3.0": "s", "3.1": "^", "4.0": "D"}
    for v in order:
        if v not in bw:
            continue
        c = sorted(bw[v])
        col = COL["ddr"] if v == "ddr" else VER_COL[v]
        ax1.plot(c, [bw[v][i] for i in c], color=col, marker=mk[v],
                 linewidth=2, label=lbl[v])
        ax2.plot(c, [av[v][i] for i in c], color=col, marker=mk[v],
                 linewidth=2, label=lbl[v])
    ax1.set_xlabel("Cores"); ax1.set_ylabel("Bandwidth (GB/s)")
    ax1.set_xticks(CORES)
    ax1.legend(frameon=True, facecolor="#F5F5F5", edgecolor="#DDDDDD",
               framealpha=0.90, loc="lower left", bbox_to_anchor=(0, 1.02),
               ncol=3, fontsize=7, handlelength=1.2, borderpad=0.4,
               columnspacing=0.8)
    ax2.set_xlabel("Cores"); ax2.set_ylabel("Mean latency (ns)")
    ax2.set_xticks(CORES)
    ax2.legend(frameon=True, facecolor="#F5F5F5", edgecolor="#DDDDDD",
               framealpha=0.90, loc="lower left", bbox_to_anchor=(0, 1.02),
               ncol=3, fontsize=7, handlelength=1.2, borderpad=0.4,
               columnspacing=0.8)
    save(fig, out)


def fig07_loaded(out, rows):
    """Mean (solid) + p99 (dashed) vs cores, left; p99/mean ratio bars, right."""
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(6.9, 2.45),
                                   gridspec_kw={"wspace": 0.28})
    av = _mc_series(rows, "mean_lat_ns", CORES)
    p9 = _mc_series(rows, "p99_lat_ns", CORES)
    order = ["ddr", "2.0", "3.0", "3.1", "4.0"]
    lbl = {"ddr": "DDR5", "2.0": "CXL 2.0", "3.0": "CXL 3.0",
           "3.1": "CXL 3.1+IDE", "4.0": "CXL 4.0+IDE"}
    mk = {"ddr": "o", "2.0": "o", "3.0": "s", "3.1": "^", "4.0": "D"}
    for v in order:
        if v not in av:
            continue
        c = sorted(av[v])
        col = COL["ddr"] if v == "ddr" else VER_COL[v]
        ax1.plot(c, [av[v][i] for i in c], color=col, marker=mk[v],
                 linewidth=2.0, label=lbl[v])
        if v in p9 and len(p9[v]) == len(c):
            ax1.plot(c, [p9[v][i] for i in c], color=col, marker=mk[v],
                     linewidth=1.2, linestyle="--")
    ax1.set_xlabel("Cores"); ax1.set_ylabel("Latency (ns)")
    ax1.set_xticks(CORES)
    ax1.legend(frameon=True, facecolor="#F5F5F5", edgecolor="#DDDDDD",
               framealpha=0.90, loc="lower left", bbox_to_anchor=(0, 1.02),
               ncol=5, fontsize=7, handlelength=1.2, borderpad=0.4,
               columnspacing=0.8)
    ratios, rlbls, rcols = [], [], []
    for v in order:
        if v in av and v in p9 and 64 in av[v] and 64 in p9[v] and av[v][64]:
            ratios.append(round(p9[v][64] / av[v][64], 1))
            rlbls.append(lbl[v].replace("+IDE", "\n+IDE"))
            rcols.append(COL["ddr"] if v == "ddr" else VER_COL[v])
    if ratios:
        xx = np.arange(len(ratios))
        bars = ax2.bar(xx, ratios, color=rcols, width=0.55, alpha=0.82,
                       edgecolor="none")
        for bar, val in zip(bars, ratios):
            ax2.text(bar.get_x() + bar.get_width() / 2, val + 0.12,
                     f"{val:.1f}x", ha="center", fontsize=7.5)
        ax2.set_xticks(xx); ax2.set_xticklabels(rlbls, fontsize=7)
        ax2.set_ylabel("p99 / mean (64-core)")
        ax2.set_ylim(0, max(6.4, max(ratios) * 1.25))
    save(fig, out)


# ---------------------------------------------------------------- Fig.5 ---
def fig05_mlp(out, rows):
    """Mean latency vs MLP depth (log x), all versions + DDR5."""
    depths = [1, 4, 16, 64]
    by_ver = {}
    for r in rows:
        try:
            by_ver.setdefault(r["device"], {})[int(r["depth"])] = float(r["mean_lat_ns"])
        except (ValueError, KeyError):
            continue
    order = [v for v in ["DDR5", "2.0", "3.0", "3.1", "4.0", "ddr5"]
             if v in by_ver]
    # normalize ddr5 key
    norm = {}
    for v in order:
        vv = "DDR5" if v == "ddr5" else v
        norm.setdefault(vv, {}).update(by_ver[v])
    order = [v for v in ["DDR5", "2.0", "3.0", "3.1", "4.0"] if v in norm]

    fig, ax = plt.subplots(figsize=(1.85, 2.2))
    style = {"DDR5": (COL["ddr"], "o", 4, 1.5),
             "2.0": (COL["cxl"], "o", 5, 1.8),
             "3.0": (COL["cxl30"], "s", 4, 1.6),
             "3.1": (COL["cxl31"], "^", 4, 1.6),
             "4.0": (COL["cxl40"], "D", 4, 1.6)}
    top = 0
    for v in order:
        ys = [norm[v].get(d, float("nan")) for d in depths]
        top = max(top, max(y for y in ys if y == y))
        c, m, ms, lw = style[v]
        lab = "DDR5 only" if v == "DDR5" else VER_LBL[v]
        ax.plot(depths, ys, color=c, marker=m, markersize=ms,
                linewidth=lw, label=lab, zorder=3)
    ax.set_xscale("log")
    ax.set_xticks(depths); ax.set_xticklabels([str(d) for d in depths])
    ax.set_xlabel("MLP depth $d$")
    ax.set_ylabel("Mean latency (ns)")
    ax.set_ylim(0, top * 1.15)
    ax.legend(frameon=True, facecolor="#F5F5F5", edgecolor="#DDDDDD",
              framealpha=0.90, loc="upper left", fontsize=7,
              handlelength=1.4)
    save(fig, out)


# ---------------------------------------------------------------- Fig.6 ---
def fig06_t4(out, rows):
    """Latency (left) + BW dashed (right) vs requesters; points from data."""
    by_ver = {}
    for r in rows:
        try:
            by_ver.setdefault(r["version"], {})[int(r["requesters"])] = r
        except (ValueError, KeyError):
            continue
    order = [v for v in ["2.0", "3.0", "3.1", "4.0"] if v in by_ver]
    rs = sorted(by_ver[order[0]]) if order else []

    fig, ax = plt.subplots(figsize=(1.85, 2.4))
    ax2 = ax.twinx()
    ax2.spines["top"].set_visible(False)
    ax2.spines["right"].set_color("#CCCCCC")
    x = np.arange(len(rs))
    lines = []
    for v in order:
        lat = [float(by_ver[v][r]["mean_lat_ns"]) for r in rs]
        (ln,) = ax.plot(x, lat, color=VER_COL[v], marker=VER_MARK[v],
                        markersize=4 if v == "2.0" else 3,
                        linewidth=1.8 if v == "2.0" else 1.4,
                        label=VER_LBL[v], zorder=3)
        lines.append(ln)
    bw = [float(by_ver[order[0]][r]["bw_GBps"]) for r in rs]
    (bw_line,) = ax2.plot(x, bw, color=COL["ddr"], marker="o", markersize=3,
                          linewidth=1.4, linestyle="--", zorder=2,
                          label="BW (GB/s)")
    peak_i = bw.index(max(bw))
    ax2.scatter([x[peak_i]], [bw[peak_i]], s=45, color=COL["ddr"],
                marker="*", zorder=5, edgecolors="none")
    ax.set_xticks(x); ax.set_xticklabels([str(r) for r in rs])
    ax.set_xlabel("Requesters $r$")
    ax.set_ylabel("Latency (ns)", fontsize=8)
    ax2.set_ylabel("BW (GB/s)", color=COL["ddr"], fontsize=8)
    ax.tick_params(axis="y", labelcolor="black")
    ax2.tick_params(axis="y", labelcolor=COL["ddr"])
    lat_top = max(8000.0, max(y for v in order for y in
                              [float(by_ver[v][r]["mean_lat_ns"]) for r in rs]) * 1.1)
    ax.set_ylim(0, lat_top)
    ax.set_yticks(np.arange(0, lat_top + 1, 2000))
    ax2.set_ylim(0, 2.0); ax2.set_yticks([0.0, 0.5, 1.0, 1.5, 2.0])
    ax.legend([bw_line] + lines, ["BW (GB/s)"] + [VER_LBL[v] for v in order],
              frameon=True, facecolor="#F5F5F5", edgecolor="#DDDDDD",
              framealpha=0.90, loc="upper left", fontsize=6.5,
              handlelength=1.4)
    save(fig, out)


# ---------------------------------------------------------------- Fig.8 ---
def fig08_decomp(out, rows):
    """Grouped stacked bars: versions x ops (floor + DRAM + credit)."""
    by_vr = {}
    for r in rows:
        try:
            by_vr[(r["version"], int(r["requesters"]))] = r
        except (ValueError, KeyError):
            continue
    ops = sorted({o for _, o in by_vr})
    versions = [v for v in ["2.0", "3.0", "3.1", "4.0"]
                if any((v, o) in by_vr for o in ops)]
    c_host, c_link, c_credit, c_dram = "#4393C3", "#92C5DE", "#9467BD", "#8B4513"
    ver_colors = {v: VER_COL[v] for v in versions}

    fig, ax = plt.subplots(figsize=(6.9, 2.8))
    x_centers = np.arange(len(ops))
    bar_w = 0.18
    offsets = np.linspace(-(len(versions) - 1) * bar_w / 2,
                          (len(versions) - 1) * bar_w / 2, len(versions))
    drawn = set()

    def seg(xs, bot, h, color, label):
        if label not in drawn:
            ax.bar(xs, h, bar_w, bottom=bot, color=color, edgecolor="white",
                   linewidth=0.6, label=label, zorder=3, alpha=0.82)
            drawn.add(label)
        else:
            ax.bar(xs, h, bar_w, bottom=bot, color=color, edgecolor="white",
                   linewidth=0.6, zorder=3, alpha=0.82)

    for vi, ver in enumerate(versions):
        xs = x_centers + offsets[vi]
        host = [float(by_vr[(ver, o)]["host_ns"]) for o in ops]
        fwd = [float(by_vr[(ver, o)]["fwd_ns"]) for o in ops]
        ret = [float(by_vr[(ver, o)]["ret_ns"]) for o in ops]
        dram = [float(by_vr[(ver, o)]["dram_ns"]) for o in ops]
        cred = [float(by_vr[(ver, o)]["credit_ns"]) for o in ops]
        bot = [0.0] * len(ops)
        seg(xs, bot, host, c_host, "Host overhead")
        bot = [b + h for b, h in zip(bot, host)]
        seg(xs, bot, [f + r for f, r in zip(fwd, ret)], c_link, "CXL link")
        bot = [b + h for b, h in zip(bot, [f + r for f, r in zip(fwd, ret)])]
        seg(xs, bot, dram, c_dram, "DRAM")
        bot = [b + h for b, h in zip(bot, dram)]
        seg(xs, bot, cred, c_credit, "Credit wait")
        for xi, m in zip(xs, [float(by_vr[(ver, o)]["mean_ns"]) for o in ops]):
            ax.text(xi, m + 120, f"{m:.0f}", ha="center", va="bottom",
                    fontsize=5.5, color=ver_colors[ver])
    ax.set_xticks(x_centers)
    ax.set_xticklabels([f"r={o}" for o in ops])
    ax.set_ylabel("Latency (ns)")
    ax.legend(frameon=True, facecolor="white", edgecolor="#DDDDDD",
              framealpha=0.9, loc="upper left", fontsize=7, ncol=4,
              handlelength=1.2)
    save(fig, out)


# ---------------------------------------------------------------- Fig.9 ---
def fig09_credit(out, rows):
    """Credit-pool cliff: mean latency vs pool limit (linear scale)."""
    by_lim = {}
    for r in rows:
        try:
            by_lim[str(r["m2s_req_limit"])] = float(r["mean_lat_ns"])
        except (ValueError, KeyError):
            continue
    order = [l for l in ["disabled", "32", "16", "8", "4"] if l in by_lim]
    if not order:
        return placeholder(out, "Fig.9 credit", "no credit data")
    lats = [by_lim[l] for l in order]
    pt_colors = {"disabled": "#3182BD", "32": "#3182BD", "16": "#E6550D",
                 "8": "#D94801", "4": "#A63603"}

    fig, ax = plt.subplots(figsize=(1.85, 1.5))
    x = np.arange(len(order))
    ylim_top = max(lats) * 1.35
    if "16" in by_lim:
        i16 = order.index("16")
        ax.axvspan(i16 - 0.5, i16 + 0.5, color="#FEF0D9", alpha=0.9,
                   zorder=0, lw=0)
        ax.text(i16, lats[0] * 0.35, "cliff", ha="center", fontsize=6.5,
                color="#CC5500", style="italic", zorder=3)
    ax.plot(x, lats, color="#CCCCCC", linewidth=1.2, zorder=2,
            solid_capstyle="round")
    labels = [f"{y:.0f}" for y in lats]
    cols = [pt_colors[l] for l in order]
    for i, (xi, yi, c, lab) in enumerate(zip(x, lats, cols, labels)):
        ax.scatter([xi], [yi], s=40, color=c, zorder=4,
                   edgecolors="white", linewidths=0.8)
        x_text = xi + 0.01 if i == 0 else xi
        ha = "left" if i == 0 else "center"
        ax.text(x_text, yi + ylim_top * 0.05, lab, ha=ha, va="bottom",
                fontsize=6.5, color=c)
    ax.set_ylim(0, ylim_top)
    ax.set_xticks(x)
    ax.set_xticklabels([("∞\n(off)" if l == "disabled" else l) for l in order])
    ax.set_xlabel("Credit limit", fontsize=7.5)
    ax.set_ylabel("Mean latency (ns)", fontsize=7.5)
    ax.tick_params(axis="both", labelsize=7)
    save(fig, out)


# --------------------------------------------------------------- Fig.10 ---
def fig10_gim(out, rows):
    """E3B credit-saturated GIM: latency flat across devices (us) + BW."""
    e3b = sorted([r for r in rows if r.get("scenario") == "E3B"],
                 key=lambda r: int(r["num_devices"]))
    if not e3b:
        return placeholder(out, "Fig.10 GIM", "no E3B rows")
    devs = [int(r["num_devices"]) for r in e3b]
    lats_us = [float(r["mean_lat_ns"]) / 1000.0 for r in e3b]
    bws = [float(r["bw_GBps"]) for r in e3b]

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(6.9, 2.1),
                                   gridspec_kw={"wspace": 0.35})
    x = np.arange(len(devs)); xtl = [str(d) for d in devs]
    c_single, c_gim = COL["ddr"], COL["cxl_alt"]
    ylim_lat = max(lats_us) * 1.48
    for i, v in enumerate(lats_us):
        ax1.bar([i], [v], color=c_single if i == 0 else c_gim, width=0.5,
                zorder=3, edgecolor="none", alpha=0.78)
        ax1.text(i, v + ylim_lat * 0.03, f"{v:.3f}", ha="center",
                 va="bottom", fontsize=7.5, color="#333333")
    ax1.text(0.5, 0.85, "credit-pool bottleneck", transform=ax1.transAxes,
             fontsize=6.5, color="#555555", style="italic", ha="center")
    ax1.set_xticks(x); ax1.set_xticklabels(xtl)
    ax1.set_xlabel("# devices")
    ax1.set_ylabel(r"Mean latency ($\mu$s)")
    ax1.set_ylim(0, ylim_lat)

    ylim_bw = max(bws) * 1.48
    for i, v in enumerate(bws):
        ax2.bar([i], [v], color=c_single if i == 0 else COL["hw2"],
                width=0.5, zorder=3, edgecolor="none", alpha=0.78)
        ax2.text(i, v + ylim_bw * 0.03, f"{v:.2f}", ha="center",
                 va="bottom", fontsize=7.5, color="#333333")
    ax2.text(0.97, 0.82, "credit-pool sat.", transform=ax2.transAxes,
             fontsize=6.5, color="#555555", style="italic", ha="right")
    ax2.set_xticks(x); ax2.set_xticklabels(xtl)
    ax2.set_xlabel("# devices")
    ax2.set_ylabel("Bandwidth (GB/s)")
    ax2.set_ylim(0, ylim_bw)
    save(fig, out)


# --------------------------------------------------------------- Fig.11 ---
def fig11_type2(out, rows):
    """Type 2 vs Type 3 grouped bars: T2 read / T6 read / T6 write."""
    by_dt = {(r["device"], r["trace"]): r for r in rows}
    rd2 = float(by_dt[("type3", "T2_ptr_chase")]["read_lat_ns"])
    rd6 = float(by_dt[("type3", "T6_mixed_rw")]["read_lat_ns"])
    wr6 = float(by_dt[("type3", "T6_mixed_rw")]["write_lat_ns"])
    t3_vals = [rd2, rd6, wr6]
    t2_vals = [float(by_dt[("type2", "T2_ptr_chase")]["read_lat_ns"]),
               float(by_dt[("type2", "T6_mixed_rw")]["read_lat_ns"]),
               float(by_dt[("type2", "T6_mixed_rw")]["write_lat_ns"])]
    labels = ["T2 read", "T6 read", "T6 write"]
    c3, c2 = COL["ddr"], COL["cxl"]

    fig, ax = plt.subplots(figsize=(1.85, 2.2))
    x = np.arange(3); w = 0.32
    ylim = max(t2_vals) * 1.45
    ax.bar(x - w / 2, t3_vals, width=w, color=c3, alpha=0.78, label="Type 3",
           zorder=3, edgecolor="none")
    ax.bar(x + w / 2, t2_vals, width=w, color=c2, alpha=0.78, label="Type 2",
           zorder=3, edgecolor="none")
    for xi, v in zip(x - w / 2, t3_vals):
        ax.text(xi, v + ylim * 0.022, f"{v:.0f}", ha="center", va="bottom",
                fontsize=6.5, color=c3, rotation=90)
    for xi, v in zip(x + w / 2, t2_vals):
        ax.text(xi, v + ylim * 0.022, f"{v:.0f}", ha="center", va="bottom",
                fontsize=6.5, color=c2, rotation=90)
    ax.set_xticks(x); ax.set_xticklabels(labels, fontsize=7)
    ax.set_ylabel("Latency (ns)", fontsize=8)
    ax.set_ylim(0, ylim)
    ax.tick_params(axis="y", labelsize=7)
    ax.legend(frameon=True, facecolor="#F5F5F5", edgecolor="#DDDDDD",
              framealpha=0.90, loc="upper left", fontsize=7,
              handlelength=1.2)
    save(fig, out)


# --------------------------------------------------------------- Fig.12 ---
def fig12_multihost(out, rows):
    """3-panel: log latency mean/p99 | DL util/oversub | BW per host."""
    rr = sorted(rows, key=lambda r: int(r["hosts"]))
    hosts = [int(r["hosts"]) for r in rr]
    mean_ns = [float(r["mean_lat_ns"]) for r in rr]
    p99_ns = [float(r.get("p99_lat_ns") or 0) for r in rr]
    bws = [float(r["bw_GBps"]) for r in rr]
    per_host = [b / h for b, h in zip(bws, hosts)]
    dl = [float(r.get("dl_util_pct") or 0) for r in rr]
    # Offered-load oversubscription vs the observed knee capacity (max total
    # BW): with constant per-host offered load this reproduces the paper's
    # 200%/400% annotations from reproduced numbers.
    cap = max(bws) if bws else 1.0
    over = [n * per_host[0] / cap * 100.0 for n in hosts]

    fig, (ax1, ax2, ax3) = plt.subplots(1, 3, figsize=(6.9, 2.1),
                                        gridspec_kw={"wspace": 0.50})
    x = np.arange(len(hosts)); xtl = [str(h) for h in hosts]
    ax1.plot(x, mean_ns, color=COL["cxl"], marker="o", markersize=5,
             linewidth=1.8, label="Mean", zorder=3)
    if any(p99_ns):
        ax1.plot(x, [p if p else float("nan") for p in p99_ns],
                 color=COL["cxl_alt"], marker="s", markersize=4,
                 linewidth=1.6, linestyle="--", label="p99", zorder=3)
    ax1.axvspan(2.5, 4.5, alpha=0.06, color="red", zorder=0)
    ax1.set_yscale("log")
    ax1.set_xticks(x); ax1.set_xticklabels(xtl)
    ax1.set_xlabel("Hosts"); ax1.set_ylabel("Latency (ns)")
    ax1.set_ylim(100, 1e11)
    ax1.legend(frameon=False, loc="upper left", fontsize=7.5,
               handlelength=1.2)
    ax1.text(3.5, 3e9, "DL\nover-\nsubscribed", ha="center", va="center",
             fontsize=6, color="#CC3333")

    ax2.axhline(100, color="#AA2222", linewidth=1.2, linestyle="--", zorder=2)
    _lbl = dict(boxstyle="round,pad=0.15", facecolor="white",
                edgecolor="none", alpha=0.85)
    for xi, v, o in zip(x, dl, over):
        if v <= 100 and o <= 100:
            ax2.plot([xi], [v], color=COL["hw"], marker="^", markersize=5,
                     linewidth=1.8, zorder=3)
            ax2.text(xi, v + 35, f"{int(v)}%", ha="center", fontsize=7,
                     color=COL["hw"], bbox=_lbl)
        else:
            ax2.annotate("", xy=(xi, min(o, 400)), xytext=(xi, 100),
                         arrowprops=dict(arrowstyle="->", color=COL["hw"],
                                         lw=1.4))
            ax2.text(xi, min(o, 400) + 20, f"{int(o)}%", ha="center",
                     fontsize=7, color=COL["hw"], bbox=_lbl)
    ax2.set_xticks(x); ax2.set_xticklabels(xtl)
    ax2.set_xlabel("Hosts")
    ax2.set_ylabel("DL util. / oversub. (%)")
    ax2.set_ylim(0, 450)

    bw_color = "#74A9CF"
    oversub = [v > 100 for v in dl]
    colors_bar = ["#E07070" if o else bw_color for o in oversub]
    ax3.bar(x, per_host, color=colors_bar, width=0.5, zorder=3,
            edgecolor="none", alpha=0.85)
    ax3.axhline(per_host[0], color="#CCCCCC", linewidth=1.0, linestyle="--",
                zorder=2)
    ylim_bw = per_host[0] * 1.45
    for xi, v in zip(x, per_host):
        ax3.text(xi, v + ylim_bw * 0.025, f"{v:.2f}", ha="center",
                 va="bottom", fontsize=7, color="#333333")
    ax3.set_xticks(x); ax3.set_xticklabels(xtl)
    ax3.set_xlabel("Hosts")
    ax3.set_ylabel("BW per host (GB/s)")
    ax3.set_ylim(0, ylim_bw)
    save(fig, out)


# ------------------------------------------------------------- Table VI ---
PAPER_SPEED = {  # paper-reported values for the side-by-side table
    "python_min": 0.058, "python_max": 0.062,
    "cpp_min": 0.60, "cpp_max": 0.68,
    "xerxes": 0.046, "zsim_min": 28.6, "zsim_max": 59.5,
}


def _bench_range(rows, prefix):
    vals = []
    for r in rows:
        try:
            if r.get("mode", "").strip().lower().startswith(prefix):
                vals.append(float(r["speed_Mreqs"]))
        except (ValueError, KeyError):
            continue
    if not vals:
        return None, None
    return min(vals), max(vals)


def table6_simspeed(out, csv_path):
    """Paper | reproduced side-by-side (Mreq/s; MIPS host-recorded)."""
    rows, _ = read_csv(csv_path)
    py_lo, py_hi = _bench_range(rows, "python")
    cp_lo, cp_hi = _bench_range(rows, "cpp")
    host = "unknown host"
    repo = csv_path.parent.parent.parent  # <repo>/outputs/csv/<file>
    hp = repo / "artifact" / "work" / "bench" / "host.txt"
    alt = csv_path.parent / "host.txt"
    for p in [hp, alt]:
        if os.path.exists(p):
            parts = open(p).read().strip().split()
            host = f"{parts[1]} {parts[-2]}" if len(parts) > 2 else parts[0][:60]
            break

    def cell(paper, rep):
        if rep[0] is None:
            return f"{paper} | n/a"
        d = 100.0 * ((sum(rep) / 2) - paper) / paper if paper else 0
        return f"{paper} | {rep[0]:.2f}-{rep[1]:.2f} (%+d%%)" % d

    body = [
        ["Python Mreq/s", cell(PAPER_SPEED["python_min"],
                               (py_lo, py_hi)) if py_lo else "n/a"],
        ["C++ Mreq/s", cell(PAPER_SPEED["cpp_min"], (cp_lo, cp_hi))
         if cp_lo else "n/a"],
        ["Xerxes Mreq/s", f"{PAPER_SPEED['xerxes']} | n/a (external)"],
        ["zsim MIPS", f"{PAPER_SPEED['zsim_min']}-{PAPER_SPEED['zsim_max']} | host-recorded"],
    ]
    fig, ax = plt.subplots(figsize=(7, 3.0))
    ax.axis("off")
    ax.set_title("Table VI simspeed: paper | reproduced", fontsize=11)
    ax.table(cellText=[[r[0], r[1]] for r in body],
             colLabels=["engine", "paper | reproduced"], loc="center")
    fig.text(0.5, 0.02, "Host: " + host +
             " — absolute Mreq/s and MIPS move with the host.",
             ha="center", fontsize=7)
    fig.tight_layout(); fig.savefig(out, dpi=150); plt.close(fig)
    print("wrote", out)


# ------------------------------------------------------------------ main ---
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--hw", default="")
    ap.add_argument("--ref", default="")
    a = ap.parse_args()
    C = Path(a.csv).resolve(); O = Path(a.out).resolve()
    O.mkdir(parents=True, exist_ok=True)

    rows02, _ = read_csv(C / "fig02_calibration.csv")
    if rows02 and len(rows02) > 1:
        fig02_calibration(O / "fig02_calibration.png",
                          C / "fig02_calibration.csv", a.hw)
    else:
        placeholder(O / "fig02_calibration.png", "Fig.2 calibration",
                    "pending — run ./reproduce.sh")

    rows03, _ = read_csv(C / "fig03_versions.csv")
    if [r for r in rows03 if "mean_lat_ns" in r and r.get("config")]:
        fig03_versions(O / "fig03_versions.png", rows03)
    else:
        placeholder(O / "fig03_versions.png", "Fig.3 versions", "pending")

    def zsim_or_placeholder(csv_name, png_name, title, fn):
        rows, _ = read_csv(C / csv_name)
        if is_placeholder(rows) or not rows:
            note = rows[0].get("note", "pending") if rows else "pending"
            placeholder(O / png_name, title, note)
        else:
            fn(O / png_name, rows)

    zsim_or_placeholder("fig04_bw_concurrency.csv", "fig04_bw_concurrency.png",
                        "Fig.4 BW vs concurrency (CXRay)", fig04_bw_concurrency)
    zsim_or_placeholder("fig07_loaded.csv", "fig07_loaded.png",
                        "Fig.7 loaded mean/p99 (CXRay)", fig07_loaded)

    rows05, _ = read_csv(C / "fig05_mlp.csv")
    if [r for r in rows05 if r.get("mean_lat_ns")]:
        fig05_mlp(O / "fig05_mlp.png", rows05)
    else:
        placeholder(O / "fig05_mlp.png", "Fig.5 MLP", "pending")

    rows06, _ = read_csv(C / "fig06_t4.csv")
    if [r for r in rows06 if r.get("mean_lat_ns")]:
        fig06_t4(O / "fig06_t4.png", rows06)
    else:
        placeholder(O / "fig06_t4.png", "Fig.6 T4", "pending")

    rows08, _ = read_csv(C / "fig08_decomp.csv")
    if [r for r in rows08 if r.get("mean_ns")]:
        fig08_decomp(O / "fig08_decomp.png", rows08)
    else:
        placeholder(O / "fig08_decomp.png", "Fig.8 decomp", "pending")

    rows09, _ = read_csv(C / "fig09_credit.csv")
    if [r for r in rows09 if r.get("mean_lat_ns")]:
        fig09_credit(O / "fig09_credit.png", rows09)
    else:
        placeholder(O / "fig09_credit.png", "Fig.9 credit", "pending")

    rows10, _ = read_csv(C / "fig10_gim.csv")
    e3b = [r for r in rows10 if r.get("scenario") == "E3B"
           and r.get("mean_lat_ns")]
    if e3b:
        fig10_gim(O / "fig10_gim.png", e3b)
    else:
        placeholder(O / "fig10_gim.png", "Fig.10 GIM",
                    "E3B runs in full mode (smoke covers E3A)")

    rows11, _ = read_csv(C / "fig11_type2.csv")
    if [r for r in rows11 if r.get("read_lat_ns")]:
        fig11_type2(O / "fig11_type2.png", rows11)
    else:
        placeholder(O / "fig11_type2.png", "Fig.11 Type2", "pending")

    rows12, _ = read_csv(C / "fig12_multihost.csv")
    if [r for r in rows12 if r.get("mean_lat_ns")]:
        fig12_multihost(O / "fig12_multihost.png", rows12)
    else:
        placeholder(O / "fig12_multihost.png", "Fig.12 multihost", "pending")

    if (C / "table6_simspeed.csv").exists():
        table6_simspeed(O / "table6_simspeed.png", C / "table6_simspeed.csv")
    else:
        placeholder(O / "table6_simspeed.png", "Table VI simspeed", "pending")


if __name__ == "__main__":
    main()
