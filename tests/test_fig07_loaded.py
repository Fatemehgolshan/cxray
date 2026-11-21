#!/usr/bin/env python3
"""
Regression test for the Fig.7 loaded-latency tier.

Fig.7 must show GENUINE load tails: p99 strictly above the mean, widening
with core count. The tier is bursty closed-loop (one outstanding miss per
core, uniform-random reissues, finite credit pools, queued DRAM backend).
An unconstrained-credits or deterministic-stride configuration collapses
the distribution (p99 == mean) and must never feed Fig.7 again: that exact
mistake once shipped 225.8/225.8 ns at 16-core CXL 2.0, below the old
loaded mean with zero tails.

Anchor (paper StatLdTwoZeroEightCoreMean/P99, full 10k-req recipe):
  CXL 2.0, 8 cores -> mean 229.3 ns, p99 287.5 ns (ratio ~1.25x)

This test runs a short 1k-req/requester version and asserts the structural
invariants (tails exist, loaded mean at/above the 225.8 ns envelope floor),
not the exact anchor values.

Run with: python3 -m pytest tests/test_fig07_loaded.py -v
"""
import csv
import os
import subprocess
import sys
import tempfile

import pytest

REPO = os.path.join(os.path.dirname(__file__), "..")
TIER = os.path.join(REPO, "scripts", "run_cxray_loaded.py")

# Paper anchors (StatLdTwoZero*): full-recipe values for reference.
ANCHOR_MEAN = 229.3
ANCHOR_P99 = 287.5
# Fig.4 envelope floor at 8c (StatCxlEightCoreAvgLat): loaded must not sink below it.
ENVELOPE_FLOOR = 225.8


def _loaded_csv(per_req=1000):
    out = tempfile.mkdtemp()
    work = tempfile.mkdtemp()
    cmd = [sys.executable, TIER, "--out-csv", out, "--work", work,
           "--cores", "8", "--versions", "2.0", "--per-req", str(per_req)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    assert r.returncode == 0, f"loaded tier failed: {r.stderr[-800:]}"
    with open(os.path.join(out, "fig07_loaded.csv")) as f:
        rows = list(csv.DictReader(f))
    assert len(rows) == 1, f"expected one row, got {rows}"
    return rows[0]


def test_tier_script_exists():
    """The loaded tier script is the Fig.7 producer."""
    assert os.path.exists(TIER), (
        "scripts/run_cxray_loaded.py missing: Fig.7 has no loaded-latency "
        "producer (the Fig.4 envelope tier is degenerate by construction)")


def test_loaded_tier_has_tails():
    """p99 must strictly exceed the mean (tails exist under load)."""
    row = _loaded_csv()
    mean, p99 = float(row["mean_lat_ns"]), float(row["p99_lat_ns"])
    assert row["method"] == "simulated"
    assert p99 > mean, (
        f"degenerate distribution: p99={p99} <= mean={mean}; the tier lost "
        f"its burstiness (check credits/backend/address mode)")
    assert p99 / mean > 1.1, (
        f"tail too thin: p99/mean={p99 / mean:.2f}; expected a visible tail")


def test_loaded_mean_near_anchor_and_above_floor():
    """Short-run mean must sit near the paper anchor and above the floor."""
    row = _loaded_csv()
    mean = float(row["mean_lat_ns"])
    assert mean >= ENVELOPE_FLOOR, (
        f"loaded mean {mean} below the {ENVELOPE_FLOOR} ns envelope floor: "
        f"queueing cannot reduce latency, the recipe is wrong")
    assert abs(mean - ANCHOR_MEAN) / ANCHOR_MEAN < 0.15, (
        f"mean {mean} far from paper anchor {ANCHOR_MEAN}")
