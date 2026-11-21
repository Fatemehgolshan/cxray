#!/usr/bin/env python3
"""
zsim Short Experiment Configuration  (~1 hour on cluster)

A trimmed-down version of migration_study.py designed to run in roughly 1 hour
while still producing representative graphs across policies and workload types.

Reductions vs. migration_study.py:
  - 1 base config  (WeakSramCaches only; omit AggrSramCaches)
  - 6 migration policies  (vs. 21 in the full study)
  - 4 workloads  (2 ML + 2 GAPBS; vs. ~25 in the full study)
  - Total: 1 × 6 × 4 = 24 experiments

Simulation parameters differ from the full study so migrations actually fire:
  - maxTotalInstrs = 500_000_000  (500M; full study: 10M — too short to see any migration)
  - migrationIntervalAccesses = 10_000_000  (10M; full study: 100M)
  → ~50 migration intervals per run, enough to see policy differentiation

Policy selection rationale:
  none              – static baseline (everything in CXL with slowFirst)
  freqBased         – frequency-based classic
  robStallBased     – ROB stall-based (metric: robStallCycles)
  robPressureWeighted – advanced ROB stall with pressure weighting
  criticalityWeighted – criticality / synchronization detection
  fsc+cema          – hybrid (Frequency-Stall Composite + Cross-Interval EMA)

Workload selection rationale:
  resnet50    – ML inference, large working set, thread-pool parallelism
  lightgbm    – gradient boosting, very different memory footprint from ONNX
  bfs_twitter – graph BFS, extremely irregular memory access (pointer chasing)
  pr_twitter  – graph PageRank, regular iterative, high bandwidth demand

=== Available Migration Policies (reference) ===

BASELINE METHODS:
  - none: No migration, static placement only
  - freqBased: Frequency-based migration (access count)
  - robStallBased: ROB stall-based migration with variants:
      * robStallBinary: Binary flag (page caused stall or not)
      * robStallCycles: Total stall cycles per page
      * robStallCount: Number of stall events per page

NEW ADVANCED METHODS:
  - robPressureWeighted: Weights ROB stalls by inferred ROB pressure
  - criticalityWeighted: Weights stalls by instruction criticality
  - qlearning: Online reinforcement learning for page placement

HYBRID METHODS:
  fsc, sys, sse, spfd, cema, clcas, sbs, and combinations thereof
"""

import numpy as np
from datetime import datetime

# Base configuration file – only the WeakSramCaches variant for speed.
# Switch to a list to add AggrSramCaches when more coverage is needed.
BASE_CONFIG = [
    "../tests/Tiering_MessDdrCxl_WeakSramCaches.cfg",
]

# Output directory base (will append config name and timestamp)
OUTPUT_DIR = f'results/results_MigrationShort_{datetime.now().strftime("%Y%m%d_%H%M%S")}'

# Warmup phase: fraction of simulation to ignore for metrics (0.0 to 1.0)
WARMUP_PHASE = 0.2

# Custom metric calculators
# Each calculator receives (stats, warmup_idx) and returns a numeric value
# stats: h5py dataset from zsim.h5 (stats['stats']['root'])
# warmup_idx: index of first record after warmup

# ---------------------------------------------------------------------------
# Shared migrator-lookup helpers (mirrors migration_study.py)
# ---------------------------------------------------------------------------
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
    """Calculate fast memory (DDR) hit ratio"""
    mem = stats['mem']
    accesses_start = np.sum(mem['accesses'][warmup_idx])
    accesses_end = np.sum(mem['accesses'][-1])
    ddr_start = np.sum(mem['ddrAccesses'][warmup_idx])
    ddr_end = np.sum(mem['ddrAccesses'][-1])

    total_accesses = accesses_end - accesses_start
    total_ddr = ddr_end - ddr_start

    if total_accesses > 0:
        return total_ddr / total_accesses
    return 0.0

def calculate_migrationRate(stats, warmup_idx):
    """Calculate page migration rate (migrations per 1M memory accesses)"""
    try:
        mem_data = stats['mem']
        migrator_data = _find_any_migrator(mem_data)
        if migrator_data is None or 'numPageMigrations' not in migrator_data.dtype.names:
            return 0.0

        migrations_start = np.sum(migrator_data['numPageMigrations'][warmup_idx])
        migrations_end   = np.sum(migrator_data['numPageMigrations'][-1])
        total_migrations = migrations_end - migrations_start

        if 'numAccesses' in migrator_data.dtype.names:
            acc = migrator_data['numAccesses']
        elif 'pageTrackerAccesses' in migrator_data.dtype.names:
            acc = migrator_data['pageTrackerAccesses']
        else:
            acc = mem_data['accesses']
        total_accesses = np.sum(acc[-1]) - np.sum(acc[warmup_idx])

        if total_accesses > 0:
            return (total_migrations / total_accesses) * 1e6
        return 0.0
    except Exception:
        return 0.0

