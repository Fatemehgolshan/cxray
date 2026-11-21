#!/usr/bin/env python3
"""
zsim Paper Experiment Configuration

Generates all simulation data needed for the paper on CXL-aware page migration.
This file is self-contained and produces a complete dataset for all paper figures.

=== Paper Narrative: Four Observation→Method Chains ===

§3.1 / Fig [contamination]: Stall Cycles are Contaminated by CXL Link Congestion
  - Observation: During CXL link congestion ALL accesses (hot and cold) see elevated
    latency. A cycles-based migrator labels cold pages "hot" during congestion and
    wastes migration budget on pages that revert to cold immediately after migration.
  - Evidence: fsc+sbs regresses to 0.28x IPC on mobilenetv2, 0.42x on vgg16 vs 'none'.
  - Method: Use stall event COUNT (fsc_cnt) instead of stall CYCLES (fsc).

§3.2 / Fig [bimodal]: CXL Latency has a Bimodal Distribution
  - Observation: CXL stall latency is bimodal (base link: ~300 cy vs congested: >1000 cy).
    A page with HIGH stall variance is a "congestion victim" — it is slow because the
    link is busy, not because it is inherently cold. Migrating it wastes a DDR slot.
  - Method: Stall Variance Aware (SVA) — track EMA of stall^2; suppress pages with
    high coefficient of variation (CV = std/mean). Ablation: fsc_cnt+sva+sbs.

§3.3 / Fig [fanout]: CXL Link is a Single Serialized Path Shared by All Cores
  - Observation: Unlike DRAM with independent per-channel queues, all cores share one
    CXL link. A page stalling N cores causes N×pipeline damage AND head-of-line
    blocking in the CXL request queue — disproportionately expensive.
  - Method: Cross-Core Fan-out (CCF) — per-page bitmask of stalling cores; boost
    migration score by popcount(mask). Ablation: fsc_cnt+sva+ccf+sbs vs fsc_cnt+sva+sbs.

§3.4 / Fig [overhead]: Migration Bandwidth Competes on the Same CXL Link
  - Observation: Each promoted page reads 4 KB from CXL over the shared link, consuming
    link bandwidth. Unlike DRAM migration (separate DDR channel), CXL migration directly
    competes with demand traffic and must be budgeted explicitly.
  - Method: Model migration overhead (migrationCostCyclesPerPage) + bandwidth cap.

§5 Discussion: CXL Link has a Bandwidth Knee (GKD/SCA)
  - Observation: Past ~70% link utilization, latency rises super-linearly. Aggressive
    migration near the knee can quickly relieve congestion, but self-defeating feedback
    occurs at very high congestion where migration competes with demand.
  - Evidence: fsc_cnt+gkd+sca = +1.06% geomean vs +6.45% for the proposed method.
    Reveals high workload sensitivity — a direction for future adaptive policies.

=== Proposed Method ===

  fsc_cnt+sva+ccf+sbs: +6.45% geomean IPC over no-migration baseline
    = frequency-stall COUNT composite (robust against congestion false positives)
    + stall variance awareness (SVA: deprioritize congestion victims)
    + cross-core fan-out weighting (CCF: prioritize pages that hurt many cores)
    + stability bonus (SBS: hysteresis to prevent over-migration churn)

=== Sweeps ===

  Sweep A — Main policy comparison (all 4 configs × 4 GB DRAM)
    Policies: baselines + fsc contamination demo + ablation chain + proposed + discussion
    Job count: 25 workloads × 10 policies × 4 configs = 1000 jobs

  Sweep B — DRAM capacity sensitivity (MessDdrCxl_WeakSramCaches × 2/4/8 GB)
    Policies: none, freqBased, fsc_cnt, fsc_cnt+sva+ccf+sbs
    Job count: 25 × 4 policies × 3 DRAM sizes × 1 config = 300 jobs

  Sweep C — fscAlpha sensitivity (all 4 configs × 4 GB)
    Policy: fsc_cnt+sva+ccf+sbs at alpha ∈ {0.0001, 0.001, 0.005, 0.01}
    Job count: 25 × 4 alphas × 4 configs = 400 jobs

  Sweep D — CXL characteristic model comparison (all 4 configs × 4 GB)
    Quantifies individual impact of Model A (BW congestion) and Model B (HoL blocking)
    Policies: none, fsc_cnt+sva+ccf+sbs × 4 model configurations (off/A/B/both)
    Job count: 25 × 2 policies × 4 model configs × 4 configs = 800 jobs

  Total: ~2500 jobs
"""

import numpy as np
from datetime import datetime

# Base configuration files — all 4 to show generalizability across memory models
# Primary results come from Tiering_MessDdrCxl_WeakSramCaches (WeaveMessMemCtrl).
BASE_CONFIG = [
    "../tests/Tiering_MessDdrCxl_WeakSramCaches.cfg",   # PRIMARY: realistic BW curves, weak LLC
    "../tests/Tiering_MessDdrCxl_AggrSramCaches.cfg",   # cache sensitivity: aggressive L3
    "../tests/Tiering_ZddrCxl_WeakSramCaches.cfg",      # model sensitivity: simple DDR timing
    "../tests/Tiering_ZddrCxl_AggrSramCaches.cfg",      # both: aggressive LLC + simple DDR
]

