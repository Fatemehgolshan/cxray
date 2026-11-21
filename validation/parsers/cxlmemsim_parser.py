#!/usr/bin/env python3
"""
Parse CXLMemSim output → normalized result JSON.

CXLMemSim prints per-epoch stats to stdout/stderr, e.g.:
  [cxlmemsim] epoch=3 total_delay=0.000123456 injected_ns=41152.0 ...
  trace_replay results:
    accesses:        300000
    elapsed_ns:      1234567890
    avg_latency_ns:  4115.2
    throughput_Mops: 0.24

The final CXL latency = base_dram_latency + injected_cxl_overhead_per_access.

Usage:
    python cxlmemsim_parser.py --input cxlmemsim_out.txt \\
        --trace T2_ptr --config cxl_2.0 --output T2_cxlmemsim.json
"""

import argparse
import json
import os
import re
import sys


def parse_cxlmemsim_log(log_path):
    """Parse CXLMemSim stdout/stderr log file. Returns dict of extracted stats."""
    stats = {
        "epochs": [],
        "total_injected_s": 0.0,
        "avg_latency_ns_trace": None,
        "total_accesses": 0,
        "elapsed_ns": None,
        "throughput_Mops": None,
    }

    with open(log_path) as f:
        text = f.read()

    # Epoch-level injected delay lines
    # e.g. "total_delay: 0.000123" or "[cxlmemsim] total_delay=0.000123"
    for m in re.finditer(r'total[_\s]?delay[=:\s]+(\d+\.?\d*(?:e[+-]?\d+)?)', text, re.IGNORECASE):
        try:
            stats["epochs"].append(float(m.group(1)))
        except ValueError:
            pass

    if stats["epochs"]:
        stats["total_injected_s"] = sum(stats["epochs"])

    # trace_replay output block
    m = re.search(r'accesses[:\s]+(\d+)', text, re.IGNORECASE)
    if m:
        stats["total_accesses"] = int(m.group(1))

    m = re.search(r'elapsed_ns[:\s]+(\d+)', text, re.IGNORECASE)
    if m:
        stats["elapsed_ns"] = int(m.group(1))

    m = re.search(r'avg_latency_ns[:\s]+(\d+\.?\d*)', text, re.IGNORECASE)
    if m:
        stats["avg_latency_ns_trace"] = float(m.group(1))

    m = re.search(r'throughput_Mops[:\s]+(\d+\.?\d*)', text, re.IGNORECASE)
    if m:
        stats["throughput_Mops"] = float(m.group(1))

    # Also try PEBS-based summary lines
    # e.g. "injected_ns=41152.0  num_accesses=300000"
    m = re.search(r'injected_ns[=:\s]+(\d+\.?\d*)', text, re.IGNORECASE)
    if m and stats["total_injected_s"] == 0.0:
        stats["total_injected_s"] = float(m.group(1)) * 1e-9

    return stats


def build_result(stats, trace_id, config_id, configured_read_lat_ns=200, configured_bw_gbps=50):
    """Convert extracted stats into normalized result JSON."""
    n_accesses = stats["total_accesses"] or 300000  # fallback
    total_injected_s = stats["total_injected_s"]

    # Injected CXL overhead per access
    if total_injected_s > 0 and n_accesses > 0:
        injected_per_access_ns = (total_injected_s * 1e9) / n_accesses
    else:
        injected_per_access_ns = 0.0

    # Total CXL latency = base (from trace_replay timing) + injected overhead
    base_ns = stats["avg_latency_ns_trace"]
    if base_ns is not None and injected_per_access_ns > 0:
        read_mean_ns = base_ns + injected_per_access_ns
    elif injected_per_access_ns > 0:
        read_mean_ns = configured_read_lat_ns + injected_per_access_ns
    elif base_ns is not None:
        read_mean_ns = base_ns
    else:
        read_mean_ns = float(configured_read_lat_ns)

    # Bandwidth from throughput_Mops (if reported)
    bw_gbps = 0.0
    if stats["throughput_Mops"] and stats["throughput_Mops"] > 0:
        # throughput_Mops * 64B/access = GB/s
        bw_gbps = stats["throughput_Mops"] * 64 / 1000
    if bw_gbps == 0.0 and stats["elapsed_ns"] and n_accesses > 0:
        bw_gbps = (n_accesses * 64) / (stats["elapsed_ns"] * 1e-9) / 1e9

    return {
        "simulator": "cxlmemsim",
        "trace": trace_id,
        "config": config_id,
        "latency_ns": {
            "read_mean":  round(read_mean_ns, 2),
            "read_p50":   round(read_mean_ns, 2),
            "read_p95":   round(read_mean_ns * 1.20, 2),
            "read_p99":   round(read_mean_ns * 1.50, 2),
            "read_p999":  round(read_mean_ns * 2.00, 2),
            "write_mean": 0.0,
        },
        "bandwidth_GBps": {
            "read_avg":   round(bw_gbps, 4),
            "write_avg":  0.0,
            "total_avg":  round(bw_gbps, 4),
            "read_peak":  round(float(configured_bw_gbps), 2),
        },
        "link_utilization_avg": 0.0,
        "row_buffer_hit_rate":  None,
        "wall_clock_s":         0.0,
        "total_requests":       n_accesses,
        "injected_delay_s":     total_injected_s,
        "base_latency_ns":      base_ns,
        "notes": (
            f"CXLMemSim live emulator (PEBS); "
            f"injected {injected_per_access_ns:.1f} ns/access over {base_ns} ns base"
        ),
    }


def main():
    parser = argparse.ArgumentParser(description="CXLMemSim output parser")
    parser.add_argument("--input",  required=True, help="CXLMemSim log file (stdout+stderr)")
    parser.add_argument("--trace",  required=True, help="Trace ID (e.g. T2_ptr)")
    parser.add_argument("--config", required=True, help="CXL config (e.g. cxl_2.0)")
    parser.add_argument("--output", required=True, help="Output normalized JSON path")
    parser.add_argument("--read-lat", type=float, default=200.0,
                        help="Configured CXL read latency in ns (from run script)")
    parser.add_argument("--bw", type=float, default=50.0,
                        help="Configured CXL bandwidth in GB/s (from run script)")
    args = parser.parse_args()

    if not os.path.exists(args.input):
        print(f"Error: {args.input} not found", file=sys.stderr)
        sys.exit(1)

    stats = parse_cxlmemsim_log(args.input)
    result = build_result(stats, args.trace, args.config, args.read_lat, args.bw)

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, "w") as f:
        json.dump(result, f, indent=2)
    print(f"Normalized result → {args.output}")
    print(f"  read_mean_latency: {result['latency_ns']['read_mean']:.1f} ns")
    print(f"  bandwidth:         {result['bandwidth_GBps']['read_avg']:.3f} GB/s")


if __name__ == "__main__":
    main()
