#!/usr/bin/env python3
"""
Parse CXRay output → normalized result JSON.

Supports two output formats:
  Python engine: outputs summary.json with nested schema (latency_ns, bandwidth_GBps, ...).
  C++ engine:    outputs <tag>_stats.json with flat schema (mean_latency_ns, bandwidth_gbps, ...).

The parser detects the format automatically by checking which file exists.

Usage:
    python cxray_parser.py --input results/ --trace T1 --config cxl_2.0 --output /tmp/cxray_T1.json
"""

import argparse
import glob
import json
import os
import sys


def _parse_python_summary(raw):
    """Extract normalized fields from Python engine summary.json (nested schema)."""
    return {
        "latency_ns":          raw.get("latency_ns", {}),
        "bandwidth_GBps":      raw.get("bandwidth_GBps", {}),
        "link_utilization_avg": raw.get("link", {}).get("utilization_aggregate",
                                 raw.get("link_utilization", {}).get("upstream_avg", 0.0)),
        "row_buffer_hit_rate": raw.get("dram_model", {}).get("row_buffer_hit_rate",
                                raw.get("dram", {}).get("row_buffer_hit_rate", 0.0)),
        "wall_clock_s":        raw.get("wall_clock_s", 0.0),
        "total_requests":      raw.get("total_requests", 0),
        "notes":               f"CXRay-py v{raw.get('version','?')}",
    }


def _parse_cpp_stats(raw):
    """Extract normalized fields from C++ engine <tag>_stats.json (flat schema)."""
    mean_lat  = raw.get("mean_latency_ns", 0.0)
    p99_lat   = raw.get("p99_latency_ns",  0.0)
    p50_lat   = raw.get("p50_latency_ns",  0.0)
    reads     = raw.get("reads",  0)
    writes    = raw.get("writes", 0)
    total     = raw.get("total_requests", reads + writes)
    bw_total  = raw.get("bandwidth_gbps",  raw.get("effective_bw_gbps", 0.0))
    # C++ flat stats don't split read/write latency — per-op fields are null.
    return {
        "latency_ns": {
            "all_mean":   mean_lat,
            "read_mean":  None,
            "write_mean": None,
            "all_p50":    p50_lat,
            "all_p99":    p99_lat,
            "all_min":    raw.get("min_latency_ns", 0.0),
            "all_max":    raw.get("max_latency_ns", 0.0),
        },
        "bandwidth_GBps": {
            "total_avg": bw_total,
            "read_avg":  bw_total * reads / total if total > 0 else 0.0,
            "write_avg": bw_total * writes / total if total > 0 else 0.0,
        },
        "link_utilization_avg": raw.get("m2s_link_utilization_frac",
                                raw.get("link_utilization_frac", 0.0)),
        "row_buffer_hit_rate":  raw.get("row_hit_rate", 0.0),
        "wall_clock_s":         0.0,
        "total_requests":       total,
        "notes":                f"CXRay-cpp tag={raw.get('run_tag','?')}",
    }


def parse(input_dir, trace_id, config_id, tag=None):
    # Try Python engine output first.
    summary_path = os.path.join(input_dir, "summary.json")
    if os.path.exists(summary_path):
        with open(summary_path) as f:
            raw = json.load(f)
        fields = _parse_python_summary(raw)
    else:
        # Fall back to C++ engine output: find *_stats.json in the directory.
        if tag:
            # Exact file specified by tag (matches C++ run_tag convention).
            cpp_path = os.path.join(input_dir, f"{tag}_stats.json")
            if not os.path.exists(cpp_path):
                print(f"Error: {cpp_path} not found", file=sys.stderr)
                sys.exit(1)
            cpp_files = [cpp_path]
        else:
            cpp_files = glob.glob(os.path.join(input_dir, "*_stats.json"))

        if not cpp_files:
            print(f"Error: neither summary.json nor *_stats.json found in {input_dir}",
                  file=sys.stderr)
            sys.exit(1)

        if len(cpp_files) > 1 and not tag:
            # Ambiguous: multiple runs in same directory. Require --tag to disambiguate.
            names = [os.path.basename(p) for p in sorted(cpp_files)]
            print(f"Error: multiple stats files in {input_dir}: {names}. "
                  f"Use --tag to select one.", file=sys.stderr)
            sys.exit(1)

        with open(cpp_files[0]) as f:
            raw = json.load(f)
        fields = _parse_cpp_stats(raw)

    return {
        "simulator":  "cxray",
        "trace":      trace_id,
        "config":     config_id,
        **fields,
    }

def main():
    parser = argparse.ArgumentParser(description="CXRay output parser")
    parser.add_argument("--input", required=True, help="CXRay output directory")
    parser.add_argument("--trace", required=True, help="Trace ID (e.g. T1)")
    parser.add_argument("--config", required=True, help="Config ID (e.g. cxl_2.0)")
    parser.add_argument("--output", required=True, help="Output normalized JSON file")
    parser.add_argument("--tag", default=None,
                        help="C++ run_tag to select exact <tag>_stats.json "
                             "(required when multiple stats files exist in --input)")
    args = parser.parse_args()

    result = parse(args.input, args.trace, args.config, tag=args.tag)

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, 'w') as f:
        json.dump(result, f, indent=2)
    print(f"Normalized result → {args.output}")
    print(f"  read_mean_latency: {result['latency_ns'].get('read_mean', 'N/A')} ns")
    print(f"  read_avg_bw:       {result['bandwidth_GBps'].get('read_avg', 'N/A')} GB/s")

if __name__ == "__main__":
    main()
