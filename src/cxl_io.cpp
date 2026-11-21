#include "cxl_io.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace cxl {

// ---------------------------------------------------------------------------
// CXLIOController
// ---------------------------------------------------------------------------

CXLIOController::CXLIOController(const CXLIOConfig& cfg)
    : cfg_(cfg)
{
}

// ---------------------------------------------------------------------------
// Internal helper: estimate credit back-pressure stall.
//
// Simplified model: if inflightIO_ >= cap, a stall proportional to the
// link round-trip time divided by the credit window is injected.
// After stalling, the inflight counter resets to zero (batch completes).
// ---------------------------------------------------------------------------
double CXLIOController::computeCreditStall(int cap) {
    if (cap <= 0) return 0.0;
    if (inflightIO_ < cap) {
        ++inflightIO_;
        return 0.0;
    }
    // Credit window exhausted — stall one round-trip worth of backpressure.
    // Round-trip = 2 × linkOneWayLatencyNs.
    double stallNs = 2.0 * cfg_.linkOneWayLatencyNs / static_cast<double>(cap);
    inflightIO_ = 0;
    return stallNs;
}

// ---------------------------------------------------------------------------
// cfgRead
//
// Path: host TX → link → device config register lookup → link → host RX
// Latency = 2 × linkOneWayLatencyNs + cfgRegisterLookupNs + 2 × cfgTlpOverheadNs
// ---------------------------------------------------------------------------
IOTxnResult CXLIOController::cfgRead(double /*arrivalNs*/,
                                     uint16_t /*busDevFunc*/,
                                     uint16_t /*regOffset*/) {
    double stallNs  = computeCreditStall(cfg_.ioVCCredits);
    double latency  = 2.0 * cfg_.linkOneWayLatencyNs
                    + cfg_.cfgRegisterLookupNs
                    + 2.0 * cfg_.cfgTlpOverheadNs;

    ++cfgCount_;
    totalCfgNs_ += latency;

    return IOTxnResult{
        IOTxnType::CFG_RD,
        latency,
        stallNs,
        /*payloadBytes=*/ 4u,
        /*isDeviceToHost=*/ false
    };
}

// ---------------------------------------------------------------------------
// cfgWrite
//
// Path: host TX → link → device config register write
// No data returned — completion status arrives ~simultaneously on return path.
// Latency = linkOneWayLatencyNs + cfgTlpOverheadNs
// ---------------------------------------------------------------------------
IOTxnResult CXLIOController::cfgWrite(double /*arrivalNs*/,
                                      uint16_t /*busDevFunc*/,
                                      uint16_t /*regOffset*/) {
    double stallNs  = computeCreditStall(cfg_.ioVCCredits);
    double latency  = cfg_.linkOneWayLatencyNs
                    + cfg_.cfgTlpOverheadNs;

    ++cfgCount_;
    totalCfgNs_ += latency;

    return IOTxnResult{
        IOTxnType::CFG_WR,
        latency,
        stallNs,
        /*payloadBytes=*/ 4u,
        /*isDeviceToHost=*/ false
    };
}

// ---------------------------------------------------------------------------
// mmioRead
//
// Path: host TX → link → device MMIO handler → link → host RX (CplD)
// Latency = 2 × linkOneWayLatencyNs + mmioHandlerNs + 2 × mmioTlpOverheadNs
// ---------------------------------------------------------------------------
IOTxnResult CXLIOController::mmioRead(double /*arrivalNs*/,
                                      uint64_t /*barAddr*/,
                                      uint32_t sizeBytes) {
    uint32_t clampedSize = std::min(sizeBytes, 256u);

    double stallNs  = computeCreditStall(cfg_.ioVCCredits);
    double latency  = 2.0 * cfg_.linkOneWayLatencyNs
                    + cfg_.mmioHandlerNs
                    + 2.0 * cfg_.mmioTlpOverheadNs;

    ++mmioCount_;
    totalMMIONs_ += latency;

    return IOTxnResult{
        IOTxnType::MMIO_RD,
        latency,
        stallNs,
        clampedSize,
        /*isDeviceToHost=*/ false
    };
}

// ---------------------------------------------------------------------------
// mmioWrite
//
// Path: host TX → link → device MMIO handler (posted write, no data return)
// Latency = linkOneWayLatencyNs + mmioTlpOverheadNs
// ---------------------------------------------------------------------------
IOTxnResult CXLIOController::mmioWrite(double /*arrivalNs*/,
                                       uint64_t /*barAddr*/,
                                       uint32_t sizeBytes) {
    uint32_t clampedSize = std::min(sizeBytes, 256u);

    double stallNs  = computeCreditStall(cfg_.ioVCCredits);
    double latency  = cfg_.linkOneWayLatencyNs
                    + cfg_.mmioTlpOverheadNs;

    ++mmioCount_;
    totalMMIONs_ += latency;

    return IOTxnResult{
        IOTxnType::MMIO_WR,
        latency,
        stallNs,
        clampedSize,
        /*isDeviceToHost=*/ false
    };
}

