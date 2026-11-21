#!/usr/bin/env python3
"""
run_feature_experiments.py
Run CXRay experiments for all four new features and write CSV summary tables
for inclusion in the paper.

Experiments:
  E1. PCIe credit-pool back-pressure sweep (M2S_REQ limit: disabled, 8, 16, 32, 64)
  E2. CXL switch topology sweep (0, 1, 2, 3 hops at 40 ns/hop)
  E3. GIM device-count sweep (1, 2, 4 devices)
  E4. CXL.cache Type 2 vs Type 3 (read-heavy T2 trace, mixed T6 trace)

Outputs (in paper/figures/):
  credit_pool_sweep.csv
  switch_sweep.csv
  gim_sweep.csv
  type2_vs_type3.csv
"""

import subprocess, json, os, sys, csv, shutil, tempfile

REPO    = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SIM     = os.path.join(REPO, "src", "cxray.py")
CFGDIR  = os.path.join(REPO, "configs")
TRACES  = os.path.join(REPO, "validation", "traces")
OUT_FIG = os.path.join(REPO, "paper", "figures")
TMPDIR  = tempfile.mkdtemp(prefix="cxray_exp_")

os.makedirs(OUT_FIG, exist_ok=True)


def run_sim(config, trace, overrides=None, label=""):
    """Run one CXRay simulation, return summary dict."""
    outdir = os.path.join(TMPDIR, label.replace(" ", "_").replace("/", "_"))
    os.makedirs(outdir, exist_ok=True)
    cmd = [sys.executable, SIM, "-c", config, "-t", trace, "-o", outdir]
    if overrides:
        for k, v in overrides.items():
            cmd += ["--set", f"{k}={v}"]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"[ERROR] {label}: {result.stderr[-400:]}")
        return None
    summary_path = os.path.join(outdir, "summary.json")
    if not os.path.exists(summary_path):
        print(f"[ERROR] {label}: no summary.json")
        return None
    with open(summary_path) as f:
        return json.load(f)


def write_csv(path, rows, fieldnames):
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames, lineterminator="\n")
        w.writeheader()
        w.writerows(rows)
    print(f"  → {path}")


# ============================================================================
# E1: Credit-pool back-pressure sweep
# ============================================================================

def _gen_e1_seed(path, nreq=32):
    """Seed trace for closed-loop E1 experiment: one entry per requester.

    Closed-loop injection avoids open-system queue divergence.  At steady state
    with credit limit L and nreq requesters the mean latency stabilises at
    nreq × RTT / L, giving sub-µs to µs-scale stalls for realistic limits.
    stride=64 B spreads requests across all 32 DRAM banks so DRAM is not the
    bottleneck; the credit pool is the sole serialisation point.
    """
    CACHELINE = 64
    with open(path, "w") as f:
        f.write(f"# E1 seed: {nreq} requesters (closed-loop)\n")
        for rid in range(nreq):
            addr = rid * CACHELINE
            f.write(f"{rid} R 0x{addr:016x} {CACHELINE} {rid}\n")


