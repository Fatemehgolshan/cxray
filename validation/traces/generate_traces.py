#!/usr/bin/env python3
"""
Generate T1-T9 CXRay validation traces.

Trace format (one request per line):
    <timestamp_ns> <op> <addr_hex> <size_bytes> <requester_id> [flags]

Usage:
    python3 generate_traces.py                     # generate all traces to validation/traces/
    python3 generate_traces.py --trace T1          # generate only T1
    python3 generate_traces.py --output /some/dir  # write to alternate directory
"""

import argparse
import os
import random
import sys


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _write_line(fh, ts_ns, op, addr, size, req_id, flags=""):
    """Write a single trace line."""
    parts = [str(ts_ns), op, f"0x{addr:016x}", str(size), str(req_id)]
    if flags:
        parts.append(flags)
    fh.write(" ".join(parts) + "\n")


# ---------------------------------------------------------------------------
# T1 – Sequential read, 4M requests, 16 requesters interleaved, stride-64
# ---------------------------------------------------------------------------

def gen_t1(out_dir):
    fname = os.path.join(out_dir, "T1_seq_read.trace")
    num_requests = 4_000_000
    num_requesters = 16
    cacheline = 64
    inter_issue_ns = 14  # ~PCIe 5 x16 line rate spacing

    addrs = [0x0000_0000] * num_requesters  # per-requester address cursor

    with open(fname, "w") as f:
        f.write("# T1: Sequential read, 4M requests, 16 requesters interleaved, stride-64\n")
        ts = 0
        for i in range(num_requests):
            rid = i % num_requesters
            _write_line(f, ts, "R", addrs[rid], cacheline, rid)
            addrs[rid] += cacheline
            ts += inter_issue_ns

    count = num_requests
    print(f"T1: wrote {count:,} requests -> {fname}")
    return count


# ---------------------------------------------------------------------------
# T2 – Pointer-chasing, 100K requests, single requester, 300 ns spacing
# ---------------------------------------------------------------------------

def gen_t2(out_dir):
    fname = os.path.join(out_dir, "T2_ptr_chase.trace")
    num_requests = 100_000
    mem_range = 1 << 20  # 1 MB
    spacing_ns = 300      # simulate dependent load serialization
    rng = random.Random(0x42)

    with open(fname, "w") as f:
        f.write("# T2: Pointer-chasing, 100K requests, single requester, 300ns spacing\n")
        ts = 0
        addr = rng.randrange(0, mem_range, 64)
        for _ in range(num_requests):
            _write_line(f, ts, "R", addr, 64, 0)
            addr = rng.randrange(0, mem_range, 64)
            ts += spacing_ns

    print(f"T2: wrote {num_requests:,} requests -> {fname}")
    return num_requests


# ---------------------------------------------------------------------------
# T3 – Random read, 7 files for depths 1,2,4,8,16,32,64
#       uniform random 64-bit addresses in 4 GB range, 100K requests each
#       injection spacing: depth * 200 ns between batches
# ---------------------------------------------------------------------------

def gen_t3(out_dir):
    """
    T3: Multi-chain pointer-chase (MLP depth sweep).
    depth = number of concurrent in-flight requests.
    Each batch of `depth` requests arrives at the same timestamp.
    Batches are spaced by base_rtt_ns so requests don't pile up unboundedly.
    Golden: latency-bandwidth curve; higher depth -> higher BW and higher latency.
    """
    depths = [1, 2, 4, 8, 16, 32, 64]
    num_batches = 10_000       # batches per file (total reqs = depth * num_batches)
    mem_range = 4 * (1 << 30)  # 4 GB
    # Base RTT for CXL 2.0 (calibrated): ~30ns link + 20ns ctrl + ~70ns DRAM + 97ns host = ~217ns
    # Use 250ns batch spacing so depth=1 has no queuing; higher depths build queues.
    batch_spacing_ns = 250
    rng = random.Random(0xDEAD)
    total = 0

    for depth in depths:
        fname = os.path.join(out_dir, f"T3_rand_depth{depth}.trace")
        num_requests = depth * num_batches
        with open(fname, "w") as f:
            f.write(f"# T3: MLP depth={depth}, {num_batches} batches of {depth} concurrent reqs\n")
            f.write(f"# Golden: loaded latency-BW curve; depth={depth} in-flight requests\n")
            for batch in range(num_batches):
                ts = batch * batch_spacing_ns
                for req in range(depth):
                    addr = rng.randrange(0, mem_range, 64)
                    # Each req in batch gets unique requester ID (enables true depth parallelism)
                    _write_line(f, ts, "R", addr, 64, req)
        total += num_requests
        print(f"T3 depth={depth:2d}: wrote {num_requests:,} requests ({num_batches} batches) -> {fname}")

    return total


