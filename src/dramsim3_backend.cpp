// ---------------------------------------------------------------------------
// dramsim3_backend.cpp — DRAMSim3 backend implementation for CXRay.
//
// Integration note: The createDRAMBackend() factory lives in
// simple_dram_backend.cpp (which this project does not own here).
// To wire up the "dramsim3" case in the factory, add the following to
// simple_dram_backend.cpp (inside createDRAMBackend, before the final throw):
//
//   #ifdef HAVE_DRAMSIM3
//   if (type == "dramsim3") {
//       return new DRAMSim3Backend(configFile, ".");
//   }
//   #endif
//
// and add:  #include "dramsim3_backend.h"
// at the top of simple_dram_backend.cpp.
// ---------------------------------------------------------------------------

#include "dramsim3_backend.h"
#include <stdexcept>
#include <cmath>

namespace cxl {

#ifdef HAVE_DRAMSIM3

DRAMSim3Backend::DRAMSim3Backend(const std::string& cfgPath,
                                 const std::string& outputDir)
    : clockCycle_(0)
{
    // DRAMSim3 API: GetMemorySystem returns a MemorySystem*
    // Read callbacks are registered at construction
    mem_ = dramsim3::GetMemorySystem(
        cfgPath, outputDir,
        [this](uint64_t addr) { readCallback(addr); },
        [this](uint64_t addr) { writeCallback(addr); }
    );

    // Get tCK from DRAMSim3 config (it exposes timing via GetTCK or similar).
    // Use a default of 0.417 ns (DDR5-4800) if not exposed by the library.
    tCK_ns_ = 0.417;  // override at runtime if DRAMSim3 exposes this
}

DRAMSim3Backend::~DRAMSim3Backend() {
    if (mem_) {
        mem_->PrintStats();
        delete mem_;
    }
}

uint64_t DRAMSim3Backend::addRequest(uint64_t addr, bool isWrite, double arrivalNs) {
    uint64_t id = nextId_++;
    pending_[id] = { addr, isWrite, arrivalNs, -1.0 };
    addrToId_[addr] = id;

    // Advance DRAMSim3 to arrivalNs before adding the transaction
    advanceTo(arrivalNs);

    bool ok = mem_->WillAcceptTransaction(addr, isWrite);
    if (!ok) {
        // Queue full: add a small stall and retry
        advanceTo(arrivalNs + tCK_ns_ * 4);
    }
    mem_->AddTransaction(addr, isWrite);
    totalBytes_ += 64;
    return id;
}

double DRAMSim3Backend::getCompletionNs(uint64_t requestId) {
    auto it = pending_.find(requestId);
    if (it == pending_.end()) return -1.0;

    // If not yet complete, advance simulation until it completes
    if (it->second.completionNs < 0) {
        // Advance up to 1000 cycles to find completion
        for (int i = 0; i < 1000 && it->second.completionNs < 0; ++i) {
            mem_->ClockTick();
            clockCycle_++;
        }
    }
    return it->second.completionNs;
}

void DRAMSim3Backend::tick(double currentNs) {
    advanceTo(currentNs);
}

void DRAMSim3Backend::advanceTo(double targetNs) {
    uint64_t targetCycle = static_cast<uint64_t>(std::ceil(targetNs / tCK_ns_));
    while (clockCycle_ < targetCycle) {
        mem_->ClockTick();
        clockCycle_++;
    }
    simDurNs_ = clockCycle_ * tCK_ns_;
}

void DRAMSim3Backend::readCallback(uint64_t addr) {
    auto it = addrToId_.find(addr);
    if (it != addrToId_.end()) {
        uint64_t id = it->second;
        if (pending_.count(id)) {
            pending_[id].completionNs = clockCycle_ * tCK_ns_;
            rowHits_++;  // approximate (DRAMSim3 doesn't expose hit/miss easily)
        }
    }
}

void DRAMSim3Backend::writeCallback(uint64_t addr) {
    auto it = addrToId_.find(addr);
    if (it != addrToId_.end()) {
        uint64_t id = it->second;
        if (pending_.count(id))
            pending_[id].completionNs = clockCycle_ * tCK_ns_;
    }
}

double DRAMSim3Backend::getBandwidthGBps() const {
    if (simDurNs_ <= 0) return 0.0;
    return static_cast<double>(totalBytes_) / simDurNs_;
}

double DRAMSim3Backend::getRowHitRate() const {
    uint64_t total = rowHits_ + rowMisses_;
    return total ? static_cast<double>(rowHits_) / total : 0.0;
}

uint64_t DRAMSim3Backend::getRowHits()      const { return rowHits_; }
uint64_t DRAMSim3Backend::getRowMisses()    const { return rowMisses_; }
uint64_t DRAMSim3Backend::getRowConflicts() const { return 0; }

#endif  // HAVE_DRAMSIM3

}  // namespace cxl
