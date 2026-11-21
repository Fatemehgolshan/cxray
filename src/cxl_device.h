#pragma once
#include "cxl_protocol.h"
#include "cxl_dram_backend.h"
#include "cxl_ras.h"
#include <cstdint>
#include <string>
#include <memory>

namespace cxl {

// ---------------------------------------------------------------------------
// CXL Device Models — Type 1, 2, 3.
// Type 3: Memory Expander (HDM decoder + DRAM backend + RAS engine).
// Type 2: Accelerator + HDM (adds DCOH snoop filter on top of Type 3).
// Type 1: I/O Accelerator (CXL.cache only, no DRAM — not modeled here).
// ---------------------------------------------------------------------------

struct BackInvalidationConfig {
    bool   enable      = true;    // model BI transactions
    double biRttNs     = 210.0;  // round-trip time for BISnp→BIRsp (ns)
    double writebackNs = 15.0;   // device writeback to DRAM for M-state line
};

struct TSPConfig {
    bool     enable           = false;
    double   sessionSetupNs   = 800.0;   // one-time key exchange overhead
    double   keyRotationNs    = 120.0;   // per-rotation stall
    uint64_t keyRotationBytes = 64ULL * 1024 * 1024;  // rotate every 64 MB
};

struct DeviceConfig {
    int          type                    = 3;
    uint32_t     queueDepth              = 64;
    double       controllerLatencyNs     = 50.0;
    uint64_t     hdmSizeGB               = 128;
    uint32_t     hdmInterleaveGranBytes  = 256;
    std::string  rowBufferPolicy         = "open";
    int          numChannels             = 1;
    int          numRanks                = 2;
    int          numBanks                = 16;
    std::string  dramBackendType         = "simple";
    std::string  dramConfigFile          = "";
    DRAMTimingParams dramTiming;

    // CXL.cache coherence overhead — host-side DCOH snoop-filter lookup latency
    // that serialises with each memory request (even for Type 3, the host
    // CXL port does a directory/cache-probe before forwarding to the device).
    // Typical measured value: 3–5 ns on SPR-class platforms.
    double       coherenceOverheadNs     = 0.0;

    // IDE (Integrity and Data Encryption) — AES-256-GCM pipeline
    // Mandatory for multi-tenant CXL 3.1 deployments (TSP + IDE §4.5).
    // Encrypts data in-flight; decrypt + auth-tag check adds ~8 ns at line rate.
    bool         ideEnable               = false;
    double       ideLatencyNs            = 8.0;

    // Back-Invalidation configuration (Type 2 only)
    BackInvalidationConfig bi;

    // TSP (Trusted Security Protocol) session modeling
    TSPConfig tsp;
};

// HDM decoder: maps incoming physical addresses to backend bank
struct HDMDecoder {
    uint64_t baseAddr     = 0;
    uint64_t sizeBytes    = 0;
    uint32_t interleaveGran = 256;

    bool contains(uint64_t addr) const {
        return addr >= baseAddr && addr < baseAddr + sizeBytes;
    }
    // Map host PA → device-local address
    uint64_t translate(uint64_t addr) const {
        return addr - baseAddr;
    }
};

// MESI cache line state for DCOH snoop filter
enum class CacheLineState : uint8_t {
    INVALID   = 0,
    SHARED    = 1,
    EXCLUSIVE = 2,
    MODIFIED  = 3,
};

// DCOH snoop filter (Type 2 only) — inclusive, direct-mapped per set
struct DCOHSnoopFilter {
    static constexpr uint32_t NUM_SETS = 1024;
    static constexpr uint32_t ASSOC    = 8;

    struct CacheLineEntry {
        uint64_t       tag   = 0;
        bool           valid = false;
        CacheLineState state = CacheLineState::INVALID;
    };
    CacheLineEntry sets[NUM_SETS][ASSOC] = {};

    // Returns true if line is cached in device (possible BI needed)
    bool probe(uint64_t addr) const;
    void insert(uint64_t addr);
    void evict(uint64_t addr);

    // Returns state of the line (INVALID if not present)
    CacheLineState getState(uint64_t addr) const;

    // Set the MESI state of a line already in the snoop filter
    void setState(uint64_t addr, CacheLineState state);

    // Returns true if a Back-Invalidation is required for a host access.
    // hostIsWrite: true for MemWr/MemWrPtl, false for reads.
    // M state: BI required (dirty writeback + round-trip).
    // E state on host write: BI required (host is taking ownership).
    // S state: no BI needed for reads; host write may still invalidate but
    //          that is handled via ordinary invalidation, not full BI here.
    bool needsBackInvalidation(uint64_t addr, bool hostIsWrite) const;
};

// Result struct returned by processRequestDetailed()
struct ProcessResult {
    double totalNs       = 0.0;
    double coherenceNs   = 0.0;  // includes BI overhead if triggered
    double rasNs         = 0.0;  // RAS stall overhead (scrub, PPR, viral, poison)
    bool   biTriggered   = false;
    bool   tspSetup      = false;
    bool   tspKeyRotated = false;
};

class CXLDevice {
public:
    CXLDevice(const DeviceConfig& cfg, const RASConfig& rasCfg, uint32_t seed = 42);

    // Process one M2S request. Returns total device-side latency (ns).
    // Includes: controller latency + DRAM latency + RAS overhead.
    double processRequest(const M2SReq& req, double arrivalAtDeviceNs,
                          bool& rowHitOut);

    // Detailed variant — returns per-component breakdown.
    ProcessResult processRequestDetailed(const M2SReq& req, double arrivalAtDeviceNs,
                                         bool& rowHitOut);

    // Compute pre-DRAM overhead (controller + coherence + RAS) and apply DCOH
    // state updates, WITHOUT submitting a DRAM request.  Use this in the
    // event-driven detailed pipeline so that DRAM timing comes exclusively from
    // the DetailedDRAMQueue.  Returns the number of ns to add to arrivalNs
    // before DRAM dispatch; coherenceNsOut receives the coherence component.
    double computePreDRAMNs(const M2SReq& req, double arrivalNs,
                             double& coherenceNsOut);

    // Stats
    double   dramRowHitRate()   const { return dram_->getRowHitRate(); }
    uint64_t dramRowHits()      const { return dram_->getRowHits(); }
    uint64_t dramRowMisses()    const { return dram_->getRowMisses(); }
    uint64_t dramRowConflicts() const { return dram_->getRowConflicts(); }
    double   dramBandwidthGBps()const { return dram_->getBandwidthGBps(); }

    RASEngine&       ras()        { return *ras_; }
    const RASEngine& ras() const  { return *ras_; }
    IDRAMBackend*    dram()       { return dram_.get(); }

    const DeviceConfig& config() const { return cfg_; }
    const HDMDecoder&   hdm()    const { return hdm_; }

private:
    DeviceConfig                  cfg_;
    HDMDecoder                    hdm_;
    std::unique_ptr<IDRAMBackend> dram_;
    std::unique_ptr<RASEngine>    ras_;
    std::unique_ptr<DCOHSnoopFilter> dcoh_;  // Type 2 only

    // TSP session state
    bool     tspSessionEstablished_ = false;
    uint64_t tspBytesThisKey_       = 0;
};

}  // namespace cxl
