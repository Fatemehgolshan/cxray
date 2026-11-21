#!/usr/bin/env python3
"""
Unit tests for multi-host CXRay support.
Run with: python3 -m pytest tests/test_multihost.py -v
"""
import sys, os, tempfile, json
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

import pytest
from cxray import (
    load_multi_traces, merge_traces,
    CXRaySimulator, resolve_config, parse_libconfig,
    write_outputs,
)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _minimal_cfg(num_hosts=1, per_host_credits=16):
    """Return a resolved config dict with multi-host defaults."""
    raw = parse_libconfig(f"""
        cxl_version = 2.0
        link.pcie_gen = 5
        link.pcie_lanes = 8
        link.host_port_latency_ns = 15
        link.link_phys_latency_ns = 3
        link.device_port_latency_ns = 12
        link.flit_size_bytes = 66
        link.flit_payload_bytes = 64
        link.fec_enable = false
        link.fec_decode_latency_ns = 0
        link.retry_prob_per_flit = 1e-12
        link.vc_arbitration_ns = 1.0
        link.credits.m2s_req = 32
        link.credits.m2s_rwd = 16
        link.credits.s2m_ndr = 32
        link.credits.s2m_drs = 32
        has_switch = false
        device.controller_latency_ns = 20
        device.dram_type = ddr5_4800
        device.coherence_overhead_ns = 0
        device.ide_enable = false
        host_overhead_ns = 97
        ras.enable = false
        numa.num_nodes = 1
        migration.enable = false
        multihost.num_hosts = {num_hosts}
        multihost.per_host_credits = {per_host_credits}
        multihost.downstream_bw_GBps = 0
        multihost.separate_upstream_links = true
    """)
    return resolve_config(raw)


def _make_trace_file(rows, tmp_dir, name):
    path = os.path.join(tmp_dir, name)
    with open(path, "w") as f:
        for ts, op, addr, size, rid in rows:
            f.write(f"{ts} {op} {hex(addr)} {size} {rid}\n")
    return path


def _req(ts, host_id=0, op="R", addr=0x1000, size=64):
    return {"timestamp_ns": ts, "op": op, "addr": addr,
            "size": size, "requester_id": host_id, "flags": set()}


# ---------------------------------------------------------------------------
# Step 3 tests
# ---------------------------------------------------------------------------

def test_merge_traces_order():
    """merge_traces must yield requests sorted by timestamp across all hosts."""
    h0 = [_req(100, 0), _req(300, 0), _req(500, 0)]
    h1 = [_req(200, 1), _req(400, 1), _req(600, 1)]
    merged = list(merge_traces([h0, h1]))
    ts = [r["timestamp_ns"] for r in merged]
    assert ts == sorted(ts), f"merge not sorted: {ts}"
    assert len(merged) == 6


def test_host_id_assignment(tmp_path):
    """load_multi_traces must assign host_id = file index, preserve requester_id."""
    p0 = _make_trace_file([(100, "R", 0x1000, 64, 99)], str(tmp_path), "h0.trace")
    p1 = _make_trace_file([(200, "R", 0x2000, 64, 99)], str(tmp_path), "h1.trace")
    traces = load_multi_traces([p0, p1])
    assert traces[0][0]["host_id"] == 0, "host_id for file 0 wrong"
    assert traces[1][0]["host_id"] == 1, "host_id for file 1 wrong"
    # requester_id must be preserved from the trace file, not overwritten.
    assert traces[0][0]["requester_id"] == 99, "requester_id from trace not preserved"
    assert traces[1][0]["requester_id"] == 99, "requester_id from trace not preserved"


def test_merge_traces_deterministic():
    """Tie-breaking by host_id makes merge deterministic."""
    h0 = [_req(100, 0)]
    h1 = [_req(100, 1)]
    r1 = list(merge_traces([h0, h1]))
    r2 = list(merge_traces([h0, h1]))
    assert [r["requester_id"] for r in r1] == [r["requester_id"] for r in r2]


# ---------------------------------------------------------------------------
# Step 4/5/6 tests — simulator correctness
# ---------------------------------------------------------------------------

def test_single_host_regression():
    """
    2-host sim with both hosts running identical requests must produce
    aggregate latency close to a matching single-host sim.
    Only checks that single-host code path is not broken by the refactor.
    """
    cfg1 = _minimal_cfg(num_hosts=1)
    cfg2 = _minimal_cfg(num_hosts=2)

    reqs = [_req(i * 1000, 0) for i in range(50)]

    sim1 = CXRaySimulator(cfg1)
    sim1.run(reqs)
    lats1 = [r["latency_ns"] for r in sim1.results]

    # Same requests but tagged for host 0 in a 2-host sim
    sim2 = CXRaySimulator(cfg2)
    sim2.run(reqs)
    lats2 = [r["latency_ns"] for r in sim2.results]

    mean1 = sum(lats1) / len(lats1)
    mean2 = sum(lats2) / len(lats2)
    # Allow up to 5% divergence; they share the same link but have different pool depths
    assert abs(mean1 - mean2) / mean1 < 0.05, (
        f"Single-host regression: mean lat {mean1:.1f} vs {mean2:.1f}")


