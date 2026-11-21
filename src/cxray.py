#!/usr/bin/env python3
"""
CXRay: Fast, Full-Featured CXL Simulator
Trace-driven simulation with CXL 2.0 through 4.0 feature modeling.

Features modeled:
  1. PCIe credit-pool back-pressure (M2S_REQ + M2S_RWD independent pools,
     CXL 3.1 §4.2.3)
  2. CXL switch / fabric latency (HBR cut-through, PBR; per-hop additive,
     both forward and return paths)
  3. Multi-host GIM arbitration (CXL 3.0+: per-request fabric arbitration
     overhead when num_devices > 1)
  4. CXL.cache Type 2 device (DCOH snoop-filter lookup overhead, + Back-
     Invalidation round-trip on host write requests)
  5. RAS layer (patrol scrub, sPPR, hPPR rank-block, viral, data poison)
  6. IDE per-access encryption overhead + TSP session setup / key rotation
  7. FEC decode latency (PCIe Gen6/7 RS-FEC)
  8. CXL 4.0 Bundled Port (PCIe Gen7 x8, bandwidth aggregation over N links)

Usage:
    ./cxray.py --config configs/cxl_3.1.cfg --trace trace.trace \\
               --output results/
"""

import sys, os, re, json, math, time, argparse, random
import heapq
from collections import defaultdict, deque
from datetime import datetime, timezone

__version__ = "0.2.0"

# ---------------------------------------------------------------------------
# Config parser — handles dotted keys (link.pcie_gen = 5) and nested blocks
# ---------------------------------------------------------------------------

def _expand_dotted_keys(flat):
    """{"a.b.c": v, "x.y": w} → {"a": {"b": {"c": v}}, "x": {"y": w}}."""
    result = {}
    for key, val in flat.items():
        parts = key.split(".")
        d = result
        for part in parts[:-1]:
            if part not in d or not isinstance(d[part], dict):
                d[part] = {}
            d = d[part]
        last = parts[-1]
        if isinstance(val, dict) and isinstance(d.get(last), dict):
            # Merge nested dicts rather than overwrite
            def _deep_merge(dst, src):
                for k, v in src.items():
                    if isinstance(v, dict) and isinstance(dst.get(k), dict):
                        _deep_merge(dst[k], v)
                    else:
                        dst[k] = v
            _deep_merge(d[last], val)
        else:
            d[last] = val
    return result


def parse_libconfig(text):
    """Parse libconfig-style config (dotted keys + curly-brace blocks)."""
    text = re.sub(r'#[^\n]*', '', text)
    text = re.sub(r'//[^\n]*', '', text)

    def _parse_block(tokens, pos):
        d = {}
        while pos < len(tokens):
            tok = tokens[pos]
            if tok == '}':
                return d, pos + 1
            key = tok; pos += 1
            if pos >= len(tokens) or tokens[pos] != '=':
                continue
            pos += 1
            val, pos = _parse_value(tokens, pos)
            if pos < len(tokens) and tokens[pos] == ';':
                pos += 1
            d[key] = val
        return d, pos

    def _parse_value(tokens, pos):
        tok = tokens[pos]
        if tok == '{':
            inner, pos = _parse_block(tokens, pos + 1)
            return inner, pos
        elif tok.startswith('"'):
            return tok[1:-1], pos + 1
        elif tok.lower() == 'true':
            return True, pos + 1
        elif tok.lower() == 'false':
            return False, pos + 1
        else:
            try:
                return (float(tok) if ('.' in tok or 'e' in tok.lower())
                        else int(tok)), pos + 1
            except ValueError:
                return tok, pos + 1

    token_re = re.compile(r'"[^"]*"|[{}=;]|[^\s{}=;"]+')
    tokens = token_re.findall(text)
    flat, _ = _parse_block(tokens, 0)
    return _expand_dotted_keys(flat)


def get(cfg, *path, default=None):
    cur = cfg
    for key in path:
        if not isinstance(cur, dict) or key not in cur:
            return default
        cur = cur[key]
    return cur


def merge_defaults(user_cfg, defaults):
    result = {}
    all_keys = set(defaults.keys()) | set(user_cfg.keys() if user_cfg else [])
    for k in all_keys:
        dval = defaults.get(k)
        uval = user_cfg.get(k) if user_cfg else None
        if isinstance(dval, dict):
            result[k] = merge_defaults(uval if isinstance(uval, dict) else {}, dval)
        else:
            result[k] = uval if uval is not None else dval
    return result


# ---------------------------------------------------------------------------
# Default configuration (CXL 2.0 / Gen5×8 defaults)
# ---------------------------------------------------------------------------

DEFAULTS = {
    # Top-level keys (match config file format)
    "cxl_version":      "2.0",
    "host_overhead_ns": 97,     # host CXL controller TX processing (ns)
    "has_switch":       False,

    "link": {
        "pcie_gen":               5,
        "pcie_lanes":             8,
        "host_port_latency_ns":   15,   # host-side port serialisation (ns)
        "link_phys_latency_ns":   3,    # physical propagation (ns)
        "device_port_latency_ns": 12,   # device-side port deserialization (ns)
        "flit_size_bytes":        66,
        "flit_payload_bytes":     64,
        "fec_enable":             False,
        "fec_decode_latency_ns":  0.0,
        "vc_arbitration_ns":      1.0,
        "credits": {
            "m2s_req": 32,   # M2S_REQ outstanding read credits
            "m2s_rwd": 16,   # M2S_RWD outstanding write credits
            "s2m_ndr": 32,
            "s2m_drs": 32,
        },
    },

    "switch": {
        "mode":                 "pbr",
        "latency_ns":           40,    # per-hop cut-through latency (ns)
        "num_hops":             1,
        "queue_capacity_flits": 32,
    },

    "device": {
        "type":                  3,     # 3=Type3 memory, 2=Type2 accel+HDM
        "controller_latency_ns": 20,
        "dram_type":             "ddr5_4800",
        "coherence_overhead_ns": 0.0,  # DCOH snoop-filter lookup (Type 2)
        "ide_enable":            False,
        "ide_latency_ns":        0.0,  # per-direction IDE overhead (ns)
        "bi_enable":             False,
        "bi_rtt_ns":             0.0,  # Back-Invalidation RTT on writes
        "tsp_enable":            False,
        "tsp_key_rotation_mb":   64,
    },

    "dram": {
        "technology":              "DDR5-4800",
        "tCL_ns":                  16.67,
        "tRCD_ns":                 16.67,
        "tRP_ns":                  16.67,
        "row_hit_latency_ns":      16.67,
        "row_miss_latency_ns":     50.0,
        "row_conflict_latency_ns": 66.7,
        "tWR_ns":                  15.0,   # DDR5-4800 tWR: ~15 ns gives write bank cap ~25 GB/s matching CZ120 hardware
        "num_channels":            1,
        "num_ranks":               2,
        "num_banks":               16,
        "num_rows_per_bank":       65536,
    },

    "fabric": {
        "p2p_enable":        False,
        "gim_enable":        False,
        "gim_num_devices":   1,
        "gim_interleave_gran": 256,
        "gim_arbitration_ns":  10.0,  # per-request fabric arbitration (ns)
    },

    "multihost": {
        "num_hosts":        1,      # number of hosts sharing this switch port
        "per_host_credits": 16,     # M2S_REQ credits per host (reduced from 32)
    },

    "ras": {
        "enable":                   True,
        "scrub_interval_us":        100,
        "patrol_scrub_bw_fraction": 0.05,
        "sppr_enable":              False,
        "hppr_enable":              False,
        "data_poison":              False,
        "viral":                    False,
        "dbit_enable":              False,
        "cvme_threshold":           256,
        "cvme_window_us":           1000,
        "ppr_latency_us":           200,
        "ppr_max_repairs":          64,
        "error_injection": {
            "enable":             False,
            "correctable_rate":   0.0,
            "uncorrectable_rate": 0.0,
            "injection_mode":     "random",
        },
    },

    "sim": {
        "mode":                   "fast",
        "seed":                   42,
        "max_outstanding_requests": 256,
        "stats_window_ns":        1000,
    },
}