// ---------------------------------------------------------------------------
// Internal helper: DMA latency formula (shared by read and write).
//
// transferNs = chunkBytes / dmaLinkBwGBps
//   (dmaLinkBwGBps is in GB/s = bytes/ns, so division gives ns directly)
// latency = dmaDescriptorNs + transferNs × (1 + dmaTlpOverheadFrac)
// ---------------------------------------------------------------------------
static double computeDmaLatency(const CXLIOConfig& cfg, uint32_t chunkBytes) {
    // GB/s → bytes/ns: 1 GB/s = 1 byte/ns
    double transferNs = static_cast<double>(chunkBytes) / cfg.dmaLinkBwGBps;
    return cfg.dmaDescriptorNs + transferNs * (1.0 + cfg.dmaTlpOverheadFrac);
}

// ---------------------------------------------------------------------------
// dmaRead
//
// Host-initiated block read from device memory via DMA engine.
// ---------------------------------------------------------------------------
IOTxnResult CXLIOController::dmaRead(double /*arrivalNs*/,
                                     uint64_t /*srcAddr*/,
                                     uint64_t /*dstAddr*/,
                                     uint32_t chunkBytes) {
    double stallNs  = computeCreditStall(cfg_.ioVCCredits);
    double latency  = computeDmaLatency(cfg_, chunkBytes);

    ++dmaCount_;
    totalDMANs_ += latency;

    return IOTxnResult{
        IOTxnType::DMA_RD,
        latency,
        stallNs,
        chunkBytes,
        /*isDeviceToHost=*/ false
    };
}

// ---------------------------------------------------------------------------
// dmaWrite
//
// Host-initiated block write to device memory via DMA engine.
// ---------------------------------------------------------------------------
IOTxnResult CXLIOController::dmaWrite(double /*arrivalNs*/,
                                      uint64_t /*srcAddr*/,
                                      uint64_t /*dstAddr*/,
                                      uint32_t chunkBytes) {
    double stallNs  = computeCreditStall(cfg_.ioVCCredits);
    double latency  = computeDmaLatency(cfg_, chunkBytes);

    ++dmaCount_;
    totalDMANs_ += latency;

    return IOTxnResult{
        IOTxnType::DMA_WR,
        latency,
        stallNs,
        chunkBytes,
        /*isDeviceToHost=*/ false
    };
}

// ---------------------------------------------------------------------------
// Internal helper: MSI/MSI-X latency formula (shared by both variants).
//
// Path: device → link → host IOMMU lookup → interrupt controller → CPU
// Latency = linkOneWayLatencyNs + msiIommuLookupNs + msiInterruptDeliveryNs
// ---------------------------------------------------------------------------
static double computeMsiLatency(const CXLIOConfig& cfg) {
    return cfg.linkOneWayLatencyNs
         + cfg.msiIommuLookupNs
         + cfg.msiInterruptDeliveryNs;
}

// ---------------------------------------------------------------------------
// msiInterrupt
//
// Device sends a small MWr TLP to the host interrupt controller.
// ---------------------------------------------------------------------------
IOTxnResult CXLIOController::msiInterrupt(double /*deviceTimeNs*/,
                                          uint32_t /*vector*/) {
    double stallNs = computeCreditStall(cfg_.ioVCCredits);
    double latency = computeMsiLatency(cfg_);

    ++msiCount_;

    return IOTxnResult{
        IOTxnType::MSI,
        latency,
        stallNs,
        /*payloadBytes=*/ 8u,
        /*isDeviceToHost=*/ true
    };
}

// ---------------------------------------------------------------------------
// msixInterrupt
//
// MSI-X uses the same physical path as MSI; latency model is identical.
// ---------------------------------------------------------------------------
IOTxnResult CXLIOController::msixInterrupt(double /*deviceTimeNs*/,
                                           uint32_t /*vector*/) {
    double stallNs = computeCreditStall(cfg_.ioVCCredits);
    double latency = computeMsiLatency(cfg_);

    ++msiCount_;

    return IOTxnResult{
        IOTxnType::MSIX,
        latency,
        stallNs,
        /*payloadBytes=*/ 8u,
        /*isDeviceToHost=*/ true
    };
}

}  // namespace cxl