# ---------------------------------------------------------------------------
# T4 – Sequential reads, sweep requesters 1,2,4,8,16 — bandwidth saturation sweep
# ---------------------------------------------------------------------------

def gen_t4(out_dir):
    """
    T4: BW scalability — each requester issues independently at full rate (14ns/req).
    With N requesters issuing simultaneously, aggregate BW scales until link saturation.
    Golden: BW saturates at ~4 threads on CXL vs >8 threads on DDR.
    """
    requester_counts = [1, 2, 4, 8, 16, 32, 64, 128]
    reqs_per_requester = 100_000   # 100K per requester (reduces file size for high counts)
    cacheline = 64
    per_req_interval_ns = 14       # each requester issues one request every 14 ns
    total = 0

    for nreq in requester_counts:
        fname = os.path.join(out_dir, f"T4_seq_req{nreq}.trace")
        # Each requester has its own independent timeline
        # requester i issues at: i*offset, i*offset+per_req_interval, ...
        # where offset spaces requesters within first cycle to avoid exact collision
        offset_ns = per_req_interval_ns // max(1, nreq)
        addrs = [rid * 0x0100_0000 for rid in range(nreq)]  # separate address ranges
        entries = []
        for rid in range(nreq):
            ts = rid * offset_ns  # stagger start slightly
            for _ in range(reqs_per_requester):
                entries.append((ts, "R", addrs[rid], cacheline, rid))
                addrs[rid] += cacheline
                ts += per_req_interval_ns
        entries.sort(key=lambda e: e[0])
        with open(fname, "w") as f:
            f.write(f"# T4: BW scalability, {nreq} independent requester(s), "
                    f"each at 14ns inter-issue\n")
            f.write(f"# Golden: BW saturates at ~4 threads on CXL\n")
            for ts, op, addr, size, rid in entries:
                _write_line(f, ts, op, addr, size, rid)
        num_requests = nreq * reqs_per_requester
        total += num_requests
        print(f"T4 req={nreq:3d}: wrote {num_requests:,} requests -> {fname}")

    return total


# ---------------------------------------------------------------------------
# T5 – Hardware calibration: sequential, shared interleaved addr, varying rate
#       Sweeps pause interval across 3 read/write ratios (read, write, mixed)
# ---------------------------------------------------------------------------

def gen_t5(out_dir):
    """
    T5: Hardware calibration trace.
    Sequential accesses over a shared interleaved address space, r requesters,
    injection rate controlled by pause_ns parameter.  Matches the CZ120 measurement
    methodology: sweep pause interval to produce a bandwidth-latency curve from
    near-zero load to link saturation, across three read/write ratios.
    """
    num_requesters = 16
    reqs_per_requester = 50_000
    cacheline = 64
    pause_values_ns = [100_000, 40_000, 10_000, 5_000, 3_000, 1_000, 500, 200, 100, 14]
    rw_ratios = [
        ("read_dominant", 1.00),   # 100% read
        ("write_dominant", 0.00),  # 100% write
        ("mixed",          0.50),  # 50/50
    ]
    rng = random.Random(0xCAFE_BABE)
    total = 0

    for ratio_name, read_frac in rw_ratios:
        for pause_ns in pause_values_ns:
            fname = os.path.join(
                out_dir, f"T5_calib_{ratio_name}_pause{pause_ns}.trace"
            )
            # Shared interleaved address: all requesters draw from the same
            # sequential address stream (interleaved by requester index).
            shared_addr = 0x0000_0000
            entries = []
            for rid in range(num_requesters):
                ts = rid * pause_ns  # stagger requester start
                addr = shared_addr + rid * cacheline  # interleaved offset
                for _ in range(reqs_per_requester):
                    op = "R" if rng.random() < read_frac else "W"
                    entries.append((ts, op, addr, cacheline, rid))
                    addr += num_requesters * cacheline  # stride over all requesters
                    ts += pause_ns
            entries.sort(key=lambda e: e[0])
            with open(fname, "w") as f:
                f.write(f"# T5: Hardware calibration, {ratio_name}, pause={pause_ns}ns, "
                        f"{num_requesters} requesters\n")
                f.write(f"# Shared interleaved address space; matches CZ120 measurement\n")
                for ts, op, addr, size, rid in entries:
                    _write_line(f, ts, op, addr, size, rid)
            num_requests = num_requesters * reqs_per_requester
            total += num_requests
            print(f"T5 {ratio_name:16s} pause={pause_ns:7d}ns: wrote {num_requests:,} requests -> {fname}")

    return total


# ---------------------------------------------------------------------------
# T6 – Mixed 70% read / 30% write, 2M requests, sequential, interleaved
# ---------------------------------------------------------------------------