def calculate_robStallCycles(stats, warmup_idx):
    """Calculate total ROB head stall cycles"""
    try:
        mem = stats['mem']
        stalls_start = np.sum(mem['robHeadStallCycles'][warmup_idx])
        stalls_end = np.sum(mem['robHeadStallCycles'][-1])
        return stalls_end - stalls_start
    except:
        return 0.0

def calculate_avgRobPressure(stats, warmup_idx):
    """Calculate average ROB pressure (for robPressureWeighted method)"""
    try:
        mem_data = stats['mem']

        # Find RobPressureWeighted migrator field
        migrator_field = None
        for field_name in mem_data.dtype.names:
            if 'RobPressureWeighted' in field_name:
                migrator_field = field_name
                break

        if migrator_field is None:
            return 0.0

        migrator_data = mem_data[migrator_field]

        # Check if fields exist
        if 'avgRobPressure' not in migrator_data.dtype.names or \
           'numMigrationTriggers' not in migrator_data.dtype.names:
            return 0.0

        pressure_start = np.sum(migrator_data['avgRobPressure'][warmup_idx])
        pressure_end = np.sum(migrator_data['avgRobPressure'][-1])
        triggers = np.sum(migrator_data['numMigrationTriggers'][-1]) - \
                   np.sum(migrator_data['numMigrationTriggers'][warmup_idx])

        if triggers > 0:
            return ((pressure_end - pressure_start) / triggers) / 1000.0  # Convert back from fixed-point
        return 0.0
    except Exception as e:
        return 0.0

def calculate_avgCriticality(stats, warmup_idx):
    """Calculate average criticality score (for criticalityWeighted method)"""
    try:
        mem_data = stats['mem']

        # Find CriticalityWeighted migrator field
        migrator_field = None
        for field_name in mem_data.dtype.names:
            if 'CriticalityWeighted' in field_name:
                migrator_field = field_name
                break

        if migrator_field is None:
            return 0.0

        migrator_data = mem_data[migrator_field]

        # Check if fields exist
        if 'avgCriticalityScore' not in migrator_data.dtype.names or \
           'numMigrationTriggers' not in migrator_data.dtype.names:
            return 0.0

        crit_start = np.sum(migrator_data['avgCriticalityScore'][warmup_idx])
        crit_end = np.sum(migrator_data['avgCriticalityScore'][-1])
        triggers = np.sum(migrator_data['numMigrationTriggers'][-1]) - \
                   np.sum(migrator_data['numMigrationTriggers'][warmup_idx])

        if triggers > 0:
            return ((crit_end - crit_start) / triggers) / 1000.0  # Convert back from fixed-point
        return 0.0
    except Exception as e:
        return 0.0

def calculate_qlearningReward(stats, warmup_idx):
    """Calculate total reward (for qlearning method)"""
    try:
        mem_data = stats['mem']

        # Find QLearning migrator field
        migrator_field = None
        for field_name in mem_data.dtype.names:
            if 'QLearning' in field_name:
                migrator_field = field_name
                break

        if migrator_field is None:
            return 0.0

        migrator_data = mem_data[migrator_field]

        # Check if totalReward field exists
        if 'totalReward' not in migrator_data.dtype.names:
            return 0.0

        reward_start = np.sum(migrator_data['totalReward'][warmup_idx])
        reward_end = np.sum(migrator_data['totalReward'][-1])
        return reward_end - reward_start
    except Exception as e:
        return 0.0

# =============================================================================
# Memory Latency & Bandwidth Metrics
# =============================================================================

def calculate_avgDdrLatency(stats, warmup_idx):
    """Calculate average DDR access latency in nanoseconds"""
    try:
        mem = stats['mem']
        accesses_start = np.sum(mem['ddrAccesses'][warmup_idx])
        accesses_end = np.sum(mem['ddrAccesses'][-1])
        latency_start = np.sum(mem['ddrLatency'][warmup_idx])
        latency_end = np.sum(mem['ddrLatency'][-1])

        total_accesses = accesses_end - accesses_start
        total_latency = latency_end - latency_start

        if total_accesses > 0:
            return total_latency / total_accesses
        return 0.0
    except:
        return 0.0

