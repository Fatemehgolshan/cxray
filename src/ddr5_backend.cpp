// ---------------------------------------------------------------------------
// ddr5_backend.cpp — JEDEC-accurate DDR5/DDR4 DRAM backend implementation.
//
// Scheduling policy: arrival-order (requests served in arrival order per bank).
// The backend is event-driven (not cycle-stepped); each addRequest() call
// computes the exact completion time by enforcing all relevant JEDEC timing
// constraints in order:
//
//   1. Refresh check (auto ABR when tREFI expires)
//   2. Rank block from hPPR (if set)
//   3. tRRD_S/L  — ACT-to-ACT spacing (same / different bank group)
//   4. tFAW     — four-activation window
//   5. tCCD_S/L  — CAS-to-CAS spacing
//   6. tWTR_S/L  — write-to-read turnaround
//   7. tRAS      — minimum active time before precharge
//   8. tRP       — precharge time
//   9. tRCD      — activate-to-CAS delay
//  10. tCL + tBL — CAS latency + burst length
// ---------------------------------------------------------------------------

#include "ddr5_backend.h"
#include <algorithm>
#include <cmath>
#include <cassert>
#include <stdexcept>

namespace cxl {

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

DDR5Backend::DDR5Backend(const DRAMTimingParams& timing,
                         int numChannels,
                         int numRanks,
                         int writeBufferCapacity)
    : timing_(timing),
      numChannels_(numChannels),
      numRanks_(numRanks),
      numBanksPerRank_(timing.numBankGroups * timing.numBanksPerGroup),
      writeBufferCap_(writeBufferCapacity)
{
    if (numChannels_ < 1) numChannels_ = 1;
    if (numRanks_    < 1) numRanks_    = 1;

    int totalBanks      = numChannels_ * numRanks_ * numBanksPerRank_;
    int totalBankGroups = numChannels_ * numRanks_ * timing_.numBankGroups;
    int totalRanks      = numChannels_ * numRanks_;

    banks_.resize(totalBanks);
    bankGroups_.resize(totalBankGroups);
    ranks_.resize(totalRanks);
}

// ---------------------------------------------------------------------------
// Address decode: RoBaRaCoCh mapping with bank-group awareness.
//
//  Bit layout (from LSB):
//    [5:0]   — column offset within cacheline (64B = 6 bits, always 0 at CL gran)
//    [5+ch_bits-1 : 6]           — channel select
//    [5+ch_bits+col_bits-1 : …]  — remaining column bits (simplified: 0 here)
//    bg bits, bank bits, rank bits, row bits
//
// For simplicity we use a flat interleaving that spreads accesses across
// channels, then rank, then bank group, then bank, then row.
// ---------------------------------------------------------------------------

DDR5Backend::AddrFields DDR5Backend::decodeAddr(uint64_t addr) const {
    // Work at cacheline granularity
    uint64_t cl = addr >> 6;

    // Bit widths
    int chBits = 0; { int n = numChannels_; while (n > 1) { chBits++;  n >>= 1; } }
    int rkBits = 0; { int n = numRanks_;    while (n > 1) { rkBits++;  n >>= 1; } }
    int bgBits = 0; { int n = timing_.numBankGroups;    while (n > 1) { bgBits++; n >>= 1; } }
    int bkBits = 0; { int n = timing_.numBanksPerGroup; while (n > 1) { bkBits++; n >>= 1; } }

    AddrFields af;
    uint64_t tmp = cl;

    // Channel interleave at cacheline granularity (low bits → spread streams)
    af.channelId = static_cast<int>(tmp & ((1 << chBits) - 1));
    tmp >>= chBits;

    // Bank group
    af.bgId = static_cast<int>(tmp & ((1 << bgBits) - 1));
    tmp >>= bgBits;

    // Bank within group
    af.bankId = static_cast<int>(tmp & ((1 << bkBits) - 1));
    tmp >>= bkBits;

    // Rank
    af.rankId = static_cast<int>(tmp & ((1 << rkBits) - 1));
    tmp >>= rkBits;

    // Column (low bits within row — minimal, not needed for timing)
    af.colId = static_cast<int>(tmp & 0xF);
    tmp >>= 4;

    // Row (remaining upper bits)
    af.rowId = static_cast<int64_t>(tmp);

    // Guard against non-power-of-two counts (fallback to modulo)
    if (numChannels_ & (numChannels_ - 1))
        af.channelId = static_cast<int>((addr >> 6) % numChannels_);
    if (numRanks_ & (numRanks_ - 1))
        af.rankId = static_cast<int>(((addr >> 6) / numChannels_) % numRanks_);

    return af;
}

// ---------------------------------------------------------------------------
// checkRefresh — auto all-bank refresh when tREFI expires.
// Closes all banks in the rank and stalls until tRFC completes.
// ---------------------------------------------------------------------------

void DDR5Backend::checkRefresh(int rIdx, double nowNs) {
    RankState& rank = ranks_[rIdx];
    if (nowNs < rank.lastRefreshNs + timing_.tREFI_ns) return;  // not yet due

    // Determine channel and rank from flat index
    int ch   = rIdx / numRanks_;
    int rnk  = rIdx % numRanks_;

    // Compute refresh start: max(nowNs, all busy-until across this rank)
    double refreshStart = nowNs;
    for (int bg = 0; bg < timing_.numBankGroups; bg++) {
        for (int bk = 0; bk < timing_.numBanksPerGroup; bk++) {
            int bi = bankIdx(ch, rnk, bg, bk);
            refreshStart = std::max(refreshStart, banks_[bi].busyUntilNs);
        }
    }

    double refreshEnd = refreshStart + timing_.tRFC_ns;

    // Close all banks in this rank
    for (int bg = 0; bg < timing_.numBankGroups; bg++) {
        for (int bk = 0; bk < timing_.numBanksPerGroup; bk++) {
            int bi = bankIdx(ch, rnk, bg, bk);
            banks_[bi].openRow     = -1;
            banks_[bi].activatedNs = -1e18;
            banks_[bi].busyUntilNs = refreshEnd;
            banks_[bi].refreshes++;
        }
    }

    rank.refreshDoneNs  = refreshEnd;
    // Advance lastRefreshNs by exactly one interval (don't skip multiples)
    rank.lastRefreshNs += timing_.tREFI_ns;
}

// ---------------------------------------------------------------------------
// Timing constraint helpers
// ---------------------------------------------------------------------------

double DDR5Backend::enforceTRRD(int rIdx, int bgId, bool sameBG, double t) {
    // Same BG: tRRD_S, different BG: tRRD_L
    double constraint = sameBG ? timing_.tRRD_S_ns : timing_.tRRD_L_ns;
    int ch  = rIdx / numRanks_;
    int rnk = rIdx % numRanks_;
    int gi  = bgIdx(ch, rnk, bgId);
    return std::max(t, bankGroups_[gi].lastActNs + constraint);
}

double DDR5Backend::enforceTFAW(RankState& rank, double t) {
    // Trim activations older than tFAW window
    while (!rank.actTimes.empty() &&
           rank.actTimes.front() + timing_.tFAW_ns <= t) {
        rank.actTimes.pop_front();
    }
    if (static_cast<int>(rank.actTimes.size()) >= 4) {
        // Must wait until the oldest of the 4 falls out of window
        t = std::max(t, rank.actTimes.front() + timing_.tFAW_ns);
        // Trim again after advancing time
        while (!rank.actTimes.empty() &&
               rank.actTimes.front() + timing_.tFAW_ns <= t) {
            rank.actTimes.pop_front();
        }
    }
    return t;
}

double DDR5Backend::enforceTCCD(int rIdx, int bgId, bool sameBG, bool /*isWrite*/, double t) {
    double constraint = sameBG ? timing_.tCCD_S_ns : timing_.tCCD_L_ns;
    int ch  = rIdx / numRanks_;
    int rnk = rIdx % numRanks_;
    int gi  = bgIdx(ch, rnk, bgId);
    return std::max(t, bankGroups_[gi].lastCasNs + constraint);
}

double DDR5Backend::enforceTWTR(int rIdx, int bgId, bool sameBG, double t) {
    int ch  = rIdx / numRanks_;
    int rnk = rIdx % numRanks_;

    // Same BG: tWTR_S from last write in this BG
    // Diff BG: tWTR_L from last write in any BG (rank-wide)
    if (sameBG) {
        int gi = bgIdx(ch, rnk, bgId);
        t = std::max(t, bankGroups_[gi].lastWrNs + timing_.tWTR_S_ns);
    } else {
        t = std::max(t, ranks_[rIdx].lastWrAnyNs + timing_.tWTR_L_ns);
    }
    return t;
}

void DDR5Backend::doActivate(AddrFields& af, double actNs) {
    int rIdx = rankIdx(af.channelId, af.rankId);
    int gi   = bgIdx(af.channelId, af.rankId, af.bgId);
    int bi   = bankIdx(af.channelId, af.rankId, af.bgId, af.bankId);

    banks_[bi].activatedNs       = actNs;
    bankGroups_[gi].lastActNs    = actNs;
    ranks_[rIdx].actTimes.push_back(actNs);
}

void DDR5Backend::doPrecharge(AddrFields& af, double preNs) {
    int bi = bankIdx(af.channelId, af.rankId, af.bgId, af.bankId);
    banks_[bi].openRow     = -1;
    banks_[bi].activatedNs = -1e18;
    banks_[bi].busyUntilNs = preNs + timing_.tRP_ns;
}

// ---------------------------------------------------------------------------
// scheduleRead — compute read completion time, enforcing all JEDEC constraints.
// ---------------------------------------------------------------------------

double DDR5Backend::scheduleRead(AddrFields& af, double arrivalNs) {
    int rIdx = rankIdx(af.channelId, af.rankId);
    int bi   = bankIdx(af.channelId, af.rankId, af.bgId, af.bankId);

    // 1. Auto-refresh check
    checkRefresh(rIdx, arrivalNs);

    // 2. Start time: max(arrival, rank refresh done, any hPPR block)
    double t = std::max(arrivalNs, ranks_[rIdx].refreshDoneNs);
    auto hpprIt = hpprRankBlockUntilNs_.find(rIdx);
    if (hpprIt != hpprRankBlockUntilNs_.end())
        t = std::max(t, hpprIt->second);

    // 3. Wait for bank to be free
    t = std::max(t, banks_[bi].busyUntilNs);

    BankState& bank = banks_[bi];
    double     casNs, completeNs;

    if (bank.openRow == af.rowId) {
        // ── Row hit ─────────────────────────────────────────────────────
        // Enforce tCCD: apply only the relevant constraint based on whether
        // the previous CAS was to the same bank group (tCCD_S) or a different
        // one (tCCD_L).  tCCD_S > tCCD_L — same BG has the tighter constraint.
        {
            int gi = bgIdx(af.channelId, af.rankId, af.bgId);
            bool prevSameBG = (ranks_[rIdx].lastCasBgId >= 0) &&
                              (ranks_[rIdx].lastCasBgId == af.bgId);
            if (prevSameBG) {
                t = std::max(t, bankGroups_[gi].lastCasNs + timing_.tCCD_S_ns);
            } else if (ranks_[rIdx].lastCasBgId >= 0) {
                // Cross-BG: enforce tCCD_L from the any-BG last CAS
                t = std::max(t, ranks_[rIdx].lastCasAnyNs + timing_.tCCD_L_ns);
                // Plus any same-BG constraint from earlier within this BG
                if (bankGroups_[gi].lastCasNs > -1e17)
                    t = std::max(t, bankGroups_[gi].lastCasNs + timing_.tCCD_S_ns);
            }
        }

        // Enforce tWTR (if last operation was a write, must wait tWTR_S or _L)
        if (bank.lastWasWrite) {
            t = enforceTWTR(rIdx, af.bgId, true,  t);
            t = enforceTWTR(rIdx, af.bgId, false, t);
        }

        casNs      = t;
        completeNs = casNs + timing_.tCL_ns + timing_.tBL_ns;

        // Update state
        bank.busyUntilNs  = casNs + timing_.tCL_ns;
        bank.lastWasWrite = false;
        bank.rowHits++;
        totalRowHits_++;

        // Update BG and rank last-CAS time
        int gi = bgIdx(af.channelId, af.rankId, af.bgId);
        bankGroups_[gi].lastCasNs  = casNs;
        ranks_[rIdx].lastCasAnyNs  = casNs;
        ranks_[rIdx].lastCasBgId   = af.bgId;

    } else {
        // ── Row conflict or row miss ─────────────────────────────────────
        if (bank.openRow >= 0) {
            // Row conflict: must precharge first
            // tRAS: cannot precharge until bank has been active for tRAS
            double preNs = std::max(t, bank.activatedNs + timing_.tRAS_ns);
            doPrecharge(af, preNs);
            bank.rowConflicts++;
            totalRowConflicts_++;
        } else {
            bank.rowMisses++;
            totalRowMisses_++;
        }

        // After precharge (or idle), wait for bank ready
        t = std::max(t, bank.busyUntilNs);

        // Enforce tRRD (ACT-to-ACT spacing)
        // Check both same-BG and cross-BG to be safe — we only need the relevant one,
        // but taking max of both is conservative and correct.
        t = enforceTRRD(rIdx, af.bgId, true,  t);  // tRRD_S (same BG)
        t = enforceTRRD(rIdx, af.bgId, false, t);  // tRRD_L (diff BG) — always apply

        // Enforce tFAW
        t = enforceTFAW(ranks_[rIdx], t);

        // Issue ACT
        double actNs = t;
        doActivate(af, actNs);
        bank.openRow      = af.rowId;
        bank.busyUntilNs  = actNs + timing_.tRCD_ns;
        t = bank.busyUntilNs;

        // Now CAS: apply tCCD based on same/cross BG vs. last CAS
        {
            int gi = bgIdx(af.channelId, af.rankId, af.bgId);
            bool prevSameBG = (ranks_[rIdx].lastCasBgId >= 0) &&
                              (ranks_[rIdx].lastCasBgId == af.bgId);
            if (prevSameBG) {
                t = std::max(t, bankGroups_[gi].lastCasNs + timing_.tCCD_S_ns);
            } else if (ranks_[rIdx].lastCasBgId >= 0) {
                t = std::max(t, ranks_[rIdx].lastCasAnyNs + timing_.tCCD_L_ns);
                if (bankGroups_[gi].lastCasNs > -1e17)
                    t = std::max(t, bankGroups_[gi].lastCasNs + timing_.tCCD_S_ns);
            }
        }

        // Enforce tWTR if last was a write (on any bank)
        t = enforceTWTR(rIdx, af.bgId, false, t);   // tWTR_L (cross-BG)

        casNs      = t;
        completeNs = casNs + timing_.tCL_ns + timing_.tBL_ns;

        // Update state
        bank.busyUntilNs  = casNs + timing_.tCL_ns;
        bank.lastWasWrite = false;

        int gi = bgIdx(af.channelId, af.rankId, af.bgId);
        bankGroups_[gi].lastCasNs  = casNs;
        ranks_[rIdx].lastCasAnyNs  = casNs;
        ranks_[rIdx].lastCasBgId   = af.bgId;
    }

    return completeNs;
}

// ---------------------------------------------------------------------------
// scheduleWrite — compute write completion time.
// Writes use the same logic as reads but set lastWasWrite and update tWTR.
// ---------------------------------------------------------------------------

double DDR5Backend::scheduleWrite(AddrFields& af, double arrivalNs) {
    int rIdx = rankIdx(af.channelId, af.rankId);
    int bi   = bankIdx(af.channelId, af.rankId, af.bgId, af.bankId);

    // 1. Refresh check
    checkRefresh(rIdx, arrivalNs);

    // 2. Start time
    double t = std::max(arrivalNs, ranks_[rIdx].refreshDoneNs);
    auto hpprIt = hpprRankBlockUntilNs_.find(rIdx);
    if (hpprIt != hpprRankBlockUntilNs_.end())
        t = std::max(t, hpprIt->second);

    // 3. Bank free
    t = std::max(t, banks_[bi].busyUntilNs);

    BankState& bank = banks_[bi];
    double     casNs, completeNs;

    if (bank.openRow == af.rowId) {
        // Row hit — enforce tCCD with same/cross-BG selection
        {
            int gi = bgIdx(af.channelId, af.rankId, af.bgId);
            bool prevSameBG = (ranks_[rIdx].lastCasBgId >= 0) &&
                              (ranks_[rIdx].lastCasBgId == af.bgId);
            if (prevSameBG) {
                t = std::max(t, bankGroups_[gi].lastCasNs + timing_.tCCD_S_ns);
            } else if (ranks_[rIdx].lastCasBgId >= 0) {
                t = std::max(t, ranks_[rIdx].lastCasAnyNs + timing_.tCCD_L_ns);
                if (bankGroups_[gi].lastCasNs > -1e17)
                    t = std::max(t, bankGroups_[gi].lastCasNs + timing_.tCCD_S_ns);
            }
        }

        casNs      = t;
        // Write latency: CAS-W + write latency + burst
        completeNs = casNs + timing_.tCL_ns + timing_.tBL_ns;

        bank.busyUntilNs  = casNs + timing_.tCL_ns + timing_.tWR_ns;
        bank.lastWasWrite = true;
        bank.rowHits++;
        totalRowHits_++;

        int gi = bgIdx(af.channelId, af.rankId, af.bgId);
        bankGroups_[gi].lastCasNs   = casNs;
        bankGroups_[gi].lastWrNs    = casNs;
        ranks_[rIdx].lastCasAnyNs   = casNs;
        ranks_[rIdx].lastWrAnyNs    = casNs;
        ranks_[rIdx].lastCasBgId    = af.bgId;

    } else {
        if (bank.openRow >= 0) {
            // Row conflict
            double preNs = std::max(t, bank.activatedNs + timing_.tRAS_ns);
            doPrecharge(af, preNs);
            bank.rowConflicts++;
            totalRowConflicts_++;
        } else {
            bank.rowMisses++;
            totalRowMisses_++;
        }

        t = std::max(t, bank.busyUntilNs);
        t = enforceTRRD(rIdx, af.bgId, true,  t);
        t = enforceTRRD(rIdx, af.bgId, false, t);
        t = enforceTFAW(ranks_[rIdx], t);

        double actNs = t;
        doActivate(af, actNs);
        bank.openRow      = af.rowId;
        bank.busyUntilNs  = actNs + timing_.tRCD_ns;
        t = bank.busyUntilNs;

        {
            int gi = bgIdx(af.channelId, af.rankId, af.bgId);
            bool prevSameBG = (ranks_[rIdx].lastCasBgId >= 0) &&
                              (ranks_[rIdx].lastCasBgId == af.bgId);
            if (prevSameBG) {
                t = std::max(t, bankGroups_[gi].lastCasNs + timing_.tCCD_S_ns);
            } else if (ranks_[rIdx].lastCasBgId >= 0) {
                t = std::max(t, ranks_[rIdx].lastCasAnyNs + timing_.tCCD_L_ns);
                if (bankGroups_[gi].lastCasNs > -1e17)
                    t = std::max(t, bankGroups_[gi].lastCasNs + timing_.tCCD_S_ns);
            }
        }

        casNs      = t;
        completeNs = casNs + timing_.tCL_ns + timing_.tBL_ns;

        bank.busyUntilNs  = casNs + timing_.tCL_ns + timing_.tWR_ns;
        bank.lastWasWrite = true;

        int gi = bgIdx(af.channelId, af.rankId, af.bgId);
        bankGroups_[gi].lastCasNs  = casNs;
        bankGroups_[gi].lastWrNs   = casNs;
        ranks_[rIdx].lastCasAnyNs  = casNs;
        ranks_[rIdx].lastWrAnyNs   = casNs;
        ranks_[rIdx].lastCasBgId   = af.bgId;
    }

    return completeNs;
}

// ---------------------------------------------------------------------------
// Write buffer helpers
// ---------------------------------------------------------------------------

bool DDR5Backend::checkWriteBuffer(uint64_t addr, double arrivalNs,
                                   double& completionNs) {
    // Check if there's already a pending write to the same cacheline
    uint64_t cl = (addr >> 6) << 6;
    for (auto& pw : writeBuffer_) {
        if (((pw.addr >> 6) << 6) == cl) {
            // Write merge: complete immediately (write already pending)
            completionNs = arrivalNs + timing_.tBL_ns;  // minimal ack
            return true;
        }
    }
    return false;
}

void DDR5Backend::tryDrainWriteBuffer() {
    // Drain all entries in FIFO order (simplified: drain oldest first)
    while (!writeBuffer_.empty()) {
        auto& pw = writeBuffer_.front();
        // Re-decode for correctness
        auto decoded = decodeAddr(pw.addr);
        double done = scheduleWrite(decoded, pw.arrivalNs);
        writeBufferCompletions_[pw.id] = done;
        writeBuffer_.pop_front();
    }
}

// ---------------------------------------------------------------------------
// addRequest — public entry point
// ---------------------------------------------------------------------------

uint64_t DDR5Backend::addRequest(uint64_t addr, bool isWrite, double arrivalNs) {
    totalRequests_++;
    totalBytes_ += 64;
    if (arrivalNs < simStartNs_) simStartNs_ = arrivalNs;

    uint64_t id = nextId_++;
    AddrFields af = decodeAddr(addr);

    double completionNs;
    bool   wasHit = false;

    if (isWrite) {
        // Check write buffer for merge opportunity
        if (checkWriteBuffer(addr, arrivalNs, completionNs)) {
            pending_[id] = {completionNs, false};
            if (completionNs > simEndNs_) simEndNs_ = completionNs;
            return id;
        }

        // If write buffer is full, drain it first
        if (static_cast<int>(writeBuffer_.size()) >= writeBufferCap_) {
            tryDrainWriteBuffer();
        }

        // Buffer the write: schedule it in arrival order
        // Check if row is already open before scheduling (for hit tracking)
        {
            int bi = bankIdx(af.channelId, af.rankId, af.bgId, af.bankId);
            wasHit = (banks_[bi].openRow == af.rowId);
        }
        completionNs = scheduleWrite(af, arrivalNs);
    } else {
        // Reads: drain write buffer if there are pending writes (to avoid
        // reading stale data if a write to the same address is buffered)
        if (!writeBuffer_.empty()) {
            tryDrainWriteBuffer();
        }
        // Check if row is already open before scheduling (for hit tracking)
        {
            int bi = bankIdx(af.channelId, af.rankId, af.bgId, af.bankId);
            wasHit = (banks_[bi].openRow == af.rowId);
        }
        completionNs = scheduleRead(af, arrivalNs);
    }

    pending_[id] = {completionNs, wasHit};
    if (completionNs > simEndNs_) simEndNs_ = completionNs;
    return id;
}

// ---------------------------------------------------------------------------
// getCompletionNs
// ---------------------------------------------------------------------------

double DDR5Backend::getCompletionNs(uint64_t requestId) {
    // Check write buffer completions first
    auto wbIt = writeBufferCompletions_.find(requestId);
    if (wbIt != writeBufferCompletions_.end()) return wbIt->second;

    auto it = pending_.find(requestId);
    if (it == pending_.end()) return -1.0;
    return it->second.completionNs;
}

// ---------------------------------------------------------------------------
// getBandwidthGBps — bytes / simulation window (ns → GB/s directly)
// ---------------------------------------------------------------------------

double DDR5Backend::getBandwidthGBps() const {
    double window = simEndNs_ - simStartNs_;
    if (window <= 0.0 || totalBytes_ == 0) return 0.0;
    return static_cast<double>(totalBytes_) / window;
}

// ---------------------------------------------------------------------------
// getRowHitRate
// ---------------------------------------------------------------------------

double DDR5Backend::getRowHitRate() const {
    uint64_t total = totalRowHits_ + totalRowMisses_ + totalRowConflicts_;
    return total > 0 ? static_cast<double>(totalRowHits_) / total : 0.0;
}

// ---------------------------------------------------------------------------
// RAS hooks
// ---------------------------------------------------------------------------

void DDR5Backend::setPPRBankBlock(int globalBankId, double blockUntilNs) {
    if (globalBankId >= 0 && globalBankId < static_cast<int>(banks_.size())) {
        banks_[globalBankId].busyUntilNs =
            std::max(banks_[globalBankId].busyUntilNs, blockUntilNs);
        banks_[globalBankId].openRow = -1;  // close the row during repair
    }
}

void DDR5Backend::setHPPRRankBlock(int rankId, double blockUntilNs) {
    if (rankId >= 0 && rankId < static_cast<int>(ranks_.size())) {
        hpprRankBlockUntilNs_[rankId] = blockUntilNs;
        // Close all banks in this rank
        int ch  = rankId / numRanks_;
        int rnk = rankId % numRanks_;
        for (int bg = 0; bg < timing_.numBankGroups; bg++) {
            for (int bk = 0; bk < timing_.numBanksPerGroup; bk++) {
                int bi = bankIdx(ch, rnk, bg, bk);
                banks_[bi].openRow     = -1;
                banks_[bi].busyUntilNs = blockUntilNs;
            }
        }
        ranks_[rankId].refreshDoneNs = std::max(ranks_[rankId].refreshDoneNs, blockUntilNs);
    }
}

double DDR5Backend::scheduleScrubRead(uint64_t addr, double requestedNs) {
    // Inject a scrub read as a regular read request; return its completion time.
    AddrFields af = decodeAddr(addr);
    int rIdx = rankIdx(af.channelId, af.rankId);
    checkRefresh(rIdx, requestedNs);
    double t = std::max(requestedNs, ranks_[rIdx].refreshDoneNs);
    t = std::max(t, banks_[bankIdx(af.channelId, af.rankId, af.bgId, af.bankId)].busyUntilNs);
    // Scrub reads always go to closed row (worst case = miss)
    // to avoid disturbing the open row policy for real traffic.
    double completeNs = t + timing_.tRCD_ns + timing_.tCL_ns + timing_.tBL_ns;
    return completeNs;
}

}  // namespace cxl
