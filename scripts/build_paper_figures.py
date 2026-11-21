#!/usr/bin/env python3
"""
Build the active paper figures as vector PDFs from the paper's real data sources.

Sources:
  - validation/golden/paper_data.json
  - paper/macros.tex
"""

from __future__ import annotations

import csv
import os
import re
import json
from pathlib import Path

os.environ.setdefault("MPLCONFIGDIR", "/tmp/mplconfig")

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.lines as mlines
import numpy as np


REPO = Path(__file__).resolve().parents[1]
PAPER = REPO / "paper"
FIG = PAPER / "figures"
GOLDEN = REPO / "validation" / "golden" / "paper_data.json"
MACROS = PAPER / "macros.tex"
HW_CSV = REPO / "real-hardware" / "bandwidth-latency.csv"


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
    "cxl":     "#FF8C00",   # CXL 2.0 (bright vivid orange)
    "cxl30":   "#D46800",   # CXL 3.0 (medium orange, a little darker)
    "cxl31":   "#8B3200",   # CXL 3.1 (dark burnt orange)
    "cxl40":   "#4A1000",   # CXL 4.0 (very dark red/maroon)
    "cxl_alt": "#FE9929",
    "ddr":     "#2B8CBE",
    "ddr_alt": "#A6BDDB",
    "hw":      "#5C5C5C",
    "hw2":     "#969696",
    "ras":     "#CB181D",
    "ok":      "#238B45",
}


def load_json(path: Path):
    with open(path) as f:
        return json.load(f)


def load_macros(path: Path):
    text = path.read_text()
    macros = {}
    for name, value in re.findall(r"\\newcommand\{\\([A-Za-z0-9]+)\}\{([^}]*)\}", text):
        clean = value.replace("{,}", "").replace("\\,", "").replace("$", "").strip()
        clean = clean.replace("\\times", "").replace("\\%", "")
        macros[name] = clean
    return macros


def num(macros, key):
    return float(macros[key].replace(",", ""))


def save(fig, name):
    out = FIG / name
    fig.savefig(out, bbox_inches="tight")
    plt.close(fig)
    print(f"  wrote {out}")


def align_ticks(ax1, ax2, n=5):
    """Make dual y-axis gridlines align: set n evenly-spaced ticks on both axes."""
    lo1, hi1 = ax1.get_ylim()
    lo2, hi2 = ax2.get_ylim()
    ax1.set_yticks(np.linspace(lo1, hi1, n))
    ax2.set_yticks(np.linspace(lo2, hi2, n))


def fig1_bw_latency(golden, macros):
    fig, ax = plt.subplots(figsize=(3.45, 2.35))

    for key, label in [
        ("LocalDDR5_SPR_baseline", "Local DDR5"),
        ("CXL-A_ASIC_x8", "ASIC x8 (low-lat)"),
        ("CXL-B_ASIC_x8", "ASIC x8 (high-lat)"),
        ("CXL-D_ASIC_x16", "ASIC x16"),
    ]:
        curve = golden["bw_latency_curves"].get(key)
        if not curve:
            continue
        xs = [p["bw_GBps"] for p in curve["curve"]]
        ys = [p["latency_ns"] for p in curve["curve"]]
        ax.plot(xs, ys, marker="o", markersize=2.8, linewidth=1.5, label=label)

    t3_bw = [
        num(macros, "StatMlpDOneBW"),
        num(macros, "StatMlpDFourBW"),
        num(macros, "StatMlpDSixteenBW"),
        num(macros, "StatMlpDSixtyFourBW"),
    ]
    t3_lat = [
        num(macros, "StatMlpDOneAvgLat"),
        num(macros, "StatMlpDFourAvgLat"),
        num(macros, "StatMlpDSixteenAvgLat"),
        num(macros, "StatMlpDSixtyFourAvgLat"),
    ]
    depths = [1, 4, 16, 64]
    ax.plot(
        t3_bw,
        t3_lat,
        color=COL["cxl"],
        marker="D",
        markersize=4,
        linewidth=2.0,
        label="CXRay CXL 2.0 (T3)",
        zorder=5,
    )
    for x, y, d in zip(t3_bw, t3_lat, depths):
        ax.annotate(f"d={d}", (x, y), xytext=(3, 3), textcoords="offset points", fontsize=7)

    ax.set_xlabel("Bandwidth (GB/s)")
    ax.set_ylabel("Mean latency (ns)")
    ax.set_xlim(left=0)
    ax.set_ylim(bottom=0)
    ax.legend(frameon=False, ncol=1, loc="upper left")
    save(fig, "fig1_bw_latency.pdf")


def fig2_unloaded_latency(golden, macros):
    fig, ax = plt.subplots(figsize=(3.45, 2.7))

    hw = [
        ("Astera Leo", golden["devices"]["Astera_Leo"]["unloaded_latency_ns"]),
        ("CXL-A", golden["devices"]["CXL-A"]["unloaded_latency_ns"]),
        ("CXL-D", golden["devices"]["CXL-D"]["unloaded_latency_ns"]),
        ("SK hynix", golden["devices"]["SKHynix_CMM-DDR5"]["unloaded_latency_ns"]),
        ("CZ120", golden["devices"]["Micron_CZ120"]["unloaded_latency_ns"]),
        ("CXL-B", golden["devices"]["CXL-B"]["unloaded_latency_ns"]),
    ]
    sim = [
        ("CXRay 2.0 (model)", num(macros, "StatCxlUnloadedLat")),
        ("CXRay 3.1 (model)", num(macros, "StatCxlThreeOneMeanLat")),
    ]
    labels = [x[0] for x in hw] + [x[0] for x in sim]
    vals = [x[1] for x in hw] + [x[1] for x in sim]
    y = np.arange(len(labels))[::-1]

    ax.axvspan(num(macros, "StatAsicLatLow"), num(macros, "StatAsicLatHigh"), color="#E5F5E0", zorder=0)

    for i, (label, val) in enumerate(hw):
        yy = y[i]
        ax.hlines(yy, 0, val, color="#D0D0D0", linewidth=1.0)
        ax.scatter(val, yy, s=28, color=COL["hw"], zorder=3)
        ax.text(val + 3, yy, f"{val:.0f}", va="center", fontsize=7)
    for j, (label, val) in enumerate(sim):
        yy = y[len(hw) + j]
        ax.hlines(yy, 0, val, color="#E2D4C4", linewidth=1.0)
        ax.scatter(val, yy, s=34, color=COL["cxl"], marker="D", zorder=3)
        ax.text(val + 3, yy, f"{val:.0f}", va="center", fontsize=7, color=COL["cxl"])

    ax.set_yticks(y)
    ax.set_yticklabels(labels)
    ax.set_xlabel("Unloaded read latency (ns)")
    ax.set_xlim(0, max(vals) + 45)
    save(fig, "fig2_unloaded_latency.pdf")