def compute_derived(cfg):
    """Compute all derived parameters from resolved config."""
    link   = cfg["link"]
    sw     = cfg["switch"]
    dev    = cfg["device"]
    fab    = cfg["fabric"]

    # --- Link bandwidth ---
    gen, lanes = link["pcie_gen"], link["pcie_lanes"]
    speed_GTs  = {7: 128.0, 6: 64.0}.get(gen, 32.0)
    efficiency = 0.85 if gen >= 6 else 0.98   # PAM4 (Gen6/7) vs NRZ 128b/130b (Gen5)
    bw_GBps    = lanes * speed_GTs * efficiency / 8.0

    # Bundled Port (CXL 4.0): aggregate bandwidth of N physical links
    bp          = cfg.get("bundled_port", {})
    bp_enable   = bool(bp.get("enable", False))
    bp_ports    = max(1, int(bp.get("num_ports", 1)))
    if bp_enable and bp_ports > 1:
        bw_GBps *= bp_ports

    # --- One-way link latency (physical) ---
    link_ns = (link["host_port_latency_ns"]
               + link["link_phys_latency_ns"]
               + link["device_port_latency_ns"])

    # --- FEC decode latency (Gen6 RS-FEC, both directions) ---
    fec_ns = link["fec_decode_latency_ns"] if link["fec_enable"] else 0.0

    # --- IDE per-direction overhead ---
    ide_ns = dev["ide_latency_ns"] if dev["ide_enable"] else 0.0

    # --- Switch latency (per hop, symmetric fwd/ret) ---
    has_sw     = bool(cfg.get("has_switch", False))
    sw_hop_ns  = float(sw["latency_ns"])
    sw_hops    = int(sw["num_hops"])
    switch_fwd_ns = (sw_hop_ns * sw_hops) if has_sw else 0.0
    switch_ret_ns = switch_fwd_ns

    # --- Device-side coherence overhead (Type 2 DCOH, forward path only) ---
    is_type2  = (int(dev.get("type", 3)) == 2)
    dcoh_ns   = float(dev["coherence_overhead_ns"]) if is_type2 else 0.0

    # --- GIM arbitration (forward path only) ---
    gim_en    = bool(fab["gim_enable"])
    gim_ns    = float(fab["gim_arbitration_ns"]) if gim_en else 0.0
    gim_devs  = int(fab["gim_num_devices"]) if gim_en else 1

    # --- Back-Invalidation (Type 2, write requests, appended to return) ---
    bi_rtt_ns = float(dev["bi_rtt_ns"]) if (is_type2 and dev["bi_enable"]) else 0.0

    # --- Total fixed forward overhead (excluding credit stall and BW queuing):
    #   host_overhead + link propagation + FEC fwd + IDE fwd
    #   + switch fwd + DCOH + GIM arbitration
    host_ns = float(cfg.get("host_overhead_ns", 97))
    fwd_ns = host_ns + link_ns + fec_ns + ide_ns + switch_fwd_ns + dcoh_ns + gim_ns

    # --- Total fixed return overhead:
    #   link propagation + FEC ret + IDE decrypt + switch ret
    ret_ns = link_ns + fec_ns + ide_ns + switch_ret_ns

    # --- RTT for credit-pool: round-trip without DRAM (≈ 2×(host+link)) ---
    rtt_ns = 2.0 * (host_ns + link_ns)

    # --- Flit transmission time (BW enforcement) ---
    # Use full wire flit size (header + payload) — wire is occupied for entire flit.
    flit_payload = link["flit_payload_bytes"]
    flit_size    = link.get("flit_size_bytes", flit_payload)
    flit_tx_ns   = flit_size / bw_GBps   # ns to transmit one flit on wire

    cfg["derived"] = {
        "link_bandwidth_GBps": round(bw_GBps, 3),
        "link_one_way_ns":     round(link_ns, 2),
        "fwd_ns":              round(fwd_ns, 2),
        "ret_ns":              round(ret_ns, 2),
        "rtt_ns":              round(rtt_ns, 2),
        "flit_tx_ns":          round(flit_tx_ns, 4),
        "is_type2":            is_type2,
        "bi_rtt_ns":           bi_rtt_ns,
        "switch_fwd_ns":       switch_fwd_ns,
        "switch_ret_ns":       switch_ret_ns,
        "gim_enabled":         gim_en,
        "gim_num_devices":     gim_devs,
        "has_switch":          has_sw,
        "dcoh_ns":             dcoh_ns,
        "fec_ns":              fec_ns,
        "ide_ns":              ide_ns,
        "bundled_port_enable": bp_enable,
        "bundled_port_num_ports": bp_ports,
    }

    # Multi-host
    num_hosts = get(cfg, "multihost", "num_hosts", default=1)
    cfg.setdefault("derived", {})["num_hosts"] = int(num_hosts)

    return cfg


def resolve_config(user_cfg):
    cfg = merge_defaults(user_cfg, DEFAULTS)
    return compute_derived(cfg)


# ---------------------------------------------------------------------------
# Trace reader
# ---------------------------------------------------------------------------

