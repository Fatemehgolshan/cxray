#!/usr/bin/env python3
"""
Regression test for the Fig.4 closed-loop envelope tier.

Fig.4 must come from CXRay closed-loop multicore sweeps (V4 recipe:
unconstrained credits, closed_loop_per_req=10000, stride 64 B, 16 MB
regions, default/simple DRAM backend) -- NOT from zsim execution, which
models finite link/DRAM queues and collapses under the same load.
(Fig.7 loaded tails live in tests/test_fig07_loaded.py.)

Anchor values are paper macros reproduced to 0.1 ns by this recipe:
  CXL 3.1, 16 cores -> 245.2 ns (StatCxlThreeOneSixteenCoreAvgLat)
  CXL 3.0, 32 cores -> 229.1 ns (StatCxlThreeZeroThirtyTwoCoreAvgLat)
  CXL 4.0, 16 cores -> 218.5 ns (StatCxlFourZeroSixteenCoreAvgLat)

Run with: python3 -m pytest tests/test_fig04_tier.py -v
"""
import json
import os
import subprocess
import sys
import tempfile

import pytest

REPO = os.path.join(os.path.dirname(__file__), "..")
SIM = os.path.join(REPO, "src", "cxray.py")
CFGDIR = os.path.join(REPO, "configs")

TIER = os.path.join(REPO, "scripts", "run_cxray_multicore.py")


def _closed_loop(cfg_name, n_req):
    """Run one V4-recipe closed-loop point, return (mean_ns, bw_GBps)."""
    seed = os.path.join(tempfile.mkdtemp(), "seed.trace")
    with open(seed, "w") as f:
        for rid in range(n_req):
            f.write(f"0 R 0x{rid * 64 * 4096:016x} 64 {rid}\n")
    out = tempfile.mkdtemp()
    cmd = [sys.executable, SIM, "-c", os.path.join(CFGDIR, cfg_name),
           "-t", seed, "-o", out,
           "--set", "inject.closed_loop_per_req=10000",
           "--set", "inject.stride_bytes=64",
           "--set", "inject.region_bytes=16777216",
           "--set", "link.credits.m2s_req=0",
           "--set", "link.credits.m2s_rwd=0",
           "--set", "link.credits.s2m_drs=0",
           "--set", "link.credits.s2m_ndr=0"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    assert r.returncode == 0, f"sim failed: {r.stderr[-500:]}"
    with open(os.path.join(out, "summary.json")) as f:
        s = json.load(f)
    return s["latency_ns"]["all_mean"], s["bandwidth_GBps"]["total_avg"]


def test_tier_script_exists():
    """The closed-loop tier script is the Figs 4/7 producer."""
    assert os.path.exists(TIER), (
        "scripts/run_cxray_multicore.py missing: Figs 4/7 have no "
        "closed-loop producer (zsim execution is the wrong tier: it "
        "collapses where the paper shows flat latency)")


def test_multicore_31_16c_matches_paper():
    mean, _ = _closed_loop("cxl_3.1.cfg", 16)
    assert mean == pytest.approx(245.2, abs=0.5)


def test_multicore_30_32c_matches_paper():
    mean, _ = _closed_loop("cxl_3.0.cfg", 32)
    assert mean == pytest.approx(229.1, abs=0.5)


def test_multicore_40_16c_matches_paper():
    mean, _ = _closed_loop("cxl_4.0.cfg", 16)
    assert mean == pytest.approx(218.5, abs=0.5)


def test_multicore_bandwidth_little_law():
    """BW must equal cores*64B/mean (one in-flight miss per core)."""
    for cfg, n, lat in [("cxl_3.1.cfg", 16, 245.2),
                        ("cxl_3.0.cfg", 32, 229.1)]:
        mean, bw = _closed_loop(cfg, n)
        assert bw == pytest.approx(n * 64 / mean, rel=0.01)
        assert mean == pytest.approx(lat, abs=0.5)
