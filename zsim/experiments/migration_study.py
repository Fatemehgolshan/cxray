#!/usr/bin/env python3
"""
zsim Experiment Configuration

Define your parameter sweeps here similar to MARS's KNOBS pattern.

=== Available Migration Policies ===

BASELINE METHODS:
  - none: No migration, static placement only
  - freqBased: Frequency-based migration (access count)
  - robStallBased: ROB stall-based migration with variants:
      * robStallBinary: Binary flag (page caused stall or not)
      * robStallCycles: Total stall cycles per page
      * robStallCount: Number of stall events per page

NEW ADVANCED METHODS:
  - robPressureWeighted: Weights ROB stalls by inferred ROB pressure
      * Tracks stall bursts and intensity in sliding window
      * Higher ROB pressure = more critical stalls
      * Formula: weightedStalls = cycles × (1.0 + robPressure)

  - criticalityWeighted: Weights stalls by instruction criticality
      * Detects synchronized access patterns (memory barriers)
      * Tracks temporal burstiness of page accesses
      * Combines burstiness (30%) + synchronization (50%) + stall intensity (20%)

  - qlearning: Online reinforcement learning for page placement
      * State: Access frequency × stall level × current location (32 states)
      * Actions: Keep FAST, Move SLOW, Move FAST, Keep SLOW
      * Learns optimal policy via Q-learning (α=0.1, γ=0.9, ε-greedy)
      * Reward: Stall reduction minus migration penalty
"""

import numpy as np
from datetime import datetime

# Base configuration file(s) - can be a single file or list of files
# Each config will generate a separate set of experiments
BASE_CONFIG = [
    "../tests/Tiering_ZddrCxl_AggrSramCaches.cfg",
    "../tests/Tiering_ZddrCxl_WeakSramCaches.cfg",
    "../tests/Tiering_MessDdrCxl_WeakSramCaches.cfg",
    "../tests/Tiering_MessDdrCxl_AggrSramCaches.cfg",
]

# Alternative: Use a single config file (backward compatible)
# BASE_CONFIG = "../tests/Tiering_SimpleDdrCxl_WeakSramCaches.cfg"

# Output directory base (will append config name and timestamp)
OUTPUT_DIR = f'results/results_Migration_{datetime.now().strftime("%Y%m%d_%H%M%S")}'

# Warmup phase: fraction of simulation to ignore for metrics (0.0 to 1.0)
# Default: 0.2 (ignore first 20% of simulation for warmup)
WARMUP_PHASE = 0.2

# Custom metric calculators
# Each calculator receives (stats, warmup_idx) and returns a numeric value
# stats: h5py dataset from zsim.h5 (stats['stats']['root'])
# warmup_idx: index of first record after warmup

