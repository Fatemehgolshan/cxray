#!/usr/bin/env python3
"""
Cross-tier timing consistency: the zsim execution tier and the CXRay
trace-driven tier must model the SAME CXL 2.0 link timing.

Background: Figs 4/7 report zsim execution numbers (2.0, DDR) alongside
CXRay closed-loop numbers (3.x/4.0). The two tiers silently diverged --
zsim's xeon configs carried linkOneWayNs=24 (floor 145 ns) while the
calibrated CXRay model sums to 30 ns one-way (floor 157 ns, matching the
Fig. 2 hardware plateau). Loaded zsim means then sat ~30 ns below the
hardware-calibrated floor, and the paper's derived p99 comments no longer
matched any measured value.

This test pins the contract: for every xeon_cxl_c<N>.cfg, host overhead
and one-way link latency must equal the CXRay cxl_2.0.cfg values.

Run with: python3 -m pytest tests/test_tier_timing.py -v
"""
import os
import re

import pytest

REPO = os.path.join(os.path.dirname(__file__), "..")
CXRAY_CFG = os.path.join(REPO, "configs", "cxl_2.0.cfg")
ZSIM_TESTS = os.path.join(REPO, "zsim", "tests")
COUNTS = [4, 8, 16, 32, 64]


def _kv(path, key):
    """Return float value for 'key = <num>' (tolerates trailing ';' / comments)."""
    with open(path) as f:
        text = f.read()
    m = re.search(r"(?<![\w.])" + re.escape(key) + r"\s*=\s*([0-9.]+)", text)
    assert m, f"{key} not found in {path}"
    return float(m.group(1))


def _cxray_one_way():
    return (_kv(CXRAY_CFG, "link.host_port_latency_ns")
            + _kv(CXRAY_CFG, "link.link_phys_latency_ns")
            + _kv(CXRAY_CFG, "link.device_port_latency_ns"))


def _xeon_cfg(n):
    p = os.path.join(ZSIM_TESTS, f"xeon_cxl_c{n}.cfg")
    assert os.path.exists(p), f"missing execution-tier config {p}"
    return p


@pytest.mark.parametrize("cores", COUNTS)
def test_zsim_link_matches_cxray_model(cores):
    """zsim one-way link latency must equal the CXRay port sum."""
    cfg = _xeon_cfg(cores)
    assert _kv(cfg, "linkOneWayNs") == pytest.approx(_cxray_one_way(), abs=0.05), (
        f"{cfg}: linkOneWayNs disagrees with CXRay cxl_2.0.cfg port sum "
        f"({_cxray_one_way()} ns one-way)"
    )


@pytest.mark.parametrize("cores", COUNTS)
def test_zsim_host_overhead_matches_cxray_model(cores):
    """zsim host overhead must equal the CXRay host_overhead_ns."""
    cfg = _xeon_cfg(cores)
    assert _kv(cfg, "hostOverheadNs") == pytest.approx(
        _kv(CXRAY_CFG, "host_overhead_ns"), abs=1e-9)
