// ---------------------------------------------------------------------------
// ramulator2_backend.cpp — Ramulator2 backend implementation for CXRay.
//
// Integration note: The createDRAMBackend() factory lives in
// simple_dram_backend.cpp (which this project does not own here).
// To wire up the "ramulator2" case in the factory, add the following to
// simple_dram_backend.cpp (inside createDRAMBackend, before the final throw):
//
//   #ifdef HAVE_RAMULATOR2
//   if (type == "ramulator2") {
//       return new Ramulator2Backend(configFile);
//   }
//   #endif
//
// and add:  #include "ramulator2_backend.h"
// at the top of simple_dram_backend.cpp.
// ---------------------------------------------------------------------------

#include "ramulator2_backend.h"
#include <stdexcept>
#include <cmath>

namespace cxl {

#ifdef HAVE_RAMULATOR2

Ramulator2Backend::Ramulator2Backend(const std::string& cfgPath)
    : clockCycle_(0)
{
    // Ramulator2 uses a YAML config file.  Create() parses the config and
    // returns an IMemorySystem*.  The exact factory call follows the
    // Ramulator2 public API (YAML::LoadFile + IMemorySystem::create).
    auto config = Ramulator::Config::parse_config_file(cfgPath, {});
    mem_ = Ramulator::Factory::create_memory_system(config);
    mem_->init();

    // Default tCK for DDR5-4800; Ramulator2 may expose this via config.
    tCK_ns_ = 0.417;
}

Ramulator2Backend::~Ramulator2Backend() {
    if (mem_) {
        mem_->finalize();
        delete mem_;
    }
}

uint64_t Ramulator2Backend::addRequest(uint64_t addr, bool isWrite, double arrivalNs) {
    uint64_t id = nextId_++;
    pending_[id] = { addr, isWrite, arrivalNs, -1.0 };
    addrToId_[addr] = id;

    // Advance Ramulator2 to arrivalNs before submitting the request
    advanceTo(arrivalNs);

    // Build a Ramulator2 Request with a completion callback
    Ramulator::Request req(
        static_cast<Ramulator::Addr_t>(addr),
        isWrite ? Ramulator::Request::Type::Write : Ramulator::Request::Type::Read
    );
    req.callback = [this](Ramulator::Request& r) { completionCallback(r); };

    // Retry once if the memory system is not ready to accept
    if (!mem_->send(req)) {
        advanceTo(arrivalNs + tCK_ns_ * 4);
        mem_->send(req);
    }

    totalBytes_ += 64;
    return id;
}

double Ramulator2Backend::getCompletionNs(uint64_t requestId) {
    auto it = pending_.find(requestId);
    if (it == pending_.end()) return -1.0;

    // If not yet complete, advance simulation until it completes
    if (it->second.completionNs < 0) {
        for (int i = 0; i < 1000 && it->second.completionNs < 0; ++i) {
            mem_->tick();
            clockCycle_++;
        }
    }
    return it->second.completionNs;
}

void Ramulator2Backend::tick(double currentNs) {
    advanceTo(currentNs);
}

void Ramulator2Backend::advanceTo(double targetNs) {
    uint64_t targetCycle = static_cast<uint64_t>(std::ceil(targetNs / tCK_ns_));
    while (clockCycle_ < targetCycle) {
        mem_->tick();
        clockCycle_++;
    }
    simDurNs_ = clockCycle_ * tCK_ns_;
}

void Ramulator2Backend::completionCallback(Ramulator::Request& req) {
    uint64_t addr = static_cast<uint64_t>(req.addr);
    auto it = addrToId_.find(addr);
    if (it != addrToId_.end()) {
        uint64_t id = it->second;
        auto pit = pending_.find(id);
        if (pit != pending_.end()) {
            pit->second.completionNs = clockCycle_ * tCK_ns_;
            // Ramulator2 does not directly expose row hit/miss in the callback;
            // use a simple heuristic: count all completions as hits for now.
            // A more accurate approach would inspect req.command history.
            rowHits_++;
        }
    }
}

double Ramulator2Backend::getBandwidthGBps() const {
    if (simDurNs_ <= 0) return 0.0;
    return static_cast<double>(totalBytes_) / simDurNs_;
}

double Ramulator2Backend::getRowHitRate() const {
    uint64_t total = rowHits_ + rowMisses_ + rowConflicts_;
    return total ? static_cast<double>(rowHits_) / total : 0.0;
}

uint64_t Ramulator2Backend::getRowHits()      const { return rowHits_; }
uint64_t Ramulator2Backend::getRowMisses()    const { return rowMisses_; }
uint64_t Ramulator2Backend::getRowConflicts() const { return rowConflicts_; }

#endif  // HAVE_RAMULATOR2

}  // namespace cxl
