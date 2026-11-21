#!/usr/bin/env python3
"""
Parse Ramulator2 output → normalized result JSON.

Ramulator2 outputs a YAML stats file with fields like:
    memory_system.DRAM.average_read_latency
    memory_system.DRAM.total_bandwidth
    ...

Usage:
    python ramulator2_parser.py --input /tmp/ramulator2_stats.yaml --trace T1 --config cxl_2.0 --output /tmp/ram2_T1.json
"""

import argparse
import json
import os
import sys
import re

def parse_yaml_flat(yaml_file):
    """
    Simple flat YAML parser for Ramulator2 stats (key: value format).
    Handles nested keys via dot notation in the keys themselves.
    """
    stats = {}
    with open(yaml_file) as f:
        for line in f:
            line = line.rstrip()
            if not line or line.startswith('#'):
                continue
            # match "key: value" or "  key: value"
            m = re.match(r'^[\s-]*([^:]+):\s*(.+)$', line)
            if m:
                key = m.group(1).strip()
                val = m.group(2).strip()
                try:
                    stats[key] = float(val)
                except ValueError:
                    stats[key] = val
    return stats

def find_stat(stats, patterns):
    """Search for a stat matching any of the given substring patterns."""
    for pat in patterns:
        for k, v in stats.items():
            if pat.lower() in k.lower():
                try:
                    return float(v)
                except (TypeError, ValueError):
                    pass
    return 0.0

def parse(input_path, trace_id, config_id):
    if not os.path.exists(input_path):
        print(f"Error: {input_path} not found", file=sys.stderr)
        sys.exit(1)

    stats = parse_yaml_flat(input_path)

    # Ramulator2 latency is in DRAM cycles (tCK = 0.625ns for DDR4-3200)
    tCK_ns = 0.625

    avg_read_lat_cyc = find_stat(stats, ["avg_read_latency", "average_read", "read_latency"])
    avg_write_lat_cyc = find_stat(stats, ["avg_write_latency", "average_write", "write_latency"])
    total_reqs = find_stat(stats, ["total_num_read_requests", "num_read_reqs", "total_requests", "num_requests"])

    # BW: Ramulator2 doesn't report bandwidth directly; compute from counters
    # BW (GB/s) = num_read_reqs * 64B / (memory_system_cycles * tCK_ns * 1e-9) / 1e9
    mem_cycles = find_stat(stats, ["memory_system_cycles"])
    read_bw = 0.0
    if total_reqs > 0 and mem_cycles > 0:
        sim_time_s = mem_cycles * tCK_ns * 1e-9
        read_bw = (total_reqs * 64) / sim_time_s / 1e9

    write_reqs = find_stat(stats, ["total_num_write_requests", "num_write_reqs"])
    write_bw = 0.0
    if write_reqs > 0 and mem_cycles > 0:
        sim_time_s = mem_cycles * tCK_ns * 1e-9
        write_bw = (write_reqs * 64) / sim_time_s / 1e9

    total_bw = read_bw + write_bw

    # Row hit rate from raw counters
    row_hits = find_stat(stats, ["row_hits", "read_row_hits"])
    row_hit = (row_hits / total_reqs) if total_reqs > 0 and row_hits > 0 else \
              find_stat(stats, ["row_hit_rate", "row_buffer_hit"])

    result = {
        "simulator": "ramulator2",
        "trace": trace_id,
        "config": config_id,
        "latency_ns": {
            "read_mean": round(avg_read_lat_cyc * tCK_ns, 2),
            "read_p50": 0.0,
            "read_p95": 0.0,
            "read_p99": 0.0,
            "read_p999": 0.0,
            "write_mean": round(avg_write_lat_cyc * tCK_ns, 2),
        },
        "bandwidth_GBps": {
            "read_avg": round(read_bw, 4),
            "write_avg": round(write_bw, 4),
            "total_avg": round(total_bw, 4),
            "read_peak": 0.0,
        },
        "link_utilization_avg": 0.0,
        "row_buffer_hit_rate": round(row_hit, 4),
        "wall_clock_s": 0.0,
        "total_requests": int(total_reqs),
        "notes": "Ramulator2 (DRAM layer only, no CXL link)",
    }
    return result

def main():
    parser = argparse.ArgumentParser(description="Ramulator2 output parser")
    parser.add_argument("--input", required=True, help="Ramulator2 stats YAML file")
    parser.add_argument("--trace", required=True, help="Trace ID")
    parser.add_argument("--config", required=True, help="Config ID")
    parser.add_argument("--output", required=True, help="Output normalized JSON")
    args = parser.parse_args()

    result = parse(args.input, args.trace, args.config)

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, 'w') as f:
        json.dump(result, f, indent=2)
    print(f"Normalized result → {args.output}")
    print(f"  read_mean_latency: {result['latency_ns'].get('read_mean', 'N/A')} ns")

if __name__ == "__main__":
    main()
