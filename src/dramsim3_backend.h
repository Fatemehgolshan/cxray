#pragma once
#include "cxl_dram_backend.h"
#include <string>
#include <unordered_map>
#include <queue>
#include <functional>
#include <stdexcept>

#ifdef HAVE_DRAMSIM3
#include "dramsim3.h"
#endif

namespace cxl {

#ifdef HAVE_DRAMSIM3

// ---------------------------------------------------------------------------
// DRAMSim3Backend — wraps the DRAMSim3 MemorySystem as an IDRAMBackend.
//
// DRAMSim3 is callback-based: you call AddTransaction() and it calls
// read_callback / write_callback when complete.
// We bridge this to the request-ID poll model of IDRAMBackend.
// ---------------------------------------------------------------------------
class DRAMSim3Backend : public IDRAMBackend {
public:
    // cfg_path: path to DRAMSim3 .ini config file (e.g. DDR5_4800.ini)
    // output_dir: DRAMSim3 stats output directory
    explicit DRAMSim3Backend(const std::string& cfgPath,
                             const std::string& outputDir = ".");
    ~DRAMSim3Backend() override;

    uint64_t addRequest(uint64_t addr, bool isWrite, double arrivalNs) override;
    double   getCompletionNs(uint64_t requestId) override;
    void     tick(double currentNs) override;

    double   getBandwidthGBps() const override;
    double   getRowHitRate()    const override;
    uint64_t getRowHits()       const override;
    uint64_t getRowMisses()     const override;
    uint64_t getRowConflicts()  const override;
    std::string name() const override { return "DRAMSim3"; }

private:
    dramsim3::MemorySystem* mem_;

    // Clock: DRAMSim3 runs in integer cycles; we convert ns <-> cycles
    double tCK_ns_;          // DRAMSim3 clock period (ns)
    uint64_t clockCycle_;    // current DRAMSim3 clock cycle

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
    uint64_t totalBytes_  = 0;
    double   simDurNs_    = 0.0;

    // Callbacks registered with DRAMSim3
    void readCallback(uint64_t addr);
    void writeCallback(uint64_t addr);

    // Advance DRAMSim3 to cover the given ns timestamp
    void advanceTo(double targetNs);
};

#else  // !HAVE_DRAMSIM3

// Stub: satisfies linker when DRAMSim3 is not available.
// createDRAMBackend() will never instantiate this.
class DRAMSim3Backend : public IDRAMBackend {
public:
    explicit DRAMSim3Backend(const std::string&, const std::string& = ".") {
        throw std::runtime_error("DRAMSim3 not compiled in (set DRAMSIM3_DIR in CMake)");
    }
    uint64_t addRequest(uint64_t, bool, double) override { return 0; }
    double   getCompletionNs(uint64_t)          override { return -1; }
    void     tick(double)                        override {}
    double   getBandwidthGBps() const override { return 0; }
    double   getRowHitRate()    const override { return 0; }
    uint64_t getRowHits()       const override { return 0; }
    uint64_t getRowMisses()     const override { return 0; }
    uint64_t getRowConflicts()  const override { return 0; }
    std::string name() const override { return "DRAMSim3-stub"; }
};

#endif  // HAVE_DRAMSIM3

}  // namespace cxl
