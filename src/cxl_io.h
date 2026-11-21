#pragma once
#include <cstdint>
#include <string>

namespace cxl {

// ── CXL.io transaction types ──────────────────────────────────────────────────

enum class IOTxnType : uint8_t {
    CFG_RD   = 0,   // Config space read (4B)
    CFG_WR   = 1,   // Config space write (4B)
    MMIO_RD  = 2,   // BAR memory read (1-256B)
    MMIO_WR  = 3,   // BAR memory write (1-256B)
    DMA_RD   = 4,   // DMA read (4KB-4MB)
    DMA_WR   = 5,   // DMA write (4KB-4MB)
    MSI      = 6,   // MSI interrupt (device→host)
    MSIX     = 7,   // MSI-X interrupt (device→host)
    CPL      = 8,   // Completion (device→host, data or status)
};

// ── CXL.io configuration ──────────────────────────────────────────────────────

struct CXLIOConfig {
    // Link parameters (shared with CXL.mem physical link)
    double linkOneWayLatencyNs    = 30.0;   // one-way link propagation

    // Config space access
    double cfgRegisterLookupNs   = 5.0;    // device config register access
    double cfgTlpOverheadNs      = 8.0;    // TLP header/framing overhead

    // MMIO
    double mmioHandlerNs         = 10.0;   // device MMIO handler latency
    double mmioTlpOverheadNs     = 8.0;    // TLP header/framing overhead

    // DMA
    double dmaDescriptorNs       = 150.0;  // per-descriptor setup latency
    double dmaTlpOverheadFrac    = 0.05;   // 5% TLP overhead fraction
    double dmaLinkBwGBps         = 32.0;   // link bandwidth for DMA (inherit from PCIe gen)

    // MSI/MSI-X
    double msiIommuLookupNs      = 20.0;   // host IOMMU lookup + routing
    double msiInterruptDeliveryNs= 50.0;   // interrupt delivery to CPU

    // Completion
    double completionOverheadNs  = 5.0;    // CplD framing overhead

    // Credit limits (CXL.io VC has its own credit pool)
    int    ioVCCredits           = 16;     // outstanding CXL.io requests
};

// ── Transaction result ────────────────────────────────────────────────────────

struct IOTxnResult {
    IOTxnType type;
    double    latencyNs;       // end-to-end latency for this transaction
    double    creditStallNs;   // credit back-pressure stall (if VC saturated)
    uint32_t  payloadBytes;    // actual payload size
    bool      isDeviceToHost;  // true for MSI/MSI-X/CPL
};

// ── CXLIOController — models CXL.io transaction latency ──────────────────────

class CXLIOController {
public:
    explicit CXLIOController(const CXLIOConfig& cfg);

    // Model a config space read (4B, host→device→host).
    IOTxnResult cfgRead(double arrivalNs, uint16_t busDevFunc, uint16_t regOffset);

    // Model a config space write (4B, host→device).
    IOTxnResult cfgWrite(double arrivalNs, uint16_t busDevFunc, uint16_t regOffset);

    // Model a BAR MMIO read (sizeBytes: 1-256B).
    IOTxnResult mmioRead(double arrivalNs, uint64_t barAddr, uint32_t sizeBytes);

    // Model a BAR MMIO write.
    IOTxnResult mmioWrite(double arrivalNs, uint64_t barAddr, uint32_t sizeBytes);

    // Model a DMA read (host→device memory, chunkBytes: 4KB-4MB).
    // Returns latency for the full DMA transfer.
    IOTxnResult dmaRead(double arrivalNs, uint64_t srcAddr, uint64_t dstAddr,
                        uint32_t chunkBytes);

    // Model a DMA write.
    IOTxnResult dmaWrite(double arrivalNs, uint64_t srcAddr, uint64_t dstAddr,
                         uint32_t chunkBytes);

    // Model a device-initiated MSI interrupt.
    IOTxnResult msiInterrupt(double deviceTimeNs, uint32_t vector);

    // Model a device-initiated MSI-X interrupt.
    IOTxnResult msixInterrupt(double deviceTimeNs, uint32_t vector);

    // Summary stats
    uint64_t totalCfgTransactions()  const { return cfgCount_; }
    uint64_t totalMMIOTransactions() const { return mmioCount_; }
    uint64_t totalDMATransactions()  const { return dmaCount_; }
    uint64_t totalMSITransactions()  const { return msiCount_; }
    double   totalCfgLatencyNs()     const { return totalCfgNs_; }
    double   totalMMIOLatencyNs()    const { return totalMMIONs_; }
    double   totalDMALatencyNs()     const { return totalDMANs_; }

private:
    CXLIOConfig cfg_;

    // Outstanding IO VC credits (time-aware sliding window)
    // (simplified: just track count, not full deque model)
    int    inflightIO_  = 0;

    // Stats
    uint64_t cfgCount_  = 0;
    uint64_t mmioCount_ = 0;
    uint64_t dmaCount_  = 0;
    uint64_t msiCount_  = 0;
    double   totalCfgNs_  = 0.0;
    double   totalMMIONs_ = 0.0;
    double   totalDMANs_  = 0.0;

    // Internal: compute credit stall and update inflight counter
    double computeCreditStall(int cap);
};

}  // namespace cxl