def calculate_avgCxlLatency(stats, warmup_idx):
    """Calculate average CXL access latency in nanoseconds"""
    try:
        mem = stats['mem']
        accesses_start = np.sum(mem['cxlAccesses'][warmup_idx])
        accesses_end = np.sum(mem['cxlAccesses'][-1])
        latency_start = np.sum(mem['cxlLatency'][warmup_idx])
        latency_end = np.sum(mem['cxlLatency'][-1])

        total_accesses = accesses_end - accesses_start
        total_latency = latency_end - latency_start

        if total_accesses > 0:
            return total_latency / total_accesses
        return 0.0
    except:
        return 0.0

def calculate_ddrBandwidthGBps(stats, warmup_idx):
    """Calculate DDR bandwidth utilization in GB/s"""
    try:
        mem = stats['mem']
        cores = stats['c']

        # Get simulation time in cycles
        cycles_start = np.max(cores['cycles'][warmup_idx])
        cycles_end = np.max(cores['cycles'][-1])
        total_cycles = cycles_end - cycles_start

        # Get DDR accesses (each access is typically 64 bytes)
        accesses_start = np.sum(mem['ddrAccesses'][warmup_idx])
        accesses_end = np.sum(mem['ddrAccesses'][-1])
        total_accesses = accesses_end - accesses_start

        # Assume 64-byte cache line, 2GHz frequency
        bytes_transferred = total_accesses * 64
        time_seconds = total_cycles / 2e9  # 2GHz

        if time_seconds > 0:
            return bytes_transferred / time_seconds / 1e9  # GB/s
        return 0.0
    except:
        return 0.0

def calculate_cxlBandwidthGBps(stats, warmup_idx):
    """Calculate CXL bandwidth utilization in GB/s"""
    try:
        mem = stats['mem']
        cores = stats['c']

        # Get simulation time in cycles
        cycles_start = np.max(cores['cycles'][warmup_idx])
        cycles_end = np.max(cores['cycles'][-1])
        total_cycles = cycles_end - cycles_start

        # Get CXL accesses
        accesses_start = np.sum(mem['cxlAccesses'][warmup_idx])
        accesses_end = np.sum(mem['cxlAccesses'][-1])
        total_accesses = accesses_end - accesses_start

        # Assume 64-byte cache line, 2GHz frequency
        bytes_transferred = total_accesses * 64
        time_seconds = total_cycles / 2e9  # 2GHz

        if time_seconds > 0:
            return bytes_transferred / time_seconds / 1e9  # GB/s
        return 0.0
    except:
        return 0.0

def calculate_totalMemBandwidthGBps(stats, warmup_idx):
    """Calculate total memory bandwidth (DDR + CXL) in GB/s"""
    try:
        return calculate_ddrBandwidthGBps(stats, warmup_idx) + calculate_cxlBandwidthGBps(stats, warmup_idx)
    except:
        return 0.0

# =============================================================================
# Access Pattern Metrics
# =============================================================================

def calculate_ddrAccessCount(stats, warmup_idx):
    """Calculate total DDR accesses (in millions)"""
    try:
        mem = stats['mem']
        accesses_start = np.sum(mem['ddrAccesses'][warmup_idx])
        accesses_end = np.sum(mem['ddrAccesses'][-1])
        return (accesses_end - accesses_start) / 1e6
    except:
        return 0.0

def calculate_cxlAccessCount(stats, warmup_idx):
    """Calculate total CXL accesses (in millions)"""
    try:
        mem = stats['mem']
        accesses_start = np.sum(mem['cxlAccesses'][warmup_idx])
        accesses_end = np.sum(mem['cxlAccesses'][-1])
        return (accesses_end - accesses_start) / 1e6
    except:
        return 0.0

def calculate_cxlAccessRatio(stats, warmup_idx):
    """Calculate fraction of accesses going to CXL (0.0 = all DDR, 1.0 = all CXL)"""
    try:
        mem = stats['mem']
        ddr_start = np.sum(mem['ddrAccesses'][warmup_idx])
        ddr_end = np.sum(mem['ddrAccesses'][-1])
        cxl_start = np.sum(mem['cxlAccesses'][warmup_idx])
        cxl_end = np.sum(mem['cxlAccesses'][-1])

        total_ddr = ddr_end - ddr_start
        total_cxl = cxl_end - cxl_start
        total_accesses = total_ddr + total_cxl

        if total_accesses > 0:
            return total_cxl / total_accesses
        return 0.0
    except:
        return 0.0

# =============================================================================
# Migration Overhead Metrics
# =============================================================================

