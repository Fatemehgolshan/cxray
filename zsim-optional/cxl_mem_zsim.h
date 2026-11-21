#pragma once
// CXLMemory — CXL memory controller for zsim.
//
// Models a CXL memory device as a protocol-overhead wrapper around zsim's
// own cycle-accurate DDRMemory backend.  Four features are modeled on top of
// the basic forward/return latency decomposition:
//
//   1. PCIe credit-pool back-pressure (CXL 3.1 §4.2.3)
//      M2S_REQ (reads) and M2S_RWD (writes) have independent credit pools.
//      When outstanding credits hit the limit, new requests stall until the
//      oldest in-flight credit returns (≈ one RTT).
//
//   2. CXL switch / fabric latency (CXL 2.0+ HBR, CXL 3.x PBR)
//      A configurable per-hop switch cut-through latency is added to both
//      the forward and return paths.  Multi-hop fabrics multiply by numHops.
//
//   3. Multi-host / GIM fabric arbitration (CXL 3.0+)
//      When numDevices > 1 (Global Interleave Memory), a fabric arbitration
//      overhead (fabricLatNs) is added to the forward path per request.
//
//   4. CXL device type: Type 3 (memory expander) vs. Type 2 (accelerator+HDM)
//      Type 2 adds a DCOH snoop-filter lookup on every request (coherenceNs)
//      and a Back-Invalidation (BI) round-trip on host write requests
//      (biRttNs) to model the device flushing a dirty M-state cacheline.
//
// Total request latency breakdown:
//   creditStall + (hostOverhead + link_fwd + coherence + switch_fwd + fabric)
//                + DRAM
//                + (link_ret + switch_ret [+ biRtt if Type2 write])
//
// Statistics (HPCA'27 revision, Sec. VII): the controller exports, per request
// class (demand read / demand write / migration read / migration write),
// request counts, end-to-end latency totals and 8 ns-bucket latency
// histograms, credit-stall cycles and stalled-request counts per pool
// (M2S_REQ / M2S_RWD) and per source (demand / migration), the peak number of
// simultaneously outstanding credits per pool, and demand-traffic statistics
// restricted to the window of a migration burst.  Demand latencies are
// measured in the weave phase (they include DRAM queuing); migration
// requests carry no timing events and use their bound-phase latency.

#include "memory_hierarchy.h"
#include "g_std/g_string.h"
#include "locks.h"
#include "stats.h"
#include <cstdint>

class DDRMemory;

class CXLMemory : public MemObject {
public:
    // -----------------------------------------------------------------------
    // Config — all tunable parameters for one CXL memory endpoint.
    // -----------------------------------------------------------------------
    struct Config {
        // Basic per-path overhead (ns)
        uint32_t hostOverheadNs = 97;   // host CXL controller TX processing
        uint32_t linkOneWayNs   = 24;   // PCIe link one-way propagation

        // --- Credit-pool back-pressure (CXL 3.1 §4.2.3) ---
        // Set creditLimitReq = 0 to disable back-pressure modeling.
        uint32_t creditLimitReq = 32;   // M2S_REQ outstanding read credits
        uint32_t creditLimitRwd = 16;   // M2S_RWD outstanding write credits
        uint32_t rttNs          = 0;    // estimated RTT for credit return
                                        // (0 = auto: 2*(hostOverheadNs+linkOneWayNs))

        // --- Switch / fabric (CXL 2.0+ HBR, CXL 3.x PBR) ---
        bool     hasSwitch      = false;
        uint32_t switchLatNs    = 50;   // per-hop cut-through + arb latency (ns)
        uint32_t switchHops     = 1;    // hops to CXL device (symmetric both dirs)

        // --- Multi-host / GIM (CXL 3.0+) ---
        uint32_t numDevices     = 1;    // interleaved CXL devices (1 = no GIM)
        uint32_t fabricLatNs    = 0;    // extra fabric arbitration per request (ns)

        // --- Device type and coherence ---
        // deviceType 3: Type 3 memory expander (no device-side cache)
        // deviceType 2: Type 2 accelerator+HDM — adds DCOH snoop filter and BI
        uint32_t deviceType     = 3;
        uint32_t coherenceNs    = 0;    // DCOH lookup latency (ns; 3-5 ns typical)
        uint32_t biRttNs        = 0;    // BI round-trip on host writes (Type 2; ns)

