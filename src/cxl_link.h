#pragma once
#include <cstdint>
#include <string>
#include <algorithm>
#include <deque>
#include "cxl_sim_engine.h"

namespace cxl {

// ---------------------------------------------------------------------------
// LatencyBreakdown — per-component latency decomposition for one request.
// ---------------------------------------------------------------------------

struct LatencyBreakdown {
    double hostPortNs    = 0.0;  // host CXL controller TX overhead
    double linkPhysNs    = 0.0;  // PCIe5/6 physical propagation (board trace)
    double devicePortNs  = 0.0;  // CXL device controller RX + internal decode
    double fecNs         = 0.0;  // FEC decode overhead, both directions (Gen6 PAM4)
    double dramNs        = 0.0;  // DRAM access time
    double rasNs         = 0.0;  // RAS overhead (scrub, PPR, poison, viral)
    double coherenceNs   = 0.0;  // CXL.cache DCOH / snoop-filter lookup
    double ideNs         = 0.0;  // IDE (AES-256-GCM) encryption/decryption
    double returnNs      = 0.0;  // device TX + link RX + host completion path
    double creditStallNs = 0.0;  // credit back-pressure stall
    double retryNs       = 0.0;  // link-level replay/retry penalty (rare tail event)

    double totalNs() const {
        return hostPortNs + linkPhysNs + devicePortNs + fecNs +
               dramNs + rasNs + coherenceNs + ideNs +
               returnNs + creditStallNs + retryNs;
    }
};

// ---------------------------------------------------------------------------
// Per-FLIT-type credit pools (CXL 3.1 §4.2.3)
//   M2S_REQ  — host→device: requests without data (e.g. MemRd)
//   M2S_RWD  — host→device: requests with data   (e.g. MemWr)
//   S2M_NDR  — device→host: responses without data (e.g. MemRsp/ack)
//   S2M_DRS  — device→host: responses with data   (e.g. MemData/read compl.)
//
// Sizing: CXL spec minimum is 8 per pool. High-perf devices advertise 32–64.
// With 32 credits and RTT ≈ 210 ns: max pipeline throughput = 32*64B/210ns ≈ 9.8 GB/s.
// ---------------------------------------------------------------------------

struct CreditPools {
    int m2sReq = 32;  // outstanding MemRd credits (reads without data)
    int m2sRwd = 16;  // outstanding MemWr credits (writes carry data)
    int s2mNdr = 32;  // response ACK credits (non-data responses)
    int s2mDrs = 32;  // read data credits (data response slots)
};

// ---------------------------------------------------------------------------
// VCType — CXL Virtual Channel identifiers (CXL 3.1 §3.1)
//
//   CXL.io    (VC0): PCIe TLP traffic — config, MMIO, DMA, MSI/MSI-X.
//                    Typically small payloads (4–64 B).
//   CXL.mem   (VC1): Memory access traffic — M2S_REQ/RWD and S2M_NDR/DRS.
//                    64 B cacheline granularity.
//   CXL.cache (VC2): Cache coherence traffic — snoop requests, back-invalidations,
//                    DCOH. 64 B cacheline granularity.
//
// VCs share the physical PCIe lane bandwidth but have independent flow control
// and serialization queues, preventing Head-of-Line (HOL) blocking across VCs.
// ---------------------------------------------------------------------------

enum class VCType : uint8_t {
    CXL_IO    = 0,  // PCIe TLP: config, MMIO, DMA, MSI
    CXL_MEM   = 1,  // CXL.mem: M2S_REQ, M2S_RWD, S2M_NDR, S2M_DRS
    CXL_CACHE = 2,  // CXL.cache: snoop, BI, DCOH
    NUM_VCS   = 3
};

// ---------------------------------------------------------------------------
// LinkConfig — full CXL link configuration.
// ---------------------------------------------------------------------------

struct LinkConfig {
    int     pcieGen               = 5;      // 5 or 6
    int     pcieLanes             = 16;