OUTPUT_DIR = f'results/results_MigrationPaper_{datetime.now().strftime("%Y%m%d_%H%M%S")}'

WARMUP_PHASE = 0.2

# =============================================================================
# Metric Calculators
# =============================================================================

_STANDALONE_MIGRATOR_PATTERNS = [
    'PageMigrator', 'Pagemigrator', 'FreqBased', 'RobStallBased',
    'RobPressureWeighted', 'CriticalityWeighted', 'QLearning',
]
_HYBRID_METHOD_NAMES_NORM = [
    n.replace("+", "").replace("_", "") for n in [
        "fsc", "sys", "sse", "spfd", "cema", "clcas", "sbs",
        "fsc+cema", "fsc+sbs", "fsc+cema+sbs",
        "sse+spfd", "sse+spfd+clcas", "sys+cema",
        "fsc_cnt", "fsc_cnt+sbs", "fsc_cnt+cema+sbs", "sse+spfd+cnt",
        "sva", "gkd", "sca", "ccf",
        "fsc_cnt+sva", "fsc_cnt+ccf", "fsc_cnt+sva+ccf",
        "fsc_cnt+sva+sbs", "fsc_cnt+sva+ccf+sbs",
        "fsc_cnt+gkd+sca", "fsc_cnt+sva+ccf+gkd+sca+sbs",
    ]
]

def _find_any_migrator(mem_data):
    """Return the migrator structured-array from mem stats, or None."""
    for field_name in mem_data.dtype.names:
        for pat in _STANDALONE_MIGRATOR_PATTERNS:
            if pat in field_name:
                return mem_data[field_name]
        norm = field_name.lower().replace("+", "").replace("_", "")
        for h in _HYBRID_METHOD_NAMES_NORM:
            if h in norm:
                return mem_data[field_name]
    return None

def calculate_fastMemHitRatio(stats, warmup_idx):
    mem = stats['mem']
    total_accesses = np.sum(mem['accesses'][-1]) - np.sum(mem['accesses'][warmup_idx])
    total_ddr = np.sum(mem['ddrAccesses'][-1]) - np.sum(mem['ddrAccesses'][warmup_idx])
    return total_ddr / total_accesses if total_accesses > 0 else 0.0

def calculate_migrationRate(stats, warmup_idx):
    try:
        mem_data = stats['mem']
        migrator_data = _find_any_migrator(mem_data)
        if migrator_data is None or 'numPageMigrations' not in migrator_data.dtype.names:
            return 0.0
        total_migrations = (np.sum(migrator_data['numPageMigrations'][-1])
                            - np.sum(migrator_data['numPageMigrations'][warmup_idx]))
        if 'numAccesses' in migrator_data.dtype.names:
            acc = migrator_data['numAccesses']
        elif 'pageTrackerAccesses' in migrator_data.dtype.names:
            acc = migrator_data['pageTrackerAccesses']
        else:
            acc = mem_data['accesses']
        total_accesses = np.sum(acc[-1]) - np.sum(acc[warmup_idx])
        return (total_migrations / total_accesses) * 1e6 if total_accesses > 0 else 0.0
    except Exception:
        return 0.0

def calculate_robStallCycles(stats, warmup_idx):
    try:
        mem = stats['mem']
        return (np.sum(mem['robHeadStallCycles'][-1])
                - np.sum(mem['robHeadStallCycles'][warmup_idx]))
    except Exception:
        return 0.0

def calculate_avgDdrLatency(stats, warmup_idx):
    try:
        mem = stats['mem']
        total_accesses = (np.sum(mem['ddrAccesses'][-1])
                          - np.sum(mem['ddrAccesses'][warmup_idx]))
        total_latency = (np.sum(mem['ddrLatency'][-1])
                         - np.sum(mem['ddrLatency'][warmup_idx]))
        return total_latency / total_accesses if total_accesses > 0 else 0.0
    except Exception:
        return 0.0

def calculate_avgCxlLatency(stats, warmup_idx):
    try:
        mem = stats['mem']
        total_accesses = (np.sum(mem['cxlAccesses'][-1])
                          - np.sum(mem['cxlAccesses'][warmup_idx]))
        total_latency = (np.sum(mem['cxlLatency'][-1])
                         - np.sum(mem['cxlLatency'][warmup_idx]))
        return total_latency / total_accesses if total_accesses > 0 else 0.0
    except Exception:
        return 0.0

def _bw_gbps(accesses, warmup_idx, stats):
    """Helper: bandwidth in GB/s given access count array."""
    cores = stats['c']
    total_cycles = np.max(cores['cycles'][-1]) - np.max(cores['cycles'][warmup_idx])
    total_bytes = (np.sum(accesses[-1]) - np.sum(accesses[warmup_idx])) * 64
    time_s = total_cycles / 2e9
    return total_bytes / time_s / 1e9 if time_s > 0 else 0.0

def calculate_ddrBandwidthGBps(stats, warmup_idx):
    try:
        return _bw_gbps(stats['mem']['ddrAccesses'], warmup_idx, stats)
    except Exception:
        return 0.0