        // --- Contention-simulation domain of this controller's timing events ---
        // (informational for the statistics; does not change timing)
        uint32_t domain         = 0;
    };

    // Request-class tag.  A MemReq whose flags carry this bit is migration
    // traffic issued by the tiering model (TieringMemory); everything else is
    // demand traffic.  The bit is above MemReq::Flag's range.
    static constexpr uint32_t FLAG_MIGRATION = (1u << 30);

    // Latency histogram geometry, three segments plus overflow:
    //   bins [0, 500)      : 8 ns each      (0 .. 4 us)
    //   bins [500, 1000)   : 128 ns each    (4 us .. 68 us)
    //   bins [1000, 1500)  : 2 us each      (68 us .. 1068 us)
    //   bin  1500          : overflow (>= 1068 us)
    static constexpr uint32_t HIST_BIN_NS      = 8;
    static constexpr uint32_t HIST_BINS        = 500;
    static constexpr uint32_t HIST_COARSE_NS   = 128;
    static constexpr uint32_t HIST_COARSE_BINS = 500;
    static constexpr uint32_t HIST_TAIL_NS     = 2000;
    static constexpr uint32_t HIST_TAIL_BINS   = 500;
    static constexpr uint32_t HIST_SIZE        = HIST_BINS + HIST_COARSE_BINS + HIST_TAIL_BINS + 1;

    enum ReqClass { DEMAND_RD = 0, DEMAND_WR = 1, MIG_RD = 2, MIG_WR = 3, NUM_CLASSES = 4 };
    enum Pool     { POOL_REQ = 0, POOL_RWD = 1, NUM_POOLS = 2 };
    enum Source   { SRC_DEMAND = 0, SRC_MIG = 1, NUM_SOURCES = 2 };

    // Primary constructor (all features)
    CXLMemory(DDRMemory* ddrBackend, const Config& cfg, uint32_t freqMHz,
              const g_string& name);

    // Legacy constructor — only hostOverheadNs + linkOneWayNs; credit pool and
    // other features disabled.  Keeps existing configs (xeon_cxl_*.cfg) working.
    CXLMemory(DDRMemory* ddrBackend, uint32_t hostOverheadNs, uint32_t linkOneWayNs,
              uint32_t freqMHz, const g_string& name);

    // MemObject interface
    uint64_t access(MemReq& req) override;
    void initStats(AggregateStat* parentStat) override;
    const char* getName() override { return name_.c_str(); }

    // Migration-burst bookkeeping.  TieringMemory brackets each batch of
    // migration requests (one epoch boundary) with these calls; the window
    // [first issue cycle, last dispatch cycle] of the batch is the "burst",
    // and demand requests issued inside it are counted separately.
    void migrationBatchBegin();
    void migrationBatchEnd();

    // Weave-phase completion callback (used by CXLEndEvent).
    void recordWeaveLatency(uint32_t cls, bool inBurst, uint64_t latCycles);

    uint64_t getRttCycles() const { return rttCycles_; }
    uint32_t getCreditLimitReq() const { return creditLimitReq_; }
    uint32_t getCreditLimitRwd() const { return creditLimitRwd_; }

private:
    // Credit ring-buffer capacity.  Must be >= max(creditLimitReq, creditLimitRwd).
    // Power-of-2 so we can use bitwise AND for fast modulo.
    static constexpr int CREDIT_BUF = 128;
    static constexpr int CREDIT_MASK = CREDIT_BUF - 1;

    g_string    name_;
    DDRMemory*  ddrBackend_;

    // Fixed-overhead cycle counts (computed once in constructor)
    uint64_t    cxlFwdCycles_;  // host + link_fwd + coherence + switch_fwd + fabric
    uint64_t    cxlRetCycles_;  // link_ret + switch_ret

    // Credit-pool configuration
    uint32_t    creditLimitReq_;  // 0 = disabled
    uint32_t    creditLimitRwd_;  // 0 = disabled
    uint64_t    rttCycles_;

    // Sliding-window ring buffers: each entry holds the cycle at which one
    // in-flight credit returns (= dispatch_cycle + rttCycles_).
    uint64_t    reqReturnAt_[CREDIT_BUF];
    int         reqHead_ = 0, reqTail_ = 0, reqCount_ = 0;