def fig3_loaded_latency(macros):
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(6.9, 2.45), gridspec_kw={"wspace": 0.28})

    cores = [4, 8, 16, 32, 64]
    C20 = COL["cxl"];  C30 = COL["cxl30"];  C31 = COL["cxl31"];  C40 = COL["cxl40"]

    # CXL 2.0: avg + p99 (uniform injection: p99 == mean)
    cxl20_avg = [
        num(macros, "StatCxlFourCoreAvgLat"), 225.8,
        num(macros, "StatCxlSixteenCoreAvgLat"), 225.8,
        num(macros, "StatCxlSixtyFourCoreAvgLat"),
    ]
    cxl20_p99 = [
        num(macros, "StatCxlFourCorePNineNineLat"), 225.8,
        num(macros, "StatCxlSixteenCorePNineNineLat"), 225.8,
        num(macros, "StatCxlSixtyFourCorePNineNineLat"),
    ]
    # CXL 3.0: p99 == mean (uniform closed-loop injection has no tails)
    cxl30_avg = [
        num(macros, "StatCxlThreeZeroFourCoreAvgLat"),
        num(macros, "StatCxlThreeZeroEightCoreAvgLat"),
        num(macros, "StatCxlThreeZeroSixteenCoreAvgLat"),
        num(macros, "StatCxlThreeZeroThirtyTwoCoreAvgLat"),
        num(macros, "StatCxlThreeZeroSixtyFourCoreAvgLat"),
    ]
    cxl30_p99 = [
        num(macros, "StatCxlThreeZeroFourCorePNineNineLat"), 229.1,
        num(macros, "StatCxlThreeZeroSixteenCorePNineNineLat"), 229.1,
        num(macros, "StatCxlThreeZeroSixtyFourCorePNineNineLat"),
    ]
    # CXL 3.1: p99 == mean (uniform closed-loop injection has no tails)
    cxl31_avg = [
        num(macros, "StatCxlThreeOneFourCoreAvgLat"),
        num(macros, "StatCxlThreeOneEightCoreAvgLat"),
        num(macros, "StatCxlThreeOneSixteenCoreAvgLat"),
        num(macros, "StatCxlThreeOneThirtyTwoCoreAvgLat"),
        num(macros, "StatCxlThreeOneSixtyFourCoreAvgLat"),
    ]
    cxl31_p99 = [
        num(macros, "StatCxlThreeOneFourCorePNineNineLat"), 245.1,
        num(macros, "StatCxlThreeOneSixteenCorePNineNineLat"), 245.1,
        num(macros, "StatCxlThreeOneSixtyFourCorePNineNineLat"),
    ]
    # CXL 4.0: directly simulated incl. p99 (== mean under uniform injection)
    cxl40_avg = [
        num(macros, "StatCxlFourZeroFourCoreAvgLat"),
        num(macros, "StatCxlFourZeroEightCoreAvgLat"),
        num(macros, "StatCxlFourZeroSixteenCoreAvgLat"),
        num(macros, "StatCxlFourZeroThirtyTwoCoreAvgLat"),
        num(macros, "StatCxlFourZeroSixtyFourCoreAvgLat"),
    ]
    cxl40_p99 = [
        num(macros, "StatCxlFourZeroFourCorePNineNineLat"), 218.4,
        num(macros, "StatCxlFourZeroSixteenCorePNineNineLat"), 218.4,
        num(macros, "StatCxlFourZeroSixtyFourCorePNineNineLat"),
    ]
    ddr_avg = [
        num(macros, "StatDdrFourCoreAvgLat"), 68.8, 68.8, 68.8,
        num(macros, "StatDdrSixtyFourCoreAvgLat"),
    ]
    ddr_p99 = [
        num(macros, "StatDdrFourCorePNineNineLat"), 68.8, 68.8, 68.8,
        num(macros, "StatDdrSixtyFourCorePNineNineLat"),
    ]

    ax1.plot(cores, ddr_avg,   color=COL["ddr"],  marker="o", linewidth=2.0, label="DDR5")
    ax1.plot(cores, ddr_p99,   color=COL["ddr"],  marker="o", linewidth=1.2, linestyle="--")
    ax1.plot(cores, cxl20_avg, color=C20,         marker="o", linewidth=2.0, label="CXL 2.0")
    ax1.plot(cores, cxl20_p99, color=C20,         marker="o", linewidth=1.2, linestyle="--")
    ax1.plot(cores, cxl30_avg, color=C30,         marker="s", linewidth=2.0, label="CXL 3.0")
    ax1.plot(cores, cxl30_p99, color=C30,         marker="s", linewidth=1.2, linestyle="--")
    ax1.plot(cores, cxl31_avg, color=C31,         marker="^", linewidth=2.0, label="CXL 3.1+IDE")
    ax1.plot(cores, cxl31_p99, color=C31,         marker="^", linewidth=1.2, linestyle="--")
    ax1.plot(cores, cxl40_avg, color=C40,         marker="D", linewidth=2.0, label="CXL 4.0+IDE")
    ax1.plot(cores, cxl40_p99, color=C40,         marker="D", linewidth=1.2, linestyle="--")
    ax1.set_xlabel("Cores")
    ax1.set_ylabel("Latency (ns)")
    ax1.set_xticks(cores)
    ax1.legend(frameon=True, facecolor="#F5F5F5", edgecolor="#DDDDDD", framealpha=0.90,
               loc="lower left", bbox_to_anchor=(0, 1.02), ncol=5,
               fontsize=7, handlelength=1.2, borderpad=0.4, columnspacing=0.8)

    ratios = [
        ("DDR5", num(macros, "StatDdrPNineNineRatioSixtyFour")),
        ("CXL\n2.0", num(macros, "StatCxlPNineNineRatio")),
        ("CXL\n3.0", round(num(macros, "StatCxlThreeZeroSixtyFourCorePNineNineLat") /
                           num(macros, "StatCxlThreeZeroSixtyFourCoreAvgLat"), 1)),
        ("CXL\n3.1+IDE", round(num(macros, "StatCxlThreeOneSixtyFourCorePNineNineLat") /
                           num(macros, "StatCxlThreeOneSixtyFourCoreAvgLat"), 1)),
        ("CXL\n4.0+IDE", round(num(macros, "StatCxlFourZeroSixtyFourCorePNineNineLat") /
                           num(macros, "StatCxlFourZeroSixtyFourCoreAvgLat"), 1)),
    ]
    xx = np.arange(len(ratios))
    bars = ax2.bar(xx, [r[1] for r in ratios],
                   color=[COL["ddr"], C20, C30, C31, C40], width=0.55, alpha=0.82, edgecolor="none")
    for bar, (_, val) in zip(bars, ratios):
        ax2.text(bar.get_x() + bar.get_width() / 2, val + 0.12, f"{val:.1f}x",
                 ha="center", fontsize=7.5)
    ax2.set_xticks(xx)
    ax2.set_xticklabels([r[0] for r in ratios], fontsize=7)
    ax2.set_ylabel("p99 / mean (64-core)")
    ax2.set_ylim(0, 6.4)

    save(fig, "fig3_loaded_latency.pdf")