def calculate_cxlBandwidthGBps(stats, warmup_idx):
    try:
        return _bw_gbps(stats['mem']['cxlAccesses'], warmup_idx, stats)
    except Exception:
        return 0.0

def calculate_totalMemBandwidthGBps(stats, warmup_idx):
    return calculate_ddrBandwidthGBps(stats, warmup_idx) + calculate_cxlBandwidthGBps(stats, warmup_idx)

def calculate_ddrAccessCount(stats, warmup_idx):
    try:
        mem = stats['mem']
        return (np.sum(mem['ddrAccesses'][-1]) - np.sum(mem['ddrAccesses'][warmup_idx])) / 1e6
    except Exception:
        return 0.0

def calculate_cxlAccessCount(stats, warmup_idx):
    try:
        mem = stats['mem']
        return (np.sum(mem['cxlAccesses'][-1]) - np.sum(mem['cxlAccesses'][warmup_idx])) / 1e6
    except Exception:
        return 0.0

def calculate_cxlAccessRatio(stats, warmup_idx):
    try:
        mem = stats['mem']
        ddr = np.sum(mem['ddrAccesses'][-1]) - np.sum(mem['ddrAccesses'][warmup_idx])
        cxl = np.sum(mem['cxlAccesses'][-1]) - np.sum(mem['cxlAccesses'][warmup_idx])
        total = ddr + cxl
        return cxl / total if total > 0 else 0.0
    except Exception:
        return 0.0

def calculate_migrationCount(stats, warmup_idx):
    try:
        migrator_data = _find_any_migrator(stats['mem'])
        if migrator_data is None or 'numPageMigrations' not in migrator_data.dtype.names:
            return 0.0
        return (np.sum(migrator_data['numPageMigrations'][-1])
                - np.sum(migrator_data['numPageMigrations'][warmup_idx]))
    except Exception:
        return 0.0

def calculate_migrationBandwidthGBps(stats, warmup_idx):
    try:
        migrator_data = _find_any_migrator(stats['mem'])
        if migrator_data is None or 'numPageMigrations' not in migrator_data.dtype.names:
            return 0.0
        total_migrations = (np.sum(migrator_data['numPageMigrations'][-1])
                            - np.sum(migrator_data['numPageMigrations'][warmup_idx]))
        cores = stats['c']
        total_cycles = np.max(cores['cycles'][-1]) - np.max(cores['cycles'][warmup_idx])
        time_s = total_cycles / 2e9
        return (total_migrations * 4096) / time_s / 1e9 if time_s > 0 else 0.0
    except Exception:
        return 0.0

def calculate_robStallCyclesPerKInstr(stats, warmup_idx):
    try:
        mem = stats['mem']
        cores = stats['c']
        stalls = (np.sum(mem['robHeadStallCycles'][-1])
                  - np.sum(mem['robHeadStallCycles'][warmup_idx]))
        instrs = (np.sum(cores['instrs'][-1])
                  - np.sum(cores['instrs'][warmup_idx]))
        return (stalls / instrs) * 1000 if instrs > 0 else 0.0
    except Exception:
        return 0.0

def calculate_memStallRatio(stats, warmup_idx):
    try:
        mem = stats['mem']
        cores = stats['c']
        stalls = (np.sum(mem['robHeadStallCycles'][-1])
                  - np.sum(mem['robHeadStallCycles'][warmup_idx]))
        cycles = (np.max(cores['cycles'][-1])
                  - np.max(cores['cycles'][warmup_idx]))
        return stalls / cycles if cycles > 0 else 0.0
    except Exception:
        return 0.0

def calculate_l3MissRate(stats, warmup_idx):
    try:
        if 'l3' not in stats.dtype.names:
            return 0.0
        l3 = stats['l3']
        hits = np.sum(l3['hGETS'][-1]) - np.sum(l3['hGETS'][warmup_idx])
        misses = np.sum(l3['mGETS'][-1]) - np.sum(l3['mGETS'][warmup_idx])
        total = hits + misses
        return misses / total if total > 0 else 0.0
    except Exception:
        return 0.0

def calculate_llscMPKI(stats, warmup_idx):
    """Last Level SRAM Cache (L3) Misses Per Kilo Instructions."""
    try:
        l3 = stats['l3']
        misses = (np.sum(l3['mGETS'][-1])    + np.sum(l3['mGETXIM'][-1])  + np.sum(l3['mGETXSM'][-1])
                - np.sum(l3['mGETS'][warmup_idx]) - np.sum(l3['mGETXIM'][warmup_idx]) - np.sum(l3['mGETXSM'][warmup_idx]))
        instrs = np.sum(stats['c']['instrs'][-1]) - np.sum(stats['c']['instrs'][warmup_idx])
        if instrs <= 0:
            return 0.0
        return misses / instrs * 1000.0
    except Exception:
        return 0.0

def calculate_memoryFootprintMB(stats, warmup_idx):
    """Memory footprint: unique page addresses ever seen in TieringMemory × 4KB, in MB."""
    try:
        # uniquePages is a cumulative high-watermark; take the final value (not a delta)
        unique = np.sum(stats['mem']['uniquePages'][-1])
        return unique * 4096.0 / 1024.0 / 1024.0
    except Exception:
        return 0.0