def test_credit_isolation():
    """
    Host 0 saturating its credit pool must not stall host 1.
    Verified by checking host 1's credit_stall_ns remains 0 even when
    host 0 floods with concurrent requests.
    """
    cfg = _minimal_cfg(num_hosts=2, per_host_credits=2)
    sim = CXRaySimulator(cfg)

    # Host 0: 20 requests at t=0 (saturates pool immediately)
    reqs = [_req(0, host_id=0, addr=0x1000 + i * 64) for i in range(20)]
    # Host 1: 1 request at t=0, well within its own pool
    reqs.append(_req(0, host_id=1, addr=0x9000))
    reqs.sort(key=lambda r: (r["timestamp_ns"], r["requester_id"]))

    sim.run(reqs)
    h1_stalls = [r["credit_stall_ns"] for r in sim.results
                 if r["requester_id"] == 1]
    assert all(s == 0.0 for s in h1_stalls), (
        f"Host 1 unexpectedly stalled: {h1_stalls}")


def test_switch_dl_serialization():
    """
    With 2 hosts, two simultaneous requests must be serialized on switch_dl:
    the second request's flit-start time must be >= the first's flit-start + flit_tx.
    Validated indirectly: the two complete_ns must differ by at least one flit-TX time.
    """
    cfg = _minimal_cfg(num_hosts=2)
    cfg["has_switch"] = True
    # Add minimal switch config so resolve_config can derive switch_fwd_ns
    cfg.setdefault("switch", {}).update({
        "mode": "pbr", "latency_ns": 40, "num_hops": 1,
        "queue_capacity_flits": 32,
    })
    from cxray import resolve_config as rc2
    cfg = rc2(cfg)

    sim = CXRaySimulator(cfg)
    reqs = [_req(0, host_id=0), _req(0, host_id=1, addr=0x2000)]
    sim.run(reqs)

    c = sorted(r["complete_ns"] for r in sim.results)
    flit_bw  = cfg["derived"]["link_bandwidth_GBps"]
    flit_pay = cfg["link"]["flit_payload_bytes"]
    min_delta = flit_pay / flit_bw   # ns per flit at this link speed
    assert c[1] - c[0] >= min_delta - 1e-9, (
        f"switch_dl serialization gap {c[1]-c[0]:.2f} < flit_tx {min_delta:.2f}")


# ---------------------------------------------------------------------------
# Step 7 test
# ---------------------------------------------------------------------------

def test_per_host_stats_sum():
    """sum(per_host[h].requests) must equal total_requests."""
    cfg = _minimal_cfg(num_hosts=3)
    sim = CXRaySimulator(cfg)
    reqs = [_req(i * 500, host_id=i % 3) for i in range(60)]
    sim.run(reqs)

    summary = sim.compute_summary("dummy.trace", "dummy.cfg", 0.1)
    ph = summary["per_host"]
    assert ph, "per_host empty for num_hosts=3"
    total = sum(v["requests"] for v in ph.values())
    assert total == summary["total_requests"], (
        f"per_host sum {total} != total_requests {summary['total_requests']}")


def test_per_host_empty_for_single_host():
    """per_host must be {} when num_hosts == 1."""
    cfg = _minimal_cfg(num_hosts=1)
    sim = CXRaySimulator(cfg)
    sim.run([_req(0)])
    summary = sim.compute_summary("t.trace", "c.cfg", 0.01)
    assert summary["per_host"] == {}, "per_host non-empty for single-host sim"


# ---------------------------------------------------------------------------
# Step 8 test
# ---------------------------------------------------------------------------

def test_per_host_csv_written(tmp_path):
    """write_outputs must create per_host/host_N_latency.csv for multi-host."""
    cfg = _minimal_cfg(num_hosts=2)
    sim = CXRaySimulator(cfg)
    reqs = [_req(i * 500, host_id=i % 2) for i in range(20)]
    sim.run(reqs)
    import time as _time
    summary = sim.compute_summary("t.trace", "c.cfg", 0.01)
    write_outputs(sim, cfg, summary, str(tmp_path), "t.trace", "c.cfg", 0.01)
    assert os.path.exists(str(tmp_path / "per_host" / "host_0_latency.csv"))
    assert os.path.exists(str(tmp_path / "per_host" / "host_1_latency.csv"))

    with open(str(tmp_path / "per_host" / "host_0_latency.csv")) as f:
        lines = f.readlines()
    assert lines[0].startswith("timestamp_ns"), "missing CSV header"
    assert len(lines) > 1, "no data rows"