def exp_credit_pool():
    print("\n[E1] Credit-pool sweep (M2S_REQ limit, closed-loop)")
    cfg   = os.path.join(CFGDIR, "cxl_3.1.cfg")

    # Closed-loop: 32 requesters × 1000 requests each.
    # Each requester requeues immediately after completion → no queue divergence.
    # With limit L, steady-state mean lat ≈ 32 × RTT / L (Little's law):
    #   disabled/32 → baseline  ~246 ns (current model; was ~295 ns pre-flit)
    #   limit=16    → ~590 ns  (2×)
    #   limit=8     → ~1180 ns (4×, ~1.2 µs)
    #   limit=4     → ~2360 ns (8×, ~2.4 µs)
    seed = os.path.join(TMPDIR, "e1_seed.trace")
    _gen_e1_seed(seed, nreq=32)

    rows  = []
    for limit in [0, 32, 16, 8, 4]:
        label     = f"credit_req{limit}"
        overrides = {
            "dram.backend":             "queued",
            "link.credits.m2s_req":     limit,
            "inject.closed_loop_per_req": 1000,
            "inject.stride_bytes":      64,
            "inject.region_bytes":      16777216,   # 16 MB
        }
        s = run_sim(cfg, seed, overrides=overrides, label=label)
        if s is None:
            continue
        lat = s["latency_ns"]
        cp  = s["credit_pools"]
        rows.append({
            "m2s_req_limit":       limit if limit > 0 else "disabled",
            "mean_lat_ns":         lat["all_mean"],
            "p99_lat_ns":          lat["all_p99"],
            "credit_stall_events": cp["req_pool"]["stall_events"],
            "total_stall_ns":      cp["total_stall_ns"],
            "stall_frac":          cp["stall_fraction"],
            "bw_GBps":             s["bandwidth_GBps"]["total_avg"],
        })
        print(f"  limit={limit if limit>0 else 'disabled':>8}  "
              f"mean={lat['all_mean']:12.1f} ns  "
              f"p99={lat['all_p99']:12.1f} ns  "
              f"stall_events={cp['req_pool']['stall_events']:8}")

    write_csv(os.path.join(OUT_FIG, "credit_pool_sweep.csv"), rows,
              ["m2s_req_limit","mean_lat_ns","p99_lat_ns",
               "credit_stall_events","total_stall_ns","stall_frac","bw_GBps"])
    return rows


# ============================================================================
# E2: CXL switch topology sweep (hop count)
# ============================================================================

def exp_switch():
    print("\n[E2] Switch topology sweep (hop count)")
    trace = os.path.join(TRACES, "T2_ptr_chase.trace")
    rows  = []

    for hops in [0, 1, 2, 3]:
        label = f"switch_{hops}hop"
        if hops == 0:
            cfg = os.path.join(CFGDIR, "cxl_3.1.cfg")
            overrides = {}
        else:
            cfg = os.path.join(CFGDIR, "cxl_3.1.cfg")
            overrides = {
                "has_switch":           True,
                "switch.num_hops":      hops,
                "switch.latency_ns":    40,
            }
        s = run_sim(cfg, trace, overrides=overrides, label=label)
        if s is None:
            continue
        lat   = s["latency_ns"]
        ovhd  = s["overhead_ns"]
        sw_ns = ovhd["switch_fwd"] + ovhd["switch_ret"]
        rows.append({
            "num_hops":            hops,
            "switch_overhead_ns":  round(sw_ns, 1),
            "fwd_fixed_ns":        ovhd["fwd_fixed"],
            "mean_lat_ns":         lat["all_mean"],
            "p99_lat_ns":          lat["all_p99"],
            "overhead_vs_direct_ns": round(lat["all_mean"] - rows[0]["mean_lat_ns"], 1)
                                      if rows else 0.0,
        })
        print(f"  hops={hops}  sw_overhead={sw_ns:.0f} ns  "
              f"mean={lat['all_mean']:.1f} ns")

    if len(rows) > 1:
        base = rows[0]["mean_lat_ns"]
        for r in rows:
            r["overhead_vs_direct_ns"] = round(r["mean_lat_ns"] - base, 1)

    write_csv(os.path.join(OUT_FIG, "switch_sweep.csv"), rows,
              ["num_hops","switch_overhead_ns","fwd_fixed_ns",
               "mean_lat_ns","p99_lat_ns","overhead_vs_direct_ns"])
    return rows


# ============================================================================
# E3: GIM device-count sweep
# ============================================================================

def _gen_e3_seed(path, nreq, label):
    """Seed trace for closed-loop GIM experiments: one entry per requester."""
    if os.path.exists(path):
        return
    with open(path, "w") as f:
        f.write(f"# E3 seed: {nreq} requesters, base=rid*64\n")
        for rid in range(nreq):
            f.write(f"0 R 0x{rid*64:016x} 64 {rid}\n")