def gen_t6(out_dir):
    fname = os.path.join(out_dir, "T6_mixed_rw.trace")
    num_requests = 2_000_000
    num_requesters = 16
    cacheline = 64
    inter_issue_ns = 14
    rng = random.Random(0xBEEF)

    addrs = [0x0000_0000] * num_requesters

    with open(fname, "w") as f:
        f.write("# T6: Mixed 70% read / 30% write, 2M requests, sequential, interleaved\n")
        ts = 0
        for i in range(num_requests):
            rid = i % num_requesters
            op = "R" if rng.random() < 0.70 else "W"
            _write_line(f, ts, op, addrs[rid], cacheline, rid)
            addrs[rid] += cacheline
            ts += inter_issue_ns

    print(f"T6: wrote {num_requests:,} requests -> {fname}")
    return num_requests


# ---------------------------------------------------------------------------
# T7 – Sequential reads at 80% BW (every 14 ns), 100K requests
#       Baseline trace for scrub-interference test (RAS config controls scrub)
# ---------------------------------------------------------------------------

def gen_t7(out_dir):
    fname = os.path.join(out_dir, "T7_scrub_baseline.trace")
    num_requests = 100_000
    cacheline = 64
    # PCIe5 x16 ~62.7 GB/s; 80% => issue every ~14 ns (same spacing as max BW)
    inter_issue_ns = 14
    addr = 0x0000_0000

    with open(fname, "w") as f:
        f.write("# T7: Sequential reads at 80% BW, 100K requests (scrub interference baseline)\n")
        f.write("# RAS scrub controlled by config, not trace flags\n")
        ts = 0
        for _ in range(num_requests):
            _write_line(f, ts, "R", addr, cacheline, 0)
            addr += cacheline
            ts += inter_issue_ns

    print(f"T7: wrote {num_requests:,} requests -> {fname}")
    return num_requests


# ---------------------------------------------------------------------------
# T8 – Random reads, 8 requesters, 50K requests, then PPR marker at t=500us
# ---------------------------------------------------------------------------

def gen_t8(out_dir):
    """
    T8: hPPR event impact test.
    - Phase 1: 20K random reads, 8 requesters at 50ns open-loop inter-issue
    - PPR_MARKER injected at t=500us (halfway through closed-loop sim time)
    - Phase 2: 20K random reads continuing after PPR
    - RAS enabled config will show hPPR latency spike; CXL 3.0 (no RAS) shows flat
    """
    fname = os.path.join(out_dir, "T8_random_ppr.trace")
    num_requests_phase = 20_000   # requests per phase
    num_requesters = 8
    mem_range = 4 * (1 << 30)   # 4 GB
    inter_issue_ns = 50
    rng = random.Random(0xCAFE)

    # PPR marker timestamp: at midpoint of open-loop timestamps
    # Phase 1 ends at 20000 * 50ns = 1ms open-loop; use 500us as marker ts
    ppr_ts_ns = 500_000  # 500 μs — fires in the middle of closed-loop sim

    with open(fname, "w") as f:
        f.write("# T8: hPPR event test, 8 requesters, 40K requests; hPPR at t=500us\n")
        f.write("# Phase 1: pre-PPR random reads\n")
        ts = 0
        for i in range(num_requests_phase):
            rid = i % num_requesters
            addr = rng.randrange(0, mem_range, 64)
            _write_line(f, ts, "R", addr, 64, rid)
            ts += inter_issue_ns

        # PPR marker: embedded at ts=500us (between phase 1 and phase 2 requests)
        f.write(f"# PPR_MARKER ts={ppr_ts_ns} type=hard_ppr target_row=0x0000000200000000\n")

        f.write("# Phase 2: post-PPR random reads (affected by repair stall)\n")
        for i in range(num_requests_phase):
            rid = i % num_requesters
            addr = rng.randrange(0, mem_range, 64)
            _write_line(f, ts, "R", addr, 64, rid)
            ts += inter_issue_ns

    total = 2 * num_requests_phase
    print(f"T8: wrote {total:,} requests + PPR marker -> {fname}")
    return total


# ---------------------------------------------------------------------------
# T9 – N independent pointer-chase traces, 16 req/host — multi-host sharing
# ---------------------------------------------------------------------------