def calculate_freqBasedHotPages(stats, warmup_idx):
    try:
        for field_name in stats['mem'].dtype.names:
            if 'FreqBased' in field_name:
                mig = stats['mem'][field_name]
                if 'numHotPages' in mig.dtype.names:
                    return float(np.sum(mig['numHotPages'][-1]))
        return 0.0
    except Exception:
        return 0.0

def calculate_robStallPagesTracked(stats, warmup_idx):
    try:
        for field_name in stats['mem'].dtype.names:
            if 'RobStallBased' in field_name:
                mig = stats['mem'][field_name]
                if 'numStallPages' in mig.dtype.names:
                    return float(np.sum(mig['numStallPages'][-1]))
        return 0.0
    except Exception:
        return 0.0

# ---------------------------------------------------------------------------
# Hybrid-method specific metrics
# ---------------------------------------------------------------------------

def _find_hybrid_migrator(mem_data):
    HYBRID_NAMES = [
        "fsc", "sys", "sse", "spfd", "cema", "clcas", "sbs",
        "fsc+cema", "fsc+sbs", "fsc+cema+sbs",
        "sse+spfd", "sse+spfd+clcas", "sys+cema",
        "fsc_cnt", "fsc_cnt+sbs", "fsc_cnt+cema+sbs", "sse+spfd+cnt",
        "sva", "gkd", "sca", "ccf",
        "fsc_cnt+sva", "fsc_cnt+ccf", "fsc_cnt+sva+ccf",
        "fsc_cnt+sva+sbs", "fsc_cnt+sva+ccf+sbs",
        "fsc_cnt+gkd+sca", "fsc_cnt+sva+ccf+gkd+sca+sbs",
    ]
    for field_name in mem_data.dtype.names:
        lower = field_name.lower()
        for h in HYBRID_NAMES:
            if h.replace("+", "").replace("_", "") in lower.replace("+", "").replace("_", ""):
                return mem_data[field_name]
    return None

def calculate_hybridAvgEmaStall(stats, warmup_idx):
    try:
        mig = _find_hybrid_migrator(stats['mem'])
        if mig is None or 'avgEmaStall' not in mig.dtype.names:
            return 0.0
        triggers = (np.sum(mig['numMigrationTriggers'][-1])
                    - np.sum(mig['numMigrationTriggers'][warmup_idx]))
        val = np.sum(mig['avgEmaStall'][-1]) - np.sum(mig['avgEmaStall'][warmup_idx])
        return val / triggers if triggers > 0 else 0.0
    except Exception:
        return 0.0

def calculate_hybridAvgStabilityBonus(stats, warmup_idx):
    try:
        mig = _find_hybrid_migrator(stats['mem'])
        if mig is None or 'avgStabilityBonus' not in mig.dtype.names:
            return 0.0
        triggers = (np.sum(mig['numMigrationTriggers'][-1])
                    - np.sum(mig['numMigrationTriggers'][warmup_idx]))
        val = np.sum(mig['avgStabilityBonus'][-1]) - np.sum(mig['avgStabilityBonus'][warmup_idx])
        return (val / triggers) / 1000.0 if triggers > 0 else 0.0
    except Exception:
        return 0.0

def calculate_hybridAvgShadowYield(stats, warmup_idx):
    try:
        mig = _find_hybrid_migrator(stats['mem'])
        if mig is None or 'avgShadowStallYield' not in mig.dtype.names:
            return 0.0
        triggers = (np.sum(mig['numMigrationTriggers'][-1])
                    - np.sum(mig['numMigrationTriggers'][warmup_idx]))
        val = np.sum(mig['avgShadowStallYield'][-1]) - np.sum(mig['avgShadowStallYield'][warmup_idx])
        return (val / triggers) / 1000.0 if triggers > 0 else 0.0
    except Exception:
        return 0.0

def calculate_migrationOverheadCycles(stats, warmup_idx):
    try:
        mem = stats['mem']
        if 'migrationOverheadCycles' not in mem.dtype.names:
            return 0.0
        return (np.sum(mem['migrationOverheadCycles'][-1])
                - np.sum(mem['migrationOverheadCycles'][warmup_idx]))
    except Exception:
        return 0.0

def calculate_hybridKneeTriggers(stats, warmup_idx):
    try:
        mig = _find_hybrid_migrator(stats['mem'])
        if mig is None or 'kneeTriggers' not in mig.dtype.names:
            return 0.0
        return (np.sum(mig['kneeTriggers'][-1])
                - np.sum(mig['kneeTriggers'][warmup_idx]))
    except Exception:
        return 0.0

def calculate_hybridSuppressionTriggers(stats, warmup_idx):
    try:
        mig = _find_hybrid_migrator(stats['mem'])
        if mig is None or 'suppressionTriggers' not in mig.dtype.names:
            return 0.0
        return (np.sum(mig['suppressionTriggers'][-1])
                - np.sum(mig['suppressionTriggers'][warmup_idx]))
    except Exception:
        return 0.0