# ---------------------------------------------------------------------------
# Shared migrator-lookup helpers
# ---------------------------------------------------------------------------
# Old-style standalone migrators register stats under a field name that
# contains one of these substrings.
_STANDALONE_MIGRATOR_PATTERNS = [
    'PageMigrator', 'Pagemigrator', 'FreqBased', 'RobStallBased',
    'RobPressureWeighted', 'CriticalityWeighted', 'QLearning',
]
# HybridPageMigrator registers stats under its methodName (e.g. "mem.fsc+sbs").
# Normalise away + and _ for matching.
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
    """Return the migrator structured-array from mem stats, or None.

    Handles both:
    - Old-style standalone migrators (FreqBased, RobStallBased, …) whose stat
      group name contains a recognisable class-name substring.
    - HybridPageMigrator, which registers stats under its methodName
      (e.g. 'mem.fsc+sbs') — none of the class-name patterns match it.
    """
    for field_name in mem_data.dtype.names:
        # Standalone migrators
        for pat in _STANDALONE_MIGRATOR_PATTERNS:
            if pat in field_name:
                return mem_data[field_name]
        # HybridPageMigrator
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
        if migrator_data is None:
            return 0.0

        if 'numPageMigrations' not in migrator_data.dtype.names:
            return 0.0

        migrations_start = np.sum(migrator_data['numPageMigrations'][warmup_idx])
        migrations_end   = np.sum(migrator_data['numPageMigrations'][-1])
        total_migrations = migrations_end - migrations_start

        # numAccesses is in the PageMigrator base-class sub-group for hybrid
        # migrators. Use pageTrackerAccesses (same count, top-level) as the
        # preferred fallback, then fall back to total TieringMemory accesses.
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

        # Assume 4KB pages, 2GHz frequency
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
    """Average pages-with-stall-signal per migration interval (robStallBased methods).

    Uses the cumulative counter numStallPagesObserved: take the post-warmup delta
    and divide by the number of migration triggers in the same window.
    """
    try:
        mem_data = stats['mem']

        migrator_field = None
        for field_name in mem_data.dtype.names:
            if 'RobStallBased' in field_name:
                migrator_field = field_name
                break

        if migrator_field is None:
            return 0.0

        migrator_data = mem_data[migrator_field]

        if 'numStallPagesObserved' not in migrator_data.dtype.names:
            return 0.0

        stall_delta = (np.sum(migrator_data['numStallPagesObserved'][-1]) -
                       np.sum(migrator_data['numStallPagesObserved'][warmup_idx]))

        # Normalise by number of migration triggers in the post-warmup window.
        # numMigrationTriggers lives in the nested PageMigrator stat group.
        # Fall back to raw delta if we can't find it.
        try:
            # The nested group has the same name as the migrator field (zsim naming convention)
            inner = migrator_data[migrator_field] if migrator_field in migrator_data.dtype.names \
                    else migrator_data
            if 'numMigrationTriggers' in inner.dtype.names:
                triggers = (np.sum(inner['numMigrationTriggers'][-1]) -
                            np.sum(inner['numMigrationTriggers'][warmup_idx]))
                if triggers > 0:
                    return stall_delta / triggers
        except Exception:
            pass

        return float(stall_delta)
    except Exception:
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
        # uniquePages is a cumulative high-watermark; take the final value (not a delta)
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
        # New CXL-aware ideas
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

# =============================================================================
# New CXL-aware idea metrics (SVA / GKD / SCA / CCF)
# =============================================================================

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
    """Migration overhead cycles per CXL access (contribution of migration to CXL latency)."""
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
    'l3MissRate': calculate_l3MissRate,
    'llscMPKI':   calculate_llscMPKI,
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
    # Per-access overhead contributions (overhead / CXL accesses)
    'migrationOverheadPerCxlAccess':  calculate_migrationOverheadPerCxlAccess,
    'cxlBwCongestionPerAccess':       calculate_cxlBwCongestionPerAccess,
    'cxlHoLBlockingPerAccess':        calculate_cxlHoLBlockingPerAccess,
}

