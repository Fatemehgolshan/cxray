#include "cxl_device.h"
#include "simple_dram_backend.h"
#include "ddr5_backend.h"
#include <algorithm>
#include <stdexcept>

namespace cxl {

// ---------------------------------------------------------------------------
// DCOHSnoopFilter
// ---------------------------------------------------------------------------

bool DCOHSnoopFilter::probe(uint64_t addr) const {
    uint32_t set = (addr >> 6) % NUM_SETS;
    uint64_t tag = addr >> 6;
    for (uint32_t w = 0; w < ASSOC; w++) {
        if (sets[set][w].valid && sets[set][w].tag == tag) return true;
    }
    return false;
}

void DCOHSnoopFilter::insert(uint64_t addr) {
    uint32_t set = (addr >> 6) % NUM_SETS;
    uint64_t tag = addr >> 6;
    // Check if already present — update state to EXCLUSIVE if re-inserting
    for (uint32_t w = 0; w < ASSOC; w++) {
        if (sets[set][w].valid && sets[set][w].tag == tag) {
            sets[set][w].state = CacheLineState::EXCLUSIVE;
            return;
        }
    }
    // Find empty way
    for (uint32_t w = 0; w < ASSOC; w++) {
        if (!sets[set][w].valid) {
            sets[set][w] = {tag, true, CacheLineState::EXCLUSIVE};
            return;
        }
    }
    // Evict way 0 (pseudo-LRU)
    sets[set][0] = {tag, true, CacheLineState::EXCLUSIVE};
}

void DCOHSnoopFilter::evict(uint64_t addr) {
    uint32_t set = (addr >> 6) % NUM_SETS;
    uint64_t tag = addr >> 6;
    for (uint32_t w = 0; w < ASSOC; w++) {
        if (sets[set][w].valid && sets[set][w].tag == tag) {
            sets[set][w].valid = false;
            sets[set][w].state = CacheLineState::INVALID;
            return;
        }
    }
}

CacheLineState DCOHSnoopFilter::getState(uint64_t addr) const {
    uint32_t set = (addr >> 6) % NUM_SETS;
    uint64_t tag = addr >> 6;
    for (uint32_t w = 0; w < ASSOC; w++) {
        if (sets[set][w].valid && sets[set][w].tag == tag) {
            return sets[set][w].state;
        }
    }
    return CacheLineState::INVALID;
}

void DCOHSnoopFilter::setState(uint64_t addr, CacheLineState state) {
    uint32_t set = (addr >> 6) % NUM_SETS;
    uint64_t tag = addr >> 6;
    for (uint32_t w = 0; w < ASSOC; w++) {
        if (sets[set][w].valid && sets[set][w].tag == tag) {
            sets[set][w].state = state;
            return;
        }
    }
}

bool DCOHSnoopFilter::needsBackInvalidation(uint64_t addr, bool hostIsWrite) const {
    CacheLineState st = getState(addr);
    if (st == CacheLineState::MODIFIED) {
        // Device has dirty data — must writeback regardless of read or write
        return true;
    }
    if (st == CacheLineState::EXCLUSIVE && hostIsWrite) {
        // Device has the only clean copy and host wants to write — BI required
        return true;
    }
    // SHARED or INVALID: no BI needed
    return false;
}

// ---------------------------------------------------------------------------
// CXLDevice
// ---------------------------------------------------------------------------

CXLDevice::CXLDevice(const DeviceConfig& cfg, const RASConfig& rasCfg, uint32_t seed)
    : cfg_(cfg)
{
    // HDM: map entire address space by default
    hdm_.baseAddr   = 0;
    hdm_.sizeBytes  = static_cast<uint64_t>(cfg.hdmSizeGB) << 30;
    hdm_.interleaveGran = cfg.hdmInterleaveGranBytes;

    // Create DRAM backend
    dram_.reset(createDRAMBackend(
        cfg.dramBackendType,
        cfg.dramTiming,
        cfg.numChannels,
        cfg.numRanks,
        cfg.numBanks,
        cfg.dramConfigFile));

    // RAS engine
    ras_ = std::make_unique<RASEngine>(rasCfg, seed);

    // DCOH for Type 2
    if (cfg.type == 2) {
        dcoh_ = std::make_unique<DCOHSnoopFilter>();
    }
}

ProcessResult CXLDevice::processRequestDetailed(const M2SReq& req,
                                                 double arrivalAtDeviceNs,
                                                 bool& rowHitOut) {
    ProcessResult result;
    double t = arrivalAtDeviceNs;

    // 1. Controller latency (parsing, address decode, HDM lookup)
    t += cfg_.controllerLatencyNs;

    // 1b. CXL.cache coherence overhead / Back-Invalidation
    double coherenceNs = 0.0;
    bool isWrite = (req.opcode == M2SOpcode::MemWr || req.opcode == M2SOpcode::MemWrPtl);

    if (cfg_.type == 2) {
        if (dcoh_ && cfg_.bi.enable && dcoh_->needsBackInvalidation(req.addr, isWrite)) {
            // M state (or E on write): device writeback + BI round-trip
            coherenceNs = cfg_.bi.writebackNs + cfg_.bi.biRttNs;
            result.biTriggered = true;
        } else {
            coherenceNs = cfg_.coherenceOverheadNs;  // DCOH snoop-filter lookup
        }
    } else {
        // Type 1/3: no host-coherent DCOH overhead
        coherenceNs = 0.0;
    }
    t += coherenceNs;
    result.coherenceNs = coherenceNs;

    // 2. RAS overhead (scrub contention, PPR stall, viral, poison check)
    auto* ddr5ForRas = dynamic_cast<DDR5Backend*>(dram_.get());
    auto rasResult = ras_->process(req.addr, isWrite, t, 64, ddr5ForRas);
    t += rasResult.extraNs;
    result.rasNs = rasResult.extraNs;

    // 3. DRAM access
    uint64_t dramAddr = hdm_.translate(req.addr);
    uint64_t dramReqId = dram_->addRequest(dramAddr, isWrite, t);
    double dramDone = dram_->getCompletionNs(dramReqId);
    // Use DDR5Backend's accurate hit tracking if available, otherwise fall back to heuristic
    {
        auto* ddr5 = dynamic_cast<DDR5Backend*>(dram_.get());
        if (ddr5) {
            rowHitOut = ddr5->wasRowHit(dramReqId);
        } else {
            // Heuristic: hit if latency ≈ tCL+tBL+tCCD_L (within 2 ns tolerance)
            double hitThresh = cfg_.dramTiming.rowHitLatNs() + cfg_.dramTiming.tCCD_L_ns + 2.0;
            rowHitOut = (dramDone - t) <= hitThresh;
        }
    }
    t = dramDone;

    // 4. For Type 2: update DCOH snoop filter with MESI state transitions
    if (cfg_.type == 2 && dcoh_) {
        if (isWrite) {
            dcoh_->evict(req.addr);  // host write invalidates device copy
        } else {
            // Host read: device line transitions to Shared (if device had E/M, now downgraded)
            auto prevState = dcoh_->getState(req.addr);
            if (prevState == CacheLineState::MODIFIED ||
                prevState == CacheLineState::EXCLUSIVE) {
                dcoh_->setState(req.addr, CacheLineState::SHARED);
            }
            // If device doesn't have line (INVALID): insert it as EXCLUSIVE
            // so subsequent probes work; host will also get a copy (Shared semantics)
            // but we track device's view only.
            if (prevState == CacheLineState::INVALID) {
                dcoh_->insert(req.addr);
            }
        }
    }

    // 5. IDE (Integrity and Data Encryption) — AES-256-GCM pipeline.
    //    Applied on the response path: decrypt + auth-tag verification before
    //    data is forwarded to the host LLC.
    if (cfg_.ideEnable) {
        // TSP session setup (one-time, amortized)
        if (cfg_.tsp.enable && !tspSessionEstablished_) {
            t += cfg_.tsp.sessionSetupNs;
            tspSessionEstablished_ = true;
            result.tspSetup = true;
        }
        // Key rotation
        if (cfg_.tsp.enable) {
            tspBytesThisKey_ += 64;  // one cacheline
            if (tspBytesThisKey_ >= cfg_.tsp.keyRotationBytes) {
                t += cfg_.tsp.keyRotationNs;
                tspBytesThisKey_ = 0;
                result.tspKeyRotated = true;
            }
        }
        t += cfg_.ideLatencyNs;
    }

    result.totalNs = t - arrivalAtDeviceNs;
    return result;
}

double CXLDevice::computePreDRAMNs(const M2SReq& req, double arrivalNs,
                                    double& coherenceNsOut) {
    double t = arrivalNs;
    bool isWrite = (req.opcode == M2SOpcode::MemWr || req.opcode == M2SOpcode::MemWrPtl);

    // Controller latency (address decode, HDM lookup)
    t += cfg_.controllerLatencyNs;

    // CXL.cache coherence overhead / Back-Invalidation
    double coherenceNs = 0.0;
    if (cfg_.type == 2) {
        if (dcoh_ && cfg_.bi.enable && dcoh_->needsBackInvalidation(req.addr, isWrite)) {
            coherenceNs = cfg_.bi.writebackNs + cfg_.bi.biRttNs;
        } else {
            coherenceNs = cfg_.coherenceOverheadNs;  // DCOH snoop-filter lookup
        }
    } else {
        // Type 1/3: no host-coherent DCOH overhead
        coherenceNs = 0.0;
    }
    t += coherenceNs;
    coherenceNsOut = coherenceNs;

    // RAS overhead (scrub contention, PPR stall, viral, poison check)
    auto* ddr5ForRas = dynamic_cast<DDR5Backend*>(dram_.get());
    auto rasResult = ras_->process(req.addr, isWrite, t, 64, ddr5ForRas);
    t += rasResult.extraNs;

    // Apply DCOH state transitions (must happen regardless of DRAM path)
    if (cfg_.type == 2 && dcoh_) {
        if (isWrite) {
            dcoh_->evict(req.addr);
        } else {
            auto prevState = dcoh_->getState(req.addr);
            if (prevState == CacheLineState::MODIFIED ||
                prevState == CacheLineState::EXCLUSIVE) {
                dcoh_->setState(req.addr, CacheLineState::SHARED);
            }
            if (prevState == CacheLineState::INVALID) {
                dcoh_->insert(req.addr);
            }
        }
    }

    // IDE forward-path encryption: same placement as processRequestDetailed().
    // Must be present here so the detailed sim engine path matches the fast path.
    if (cfg_.ideEnable) {
        if (cfg_.tsp.enable && !tspSessionEstablished_) {
            t += cfg_.tsp.sessionSetupNs;
            tspSessionEstablished_ = true;
        }
        if (cfg_.tsp.enable) {
            tspBytesThisKey_ += 64;
            if (tspBytesThisKey_ >= cfg_.tsp.keyRotationBytes) {
                t += cfg_.tsp.keyRotationNs;
                tspBytesThisKey_ = 0;
            }
        }
        t += cfg_.ideLatencyNs;
    }

    return t - arrivalNs;
}

double CXLDevice::processRequest(const M2SReq& req, double arrivalAtDeviceNs,
                                  bool& rowHitOut) {
    return processRequestDetailed(req, arrivalAtDeviceNs, rowHitOut).totalNs;
}

}  // namespace cxl
