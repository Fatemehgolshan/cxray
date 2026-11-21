// CXL memory backend for zsim.
//
// Wraps zsim's DDRMemory with a CXL protocol overhead model that accounts for:
//
//   1. PCIe credit-pool back-pressure (CXL 3.1 §4.2.3)
//      M2S_REQ (reads) and M2S_RWD (writes) each have an independent sliding-
//      window credit pool.  When the pool is exhausted the request stalls until
//      the oldest in-flight credit returns (≈ one RTT).
//
//   2. CXL switch / fabric latency (CXL 2.0+ HBR, CXL 3.x PBR)
//      Per-hop cut-through latency is added to both the forward and return
//      paths.  Multi-hop fabrics multiply by switchHops.
//
//   3. Multi-host / GIM fabric arbitration (CXL 3.0+)
//      When numDevices > 1 an extra fabricLatNs overhead is added per request
//      to model Global Interleave Memory address-decode and arbitration.
//
//   4. CXL device type: Type 3 (memory expander) vs. Type 2 (accelerator+HDM)
//      Type 2 adds a DCOH snoop-filter lookup on every request (coherenceNs)
//      and a Back-Invalidation round-trip on host write requests (biRttNs).
//
// Total latency:
//   creditStall
//   + (hostOverhead + link_fwd + coherence + switch_fwd + fabric)   <- cxlFwdCycles_
//   + DRAM
//   + (link_ret + switch_ret [+ biRtt if Type2 write])              <- cxlRetCycles_ [+ biRttCycles_]
//
// Statistics: see cxl_mem_zsim.h.  The end-to-end latency of a demand request
// is measured in the weave phase by wrapping the DDR backend's timing record
// with two marker events (CXLStartEvent -> DDR events -> CXLEndEvent), so the
// recorded value includes DRAM queuing, the credit stall and the fixed CXL
// overheads.  Requests without a timing recorder (migration traffic issued by
// TieringMemory, or non-timing cores) record their bound-phase latency.

#include "cxl_mem_zsim.h"
#include "ddr_mem.h"
#include "event_recorder.h"
#include "log.h"
#include "timing_event.h"
#include "zsim.h"
#include <algorithm>

// ---------------------------------------------------------------------------
// Weave-phase marker events
// ---------------------------------------------------------------------------

class CXLEndEvent : public TimingEvent {
    private:
        CXLMemory* mem_;
        uint32_t   cls_;
        bool       inBurst_;
        uint64_t   fixedCycles_;   // credit stall + forward overhead (before the DDR start marker)
        uint64_t   startCycle_;    // set by CXLStartEvent when the request enters the DDR backend

    public:
        CXLEndEvent(CXLMemory* mem, uint32_t cls, bool inBurst, uint64_t fixedCycles,
                    uint32_t retDelay, int32_t domain)
            : TimingEvent(retDelay, 0, domain), mem_(mem), cls_(cls), inBurst_(inBurst),
              fixedCycles_(fixedCycles), startCycle_(0) {}

        void setStartCycle(uint64_t c) { startCycle_ = c; }

        void simulate(uint64_t startCycle) override {
            uint64_t lat = fixedCycles_ + ((startCycle > startCycle_) ? (startCycle - startCycle_) : 0);
            mem_->recordWeaveLatency(cls_, inBurst_, lat);
            done(startCycle);
        }
};

class CXLStartEvent : public TimingEvent {
    private:
        CXLEndEvent* endEv_;

    public:
        CXLStartEvent(CXLEndEvent* endEv, int32_t domain)
            : TimingEvent(0, 0, domain), endEv_(endEv) {}

