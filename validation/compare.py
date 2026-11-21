#!/usr/bin/env python3
"""
Cross-simulator comparison script.

Collects normalized result JSONs from multiple simulators across traces,
compares them against golden data, and produces:
  - comparison_table.csv
  - error_vs_golden.csv
  - terminal report

Usage:
    python compare.py \\
        --results results_dir/ \\
        --golden golden/paper_data.json \\
        --output reports/

Results directory structure (populated by run scripts or manually):
    results_dir/
      cxray/
        T1_cxl_2.0.json
        T2_cxl_2.0.json
        ...
      xerxes/
        T1_cxl_2.0.json
        ...
      dramsim3/
        T1_cxl_2.0.json
        ...
      ramulator2/
        T1_cxl_2.0.json
        ...
"""

import argparse
import json
import os
import sys
import csv
import glob
from collections import defaultdict

SIMULATORS = ["cxray", "xerxes", "gem5_cxl", "cxlmemsim", "dramsim3", "ramulator2"]

METRICS = [
    ("latency_ns.read_mean",  "Read Mean Latency (ns)"),
    ("latency_ns.read_p99",   "Read P99 Latency (ns)"),
    ("bandwidth_GBps.read_avg", "Read Avg BW (GB/s)"),
    ("bandwidth_GBps.total_avg", "Total Avg BW (GB/s)"),
    ("row_buffer_hit_rate",   "Row Buffer Hit Rate"),
    ("wall_clock_s",          "Wall Clock (s)"),
]

# When the primary path returns None/0, try these fallback paths.
# C++ flat stats lack per-direction latency; fall back to all_mean/all_p99.
METRIC_FALLBACKS = {
    "latency_ns.read_mean":  "latency_ns.all_mean",
    "latency_ns.read_p99":   "latency_ns.all_p99",
    "bandwidth_GBps.read_peak": "bandwidth_GBps.read_avg",
}

def get_nested(d, path):
    """Get nested dict value by dot-separated path."""
    keys = path.split(".")
    cur = d
    for k in keys:
        if not isinstance(cur, dict) or k not in cur:
            return None
        cur = cur[k]
    return cur

def get_metric(d, path):
    """Get metric with automatic fallback for paths that may be null."""
    val = get_nested(d, path)
    if val is None and path in METRIC_FALLBACKS:
        val = get_nested(d, METRIC_FALLBACKS[path])
    return val

NORMALIZED_REQUIRED_KEYS = ("simulator", "trace", "config")

def load_results(results_dir):
    """Load normalized JSON results only. Returns dict[sim][trace_config] = result.

    Rejects raw simulator stats files that lack the normalized schema fields
    (simulator, trace, config) to prevent raw *_stats.json from polluting comparisons.
    """
    data = defaultdict(dict)
    for sim in SIMULATORS:
        sim_dir = os.path.join(results_dir, sim)
        if not os.path.isdir(sim_dir):
            continue
        for jf in glob.glob(os.path.join(sim_dir, "*.json")):
            name = os.path.splitext(os.path.basename(jf))[0]  # e.g. T1_cxl_2.0
            with open(jf) as f:
                try:
                    result = json.load(f)
                except json.JSONDecodeError as e:
                    print(f"Warning: could not parse {jf}: {e}")
                    continue
            if not all(k in result for k in NORMALIZED_REQUIRED_KEYS):
                print(f"Info: skipping {jf} (missing normalized schema fields)")
                continue
            data[sim][name] = result
    return data

def load_golden(golden_file):
    """Load golden data."""
    with open(golden_file) as f:
        return json.load(f)

def error_pct(simulated, golden):
    """Compute % error."""
    if golden == 0:
        return None
    return abs(simulated - golden) / golden * 100.0