    // Split one-way latency into three components:
    double  hostPortLatencyNs     = 15.0;   // host CXL TX processing
    double  linkPhysLatencyNs     = 3.0;    // PCIe5 physical propagation (~20 cm board trace)
    double  devicePortLatencyNs   = 12.0;   // device RX processing
    // Total one-way: hostPortLatencyNs + linkPhysLatencyNs + devicePortLatencyNs = 30 ns
    // The legacy linkLatencyNs field is kept for backward compatibility.
    // When nonzero it overrides the split fields for enqueueRequest.
    double  linkLatencyNs         = 0.0;    // 0 = use split fields (recommended)

    bool    fullDuplex            = true;

    // FEC (mandatory for PCIe Gen 6 PAM4; not present in Gen 5 NRZ).
    // fecDecodeLatencyNs is added per-direction (request + response = 2×).
    bool    fecEnable             = false;
    double  fecDecodeLatencyNs    = 0.0;    // ~4 ns for RS-FEC on Gen6

    // Link-level replay buffer (PAM4 uncorrectable errors → LTSSM retry).
    // With FEC, uncorrectable probability per FLIT ≈ 1e-10 at nominal margin.
    // When a retry occurs, one full RTT is burned re-sending the FLIT.
    bool    retryEnable           = true;
    double  retryProbPerFlit      = 0.0;    // P(retry) per FLIT; set by version defaults
    double  retryLatencyNs        = 300.0;  // replay-buffer RTT ≈ one link round-trip

    // Credit-stall estimation: when credit pool exhausted, stall ≈ RTT / credits.
    double  estimatedRttNs        = 210.0;  // calibrated to CXL 2.0; update per version

    // VC arbitration: small fixed overhead from VC selector pipeline stage.
    // Present even with single active VC (CXL.io + CXL.mem share the mux).
    // When > 0, this value overrides per-VC arbNs for all VCs (backward compat).
    double  vcArbitrationNs       = 1.0;

    int     maxOutstanding        = 256;    // total outstanding request credits
    int     queueDepth            = 64;     // device request queue depth

    // FLIT geometry — differs by generation:
    //   Gen 5 (CXL 1.1/2.0): 528-bit FLIT = 66 B total, 64 B payload
    //   Gen 6 (CXL 3.0/3.1): 256-byte FLIT, ~236 B usable payload (FEC + header overhead)
    int     flitSizeBytes         = 66;
    int     flitPayloadBytes      = 64;

    // Bundled Port (CXL 4.0): aggregate multiple physical ports into one logical device.
    // Bandwidth scales linearly; latency is the single-link floor (fastest constituent).
    bool bundledPortEnable   = false;
    int  bundledPortNumPorts = 1;

    // Per-FLIT-type credit pool sizes
    CreditPools credits;

    // -----------------------------------------------------------------------
    // Per-VC credit configuration
    // Each VC has an independent TX credit window and arbitration overhead,
    // enabling HOL-blocking prevention: a stall on VC0 does not delay VC1/VC2.
    // -----------------------------------------------------------------------
    struct VCCreditConfig {
        int    txCredits = 32;   // outstanding TX credits for this VC
        double arbNs     = 1.0;  // VC arbitration overhead (mux pipeline stage)
    };
    VCCreditConfig vcIO;     // CXL.io   (VC0) — default: 16 tx, 1.0 ns arb
    VCCreditConfig vcMem;    // CXL.mem  (VC1) — default: 32 tx, 1.0 ns arb
    VCCreditConfig vcCache;  // CXL.cache(VC2) — default: 16 tx, 0.5 ns arb

    LinkConfig() {
        vcIO.txCredits    = 16;  vcIO.arbNs    = 1.0;
        vcMem.txCredits   = 32;  vcMem.arbNs   = 1.0;
        vcCache.txCredits = 16;  vcCache.arbNs = 0.5;
    }

    // One-way link latency (symmetric): host_port + phys + device_port.
    // Both directions use the full sum so forward and return path are treated
    // identically, matching the Python simulator model.
    double effectiveLinkLatencyNs() const {
        if (linkLatencyNs > 0.0) return linkLatencyNs;
        return hostPortLatencyNs + linkPhysLatencyNs + devicePortLatencyNs;
    }
    double returnLinkLatencyNs() const {
        if (linkLatencyNs > 0.0) return linkLatencyNs;
        return hostPortLatencyNs + linkPhysLatencyNs + devicePortLatencyNs;
    }

