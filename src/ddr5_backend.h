#pragma once
// ---------------------------------------------------------------------------
// DDR5Backend — JEDEC-accurate DDR5/DDR4 DRAM backend.
//
// Features:
//   • Arrival-order scheduling (requests served in per-bank arrival order)
//   • Per-bank-group timing: tRRD_S/L, tCCD_S/L, tWTR_S/L
//   • Four-activation window (tFAW)
//   • Automatic all-bank refresh (tREFI / tRFC)
//   • Write buffer with configurable drain threshold
//   • Per-bank state machine: IDLE → ACTIVE → row-hit CAS |
//                             conflict PRE → ACTIVE → CAS
//   • RAS integration hooks: sPPR bank block, hPPR rank block, scrub reads
// ---------------------------------------------------------------------------

#include "cxl_dram_backend.h"
#include <vector>
#include <deque>
#include <unordered_map>
#include <string>
#include <algorithm>
#include <cmath>

namespace cxl {

// ── Per-bank state ───────────────────────────────────────────────────────────
struct BankState {
    int64_t  openRow      = -1;       // -1 = precharged / idle
    double   busyUntilNs  = 0.0;      // bank available after this time
    double   activatedNs  = -1e18;    // when last ACT was issued (for tRAS check)
    bool     lastWasWrite = false;    // for tWTR enforcement
    uint64_t rowHits      = 0;
    uint64_t rowMisses    = 0;
    uint64_t rowConflicts = 0;
    uint64_t refreshes    = 0;
};

// ── Per-bank-group timing state ──────────────────────────────────────────────
struct BankGroupState {
    double lastCasNs = -1e18;   // last CAS (read or write) command time
    double lastActNs = -1e18;   // last ACT command time
    double lastWrNs  = -1e18;   // last CAS-W time (for tWTR_S within BG)
};

// ── Per-rank state (for tFAW and refresh) ───────────────────────────────────
struct RankState {
    std::deque<double> actTimes;    // rolling ACT timestamps for tFAW window
    double lastRefreshNs = 0.0;     // simulation time when last ABR was issued
    double refreshDoneNs = 0.0;     // rank available again after ABR (+ tRFC)
    double lastCasAnyNs  = -1e18;   // any CAS, for cross-BG tWTR_L enforcement
    double lastWrAnyNs   = -1e18;   // any CAS-W, for cross-BG tWTR_L enforcement
    int    lastCasBgId   = -1;      // bank-group of most recent CAS (-1 = none yet)
};

// ── Pending write buffer entry ───────────────────────────────────────────────
struct PendingWrite {
    uint64_t id;
    uint64_t addr;
    double   arrivalNs;
    int      rankId, bgId, bankId;
    int64_t  rowId;
};

// ---------------------------------------------------------------------------
// DDR5Backend
// ---------------------------------------------------------------------------
class DDR5Backend : public IDRAMBackend {
public:
    /// @param timing         JEDEC timing parameters (use DRAMTimingParams::DDR5_4800() etc.)
    /// @param numChannels    Number of DRAM channels (default 1)
    /// @param numRanks       Number of ranks per channel (default 2)
    /// @param writeBufferCapacity  Max write buffer entries before forced drain (default 16)
    DDR5Backend(const DRAMTimingParams& timing,
                int numChannels = 1,
                int numRanks    = 2,
                int writeBufferCapacity = 16);

    // ── IDRAMBackend interface ────────────────────────────────────────────
    uint64_t    addRequest(uint64_t addr, bool isWrite, double arrivalNs) override;
    double      getCompletionNs(uint64_t requestId) override;
    void        tick(double /*currentNs*/) override {}   // event-driven; tick unused
    double      getBandwidthGBps() const override;
    double      getRowHitRate()    const override;
    uint64_t    getRowHits()       const override { return totalRowHits_; }
    uint64_t    getRowMisses()     const override { return totalRowMisses_; }
    uint64_t    getRowConflicts()  const override { return totalRowConflicts_; }
    std::string name()             const override { return "ddr5_frfcfs"; }

    /// Returns true if the most recently completed request (by id) was a row hit.
    bool wasRowHit(uint64_t requestId) const {
        auto it = pending_.find(requestId);
        return it != pending_.end() && it->second.rowHit;
    }

    // ── Timing exposure for RAS integration ─────────────────────────────
    const DRAMTimingParams& timing() const { return timing_; }

    // ── RAS integration hooks ────────────────────────────────────────────

    /// Block a specific bank during sPPR repair.
    /// globalBankId = channelId*numRanks*numBanksPerRank + rankId*numBanksPerRank + bankIdx
    void setPPRBankBlock(int globalBankId, double blockUntilNs);

