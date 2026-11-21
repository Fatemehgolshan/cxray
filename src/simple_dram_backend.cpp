#include "simple_dram_backend.h"
#include "ddr5_backend.h"
#include "dramsim3_backend.h"
#include "ramulator2_backend.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace cxl {

SimpleDRAMBackend::SimpleDRAMBackend(const DRAMTimingParams& timing,
                                     int numChannels,
                                     int numRanks,
                                     int numBanks,
                                     const std::string& rowPolicy)
    : timing_(timing),
      numBanks_(numChannels * numRanks * numBanks),
      openPage_(rowPolicy == "open"),
      banks_(numChannels * numRanks * numBanks)
{}

void SimpleDRAMBackend::addrToBank(uint64_t addr, int& bankId, int64_t& rowId) const {
    // Cacheline-granularity bank/row mapping (RoBaRaCoCh)
    uint64_t cl = addr >> 6;  // cacheline index
    bankId = static_cast<int>(cl % numBanks_);
    rowId  = static_cast<int64_t>((cl / numBanks_) % numRows_);
}

uint64_t SimpleDRAMBackend::addRequest(uint64_t addr, bool /*isWrite*/, double arrivalNs) {
    int bankId;
    int64_t rowId;
    addrToBank(addr, bankId, rowId);

    BankState& bank = banks_[bankId];
    double latNs;

    // Determine if row hit/miss/conflict
    if (openPage_) {
        if (bank.openRow == rowId) {
            // Row hit
            latNs = timing_.rowHitLatNs();
            rowHits_++;
        } else if (bank.openRow < 0) {
            // Empty bank — miss (activate)
            latNs = timing_.rowMissLatNs();
            rowMisses_++;
            bank.openRow = rowId;
        } else {
            // Row conflict — precharge + activate
            latNs = timing_.rowConflictLatNs();
            rowConflicts_++;
            bank.openRow = rowId;
        }
    } else {
        // Closed-page: always miss
        latNs = timing_.rowMissLatNs();
        rowMisses_++;
        bank.openRow = -1;
    }

    // Bank busy until: max(arrival, previous access end) + latency
    double startNs = std::max(arrivalNs, bank.busyUntilNs);
    double completeNs = startNs + latNs;
    bank.busyUntilNs = completeNs;

    // Track stats
    totalBytes_ += 64;
    if (simDurationNs_ < completeNs) simDurationNs_ = completeNs;

    uint64_t id = nextId_++;
    pending_[id] = {completeNs};
    return id;
}

double SimpleDRAMBackend::getCompletionNs(uint64_t requestId) {
    auto it = pending_.find(requestId);
    if (it == pending_.end()) return -1.0;
    return it->second.completeNs;
}

double SimpleDRAMBackend::getBandwidthGBps() const {
    if (simDurationNs_ <= 0) return 0.0;
    return (static_cast<double>(totalBytes_) / simDurationNs_);  // bytes/ns = GB/s
}

double SimpleDRAMBackend::getRowHitRate() const {
    uint64_t total = rowHits_ + rowMisses_ + rowConflicts_;
    return total > 0 ? static_cast<double>(rowHits_) / total : 0.0;
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

IDRAMBackend* createDRAMBackend(const std::string& type,
                                const DRAMTimingParams& timing,
                                int numChannels,
                                int numRanks,
                                int numBanks,
                                const std::string& configFile) {
    (void)configFile;
    (void)numBanks;

    if (type == "simple" || type == "") {
        return new SimpleDRAMBackend(timing, numChannels, numRanks,
                                     timing.totalBanksPerRank());
    }
    // DDR5 / DDR4 backends — select timing preset based on type string
    if (type == "ddr5" || type == "ddr5_4800") {
        return new DDR5Backend(DRAMTimingParams::DDR5_4800(), numChannels, numRanks);
    }
    if (type == "ddr5_6400") {
        return new DDR5Backend(DRAMTimingParams::DDR5_6400(), numChannels, numRanks);
    }
    if (type == "ddr4" || type == "ddr4_3200") {
        return new DDR5Backend(DRAMTimingParams::DDR4_3200(), numChannels, numRanks);
    }
    if (type == "ddr4_2400") {
        return new DDR5Backend(DRAMTimingParams::DDR4_2400(), numChannels, numRanks);
    }
    // External backends: DRAMSim3 and Ramulator2
    if (type == "dramsim3") {
        return new DRAMSim3Backend(configFile.empty() ? "DDR5_4800.ini" : configFile, ".");
    }
    if (type == "ramulator2") {
        return new Ramulator2Backend(configFile.empty() ? "ramulator2.yaml" : configFile);
    }
    throw std::runtime_error("DRAM backend '" + type + "' not compiled in. "
                             "Supported: simple, ddr5, ddr5_4800, ddr5_6400, "
                             "ddr4, ddr4_3200, ddr4_2400.");
}

}  // namespace cxl