def exp_gim():
    print("\n[E3] GIM device-count sweep (two scenarios)")
    cfgs = {
        1: os.path.join(CFGDIR, "cxl_3.1.cfg"),
        2: os.path.join(CFGDIR, "cxl_3.1_gim2.cfg"),
        4: os.path.join(CFGDIR, "cxl_3.1_gim4.cfg"),
    }
    rows = []

    # E3A: overhead isolation — 32 req, closed-loop, bank-isolated, credits unconstrained
    seed_a = os.path.join(TMPDIR, "e3a_seed.trace")
    _gen_e3_seed(seed_a, nreq=32, label="E3A")
    print("  [E3A] overhead isolation (32 req, credit=0, bank-isolated, stride=2048B)")
    for num_dev, cfg in cfgs.items():
        label = f"e3a_gim{num_dev}"
        overrides = {
            "dram.backend": "queued",
            "link.credits.m2s_req": 0,
            "inject.closed_loop_per_req": 1000,
            "inject.stride_bytes": 2048,
            "inject.region_bytes": 67108864,
        }
        s = run_sim(cfg, seed_a, overrides=overrides, label=label)
        if s is None:
            continue
        lat  = s["latency_ns"]
        ovhd = s["overhead_ns"]
        rows.append({
            "scenario": "E3A_overhead",
            "num_devices": num_dev,
            "gim_arb_ns": ovhd["hdm_interleave_arbitration_ns"],
            "mean_lat_ns": lat["all_mean"],
            "p99_lat_ns": lat["all_p99"],
            "total_bw_GBps": s["bandwidth_GBps"]["total_avg"],
            "credit_stalls": s["credit_pools"]["req_pool"]["stall_events"],
        })
        print(f"    devices={num_dev}  gim_arb={ovhd['hdm_interleave_arbitration_ns']:.0f} ns  "
              f"mean={lat['all_mean']:.1f} ns")

    # E3B: credit saturation — 128 req, credit=8, GIM device count irrelevant
    seed_b = os.path.join(TMPDIR, "e3b_seed.trace")
    _gen_e3_seed(seed_b, nreq=128, label="E3B")
    print("  [E3B] credit saturation (128 req, credit=8)")
    for num_dev, cfg in cfgs.items():
        label = f"e3b_gim{num_dev}"
        overrides = {
            "dram.backend": "queued",
            "link.credits.m2s_req": 8,
            "inject.closed_loop_per_req": 2000,
            "inject.stride_bytes": 64,
            "inject.region_bytes": 16777216,
        }
        s = run_sim(cfg, seed_b, overrides=overrides, label=label)
        if s is None:
            continue
        lat  = s["latency_ns"]
        ovhd = s["overhead_ns"]
        rows.append({
            "scenario": "E3B_credit_sat",
            "num_devices": num_dev,
            "gim_arb_ns": ovhd["hdm_interleave_arbitration_ns"],
            "mean_lat_ns": lat["all_mean"],
            "p99_lat_ns": lat["all_p99"],
            "total_bw_GBps": s["bandwidth_GBps"]["total_avg"],
            "credit_stalls": s["credit_pools"]["req_pool"]["stall_events"],
        })
        print(f"    devices={num_dev}  gim_arb={ovhd['hdm_interleave_arbitration_ns']:.0f} ns  "
              f"mean={lat['all_mean']:.1f} ns  stalls={s['credit_pools']['req_pool']['stall_events']}")

    write_csv(os.path.join(OUT_FIG, "gim_sweep.csv"), rows,
              ["scenario","num_devices","gim_arb_ns","mean_lat_ns","p99_lat_ns",
               "total_bw_GBps","credit_stalls"])
    return rows


# ============================================================================
# E4: Type 2 vs Type 3
# ============================================================================