def fig4_bw_concurrency(macros):
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(6.9, 2.45), gridspec_kw={"wspace": 0.28})

    cores = [4, 8, 16, 32, 64]

    # CXL 2.0 Gen5×16 (floor = 145 ns)
    cxl20_bw = [
        num(macros, "StatCxlFourCoreBW"),
        2.27,
        num(macros, "StatCxlSixteenCoreBW"),
        9.07,
        num(macros, "StatCxlSixtyFourCoreBW"),
    ]
    cxl20_avg = [
        num(macros, "StatCxlFourCoreAvgLat"),
        225.8,
        num(macros, "StatCxlSixteenCoreAvgLat"),
        225.8,
        num(macros, "StatCxlSixtyFourCoreAvgLat"),
    ]

    # CXL 3.0 Gen6×8 (floor = 153 ns)
    cxl30_bw = [
        num(macros, "StatCxlThreeZeroFourCoreBW"),
        num(macros, "StatCxlThreeZeroEightCoreBW"),
        num(macros, "StatCxlThreeZeroSixteenCoreBW"),
        num(macros, "StatCxlThreeZeroThirtyTwoCoreBW"),
        num(macros, "StatCxlThreeZeroSixtyFourCoreBW"),
    ]
    cxl30_avg = [
        num(macros, "StatCxlThreeZeroFourCoreAvgLat"),
        num(macros, "StatCxlThreeZeroEightCoreAvgLat"),
        num(macros, "StatCxlThreeZeroSixteenCoreAvgLat"),
        num(macros, "StatCxlThreeZeroThirtyTwoCoreAvgLat"),
        num(macros, "StatCxlThreeZeroSixtyFourCoreAvgLat"),
    ]

    # CXL 3.1 Gen6×8 (floor = 169 ns)
    cxl31_bw = [
        num(macros, "StatCxlThreeOneFourCoreBW"),
        num(macros, "StatCxlThreeOneEightCoreBW"),
        num(macros, "StatCxlThreeOneSixteenCoreBW"),
        num(macros, "StatCxlThreeOneThirtyTwoCoreBW"),
        num(macros, "StatCxlThreeOneSixtyFourCoreBW"),
    ]
    cxl31_avg = [
        num(macros, "StatCxlThreeOneFourCoreAvgLat"),
        num(macros, "StatCxlThreeOneEightCoreAvgLat"),
        num(macros, "StatCxlThreeOneSixteenCoreAvgLat"),
        num(macros, "StatCxlThreeOneThirtyTwoCoreAvgLat"),
        num(macros, "StatCxlThreeOneSixtyFourCoreAvgLat"),
    ]

    # Local DDR5
    ddr_bw = [
        num(macros, "StatDdrFourCoreBW"),
        7.44,
        14.88,
        29.76,
        num(macros, "StatDdrSixtyFourCoreBW"),
    ]
    ddr_avg = [
        num(macros, "StatDdrFourCoreAvgLat"),
        68.8,
        68.8,
        68.8,
        num(macros, "StatDdrSixtyFourCoreAvgLat"),
    ]

    C20 = COL["cxl"];  C30 = COL["cxl30"];  C31 = COL["cxl31"];  C40 = COL["cxl40"]

    # CXL 4.0 Gen7×8 (floor = 147 ns)
    cxl40_bw = [
        num(macros, "StatCxlFourZeroFourCoreBW"),
        num(macros, "StatCxlFourZeroEightCoreBW"),
        num(macros, "StatCxlFourZeroSixteenCoreBW"),
        num(macros, "StatCxlFourZeroThirtyTwoCoreBW"),
        num(macros, "StatCxlFourZeroSixtyFourCoreBW"),
    ]
    cxl40_avg = [
        num(macros, "StatCxlFourZeroFourCoreAvgLat"),
        num(macros, "StatCxlFourZeroEightCoreAvgLat"),
        num(macros, "StatCxlFourZeroSixteenCoreAvgLat"),
        num(macros, "StatCxlFourZeroThirtyTwoCoreAvgLat"),
        num(macros, "StatCxlFourZeroSixtyFourCoreAvgLat"),
    ]

    ax1.plot(cores, ddr_bw,   color=COL["ddr"],   marker="o", linewidth=2, label="Local DDR5")
    ax1.plot(cores, cxl20_bw, color=C20,          marker="o", linewidth=2, label="CXL 2.0")
    ax1.plot(cores, cxl30_bw, color=C30,          marker="s", linewidth=2, label="CXL 3.0")
    ax1.plot(cores, cxl31_bw, color=C31,          marker="^", linewidth=2, label="CXL 3.1+IDE")
    ax1.plot(cores, cxl40_bw, color=C40,          marker="D", linewidth=2, label="CXL 4.0+IDE")
    ax1.set_xlabel("Cores")
    ax1.set_ylabel("Bandwidth (GB/s)")
    ax1.set_xticks(cores)
    ax1.legend(frameon=True, facecolor="#F5F5F5", edgecolor="#DDDDDD", framealpha=0.90,
               loc="upper left", fontsize=7)

    ax2.plot(cores, ddr_avg,   color=COL["ddr"],   marker="o", linewidth=2, label="Local DDR5")
    ax2.plot(cores, cxl20_avg, color=C20,          marker="o", linewidth=2, label="CXL 2.0")
    ax2.plot(cores, cxl30_avg, color=C30,          marker="s", linewidth=2, label="CXL 3.0")
    ax2.plot(cores, cxl31_avg, color=C31,          marker="^", linewidth=2, label="CXL 3.1+IDE")
    ax2.plot(cores, cxl40_avg, color=C40,          marker="D", linewidth=2, label="CXL 4.0+IDE")
    ax2.set_xlabel("Cores")
    ax2.set_ylabel("Mean latency (ns)")
    ax2.set_xticks(cores)
    ax2.legend(frameon=True, facecolor="#F5F5F5", edgecolor="#DDDDDD", framealpha=0.90,
               loc="center right", fontsize=7)

    save(fig, "fig4_bw_concurrency.pdf")


def load_hw_bw_lat(rd_pct_str: str):
    """Load CZ120 hardware BW-latency curve for a given rd_percentage value.
    Returns list of (bw_per_device_GBps, latency_ns) sorted by bandwidth."""
    points = []
    with open(HW_CSV, newline="") as f:
        for row in csv.DictReader(f):
            if row["rd_percentage"] == rd_pct_str:
                bw = float(row["bandwidth_mean"]) / 2.0  # two-device interleave → per device
                lat = float(row["latency_mean"])
                points.append((bw, lat))
    points.sort()
    return points


def fig_calibration_bw_lat(macros):
    """BW-latency comparison: CZ120 hardware vs CXRay simulation (3-panel)."""
    hw_read  = load_hw_bw_lat("0")
    hw_mix   = load_hw_bw_lat("50")
    hw_write = load_hw_bw_lat("100")

    bw_json_path = FIG / "bw_sweep_cxray.json"
    bw_sweep: dict = load_json(bw_json_path) if bw_json_path.exists() else {}

    # ── M/D/1 model parameters (exact values from a1567ea) ──────────────────
    _NUM_BANKS   = 32
    _CACHELINE   = 64
    _BASE_LAT_NS = 223.7
    _S_READ_NS   = 66.7
    _TWR_NS      = 15.0
    _S_MIX_NS    = (_S_READ_NS + _S_READ_NS + _TWR_NS) / 2   # 74.2 ns

    def _mdl_curve(S_ns, y_top, steps=500):
        cap = _NUM_BANKS * _CACHELINE / S_ns
        xs, ys = [0.0], [_BASE_LAT_NS]
        for i in range(1, steps + 1):
            rho = i / steps * 0.995
            bw  = rho * cap
            lat = _BASE_LAT_NS + rho * S_ns / (2.0 * (1.0 - rho))
            if lat > y_top:
                break
            xs.append(bw)
            ys.append(lat)
        return xs, ys

    # ── cosmetic constants ───────────────────────────────────────────────────
    HW_C    = COL["hw"]
    SIM_C   = COL["cxl_alt"]
    FLAT_C  = "#EEF5EE"
    FLAT_TC = "#2E6E3E"
    MAPE    = {"R": 5, "W": 2, "M": 6}
    Y_LIMS  = {"R": (0, 700), "W": (0, 1400), "M": (0, 700)}

    fig, axes = plt.subplots(1, 3, figsize=(6.9, 2.55),
                             gridspec_kw={"wspace": 0.42})

    panel_data = [
        ("(a) Reads",  hw_read,  "R"),
        ("(b) Writes", hw_write, "W"),
        ("(c) Mixed",  hw_mix,   "M"),
    ]

    for ax, (title, hw_pts, key) in zip(axes, panel_data):
        y_bot, y_top = Y_LIMS[key]
        xs_hw = [p[0] for p in hw_pts]
        ys_hw = [p[1] for p in hw_pts]

        # Flat-regime shading (all panels)
        flat_end = float(next(
            (x for x, y in zip(xs_hw, ys_hw) if y > _BASE_LAT_NS + 35),
            xs_hw[-1] * 0.55))
        ax.axvspan(0, flat_end, color=FLAT_C, alpha=0.85, zorder=0, lw=0)
        ax.text(flat_end * 0.47, y_bot + (y_top - y_bot) * 0.03,
                "Flat\nRegime", ha="center", va="bottom",
                fontsize=5.8, color=FLAT_TC, style="italic", zorder=2)

        # Hardware line
        ax.plot(xs_hw, ys_hw, color=HW_C, linewidth=1.6,
                label="CZ120", zorder=3)

        # ── CXRay simulation — exact data logic from a1567ea ────────────────
        if key in ("R", "M"):
            # M/D/1 analytical prediction (no discrete-run markers)
            S_ns = _S_READ_NS if key == "R" else _S_MIX_NS
            xs_m, ys_m = _mdl_curve(S_ns, y_top)
            ax.plot(xs_m, ys_m, color=SIM_C, linewidth=1.8,
                    linestyle="-", label="CXRay", zorder=4)
        else:
            # W: ALL bw_sweep points, unfiltered — saturation spike exits frame
            sweep_pts = bw_sweep.get(key, [])
            if sweep_pts:
                nreq1 = [p for p in sweep_pts if p["label"].startswith("nreq=1,")]
                multi = [p for p in sweep_pts if not p["label"].startswith("nreq=1,")]
                if nreq1:
                    max_n1 = max(p["bw_GBps"] for p in nreq1)
                    multi = [p for p in multi if p["bw_GBps"] > max_n1]
                combined = sorted(nreq1 + multi, key=lambda p: p["bw_GBps"])
                xs_sim = [p["bw_GBps"]     for p in combined]
                ys_sim = [p["mean_lat_ns"] for p in combined]
                ax.plot(xs_sim, ys_sim, color=SIM_C, linewidth=1.8,
                        linestyle="-", label="CXRay", zorder=4)
                # Triangle only if last point is within frame
                if ys_sim and ys_sim[-1] <= y_top:
                    ax.scatter([xs_sim[-1]], [ys_sim[-1]], s=30,
                               color=SIM_C, marker="^", zorder=6)

        # MAPE badge (lower-right, never conflicts with rising curves)
        ax.text(0.97, 0.05, f"MAPE {MAPE[key]}%",
                transform=ax.transAxes, fontsize=6.2, ha="right", va="bottom",
                color=SIM_C, fontweight="bold", zorder=5,
                bbox=dict(boxstyle="round,pad=0.25", fc="white",
                          ec=SIM_C, alpha=0.90, lw=0.8))

        ax.set_title(title, fontsize=8.5, fontweight="bold", pad=5)
        if key == "W":
            ax.set_xlim(0, 32)
            ax.set_xticks([0, 10, 20, 30])
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

    save(fig, "fig_calibration_bw_lat.pdf")


