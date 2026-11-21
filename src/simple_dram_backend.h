#pragma once
#include "cxl_dram_backend.h"
#include <unordered_map>
#include <vector>
#include <string>

namespace cxl {

// ---------------------------------------------------------------------------
// SimpleDRAMBackend — analytical open/closed page model.
// Used in CXRay fast mode. No cycle-level events — pure timing formulas.
// ---------------------------------------------------------------------------

class SimpleDRAMBackend : public IDRAMBackend {
public:
    SimpleDRAMBackend(const DRAMTimingParams& timing,
                      int numChannels,
                      int numRanks,
                      int numBanks,
                      const std::string& rowPolicy = "open");

    uint64_t addRequest(uint64_t addr, bool isWrite, double arrivalNs) override;
    double   getCompletionNs(uint64_t requestId) override;
    void     tick(double /*currentNs*/) override {}   // no-op for analytical model
    double   getBandwidthGBps() const override;
    double   getRowHitRate()    const override;
    uint64_t getRowHits()       const override { return rowHits_; }
    uint64_t getRowMisses()     const override { return rowMisses_; }
    uint64_t getRowConflicts()  const override { return rowConflicts_; }
    std::string name()          const override { return "SimpleDRAM"; }

private:
    struct BankState {
        int64_t  openRow     = -1;  // -1 = no open row (closed)
        double   busyUntilNs = 0.0; // row still being accessed
    };

    // Address mapping: addr → (bankId, rowId)
    void addrToBank(uint64_t addr, int& bankId, int64_t& rowId) const;

    DRAMTimingParams timing_;
    int numBanks_;         // total banks = channels × ranks × banks_per_rank
    int numRows_          = 65536;
    bool openPage_;

    std::vector<BankState> banks_;

    // Request → completion time map
    uint64_t nextId_   = 0;
    struct PendingReq { double completeNs; };
    std::unordered_map<uint64_t, PendingReq> pending_;

    // Stats
    mutable uint64_t rowHits_      = 0;
    mutable uint64_t rowMisses_    = 0;
    mutable uint64_t rowConflicts_ = 0;
    double totalBytesNs_   = 0.0;  // accumulated bytes × ns for BW calc
    double simDurationNs_  = 0.0;
    uint64_t totalBytes_   = 0;
};

}  // namespace cxl