def calculate_migrationCount(stats, warmup_idx):
    """Calculate total number of page migrations"""
    try:
        mem_data = stats['mem']
        migrator_data = _find_any_migrator(mem_data)
        if migrator_data is None or 'numPageMigrations' not in migrator_data.dtype.names:
            return 0.0
        start = np.sum(migrator_data['numPageMigrations'][warmup_idx])
        end   = np.sum(migrator_data['numPageMigrations'][-1])
        return end - start
    except Exception:
        return 0.0

def calculate_migrationBandwidthGBps(stats, warmup_idx):
    """Calculate migration bandwidth overhead in GB/s"""
    try:
        mem_data = stats['mem']
        migrator_data = _find_any_migrator(mem_data)
        if migrator_data is None or 'numPageMigrations' not in migrator_data.dtype.names:
            return 0.0

        migrations_start = np.sum(migrator_data['numPageMigrations'][warmup_idx])
        migrations_end   = np.sum(migrator_data['numPageMigrations'][-1])
        total_migrations = migrations_end - migrations_start

        cores = stats['c']
        total_cycles = np.max(cores['cycles'][-1]) - np.max(cores['cycles'][warmup_idx])

        bytes_migrated = total_migrations * 4096
        time_seconds = total_cycles / 2e9

        if time_seconds > 0:
            return bytes_migrated / time_seconds / 1e9  # GB/s
        return 0.0
    except Exception:
        return 0.0

# =============================================================================
# ROB & Performance Bottleneck Metrics
# =============================================================================

def calculate_robStallCyclesPerKInstr(stats, warmup_idx):
    """Calculate ROB stall cycles per 1K instructions (normalized bottleneck metric)"""
    try:
        mem = stats['mem']
        cores = stats['c']

        stalls_start = np.sum(mem['robHeadStallCycles'][warmup_idx])
        stalls_end = np.sum(mem['robHeadStallCycles'][-1])
        instrs_start = np.sum(cores['instrs'][warmup_idx])
        instrs_end = np.sum(cores['instrs'][-1])

        total_stalls = stalls_end - stalls_start
        total_instrs = instrs_end - instrs_start

        if total_instrs > 0:
            return (total_stalls / total_instrs) * 1000  # Per 1K instructions
        return 0.0
    except:
        return 0.0

def calculate_memStallRatio(stats, warmup_idx):
    """Calculate ratio of memory stalls to total execution cycles"""
    try:
        mem = stats['mem']
        cores = stats['c']

        stalls_start = np.sum(mem['robHeadStallCycles'][warmup_idx])
        stalls_end = np.sum(mem['robHeadStallCycles'][-1])
        cycles_start = np.max(cores['cycles'][warmup_idx])
        cycles_end = np.max(cores['cycles'][-1])

        total_stalls = stalls_end - stalls_start
        total_cycles = cycles_end - cycles_start

        if total_cycles > 0:
            return total_stalls / total_cycles
        return 0.0
    except:
        return 0.0

# =============================================================================
# Method-Specific Advanced Metrics
# =============================================================================

def calculate_freqBasedHotPages(stats, warmup_idx):
    """Calculate number of hot pages tracked (freqBased method)"""
    try:
        mem_data = stats['mem']

        # Find FreqBased migrator field
        migrator_field = None
        for field_name in mem_data.dtype.names:
            if 'FreqBased' in field_name:
                migrator_field = field_name
                break

        if migrator_field is None:
            return 0.0

        migrator_data = mem_data[migrator_field]

        # Check if numHotPages field exists
        if 'numHotPages' not in migrator_data.dtype.names:
            return 0.0

        hot_pages_end = np.sum(migrator_data['numHotPages'][-1])
        return hot_pages_end
    except Exception as e:
        return 0.0

def calculate_robStallPagesTracked(stats, warmup_idx):
    """Calculate number of pages causing ROB stalls (robStallBased methods)"""
    try:
        mem_data = stats['mem']

        # Find RobStallBased migrator field
        migrator_field = None
        for field_name in mem_data.dtype.names:
            if 'RobStallBased' in field_name:
                migrator_field = field_name
                break

        if migrator_field is None:
            return 0.0

        migrator_data = mem_data[migrator_field]

        # Check if numStallPages field exists
        if 'numStallPages' not in migrator_data.dtype.names:
            return 0.0

        stall_pages_end = np.sum(migrator_data['numStallPages'][-1])
        return stall_pages_end
    except Exception as e:
        return 0.0