        void simulate(uint64_t startCycle) override {
            endEv_->setStartCycle(startCycle);
            done(startCycle);
        }
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

uint64_t CXLMemory::nsToCy_(uint32_t ns, uint32_t freqMHz) const {
    // Round up so we never undercount latency.
    return ((uint64_t)ns * freqMHz + 999u) / 1000u;
}

void CXLMemory::init_(const Config& cfg, uint32_t freqMHz) {
    futex_init(&lock_);

    // Forward overhead: host controller + one-way link + DCOH (Type 2) +
    //                   switch forward hops + GIM fabric arbitration.
    uint32_t switchFwdNs = cfg.hasSwitch ? cfg.switchLatNs * cfg.switchHops : 0;
    uint32_t fabricNs    = (cfg.numDevices > 1) ? cfg.fabricLatNs : 0;
    uint32_t fwdNs = cfg.hostOverheadNs + cfg.linkOneWayNs
                   + cfg.coherenceNs + switchFwdNs + fabricNs;
    cxlFwdCycles_ = nsToCy_(fwdNs, freqMHz);

    // Return overhead: one-way link + switch return hops.
    uint32_t switchRetNs = cfg.hasSwitch ? cfg.switchLatNs * cfg.switchHops : 0;
    uint32_t retNs = cfg.linkOneWayNs + switchRetNs;
    cxlRetCycles_ = nsToCy_(retNs, freqMHz);

    // Credit pools.
    creditLimitReq_ = cfg.creditLimitReq;
    creditLimitRwd_ = cfg.creditLimitRwd;
    uint32_t rtt = cfg.rttNs ? cfg.rttNs
                              : 2 * (cfg.hostOverheadNs + cfg.linkOneWayNs);
    rttCycles_ = nsToCy_(rtt, freqMHz);

    // Zero-initialise ring buffers.
    for (int i = 0; i < CREDIT_BUF; i++) reqReturnAt_[i] = rwdReturnAt_[i] = 0;
    reqHead_ = reqTail_ = reqCount_ = 0;
    rwdHead_ = rwdTail_ = rwdCount_ = 0;

    // Type 2 coherence / BI.
    isType2_     = (cfg.deviceType == 2);
    biRttCycles_ = isType2_ ? nsToCy_(cfg.biRttNs, freqMHz) : 0;

    // Statistics geometry.
    domain_ = cfg.domain;
    histBinCycles_ = std::max<uint64_t>(1, nsToCy_(HIST_BIN_NS, freqMHz));
    histCoarseCycles_ = std::max<uint64_t>(1, nsToCy_(HIST_COARSE_NS, freqMHz));
    histTailCycles_ = std::max<uint64_t>(1, nsToCy_(HIST_TAIL_NS, freqMHz));
    histFineLimit_ = histBinCycles_ * HIST_BINS;
    histCoarseLimit_ = histFineLimit_ + histCoarseCycles_ * HIST_COARSE_BINS;

    inBatch_ = false;
    batchReqs_ = batchStart_ = batchEnd_ = 0;
    lastBurstStart_ = lastBurstEnd_ = 0;

    info("%s: CXL fwd %ld cy, ret %ld cy, rtt %ld cy, credits REQ %d RWD %d, hist bin %ld cy",
         name_.c_str(), cxlFwdCycles_, cxlRetCycles_, rttCycles_, creditLimitReq_, creditLimitRwd_, histBinCycles_);
}

// ---------------------------------------------------------------------------
// Constructors
// ---------------------------------------------------------------------------

CXLMemory::CXLMemory(DDRMemory* ddrBackend, const Config& cfg,
                     uint32_t freqMHz, const g_string& name)
    : name_(name), ddrBackend_(ddrBackend)
{
    init_(cfg, freqMHz);
}

CXLMemory::CXLMemory(DDRMemory* ddrBackend, uint32_t hostOverheadNs,
                     uint32_t linkOneWayNs, uint32_t freqMHz,
                     const g_string& name)
    : name_(name), ddrBackend_(ddrBackend)
{
    Config cfg;
    cfg.hostOverheadNs  = hostOverheadNs;
    cfg.linkOneWayNs    = linkOneWayNs;
    cfg.creditLimitReq  = 0;  // disabled in legacy mode
    cfg.creditLimitRwd  = 0;
    init_(cfg, freqMHz);
}

// ---------------------------------------------------------------------------
// Credit pool
// ---------------------------------------------------------------------------

// Acquire one credit from the sliding-window pool described by
// [returnAt / head / tail / count / limit].
//
// Algorithm:
//   1. Expire stale credits: pop from head while returnAt[head] <= reqCycle.
//   2. If pool exhausted (count >= limit), stall until oldest credit returns.
//      The stall advances reqCycle and shifts the new credit's return time.
//   3. Issue a new credit: push returnAt = (reqCycle + rttCycles_) at tail.
//
// Returns the number of stall cycles added (0 when pool is not exhausted).
// Caller must hold lock_.
uint64_t CXLMemory::acquireCredit_(uint64_t* returnAt, int& head, int& tail,
                                   int& count, int limit, uint64_t reqCycle)
{
    if (limit == 0) return 0;  // pool disabled

    // 1. Expire credits that have already returned.
    while (count > 0 && returnAt[head] <= reqCycle) {
        head = (head + 1) & CREDIT_MASK;
        count--;
    }

    // 2. Stall if pool exhausted.
    uint64_t stall = 0;
    if (count >= limit) {
        // Oldest in-flight credit returns at returnAt[head].
        uint64_t unblockAt = returnAt[head];
        if (unblockAt > reqCycle) {
            stall = unblockAt - reqCycle;
            reqCycle = unblockAt;
        }
        head = (head + 1) & CREDIT_MASK;
        count--;
    }

    // 3. Issue a new credit.
    assert(count < CREDIT_BUF);  // ring buffer must not overflow
    returnAt[tail] = reqCycle + rttCycles_;
    tail = (tail + 1) & CREDIT_MASK;
    count++;

    return stall;
}

// ---------------------------------------------------------------------------
// Migration-burst bookkeeping (called by TieringMemory)
// ---------------------------------------------------------------------------

void CXLMemory::migrationBatchBegin() {
    futex_lock(&lock_);
    inBatch_ = true;
    batchReqs_ = 0;
    batchStart_ = batchEnd_ = 0;
    futex_unlock(&lock_);
}

void CXLMemory::migrationBatchEnd() {
    futex_lock(&lock_);
    if (inBatch_ && batchReqs_ > 0) {
        bursts_.inc();
        burstCycles_.inc(batchEnd_ - batchStart_);
        burstReqsIssued_.inc(batchReqs_);
        lastBurstStart_ = batchStart_;
        lastBurstEnd_   = batchEnd_;
    }
    inBatch_ = false;
    futex_unlock(&lock_);
}

void CXLMemory::recordWeaveLatency(uint32_t cls, bool inBurst, uint64_t latCycles) {
    // Weave phase: one contention thread per domain, never concurrent with the bound phase.
    lat_.inc(cls, latCycles);
    hist_[cls].inc(histBin_(latCycles));
    if (inBurst && cls <= DEMAND_WR) {
        burstDemandLat_.inc(cls, latCycles);
        burstHist_[cls].inc(histBin_(latCycles));
    }
}

// ---------------------------------------------------------------------------
// MemObject interface
// ---------------------------------------------------------------------------

uint64_t CXLMemory::access(MemReq& req) {
    // Clean writebacks carry no data back to the requester; charge nothing.
    if (req.type == PUTS) {
        *req.state = I;
        return req.cycle;
    }

    // Determine whether this is a write (PUTX / GETX) for BI purposes.
    bool isWrite = (req.type == PUTX || req.type == GETX);

    // Set MESI state (memory is always the last level and always grants).
    switch (req.type) {
        case PUTX:  *req.state = I;                                  break;
        case GETS:  *req.state = req.is(MemReq::NOEXCL) ? S : E;    break;
        case GETX:  *req.state = M;                                  break;
        default:    panic("CXLMemory::access: unexpected type %d", (int)req.type);
    }

    uint64_t origCycle = req.cycle;
    bool     isMig = (req.flags & FLAG_MIGRATION) != 0;
    // Statistics class follows the data direction (GETS and GETX return a line: "rd"; PUTX writes one: "wr"),
    // matching the DDR backend's rd/wr accounting.  The credit pool follows the existing model (GETX uses RWD).
    bool     isDataRead = (req.type == GETS || req.type == GETX);
    uint32_t cls   = (isMig ? MIG_RD : DEMAND_RD) + (isDataRead ? 0 : 1);
    uint32_t pool  = isWrite ? POOL_RWD : POOL_REQ;
    uint32_t src   = isMig ? SRC_MIG : SRC_DEMAND;

    // -----------------------------------------------------------------------
    // Credit-pool back-pressure + bound-phase statistics (under lock).
    // -----------------------------------------------------------------------
    uint64_t creditStall = 0;
    bool inBurst = false;
    futex_lock(&lock_);
    if (isWrite) {
        if (creditLimitRwd_ > 0) {
            creditStall = acquireCredit_(rwdReturnAt_, rwdHead_, rwdTail_,
                                         rwdCount_, (int)creditLimitRwd_,
                                         origCycle);
            if ((uint64_t)rwdCount_ > peakOutstandingRwd_.get()) peakOutstandingRwd_.set(rwdCount_);
        }
    } else {
        if (creditLimitReq_ > 0) {
            creditStall = acquireCredit_(reqReturnAt_, reqHead_, reqTail_,
                                         reqCount_, (int)creditLimitReq_,
                                         origCycle);
            if ((uint64_t)reqCount_ > peakOutstandingReq_.get()) peakOutstandingReq_.set(reqCount_);
        }
    }

    reqs_.inc(cls);
    if (creditStall) {
        creditStallCycles_.inc(pool * NUM_SOURCES + src, creditStall);
        creditStalled_.inc(pool * NUM_SOURCES + src);
        clsStallCycles_.inc(cls, creditStall);
        clsStalled_.inc(cls);
    }
    if (isMig) {
        if (inBatch_) {
            if (batchReqs_ == 0) batchStart_ = batchEnd_ = origCycle;
            batchEnd_ = std::max(batchEnd_, origCycle + creditStall);
            batchReqs_++;
        }
    } else {
        inBurst = (lastBurstEnd_ > 0 && origCycle >= lastBurstStart_ && origCycle <= lastBurstEnd_);
        if (inBurst) {
            burstDemandReqs_.inc(cls);
            if (creditStall) {
                burstDemandStalled_.inc(cls);
                burstDemandStallCycles_.inc(cls, creditStall);
            }
        }
    }
    futex_unlock(&lock_);

    // -----------------------------------------------------------------------
    // Forward path: credit stall + CXL overhead, then DDR.
    // -----------------------------------------------------------------------
    req.cycle = origCycle + creditStall + cxlFwdCycles_;
    uint64_t ddrDone = ddrBackend_->access(req);

    // Restore req.cycle for the caller (zsim convention).
    req.cycle = origCycle;

    // -----------------------------------------------------------------------
    // Return path: link return + switch return [+ BI RTT for Type 2 writes].
    // -----------------------------------------------------------------------
    uint64_t retDelay = cxlRetCycles_ + ((isType2_ && isWrite) ? biRttCycles_ : 0);
    uint64_t ret = ddrDone + retDelay;

    // -----------------------------------------------------------------------
    // End-to-end latency statistics.  If the DDR backend produced a timing
    // record (timing core), wrap it so the latency is measured in the weave
    // phase; otherwise record the bound-phase latency now.
    // -----------------------------------------------------------------------
    EventRecorder* evRec = zinfo->eventRecorders[req.srcId];
    if (evRec && evRec->hasRecord()) {
        TimingRecord tr = evRec->popRecord();
        CXLEndEvent* endEv = new (evRec) CXLEndEvent(this, cls, inBurst, creditStall + cxlFwdCycles_,
                                                     (uint32_t)retDelay, domain_);
        CXLStartEvent* startEv = new (evRec) CXLStartEvent(endEv, domain_);
        startEv->setMinStartCycle(tr.reqCycle);
        endEv->setMinStartCycle(ret);
        startEv->addChild(tr.startEvent, evRec);
        tr.endEvent->addChild(endEv, evRec);
        tr.startEvent = startEv;
        tr.endEvent   = endEv;
        tr.respCycle  = ret;
        evRec->pushRecord(tr);
    } else {
        futex_lock(&lock_);
        lat_.inc(cls, ret - origCycle);
        hist_[cls].inc(histBin_(ret - origCycle));
        futex_unlock(&lock_);
    }
    futex_lock(&lock_);
    boundLat_.inc(cls, ret - origCycle);
    futex_unlock(&lock_);

    return ret;
}

void CXLMemory::initStats(AggregateStat* parentStat) {
    AggregateStat* s = new AggregateStat();
    s->init(name_.c_str(), "CXL memory (DDR backend) stats");
    ddrBackend_->initStats(s);

    static const char* clsNames[NUM_CLASSES]   = {"demandRd", "demandWr", "migRd", "migWr"};
    static const char* poolSrcNames[NUM_POOLS * NUM_SOURCES] = {"reqDemand", "reqMig", "rwdDemand", "rwdMig"};
    static const char* rwNames[2] = {"rd", "wr"};

    reqs_.init("cxlReqs", "CXL requests by class", NUM_CLASSES, clsNames); s->append(&reqs_);
    lat_.init("cxlLat", "CXL end-to-end latency cycles by class (weave-phase for demand)", NUM_CLASSES, clsNames); s->append(&lat_);
    boundLat_.init("cxlBoundLat", "CXL bound-phase latency cycles by class", NUM_CLASSES, clsNames); s->append(&boundLat_);
    creditStallCycles_.init("cxlCreditStallCycles", "Credit-stall cycles by pool (REQ/RWD) and source (demand/migration)", NUM_POOLS * NUM_SOURCES, poolSrcNames); s->append(&creditStallCycles_);
    creditStalled_.init("cxlCreditStalled", "Requests that stalled >= 1 cycle on a credit, by pool and source", NUM_POOLS * NUM_SOURCES, poolSrcNames); s->append(&creditStalled_);
    clsStallCycles_.init("cxlClsStallCycles", "Credit-stall cycles by request class", NUM_CLASSES, clsNames); s->append(&clsStallCycles_);
    clsStalled_.init("cxlClsStalled", "Requests that stalled >= 1 cycle on a credit, by request class", NUM_CLASSES, clsNames); s->append(&clsStalled_);
    peakOutstandingReq_.init("cxlPeakOutstandingReq", "Peak simultaneous outstanding M2S_REQ credits"); s->append(&peakOutstandingReq_);
    peakOutstandingRwd_.init("cxlPeakOutstandingRwd", "Peak simultaneous outstanding M2S_RWD credits"); s->append(&peakOutstandingRwd_);
    bursts_.init("cxlMigBursts", "Migration batches (epochs) with >= 1 migration request"); s->append(&bursts_);
    burstCycles_.init("cxlMigBurstCycles", "Sum over bursts of (last dispatch - first issue) cycles"); s->append(&burstCycles_);
    burstReqsIssued_.init("cxlMigBurstReqs", "Migration requests issued inside recorded bursts"); s->append(&burstReqsIssued_);
    burstDemandReqs_.init("cxlBurstDemandReqs", "Demand requests issued inside a migration-burst window", 2, rwNames); s->append(&burstDemandReqs_);
    burstDemandStalled_.init("cxlBurstDemandStalled", "In-burst demand requests that stalled on a credit", 2, rwNames); s->append(&burstDemandStalled_);
    burstDemandStallCycles_.init("cxlBurstDemandStallCycles", "In-burst demand credit-stall cycles", 2, rwNames); s->append(&burstDemandStallCycles_);
    burstDemandLat_.init("cxlBurstDemandLat", "In-burst demand end-to-end latency cycles", 2, rwNames); s->append(&burstDemandLat_);

    static const char* histNames[NUM_CLASSES] = {"cxlHistDemandRd", "cxlHistDemandWr", "cxlHistMigRd", "cxlHistMigWr"};
    for (uint32_t c = 0; c < NUM_CLASSES; c++) {
        hist_[c].init(histNames[c], "End-to-end latency histogram: 500 x 8 ns bins (0-4 us), 500 x 128 ns bins (4-68 us), 500 x 2 us bins (68-1068 us), last bin = overflow", HIST_SIZE);
        s->append(&hist_[c]);
    }
    static const char* burstHistNames[2] = {"cxlBurstHistDemandRd", "cxlBurstHistDemandWr"};
    for (uint32_t c = 0; c < 2; c++) {
        burstHist_[c].init(burstHistNames[c], "In-burst demand latency histogram: 500 x 8 ns bins (0-4 us), 500 x 128 ns bins (4-68 us), 500 x 2 us bins (68-1068 us), last bin = overflow", HIST_SIZE);
        s->append(&burstHist_[c]);
    }

    parentStat->append(s);
}