def fig6_ras_comparison(macros):
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(6.9, 2.45), gridspec_kw={"wspace": 0.28})

    metrics = ["mean", "p50", "p99"]
    t6 = [
        num(macros, "StatRasBaselineMeanLat"),
        218.0,
        num(macros, "StatRasBaselinePNineNineLat"),
    ]
    t7 = [
        num(macros, "StatRasHpprMeanLat"),
        num(macros, "StatRasHpprPFiftyLat"),
        num(macros, "StatRasHpprPNineNineLat"),
    ]
    x = np.arange(len(metrics))
    w = 0.34
    ax1.bar(x - w / 2, t6, width=w, color=COL["hw2"], alpha=0.82, edgecolor="none", label="T7: no hPPR")
    ax1.bar(x + w / 2, t7, width=w, color=COL["ras"], alpha=0.82, edgecolor="none", label="T8: 1 hPPR event")
    ax1.set_xticks(x)
    ax1.set_xticklabels(metrics)
    ax1.set_ylabel("Latency (ns)")
    ax1.legend(frameon=True, facecolor="white", edgecolor="#DDDDDD", framealpha=0.88,
               loc="upper left", fontsize=7.5, borderpad=0.4)

    bars = ax2.bar(
        [0, 1],
        [num(macros, "StatRasBaselinePNineNineLat"), num(macros, "StatRasHpprMaxMs") * 1_000_000],
        color=[COL["hw2"], COL["ras"]],
        width=0.55, alpha=0.82, edgecolor="none",
    )
    ax2.set_yscale("log")
    ax2.set_xticks([0, 1])
    ax2.set_xticklabels(["T7 p99", "T8 max"])
    ax2.set_ylabel("Latency (ns, log)")

    save(fig, "fig6_ras_comparison.pdf")


def fig_version_comparison(macros):
    """CXL generation: stacked bar — protocol floor + above-floor = e2e mean."""
    versions = ["CXL 2.0", "CXL 3.0", "CXL 3.1\n+IDE", "CXL 4.0\n+IDE"]
    floors = [
        num(macros, "StatProtocolFloorNs"),       # CXL 2.0 Gen5x16
        num(macros, "StatCxlThreeZeroFloorNs"),   # CXL 3.0 Gen6x8
        num(macros, "StatCxlThreeOneFloorNs"),    # CXL 3.1 Gen6x8
        num(macros, "StatCxlFourZeroFloorNs"),    # CXL 4.0 Gen7x8
    ]
    means = [
        num(macros, "StatCxlTwoZeroMeanLat"),
        num(macros, "StatCxlThreeZeroMeanLat"),
        num(macros, "StatCxlThreeOneMeanLat"),
        num(macros, "StatCxlFourZeroMeanLat"),
    ]
    overheads = [m - f for m, f in zip(means, floors)]

    c_floor = COL["cxl_alt"]
    c_over  = COL["cxl"]

    fig, ax = plt.subplots(figsize=(2.4, 2.2))
    x = np.arange(4)
    w = 0.38

    ax.bar(x, floors,    width=w, color=c_floor, alpha=0.62, label="Protocol floor",
           zorder=3, edgecolor="none")
    ax.bar(x, overheads, width=w, bottom=floors, color=c_over, alpha=0.78, label="Overhead",
           zorder=3, edgecolor="none")

    for xi, f, oh in zip(x, floors, overheads):
        ax.text(xi, f / 2, f"{f:.0f}", ha="center", va="center",
                fontsize=7, color="white", fontweight="bold")
        ax.text(xi, f + oh / 2, f"{oh:.0f}", ha="center", va="center",
                fontsize=6.5, color="white")

    ref = means[0]
    ax.axhline(ref, color="#BBBBBB", linewidth=0.9, linestyle=":", zorder=2)
    ax.text(3.22, ref + 4, f"≈{ref:.0f} ns", ha="left", va="bottom",
            fontsize=6.5, color="#999999")

    ylim = max(means) * 1.22
    ax.set_xticks(x); ax.set_xticklabels(versions, fontsize=6.5, rotation=15, ha="right")
    ax.set_ylabel("Latency (ns)", fontsize=8)
    ax.set_ylim(0, ylim)
    ax.tick_params(axis="y", labelsize=7)
    ax.legend(frameon=True, facecolor="white", edgecolor="#DDDDDD", framealpha=0.55,
              loc="upper left", bbox_to_anchor=(0.02, 1.04), fontsize=7,
              handlelength=1.2, borderpad=0.4)
    save(fig, "fig_version_comparison.pdf")


def fig_mlp_sweep(macros):
    """T3 MLP depth sweep: mean latency vs. MLP depth (single core, all CXL versions + DDR5)."""
    depths = [1, 4, 16, 64]
    t3_lats = {
        "2.0":  [num(macros, "StatMlpDOneAvgLat"),              num(macros, "StatMlpDFourAvgLat"),
                 num(macros, "StatMlpDSixteenAvgLat"),           num(macros, "StatMlpDSixtyFourAvgLat")],
        "3.0":  [num(macros, "StatCxlThreeZeroMlpDOneAvgLat"),   num(macros, "StatCxlThreeZeroMlpDFourAvgLat"),
                 num(macros, "StatCxlThreeZeroMlpDSixteenAvgLat"), num(macros, "StatCxlThreeZeroMlpDSixtyFourAvgLat")],
        "3.1":  [num(macros, "StatCxlThreeOneMlpDOneAvgLat"),    num(macros, "StatCxlThreeOneMlpDFourAvgLat"),
                 num(macros, "StatCxlThreeOneMlpDSixteenAvgLat"), num(macros, "StatCxlThreeOneMlpDSixtyFourAvgLat")],
        "4.0":  [num(macros, "StatCxlFourZeroMlpDOneAvgLat"),    num(macros, "StatCxlFourZeroMlpDFourAvgLat"),
                 num(macros, "StatCxlFourZeroMlpDSixteenAvgLat"), num(macros, "StatCxlFourZeroMlpDSixtyFourAvgLat")],
        "DDR5": [num(macros, "StatDdrMlpDOneAvgLat"),            num(macros, "StatDdrMlpDFourAvgLat"),
                 num(macros, "StatDdrMlpDSixteenAvgLat"),         num(macros, "StatDdrMlpDSixtyFourAvgLat")],
    }
    C20 = COL["cxl"];  C30 = COL["cxl30"];  C31 = COL["cxl31"];  C40 = COL["cxl40"]

    fig, ax = plt.subplots(figsize=(1.85, 2.2))
    ax.plot(depths, t3_lats["DDR5"], color=COL["ddr"], marker="o", markersize=4,
            linewidth=1.5, label="DDR5 only", zorder=2)
    ax.plot(depths, t3_lats["2.0"], color=C20, marker="o", markersize=5,
            linewidth=1.8, label="CXL 2.0", zorder=3)
    ax.plot(depths, t3_lats["3.0"], color=C30, marker="s", markersize=4,
            linewidth=1.6, label="CXL 3.0", zorder=3)
    ax.plot(depths, t3_lats["3.1"], color=C31, marker="^", markersize=4,
            linewidth=1.6, label="CXL 3.1+IDE", zorder=3)
    ax.plot(depths, t3_lats["4.0"], color=C40, marker="D", markersize=4,
            linewidth=1.6, label="CXL 4.0+IDE", zorder=3)

    ax.set_xscale("log")
    ax.set_xticks(depths)
    ax.set_xticklabels([str(d) for d in depths])
    ax.set_xlabel("MLP depth $d$")
    ax.set_ylabel("Mean latency (ns)")
    ax.set_ylim(0, 600); ax.set_yticks([0, 150, 300, 450, 600])
    ax.legend(frameon=True, facecolor="#F5F5F5", edgecolor="#DDDDDD", framealpha=0.90,
              loc="upper left", fontsize=7, handlelength=1.4)
    save(fig, "fig_mlp_sweep.pdf")