def calculate_robPressureBursts(stats, warmup_idx):
    """Calculate number of ROB pressure bursts detected (robPressureWeighted)"""
    try:
        migrator_path = None
        for key in stats['mem'].keys():
            if 'RobPressureWeighted' in key:
                migrator_path = stats['mem'][key]
                break

        if migrator_path is None or 'numBurstsDetected' not in migrator_path:
            return 0.0

        bursts_start = np.sum(migrator_path['numBurstsDetected'][warmup_idx])
        bursts_end = np.sum(migrator_path['numBurstsDetected'][-1])
        return bursts_end - bursts_start
    except:
        return 0.0

def calculate_criticalitySyncDetections(stats, warmup_idx):
    """Calculate number of synchronization patterns detected (criticalityWeighted)"""
    try:
        migrator_path = None
        for key in stats['mem'].keys():
            if 'CriticalityWeighted' in key:
                migrator_path = stats['mem'][key]
                break

        if migrator_path is None or 'numSyncDetections' not in migrator_path:
            return 0.0

        sync_start = np.sum(migrator_path['numSyncDetections'][warmup_idx])
        sync_end = np.sum(migrator_path['numSyncDetections'][-1])
        return sync_end - sync_start
    except:
        return 0.0

def calculate_qlearningAvgQValue(stats, warmup_idx):
    """Calculate average Q-value (measure of learned policy quality)"""
    try:
        migrator_path = None
        for key in stats['mem'].keys():
            if 'QLearning' in key:
                migrator_path = stats['mem'][key]
                break

        if migrator_path is None or 'avgQValue' not in migrator_path:
            return 0.0

        qvalue_start = np.sum(migrator_path['avgQValue'][warmup_idx])
        qvalue_end = np.sum(migrator_path['avgQValue'][-1])
        samples = len(range(warmup_idx, len(migrator_path['avgQValue'])))

        if samples > 0:
            return (qvalue_end - qvalue_start) / samples / 1000.0  # Convert from fixed-point
        return 0.0
    except:
        return 0.0

def calculate_qlearningExplorationRate(stats, warmup_idx):
    """Calculate exploration rate (fraction of random actions taken)"""
    try:
        migrator_path = None
        for key in stats['mem'].keys():
            if 'QLearning' in key:
                migrator_path = stats['mem'][key]
                break

        if migrator_path is None or 'numExplorations' not in migrator_path or 'numActions' not in migrator_path:
            return 0.0

        explorations_start = np.sum(migrator_path['numExplorations'][warmup_idx])
        explorations_end = np.sum(migrator_path['numExplorations'][-1])
        actions_start = np.sum(migrator_path['numActions'][warmup_idx])
        actions_end = np.sum(migrator_path['numActions'][-1])

        total_explorations = explorations_end - explorations_start
        total_actions = actions_end - actions_start

        if total_actions > 0:
            return total_explorations / total_actions
        return 0.0
    except:
        return 0.0

# =============================================================================
# Cache Performance Metrics
# =============================================================================

def calculate_l3MissRate(stats, warmup_idx):
    """Calculate L3 cache miss rate"""
    try:
        # stats is a numpy structured array, access 'l3' field directly
        if 'l3' not in stats.dtype.names:
            return 0.0

        l3_data = stats['l3']

        hits_start = np.sum(l3_data['hGETS'][warmup_idx])
        hits_end = np.sum(l3_data['hGETS'][-1])
        misses_start = np.sum(l3_data['mGETS'][warmup_idx])
        misses_end = np.sum(l3_data['mGETS'][-1])

        total_hits = hits_end - hits_start
        total_misses = misses_end - misses_start
        total_accesses = total_hits + total_misses

        if total_accesses > 0:
            return total_misses / total_accesses
        return 0.0
    except Exception as e:
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
        unique = np.sum(stats['mem']['uniquePages'][-1])
        return unique * 4096.0 / 1024.0 / 1024.0
    except Exception:
        return 0.0

# =============================================================================
# Hybrid method metrics (fsc, sys, sse, spfd, cema, clcas, sbs, combinations)
# =============================================================================