def calculate_cxlBwCongestionCycles(stats, warmup_idx):
    """Total penalty cycles injected by Model A (CXL link BW congestion)."""
    try:
        mem = stats['mem']
        if 'cxlBwCongestionCycles' not in mem.dtype.names:
            return 0.0
        return (np.sum(mem['cxlBwCongestionCycles'][-1])
                - np.sum(mem['cxlBwCongestionCycles'][warmup_idx]))
    except Exception:
        return 0.0

def calculate_cxlHoLBlockingCycles(stats, warmup_idx):
    """Total penalty cycles injected by Model B (CXL head-of-line blocking)."""
    try:
        mem = stats['mem']
        if 'cxlHoLBlockingCycles' not in mem.dtype.names:
            return 0.0
        return (np.sum(mem['cxlHoLBlockingCycles'][-1])
                - np.sum(mem['cxlHoLBlockingCycles'][warmup_idx]))
    except Exception:
        return 0.0

def calculate_migrationOverheadPerCxlAccess(stats, warmup_idx):
    """Migration overhead cycles per CXL access (overhead contribution to effective CXL latency)."""
    try:
        overhead = calculate_migrationOverheadCycles(stats, warmup_idx)
        accesses = calculate_cxlAccessCount(stats, warmup_idx)
        if accesses <= 0:
            return 0.0
        return overhead / accesses
    except Exception:
        return 0.0

def calculate_cxlBwCongestionPerAccess(stats, warmup_idx):
    """Model A BW congestion penalty cycles per CXL access."""
    try:
        penalty = calculate_cxlBwCongestionCycles(stats, warmup_idx)
        accesses = calculate_cxlAccessCount(stats, warmup_idx)
        if accesses <= 0:
            return 0.0
        return penalty / accesses
    except Exception:
        return 0.0

def calculate_cxlHoLBlockingPerAccess(stats, warmup_idx):
    """Model B HoL blocking penalty cycles per CXL access."""
    try:
        penalty = calculate_cxlHoLBlockingCycles(stats, warmup_idx)
        accesses = calculate_cxlAccessCount(stats, warmup_idx)
        if accesses <= 0:
            return 0.0
        return penalty / accesses
    except Exception:
        return 0.0

# =============================================================================
# Register all metric calculators
# =============================================================================

METRIC_CALCULATORS = {
    'fastMemHitRatio':          calculate_fastMemHitRatio,
    'migrationRate':            calculate_migrationRate,
    'robStallCycles':           calculate_robStallCycles,
    'avgDdrLatency':            calculate_avgDdrLatency,
    'avgCxlLatency':            calculate_avgCxlLatency,
    'ddrBandwidthGBps':         calculate_ddrBandwidthGBps,
    'cxlBandwidthGBps':         calculate_cxlBandwidthGBps,
    'totalMemBandwidthGBps':    calculate_totalMemBandwidthGBps,
    'ddrAccessCount':           calculate_ddrAccessCount,
    'cxlAccessCount':           calculate_cxlAccessCount,
    'cxlAccessRatio':           calculate_cxlAccessRatio,
    'migrationCount':           calculate_migrationCount,
    'migrationBandwidthGBps':   calculate_migrationBandwidthGBps,
    'robStallCyclesPerKInstr':  calculate_robStallCyclesPerKInstr,
    'memStallRatio':            calculate_memStallRatio,
    'l3MissRate':               calculate_l3MissRate,
    'llscMPKI':                 calculate_llscMPKI,
    'memoryFootprintMB':        calculate_memoryFootprintMB,
    'freqBasedHotPages':        calculate_freqBasedHotPages,
    'robStallPagesTracked':     calculate_robStallPagesTracked,
    'hybridAvgEmaStall':        calculate_hybridAvgEmaStall,
    'hybridAvgStabilityBonus':  calculate_hybridAvgStabilityBonus,
    'hybridAvgShadowYield':     calculate_hybridAvgShadowYield,
    'migrationOverheadCycles':  calculate_migrationOverheadCycles,
    'hybridKneeTriggers':       calculate_hybridKneeTriggers,
    'hybridSuppressionTriggers': calculate_hybridSuppressionTriggers,
    'cxlBwCongestionCycles':    calculate_cxlBwCongestionCycles,
    'cxlHoLBlockingCycles':     calculate_cxlHoLBlockingCycles,
    # Per-access overhead contributions
    'migrationOverheadPerCxlAccess':  calculate_migrationOverheadPerCxlAccess,
    'cxlBwCongestionPerAccess':       calculate_cxlBwCongestionPerAccess,
    'cxlHoLBlockingPerAccess':        calculate_cxlHoLBlockingPerAccess,
}

# =============================================================================
# KNOBS
# =============================================================================