    /// Block an entire rank during hPPR repair.
    void setHPPRRankBlock(int rankId, double blockUntilNs);

    /// Inject a competing patrol-scrub read at the given address / time.
    /// Returns the time when the scrub read completes.
    double scheduleScrubRead(uint64_t addr, double requestedNs);

private:
    // ── Address decode ───────────────────────────────────────────────────
    // Maps global 64-bit address → channel, rank, bank-group, bank, row, col.
    // Uses RoBaRaCoCh (Row-BankGroup-Bank-Rank-Column-Channel) interleaving
    // with bank-group awareness for DDR5.
    struct AddrFields {
        int     channelId;
        int     rankId;
        int     bgId;     // bank group index within rank
        int     bankId;   // bank index within bank group
        int64_t rowId;
        int     colId;
    };
    AddrFields decodeAddr(uint64_t addr) const;

    // ── Core scheduling ──────────────────────────────────────────────────
    double scheduleRead (AddrFields& af, double arrivalNs);
    double scheduleWrite(AddrFields& af, double arrivalNs);

    // ── Timing constraint helpers ────────────────────────────────────────
    /// Enforce tRRD_S or tRRD_L from the last ACT in the same/different BG.
    double enforceTRRD(int rankIdx, int bgId, bool sameBG, double t);
    /// Enforce tFAW: at most 4 ACT in any tFAW window.
    double enforceTFAW(RankState& rank, double t);
    /// Enforce tCCD_S or tCCD_L from last CAS in same/different BG.
    double enforceTCCD(int rankIdx, int bgId, bool sameBG, bool isWrite, double t);
    /// Enforce tWTR_S or tWTR_L (write-to-read turnaround).
    double enforceTWTR(int rankIdx, int bgId, bool sameBG, double t);

    /// Record an ACT command: update bankGroup.lastActNs, rank.actTimes.
    void doActivate (AddrFields& af, double actNs);
    /// Record a PRE command: clear openRow, apply tRP.
    void doPrecharge(AddrFields& af, double preNs);

    /// Check if a periodic all-bank refresh is due for a rank; if so, apply it.
    void checkRefresh(int rankIdx, double nowNs);

    // ── Write buffer ─────────────────────────────────────────────────────
    /// Attempt to drain write buffer entries that have been waiting.
    void   tryDrainWriteBuffer();
    /// Check if the incoming write can be satisfied from write buffer
    /// (write-merge / early ack). Returns true if handled.
    bool   checkWriteBuffer(uint64_t addr, double arrivalNs, double& completionNs);

    // ── Index helpers ────────────────────────────────────────────────────
    /// Flat bank index: [ch][rank][bg][bank]
    int bankIdx(int ch, int rank, int bg, int bank) const {
        return ((ch * numRanks_ + rank) * timing_.numBankGroups + bg)
               * timing_.numBanksPerGroup + bank;
    }
    /// Flat bank-group index: [ch][rank][bg]
    int bgIdx(int ch, int rank, int bg) const {
        return (ch * numRanks_ + rank) * timing_.numBankGroups + bg;
    }
    /// Flat rank index: [ch][rank]
    int rankIdx(int ch, int rank) const {
        return ch * numRanks_ + rank;
    }

    // ── Data members ─────────────────────────────────────────────────────
    DRAMTimingParams  timing_;
    int               numChannels_;
    int               numRanks_;
    int               numBanksPerRank_;   // numBankGroups × numBanksPerGroup
    int               writeBufferCap_;

    // Flattened state arrays (indexed via bankIdx / bgIdx / rankIdx helpers)
    std::vector<BankState>      banks_;       // [ch * numRanks * numBanksPerRank + ...]
    std::vector<BankGroupState> bankGroups_;  // [ch * numRanks * numBankGroups  + ...]
    std::vector<RankState>      ranks_;       // [ch * numRanks + rank]

    // Request completion map
    uint64_t nextId_ = 0;
    struct CompEntry { double completionNs; bool rowHit = false; };
    std::unordered_map<uint64_t, CompEntry> pending_;

    // Write buffer (stores pending write entries before draining to DRAM)
    std::deque<PendingWrite>          writeBuffer_;
    std::unordered_map<uint64_t, double> writeBufferCompletions_;  // id → completionNs

    // PPR rank block (per flat rank index)
    std::unordered_map<int, double> hpprRankBlockUntilNs_;

    // Statistics
    uint64_t totalRowHits_      = 0;
    uint64_t totalRowMisses_    = 0;
    uint64_t totalRowConflicts_ = 0;
    uint64_t totalRequests_     = 0;
    uint64_t totalBytes_        = 0;
    double   simEndNs_          = 0.0;
    double   simStartNs_        = 1e18;
};

}  // namespace cxl