def _find_hybrid_migrator(mem_data):
    """Return the hybrid migrator structured array (or None if not found)."""
    HYBRID_NAMES = [
        "fsc", "sys", "sse", "spfd", "cema", "clcas", "sbs",
        "fsc+cema", "fsc+sbs", "fsc+cema+sbs",
        "sse+spfd", "sse+spfd+clcas", "sys+cema",
        # Count-based variants
        "fsc_cnt", "fsc_cnt+sbs", "fsc_cnt+cema+sbs", "sse+spfd+cnt",
        # CXL-aware ideas
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
    """Average EMA stall score reported by hybrid methods."""
    try:
        mem_data = stats['mem']
        mig = _find_hybrid_migrator(mem_data)
        if mig is None or 'avgEmaStall' not in mig.dtype.names:
            return 0.0
        val_start = np.sum(mig['avgEmaStall'][warmup_idx])
        val_end   = np.sum(mig['avgEmaStall'][-1])
        triggers  = np.sum(mig['numMigrationTriggers'][-1]) - \
                    np.sum(mig['numMigrationTriggers'][warmup_idx])
        return ((val_end - val_start) / triggers) if triggers > 0 else 0.0
    except Exception:
        return 0.0

def calculate_hybridAvgStabilityBonus(stats, warmup_idx):
    """Average stability multiplier (×1000 fixed-point → divide by 1000)."""
    try:
        mem_data = stats['mem']
        mig = _find_hybrid_migrator(mem_data)
        if mig is None or 'avgStabilityBonus' not in mig.dtype.names:
            return 0.0
        val_start = np.sum(mig['avgStabilityBonus'][warmup_idx])
        val_end   = np.sum(mig['avgStabilityBonus'][-1])
        triggers  = np.sum(mig['numMigrationTriggers'][-1]) - \
                    np.sum(mig['numMigrationTriggers'][warmup_idx])
        if triggers > 0:
            return ((val_end - val_start) / triggers) / 1000.0
        return 0.0
    except Exception:
        return 0.0

def calculate_hybridAvgShadowYield(stats, warmup_idx):
    """Global stall yield used by SSE methods (×1000 fixed-point → divide by 1000)."""
    try:
        mem_data = stats['mem']
        mig = _find_hybrid_migrator(mem_data)
        if mig is None or 'avgShadowStallYield' not in mig.dtype.names:
            return 0.0
        val_start = np.sum(mig['avgShadowStallYield'][warmup_idx])
        val_end   = np.sum(mig['avgShadowStallYield'][-1])
        triggers  = np.sum(mig['numMigrationTriggers'][-1]) - \
                    np.sum(mig['numMigrationTriggers'][warmup_idx])
        if triggers > 0:
            return ((val_end - val_start) / triggers) / 1000.0
        return 0.0
    except Exception:
        return 0.0

def calculate_migrationOverheadCycles(stats, warmup_idx):
    """Total penalty cycles added to CXL accesses due to migration bandwidth contention."""
    try:
        mem = stats['mem']
        if 'migrationOverheadCycles' not in mem.dtype.names:
            return 0.0
        start = np.sum(mem['migrationOverheadCycles'][warmup_idx])
        end   = np.sum(mem['migrationOverheadCycles'][-1])
        return end - start
    except Exception:
        return 0.0

def calculate_hybridKneeTriggers(stats, warmup_idx):
    """Number of migration intervals where GKD detected link utilisation past the knee."""
    try:
        mem_data = stats['mem']
        mig = _find_hybrid_migrator(mem_data)
        if mig is None or 'kneeTriggers' not in mig.dtype.names:
            return 0.0
        start = np.sum(mig['kneeTriggers'][warmup_idx])
        end   = np.sum(mig['kneeTriggers'][-1])
        return end - start
    except Exception:
        return 0.0

def calculate_hybridSuppressionTriggers(stats, warmup_idx):
    """Number of migration intervals where SCA suppressed migrations due to congestion."""
    try:
        mem_data = stats['mem']
        mig = _find_hybrid_migrator(mem_data)
        if mig is None or 'suppressionTriggers' not in mig.dtype.names:
            return 0.0
        start = np.sum(mig['suppressionTriggers'][warmup_idx])
        end   = np.sum(mig['suppressionTriggers'][-1])
        return end - start
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
    # Basic metrics
    'fastMemHitRatio': calculate_fastMemHitRatio,
    'migrationRate': calculate_migrationRate,
    'robStallCycles': calculate_robStallCycles,
    'avgRobPressure': calculate_avgRobPressure,
    'avgCriticality': calculate_avgCriticality,
    'qlearningReward': calculate_qlearningReward,

    # Memory latency & bandwidth
    'avgDdrLatency': calculate_avgDdrLatency,
    'avgCxlLatency': calculate_avgCxlLatency,
    'ddrBandwidthGBps': calculate_ddrBandwidthGBps,
    'cxlBandwidthGBps': calculate_cxlBandwidthGBps,
    'totalMemBandwidthGBps': calculate_totalMemBandwidthGBps,

    # Access patterns
    'ddrAccessCount': calculate_ddrAccessCount,
    'cxlAccessCount': calculate_cxlAccessCount,
    'cxlAccessRatio': calculate_cxlAccessRatio,

    # Migration overhead
    'migrationCount': calculate_migrationCount,
    'migrationBandwidthGBps': calculate_migrationBandwidthGBps,

    # ROB & bottleneck analysis
    'robStallCyclesPerKInstr': calculate_robStallCyclesPerKInstr,
    'memStallRatio': calculate_memStallRatio,

    # Method-specific metrics
    'freqBasedHotPages': calculate_freqBasedHotPages,
    'robStallPagesTracked': calculate_robStallPagesTracked,
    'robPressureBursts': calculate_robPressureBursts,
    'criticalitySyncDetections': calculate_criticalitySyncDetections,
    'qlearningAvgQValue': calculate_qlearningAvgQValue,
    'qlearningExplorationRate': calculate_qlearningExplorationRate,

    # Cache metrics
    'l3MissRate':        calculate_l3MissRate,
    'llscMPKI':          calculate_llscMPKI,
    'memoryFootprintMB': calculate_memoryFootprintMB,

    # Hybrid method diagnostics
    'hybridAvgEmaStall':       calculate_hybridAvgEmaStall,
    'hybridAvgStabilityBonus': calculate_hybridAvgStabilityBonus,
    'hybridAvgShadowYield':    calculate_hybridAvgShadowYield,

    # CXL-aware ideas: overhead + link-level diagnostics
    'migrationOverheadCycles':    calculate_migrationOverheadCycles,
    'hybridKneeTriggers':         calculate_hybridKneeTriggers,
    'hybridSuppressionTriggers':  calculate_hybridSuppressionTriggers,
    'cxlBwCongestionCycles':      calculate_cxlBwCongestionCycles,
    'cxlHoLBlockingCycles':       calculate_cxlHoLBlockingCycles,
    # Per-access overhead contributions
    'migrationOverheadPerCxlAccess':  calculate_migrationOverheadPerCxlAccess,
    'cxlBwCongestionPerAccess':       calculate_cxlBwCongestionPerAccess,
    'cxlHoLBlockingPerAccess':        calculate_cxlHoLBlockingPerAccess,
}