def fig_t4_sweep(macros):
    """T4 bandwidth-scalability sweep: mean latency and BW vs. requester count."""
    rs = [1, 4, 8, 16, 32, 64, 128]
    t5_lats = {
        "2.0": [225.5, 225.8, 456.6,  957.0, 1957.8, 4091.6, 8359.4],
        "3.0": [228.8, 229.1, 456.8,  957.2, 1958.0, 4091.8, 8359.6],
        "3.1": [245.3, 245.6, 458.2,  958.6, 1959.4, 4093.2, 8361.0],
        # CXL 4.0: directly simulated (same closed-loop recipe as 2.0-3.1)
        "4.0": [218.5, 218.8, 456.6,  956.9, 1957.7, 4091.6, 8359.3],
    }
    t5_bws = {
        "2.0": [0.284, 1.133, 1.121, 1.070, 1.046, 1.001, 0.980],
        "3.0": [0.280, 1.117, 1.121, 1.069, 1.046, 1.001, 0.980],
        "3.1": [0.261, 1.042, 1.117, 1.068, 1.045, 1.000, 0.980],
        "4.0": [0.293, 1.169, 1.121, 1.070, 1.046, 1.001, 0.980],
    }
    C20 = COL["cxl"];  C30 = COL["cxl30"];  C31 = COL["cxl31"];  C40 = COL["cxl40"]

    fig, ax = plt.subplots(figsize=(1.85, 2.4))
    ax2 = ax.twinx()
    ax2.spines["top"].set_visible(False)
    ax2.spines["right"].set_color("#CCCCCC")

    x = np.arange(len(rs))
    r1, = ax.plot(x, t5_lats["2.0"], color=C20, marker="o", markersize=4,
                  linewidth=1.8, label="CXL 2.0", zorder=3)
    r2, = ax.plot(x, t5_lats["3.0"], color=C30, marker="s", markersize=3,
                  linewidth=1.4, label="CXL 3.0", zorder=3)
    r3, = ax.plot(x, t5_lats["3.1"], color=C31, marker="^", markersize=3,
                  linewidth=1.4, label="CXL 3.1+IDE", zorder=3)
    r4, = ax.plot(x, t5_lats["4.0"], color=C40, marker="D", markersize=3,
                  linewidth=1.4, label="CXL 4.0+IDE", zorder=3)
    bw_line, = ax2.plot(x, t5_bws["2.0"], color=COL["ddr"], marker="o", markersize=3,
                        linewidth=1.4, linestyle="--", zorder=2, label="BW (GB/s)")

    peak_i = t5_bws["2.0"].index(max(t5_bws["2.0"]))
    ax2.scatter([x[peak_i]], [t5_bws["2.0"][peak_i]], s=45, color=COL["ddr"],
                marker="*", zorder=5, edgecolors="none")

    ax.set_xticks(x)
    ax.set_xticklabels([str(r) for r in rs])
    ax.set_xlabel("Requesters $r$")
    ax.set_ylabel("Latency (ns)", fontsize=8)
    ax2.set_ylabel("BW (GB/s)", color=COL["ddr"], fontsize=8)
    ax.tick_params(axis="y", labelcolor="black")
    ax2.tick_params(axis="y", labelcolor=COL["ddr"])
    ax.set_ylim(0, 10000); ax.set_yticks([0, 2500, 5000, 7500, 10000])
    ax2.set_ylim(0, 2.0); ax2.set_yticks([0.0, 0.5, 1.0, 1.5, 2.0])
    ax.legend([bw_line, r1, r2, r3, r4],
              ["BW (GB/s)", "CXL 2.0", "CXL 3.0", "CXL 3.1+IDE", "CXL 4.0+IDE"],
              frameon=True, facecolor="#F5F5F5", edgecolor="#DDDDDD", framealpha=0.90,
              loc="upper left", fontsize=6.5, handlelength=1.4)
    save(fig, "fig_t4_sweep.pdf")


def fig_switch_sweep(macros):
    """CXL switch topology overhead: mean latency vs. hop count (line+scatter)."""
    hops = [0, 1, 2, 3]
    lats = [
        num(macros, "StatSwitchZeroHopMeanLat"),
        num(macros, "StatSwitchOneHopMeanLat"),
        num(macros, "StatSwitchTwoHopMeanLat"),
        num(macros, "StatSwitchThreeHopMeanLat"),
    ]

    fig, ax = plt.subplots(figsize=(1.85, 1.5))
    z = np.polyfit(hops, lats, 1)

    ax.fill_between(hops, lats, alpha=0.07, color=COL["cxl"], zorder=1)
    ax.plot(hops, lats, color=COL["cxl"], linewidth=1.8, zorder=3, solid_capstyle="round")
    ax.scatter(hops, lats, s=38, color=COL["cxl"], zorder=4,
               edgecolors="white", linewidths=0.8)

    for h, l in zip(hops, lats):
        ax.text(h, l + max(lats) * 0.05, f"{l:.0f}", ha="center", va="bottom",
                fontsize=6.5, color="#333333")

    ax.text(0.06, 0.88, f"+{z[0]:.0f} ns/hop",
            transform=ax.transAxes, fontsize=6.5, color="#AAAAAA", style="italic")

    ax.set_xticks(hops)
    ax.set_xticklabels(["0\n(direct)", "1", "2", "3"])
    ax.set_ylabel("Mean latency (ns)", fontsize=7.5)
    ax.set_xlabel("Hops", fontsize=7.5)
    ax.set_xlim(-0.25, 3.25)
    ax.set_ylim(0, max(lats) * 1.28)
    ax.tick_params(axis="both", labelsize=7)
    save(fig, "fig_switch_sweep.pdf")