KNOBS = [
    # -------------------------------------------------------------------------
    # Sweep A — Main policy comparison (all 4 configs × 4 GB DRAM)
    #
    # Policies are ordered to tell the paper's chain-of-ideas story:
    #
    #   Baselines:
    #     none          — all-CXL, no migration
    #     freqBased     — traditional frequency-based (access count only)
    #     robStallBased/robStallCycles — naive ROB stall cycles (prior work)
    #     robStallBased/robStallCount  — naive ROB stall count (prior work)
    #
    #   Chain 1 (stall cycles contamination):
    #     fsc+sbs       — CYCLES-based composite + stability → FAILS on mobilenetv2/vgg16
    #     fsc_cnt+sbs   — COUNT-based composite + stability → no regression (Chain 1 fix)
    #
    #   Chain 2 ablation (bimodal latency → SVA):
    #     fsc_cnt+sva+sbs   — count + variance aware + stability
    #
    #   Chain 3 ablation (serialized shared link → CCF):
    #     fsc_cnt+sva+ccf+sbs — count + variance + fan-out + stability [PROPOSED METHOD]
    #
    #   Discussion (link knee → GKD/SCA):
    #     fsc_cnt+gkd+sca               — knee detect + congestion suppression
    #     fsc_cnt+sva+ccf+gkd+sca+sbs   — kitchen sink (shows diminishing returns)
    # -------------------------------------------------------------------------
    [
        ["migrationPolicy", "robStallMetric"],
        [
            # Baselines
            ["none",                          "",              True],
            ["freqBased",                     "",              True],
            ["robStallBased",                 "robStallCycles",True],
            ["robStallBased",                 "robStallCount", True],
            # Chain 1: cycles vs count (contamination story)
            ["fsc+sbs",                       "",              True],  # broken on ML
            ["fsc_cnt+sbs",                   "",              True],  # fixed
            # Chain 2: +SVA ablation
            ["fsc_cnt+sva+sbs",               "",              True],
            # Chain 3: +CCF ablation → proposed method
            ["fsc_cnt+sva+ccf+sbs",           "",              True],  # PROPOSED
            # Discussion: link-level alternatives
            ["fsc_cnt+gkd+sca",               "",              True],
            ["fsc_cnt+sva+ccf+gkd+sca+sbs",   "",              True],
        ],
        ["DRAMSizeMB = 4096L"],
        "Migration=%1_Metric=%2_Dram4GB"
    ],

    # -------------------------------------------------------------------------
    # Sweep B — DRAM capacity sensitivity
    # Configs: MessDdrCxl_WeakSramCaches only (primary config)
    # Policies: baselines + proposed method only (keep plot clean)
    # Sizes: 2 GB (tight), 4 GB (default), 8 GB (ample)
    # Note: The 4 GB runs overlap with Sweep A; include them for self-containment.
    # -------------------------------------------------------------------------
    [
        ["migrationPolicy", "robStallMetric"],
        [
            ["none",                    "", True],
            ["freqBased",               "", True],
            ["fsc_cnt+sbs",             "", True],
            ["fsc_cnt+sva+ccf+sbs",     "", True],  # proposed
        ],
        ["DRAMSizeMB = 2048L"],
        "Migration=%1_Metric=%2_Dram2GB"
    ],
    [
        ["migrationPolicy", "robStallMetric"],
        [
            ["none",                    "", True],
            ["freqBased",               "", True],
            ["fsc_cnt+sbs",             "", True],
            ["fsc_cnt+sva+ccf+sbs",     "", True],
        ],
        ["DRAMSizeMB = 8192L"],
        "Migration=%1_Metric=%2_Dram8GB"
    ],

    # -------------------------------------------------------------------------
    # Sweep C — fscAlpha sensitivity (all 4 configs × 4 GB)
    # Sweeps the frequency/stall balance for the proposed method.
    # Higher alpha = more frequency weight (safer on noisy workloads).
    # Lower alpha = purer stall signal (better when stall is reliable).
    # -------------------------------------------------------------------------
    [
        ["migrationPolicy", "robStallMetric", "fscAlpha"],
        [
            ["fsc_cnt+sva+ccf+sbs", "", "0.0001", True],
            ["fsc_cnt+sva+ccf+sbs", "", "0.001",  True],  # default
            ["fsc_cnt+sva+ccf+sbs", "", "0.005",  True],
            ["fsc_cnt+sva+ccf+sbs", "", "0.01",   True],
        ],
        ["DRAMSizeMB = 4096L"],
        "Migration=%1_Metric=%2_Alpha=%3_Dram4GB"
    ],

    # -------------------------------------------------------------------------
    # Sweep D — CXL characteristic model comparison (all 4 configs × 4 GB)
    #
    # Purpose: quantify the individual performance impact of each CXL-specific
    # hardware characteristic that the paper proposes methods to address.
    #
    # Model A: CXL link bandwidth congestion — non-linear latency penalty when
    #   CXL access ratio exceeds a knee (~70% utilization). Quadratic penalty
    #   past the knee to model super-linear queuing delay.
    #
    # Model B: CXL head-of-line blocking — serialization penalty proportional
    #   to the number of distinct cores that access CXL in the same interval.
    #   Models the shared, single-path CXL link seen by all cores.
    #
    # Each entry below runs 2 policies (none = baseline, proposed = fsc_cnt+sva+ccf+sbs)
    # under one of 4 model configurations. Comparing IPC and penalty cycle stats
    # across entries shows how much each characteristic costs, and how much
    # migration relieves each one.
    #
    # Job count: 2 rows × 25 workloads × 4 configs × 4 entries = 800 jobs
    # -------------------------------------------------------------------------

    # D1: No CXL characteristic models (baseline — same memory as Sweep A)
    [
        ["migrationPolicy", "robStallMetric"],
        [
            ["none",                "", True],
            ["fsc_cnt+sva+ccf+sbs", "", True],
        ],
        ["DRAMSizeMB = 4096L",
         "cxlBwCongestionEnabled = false",
         "cxlHoLBlockingEnabled = false"],
        "Migration=%1_ModelA_false_ModelB_false_Dram4GB"
    ],

    # D2: Model A only — CXL link bandwidth congestion (non-linear latency knee)
    [
        ["migrationPolicy", "robStallMetric"],
        [
            ["none",                "", True],
            ["fsc_cnt+sva+ccf+sbs", "", True],
        ],
        ["DRAMSizeMB = 4096L",
         "cxlBwCongestionEnabled = true",
         "cxlHoLBlockingEnabled = false"],
        "Migration=%1_ModelA_true_ModelB_false_Dram4GB"
    ],

    # D3: Model B only — CXL head-of-line blocking (serialized shared link)
    [
        ["migrationPolicy", "robStallMetric"],
        [
            ["none",                "", True],
            ["fsc_cnt+sva+ccf+sbs", "", True],
        ],
        ["DRAMSizeMB = 4096L",
         "cxlBwCongestionEnabled = false",
         "cxlHoLBlockingEnabled = true"],
        "Migration=%1_ModelA_false_ModelB_true_Dram4GB"
    ],

    # D4: Both models enabled — full CXL characteristic overhead
    [
        ["migrationPolicy", "robStallMetric"],
        [
            ["none",                "", True],
            ["fsc_cnt+sva+ccf+sbs", "", True],
        ],
        ["DRAMSizeMB = 4096L",
         "cxlBwCongestionEnabled = true",
         "cxlHoLBlockingEnabled = true"],
        "Migration=%1_ModelA_true_ModelB_true_Dram4GB"
    ],
]

