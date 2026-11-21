#!/usr/bin/env python3
"""
Parse Xerxes output CSV → normalized result JSON.

Xerxes per-request CSV fields:
    id, type, memid, addr, send, arrive, bus_queuing, bus_time,
    switch_queuing, switch_time, snoop_evict, host_inv,
    dram_queuing, dram_time, total_time

All time values are in Xerxes simulation time units (typically ns).

Usage:
    python xerxes_parser.py --input output/xerxes_result.csv --trace T1 --config cxl_2.0 --output /tmp/xerxes_T1.json
"""

import argparse
import csv
import json
import os
import sys
import statistics

def parse(csv_file, trace_id, config_id):
    if not os.path.exists(csv_file):
        print(f"Error: {csv_file} not found", file=sys.stderr)
        sys.exit(1)

    total_times = []
    dram_times = []
    bus_times = []
    read_count = 0
    write_count = 0

    with open(csv_file, newline='') as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                total = float(row.get("total_time", 0))
                dram = float(row.get("dram_time", 0) or 0)
                bus = float(row.get("bus_time", 0) or 0)
                req_type = row.get("type", "").lower()

                total_times.append(total)
                dram_times.append(dram)
                bus_times.append(bus)

                if "read" in req_type:
                    read_count += 1
                else:
                    write_count += 1
            except (ValueError, KeyError):
                continue

    if not total_times:
        print(f"Warning: no valid rows found in {csv_file}", file=sys.stderr)
        return {"simulator": "xerxes", "trace": trace_id, "config": config_id,
                "notes": "empty output", "latency_ns": {}, "bandwidth_GBps": {}}

    def pct(lst, p):
        if not lst:
            return 0.0
        s = sorted(lst)
        return s[min(len(s)-1, int(len(s) * p / 100))]

    total_ns = max(total_times) if total_times else 1
    total_reqs = len(total_times)
    # BW estimate: assume 64B per request
    total_bytes = total_reqs * 64
    sim_duration_ns = total_ns  # approximate: last completion time

    bw_avg = (total_bytes / (sim_duration_ns * 1e-9) / 1e9) if sim_duration_ns > 0 else 0

    result = {
        "simulator": "xerxes",
        "trace": trace_id,
        "config": config_id,
        "latency_ns": {
            "read_mean": round(statistics.mean(total_times), 2) if total_times else 0,
            "read_p50": round(pct(total_times, 50), 2),
            "read_p95": round(pct(total_times, 95), 2),
            "read_p99": round(pct(total_times, 99), 2),
            "read_p999": round(pct(total_times, 99.9), 2),
            "write_mean": 0.0,  # Xerxes doesn't separate R/W latency easily
        },
        "bandwidth_GBps": {
            "read_avg": round(bw_avg * read_count / max(1, total_reqs), 4),
            "write_avg": round(bw_avg * write_count / max(1, total_reqs), 4),
            "total_avg": round(bw_avg, 4),
            "read_peak": 0.0,  # not directly available
        },
        "link_utilization_avg": 0.0,  # not reported by Xerxes
        "row_buffer_hit_rate": 0.0,   # not reported
        "wall_clock_s": 0.0,
        "total_requests": total_reqs,
        "notes": f"Xerxes CSV: {os.path.basename(csv_file)}",
    }
    return result

def main():
    parser = argparse.ArgumentParser(description="Xerxes output parser")
    parser.add_argument("--input", required=True, help="Xerxes output CSV file")
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