def fig_gim_sweep(macros):
    """GIM device sweep (E3B credit-saturated): latency flat across device counts; BW flat."""
    devs = [1, 2, 4]
    # E3B: credit-saturated scenario — all device counts give identical latency
    lats_us = [
        num(macros, "StatGimSatOneDevMeanLatUs"),
        num(macros, "StatGimSatTwoDevMeanLatUs"),
        num(macros, "StatGimSatTwoDevMeanLatUs"),   # identical for 4 devs
    ]
    bws = [
        num(macros, "StatGimOneDevBW"),
        num(macros, "StatGimTwoDevBW"),
        num(macros, "StatGimTwoDevBW"),         # identical for 4 devs
    ]

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(6.9, 2.1), gridspec_kw={"wspace": 0.35})
    x = np.arange(len(devs))
    xtl = ["1", "2", "4"]

    # Latency panel: all device counts flat under credit saturation
    c_single = COL["ddr"]
    c_gim    = COL["cxl_alt"]
    ylim_lat = max(lats_us) * 1.48
    ax1.bar([0], [lats_us[0]], color=c_single, width=0.5, zorder=3, edgecolor="none", alpha=0.78)
    ax1.bar([1], [lats_us[1]], color=c_gim,    width=0.5, zorder=3, edgecolor="none", alpha=0.78)
    ax1.bar([2], [lats_us[2]], color=c_gim,    width=0.5, zorder=3, edgecolor="none", alpha=0.78)
    for i, v in enumerate(lats_us):
        ax1.text(i, v + ylim_lat * 0.03, f"{v:.3f}",
                 ha="center", va="bottom", fontsize=7.5, color="#333333")
    ax1.text(0.5, 0.85, "credit-pool bottleneck", transform=ax1.transAxes,
             fontsize=6.5, color="#555555", style="italic", ha="center")
    ax1.set_xticks(x); ax1.set_xticklabels(xtl)
    ax1.set_xlabel("# devices")
    ax1.set_ylabel(r"Mean latency ($\mu$s)")
    ax1.set_ylim(0, ylim_lat)

    # BW panel: blue for 1 device, gray (saturated) for 2/4
    ylim_bw = max(bws) * 1.48
    ax2.bar([0], [bws[0]], color=c_single,   width=0.5, zorder=3, edgecolor="none", alpha=0.78)
    ax2.bar([1], [bws[1]], color=COL["hw2"], width=0.5, zorder=3, edgecolor="none", alpha=0.78)
    ax2.bar([2], [bws[2]], color=COL["hw2"], width=0.5, zorder=3, edgecolor="none", alpha=0.78)
    for i, v in enumerate(bws):
        ax2.text(i, v + ylim_bw * 0.03, f"{v:.2f}",
                 ha="center", va="bottom", fontsize=7.5, color="#333333")
    ax2.text(0.97, 0.82, "credit-pool sat.",
             transform=ax2.transAxes, fontsize=6.5, color="#555555",
             style="italic", ha="right")
    ax2.set_xticks(x); ax2.set_xticklabels(xtl)
    ax2.set_xlabel("# devices")
    ax2.set_ylabel("Bandwidth (GB/s)")
    ax2.set_ylim(0, ylim_bw)

    save(fig, "fig_gim_sweep.pdf")


def fig_type2_vs_type3(macros):
    """Type 2 vs. Type 3 read/write latency — clean grouped bars."""
    labels = ["T2 read", "T6 read", "T6 write"]
    t3_vals = [
        num(macros, "StatTypeThreeTTwoReadLat"),
        num(macros, "StatTypeThreeTFourReadLat"),
        num(macros, "StatTypeThreeTFourWriteLat"),
    ]
    t2_vals = [
        num(macros, "StatTypeTwoTTwoReadLat"),
        num(macros, "StatTypeTwoTFourReadLat"),
        num(macros, "StatTypeTwoTFourWriteLat"),
    ]

    c3 = COL["ddr"]    # muted blue for Type 3
    c2 = COL["cxl"]    # orange-red for Type 2

    fig, ax = plt.subplots(figsize=(1.85, 2.2))
    x = np.arange(3)
    w = 0.32
    ylim = max(t2_vals) * 1.45

    ax.bar(x - w/2, t3_vals, width=w, color=c3, alpha=0.78, label="Type 3",
           zorder=3, edgecolor="none")
    ax.bar(x + w/2, t2_vals, width=w, color=c2, alpha=0.78, label="Type 2",
           zorder=3, edgecolor="none")

    for xi, v in zip(x - w/2, t3_vals):
        ax.text(xi, v + ylim * 0.022, f"{v:.0f}", ha="center", va="bottom",
                fontsize=6.5, color=c3, rotation=90)
    for xi, v in zip(x + w/2, t2_vals):
        ax.text(xi, v + ylim * 0.022, f"{v:.0f}", ha="center", va="bottom",
                fontsize=6.5, color=c2, rotation=90)

    ax.set_xticks(x); ax.set_xticklabels(labels, fontsize=7)
    ax.set_ylabel("Latency (ns)", fontsize=8)
    ax.set_ylim(0, ylim)
    ax.tick_params(axis="y", labelsize=7)
    ax.legend(frameon=True, facecolor="#F5F5F5", edgecolor="#DDDDDD", framealpha=0.90,
              loc="upper left", fontsize=7, handlelength=1.2)
    save(fig, "fig_type2_vs_type3.pdf")


def fig_multihost(macros):
    """Multi-host: 3-panel — latency | DL utilization | BW vs. host count (N=1,2,4,8,16)."""
    hosts = [1, 2, 4, 8, 16]
    # Latency in ns (N=1..4 stable; N=8,16 divergent — convert ms→ns for uniform axis)
    mean_lats_ns = [
        num(macros, "StatMhOneHostMeanLat"),
        num(macros, "StatMhTwoHostMeanLat"),
        num(macros, "StatMhFourHostMeanLat"),
        num(macros, "StatMhEightHostMeanLatMs")  * 1e6,   # ms → ns
        num(macros, "StatMhSixteenHostMeanLatMs") * 1e6,
    ]
    p99_lats_ns = [
        num(macros, "StatMhOneHostPNineNine"),
        num(macros, "StatMhTwoHostPNineNine"),
        num(macros, "StatMhFourHostPNineNine"),
        num(macros, "StatMhEightHostPNineNineMs")  * 1e6,
        num(macros, "StatMhSixteenHostPNineNineMs") * 1e6,
    ]
    bws = [
        num(macros, "StatMhOneHostBW"),
        num(macros, "StatMhTwoHostBW"),
        num(macros, "StatMhFourHostBW"),
        num(macros, "StatMhEightHostBW"),
        num(macros, "StatMhSixteenHostBW"),
    ]
    # DL utilization: N=1..4 measured; N=8,16 oversubscribed (show as oversubscription %)
    dl_reported = [
        num(macros, "StatMhDlUtilOne"),
        num(macros, "StatMhDlUtilTwo"),
        num(macros, "StatMhDlUtilFour"),
        num(macros, "StatMhDlOversubEight"),    # 200%
        num(macros, "StatMhDlOversubSixteen"),  # 400%
    ]

    fig, (ax1, ax2, ax3) = plt.subplots(1, 3, figsize=(6.9, 2.1),
                                         gridspec_kw={"wspace": 0.50})
    x = np.arange(5)
    xtl = ["1", "2", "4", "8", "16"]

    # ── Panel 1: latency (log scale to span ns → ms) ─────────────────────────
    ax1.plot(x, mean_lats_ns, color=COL["cxl"],     marker="o", markersize=5,
             linewidth=1.8, label="Mean", zorder=3)
    ax1.plot(x, p99_lats_ns,  color=COL["cxl_alt"], marker="s", markersize=4,
             linewidth=1.6, linestyle="--", label="p99", zorder=3)
    ax1.axvspan(2.5, 4.5, alpha=0.06, color="red", zorder=0)  # oversubscribed zone
    ax1.set_yscale("log")
    ax1.set_xticks(x); ax1.set_xticklabels(xtl)
    ax1.set_xlabel("Hosts")
    ax1.set_ylabel("Latency (ns)")
    ax1.set_ylim(100, 1e11)
    ax1.legend(frameon=False, loc="upper left", fontsize=7.5, handlelength=1.2)
    ax1.text(3.5, 3e9, "DL\nover-\nsubscribed", ha="center", va="center",
             fontsize=6, color="#CC3333")

    # ── Panel 2: DL utilization / oversubscription ───────────────────────────
    # Show measured util (≤100%) for N=1..4, oversubscription ratio for N=8,16
    ax2.axhline(100, color="#AA2222", linewidth=1.2, linestyle="--", zorder=2)
    ax2.plot(x[:3], dl_reported[:3], color=COL["hw"], marker="^", markersize=5,
             linewidth=1.8, zorder=3)
    # Annotate N=8, N=16 with oversubscription arrows beyond the 100% line
    _lbl = dict(boxstyle="round,pad=0.15", facecolor="white", edgecolor="none", alpha=0.85)
    for xi, v in zip(x[3:], dl_reported[3:]):
        ax2.annotate("", xy=(xi, min(v, 400)), xytext=(xi, 100),
                     arrowprops=dict(arrowstyle="->", color=COL["hw"], lw=1.4))
        ax2.text(xi, min(v, 400) + 20, f"{int(v)}%", ha="center", fontsize=7,
                 color=COL["hw"], bbox=_lbl)
    for xi, v in zip(x[:3], dl_reported[:3]):
        ax2.text(xi, v + 35, f"{int(v)}%", ha="center", fontsize=7, color=COL["hw"],
                 bbox=_lbl)
    ax2.set_xticks(x); ax2.set_xticklabels(xtl)
    ax2.set_xlabel("Hosts")
    ax2.set_ylabel("DL util. / oversub. (%)")
    ax2.set_ylim(0, 450)

    # ── Panel 3: BW per host ──────────────────────────────────────────────────
    bw_color = "#74A9CF"
    colors_bar = [bw_color] * 3 + ["#E07070"] * 2  # red tint for oversubscribed
    ax3.bar(x, bws, color=colors_bar, width=0.5, zorder=3, edgecolor="none", alpha=0.85)
    ax3.axhline(bws[0], color="#CCCCCC", linewidth=1.0, linestyle="--", zorder=2)
    ylim_bw = bws[0] * 1.45
    for xi, v in zip(x, bws):
        ax3.text(xi, v + ylim_bw * 0.025, f"{v:.2f}",
                 ha="center", va="bottom", fontsize=7, color="#333333")
    ax3.set_xticks(x); ax3.set_xticklabels(xtl)
    ax3.set_xlabel("Hosts")
    ax3.set_ylabel("BW per host (GB/s)")
    ax3.set_ylim(0, ylim_bw)

    save(fig, "fig_multihost.pdf")