# Knobs to sweep
# Format: [
#   [knob_names],                    # List of parameter names to sweep together
#   [value_tuples],                  # List of value combinations (with optional plot flag)
#   [fixed_params],                  # Other parameters to set (but not sweep)
#   abbreviation                     # Short name for this sweep
# ]
# Value tuples can be:
#   ["value1", "value2"]             # Plot by default
#   ["value1", "value2", True]       # Explicitly plot
#   ["value1", "value2", False]      # Skip in plots (still runs, just not plotted)
KNOBS = [
    # Sweep migration policy and page size together - 1GB DRAM
    # [
    #     ["migrationPolicy", "robStallMetric"],              # Parameters to sweep
    #     [
    #         ["none", "", True],
    #         ["freqBased", "", True],
    #         ["practicalFreqBased", "", True],
    #         ["robStallBased", "robStallBinary", True],
    #         ["robStallBased", "robStallCycles", True],
    #         ["robStallBased", "robStallCount", True],
    #         ["robPressureWeighted", "", True],        # ROB pressure weighted
    #         ["criticalityWeighted", "", True],        # Criticality weighted
    #         ["qlearning", "", True],                  # Q-Learning
    #         # --- New hybrid methods (see docs/migration_ideas.md) ---
    #         ["fsc", "", True],                        # Frequency-Stall Composite
    #         ["sys", "", True],                        # Stall Yield Score
    #         ["sse", "", True],                        # Shadow Stall Estimation
    #         ["spfd", "", True],                       # Stall-Promote Frequency-Demote
    #         ["cema", "", True],                       # Cross-Interval EMA
    #         ["clcas", "", True],                      # CXL Link Congestion Amplified
    #         ["sbs", "", True],                        # Stability-Boosted Score
    #         # --- Combinations ---
    #         ["fsc+cema", "", True],                   # FSC + CEMA
    #         ["fsc+sbs", "", True],                    # FSC + SBS
    #         ["fsc+cema+sbs", "", True],               # Full Combination A (recommended)
    #         ["sse+spfd", "", True],                   # SSE + SPFD
    #         ["sse+spfd+clcas", "", True],             # Full Combination B (CXL-specific)
    #         ["sys+cema", "", True],                   # SYS + CEMA
    #     ],
    #     ["DRAMSizeMB = 1024L"],
    #     "Migration=%1_Metric=%2_Dram1GB"
    # ],

    # Sweep migration policy and page size together - 4GB DRAM
    # Third element in each value tuple is plot flag (True=plot, False=skip)
    [
        ["migrationPolicy", "robStallMetric"],              # Parameters to sweep
        [
            ["none", "", True],                              # Baseline - always plot
            ["freqBased", "", True],
            ["practicalFreqBased", "", True],
            ["robStallBased", "robStallBinary", True],
            ["robStallBased", "robStallCycles", True],
            ["robStallBased", "robStallCount", True],
            ["robPressureWeighted", "", True],               # ROB pressure weighted
            ["criticalityWeighted", "", True],               # Criticality weighted
            ["qlearning", "", True],                         # Q-Learning
            # --- New hybrid methods ---
            ["fsc", "", True],
            ["sys", "", True],
            ["sse", "", True],
            ["spfd", "", True],
            ["cema", "", True],
            ["clcas", "", True],
            ["sbs", "", True],
            ["fsc+cema", "", True],
            ["fsc+sbs", "", True],
            ["fsc+cema+sbs", "", True],
            ["sse+spfd", "", True],
            ["sse+spfd+clcas", "", True],
            ["sys+cema", "", True],
            # --- Count-based variants (stall event count instead of stall cycles) ---
            ["fsc_cnt",         "", True],    # FSC-Count: most robust ROB-stall signal
            ["fsc_cnt+sbs",     "", True],    # + stability bonus (expected to match/beat fsc+sbs)
            ["fsc_cnt+cema+sbs","", True],    # + EMA smoothing (full count combination)
            ["sse+spfd+cnt",    "", True],    # SSE+SPFD with count scoring
            # --- New CXL-aware ideas: standalone ---
            ["sva",             "", True],    # Stall Variance Aware (congestion-victim suppression)
            ["gkd",             "", True],    # Global Knee Detection (past-knee budget boost)
            ["sca",             "", True],    # Self-Competition Avoidance (congestion suppression)
            ["ccf",             "", True],    # Cross-Core Fan-out Weighting (multi-core stall boost)
            # --- FSC-Count + new ideas: primary combinations ---
            ["fsc_cnt+sva",          "", True],   # fsc_cnt + variance suppression
            ["fsc_cnt+ccf",          "", True],   # fsc_cnt + core fan-out
            ["fsc_cnt+sva+ccf",      "", True],   # fsc_cnt + variance + core fan-out
            ["fsc_cnt+sva+sbs",      "", True],   # fsc_cnt + variance + stability
            ["fsc_cnt+sva+ccf+sbs",  "", True],   # primary paper combo (variance + fan-out + stability)
            ["fsc_cnt+gkd+sca",      "", True],   # link-level combo (knee detect + suppression)
            ["fsc_cnt+sva+ccf+gkd+sca+sbs", "", True],  # full kitchen sink
            ],
        ["DRAMSizeMB = 4096L"],
        "Migration=%1_Metric=%2_Dram4GB"
        ],

    # Alpha sweep: find the best frequency-weight for fsc+sbs and fsc_cnt+sbs.
    # Higher alpha = more frequency, less stall signal (safer for noisy workloads).
    # Lower alpha = purer stall signal (better when stall is reliable).
    [
        ["migrationPolicy", "robStallMetric", "fscAlpha"],
        [
            ["fsc+sbs",     "", "0.0001", True],
            ["fsc+sbs",     "", "0.001",  True],   # default — matches main sweep
            ["fsc+sbs",     "", "0.005",  True],
            ["fsc+sbs",     "", "0.01",   True],
            ["fsc_cnt+sbs",         "", "0.0001", True],
            ["fsc_cnt+sbs",         "", "0.001",  True],   # default
            ["fsc_cnt+sbs",         "", "0.005",  True],
            ["fsc_cnt+sbs",         "", "0.01",   True],
            ["fsc_cnt+sva+ccf+sbs", "", "0.0001", True],
            ["fsc_cnt+sva+ccf+sbs", "", "0.001",  True],   # default
            ["fsc_cnt+sva+ccf+sbs", "", "0.005",  True],
            ["fsc_cnt+sva+ccf+sbs", "", "0.01",   True],
        ],
        ["DRAMSizeMB = 4096L"],
        "Migration=%1_Metric=%2_Alpha=%3_Dram4GB"
    ],
]

