#!/usr/bin/env python3
"""
Parse DRAMSim3 output → normalized result JSON.

DRAMSim3 outputs a JSON stats file (one per channel) with fields like:
    average_read_latency, average_write_latency, bandwidth, ...

It also writes a summary to stdout during simulation (captured by run script).

Usage:
    python dramsim3_parser.py --input /tmp/dramsim3_stats/ --trace T1 --config cxl_2.0 --output /tmp/dramsim3_T1.json

The --input can be either a directory of JSON stats files or a single JSON file.
"""

import argparse
import json
import os
import sys
import glob

# DRAMSim3 stat key names (may vary by version)
# Keys from DRAMsim3 JSON output
STAT_KEYS = {
    "avg_read_latency":  ["average_read_latency", "avg_read_latency", "read_latency"],
    "avg_write_latency": ["average_write_latency", "avg_write_latency", "write_latency"],
    "read_bw":           ["read_bandwidth", "bandwidth_read", "rd_bw"],
    "write_bw":          ["write_bandwidth", "bandwidth_write", "wr_bw"],
    "total_bw":          ["total_bandwidth", "bandwidth", "average_bandwidth"],
    "row_hit_rate":      ["row_hit_rate", "row_buffer_hit_rate"],
}

def find_key(d, candidates):
    """Try each candidate key name; return value if found."""
    for k in candidates:
        if k in d:
            return d[k]
    return None

def parse_json_stats(stats_file):
    """Parse a DRAMSim3 JSON stats file.

    DRAMSim3 JSON structure: {"0": {channel_stats}, "1": {...}, ...}
    Flatten by averaging across channels.
    """
    with open(stats_file) as f:
        data = json.load(f)

    # If top-level keys are channel ids (integers as strings), flatten them
    flat = {}
    channels = []
    for k, v in data.items():
        if isinstance(v, dict):
            channels.append(v)
    if channels:
        # Average numeric values across channels
        all_keys = set(k for ch in channels for k in ch)
        for k in all_keys:
            vals = [ch[k] for ch in channels if k in ch and isinstance(ch[k], (int, float))]
            if vals:
                flat[k] = sum(vals) / len(vals)
        data = flat

    result = {}
    for our_key, candidates in STAT_KEYS.items():
        val = find_key(data, candidates)
        if val is not None:
            result[our_key] = float(val)

    # Compute row hit rate from raw counters if not reported directly
    if "row_hit_rate" not in result:
        hits = data.get("num_read_row_hits", 0)
        done = data.get("num_reads_done", 0)
        if done and done > 0:
            result["row_hit_rate"] = float(hits) / float(done)

    return result

def parse(input_path, trace_id, config_id):
    """Parse DRAMSim3 output from directory or single file."""

    all_stats = []

    if os.path.isdir(input_path):
        # Look for JSON stats files
        json_files = (glob.glob(os.path.join(input_path, "*.json")) +
                      glob.glob(os.path.join(input_path, "*.stats")))
        for jf in json_files:
            try:
                stats = parse_json_stats(jf)
                if stats:
                    all_stats.append(stats)
            except Exception as e:
                print(f"Warning: could not parse {jf}: {e}", file=sys.stderr)

        # Also try text stats file (DRAMSim3 can output text format)
        txt_files = glob.glob(os.path.join(input_path, "*.txt"))
        for tf in txt_files:
            stats = parse_text_stats(tf)
            if stats:
                all_stats.append(stats)

    elif os.path.isfile(input_path):
        try:
            stats = parse_json_stats(input_path)
            all_stats.append(stats)
        except:
            stats = parse_text_stats(input_path)
            if stats:
                all_stats.append(stats)

    if not all_stats:
        print(f"Warning: no stats found in {input_path}", file=sys.stderr)
        return {"simulator": "dramsim3", "trace": trace_id, "config": config_id,
                "notes": "no stats found", "latency_ns": {}, "bandwidth_GBps": {}}

    # Average across channels
    def avg_key(key):
        vals = [s[key] for s in all_stats if key in s]
        return sum(vals) / len(vals) if vals else 0.0

    # DRAMSim3 latency is in cycles; 1 cycle = tCK = 0.417ns (DDR5-4800) or 0.625ns (DDR4-3200)
    # We use 0.625ns (DDR4-3200) as our CXL 2.0 baseline
    tCK_ns = 0.625
    avg_read_lat_cyc = avg_key("avg_read_latency")
    avg_write_lat_cyc = avg_key("avg_write_latency")
    avg_read_lat_ns = avg_read_lat_cyc * tCK_ns
    avg_write_lat_ns = avg_write_lat_cyc * tCK_ns

    # DRAMSim3 average_bandwidth is in GB/s (total_bytes / sim_time_ns)
    total_bw = avg_key("total_bw")
    read_bw = avg_key("read_bw")
    write_bw = avg_key("write_bw")
    # If only total_bw found (average_bandwidth) and no write traffic, use as read BW
    if total_bw > 0 and read_bw == 0.0 and write_bw == 0.0:
        read_bw = total_bw

    result = {
        "simulator": "dramsim3",
        "trace": trace_id,
        "config": config_id,
        "latency_ns": {
            "read_mean": round(avg_read_lat_ns, 2),
            "read_p50": 0.0,   # not reported
            "read_p95": 0.0,
            "read_p99": 0.0,
            "read_p999": 0.0,
            "write_mean": round(avg_write_lat_ns, 2),
        },
        "bandwidth_GBps": {
            "read_avg": round(read_bw, 4),
            "write_avg": round(write_bw, 4),
            "total_avg": round(total_bw, 4),
            "read_peak": 0.0,
        },
        "link_utilization_avg": 0.0,
        "row_buffer_hit_rate": round(avg_key("row_hit_rate"), 4),
        "wall_clock_s": 0.0,
        "total_requests": 0,
        "notes": f"DRAMSim3 (DRAM layer only, no CXL link)",
    }
    return result

def parse_text_stats(stats_file):
    """Parse DRAMSim3 text output format."""
    result = {}
    with open(stats_file) as f:
        for line in f:
            line = line.strip()
            if '=' in line or ':' in line:
                sep = '=' if '=' in line else ':'
                key, _, val = line.partition(sep)
                key = key.strip().lower().replace(' ', '_')
                try:
                    val_f = float(val.strip().split()[0])
                    for our_key, candidates in STAT_KEYS.items():
                        if key in [c.lower() for c in candidates]:
                            result[our_key] = val_f
                except (ValueError, IndexError):
                    pass
    return result

def main():
    parser = argparse.ArgumentParser(description="DRAMSim3 output parser")
    parser.add_argument("--input", required=True, help="DRAMSim3 stats dir or file")
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