def compare(data, golden_data, output_dir):
    os.makedirs(output_dir, exist_ok=True)

    # Collect all trace_config keys
    all_keys = set()
    for sim_data in data.values():
        all_keys.update(sim_data.keys())
    all_keys = sorted(all_keys)

    # ── comparison_table.csv ──────────────────────────────────────────────
    table_path = os.path.join(output_dir, "comparison_table.csv")
    with open(table_path, 'w', newline='') as f:
        writer = csv.writer(f)
        header = ["trace_config", "metric"] + SIMULATORS
        writer.writerow(header)

        for key in all_keys:
            for metric_path, metric_label in METRICS:
                row = [key, metric_label]
                for sim in SIMULATORS:
                    val = get_metric(data[sim].get(key, {}), metric_path)
                    row.append(f"{val:.3f}" if isinstance(val, float) else (str(val) if val is not None else ""))
                writer.writerow(row)

    print(f"Comparison table → {table_path}")

    # ── error_vs_golden.csv ───────────────────────────────────────────────
    golden_points = golden_data.get("data_points", [])
    validation_targets = golden_data.get("validation_targets", {})

    error_path = os.path.join(output_dir, "error_vs_golden.csv")
    with open(error_path, 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(["golden_id", "source", "metric", "golden_value",
                         "trace_key"] + SIMULATORS + ["notes"])

        # Map validation targets to golden data
        for trace_key, target in validation_targets.items():
            metric = target.get("metric", "")
            golden_ids = target.get("golden_ids", [])

            # Find metric path in normalized schema
            metric_map = {
                "read_mean_latency_ns": "latency_ns.read_mean",
                "peak_read_bandwidth_GBps": "bandwidth_GBps.read_peak",
                "bw_saturation_threads": None,  # not directly in schema
            }
            # Fallback paths tried when primary is 0 or None
            metric_fallback = {
                "bandwidth_GBps.read_peak": "bandwidth_GBps.read_avg",
            }
            metric_path = metric_map.get(metric)

            for gid in golden_ids:
                g = next((p for p in golden_points if p["id"] == gid), None)
                if not g:
                    continue

                golden_val = g.get("value") or (
                    (g.get("value_min", 0) + g.get("value_max", 0)) / 2
                    if "value_min" in g else None)

                row = [gid, g.get("source", ""), metric, golden_val, trace_key]

                # Find matching simulator results for this trace
                # trace_key format: T1_seq_read, match to T1_cxl_2.0 etc.
                trace_prefix = trace_key.split("_")[0]  # e.g. "T1"
                for sim in SIMULATORS:
                    best_val = None
                    for result_key, result in data[sim].items():
                        if result_key.startswith(trace_prefix):
                            if metric_path:
                                v = get_nested(result, metric_path)
                                # Fall back to alternate path if value is 0 or None
                                if (v is None or v == 0.0) and metric_path in metric_fallback:
                                    v2 = get_nested(result, metric_fallback[metric_path])
                                    if v2 is not None and v2 != 0.0:
                                        v = v2
                                if v is not None:
                                    best_val = v
                                    break
                    if best_val is not None and golden_val is not None:
                        err = error_pct(best_val, golden_val)
                        row.append(f"{best_val:.2f} ({err:.1f}%)" if err is not None else f"{best_val:.2f}")
                    else:
                        row.append("")

                row.append(g.get("notes", ""))
                writer.writerow(row)

    print(f"Error vs golden    → {error_path}")

    # ── terminal summary ──────────────────────────────────────────────────
    print("\n" + "="*70)
    print("SIMULATOR COMPARISON SUMMARY")
    print("="*70)

    for key in all_keys[:5]:  # show first 5 trace_configs
        print(f"\n  {key}:")
        for metric_path, metric_label in METRICS[:4]:
            vals = {}
            for sim in SIMULATORS:
                v = get_metric(data[sim].get(key, {}), metric_path)
                if v is not None:
                    vals[sim] = v
            if vals:
                val_str = "  ".join(f"{s}={v:.2f}" for s, v in vals.items())
                print(f"    {metric_label:<30} {val_str}")

    print("\n" + "="*70)
    print("GOLDEN DATA COMPARISON (CXRay vs hardware)")
    print("="*70)

    cxl_20_golden = {
        "T2 pointer-chase latency (ns)": (170, 250),
        "T1 read BW fraction of DDR":    (0.82, 0.83),
        "BW saturation threads":         (4, 4),
    }
    for desc, (lo, hi) in cxl_20_golden.items():
        print(f"  Golden: {desc} = [{lo}, {hi}]")

    return True

def main():
    parser = argparse.ArgumentParser(description="Cross-simulator comparison")
    parser.add_argument("--results", default="results",
                        help="Directory with per-simulator normalized JSON results")
    parser.add_argument("--golden", default="golden/paper_data.json",
                        help="Golden data JSON file")
    parser.add_argument("--output", default="reports",
                        help="Output directory for comparison reports")
    args = parser.parse_args()

    golden_path = args.golden
    if not os.path.isabs(golden_path):
        golden_path = os.path.join(os.path.dirname(__file__), golden_path)

    print(f"Loading results from: {args.results}")
    data = load_results(args.results)

    n_loaded = sum(len(v) for v in data.values())
    if n_loaded == 0:
        print("ERROR: No results found. Run simulators first and place normalized JSONs in:")
        print(f"  {args.results}/<simulator>/<trace>_<config>.json")
        print("Example:")
        print(f"  {args.results}/cxray/T1_cxl_2.0.json")
        sys.exit(1)  # nonzero so callers detect empty comparison pipeline

    print(f"Loaded {n_loaded} result(s) across {len(data)} simulator(s)")

    golden = {"data_points": [], "validation_targets": {}}
    if os.path.exists(golden_path):
        golden = load_golden(golden_path)
        print(f"Golden data: {len(golden.get('data_points', []))} data points")
    else:
        print(f"Warning: golden data not found at {golden_path}")

    compare(data, golden, args.output)

if __name__ == "__main__":
    main()