# =============================================================================
# Knobs to sweep  ← REDUCED vs. migration_study.py
#
# Full study:  33 policy combinations × 2 DRAM sizes
# Short study:  8 policy combinations × 1 DRAM size  (4 GB)
#
# Policies chosen to span the design space:
#   none                    – static baseline (all pages in slow CXL with slowFirst)
#   freqBased               – frequency-based (simple, well-understood)
#   fsc+sbs                 – Frequency-Stall Composite + stability bonus (batch 1 winner)
#   fsc_cnt+sbs             – count-based FSC + stability (robust against congestion)
#   fsc_cnt+sva+ccf+sbs     – primary paper combo: variance + fan-out + stability
#   fsc_cnt+gkd+sca         – link-level combo: knee detection + self-competition avoidance
#   fsc_cnt+sva+ccf+gkd+sca+sbs – full kitchen sink
# =============================================================================
KNOBS = [
    [
        ["migrationPolicy", "robStallMetric"],
        [
            ["none",                             ""],
            ["freqBased",                        ""],
            ["fsc+sbs",                          ""],
            ["fsc_cnt+sbs",                      ""],
            ["fsc_cnt+sva+ccf+sbs",              ""],   # primary paper combo
            ["fsc_cnt+gkd+sca",                  ""],   # link-level combo
            ["fsc_cnt+sva+ccf+gkd+sca+sbs",      ""],   # full kitchen sink
        ],
        ["DRAMSizeMB = 4096L"],
        "Migration=%1_Metric=%2_Dram4GB"
    ],
]

# =============================================================================
# WORKLOAD DEFINITIONS  ← REDUCED vs. migration_study.py
#
# Full study:  7 ML + 18 GAPBS = ~25 workloads
# Short study: 2 ML + 2 GAPBS  =   4 workloads
#
# Selection covers two very different memory access patterns per domain:
#   resnet50    – large CNN, high memory footprint, batched matmuls
#   lightgbm    – gradient boosting, pointer-heavy tree traversal
#   bfs_twitter – irregular BFS (pointer chasing), memory-bound
#   pr_twitter  – regular iterative PageRank, bandwidth-bound
# =============================================================================

# BENCHMARKS_DIR = "/path/to/proj/zsim_benchmarks"
BENCHMARKS_DIR = "/path/to/scratch/workloads"
BIN_DIR        = f"{BENCHMARKS_DIR}/bin"
MODELS_DIR     = f"{BENCHMARKS_DIR}/models"
DATASETS_DIR   = f"{BENCHMARKS_DIR}/datasets"
GRAPHS_DIR     = f"{BENCHMARKS_DIR}/graphs"
CONFIGS_DIR    = f"{BENCHMARKS_DIR}/configs"

