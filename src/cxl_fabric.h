#pragma once
#include <cstdint>
#include <vector>
#include <string>

namespace cxl {

// ── P2P Configuration ────────────────────────────────────────────────────────

struct P2PConfig {
    bool   enable             = false;
    int    numExtraHops       = 1;       // extra switch hops for D2D path
    double extraLinkLatencyNs = 30.0;   // one-way extra link latency per hop
    double extraSwitchLatNs   = 40.0;   // extra switch latency per hop
};

// ── GIM (Global Interleave Memory) Configuration ─────────────────────────────

struct GIMConfig {
    bool     enable          = false;
    int      numDevices      = 2;        // number of interleaved devices
    uint64_t interleaveGran  = 256;      // interleave granularity (bytes)
    double   arbitrationNs   = 0.0;      // per-request HDM arbitration overhead
    // Per-device link latency (if devices have different distances to host)
    // If empty, all devices assumed equal latency.
    std::vector<double> deviceLinkOffsetNs;  // extra latency per device (indexed 0..N-1)
};

// ── Address translation result ────────────────────────────────────────────────

struct GIMTranslation {
    int      deviceId        = 0;    // which physical device owns this address
    uint64_t localAddr       = 0;    // address within that device's local address space
    double   latencyOffsetNs = 0.0;  // additional latency offset for this device
};

// ── P2P latency model ─────────────────────────────────────────────────────────

class P2PModel {
public:
    explicit P2PModel(const P2PConfig& cfg) : cfg_(cfg) {}

    // Returns extra latency (ns) added by the P2P fabric path.
    // numHops: number of D2D switch hops (usually 1 for same-switch devices).
    double extraLatencyNs(int numHops = 1) const;

    // Returns effective bandwidth degradation factor [0,1] due to P2P fabric overhead.
    // For numHops=1: approximately 0.85-0.90 (fabric overhead ~10-15%).
    double bwDegradationFactor(int numHops = 1) const;

    bool enabled() const { return cfg_.enable; }
    const P2PConfig& config() const { return cfg_; }

private:
    P2PConfig cfg_;
};

// ── GIM model ────────────────────────────────────────────────────────────────

class GIMModel {
public:
    explicit GIMModel(const GIMConfig& cfg) : cfg_(cfg) {}

    // Translate a GIM (global) address to a device-local address.
    GIMTranslation translate(uint64_t gimAddr) const;

    // Returns the peak aggregate bandwidth (GB/s) = N × singleDeviceBW.
    double aggregateBwGBps(double singleDeviceBwGBps) const;

    // Returns per-device bandwidth (GB/s) for a given access pattern.
    // uniformFraction: fraction of accesses that are uniformly distributed [0,1].
    // When uniform=1.0, all devices share the load equally.
    double perDeviceBwGBps(double totalBwGBps, double uniformFraction = 1.0) const;

    // Estimate contention delay (ns) when multiple requests hit the same device.
    // inflightSameDevice: number of in-flight requests targeting the same device.
    // singleDeviceRttNs: baseline RTT to one device.
    double contentionDelayNs(int inflightSameDevice, double singleDeviceRttNs) const;

    bool enabled() const { return cfg_.enable; }
    int  numDevices() const { return cfg_.numDevices; }
    const GIMConfig& config() const { return cfg_; }

private:
    GIMConfig cfg_;
};

// ── CXLFabric — top-level fabric model combining P2P + GIM ───────────────────

struct FabricConfig {
    P2PConfig p2p;
    GIMConfig gim;
};

class CXLFabric {
public:
    explicit CXLFabric(const FabricConfig& cfg);

    // For a GIM address: translate and return device-local info.
    GIMTranslation translateGIM(uint64_t addr) const;

    // For a P2P request: return extra latency.
    double p2pExtraLatencyNs(int numHops = 1) const;

    // Aggregate bandwidth across all GIM devices.
    double gimAggregateBwGBps(double singleDeviceBwGBps) const;

    bool hasGIM() const { return cfg_.gim.enable; }
    bool hasP2P() const { return cfg_.p2p.enable; }

    const FabricConfig& config() const { return cfg_; }

private:
    FabricConfig cfg_;
    P2PModel     p2p_;
    GIMModel     gim_;
};

}  // namespace cxl