    // Return the VCCreditConfig for a given VC.
    const VCCreditConfig& vcConfig(VCType vc) const {
        switch (vc) {
            case VCType::CXL_IO:    return vcIO;
            case VCType::CXL_CACHE: return vcCache;
            case VCType::CXL_MEM:
            default:                return vcMem;
        }
    }
};

// ---------------------------------------------------------------------------
// LinkStats
// ---------------------------------------------------------------------------

struct LinkStats {
    uint64_t totalFlitsTx    = 0;
    uint64_t totalBytesTx    = 0;   // M2S request payload bytes
    // Wire occupancy (flits × full flit size, both directions)
    uint64_t m2sWireBytesTx  = 0;   // downstream (request) wire bytes
    uint64_t s2mWireBytesTx  = 0;   // upstream  (response) wire bytes
    double   creditStallNs   = 0.0;
    double   simDurationNs   = 0.0;

    /// Per-VC stall accumulation (index = VCType enum value)
    double   vcStallNs[3]    = {0.0, 0.0, 0.0};

    /// Per-credit-type release counters (debug / validation)
    uint64_t m2sReqReleased  = 0;
    uint64_t m2sRwdReleased  = 0;
    uint64_t s2mDrsConsumed  = 0;
    uint64_t s2mNdrConsumed  = 0;
    double   s2mStallNs      = 0.0;

    double utilizationFraction() const {
        if (simDurationNs <= 0) return 0.0;
        return std::min(1.0, creditStallNs / simDurationNs);
    }
};

// ---------------------------------------------------------------------------
// CXLLink
// ---------------------------------------------------------------------------

class CXLLink {
public:
    explicit CXLLink(const LinkConfig& cfg);

    // Compute link bandwidth from PCIe gen + lanes.
    static double computeBandwidthGBps(int gen, int lanes);

    double bandwidthGBps() const { return bwGBps_; }
    double flitTxTimeNs()  const { return flitTxNs_; }

    // Enqueue a downstream request (host → device).
    // Returns dispatch_ns (when request arrives at device controller).
    // creditStallNsOut: stall added due to credit exhaustion.
    // isWrite: selects M2S_RWD vs M2S_REQ credit pool.
    // vc: Virtual Channel — each VC has its own credit pool and arbitration
    //     overhead, preventing HOL blocking across VCs.
    double enqueueRequest(double arrivalNs, uint32_t sizeBytes,
                          double& creditStallNsOut, bool isWrite = false,
                          VCType vc = VCType::CXL_MEM);

    // Enqueue an upstream response (device → host).
    // Returns complete_ns (when response arrives at host).
    // hasData: selects S2M_DRS vs S2M_NDR credit pool.
    // extraReturnNs: host-side latency added after link traversal (IDE + host overhead)
    //   to correctly determine when the S2M credit slot becomes available again.
    // vc: Virtual Channel — credits freed from the appropriate VC state.
    double enqueueResponse(double deviceDoneNs, uint32_t sizeBytes,
                           bool hasData = true, VCType vc = VCType::CXL_MEM,
                           double extraReturnNs = 0.0);

    // M2S (host→device) wire utilization fraction.  Matches Python utilization_aggregate.
    double upstreamUtilization(double totalSimNs) const;
    double m2sUtilization(double totalSimNs) const;
    // S2M (device→host) wire utilization fraction.
    double s2mUtilization(double totalSimNs) const;
    // Full-duplex aggregate: (M2S + S2M) wire bytes / (2 × BW × simNs).
    double aggregateUtilization(double totalSimNs) const;

    const LinkStats& stats() const { return stats_; }

    // Accumulate wire bytes from detailed mode (which bypasses enqueueRequest/enqueueResponse).
    void addWireBytes(uint64_t m2sWireBytes, uint64_t s2mWireBytes);