WORKLOAD_GROUPS = {
    # Tuples: (command, name, has_roi)
    # has_roi=True  → simulation starts in fast-forward mode (startFastForwarded = true)
    # has_roi=False → simulation starts in detailed mode   (startFastForwarded = false)
    # Quick test: Just 1 workload
    'ml': [
        # ML workloads: has_roi=False → startFastForwarded = false
        (f"{BIN_DIR}/inference --model {MODELS_DIR}/resnet50-v2-7.onnx --batch 2048 --iterations 5000 --threads 16", "resnet50",  False),
    ],
}

# Aggregate all groups into a single flat list (deduplication preserves order)
_seen = set()
WORKLOADS = [
    (cmd, name, has_roi)
    for group in WORKLOAD_GROUPS.values()
    for cmd, name, has_roi in group
    if name not in _seen and not _seen.add(name)
]

# =============================================================================
# Global knobs  ← ADJUSTED vs. migration_study.py
#
# maxTotalInstrs is increased 50× (10M → 500M) so that:
#   - ML workloads get past initialization into steady-state computation
#   - Enough instructions accumulate for meaningful IPC / stall measurements
#
# migrationIntervalAccesses is decreased 10× (100M → 10M) so that:
#   - Migrations actually fire during the run (~50 intervals at 500M instrs)
#   - Policy differences show up in fastMemHitRatio, migrationCount, etc.
#
# Tune these two values together if cluster node-hours are the bottleneck:
#   Fewer instrs / shorter interval  →  faster, coarser view
#   More instrs  / longer interval   →  slower, more representative
# =============================================================================
GLOBAL_KNOBS = [
    "maxTotalInstrs = 50000000L",  # 50M instructions for quick testing
    "migrationIntervalAccesses = 1000000L",
]

# =============================================================================
# What to measure/plot
# =============================================================================
# Format: List of tuples (metric_name, display_mode)
# Display modes: "absolute", "normalized", "percentage"
#   - "absolute": Show raw values
#   - "normalized": Normalize to first configuration (baseline = 1.0)
#   - "percentage": Show as percentage (multiply by 100, add %)

METRICS = [
     # Workload characterization
     ("llscMPKI",          "absolute"),
     ("memoryFootprintMB", "absolute"),
     ("IPC", "normalized"),
     ("fastMemHitRatio", "percentage"),
     ("avgDdrLatency", "absolute"),
     ("avgCxlLatency", "absolute"),
     ("ddrBandwidthGBps", "absolute"),
     ("cxlBandwidthGBps", "absolute"),
     ("migrationRate", "absolute"),
     ("migrationBandwidthGBps", "absolute"),
     ("robStallCyclesPerKInstr", "absolute"),
     ("memStallRatio", "percentage"),
     ("totalMemBandwidthGBps", "absolute"),
     ("ddrAccessCount", "absolute"),
     ("cxlAccessCount", "absolute"),
     ("cxlAccessRatio", "percentage"),
     ("migrationCount", "absolute"),
     ("robStallCycles", "normalized"),
     ("l3MissRate", "percentage"),
     # Freq-based specific
     ("freqBasedHotPages", "absolute"),
     # ROB-based specific
     ("robStallPagesTracked", "absolute"),
     # RobPressureWeighted specific
     ("avgRobPressure", "absolute"),
     ("robPressureBursts", "absolute"),
     # CriticalityWeighted specific
     ("avgCriticality", "absolute"),
     ("criticalitySyncDetections", "absolute"),
     # Q-learning specific (not in short sweep, kept for parsing compatibility)
     ("qlearningReward", "absolute"),
     ("qlearningAvgQValue", "absolute"),
     ("qlearningExplorationRate", "percentage"),
     # Hybrid methods
     ("hybridAvgEmaStall", "absolute"),
     ("hybridAvgStabilityBonus", "absolute"),
     ("hybridAvgShadowYield", "absolute"),
     # CXL-aware ideas: overhead + link-level diagnostics
     ("migrationOverheadCycles",   "absolute"),
     ("hybridKneeTriggers",        "absolute"),
     ("hybridSuppressionTriggers", "absolute"),
     ("cxlBwCongestionCycles",     "absolute"),
     ("cxlHoLBlockingCycles",      "absolute"),
     # Per-access overhead contributions
     ("migrationOverheadPerCxlAccess",  "absolute"),
     ("cxlBwCongestionPerAccess",       "absolute"),
     ("cxlHoLBlockingPerAccess",        "absolute"),
]