def exp_type2():
    print("\n[E4] Type 2 vs Type 3 comparison")
    cfgs = {
        "Type3": os.path.join(CFGDIR, "cxl_3.1.cfg"),
        "Type2": os.path.join(CFGDIR, "cxl_3.1_type2.cfg"),
    }
    traces = {
        "T2_ptr_chase": os.path.join(TRACES, "T2_ptr_chase.trace"),
        "T6_mixed_rw":  os.path.join(TRACES, "T6_mixed_rw.trace"),
    }
    rows = []

    for trace_name, trace_path in traces.items():
        for dev_type, cfg in cfgs.items():
            label = f"{dev_type}_{trace_name}"
            s = run_sim(cfg, trace_path, label=label)
            if s is None:
                continue
            lat  = s["latency_ns"]
            ovhd = s["overhead_ns"]
            rows.append({
                "device_type":    dev_type,
                "trace":          trace_name,
                "mean_lat_ns":    lat["all_mean"],
                "read_lat_ns":    lat["read_mean"],
                "write_lat_ns":   lat["write_mean"],
                "dcoh_overhead_ns": ovhd["dcoh"],
                "bi_rtt_ns":        ovhd["bi_rtt"],
                "p99_lat_ns":     lat["all_p99"],
            })
            print(f"  {dev_type:5s} {trace_name:15s}  "
                  f"mean={lat['all_mean']:7.1f} ns  "
                  f"read={lat['read_mean']:7.1f}  write={lat['write_mean']:7.1f}")

    write_csv(os.path.join(OUT_FIG, "type2_vs_type3.csv"), rows,
              ["device_type","trace","mean_lat_ns","read_lat_ns","write_lat_ns",
               "dcoh_overhead_ns","bi_rtt_ns","p99_lat_ns"])
    return rows


# ============================================================================
# Main
# ============================================================================

def main():
    print(f"CXRay feature experiments")
    print(f"  repo   : {REPO}")
    print(f"  tmpdir : {TMPDIR}")
    print(f"  output : {OUT_FIG}")

    results = {}
    results["E1_credit"] = exp_credit_pool()
    results["E2_switch"] = exp_switch()
    results["E3_gim"]    = exp_gim()
    results["E4_type2"]  = exp_type2()

    # Write combined JSON for easy ingestion
    with open(os.path.join(OUT_FIG, "feature_experiments.json"), "w") as f:
        json.dump(results, f, indent=2)
    print(f"\n[done] All results in {OUT_FIG}/")

    # Print summary for macros.tex
    print("\n--- Paper macro candidates ---")
    if results["E1_credit"]:
        r_dis = next((r for r in results["E1_credit"]
                      if str(r["m2s_req_limit"]) == "disabled"), None)
        r_32  = next((r for r in results["E1_credit"]
                      if str(r["m2s_req_limit"]) == "32"), None)
        r_8   = next((r for r in results["E1_credit"]
                      if str(r["m2s_req_limit"]) == "8"), None)
        if r_dis and r_32:
            print(f"\\StatCreditBaselineMeanLat   {r_dis['mean_lat_ns']:.1f}")
            print(f"\\StatCreditLimitThirtyTwoMeanLat {r_32['mean_lat_ns']:.1f}")
        if r_8:
            print(f"\\StatCreditLimitEightMeanLat {r_8['mean_lat_ns']:.1f}")
            print(f"\\StatCreditLimitEightStallFrac {r_8['stall_frac']:.4f}")

    if results["E2_switch"]:
        r0 = next((r for r in results["E2_switch"] if r["num_hops"]==0), None)
        r1 = next((r for r in results["E2_switch"] if r["num_hops"]==1), None)
        r2 = next((r for r in results["E2_switch"] if r["num_hops"]==2), None)
        if r0: print(f"\\StatSwitchZeroHopMeanLat  {r0['mean_lat_ns']:.1f}")
        if r1: print(f"\\StatSwitchOneHopMeanLat   {r1['mean_lat_ns']:.1f}  "
                     f"(+{r1['overhead_vs_direct_ns']:.1f} ns)")
        if r2: print(f"\\StatSwitchTwoHopMeanLat   {r2['mean_lat_ns']:.1f}  "
                     f"(+{r2['overhead_vs_direct_ns']:.1f} ns)")

    if results["E3_gim"]:
        for r in results["E3_gim"]:
            print(f"\\StatGimDevs{r['num_devices']}MeanLat    {r['mean_lat_ns']:.1f}  "
                  f"bw={r['total_bw_GBps']:.3f}")

    if results["E4_type2"]:
        for r in results["E4_type2"]:
            tag = f"{r['device_type']}_{r['trace']}"
            print(f"\\Stat{tag.replace('-','').replace('_','')}MeanLat  "
                  f"{r['mean_lat_ns']:.1f}  "
                  f"write={r['write_lat_ns']:.1f}")

    shutil.rmtree(TMPDIR, ignore_errors=True)


if __name__ == "__main__":
    main()