def gen_t9(out_dir, max_hosts=16):
    """
    T9: Multi-host sharing sweep.
    N hosts, each with 16 requesters issuing pointer-chase reads independently.
    Used to sweep downstream-link utilization: N=1..16 hosts share one CXL switch.
    max_hosts caps the sweep (e.g. 4 writes per-host h0..h3 + combined n=1,2,4).

    Produces two complementary sets of files:
      T9_multihost_n{N}.trace  — combined file for all N hosts; requester IDs
                                  encode host (host_id = rid // 16); useful for
                                  documentation and single-file ingestion.
      T9_mh_h{H}.trace         — per-host file (H = 0..15); requester IDs are
                                  0-15 (local); pass N of these as separate --traces
                                  args to the simulator for correct per-host credit
                                  pool routing.
    """
    host_counts = [n for n in (1, 2, 4, 8, 16) if n <= max_hosts]
    max_hosts = max(host_counts)
    requesters_per_host = 16
    reqs_per_requester = 50_000
    mem_range = 1 << 28   # 256 MB per host (separate ranges)
    spacing_ns = 300       # pointer-chase serialization spacing (ns)
    rng = random.Random(0xFACE)
    total = 0

    # Build per-host entry lists (deterministic; one RNG state per host)
    per_host_entries = []
    for host_id in range(max_hosts):
        host_rng = random.Random(0xFACE ^ (host_id * 0x1337))
        base_addr = host_id * mem_range
        entries = []
        for rid in range(requesters_per_host):
            ts = rid * spacing_ns
            addr = base_addr + host_rng.randrange(0, mem_range, 64)
            for _ in range(reqs_per_requester):
                entries.append((ts, "R", addr, 64, rid))
                addr = base_addr + host_rng.randrange(0, mem_range, 64)
                ts += spacing_ns
        entries.sort(key=lambda e: e[0])
        per_host_entries.append(entries)

    # Write per-host files (T9_mh_h{H}.trace) — used by experiment scripts
    for host_id in range(max_hosts):
        fname = os.path.join(out_dir, f"T9_mh_h{host_id}.trace")
        with open(fname, "w") as f:
            f.write(f"# T9: per-host file for host {host_id}, "
                    f"{requesters_per_host} requesters, pointer-chase\n")
            f.write(f"# Pass as one of N --traces args; simulator assigns host_id=file_index\n")
            for ts, op, addr, size, rid in per_host_entries[host_id]:
                _write_line(f, ts, op, addr, size, rid)
        print(f"T9 host={host_id:2d}: wrote {requesters_per_host * reqs_per_requester:,} "
              f"requests -> {fname}")

    # Write combined files (T9_multihost_n{N}.trace) — interleaved, for documentation
    for n_hosts in host_counts:
        fname = os.path.join(out_dir, f"T9_multihost_n{n_hosts}.trace")
        entries = []
        for host_id in range(n_hosts):
            base_addr = host_id * mem_range
            for ts, op, addr, size, local_rid in per_host_entries[host_id]:
                global_rid = host_id * requesters_per_host + local_rid
                # Offset timestamps so hosts appear concurrent (all start at t=0)
                entries.append((ts, op, addr + (base_addr - host_id * mem_range),
                                 size, global_rid))
        entries.sort(key=lambda e: e[0])
        with open(fname, "w") as f:
            f.write(f"# T9: Multi-host sharing, {n_hosts} host(s), "
                    f"{requesters_per_host} req/host, pointer-chase\n")
            f.write(f"# Requester IDs encode host: host_id = rid // {requesters_per_host}\n")
            for ts, op, addr, size, rid in entries:
                _write_line(f, ts, op, addr, size, rid)
        num_requests = n_hosts * requesters_per_host * reqs_per_requester
        total += num_requests
        print(f"T9 combined n={n_hosts:2d}: wrote {num_requests:,} requests -> {fname}")

    return total


# ---------------------------------------------------------------------------
# Dispatch
# ---------------------------------------------------------------------------

GENERATORS = {
    "T1": gen_t1,
    "T2": gen_t2,
    "T3": gen_t3,
    "T4": gen_t4,
    "T5": gen_t5,
    "T6": gen_t6,
    "T7": gen_t7,
    "T8": gen_t8,
    "T9": gen_t9,
}


def main():
    parser = argparse.ArgumentParser(
        description="Generate CXRay validation traces T1-T9."
    )
    parser.add_argument(
        "--trace",
        choices=list(GENERATORS.keys()),
        default=None,
        help="Which trace to generate (default: all)",
    )
    parser.add_argument(
        "--output",
        default=None,
        help="Output directory (default: validation/traces/ relative to repo root)",
    )
    parser.add_argument(
        "--max-hosts",
        type=int,
        default=16,
        help="T9 only: cap the multihost sweep (default: 16)",
    )
    args = parser.parse_args()

    # Resolve output directory
    if args.output:
        out_dir = args.output
    else:
        script_dir = os.path.dirname(os.path.abspath(__file__))
        out_dir = script_dir  # script lives in validation/traces/

    os.makedirs(out_dir, exist_ok=True)

    traces_to_run = [args.trace] if args.trace else list(GENERATORS.keys())

    grand_total = 0
    for key in traces_to_run:
        if key == "T9":
            count = gen_t9(out_dir, max_hosts=args.max_hosts)
        else:
            count = GENERATORS[key](out_dir)
        grand_total += count

    print(f"\nDone. Total requests written across all generated traces: {grand_total:,}")


if __name__ == "__main__":
    main()