    uint64_t    rwdReturnAt_[CREDIT_BUF];
    int         rwdHead_ = 0, rwdTail_ = 0, rwdCount_ = 0;

    // Type 2 CXL.cache: Back-Invalidation overhead
    bool        isType2_       = false;
    uint64_t    biRttCycles_   = 0;

    // Contention-sim domain for the controller's own timing events
    uint32_t    domain_        = 0;
    uint64_t    histBinCycles_ = 1;        // cycles per fine bucket (HIST_BIN_NS ns)
    uint64_t    histCoarseCycles_ = 1;     // cycles per coarse bucket (HIST_COARSE_NS ns)
    uint64_t    histTailCycles_ = 1;       // cycles per tail bucket (HIST_TAIL_NS ns)
    uint64_t    histFineLimit_ = 0;        // cycles covered by the fine segment
    uint64_t    histCoarseLimit_ = 0;      // cycles covered by fine + coarse segments

    // Lock — same pattern as DDRMemory; protects credit ring buffers and the
    // bound-phase statistics below.
    lock_t      lock_;

    // ---- Statistics updated in the bound phase (under lock_) ----
    VectorCounter reqs_;              // [NUM_CLASSES] requests
    VectorCounter boundLat_;          // [NUM_CLASSES] bound-phase latency (stall + fwd + DDR-min + ret)
    VectorCounter creditStallCycles_; // [pool*NUM_SOURCES + src] credit-stall cycles
    VectorCounter creditStalled_;     // [pool*NUM_SOURCES + src] requests that stalled >= 1 cycle
    VectorCounter clsStallCycles_;    // [NUM_CLASSES] credit-stall cycles by class (GETX counts as rd but stalls on RWD)
    VectorCounter clsStalled_;        // [NUM_CLASSES] requests that stalled >= 1 cycle, by class
    Counter       peakOutstandingReq_, peakOutstandingRwd_;
    Counter       bursts_, burstCycles_, burstReqsIssued_;
    VectorCounter burstDemandReqs_;   // [2: demand rd, demand wr] demand requests issued inside a burst window
    VectorCounter burstDemandStalled_;      // [2] of those, stalled on their credit pool
    VectorCounter burstDemandStallCycles_;  // [2]
    // ---- Statistics updated in the weave phase (contention thread of domain_) ----
    VectorCounter lat_;               // [NUM_CLASSES] end-to-end latency cycles
    VectorCounter hist_[NUM_CLASSES]; // [NUM_CLASSES][HIST_SIZE]
    VectorCounter burstDemandLat_;    // [2] in-burst demand latency cycles
    VectorCounter burstHist_[2];      // [2][HIST_SIZE]

    // Burst window state (bound-phase cycles; under lock_)
    bool        inBatch_ = false;
    uint64_t    batchReqs_ = 0, batchStart_ = 0, batchEnd_ = 0;
    uint64_t    lastBurstStart_ = 0, lastBurstEnd_ = 0;

    // Helpers
    void     init_(const Config& cfg, uint32_t freqMHz);
    uint64_t nsToCy_(uint32_t ns, uint32_t freqMHz) const;
    uint32_t histBin_(uint64_t latCycles) const {
        if (latCycles < histFineLimit_) return (uint32_t)(latCycles / histBinCycles_);
        if (latCycles < histCoarseLimit_) return HIST_BINS + (uint32_t)((latCycles - histFineLimit_) / histCoarseCycles_);
        uint64_t b = HIST_BINS + HIST_COARSE_BINS + (latCycles - histCoarseLimit_) / histTailCycles_;
        return (b >= HIST_SIZE - 1) ? (HIST_SIZE - 1) : (uint32_t)b;
    }

    // Acquire one credit from pool [returnAt/head/tail/count/limit].
    // Expires stale credits (returnAt <= reqCycle), stalls if pool exhausted,
    // then issues a new credit with returnAt = (reqCycle+stall) + rttCycles_.
    // Returns the stall cycles added (0 when pool is not exhausted).
    uint64_t acquireCredit_(uint64_t* returnAt, int& head, int& tail, int& count,
                            int limit, uint64_t reqCycle);
};