# =============================================================================
# Workloads — full set (25 workloads across ML + graph domains)
# =============================================================================

BENCHMARKS_DIR = "/path/to/scratch/workloads"
BIN_DIR        = f"{BENCHMARKS_DIR}/bin"
MODELS_DIR     = f"{BENCHMARKS_DIR}/models"
DATASETS_DIR   = f"{BENCHMARKS_DIR}/datasets"
GRAPHS_DIR     = f"{BENCHMARKS_DIR}/graphs"
CONFIGS_DIR    = f"{BENCHMARKS_DIR}/configs"

WORKLOAD_GROUPS = {
    # Tuple format: (command, name, has_roi, should_plot)
    # has_roi=False → start in detailed mode (ML thread pools exist before ROI)
    'ml': [
        (f"{BIN_DIR}/inference --model {MODELS_DIR}/resnet50-v2-7.onnx --batch 2048 --iterations 5000 --threads 16",   "resnet50",    False, True),
        (f"{BIN_DIR}/inference --model {MODELS_DIR}/vgg16-12.onnx --batch 1024 --iterations 5000 --threads 16",         "vgg16",       False, True),
        (f"{BIN_DIR}/inference --model {MODELS_DIR}/mobilenetv2-12.onnx --batch 4096 --iterations 10000 --threads 16",  "mobilenetv2", False, True),
        (f"{BIN_DIR}/inference --model {MODELS_DIR}/inception-v2-9.onnx --batch 2048 --iterations 5000 --threads 16",   "inception_v2",False, True),
        (f"{BIN_DIR}/inference --model {MODELS_DIR}/yolov4.onnx --batch 512 --iterations 5000 --threads 16",            "yolov4",      False, True),
        (f"{BIN_DIR}/lightgbm config={CONFIGS_DIR}/lightgbm_zsim.txt",                                                  "lightgbm",    False, True),
        (f"{BIN_DIR}/xgboost_train --data {DATASETS_DIR}/HIGGS.csv --max_depth 25 --num_rounds 2000 --nthread 16",      "xgboost",     False, True),
    ],
    'graph_twitter': [
        (f"{BIN_DIR}/bc   -f {GRAPHS_DIR}/twitter.sg       -n64",  "bc_twitter",   False, True),
        (f"{BIN_DIR}/bfs  -f {GRAPHS_DIR}/twitter.sg       -n64",  "bfs_twitter",  False, True),
        (f"{BIN_DIR}/cc   -f {GRAPHS_DIR}/twitter.sg       -n64",  "cc_twitter",   False, True),
        (f"{BIN_DIR}/pr   -f {GRAPHS_DIR}/twitter.sg       -n64",  "pr_twitter",   False, True),
        (f"{BIN_DIR}/sssp -f {GRAPHS_DIR}/twitter.wsg      -n64",  "sssp_twitter", False, True),
        (f"{BIN_DIR}/tc   -f {GRAPHS_DIR}/twitter_undir.sg -n64",  "tc_twitter",   False, True),
    ],
    'graph_web': [
        (f"{BIN_DIR}/bc   -f {GRAPHS_DIR}/web.sg    -n64",  "bc_web",   False, True),
        (f"{BIN_DIR}/bfs  -f {GRAPHS_DIR}/web.sg    -n64",  "bfs_web",  False, True),
        (f"{BIN_DIR}/cc   -f {GRAPHS_DIR}/web.sg    -n64",  "cc_web",   False, True),
        (f"{BIN_DIR}/pr   -f {GRAPHS_DIR}/web.sg    -n64",  "pr_web",   False, True),
        (f"{BIN_DIR}/sssp -f {GRAPHS_DIR}/web.wsg   -n64",  "sssp_web", False, True),
        (f"{BIN_DIR}/tc   -f {GRAPHS_DIR}/webU.sg   -n64",  "tc_web",   False, True),
    ],
    'graph_road': [
        (f"{BIN_DIR}/bc   -f {GRAPHS_DIR}/road.sg        -n64",         "bc_road",   False, True),
        (f"{BIN_DIR}/bfs  -f {GRAPHS_DIR}/road.sg        -n64",         "bfs_road",  False, True),
        (f"{BIN_DIR}/cc   -f {GRAPHS_DIR}/road.sg        -n64",         "cc_road",   False, True),
        (f"{BIN_DIR}/pr   -f {GRAPHS_DIR}/road.sg        -n64",         "pr_road",   False, True),
        (f"{BIN_DIR}/sssp -f {GRAPHS_DIR}/road.wsg  -n64 -d50000",      "sssp_road", False, True),
        (f"{BIN_DIR}/tc   -f {GRAPHS_DIR}/roadU.sg       -n64",         "tc_road",   False, True),
    ],
}