def parse_trace_line(line):
    line = line.strip()
    if not line or line.startswith('#'):
        return None
    parts = line.split()
    if len(parts) < 5:
        return None
    req = {
        "timestamp_ns": int(parts[0]),
        "op":           parts[1].upper(),
        "addr":         int(parts[2], 16),
        "size":         int(parts[3]),
        "requester_id": int(parts[4]),
        "flags":        set(parts[5].split(',')) if len(parts) > 5 else set(),
    }
    if req["size"] > 64:
        reqs = []
        for i in range(req["size"] // 64):
            r = dict(req)
            r["addr"] = req["addr"] + i * 64
            r["size"] = 64
            reqs.append(r)
        return reqs
    return req


def read_trace(trace_file):
    requests = []
    with open(trace_file) as f:
        for line in f:
            result = parse_trace_line(line)
            if result is None:
                continue
            if isinstance(result, list):
                requests.extend(result)
            else:
                requests.append(result)
    requests.sort(key=lambda r: r["timestamp_ns"])
    return requests


def load_multi_traces(trace_paths):
    """
    Load one trace per host.  Each request gets:
      host_id      = file index (0-based host number)
      requester_id = preserved from the trace (local per-host requester, 0-based)
    Both fields are used downstream: host_id routes to the correct per-host
    upstream link and credit pool; requester_id is used for per-requester stats.
    """
    host_traces = []
    for host_id, path in enumerate(trace_paths):
        reqs = read_trace(path)
        for r in reqs:
            r["host_id"] = host_id
            # requester_id left as-is (local to host)
        host_traces.append(reqs)
    return host_traces


def merge_traces(host_traces):
    """
    Merge H independently-sorted request lists into a single timestamp-ordered
    iterator using heapq.merge for O(R log H) cost.
    Tie-break by host_id so the merge is deterministic.
    """
    tagged = (
        ((r["timestamp_ns"], host_id, r) for r in trace)
        for host_id, trace in enumerate(host_traces)
    )
    for _ts, _hid, req in heapq.merge(*tagged, key=lambda x: (x[0], x[1])):
        yield req


# ---------------------------------------------------------------------------
# Analytical DRAM backend
# ---------------------------------------------------------------------------

class AnalyticalDRAMBackend:
    """
    Analytical DRAM model.
    Tracks open rows per bank for row-hit/miss/conflict classification.
    """

    def __init__(self, cfg):
        dc   = cfg["dram"]
        dev  = cfg["device"]
        self.num_banks  = (dev.get("num_channels", cfg["dram"].get("num_channels", 1))
                           * dev.get("num_ranks", cfg["dram"].get("num_ranks", 2))
                           * dev.get("num_banks", cfg["dram"].get("num_banks", 16)))
        self.num_rows   = dev.get("num_rows_per_bank",
                                  cfg["dram"].get("num_rows_per_bank", 65536))
        self.t_hit      = dc["row_hit_latency_ns"]
        self.t_miss     = dc["row_miss_latency_ns"]
        self.t_conflict = dc["row_conflict_latency_ns"]

        self.open_row   = {}   # bank_id → open_row_id
        self.row_hits   = 0
        self.row_misses = 0
        self.row_conflicts = 0

    def _addr_to_bank_row(self, addr):
        cl   = addr >> 6
        bank = cl % self.num_banks
        row  = (cl // self.num_banks) % self.num_rows
        return bank, row

    def access(self, addr, is_write, current_ns):
        bank, row = self._addr_to_bank_row(addr)
        open_row  = self.open_row.get(bank)
        if open_row == row:
            self.row_hits += 1
            lat, ht = self.t_hit, "hit"
        elif open_row is None:
            self.row_misses += 1
            lat, ht = self.t_miss, "miss"
            self.open_row[bank] = row
        else:
            self.row_conflicts += 1
            lat, ht = self.t_conflict, "conflict"
            self.open_row[bank] = row
        return lat, ht

    def stats(self):
        total = self.row_hits + self.row_misses + self.row_conflicts
        return {
            "row_buffer_hits":      self.row_hits,
            "row_buffer_misses":    self.row_misses,
            "row_buffer_conflicts": self.row_conflicts,
            "row_buffer_hit_rate":  self.row_hits / total if total > 0 else 0.0,
        }




# ---------------------------------------------------------------------------
# Arrival-order DRAM backend with per-bank queuing
# ---------------------------------------------------------------------------

class QueuedFRFCFSBackend:
    """
    Arrival-order DRAM backend with per-bank queuing (event-driven).

    Unlike AnalyticalDRAMBackend, this serializes requests to the same bank:
    if a bank is busy when a new request arrives, the request waits until the
    bank is free.  This models bank-level contention correctly and enables
    accurate GIM per-device parallelism demonstration.

    The scheduling rule per bank:
      - Row hit:      wait for bank to be free, then CAS (t_hit)
      - Row miss:     wait for bank to be free, then ACT+CAS (t_miss)
      - Row conflict: wait for bank to be free, then PRE+ACT+CAS (t_conflict)

    tRRD_L (min ACT-to-ACT in same rank, different banks) is also enforced.
    """

    def __init__(self, cfg):
        dc  = cfg["dram"]
        dev = cfg.get("device", {})
        num_ch    = dc.get("num_channels", 1)
        num_ranks = dc.get("num_ranks", 2)
        num_bprank = dc.get("num_banks", 16)           # banks per rank
        self.num_banks      = num_ch * num_ranks * num_bprank
        self.num_ranks_tot  = num_ch * num_ranks
        self.num_bprank     = num_bprank
        self.num_rows       = dc.get("num_rows_per_bank", 65536)

        self.t_hit      = dc["row_hit_latency_ns"]
        self.t_miss     = dc["row_miss_latency_ns"]
        self.t_conflict = dc["row_conflict_latency_ns"]
        # tRP component (precharge time before ACT on conflict)
        self.t_rp       = self.t_conflict - self.t_miss
        # tRRD_L: min time between two ACTs to same rank (DDR5-4800 ~ 5 ns)
        self.t_rrd_l    = dc.get("tRRD_L_ns", 5.0)
        # tWR: write recovery — bank stays locked after write burst before PRE.
        # DDR5-4800 tWR=30 ns reduces write bank cap to ~21 GB/s (vs read ~31 GB/s).
        self.t_wr       = dc.get("tWR_ns", 30.0)

        # Per-bank state
        self.open_row   = [-1]    * self.num_banks   # -1 = precharged
        self.bank_free  = [0.0]   * self.num_banks   # earliest next command

        # Per-rank last-ACT time (for tRRD_L)
        self.rank_last_act = [0.0] * self.num_ranks_tot

        # Stats
        self.row_hits      = 0
        self.row_misses    = 0
        self.row_conflicts = 0

    # -- address mapping
    def _addr_to_bank_row(self, addr):
        cl   = addr >> 6                        # cacheline index
        bank = cl % self.num_banks
        row  = (cl // self.num_banks) % self.num_rows
        rank = bank // self.num_bprank
        return bank, row, rank

    # -- main access method (called in timestamp order)
    def access(self, addr, is_write, current_ns):
        bank, row, rank = self._addr_to_bank_row(addr)

        open_row  = self.open_row[bank]
        free_at   = self.bank_free[bank]
        last_act  = self.rank_last_act[rank]

        if open_row == row:
            # Row hit: just need CAS (no ACT required)
            t_cas    = max(current_ns, free_at)
            complete = t_cas + self.t_hit
            # tWR: after a write burst the bank is locked until PRE is legal
            self.bank_free[bank] = complete + (self.t_wr if is_write else 0.0)
            self.row_hits += 1
            ht = "hit"
        elif open_row == -1:
            # Row miss (bank precharged): ACT + CAS
            # Enforce tRRD_L from previous ACT in this rank
            t_act    = max(current_ns, free_at, last_act + self.t_rrd_l)
            complete = t_act + self.t_miss
            self.bank_free[bank]      = complete + (self.t_wr if is_write else 0.0)
            self.rank_last_act[rank]  = t_act
            self.open_row[bank]       = row
            self.row_misses += 1
            ht = "miss"
        else:
            # Row conflict: PRE + ACT + CAS
            t_pre  = max(current_ns, free_at)
            t_act  = max(t_pre + self.t_rp, last_act + self.t_rrd_l)
            complete = t_act + self.t_miss
            self.bank_free[bank]     = complete + (self.t_wr if is_write else 0.0)
            self.rank_last_act[rank] = t_act
            self.open_row[bank]      = row
            self.row_conflicts += 1
            ht = "conflict"

        actual_lat = complete - current_ns
        return actual_lat, ht

    def stats(self):
        total = self.row_hits + self.row_misses + self.row_conflicts
        return {
            "row_buffer_hits":      self.row_hits,
            "row_buffer_misses":    self.row_misses,
            "row_buffer_conflicts": self.row_conflicts,
            "row_buffer_hit_rate":  self.row_hits / total if total > 0 else 0.0,
        }
# ---------------------------------------------------------------------------
# PCIe credit pool (CXL 3.1 §4.2.3)
# ---------------------------------------------------------------------------

class CreditPool:
    """
    Sliding-window credit pool for one M2S virtual channel (CXL 3.1 §4.2.3).

    Each outstanding request consumes one credit. A credit is returned when
    the full round-trip completes (S2M NDR/DRS received at host). Credits are
    therefore tracked using ACTUAL completion times, not an estimated RTT.

    Usage pattern (must be called in timestamp order):
        stall = pool.acquire(t_arrive)   # check & stall; does NOT push yet
        ... compute complete_ns ...
        pool.release(complete_ns)        # record when this credit returns
    """

    def __init__(self, limit):
        self.limit   = int(limit)   # 0 = pool disabled
        self._window = []           # min-heap of credit-return timestamps
        # NOTE: must be a min-heap (heapq) because requests can complete out of
        # processing order (e.g., two ranks with independent tRRD_L cascades).
        # Using a sorted deque would cause credits to never expire when a late-
        # completing request is released before an already-returned one.

        self.total_stall_ns = 0.0
        self.total_requests = 0
        self.stall_events   = 0

    def acquire(self, req_ns):
        """
        Check credit availability at req_ns.
        Returns stall_ns (0 when pool is not exhausted or disabled).
        Caller MUST call release(complete_ns) afterwards.
        """
        if self.limit == 0:
            return 0.0

        self.total_requests += 1

        # Expire credits that have already returned (complete_ns ≤ req_ns)
        while self._window and self._window[0] <= req_ns:
            heapq.heappop(self._window)

        # Stall if pool exhausted: wait until oldest in-flight credit returns
        stall = 0.0
        if len(self._window) >= self.limit:
            unblock_at = heapq.heappop(self._window)
            if unblock_at > req_ns:
                stall = unblock_at - req_ns
            self.total_stall_ns += stall
            self.stall_events   += 1

        return stall

    def release(self, complete_ns):
        """
        Record that the credit issued for this request returns at complete_ns.
        Uses heappush so the pool is always a min-heap regardless of completion order.
        """
        if self.limit == 0:
            return
        heapq.heappush(self._window, complete_ns)

    def stats(self):
        return {
            "limit":              self.limit,
            "total_requests":     self.total_requests,
            "stall_events":       self.stall_events,
            "total_stall_ns":     round(self.total_stall_ns, 2),
            "avg_stall_per_req":  round(self.total_stall_ns / max(1, self.total_requests), 4),
        }



# ---------------------------------------------------------------------------
# Multi-host credit pools (CXL 3.x multi-tenant fabric)
# ---------------------------------------------------------------------------

class MultiHostCreditPools:
    """
    Per-host independent credit pools for multi-host CXL switch sharing.
    Each host has its own M2S_REQ pool; the shared link is serialized by
    the existing LinkBW enforcer.
    """
    def __init__(self, num_hosts, per_host_limit):
        self.pools = [CreditPool(per_host_limit) for _ in range(num_hosts)]

    def acquire(self, host_id, req_ns):
        return self.pools[host_id % len(self.pools)].acquire(req_ns)

    def release(self, host_id, complete_ns):
        self.pools[host_id % len(self.pools)].release(complete_ns)

    def stats(self):
        return [p.stats() for p in self.pools]


# ---------------------------------------------------------------------------
# Link bandwidth enforcer
# ---------------------------------------------------------------------------

class LinkBW:
    """
    Enforces link bandwidth serialization for flit transmission.
    Separate tracks for forward (host→device) and return (device→host).
    Does NOT include propagation latency (that is part of fixed overhead).
    """

    def __init__(self, bw_GBps, flit_payload_bytes, flit_size_bytes=None,
                 ret_backlog_cap_flits=0):
        self.bw_GBps       = bw_GBps
        self.flit_payload  = flit_payload_bytes
        # Wire serialization time uses full flit size (header + payload occupies the wire).
        # Falls back to payload size when flit_size is not provided (backward compat).
        wire_bytes = flit_size_bytes if flit_size_bytes else flit_payload_bytes
        self.flit_tx_ns    = wire_bytes / bw_GBps  # ns per flit on wire
        # Return-path backlog cap (0 = uncapped). Non-zero limits how far ahead
        # the serializer can run to prevent numerical runaway in pathological configs.
        self.ret_backlog_cap_flits = ret_backlog_cap_flits

        # flit_wire_bytes: wire-occupancy per flit (full flit including header+overhead).
        # Used for utilization accounting to match C++ wire-byte counting.
        self.flit_wire_bytes = flit_size_bytes if flit_size_bytes else flit_payload_bytes

        self._fwd_busy_until = 0.0  # earliest time next flit can start transmitting
        self._ret_busy_until = 0.0

        self.total_fwd_bytes = 0   # wire-bytes (num_flits × flit_wire_bytes) for utilization
        self.total_ret_bytes = 0

    def enqueue_fwd(self, t_ready_ns, size_bytes):
        """
        Enforce BW on forward path.
        Returns the time at which the last bit of the flit train finishes
        transmitting (flit completion, not start).  Caller adds link_ns to
        get the time the request arrives at the far-end receiver.
        """
        num_flits     = math.ceil(size_bytes / self.flit_payload)
        start         = max(t_ready_ns, self._fwd_busy_until)
        tx_dur        = num_flits * self.flit_tx_ns
        self._fwd_busy_until = start + tx_dur
        self.total_fwd_bytes += num_flits * self.flit_wire_bytes
        return self._fwd_busy_until  # flit completion time; caller adds link propagation

    def enqueue_ret(self, t_ready_ns, size_bytes):
        """Enforce BW on return path. Returns flit completion time.

        ret_backlog_cap_flits controls how far ahead the serializer may run.
        Default 0 = uncapped (correct for multi-host and tail studies).
        A non-zero value acts as a numerical-stability guard for single-host
        runs under very light load where floating-point drift can accumulate;
        set via link.ret_backlog_cap_flits in the config.
        """
        num_flits = math.ceil(size_bytes / self.flit_payload)
        tx_dur    = num_flits * self.flit_tx_ns
        if self.ret_backlog_cap_flits > 0:
            max_backlog_ns = self.ret_backlog_cap_flits * self.flit_tx_ns
            self._ret_busy_until = min(self._ret_busy_until,
                                       t_ready_ns + max_backlog_ns)
        start = max(t_ready_ns, self._ret_busy_until)
        self._ret_busy_until = start + tx_dur
        self.total_ret_bytes += num_flits * self.flit_wire_bytes
        return self._ret_busy_until  # flit completion time; caller adds link propagation

    def utilization(self, total_sim_ns):
        if total_sim_ns <= 0:
            return 0.0
        active_ns = self.total_fwd_bytes / self.bw_GBps
        return min(1.0, active_ns / total_sim_ns)


# ---------------------------------------------------------------------------
# RAS engine
# ---------------------------------------------------------------------------

class RASEngine:
    """
    CXL 3.1 RAS: patrol scrub contention, sPPR bank-lock, hPPR rank-block,
    data poison, viral signaling.
    """

    def __init__(self, cfg, dram_backend):
        rc = cfg["ras"]
        self.enabled = rc["enable"]
        if not self.enabled:
            return

        self.dram             = dram_backend
        self.scrub_interval_ns = rc["scrub_interval_us"] * 1000
        self.scrub_bw_frac    = rc["patrol_scrub_bw_fraction"]
        self.cvme_threshold   = rc["cvme_threshold"]
        self.cvme_window_ns   = rc["cvme_window_us"] * 1000
        self.ppr_latency_ns   = rc["ppr_latency_us"] * 1000
        self.ppr_enabled      = rc["sppr_enable"] or rc["hppr_enable"]
        self.poison_enabled   = rc["data_poison"]
        self.viral_enabled    = rc["viral"]

        ei = rc["error_injection"]
        self.inject_enabled = ei["enable"]
        self.ce_rate        = ei["correctable_rate"]
        self.uce_rate       = ei["uncorrectable_rate"]

        self.ce_count         = 0
        self.uce_count        = 0
        self.ce_timestamps    = deque()
        self.poisoned_addrs   = set()
        self.viral_active     = False
        self.viral_start_ns   = None
        self.ppr_events       = []
        self.ppr_busy_until_ns = 0

        self.error_log  = []
        self.ppr_log    = []
        self.poison_log = []
        self.viral_log  = []

        self.rng = random.Random(42)

    def process(self, req, dispatch_ns):
        if not self.enabled:
            return 0.0
        extra_ns = 0.0

        if self.inject_enabled:
            if self.rng.random() < self.ce_rate:
                self._inject_ce(req["addr"], dispatch_ns)
            if self.rng.random() < self.uce_rate:
                self._inject_uce(req["addr"], dispatch_ns)

        cacheline = req["addr"] >> 6 << 6
        if self.poison_enabled and cacheline in self.poisoned_addrs:
            self.poison_log.append({"time_ns": dispatch_ns, "addr": hex(req["addr"]),
                                    "event_type": "poison_read"})

        if self.ppr_enabled and dispatch_ns < self.ppr_busy_until_ns:
            extra_ns += self.ppr_busy_until_ns - dispatch_ns

        extra_ns += self._scrub_overhead(dispatch_ns, req["size"])

        if self.viral_active:
            extra_ns += 10.0  # viral stall per request

        return extra_ns

    def _inject_ce(self, addr, time_ns):
        self.ce_count += 1
        self.ce_timestamps.append(time_ns)
        self.error_log.append({"time_ns": time_ns, "addr": hex(addr),
                                "error_type": "CE", "corrected": True})
        cutoff = time_ns - self.cvme_window_ns
        while self.ce_timestamps and self.ce_timestamps[0] < cutoff:
            self.ce_timestamps.popleft()
        if len(self.ce_timestamps) >= self.cvme_threshold:
            self._trigger_cvme(addr, time_ns)

    def _inject_uce(self, addr, time_ns):
        self.uce_count += 1
        cl = addr >> 6 << 6
        if self.poison_enabled:
            self.poisoned_addrs.add(cl)
            self.poison_log.append({"time_ns": time_ns, "addr": hex(addr),
                                    "event_type": "poison_inject"})
        self.error_log.append({"time_ns": time_ns, "addr": hex(addr),
                               "error_type": "UCE", "corrected": False})
        if self.viral_enabled:
            self._trigger_viral(time_ns)

    def _trigger_cvme(self, addr, time_ns):
        if self.ppr_enabled and self.ppr_busy_until_ns <= time_ns:
            row_addr = (addr >> 6) // self.dram.num_banks
            self.ppr_busy_until_ns = time_ns + self.ppr_latency_ns
            ev = {"time_ns": time_ns, "row_addr": hex(row_addr),
                  "duration_ns": self.ppr_latency_ns}
            self.ppr_events.append(ev)
            self.ppr_log.append(ev)
            self.ce_timestamps.clear()

    def _trigger_viral(self, time_ns):
        if not self.viral_active:
            self.viral_active   = True
            self.viral_start_ns = time_ns
            self.viral_log.append({"time_ns": time_ns, "component_id": "device0",
                                   "event": "viral_activated"})

    def _scrub_overhead(self, current_ns, req_bytes):
        if not self.scrub_interval_ns:
            return 0.0
        phase  = current_ns % self.scrub_interval_ns
        active = self.scrub_interval_ns * self.scrub_bw_frac
        if phase < active:
            return self.scrub_bw_frac * (req_bytes / 64.0) * 0.5
        return 0.0

    def stats(self):
        if not self.enabled:
            return {"enabled": False}
        return {
            "enabled":                    True,
            "scrub_bw_overhead_fraction": self.scrub_bw_frac,
            "correctable_errors":         self.ce_count,
            "uncorrectable_errors":       self.uce_count,
            "ppr_events":                 len(self.ppr_events),
            "poisoned_lines":             len(self.poisoned_addrs),
            "viral_events":               len(self.viral_log),
        }


# ---------------------------------------------------------------------------
# Main simulator
# ---------------------------------------------------------------------------

class CXRaySimulator:
    """
    Fast event-driven CXL simulator.

    Per-request timing pipeline:

      t_arrive
        → [M2S credit stall]           CreditPool (M2S_REQ or M2S_RWD)
        → [host TX overhead]           fixed: host_overhead_ns
        → [fwd link BW serialization]  LinkBW.enqueue_fwd()
        → [fwd link propagation]       link_one_way_ns
        → [switch forward hops]        switch_fwd_ns (if has_switch)
        → [DCOH (Type 2)]              dcoh_ns
        → [GIM arbitration]            gim_ns (if gim_enable)
        → [FEC decode fwd + IDE fwd]   fec_ns + ide_ns
        → [DRAM]                       AnalyticalDRAMBackend
        → [ret link BW serialization]  LinkBW.enqueue_ret()
        → [ret link propagation]       link_one_way_ns
        → [switch return hops]         switch_ret_ns
        → [FEC decode ret + IDE ret]   fec_ns + ide_ns
        → [BI RTT (Type 2 writes)]     bi_rtt_ns
        → complete_ns
    """

    def __init__(self, cfg):
        self.cfg = cfg
        random.seed(cfg["sim"]["seed"])

        d = cfg["derived"]
        cr = cfg["link"]["credits"]

        # DRAM backends: one per GIM device (each device has independent banks)
        self.gim_enabled = d["gim_enabled"]
        self.gim_devs    = d["gim_num_devices"]
        self.gim_gran    = int(cfg["fabric"]["gim_interleave_gran"]) if self.gim_enabled else 64
        # Select DRAM backend: "queued" (arrival-order with bank serialization)
        # or "analytical" (fast, no bank queuing). Queued is required for
        # accurate GIM bank-conflict isolation and high-concurrency results.
        dram_backend_type = get(cfg, "dram", "backend", default="analytical")
        def _make_dram_backend():
            if dram_backend_type == "queued":
                return QueuedFRFCFSBackend(cfg)
            return AnalyticalDRAMBackend(cfg)

        if self.gim_enabled and self.gim_devs > 1:
            self.dram_devices = [_make_dram_backend() for _ in range(self.gim_devs)]
        else:
            self.dram_devices = [_make_dram_backend()]
        self.dram = self.dram_devices[0]  # for RASEngine reference

        self.ras  = RASEngine(cfg, self.dram)

        # Link bandwidth enforcers
        # Multi-host: per-host upstream links + optional shared downstream link.
        # Single-host: one link in host_links[0], no switch_dl.
        flit_payload  = cfg["link"]["flit_payload_bytes"]
        flit_size     = cfg["link"]["flit_size_bytes"]   # full wire flit for tx timing
        link_bw_GBps  = d["link_bandwidth_GBps"]
        d_mh          = cfg.get("multihost", {})
        separate_ul   = bool(d_mh.get("separate_upstream_links", True))

        # num_hosts resolved after the multihost block below; read it early.
        _num_hosts_early = int(d_mh.get("num_hosts", 1))

        dl_bw = float(d_mh.get("downstream_bw_GBps", 0))
        # Create switch_dl when: multi-host (always), or single-host with explicit
        # downstream_bw_GBps and a switch present (enables DL utilization measurement at N=1).
        need_dl = (_num_hosts_early > 1 and separate_ul) or \
                  (dl_bw > 0 and d["has_switch"])
        if need_dl:
            self.host_links = [
                LinkBW(bw_GBps=link_bw_GBps, flit_payload_bytes=flit_payload,
                       flit_size_bytes=flit_size)
                for _ in range(max(_num_hosts_early, 1))
            ]
            effective_dl_bw = dl_bw if dl_bw > 0 else link_bw_GBps
            self.switch_dl = LinkBW(bw_GBps=effective_dl_bw, flit_payload_bytes=flit_payload,
                                    flit_size_bytes=flit_size)
        else:
            self.host_links = [
                LinkBW(bw_GBps=link_bw_GBps, flit_payload_bytes=flit_payload,
                       flit_size_bytes=flit_size)
            ]
            self.switch_dl = None

        # One physical Flex Bus serializer per direction.
        self._fwd_link = LinkBW(bw_GBps=link_bw_GBps, flit_payload_bytes=flit_payload,
                                flit_size_bytes=flit_size)
        self._ret_link = LinkBW(bw_GBps=link_bw_GBps, flit_payload_bytes=flit_payload,
                                flit_size_bytes=flit_size)

        # Credit pools — M2S (host→device) and S2M (device→host)
        # Credits return at actual request/response completion (not estimated RTT).
        d_multihost = cfg.get("multihost", {})
        self.num_hosts = int(d_multihost.get("num_hosts", 1))
        if self.num_hosts > 1:
            per_host_limit = int(d_multihost.get("per_host_credits",
                                                  max(4, cr["m2s_req"] // self.num_hosts)))
            per_host_rwd = int(d_multihost.get("per_host_rwd_credits",
                                               max(2, cr["m2s_rwd"] // self.num_hosts)))
            self.req_pool_mh = MultiHostCreditPools(self.num_hosts, per_host_limit)
            self.rwd_pool_mh = MultiHostCreditPools(self.num_hosts, per_host_rwd)
            self.req_pool = None   # unused in multi-host mode
            self.rwd_pool = None   # unused in multi-host mode
        else:
            self.req_pool = CreditPool(cr["m2s_req"])  # reads
            self.rwd_pool = CreditPool(cr["m2s_rwd"])  # writes
            self.req_pool_mh = None
            self.rwd_pool_mh = None
        # S2M response-channel flow control (device→host).
        # S2M_DRS: data responses (read completions); S2M_NDR: non-data (write ACKs).
        # These credits are consumed when the device starts sending the response and
        # returned when the host signals receipt (at complete_ns).
        self.s2m_drs_pool = CreditPool(cr.get("s2m_drs", 32))
        self.s2m_ndr_pool = CreditPool(cr.get("s2m_ndr", 32))

        # Fixed per-request latency components (ns, from derived)
        self.host_overhead_ns = float(cfg.get("host_overhead_ns", 97))
        self.link_ns          = d["link_one_way_ns"]
        self.fec_ns           = d["fec_ns"]
        self.ide_ns           = d["ide_ns"]
        self.switch_fwd_ns    = d["switch_fwd_ns"]
        self.switch_ret_ns    = d["switch_ret_ns"]
        self.dcoh_ns          = d["dcoh_ns"]
        self.is_type2         = d["is_type2"]
        self.bi_rtt_ns        = d["bi_rtt_ns"]
        self.gim_enabled      = d["gim_enabled"]
        self.gim_devs         = d["gim_num_devices"]

        # GIM arbitration ns (only when gim_enabled)
        self.gim_arb_ns = (float(cfg["fabric"]["gim_arbitration_ns"])
                           if self.gim_enabled else 0.0)

        # TSP session state (matches C++ cxl_device.cpp tspSessionEstablished_ / tspBytesThisKey_)
        dev = cfg["device"]
        self._tsp_enable = bool(dev.get("tsp_enable", False)) and self.ide_ns > 0
        self._tsp_session_setup_ns   = 800.0
        self._tsp_key_rotation_ns    = 120.0
        self._tsp_key_rotation_bytes = int(dev.get("tsp_key_rotation_mb", 64)) * 1024 * 1024
        self._tsp_session_established = False
        self._tsp_bytes_this_key     = 0

        # Per-request results
        self.results = []

        # Stats windows
        self.window_ns         = cfg["sim"]["stats_window_ns"]
        self.bw_windows        = []
        self.lat_windows       = []
        self._win_read_bytes   = 0
        self._win_write_bytes  = 0
        self._win_lat_sum      = 0
        self._win_lat_count    = 0
        self._win_start        = 0

    def _flush_window(self, current_ns):
        while current_ns >= self._win_start + self.window_ns:
            dur_s = self.window_ns * 1e-9
            r_gbps = (self._win_read_bytes  / dur_s / 1e9) if dur_s > 0 else 0
            w_gbps = (self._win_write_bytes / dur_s / 1e9) if dur_s > 0 else 0
            avg_lat = (self._win_lat_sum / self._win_lat_count
                       if self._win_lat_count > 0 else 0)
            t_us = self._win_start / 1000.0
            self.bw_windows.append((t_us, round(r_gbps, 4), round(w_gbps, 4)))
            self.lat_windows.append((t_us, round(avg_lat, 2)))
            self._win_read_bytes  = 0
            self._win_write_bytes = 0
            self._win_lat_sum     = 0
            self._win_lat_count   = 0
            self._win_start      += self.window_ns

    def run(self, requests):
        for req in requests:
            t_arrive = float(req["timestamp_ns"])
            self._flush_window(t_arrive)

            is_write = req["op"] in ("W", "FLUSH", "RMW", "PUTX")

            # ── 1. M2S credit pool stall ─────────────────────────────────
            # acquire() returns stall_ns; credit is released (release()) after
            # computing complete_ns so return times are actual completions.
            # host_id: use explicit field from load_multi_traces if present;
            #          fall back to requester_id % num_hosts for single-file traces.
            host_id = req.get("host_id", req.get("requester_id", 0) % self.num_hosts)
            if is_write:
                if self.rwd_pool_mh is not None:
                    credit_stall = self.rwd_pool_mh.acquire(host_id, t_arrive)
                    pool = None
                else:
                    pool = self.rwd_pool
                    credit_stall = pool.acquire(t_arrive)
            elif self.req_pool_mh is not None:
                credit_stall = self.req_pool_mh.acquire(host_id, t_arrive)
                pool = None   # release handled separately below
            else:
                pool = self.req_pool
                credit_stall = pool.acquire(t_arrive)

            t_credit = t_arrive + credit_stall

            # ── 2. Host TX overhead ──────────────────────────────────────
            t_host_done = t_credit + self.host_overhead_ns

            # ── 3. Forward link BW serialization (per-host upstream link) ─
            # In multi-host mode, each host has its own independent upstream link.
            # In single-host mode, host_links[0] == _fwd_link equivalent.
            fwd_link = (self.host_links[host_id % len(self.host_links)]
                        if len(self.host_links) > 1 else self._fwd_link)
            t_flit_done = fwd_link.enqueue_fwd(t_host_done, req["size"])

            # ── 4. Forward propagation: host → switch (or host → device) ─
            t_at_switch = t_flit_done + self.link_ns

            # ── 5. Switch: intra-switch hops + shared output-port arbiter ─
            if self.switch_dl is not None:
                t_sw_in  = t_at_switch + self.switch_fwd_ns
                t_device = self.switch_dl.enqueue_fwd(t_sw_in, req["size"])
            else:
                t_device = t_at_switch + self.switch_fwd_ns

            # ── 6. Device overhead: DCOH (Type 2) + FEC + IDE ────────────
            t_device += self.dcoh_ns + self.fec_ns + self.ide_ns

            # ── 6b. TSP session setup / key rotation ─────────────────────
            if self._tsp_enable:
                if not self._tsp_session_established:
                    t_device += self._tsp_session_setup_ns
                    self._tsp_session_established = True
                self._tsp_bytes_this_key += req["size"]
                if self._tsp_bytes_this_key >= self._tsp_key_rotation_bytes:
                    t_device += self._tsp_key_rotation_ns
                    self._tsp_bytes_this_key = 0

            # ── 7. GIM arbitration ────────────────────────────────────────
            t_device += self.gim_arb_ns

            # ── 8. RAS overhead ──────────────────────────────────────────
            ras_extra = self.ras.process(req, t_device)
            t_device += ras_extra

            # ── 9. DRAM access (routed to per-device backend for GIM) ────
            if self.gim_enabled and self.gim_devs > 1:
                dev_idx  = (req["addr"] // self.gim_gran) % self.gim_devs
                dram_dev = self.dram_devices[dev_idx]
            else:
                dram_dev = self.dram_devices[0]
            dram_lat, hit_type = dram_dev.access(req["addr"], is_write, t_device)
            t_dram_done = t_device + dram_lat

            # ── 10. Shared downstream return (device → switch output port) ─
            # S2M_NDR (write ACK) is a 4-byte header-only message; S2M_DRS (read
            # completion) carries the 64-byte cacheline payload.  Match C++ fast path.
            rsp_size = 4 if is_write else req["size"]
            if self.switch_dl is not None:
                t_ret_sw_out  = self.switch_dl.enqueue_ret(t_dram_done, rsp_size)
                t_ret_sw_done = t_ret_sw_out + self.switch_ret_ns
            else:
                t_ret_sw_done = t_dram_done + self.switch_ret_ns

            # ── 11. S2M response-credit acquire ──────────────────────────
            # Device must hold an S2M slot before transmitting the response.
            # S2M_DRS for read completions (carry data); S2M_NDR for write ACKs.
            if is_write:
                s2m_stall = self.s2m_ndr_pool.acquire(t_ret_sw_done)
            else:
                s2m_stall = self.s2m_drs_pool.acquire(t_ret_sw_done)
            t_ret_sw_done += s2m_stall

            # ── 12. Return link BW serialization (per-host upstream link) ──
            # Same host link as used on the forward path.
            ret_link = (self.host_links[host_id % len(self.host_links)]
                        if len(self.host_links) > 1 else self._ret_link)
            t_ret_done = ret_link.enqueue_ret(t_ret_sw_done, rsp_size)
            complete_ns = t_ret_done + self.link_ns + self.fec_ns + self.ide_ns

            # ── 13. Type 2 Back-Invalidation on writes (worst-case profile) ─
            if self.is_type2 and is_write:
                complete_ns += self.bi_rtt_ns

            # ── 14. Release credits (M2S and S2M) at actual completion ────
            if is_write:
                if self.rwd_pool_mh is not None:
                    self.rwd_pool_mh.release(host_id, complete_ns)
                else:
                    self.rwd_pool.release(complete_ns)
                self.s2m_ndr_pool.release(complete_ns)
            elif self.req_pool_mh is not None:
                self.req_pool_mh.release(host_id, complete_ns)
                self.s2m_drs_pool.release(complete_ns)
            else:
                self.req_pool.release(complete_ns)
                self.s2m_drs_pool.release(complete_ns)

            latency_ns = complete_ns - t_arrive

            # Accumulate window stats
            if is_write:
                self._win_write_bytes += req["size"]
            else:
                self._win_read_bytes  += req["size"]
            self._win_lat_sum   += latency_ns
            self._win_lat_count += 1

            self.results.append({
                "timestamp_ns":  t_arrive,
                "complete_ns":   complete_ns,
                "latency_ns":    latency_ns,
                "op":            req["op"],
                "addr":          req["addr"],
                "requester_id":  req["requester_id"],
                "host_id":       host_id,
                "dram_hit_type": hit_type,
                "credit_stall_ns": credit_stall,
                "dram_ns":       dram_lat,
            })

        if self.results:
            self._flush_window(self.results[-1]["complete_ns"] + self.window_ns)

    def run_closed_loop(self, seed_reqs, n_per_req,
                        stride_bytes=64, region_bytes=0x1000000,
                        random_addrs=False):
        """Closed-loop (back-to-back) simulation.

        Each requester immediately issues the next request as soon as the
        previous one completes (zero think time).  This avoids the open-system
        queue-divergence artifact that arises with fixed-timestamp traces when
        the arrival rate exceeds the credit-return rate.  Outstanding demand
        stays bounded by the requester count, so finite credit pools and bank
        queues produce honest load effects instead of unbounded growth.

        seed_reqs   : list with one request dict per requester; defines the
                      starting address and operation for each requester.
        n_per_req   : total requests to simulate per requester.
        stride_bytes: address advance per successive request (default 64 B).
        region_bytes: address wraps within [base, base+region_bytes) per
                      requester (default 16 MB).
        random_addrs: when true, each reissue picks a uniform-random 64 B
                      line in the requester's region instead of striding.
                      Sequential strides march requesters in lockstep and
                      self-organize into convoys (degenerate p99 == mean);
                      random selection restores bank/row burstiness, so
                      loaded runs show genuine tails.  Uses a fixed seed, so
                      runs are bit-reproducible.
        """
        import heapq

        # Key per-requester state by (host_id, requester_id) composite.
        # Using requester_id alone collapses host-0 rid-0 with host-1 rid-0.
        per_req = {}
        for req in seed_reqs:
            hid = req.get("host_id", 0)
            rid = req["requester_id"]
            key = (hid, rid)
            if key not in per_req:
                per_req[key] = {
                    "addr":  req["addr"],
                    "base":  req["addr"],
                    "op":    req["op"],
                    "size":  req["size"],
                    "count": 0,
                }

        # 1 ns stagger between requesters to avoid exact simultaneous arrivals
        heap = [(float(i), key) for i, key in enumerate(sorted(per_req))]
        heapq.heapify(heap)
        # Fixed-seed RNG for random-address reissues (bit-reproducible runs)
        _cl_rng = random.Random(0xC10C)

        total_done = 0
        target = len(per_req) * n_per_req

        while heap and total_done < target:
            t_arrive, key = heapq.heappop(heap)
            hid, rid = key
            st = per_req[key]
            if st["count"] >= n_per_req:
                continue

            req = {
                "timestamp_ns": t_arrive,
                "op":           st["op"],
                "addr":         st["addr"],
                "size":         st["size"],
                "requester_id": rid,
                "host_id":      hid,
            }

            # ── same pipeline as run() ────────────────────────────────────
            self._flush_window(t_arrive)
            is_write = req["op"] in ("W", "FLUSH", "RMW", "PUTX")

            _host_id_d = hid
            if is_write:
                if self.rwd_pool_mh is not None:
                    credit_stall = self.rwd_pool_mh.acquire(_host_id_d, t_arrive)
                    pool = None
                else:
                    pool = self.rwd_pool
                    credit_stall = pool.acquire(t_arrive)
            elif self.req_pool_mh is not None:
                credit_stall = self.req_pool_mh.acquire(_host_id_d, t_arrive)
                pool = None
            else:
                pool = self.req_pool
                credit_stall = pool.acquire(t_arrive)

            t_credit     = t_arrive + credit_stall
            t_host_done  = t_credit + self.host_overhead_ns

            fwd_link_d = (self.host_links[_host_id_d % len(self.host_links)]
                          if len(self.host_links) > 1 else self._fwd_link)
            t_flit_done = fwd_link_d.enqueue_fwd(t_host_done, req["size"])
            t_at_switch = t_flit_done + self.link_ns

            if self.switch_dl is not None:
                t_sw_in  = t_at_switch + self.switch_fwd_ns
                t_device = self.switch_dl.enqueue_fwd(t_sw_in, req["size"])
            else:
                t_device = t_at_switch + self.switch_fwd_ns

            t_device += self.dcoh_ns + self.fec_ns + self.ide_ns

            if self._tsp_enable:
                if not self._tsp_session_established:
                    t_device += self._tsp_session_setup_ns
                    self._tsp_session_established = True
                self._tsp_bytes_this_key += req["size"]
                if self._tsp_bytes_this_key >= self._tsp_key_rotation_bytes:
                    t_device += self._tsp_key_rotation_ns
                    self._tsp_bytes_this_key = 0

            t_device += self.gim_arb_ns
            t_device += self.ras.process(req, t_device)

            if self.gim_enabled and self.gim_devs > 1:
                dev_idx  = (req["addr"] // self.gim_gran) % self.gim_devs
                dram_dev = self.dram_devices[dev_idx]
            else:
                dram_dev = self.dram_devices[0]
            dram_lat, hit_type = dram_dev.access(req["addr"], is_write, t_device)
            t_dram_done = t_device + dram_lat

            # S2M_NDR (write ACK) = 4-byte header; S2M_DRS (read completion) = full payload.
            rsp_size_d = 4 if is_write else req["size"]
            if self.switch_dl is not None:
                t_ret_sw_out  = self.switch_dl.enqueue_ret(t_dram_done, rsp_size_d)
                t_ret_sw_done = t_ret_sw_out + self.switch_ret_ns
            else:
                t_ret_sw_done = t_dram_done + self.switch_ret_ns

            if is_write:
                s2m_stall = self.s2m_ndr_pool.acquire(t_ret_sw_done)
            else:
                s2m_stall = self.s2m_drs_pool.acquire(t_ret_sw_done)
            t_ret_sw_done += s2m_stall

            ret_link_d = (self.host_links[_host_id_d % len(self.host_links)]
                          if len(self.host_links) > 1 else self._ret_link)
            t_ret_done = ret_link_d.enqueue_ret(t_ret_sw_done, rsp_size_d)
            complete_ns = t_ret_done + self.link_ns + self.fec_ns + self.ide_ns

            if self.is_type2 and is_write:
                complete_ns += self.bi_rtt_ns

            if is_write:
                if self.rwd_pool_mh is not None:
                    self.rwd_pool_mh.release(_host_id_d, complete_ns)
                else:
                    self.rwd_pool.release(complete_ns)
                self.s2m_ndr_pool.release(complete_ns)
            elif self.req_pool_mh is not None:
                self.req_pool_mh.release(_host_id_d, complete_ns)
                self.s2m_drs_pool.release(complete_ns)
            else:
                self.req_pool.release(complete_ns)
                self.s2m_drs_pool.release(complete_ns)

            latency_ns = complete_ns - t_arrive
            if is_write:
                self._win_write_bytes += req["size"]
            else:
                self._win_read_bytes  += req["size"]
            self._win_lat_sum   += latency_ns
            self._win_lat_count += 1

            self.results.append({
                "timestamp_ns":    t_arrive,
                "complete_ns":     complete_ns,
                "latency_ns":      latency_ns,
                "op":              req["op"],
                "addr":            req["addr"],
                "requester_id":    rid,
                "host_id":         hid,
                "dram_hit_type":   hit_type,
                "credit_stall_ns": credit_stall,
                "dram_ns":         dram_lat,
            })
            # ─────────────────────────────────────────────────────────────

            st["count"] += 1
            total_done  += 1
            # Advance address, wrap within requester's region
            if random_addrs:
                st["addr"] = (st["base"]
                              + 64 * _cl_rng.randrange(region_bytes // 64))
            else:
                st["addr"] = (st["base"]
                              + ((st["addr"] - st["base"] + stride_bytes)
                                 % region_bytes))

            if st["count"] < n_per_req:
                heapq.heappush(heap, (complete_ns, key))

        if self.results:
            self._flush_window(self.results[-1]["complete_ns"] + self.window_ns)

    def compute_summary(self, trace_file, config_file, wall_clock_s):
        if not self.results:
            return {}

        def pct(lst, p):
            if not lst: return 0.0
            s = sorted(lst)
            return s[max(0, int(len(s) * p / 100) - 1)]

        all_lats   = [r["latency_ns"]  for r in self.results]
        reads      = [r for r in self.results if r["op"] not in ("W","FLUSH","PUTX")]
        writes     = [r for r in self.results if r["op"] in ("W","FLUSH","RMW","PUTX")]
        read_lats  = [r["latency_ns"]  for r in reads]
        write_lats = [r["latency_ns"]  for r in writes]
        dram_ns_l  = [r["dram_ns"]     for r in self.results]
        stall_ns_l = [r["credit_stall_ns"] for r in self.results]

        t_sim = self.results[-1]["complete_ns"] - self.results[0]["timestamp_ns"]
        dur_s = t_sim * 1e-9

        total_read_bytes  = sum(64 for _ in reads)
        total_write_bytes = sum(64 for _ in writes)
        bw_read  = (total_read_bytes  / dur_s / 1e9) if dur_s > 0 else 0
        bw_write = (total_write_bytes / dur_s / 1e9) if dur_s > 0 else 0

        d = self.cfg["derived"]

        return {
            "simulator":    "CXRay",
            "version":      __version__,
            "trace":        os.path.basename(trace_file),
            "config":       os.path.basename(config_file),
            "cxl_version":  self.cfg.get("cxl_version", "?"),

            "total_requests": len(self.results),
            "total_reads":    len(reads),
            "total_writes":   len(writes),
            "simulation_time_ns": int(t_sim),
            "wall_clock_s":       round(wall_clock_s, 4),

            "latency_ns": {
                "all_mean":   round(sum(all_lats)  / len(all_lats),  2) if all_lats  else 0,
                "all_p50":    round(pct(all_lats,  50), 2),
                "all_p95":    round(pct(all_lats,  95), 2),
                "all_p99":    round(pct(all_lats,  99), 2),
                "all_p999":   round(pct(all_lats,  99.9), 2),
                "read_mean":  round(sum(read_lats) / len(read_lats), 2) if read_lats  else 0,
                "read_p99":   round(pct(read_lats,  99), 2),
                "write_mean": round(sum(write_lats)/ len(write_lats),2) if write_lats else 0,
                "write_p99":  round(pct(write_lats, 99), 2),
            },

            "dram_ns": {
                "mean": round(sum(dram_ns_l) / len(dram_ns_l), 2) if dram_ns_l else 0,
                "p99":  round(pct(dram_ns_l, 99), 2),
            },

            "bandwidth_GBps": {
                "read_avg":   round(bw_read,  4),
                "write_avg":  round(bw_write, 4),
                "total_avg":  round(bw_read + bw_write, 4),
            },

            "link": {
                "bandwidth_GBps":           d["link_bandwidth_GBps"],
                # Single-host: upstream utilization of the one link.
                # Multi-host: aggregate upstream wire bytes / single-link capacity —
                # represents the combined load expressed as a multiple of one link;
                # values > 1.0 indicate total demand exceeds one link's capacity.
                "utilization_aggregate":    round(
                    (sum(hl.total_fwd_bytes for hl in self.host_links)
                     / (d["link_bandwidth_GBps"] * t_sim))
                    if len(self.host_links) > 1 and t_sim > 0
                    else self._fwd_link.utilization(t_sim), 4),
                "switch_dl_utilization":    round(
                    self.switch_dl.utilization(t_sim), 4)
                    if self.switch_dl is not None else None,
            },

            "overhead_ns": {
                "fwd_fixed":                    d["fwd_ns"],
                "ret_fixed":                    d["ret_ns"],
                "switch_fwd":                   d["switch_fwd_ns"],
                "switch_ret":                   d["switch_ret_ns"],
                "dcoh":                         d["dcoh_ns"],
                "hdm_interleave_arbitration_ns": self.gim_arb_ns,
                "fec_per_dir":     d["fec_ns"],
                "ide_per_dir":     d["ide_ns"],
                "is_type2":        d["is_type2"],
                "bi_rtt":          d["bi_rtt_ns"],
            },

            "credit_pools": {
                "req_pool": (self.req_pool.stats() if self.req_pool is not None
                             else {"multihost": [p.stats() for p in self.req_pool_mh.pools]}
                             if self.req_pool_mh is not None else {}),
                "rwd_pool": (self.rwd_pool.stats() if self.rwd_pool is not None
                             else {"multihost": [p.stats() for p in self.rwd_pool_mh.pools]}
                             if self.rwd_pool_mh is not None else {}),
                "s2m_drs_pool":  self.s2m_drs_pool.stats(),
                "s2m_ndr_pool":  self.s2m_ndr_pool.stats(),
                "m2s_stall_ns": round(
                    (self.req_pool.total_stall_ns if self.req_pool is not None
                     else sum(p.total_stall_ns for p in self.req_pool_mh.pools)
                     if self.req_pool_mh is not None else 0.0)
                    + (self.rwd_pool.total_stall_ns if self.rwd_pool is not None
                       else sum(p.total_stall_ns for p in self.rwd_pool_mh.pools)
                       if self.rwd_pool_mh is not None else 0.0), 2),
                "s2m_stall_ns": round(
                    self.s2m_drs_pool.total_stall_ns
                    + self.s2m_ndr_pool.total_stall_ns, 2),
                "total_stall_ns": round(
                    (self.req_pool.total_stall_ns if self.req_pool is not None
                     else sum(p.total_stall_ns for p in self.req_pool_mh.pools)
                     if self.req_pool_mh is not None else 0.0)
                    + (self.rwd_pool.total_stall_ns if self.rwd_pool is not None
                       else sum(p.total_stall_ns for p in self.rwd_pool_mh.pools)
                       if self.rwd_pool_mh is not None else 0.0)
                    + self.s2m_drs_pool.total_stall_ns
                    + self.s2m_ndr_pool.total_stall_ns, 2),
                "stall_fraction": round(
                    ((self.req_pool.total_stall_ns if self.req_pool is not None
                      else sum(p.total_stall_ns for p in self.req_pool_mh.pools)
                      if self.req_pool_mh is not None else 0.0)
                     + (self.rwd_pool.total_stall_ns if self.rwd_pool is not None
                        else sum(p.total_stall_ns for p in self.rwd_pool_mh.pools)
                        if self.rwd_pool_mh is not None else 0.0)
                     + self.s2m_drs_pool.total_stall_ns
                     + self.s2m_ndr_pool.total_stall_ns)
                    / max(1, t_sim), 6),
            },

            "dram_model": self._aggregate_dram_stats(),
            "ras":        self.ras.stats(),
            "gim": {
                "num_devices": self.gim_devs if self.gim_enabled else 1,
                "enabled":     self.gim_enabled,
            },
            "per_host": self._per_host_stats(),
        }

    def _per_host_stats(self):
        """
        Return per-host latency/BW breakdown keyed by "host_N".
        Only populated when num_hosts > 1; returns {} otherwise.
        Computed from self.results in a single O(R·H) post-run pass.
        """
        if self.num_hosts <= 1:
            return {}

        def pct(lst, p):
            if not lst: return 0.0
            s = sorted(lst)
            return s[max(0, int(len(s) * p / 100) - 1)]

        by_host = defaultdict(list)
        for r in self.results:
            hid = r.get("host_id", r["requester_id"] % self.num_hosts)
            by_host[hid].append(r)

        out = {}
        for hid in range(self.num_hosts):
            recs = by_host.get(hid, [])
            if not recs:
                continue
            lats   = [r["latency_ns"] for r in recs]
            reads  = [r for r in recs if r["op"] not in ("W", "FLUSH", "RMW", "PUTX")]
            t_sim  = recs[-1]["complete_ns"] - recs[0]["timestamp_ns"]
            dur_s  = max(t_sim * 1e-9, 1e-12)
            out[f"host_{hid}"] = {
                "requests":  len(recs),
                "reads":     len(reads),
                "writes":    len(recs) - len(reads),
                "latency_ns": {
                    "mean": round(sum(lats) / len(lats), 2),
                    "p50":  round(pct(lats, 50), 2),
                    "p99":  round(pct(lats, 99), 2),
                    "p999": round(pct(lats, 99.9), 2),
                },
                "bandwidth_GBps": {
                    "total_avg": round(len(recs) * 64 / dur_s / 1e9, 4),
                },
                "credit_stall_ns": round(
                    sum(r["credit_stall_ns"] for r in recs), 2),
            }
        return out

    def _aggregate_dram_stats(self):
        """Aggregate row-buffer stats across all GIM device backends."""
        hits = sum(d.row_hits     for d in self.dram_devices)
        miss = sum(d.row_misses   for d in self.dram_devices)
        conf = sum(d.row_conflicts for d in self.dram_devices)
        total = hits + miss + conf
        return {
            "row_buffer_hits":      hits,
            "row_buffer_misses":    miss,
            "row_buffer_conflicts": conf,
            "row_buffer_hit_rate":  hits / total if total > 0 else 0.0,
        }


# ---------------------------------------------------------------------------
# Output writer
# ---------------------------------------------------------------------------

def write_outputs(sim, cfg, summary, output_dir, trace_file, config_file, wall_clock_s):
    os.makedirs(output_dir, exist_ok=True)
    for sub in ("latency", "bandwidth", "queue", "dram", "ras", "errors"):
        os.makedirs(os.path.join(output_dir, sub), exist_ok=True)

    with open(os.path.join(output_dir, "summary.json"), "w") as f:
        json.dump(summary, f, indent=2)

    # Per-request latency
    with open(os.path.join(output_dir, "latency", "latency_all.csv"), "w") as f:
        f.write("timestamp_ns,complete_ns,latency_ns,op,dram_ns,"
                "credit_stall_ns,dram_hit_type\n")
        for r in sim.results:
            f.write(f"{r['timestamp_ns']:.0f},{r['complete_ns']:.1f},"
                    f"{r['latency_ns']:.2f},{r['op']},{r['dram_ns']:.2f},"
                    f"{r['credit_stall_ns']:.2f},{r['dram_hit_type']}\n")

    # Latency histogram
    lats = [r["latency_ns"] for r in sim.results]
    if lats:
        lo, hi = min(lats), max(lats)
        bw     = max(1.0, (hi - lo) / 100)
        hist   = defaultdict(int)
        for l in lats:
            hist[int((l - lo) / bw)] += 1
        with open(os.path.join(output_dir, "latency", "latency_hist.csv"), "w") as f:
            f.write("bin_ns,count\n")
            for b in sorted(hist):
                f.write(f"{lo + b * bw:.1f},{hist[b]}\n")

        sorted_lats = sorted(lats)
        with open(os.path.join(output_dir, "latency", "latency_cdf.csv"), "w") as f:
            f.write("latency_ns,cumulative_fraction\n")
            for i, l in enumerate(sorted_lats):
                f.write(f"{l:.2f},{(i+1)/len(sorted_lats):.6f}\n")

    with open(os.path.join(output_dir, "latency", "latency_timeseries.csv"), "w") as f:
        f.write("time_us,avg_lat_ns\n")
        for t_us, avg_lat in sim.lat_windows:
            f.write(f"{t_us:.3f},{avg_lat:.2f}\n")

    # Bandwidth timeseries
    with open(os.path.join(output_dir, "bandwidth", "bw_timeseries.csv"), "w") as f:
        f.write("time_us,read_GBps,write_GBps,total_GBps\n")
        for t_us, r, w in sim.bw_windows:
            f.write(f"{t_us:.3f},{r:.4f},{w:.4f},{r+w:.4f}\n")

    # Per-host latency breakdown (multi-host only)
    if sim.num_hosts > 1:
        per_host_dir = os.path.join(output_dir, "per_host")
        os.makedirs(per_host_dir, exist_ok=True)
        by_host = defaultdict(list)
        for r in sim.results:
            hid = r.get("host_id", r["requester_id"] % sim.num_hosts)
            by_host[hid].append(r)
        for hid, recs in sorted(by_host.items()):
            with open(os.path.join(per_host_dir, f"host_{hid}_latency.csv"), "w") as f:
                f.write("timestamp_ns,latency_ns,op,credit_stall_ns\n")
                for r in recs:
                    f.write(f"{r['timestamp_ns']:.0f},{r['latency_ns']:.2f},"
                            f"{r['op']},{r['credit_stall_ns']:.2f}\n")

    # Credit pool
    with open(os.path.join(output_dir, "queue", "credit_stalls.csv"), "w") as f:
        f.write("pool,limit,stall_events,total_stall_ns,avg_stall_per_req\n")
        req_pools = []
        if sim.req_pool is not None:
            req_pools = [("M2S_REQ", sim.req_pool)]
        elif sim.req_pool_mh is not None:
            req_pools = [(f"M2S_REQ_H{i}", p)
                         for i, p in enumerate(sim.req_pool_mh.pools)]
        rwd_pools = []
        if sim.rwd_pool is not None:
            rwd_pools = [("M2S_RWD", sim.rwd_pool)]
        elif sim.rwd_pool_mh is not None:
            rwd_pools = [(f"M2S_RWD_H{i}", p)
                         for i, p in enumerate(sim.rwd_pool_mh.pools)]
        for name, pool in req_pools + rwd_pools:
            s = pool.stats()
            f.write(f"{name},{s['limit']},{s['stall_events']},"
                    f"{s['total_stall_ns']:.2f},{s['avg_stall_per_req']:.4f}\n")

    # DRAM stats
    ds = sim.dram.stats()
    with open(os.path.join(output_dir, "dram", "row_buffer_hits.csv"), "w") as f:
        f.write("metric,value\n")
        for k, v in ds.items():
            f.write(f"{k},{v}\n")

    # RAS
    rs = cfg["ras"]
    if rs["enable"] and hasattr(sim.ras, "error_log"):
        for fname, log, header in [
            ("error_log.csv", sim.ras.error_log,
             "time_ns,addr,error_type,corrected"),
            ("ppr_events.csv", sim.ras.ppr_log,
             "time_ns,row_addr,duration_ns"),
            ("poison_log.csv", sim.ras.poison_log,
             "time_ns,addr,event_type"),
            ("viral_log.csv", sim.ras.viral_log,
             "time_ns,component_id,event"),
        ]:
            with open(os.path.join(output_dir, "ras", fname), "w") as f:
                f.write(header + "\n")
                for e in log:
                    f.write(",".join(str(v) for v in e.values()) + "\n")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description=f"CXRay v{__version__}: Full-Featured CXL Simulator")
    parser.add_argument("--config", "-c", required=True)
    parser.add_argument("--trace",  "-t", default=None,
                        help="Single-host trace file")
    parser.add_argument("--traces", nargs="+", metavar="TRACE",
                        help="Per-host trace files for multi-host mode "
                             "(one file per host, merged by timestamp)")
    parser.add_argument("--output", "-o", default="results")
    # Allow per-run overrides of scalar config values (key=value pairs)
    parser.add_argument("--set", "-s", action="append", default=[],
                        metavar="KEY=VALUE",
                        help="Override config value, e.g. --set link.credits.m2s_req=8")
    args = parser.parse_args()

    if args.trace and args.traces:
        parser.error("Specify either --trace or --traces, not both")
    if not args.trace and not args.traces:
        parser.error("One of --trace or --traces is required")

    with open(args.config) as f:
        input_cfg = parse_libconfig(f.read())

    # Apply command-line overrides
    for kv in args.set:
        k, _, v = kv.partition("=")
        parts = k.strip().split(".")
        d = input_cfg
        for part in parts[:-1]:
            d = d.setdefault(part, {})
        # Type-coerce
        v = v.strip()
        try:
            v = int(v)
        except ValueError:
            try:
                v = float(v)
            except ValueError:
                if v.lower() == "true":  v = True
                elif v.lower() == "false": v = False
        d[parts[-1]] = v

    cfg = resolve_config(input_cfg)
    d   = cfg["derived"]

    print(f"[CXRay] v{__version__}  config={args.config}")
    print(f"[CXRay] CXL {cfg['cxl_version']}  PCIe Gen{cfg['link']['pcie_gen']}"
          f"×{cfg['link']['pcie_lanes']}  {d['link_bandwidth_GBps']:.1f} GB/s")
    print(f"[CXRay] fwd={d['fwd_ns']:.1f} ns  ret={d['ret_ns']:.1f} ns  "
          f"rtt={d['rtt_ns']:.1f} ns")
    cr = cfg["link"]["credits"]
    print(f"[CXRay] credits M2S_REQ={cr['m2s_req']} M2S_RWD={cr['m2s_rwd']}")
    if d["has_switch"]:
        print(f"[CXRay] switch={cfg['switch']['num_hops']} hop(s)  "
              f"{cfg['switch']['latency_ns']} ns/hop  "
              f"fwd+{d['switch_fwd_ns']:.0f}/ret+{d['switch_ret_ns']:.0f} ns")
    if d["gim_enabled"]:
        print(f"[CXRay] GIM {d['gim_num_devices']} devices  "
              f"arb={cfg['fabric']['gim_arbitration_ns']} ns")
    if d["is_type2"]:
        print(f"[CXRay] Type 2: DCOH={d['dcoh_ns']} ns  BI RTT={d['bi_rtt_ns']} ns")

    t0 = time.time()
    if args.traces:
        # Multi-host mode: infer num_hosts from file count if config differs
        cfg_num_hosts = int(cfg.get("multihost", {}).get("num_hosts", 1))
        if len(args.traces) != cfg_num_hosts:
            cfg.setdefault("multihost", {})["num_hosts"] = len(args.traces)
        host_traces = load_multi_traces(args.traces)
        requests = list(merge_traces(host_traces))
        print(f"[CXRay] Multi-host: {len(args.traces)} hosts, "
              f"{sum(len(t) for t in host_traces):,} total requests")
        trace_label = args.traces[0]   # for summary/output labelling
    else:
        print(f"[CXRay] Reading trace: {args.trace}")
        requests = read_trace(args.trace)
        print(f"[CXRay] Loaded {len(requests):,} requests")
        trace_label = args.trace

    print("[CXRay] Simulating...")
    sim = CXRaySimulator(cfg)

    # Closed-loop injection mode: each requester issues the next request
    # immediately after the previous completes (zero think time).
    # Triggered by --set inject.closed_loop_per_req=N
    _inj = cfg.get("inject", {})
    _cl_per_req  = int(_inj.get("closed_loop_per_req", 0))
    _cl_stride   = int(_inj.get("stride_bytes", 64))
    _cl_region   = int(_inj.get("region_bytes", 0x1000000))
    _cl_random   = bool(int(_inj.get("closed_loop_random", 0)))

    if _cl_per_req > 0:
        # Seed: first request per (host_id, requester_id) composite key.
        # Keying by requester_id alone collapses host 0 rid-0 and host 1 rid-0.
        _seeds = {}
        for _r in requests:
            _key = (_r.get("host_id", 0), _r["requester_id"])
            if _key not in _seeds:
                _seeds[_key] = _r
        print(f"[CXRay] Closed-loop: {len(_seeds)} requesters × {_cl_per_req} req each  "
              f"stride={_cl_stride}B  region={_cl_region//1024}KB"
              + ("  addrs=random" if _cl_random else ""))
        sim.run_closed_loop(list(_seeds.values()), _cl_per_req,
                            stride_bytes=_cl_stride, region_bytes=_cl_region,
                            random_addrs=_cl_random)
    else:
        sim.run(requests)
    wall = time.time() - t0

    summary = sim.compute_summary(trace_label, args.config, wall)
    write_outputs(sim, cfg, summary, args.output, trace_label, args.config, wall)

    lat = summary["latency_ns"]
    bw  = summary["bandwidth_GBps"]
    cp  = summary["credit_pools"]
    print(f"\n[CXRay] ── Results ──────────────────────────────────────────")
    print(f"[CXRay] Requests    : {summary['total_requests']:,} "
          f"({summary['total_reads']:,}R + {summary['total_writes']:,}W)")
    print(f"[CXRay] Mean lat    : {lat['all_mean']:.1f} ns  "
          f"(read {lat['read_mean']:.1f}  write {lat['write_mean']:.1f})")
    print(f"[CXRay] p99 lat     : {lat['all_p99']:.1f} ns")
    print(f"[CXRay] Bandwidth   : {bw['total_avg']:.3f} GB/s  "
          f"(R {bw['read_avg']:.3f}  W {bw['write_avg']:.3f})")
    def _pool_stall_events(pool_entry):
        if not isinstance(pool_entry, dict):
            return 0
        if 'stall_events' in pool_entry:
            return pool_entry['stall_events']
        return sum(p.get('stall_events', 0) for p in pool_entry.get('multihost', []))

    _req_events = _pool_stall_events(cp.get('req_pool', {}))
    _rwd_events = _pool_stall_events(cp.get('rwd_pool', {}))
    print(f"[CXRay] Credit stall: "
          f"REQ events={_req_events}  "
          f"RWD events={_rwd_events}  "
          f"total={cp['total_stall_ns']:.0f} ns")
    print(f"[CXRay] DRAM hit    : {summary['dram_model']['row_buffer_hit_rate']:.3f}")
    print(f"[CXRay] Wall clock  : {wall:.2f} s")
    print(f"[CXRay] Output      : {args.output}/summary.json")


if __name__ == "__main__":
    main()