def fig_credit_pool(macros):
    """Credit-pool cliff: linear-scale mean latency (ns) vs pool depth.

    Closed-loop experiment: 32 requesters, credit limits {disabled,32,16,8,4}.
    Cliff appears below limit=32 (=n_req); latency = n_req×RTT/limit (Little's law).
    """
    lats_ns = [
        num(macros, "StatCreditBaselineMeanLat"),          # disabled → ~236 ns
        num(macros, "StatCreditLimitThirtyTwoMeanLat"),    # limit=32 → ~236 ns
        num(macros, "StatCreditLimitSixteenMeanLat"),      # limit=16 → ~421 ns
        num(macros, "StatCreditLimitEightMeanLat"),        # limit=8  → ~792 ns
        num(macros, "StatCreditLimitFourMeanLat"),         # limit=4  → ~1535 ns
    ]
    x = np.arange(5)

    # Blue (safe, no stall) → orange → dark orange (deeper stall)
    pt_colors = ["#3182BD", "#3182BD", "#E6550D", "#D94801", "#A63603"]

    fig, ax = plt.subplots(figsize=(1.85, 1.5))

    ylim_top = max(lats_ns) * 1.35
    label_offset = ylim_top * 0.05

    # Cliff-zone shading between limit=32 and limit=16
    ax.axvspan(1.5, 2.5, color="#FEF0D9", alpha=0.9, zorder=0, lw=0)
    ax.text(2.0, lats_ns[0] * 0.35, "cliff",
            ha="center", fontsize=6.5, color="#CC5500", style="italic", zorder=3)

    ax.plot(x, lats_ns, color="#CCCCCC", linewidth=1.2, zorder=2, solid_capstyle="round")

    labels = [f"{lats_ns[0]:.0f}", f"{lats_ns[1]:.0f}",
              f"{lats_ns[2]:.0f}", f"{lats_ns[3]:.0f}", f"{lats_ns[4]:.0f}"]
    for i, (xi, yi, c, lab) in enumerate(zip(x, lats_ns, pt_colors, labels)):
        ax.scatter([xi], [yi], s=40, color=c, zorder=4,
                   edgecolors="white", linewidths=0.8)
        x_text = xi + 0.01 if i == 0 else xi
        ha = "left" if i == 0 else "center"
        ax.text(x_text, yi + label_offset, lab,
                ha=ha, va="bottom", fontsize=6.5, color=c)

    ax.set_ylim(0, ylim_top)
    ax.set_xticks(x)
    ax.set_xticklabels(["∞\n(off)", "32", "16", "8", "4"])
    ax.set_xlabel("Credit limit", fontsize=7.5)
    ax.set_ylabel("Mean latency (ns)", fontsize=7.5)
    ax.tick_params(axis="both", labelsize=7)
    save(fig, "fig_credit_pool.pdf")


def fig_simspeed(macros):
    """Simulation throughput comparison: horizontal lollipop with MIPS ranges."""
    # (lo, hi, label, color)  — bottom to top on y-axis
    entries = [
        (0.1,   1.0,  "gem5-class",    COL["hw"]),
        (1.0,   5.0,  "Xerxes",        COL["hw2"]),
        (14.0, 47.0,  "CXRay (zsim)",  COL["cxl"]),
        (100,  100,   "CXRay (trace)", COL["ddr"]),
    ]

    fig, ax = plt.subplots(figsize=(1.85, 2.2))
    ax.spines["left"].set_visible(False)

    for i, (lo, hi, label, color) in enumerate(entries):
        ax.plot([0.05, lo], [i, i], color="#E0E0E0", linewidth=1.0, zorder=1)
        ax.plot([lo, hi], [i, i], color=color, linewidth=2.8,
                solid_capstyle="round", zorder=3, alpha=0.88)
        ax.scatter([hi], [i], s=55, color=color, zorder=5,
                   edgecolors="white", linewidths=0.9)
        if lo < hi:
            ax.scatter([lo], [i], s=22, color=color, zorder=4,
                       edgecolors="white", linewidths=0.7, alpha=0.6)
        if label == "CXRay (trace)":
            txt = ">100 MIPS"
        elif lo < hi:
            txt = f"{lo:.0f}–{hi:.0f} MIPS"
        else:
            txt = f"≤{hi:.0f} MIPS"
        ax.text(hi * 1.5, i, txt, va="center", fontsize=6.5, color=color)

    ax.set_yticks(range(len(entries)))
    ax.set_yticklabels([e[2] for e in entries], fontsize=7)
    ax.tick_params(axis="y", length=0)
    ax.set_xscale("log")
    ax.set_xlim(0.04, 1200)
    ax.set_ylim(-0.6, len(entries) - 0.4)
    ax.set_xlabel("Throughput (MIPS)", fontsize=8)
    ax.set_xticks([0.1, 1, 10, 100, 1000])
    ax.set_xticklabels(["$10^{-1}$", "$10^0$", "$10^1$", "$10^2$", "$10^3$"])
    ax.tick_params(axis="x", labelsize=7)
    ax.grid(True, axis="x", color="#DDDDDD", linewidth=0.6, linestyle="--", zorder=0)
    ax.grid(False, axis="y")
    save(fig, "fig_simspeed.pdf")