# =============================================================================
# WORKLOAD DEFINITIONS
# =============================================================================

BENCHMARKS_DIR = "/path/to/scratch/workloads"
BIN_DIR        = f"{BENCHMARKS_DIR}/bin"
MODELS_DIR     = f"{BENCHMARKS_DIR}/models"
DATASETS_DIR   = f"{BENCHMARKS_DIR}/datasets"
GRAPHS_DIR     = f"{BENCHMARKS_DIR}/graphs"
CONFIGS_DIR    = f"{BENCHMARKS_DIR}/configs"

WORKLOAD_GROUPS = {
    # Tuples: (command, name, has_roi, should_plot)
    # has_roi=True  → simulation starts in fast-forward mode (startFastForwarded = true)
    # has_roi=False → simulation starts in detailed mode   (startFastForwarded = false)
    # should_plot=True  → include in plots
    # should_plot=False → run but exclude from plots
    'ml': [
        # ML workloads: has_roi=False → startFastForwarded = false
        # Their thread pools are created BEFORE zsim_roi_begin(), so they already exist
        # in fast-forward. When ROI begins, only the calling thread would exit FF and the
        # pool threads would remain in FF (never detailed-simulated). Starting in detailed
        # mode from the very beginning ensures all 16 threads are simulated. The ROI markers
        # still serve as measurement region boundaries.
        (f"{BIN_DIR}/inference --model {MODELS_DIR}/resnet50-v2-7.onnx --batch 2048 --iterations 5000 --threads 16",   "resnet50",    False, True),
        (f"{BIN_DIR}/inference --model {MODELS_DIR}/vgg16-12.onnx --batch 1024 --iterations 5000 --threads 16",         "vgg16",       False, True),
        (f"{BIN_DIR}/inference --model {MODELS_DIR}/mobilenetv2-12.onnx --batch 4096 --iterations 10000 --threads 16",  "mobilenetv2", False, True),
        (f"{BIN_DIR}/inference --model {MODELS_DIR}/inception-v2-9.onnx --batch 2048 --iterations 5000 --threads 16",   "inception_v2",False, True),
        (f"{BIN_DIR}/inference --model {MODELS_DIR}/yolov4.onnx --batch 512 --iterations 5000 --threads 16",            "yolov4",      False, True),
        (f"{BIN_DIR}/lightgbm config={CONFIGS_DIR}/lightgbm_zsim.txt",                                                  "lightgbm",    False, True),
        (f"{BIN_DIR}/xgboost_train --data {DATASETS_DIR}/HIGGS.csv --max_depth 25 --num_rounds 2000 --nthread 16",      "xgboost",     False, True),
    ],
    'graph_twitter': [
        (f"{BIN_DIR}/bc   -f {GRAPHS_DIR}/twitter.sg       -n64",        "bc_twitter",   False, True),
        (f"{BIN_DIR}/bfs  -f {GRAPHS_DIR}/twitter.sg       -n64",        "bfs_twitter",  False, True),
        (f"{BIN_DIR}/cc   -f {GRAPHS_DIR}/twitter.sg       -n64",        "cc_twitter",   False, True),
        (f"{BIN_DIR}/pr   -f {GRAPHS_DIR}/twitter.sg       -n64",        "pr_twitter",   False, True),
        (f"{BIN_DIR}/sssp -f {GRAPHS_DIR}/twitter.wsg      -n64",        "sssp_twitter", False, True),
        (f"{BIN_DIR}/tc   -f {GRAPHS_DIR}/twitter_undir.sg -n64",        "tc_twitter",   False, True),
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
        (f"{BIN_DIR}/bc   -f {GRAPHS_DIR}/road.sg   -n64",         "bc_road",   False, True),
        (f"{BIN_DIR}/bfs  -f {GRAPHS_DIR}/road.sg   -n64",         "bfs_road",  False, True),
        (f"{BIN_DIR}/cc   -f {GRAPHS_DIR}/road.sg   -n64",         "cc_road",   False, True),
        (f"{BIN_DIR}/pr   -f {GRAPHS_DIR}/road.sg   -n64",         "pr_road",   False, True),
        (f"{BIN_DIR}/sssp -f {GRAPHS_DIR}/road.wsg  -n64 -d50000", "sssp_road", False, True),
        (f"{BIN_DIR}/tc   -f {GRAPHS_DIR}/roadU.sg  -n64",         "tc_road",   False, True),
    ],
   # 'graph_kron': [
   #     (f"{BIN_DIR}/bc   -f {GRAPHS_DIR}/kron.sg   -n64", "bc_kron",  True),
   #     (f"{BIN_DIR}/bfs  -f {GRAPHS_DIR}/kron.sg   -n64", "bfs_kron", True),
   #     (f"{BIN_DIR}/cc   -f {GRAPHS_DIR}/kron.sg   -n64", "cc_kron",  True),
   #     (f"{BIN_DIR}/pr   -f {GRAPHS_DIR}/kron.sg   -n64", "pr_kron",  True),
   #     (f"{BIN_DIR}/tc   -f {GRAPHS_DIR}/kronU.sg  -n64", "tc_kron",  True),
   # ],
   # 'graph_urand': [
   #     (f"{BIN_DIR}/bc   -f {GRAPHS_DIR}/urand.sg   -n64", "bc_urand",  True),
   #     (f"{BIN_DIR}/bfs  -f {GRAPHS_DIR}/urand.sg   -n64", "bfs_urand", True),
   #     (f"{BIN_DIR}/cc   -f {GRAPHS_DIR}/urand.sg   -n64", "cc_urand",  True),
   #     (f"{BIN_DIR}/pr   -f {GRAPHS_DIR}/urand.sg   -n64", "pr_urand",  True),
   #     (f"{BIN_DIR}/tc   -f {GRAPHS_DIR}/urandU.sg  -n64", "tc_urand",  True),
   # ],
}