_seen = set()
WORKLOADS = [
    (cmd, name, has_roi, should_plot)
    for group in WORKLOAD_GROUPS.values()
    for cmd, name, has_roi, should_plot in group
    if name not in _seen and not _seen.add(name)
]

# =============================================================================
# Global knobs — same as migration_study.py (100 billion instr cap)
# =============================================================================

GLOBAL_KNOBS = [
    "maxTotalInstrs = 100000000000L",
    "gmMBytes = 98304",
]

# =============================================================================
# Metrics
# =============================================================================

METRICS = [
    # --- Primary performance metrics (paper figures) ---
    # --- Workload characterization (policy-independent; use "none" baseline values) ---
    ("llscMPKI",                  "absolute"),      # Fig: workload memory intensity
    ("memoryFootprintMB",         "absolute"),      # Fig: workload working set size

    # --- Primary performance metrics (paper figures) ---
    ("IPC",                       "normalized"),    # Fig: main results bar chart
    ("fastMemHitRatio",           "percentage"),    # Fig: DDR hit rate (shows migration effectiveness)
    ("robStallCycles",            "normalized"),    # Fig: stall reduction (chain 1 motivation)
    ("robStallCyclesPerKInstr",   "absolute"),      # Fig: bottleneck severity per workload
    ("memStallRatio",             "percentage"),    # Fig: fraction of time memory-stalled

    # --- Migration activity ---
    ("migrationCount",            "absolute"),      # Fig: total pages migrated
    ("migrationRate",             "absolute"),      # Fig: migrations per 1M accesses
    ("migrationBandwidthGBps",    "absolute"),      # Fig: migration bandwidth overhead
    ("migrationOverheadCycles",   "absolute"),      # Fig: chain 4 — overhead cycles
    ("cxlBwCongestionCycles",     "absolute"),      # Fig: Model A penalty cycles (BW congestion)
    ("cxlHoLBlockingCycles",      "absolute"),      # Fig: Model B penalty cycles (HoL blocking)
    # Per-access overhead contributions (cycles added per CXL access)
    ("migrationOverheadPerCxlAccess",  "absolute"),
    ("cxlBwCongestionPerAccess",       "absolute"),
    ("cxlHoLBlockingPerAccess",        "absolute"),

    # --- Memory system characterization ---
    ("avgDdrLatency",             "absolute"),      # Fig: DDR access latency
    ("avgCxlLatency",             "absolute"),      # Fig: CXL access latency (shows congestion relief)
    ("ddrBandwidthGBps",          "absolute"),      # Fig: DDR bandwidth utilization
    ("cxlBandwidthGBps",          "absolute"),      # Fig: CXL bandwidth utilization
    ("totalMemBandwidthGBps",     "absolute"),
    ("ddrAccessCount",            "absolute"),
    ("cxlAccessCount",            "absolute"),
    ("cxlAccessRatio",            "percentage"),    # Fig: fraction of accesses still hitting CXL

    # --- Cache metrics ---
    ("l3MissRate",                "percentage"),

    # --- Method-specific diagnostics ---
    ("freqBasedHotPages",         "absolute"),
    ("robStallPagesTracked",      "absolute"),
    ("hybridAvgEmaStall",         "absolute"),      # Fig: EMA stall signal magnitude
    ("hybridAvgStabilityBonus",   "absolute"),      # Fig: stability bonus (SBS activity)
    ("hybridAvgShadowYield",      "absolute"),
    ("hybridKneeTriggers",        "absolute"),      # Fig: GKD trigger rate
    ("hybridSuppressionTriggers", "absolute"),      # Fig: SCA suppression rate
]