def fig_latency_decomp():
    """CXL latency decomposition: grouped stacked bars for 4 CXL versions at 4 T4 points.
    Floor components use the symmetric one-way link model (floors: 145/153/169/147 ns).
    fwd/ret values include link + FEC(4 ns for Gen6/7) + IDE(8 ns for CXL 3.1/4.0).
    DRAM and credit wait are from the CXL 2.0 trace-driven measurements (version-invariant
    since both are DRAM-side; credit pressure scales similarly with load)."""
    # Protocol floor breakdown per version (symmetric model: host + fwd_link + ret_link)
    floors = {
        "2.0": {"host": 97, "fwd": 24, "ret": 24},   # total = 145 ns (no FEC/IDE)
        "3.0": {"host": 97, "fwd": 28, "ret": 28},   # total = 153 ns (link24+FEC4 each)
        "3.1": {"host": 97, "fwd": 36, "ret": 36},   # total = 169 ns (link24+FEC4+IDE8 each)
        "4.0": {"host": 97, "fwd": 25, "ret": 25},   # total = 147 ns (link13+FEC4+IDE8 each)
    }
    # Load-dependent components from CXL 2.0 trace-driven T4 (approx same across versions)
    ops = ["r=1", "r=4", "r=8", "r=16"]
    credit = [0,    0,   0,   0]
    dram   = [66,  67, 293, 778]

    ver_colors = {"2.0": COL["cxl"], "3.0": COL["cxl30"], "3.1": COL["cxl31"], "4.0": COL["cxl40"]}
    ver_labels = {"2.0": "CXL 2.0", "3.0": "CXL 3.0",
                  "3.1": "CXL 3.1+IDE", "4.0": "CXL 4.0+IDE"}

    c_host   = "#4393C3"
    c_link   = "#92C5DE"
    c_credit = "#9467BD"
    c_dram   = "#8B4513"

    n_ops   = len(ops)
    n_vers  = 4
    versions = ["2.0", "3.0", "3.1", "4.0"]
    group_gap = 0.55
    bar_w     = 0.18

    fig, ax = plt.subplots(figsize=(6.9, 2.8))

    x_centers = np.arange(n_ops)
    offsets = np.linspace(-(n_vers - 1) * bar_w / 2,
                           (n_vers - 1) * bar_w / 2, n_vers)

    drawn_legend = set()
    for vi, ver in enumerate(versions):
        f = floors[ver]
        col = ver_colors[ver]
        xs = x_centers + offsets[vi]
        for oi, x in enumerate(xs):
            bot = 0
            # Host overhead
            lbl = f"Host overhead" if "host" not in drawn_legend else ""
            ax.bar(x, f["host"], bar_w, bottom=bot, color=c_host, edgecolor="white",
                   linewidth=0.6, label=lbl, zorder=3, alpha=0.82)
            if "host" not in drawn_legend: drawn_legend.add("host")
            bot += f["host"]
            # Link (fwd+ret)
            link_ns = f["fwd"] + f["ret"]
            lbl = "Link (fwd+ret)" if "link" not in drawn_legend else ""
            ax.bar(x, link_ns, bar_w, bottom=bot, color=c_link, edgecolor="white",
                   linewidth=0.6, label=lbl, zorder=3, alpha=0.82)
            if "link" not in drawn_legend: drawn_legend.add("link")
            bot += link_ns
            # Credit wait
            lbl = "Credit wait" if "credit" not in drawn_legend else ""
            ax.bar(x, credit[oi], bar_w, bottom=bot, color=c_credit, edgecolor="#444444",
                   linewidth=0.35, hatch="xx", label=lbl, zorder=3, alpha=0.82)
            if "credit" not in drawn_legend: drawn_legend.add("credit")
            bot += credit[oi]
            # DRAM
            lbl = "DRAM service" if "dram" not in drawn_legend else ""
            ax.bar(x, dram[oi], bar_w, bottom=bot, color=c_dram, edgecolor="white",
                   linewidth=0.6, label=lbl, zorder=3, alpha=0.82)
            if "dram" not in drawn_legend: drawn_legend.add("dram")

    # Version labels in one row above the first op group; the bars are too
    # close for a rotated label per bar, so the row is ordered to match them.
    label_y = 260
    for vi, ver in enumerate(versions):
        x = x_centers[0] + offsets[vi]
        ax.text(x, label_y, ver_labels[ver], ha="center", va="bottom",
                fontsize=9, color=ver_colors[ver], rotation=90)

    ax.set_xticks(x_centers)
    ax.set_xticklabels(ops)
    ax.set_ylabel("Mean latency (ns)")
    ax.set_ylim(0, 1050)
    ax.set_yticks([0, 150, 300, 450, 600, 750, 900])
    ax.legend(frameon=True, facecolor="#F5F5F5", edgecolor="#DDDDDD", framealpha=0.90,
              loc="upper right", fontsize=8, handlelength=1.2, ncol=2)
    save(fig, "fig_latency_decomp.pdf")


def fig_casestudy():
    """Page migration: work-based completion-time change over no migration.
    Graph kernels from rebuttal/zsim-workloads/RESULTS.md; ResNet-50 and YOLOv4 from
    rebuttal/ML.md, both at the 2 GB fast tier and 512 pages/epoch where each flips.
    Positive means the same work finishes sooner."""
    kernels = ["bfs", "pr", "cc", "bc", "tc", "ResNet-50", "YOLOv4"]
    additive = [6.8, 0.3, 2.4, 0.1, -0.8, 37.4, 41.7]
    full = [0.3, 0.1, 0.4, -0.2, 2.0, -0.03, -0.7]

    fig, ax = plt.subplots(figsize=(4.6, 2.2))
    x = np.arange(len(kernels))
    w = 0.36
    ax.bar(x - w / 2, additive, w, label="Additive",
           color="#9ecae1", edgecolor="k", lw=0.4)
    ax.bar(x + w / 2, full, w, label="Full",
           color="#e6550d", edgecolor="k", lw=0.4)
    ax.axhline(0, color="k", lw=0.6)
    ax.set_xticks(x)
    ax.set_xticklabels(kernels, rotation=90, fontsize=10)
    ax.tick_params(axis="y", labelsize=9)
    ax.set_ylabel("Performance change over\nno migration (%)", fontsize=9)
    ax.set_ylim(-3, 13)
    for i, v in enumerate(additive):
        if v > 13:
            ax.text(x[i] - w / 2, 12.4, "+%d" % round(v), ha="center", va="top", fontsize=10)
    ax.legend(loc="upper left", frameon=False, handlelength=1.2, fontsize=8)
    save(fig, "fig_casestudy_migration.pdf")


def fig_bundled_port(macros):
    """CXL 4.0 Bundled Port: theoretical peak vs credit-limited BW for 1-port and 2-port."""
    one_port_theo  = num(macros, "StatBundledOnePortPeakBW")   # 108.8 GB/s
    two_port_theo  = num(macros, "StatBundledTwoPortPeakBW")   # 217.6 GB/s
    credit_limited = num(macros, "StatBundledCreditLimitedBW")  # 9.4 GB/s

    C40 = COL["cxl40"]
    c_theo   = C40
    c_credit = COL["cxl"]

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(3.6, 2.1), gridspec_kw={"wspace": 0.40})

    # Left: theoretical peak BW comparison
    labels = ["1-port\n(×8)", "2-port\n(×8+×8)"]
    vals   = [one_port_theo, two_port_theo]
    x = np.arange(2)
    bars = ax1.bar(x, vals, width=0.45, color=[c_theo, c_theo], alpha=0.82,
                   edgecolor="none", zorder=3)
    for bar, v in zip(bars, vals):
        ax1.text(bar.get_x() + bar.get_width() / 2, v + 4,
                 f"{v:.0f}", ha="center", va="bottom", fontsize=8, color=c_theo,
                 fontweight="bold")
    ax1.set_xticks(x); ax1.set_xticklabels(labels, fontsize=8)
    ax1.set_ylabel("Theoretical BW (GB/s)", fontsize=8)
    ax1.set_ylim(0, two_port_theo * 1.30)
    ax1.set_title("(a) Peak BW ceiling", fontsize=8)

    # Right: credit-limited BW (flat line — same for both configurations)
    ax2.bar(x, [credit_limited, credit_limited], width=0.45,
            color=c_credit, alpha=0.72, edgecolor="none", zorder=3)
    ax2.axhline(credit_limited, color="#BBBBBB", linewidth=0.9, linestyle=":", zorder=2)
    for xi in x:
        ax2.text(xi, credit_limited + 0.2, f"{credit_limited:.1f}",
                 ha="center", va="bottom", fontsize=8, color=c_credit, fontweight="bold")
    ax2.text(0.5, 0.55, "credit pool\nbottleneck\n(32 credits)", transform=ax2.transAxes,
             ha="center", va="center", fontsize=7, color="#555555", style="italic")
    ax2.set_xticks(x); ax2.set_xticklabels(labels, fontsize=8)
    ax2.set_ylabel("Credit-limited BW (GB/s)", fontsize=8)
    ax2.set_ylim(0, credit_limited * 3.0)
    ax2.set_title("(b) Credit-limited BW", fontsize=8)

    save(fig, "fig_bundled_port.pdf")


def main():
    FIG.mkdir(parents=True, exist_ok=True)
    golden = load_json(GOLDEN)
    macros = load_macros(MACROS)
    fig1_bw_latency(golden, macros)
    fig2_unloaded_latency(golden, macros)
    fig3_loaded_latency(macros)
    fig4_bw_concurrency(macros)
    fig6_ras_comparison(macros)
    fig_calibration_bw_lat(macros)
    fig_version_comparison(macros)
    fig_mlp_sweep(macros)
    fig_t4_sweep(macros)
    fig_switch_sweep(macros)
    fig_gim_sweep(macros)
    fig_type2_vs_type3(macros)
    fig_multihost(macros)
    fig_credit_pool(macros)
    fig_simspeed(macros)
    fig_latency_decomp()
    fig_casestudy()
    fig_bundled_port(macros)


if __name__ == "__main__":
    main()