# Workloads to run (optional - if not specified, uses command from base config)
# Each workload will be combined with all KNOBS configurations.
# Tuple format: (cmd, name, has_roi, should_plot)
# Fourth element (should_plot) controls whether this workload appears in plots
# Aggregate all groups into a single flat list (deduplication preserves order)
_seen = set()
WORKLOADS = [
    (cmd, name, has_roi, should_plot)
    for group in WORKLOAD_GROUPS.values()
    for cmd, name, has_roi, should_plot in group
    if name not in _seen and not _seen.add(name)
]

# Fixed knobs that apply to ALL experiments (global settings)
GLOBAL_KNOBS = [
    "maxTotalInstrs = 100000000000L",
    "gmMBytes = 98304",
    # "maxTotalInstrs = 10000000L",
    # "gmMBytes = 32000",
]

# =============================================================================
# What to measure/plot
# =============================================================================
# Format: List of tuples (metric_name, display_mode)
# Display modes: "absolute", "normalized", "percentage"
#   - "absolute": Show raw values
#   - "normalized": Normalize to first configuration (baseline = 1.0)
#   - "percentage": Show as percentage (multiply by 100, add %)

# Default configuration: Performance overview
METRICS = [
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
     # Q-learning specific
     ("qlearningReward", "absolute"),
     ("qlearningAvgQValue", "absolute"),
     ("qlearningExplorationRate", "percentage"),
     # Hybrid methods (fsc, sys, sse, spfd, cema, clcas, sbs and combinations)
     ("hybridAvgEmaStall", "absolute"),
     ("hybridAvgStabilityBonus", "absolute"),
     ("hybridAvgShadowYield", "absolute"),
     # Workload characterization (policy-independent, use "none" baseline)
     ("llscMPKI",          "absolute"),
     ("memoryFootprintMB", "absolute"),
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
