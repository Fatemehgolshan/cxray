#include "cxl_fabric.h"
#include <algorithm>
#include <cmath>

namespace cxl {

// ── P2PModel ──────────────────────────────────────────────────────────────────

double P2PModel::extraLatencyNs(int numHops) const {
    if (!cfg_.enable || numHops <= 0) return 0.0;
    // ×2 for round-trip on the extra link segment, plus switch latency per hop
    return numHops * (cfg_.extraLinkLatencyNs * 2.0 + cfg_.extraSwitchLatNs);
}

double P2PModel::bwDegradationFactor(int numHops) const {
    if (!cfg_.enable || numHops <= 0) return 1.0;
    // Each extra hop adds protocol overhead: headers, ACKs, credit accounting.
    // Empirically: 1 hop → ~12% overhead; 2 hops → ~22% overhead.
    return std::max(0.5, 1.0 - numHops * 0.12);
}

// ── GIMModel ──────────────────────────────────────────────────────────────────

GIMTranslation GIMModel::translate(uint64_t gimAddr) const {
    uint64_t blockIdx   = gimAddr / cfg_.interleaveGran;
    uint64_t deviceId   = blockIdx % static_cast<uint64_t>(cfg_.numDevices);
    uint64_t blockInDev = blockIdx / static_cast<uint64_t>(cfg_.numDevices);
    uint64_t localAddr  = blockInDev * cfg_.interleaveGran + (gimAddr % cfg_.interleaveGran);

    double offset = 0.0;
    if (!cfg_.deviceLinkOffsetNs.empty() &&
        deviceId < cfg_.deviceLinkOffsetNs.size()) {
        offset = cfg_.deviceLinkOffsetNs[deviceId];
    }

    return { static_cast<int>(deviceId), localAddr, offset };
}

double GIMModel::aggregateBwGBps(double singleDeviceBwGBps) const {
    if (!cfg_.enable || cfg_.numDevices <= 0) return singleDeviceBwGBps;
    return cfg_.numDevices * singleDeviceBwGBps;
}

double GIMModel::perDeviceBwGBps(double totalBwGBps, double uniformFraction) const {
    if (!cfg_.enable || cfg_.numDevices <= 0) return totalBwGBps;
    // Uniform fraction of traffic is spread evenly; remainder hits one device.
    // uniform portion per device: (totalBw * uniformFraction) / N
    // non-uniform portion stays on one device: totalBw * (1 - uniformFraction)
    double uniformLoad    = (totalBwGBps * uniformFraction) / cfg_.numDevices;
    double nonUniformLoad = totalBwGBps * (1.0 - uniformFraction);
    return uniformLoad + nonUniformLoad;
}

double GIMModel::contentionDelayNs(int inflightSameDevice,
                                    double singleDeviceRttNs) const {
    // M/D/1 queuing approximation: if N requests simultaneously hit the same
    // device, they must be served serially.
    // Extra wait = (N-1) * service_time.
    // Service time ≈ singleDeviceRttNs / numDevices
    // (device is only contacted 1/N of the time in a balanced GIM pool).
    if (inflightSameDevice <= 1) return 0.0;
    double serviceTime = singleDeviceRttNs /
                         static_cast<double>(cfg_.numDevices > 0 ? cfg_.numDevices : 1);
    return (inflightSameDevice - 1) * serviceTime;
}

// ── CXLFabric ─────────────────────────────────────────────────────────────────

CXLFabric::CXLFabric(const FabricConfig& cfg)
    : cfg_(cfg), p2p_(cfg.p2p), gim_(cfg.gim) {}

GIMTranslation CXLFabric::translateGIM(uint64_t addr) const {
    return gim_.translate(addr);
}

double CXLFabric::p2pExtraLatencyNs(int numHops) const {
    return p2p_.extraLatencyNs(numHops);
}

double CXLFabric::gimAggregateBwGBps(double singleDeviceBwGBps) const {
    return gim_.aggregateBwGBps(singleDeviceBwGBps);
}

}  // namespace cxl
