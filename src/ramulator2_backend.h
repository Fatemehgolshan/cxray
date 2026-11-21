#pragma once
#include "cxl_dram_backend.h"
#include <string>
#include <unordered_map>
#include <functional>
#include <stdexcept>

#ifdef HAVE_RAMULATOR2
#include "base/base.h"
#include "base/request.h"
#include "base/memory_system.h"
#endif

namespace cxl {

#ifdef HAVE_RAMULATOR2

// ---------------------------------------------------------------------------
// Ramulator2Backend — wraps the Ramulator2 IMemorySystem as an IDRAMBackend.
//
// Ramulator2 is callback-based: you call mem_->send(req) with a Request
// that carries a completion callback. We bridge this to the request-ID poll
// model of IDRAMBackend.
// ---------------------------------------------------------------------------
class Ramulator2Backend : public IDRAMBackend {
public:
    // cfg_path: path to Ramulator2 YAML config file
    explicit Ramulator2Backend(const std::string& cfgPath);
    ~Ramulator2Backend() override;

    uint64_t addRequest(uint64_t addr, bool isWrite, double arrivalNs) override;
    double   getCompletionNs(uint64_t requestId) override;
    void     tick(double currentNs) override;

    double   getBandwidthGBps() const override;
    double   getRowHitRate()    const override;
    uint64_t getRowHits()       const override;
    uint64_t getRowMisses()     const override;
    uint64_t getRowConflicts()  const override;
    std::string name() const override { return "Ramulator2"; }

private:
    Ramulator::IMemorySystem* mem_;

    // Clock: Ramulator2 runs in integer cycles; we convert ns <-> cycles
    double   tCK_ns_;       // clock period (ns)
    uint64_t clockCycle_;   // current Ramulator2 clock cycle

    // Request tracking
    uint64_t nextId_ = 1;
    struct PendingReq {
        uint64_t addr;
        bool     isWrite;
        double   arrivalNs;
        double   completionNs = -1.0;  // set by callback
    };
    std::unordered_map<uint64_t, PendingReq> pending_;  // id -> req
    std::unordered_map<uint64_t, uint64_t>   addrToId_; // addr -> id (for callbacks)

    // Stats
    uint64_t rowHits_     = 0;
    uint64_t rowMisses_   = 0;
    uint64_t rowConflicts_= 0;
    uint64_t totalBytes_  = 0;
    double   simDurNs_    = 0.0;

    // Callback invoked by Ramulator2 on request completion
    void completionCallback(Ramulator::Request& req);

    // Advance Ramulator2 to cover the given ns timestamp
    void advanceTo(double targetNs);
};

#else  // !HAVE_RAMULATOR2

// Stub: satisfies linker when Ramulator2 is not available.
// createDRAMBackend() will never instantiate this.
class Ramulator2Backend : public IDRAMBackend {
public:
    explicit Ramulator2Backend(const std::string&) {
        throw std::runtime_error("Ramulator2 not compiled in (set RAMULATOR2_DIR in CMake)");
    }
    uint64_t addRequest(uint64_t, bool, double) override { return 0; }
    double   getCompletionNs(uint64_t)          override { return -1; }
    void     tick(double)                        override {}
    double   getBandwidthGBps() const override { return 0; }
    double   getRowHitRate()    const override { return 0; }
    uint64_t getRowHits()       const override { return 0; }
    uint64_t getRowMisses()     const override { return 0; }
    uint64_t getRowConflicts()  const override { return 0; }
    std::string name() const override { return "Ramulator2-stub"; }
};

#endif  // HAVE_RAMULATOR2

}  // namespace cxl