    // Record S2M credit consumption and stall from detailed mode.
    void addS2mStats(uint64_t drsConsumed, uint64_t ndrConsumed, double stallNs);

    // Build a latency breakdown for a completed request.
    // dramNs: DRAM access time (from backend); rasNs: RAS overhead.
    LatencyBreakdown buildBreakdown(double arrivalNs, double deviceArrivalNs,
                                   double deviceDoneNs,   double hostDoneNs,
                                   double dramNs,         double rasNs,
                                   double creditStallNs) const;

    // ── Detailed mode: event-driven pipeline using DES engine ────────────
    // Returns the same semantics as enqueueRequest but uses actual credit
    // tokens and FLIT serialization events instead of analytical estimates.
    // evq, credits, and lane must outlive this call (owned by caller).
    double enqueueRequestDetailed(double arrivalNs, uint32_t sizeBytes,
                                   double& creditStallNsOut, bool isWrite,
                                   SimEventQueue& evq,
                                   CreditTracker& credits,
                                   FlitSerializer& lane,
                                   VCType vc = VCType::CXL_MEM);

    double enqueueResponseDetailed(double deviceDoneNs, uint32_t sizeBytes,
                                    bool hasData,
                                    SimEventQueue& evq,
                                    FlitSerializer& respLane);

private:
    LinkConfig cfg_;
    double     bwGBps_;
    double     flitTxNs_;

    // BW enforcement: track when last FLIT ends per direction.
    // Physical lane is SHARED across all VCs — two flits from different VCs
    // cannot be transmitted simultaneously on the same physical link.
    double lastReqFlitEndNs_  = 0.0;
    double lastRespFlitEndNs_ = 0.0;

    // Outstanding request counter (total credits) — legacy synchronous path
    int outstanding_ = 0;

    // Per-FLIT-type outstanding credits (synchronous path)
    int outstandingM2sReq_ = 0;
    int outstandingM2sRwd_ = 0;
    std::deque<double> inflightM2sReqReturnNs_;
    std::deque<double> inflightM2sRwdReturnNs_;

    // Per-request type tracking: FIFO queue of isWrite flags for in-flight requests.
    // Allows enqueueResponse() to free the exact credit type consumed at enqueue.
    std::deque<bool> outstandingTypes_;   // true = M2S_RWD (write), false = M2S_REQ (read)

    // S2M response credit pools (CXL 3.1 §4.2.3)
    // Consumed when device sends a response; returned when host processes it.
    // S2M_DRS (hasData=true): read completions.  S2M_NDR (hasData=false): write ACKs.
    int outstandingS2mDrs_ = 0;
    int outstandingS2mNdr_ = 0;
    // Timestamps of expected credit returns (≈ host_done_ns for each inflight response).
    std::deque<double> inflightS2mDrsReturnNs_;
    std::deque<double> inflightS2mNdrReturnNs_;

    // -----------------------------------------------------------------------
    // Per-VC state — enables HOL blocking prevention.
    // Each VC has an independent credit window and stall accumulator.
    // A stall on VC0 does NOT delay enqueue on VC1 or VC2.
    // The physical lane (lastReqFlitEndNs_) remains shared.
    // -----------------------------------------------------------------------
    struct VCState {
        /// Time-aware sliding-window credit tracker.
        /// Each entry holds the simulated time at which one TX credit returns
        /// (= issueNs + estimatedRttNs).  Entries are sorted earliest-first.
        std::deque<double> inflightReturnNs;

        /// When this VC last finished transmitting on the physical lane.
        /// Used for VC-level serialization visibility (informational).
        double lastFlitEndNs  = 0.0;

        /// Accumulated HOL stall time attributed to this VC.
        double totalStallNs   = 0.0;
    };
    VCState vcState_[static_cast<int>(VCType::NUM_VCS)];

    // Lightweight xorshift32 PRNG for stochastic retry simulation.
    // Avoids pulling in <random> and keeps link model self-contained.
    mutable uint32_t rngState_ = 0xDEADBEEFu;
    double nextUniform() const;   // [0, 1)

    LinkStats stats_;
};

}  // namespace cxl
