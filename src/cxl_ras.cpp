// ---------------------------------------------------------------------------
// cxl_ras.cpp — CXL 3.1 RAS Engine implementation.
// ---------------------------------------------------------------------------

#include "cxl_ras.h"
#include "ddr5_backend.h"
#include <algorithm>
#include <cmath>

namespace cxl {

// ============================================================================
// ScrubEngine
// ============================================================================

double ScrubEngine::getContentionNs(uint64_t addr, double nowNs, DDR5Backend* dram) {
    if (!cfg_.enable || cfg_.patrolScrubIntervalUs <= 0) return 0.0;

    double intervalNs = cfg_.patrolScrubIntervalUs * 1000.0;

    // Check if it's time to schedule the next scrub read
    if (nowNs >= lastScrubNs_ + intervalNs || lastScrubNs_ == 0.0) {
        scrubActiveAddr_ = scrubAddr_;   // record the address being scrubbed
        if (dram) {
            // Inject a scrub read at the current scrub position
            scrubDoneNs_ = dram->scheduleScrubRead(scrubActiveAddr_, nowNs);
        } else {
            // Without DDR5Backend: estimate scrub completion analytically
            scrubDoneNs_ = nowNs + 50.0;  // ~50 ns row-miss latency estimate
        }
        lastScrubNs_     = nowNs;
        totalScrubBytes_ += 64;
        // Advance scrub position for the NEXT scrub (after recording active address)
        scrubAddr_ += 64;
    }

    // Check if the scrub is accessing the same cacheline (or very nearby) as `addr`
    // Same bank contention: if the scrub address maps to the same bank as the request,
    // the request must wait for the scrub read to finish.
    if (dram && scrubDoneNs_ > nowNs) {
        // Simple heuristic: same cacheline or within same 4KB page = same bank contention
        uint64_t scrubPage = scrubActiveAddr_ >> 12;  // use in-flight scrub address
        uint64_t reqPage   = addr             >> 12;
        if (scrubPage == reqPage) {
            return std::max(0.0, scrubDoneNs_ - nowNs);
        }
        // If not same page, still add a calibrated bank-conflict penalty
        return cfg_.scrubBankConflictNs;
    }

    return 0.0;
}

// ============================================================================
// PPREngine
// ============================================================================

double PPREngine::triggerSPPR(uint64_t addr, double nowNs, DDR5Backend* dram) {
    if (!cfg_.spprEnable || spprCount_ >= cfg_.pprMaxRepairs) return 0.0;
    if (pprBusyUntilNs_ > nowNs) return 0.0;  // another PPR in progress

    double durationNs = cfg_.spprLatencyUs * 1000.0;
    pprBusyUntilNs_ = nowNs + durationNs;
    spprCount_++;

    // Approximate bank ID: cacheline index modulo typical DDR5 bank count (32 = 1ch×2rank×16banks).
    // This is a timing approximation; spec-level address decode requires the DRAM backend.
    int bankId = static_cast<int>((addr >> 6) % 32);

    if (dram) {
        dram->setPPRBankBlock(bankId, pprBusyUntilNs_);
    }

    bankBlockUntil_[bankId] = pprBusyUntilNs_;

    // Approximate row address
    uint64_t rowAddr = addr >> 6;
    log_.push_back({nowNs, rowAddr, durationNs, false, 0, bankId});

    return durationNs;
}

double PPREngine::triggerHPPR(int rankId, double nowNs, DDR5Backend* dram) {
    if (!cfg_.hpprEnable) return 0.0;

    double durationNs = cfg_.hpprDurationMs * 1e6;  // ms → ns
    pprBusyUntilNs_ = nowNs + durationNs;
    hpprCount_++;

    if (dram) {
        dram->setHPPRRankBlock(rankId, pprBusyUntilNs_);
    }

    rankBlockUntil_[rankId] = pprBusyUntilNs_;
    log_.push_back({nowNs, 0ULL, durationNs, true, rankId, -1});

    return durationNs;
}

bool PPREngine::isBankBlocked(int globalBankId, double nowNs) const {
    auto it = bankBlockUntil_.find(globalBankId);
    return it != bankBlockUntil_.end() && it->second > nowNs;
}

bool PPREngine::isRankBlocked(int rankId, double nowNs) const {
    auto it = rankBlockUntil_.find(rankId);
    return it != rankBlockUntil_.end() && it->second > nowNs;
}

// ============================================================================
// ErrorEscalator
// ============================================================================

bool ErrorEscalator::recordCE(uint64_t addr, double timeNs) {
    ceCount_++;
    errorLog_.push_back({timeNs, addr, true});

    // Sliding window: add timestamp, trim stale entries
    ceWindow_.push_back(timeNs);
    double cutoff = timeNs - cfg_.cvmeWindowUs * 1000.0;
    while (!ceWindow_.empty() && ceWindow_.front() < cutoff) {
        ceWindow_.pop_front();
    }

    // Per-cacheline CE count
    uint64_t cl = (addr >> 6) << 6;
    clCECount_[cl]++;

    // DBIT tracking
    if (cfg_.dbitEnable && clCECount_[cl] >= cfg_.dbitRowCEThreshold) {
        dbitRows_.insert(cl >> 16);  // group by ~64KB "row" granularity
    }

    // Check CVME threshold
    return static_cast<uint32_t>(ceWindow_.size()) >= cfg_.cvmeThreshold;
}

bool ErrorEscalator::recordUCE(uint64_t addr, double timeNs) {
    uceCount_++;
    errorLog_.push_back({timeNs, addr, false});
    return true;  // UCE always triggers escalation
}

bool ErrorEscalator::isDbitRow(uint64_t addr) const {
    return dbitRows_.count(addr >> 16) > 0;
}

// ============================================================================
// ViralProtocol
// ============================================================================

void ViralProtocol::onUCE(double timeNs) {
    if (!cfg_.viralEnable) return;
    if (state_ == ViralState::NORMAL) {
        state_         = ViralState::VIRAL_PENDING;
        pendingSince_  = timeNs;
        log_.push_back({timeNs, "device0", "viral_pending"});
    }
}

double ViralProtocol::process(double nowNs) {
    if (!cfg_.viralEnable) return 0.0;

    switch (state_) {
    case ViralState::NORMAL:
        return 0.0;

    case ViralState::VIRAL_PENDING:
        // Transition to VIRAL_ACTIVE on next process() call after UCE
        state_ = ViralState::VIRAL_ACTIVE;
        log_.push_back({nowNs, "device0", "viral_active"});
        return cfg_.viralPropagationDelayNs;

    case ViralState::VIRAL_ACTIVE:
        return cfg_.viralStallNs;

    case ViralState::CONTAINED:
        return 0.0;
    }
    return 0.0;
}

// ============================================================================
// PoisonManager
// ============================================================================

void PoisonManager::inject(uint64_t addr, double timeNs) {
    if (!cfg_.dataPoisonEnable) return;
    uint64_t cl = (addr >> 6) << 6;
    poisonedAddrs_.insert(cl);
    log_.push_back({timeNs, addr, "inject"});
}

double PoisonManager::onRead(uint64_t addr, double timeNs, bool& berrOut) {
    berrOut = false;
    if (!cfg_.dataPoisonEnable) return 0.0;

    uint64_t cl = (addr >> 6) << 6;
    if (!poisonedAddrs_.count(cl)) return 0.0;

    berrOut = true;
    // Only log the first read of each poisoned cacheline to avoid per-request spam.
    if (loggedPoisonReads_.insert(cl).second) {
        log_.push_back({timeNs, addr, "read"});
    }
    // ECC decode + error reporting overhead
    return 5.0;  // ns
}

double PoisonManager::onWrite(uint64_t addr, double timeNs) {
    if (!cfg_.dataPoisonEnable) return 0.0;

    uint64_t cl = (addr >> 6) << 6;
    if (poisonedAddrs_.count(cl)) {
        // Writing to a poisoned line propagates poison; log it
        log_.push_back({timeNs, addr, "write_propagate"});
        return 2.0;  // ns
    }
    return 0.0;
}

bool PoisonManager::isPoisoned(uint64_t addr) const {
    if (!cfg_.dataPoisonEnable) return false;
    return poisonedAddrs_.count((addr >> 6) << 6) > 0;
}

void PoisonManager::clear(uint64_t addr, double timeNs) {
    if (!cfg_.dataPoisonEnable) return;
    uint64_t cl = (addr >> 6) << 6;
    if (poisonedAddrs_.erase(cl)) {
        loggedPoisonReads_.erase(cl);  // reset so a future re-inject logs again
        log_.push_back({timeNs, addr, "clear"});
    }
}

// ============================================================================
// CorrelatedFaultEngine
// ============================================================================

CorrelatedFaultEngine::CorrelatedFaultEngine(const RASConfig& cfg, uint32_t seed)
    : cfg_(cfg), rng_(seed)
{}

double CorrelatedFaultEngine::effectiveRate(uint64_t addr, bool isWrite,
                                            uint64_t activations) const {
    double base = isWrite ? cfg_.uceRate : cfg_.ceRate;
    if (base <= 0.0) return 0.0;

    // Spatial correlation: if this row already has CEs, multiply rate
    uint64_t row = rowAddr(addr);
    auto it = rowCECount_.find(row);
    uint32_t rowCEs = (it != rowCECount_.end()) ? it->second : 0u;
    double spatial = (rowCEs > 0)
        ? cfg_.correlatedFaults.rowRepeatFactor
        : 1.0;

    // Temporal aging
    double aging = 1.0;
    if (activations > 0 && cfg_.correlatedFaults.agingThreshold > 0) {
        aging = 1.0 + cfg_.correlatedFaults.agingFactor
                * std::log2(1.0 + static_cast<double>(activations)
                                 / cfg_.correlatedFaults.agingThreshold);
    }

    return base * spatial * aging;
}

bool CorrelatedFaultEngine::inBurstWindow(double nowNs) const {
    return lastErrorNs_ >= 0.0
        && (nowNs - lastErrorNs_) <= cfg_.correlatedFaults.burstWindowNs;
}

uint64_t CorrelatedFaultEngine::adjacentRow(uint64_t rAddr, int offset) const {
    if (offset < 0 && static_cast<uint64_t>(-offset) > rAddr) return 0;
    return rAddr + static_cast<uint64_t>(offset);
}

void CorrelatedFaultEngine::recordActivation(uint64_t rAddr, double nowNs) {
    RowActivity& ra = rowActivity_[rAddr];
    ra.activations++;

    // Refresh-interval window management
    if (nowNs - ra.windowStartNs > cfg_.correlatedFaults.refreshIntervalNs) {
        ra.windowActivations = 1;
        ra.windowStartNs     = nowNs;
    } else {
        ra.windowActivations++;
    }
}

bool CorrelatedFaultEngine::rowHammerTriggered(uint64_t rAddr, double nowNs) const {
    auto it = rowActivity_.find(rAddr);
    if (it == rowActivity_.end()) return false;
    const RowActivity& ra = it->second;
    // Check if still within the current refresh window
    if ((nowNs - ra.windowStartNs) > cfg_.correlatedFaults.refreshIntervalNs) {
        return false;
    }
    return ra.windowActivations >= cfg_.correlatedFaults.rowHammerThresh;
}

CorrelatedFaultEngine::FaultResult
CorrelatedFaultEngine::evaluate(uint64_t addr, bool isWrite, double nowNs,
                                uint64_t activationCount) {
    FaultResult r;

    if (cfg_.correlatedFaults.model == FaultModel::INDEPENDENT) {
        // Original Bernoulli: direct rate comparison
        std::uniform_real_distribution<double> u(0.0, 1.0);
        if (cfg_.ceRate  > 0 && u(rng_) < cfg_.ceRate)  { r.ce  = true; totalCEs_++;  }
        if (cfg_.uceRate > 0 && u(rng_) < cfg_.uceRate) { r.uce = true; totalUCEs_++; }
        return r;
    }

    // ── CORRELATED mode ───────────────────────────────────────────────────

    double rate = effectiveRate(addr, isWrite, activationCount);

    if (rate <= 0.0) {
        // Check burst window
        if (inBurstWindow(nowNs)) {
            std::bernoulli_distribution bd(cfg_.correlatedFaults.burstProbability);
            if (bd(rng_)) {
                rate = std::max(cfg_.ceRate, 1e-6);
            }
        }
    }

    if (rate > 0.0) {
        std::bernoulli_distribution bd(std::min(rate, 1.0));
        if (bd(rng_)) {
            // Determine CE vs UCE: UCE is rarer by 100×
            double uceProb = (cfg_.uceRate > 0)
                ? cfg_.uceRate / (cfg_.ceRate + 1e-15) : 0.001;
            std::bernoulli_distribution uced(uceProb);
            if (uced(rng_)) {
                r.uce = true;
                totalUCEs_++;
            } else {
                r.ce = true;
                totalCEs_++;
                rowCECount_[rowAddr(addr)]++;
            }
            lastErrorNs_ = nowNs;
            r.faultAddr  = addr;
        }
    }

    // Row hammer check on adjacent rows
    if (cfg_.correlatedFaults.rowHammerEnable) {
        for (int offset : {-1, +1}) {
            uint64_t adjRow = adjacentRow(rowAddr(addr), offset);
            if (rowHammerTriggered(adjRow, nowNs)) {
                std::bernoulli_distribution rh(cfg_.correlatedFaults.rowHammerErrProb);
                if (rh(rng_)) {
                    r.ce = true;
                    totalCEs_++;
                    rowCECount_[adjRow]++;
                    r.faultAddr  = adjRow * (1ULL << 13);
                    lastErrorNs_ = nowNs;
                }
            }
        }
    }

    return r;
}

// ============================================================================
// RASEngine
// ============================================================================

RASEngine::RASEngine(const RASConfig& cfg, uint32_t seed)
    : cfg_(cfg),
      rng_(seed),
      scrub_(cfg),
      ppr_(cfg),
      escalator_(cfg),
      viral_(cfg),
      poison_(cfg)
{
    if (cfg_.correlatedFaults.model == FaultModel::CORRELATED) {
        corrFaults_ = std::make_unique<CorrelatedFaultEngine>(cfg_, seed ^ 0xF00Du);
    }
}

// ---------------------------------------------------------------------------
// process — RAS subset pipeline per request
// ---------------------------------------------------------------------------

RASEngine::Result RASEngine::process(uint64_t addr, bool isWrite,
                                     double dispatchNs, uint32_t sizeBytes,
                                     DDR5Backend* dram) {
    Result result;
    if (!cfg_.enable) return result;

    // ── 1. Error injection ────────────────────────────────────────────────
    if (cfg_.injectEnable) {
        double trials = static_cast<double>(sizeBytes) / 8.0;
        std::uniform_real_distribution<double> u(0.0, 1.0);

        if (corrFaults_) {
            // CORRELATED mode: delegate to CorrelatedFaultEngine.
            // Pass 0 for activationCount; the engine tracks internally.
            auto fr = corrFaults_->evaluate(addr, isWrite, dispatchNs, 0);
            if (fr.ce) {
                injectCE(fr.faultAddr ? fr.faultAddr : addr, dispatchNs);
            }
            if (fr.uce) {
                injectUCE(fr.faultAddr ? fr.faultAddr : addr, dispatchNs);
            }
            // Record row activation for row-hammer tracking (8KB row granularity)
            corrFaults_->recordActivation(addr >> 13, dispatchNs);
        } else {
            // INDEPENDENT mode (original Bernoulli)
            if (u(rng_) < cfg_.ceRate * trials) {
                injectCE(addr, dispatchNs);
            }
            if (u(rng_) < cfg_.uceRate * trials) {
                injectUCE(addr, dispatchNs);
            }
        }
    }

    // ── 2. Poison check ───────────────────────────────────────────────────
    if (cfg_.dataPoisonEnable) {
        if (isWrite) {
            result.extraNs += poison_.onWrite(addr, dispatchNs);
        } else {
            bool berr = false;
            result.extraNs += poison_.onRead(addr, dispatchNs, berr);
            if (berr) {
                result.berr       = true;
                result.poisonRead = true;
            }
        }
    }

    // ── 3. PPR stall ─────────────────────────────────────────────────────
    if (cfg_.pprEnable) {
        double busy = ppr_.pprBusyUntilNs();
        if (dispatchNs < busy) {
            result.extraNs += (busy - dispatchNs);
        }
    }

    // ── 4. Patrol scrub contention ───────────────────────────────────────
    result.extraNs += scrub_.getContentionNs(addr, dispatchNs, dram);

    // ── 5. Viral containment ─────────────────────────────────────────────
    if (cfg_.viralEnable) {
        double viralExtra = viral_.process(dispatchNs);
        result.extraNs += viralExtra;
        if (viral_.viralBitSet()) {
            result.viral = true;
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// injectCE / injectUCE
// ---------------------------------------------------------------------------

void RASEngine::injectCE(uint64_t addr, double timeNs) {
    bool cvme = escalator_.recordCE(addr, timeNs);
    if (cvme) {
        triggerCVME(addr, timeNs, nullptr);
    }
}

void RASEngine::injectUCE(uint64_t addr, double timeNs) {
    escalator_.recordUCE(addr, timeNs);
    if (cfg_.dataPoisonEnable) {
        poison_.inject(addr, timeNs);
    }
    if (cfg_.viralEnable) {
        viral_.onUCE(timeNs);
    }
}

void RASEngine::triggerCVME(uint64_t addr, double timeNs, DDR5Backend* dram) {
    triggerPPR(addr, timeNs, dram);
}

void RASEngine::triggerPPR(uint64_t addr, double timeNs, DDR5Backend* dram) {
    if (!cfg_.pprEnable) return;

    if (cfg_.spprEnable && !ppr_.spprCapacityExhausted()) {
        ppr_.triggerSPPR(addr, timeNs, dram);
        // sPPR repaired the row — clear poison on the affected cacheline.
        poison_.clear(addr, timeNs);
    } else if (cfg_.hpprEnable) {
        // sPPR capacity exhausted → escalate to hPPR on rank 0.
        ppr_.triggerHPPR(0, timeNs, dram);
        // hPPR repairs an entire rank; clear poison at the target address
        // (full-rank poison tracking would require a rank→address map; this
        // conservatively clears at the known failing address).
        poison_.clear(addr, timeNs);
    }
}

void RASEngine::injectPPR(uint64_t addr, double timeNs, bool hard, DDR5Backend* dram) {
    if (!cfg_.pprEnable) return;
    if (hard) {
        if (cfg_.hpprEnable)
            ppr_.triggerHPPR(0, timeNs, dram);
    } else {
        if (cfg_.spprEnable && !ppr_.spprCapacityExhausted())
            ppr_.triggerSPPR(addr, timeNs, dram);
        else if (cfg_.hpprEnable)
            ppr_.triggerHPPR(0, timeNs, dram);
    }
}

void RASEngine::triggerViral(double timeNs) {
    viral_.onUCE(timeNs);
}

// ---------------------------------------------------------------------------
// recordActivation
// ---------------------------------------------------------------------------

void RASEngine::recordActivation(uint64_t addr, double nowNs) {
    if (corrFaults_) {
        corrFaults_->recordActivation(addr >> 13, nowNs);  // 8KB row granularity
    }
}

// ---------------------------------------------------------------------------
// Query methods
// ---------------------------------------------------------------------------

bool RASEngine::isPoisoned(uint64_t addr) const {
    return poison_.isPoisoned(addr);
}

bool RASEngine::isBankBlocked(int globalBankId, double nowNs) const {
    return ppr_.isBankBlocked(globalBankId, nowNs);
}

bool RASEngine::isRankBlocked(int rankId, double nowNs) const {
    return ppr_.isRankBlocked(rankId, nowNs);
}

double RASEngine::scrubCoverage() const {
    return scrub_.scrubCoverage(totalDramBytes_);
}

}  // namespace cxl
