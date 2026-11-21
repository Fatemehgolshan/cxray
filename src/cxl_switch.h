#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace cxl {

// ---------------------------------------------------------------------------
// CXL Switch — HBR (CXL 1.x/2.0) and PBR (CXL 3.0/3.1) modes.
//
// HBR: address-range-based forwarding (hierarchical bridge routing).
// PBR: port-based packet routing (port ID in FLIT header).
// ---------------------------------------------------------------------------

enum class RoutingMode { HBR, PBR };

struct SwitchConfig {
    std::string  name              = "Switch-0";
    RoutingMode  mode              = RoutingMode::HBR;
    double       latencyNs         = 50.0;  // per-hop switch base latency (cut-through + arb)
    int          numUsp            = 1;     // upstream ports (hosts)
    int          numDsp            = 1;     // downstream ports (devices)

    // Multi-hop fabric: CXL 3.x topologies may chain switches.
    // Total switch latency = latencyNs × numHops.
    int          numHops           = 1;

    // Queuing model: M/D/1 approximation per ingress port.
    // When queueCapacityFlits > 0, queueing delay is estimated based on
    // switch port utilisation relative to ingressBwGBps.
    // Set ingressBwGBps = 0 to inherit the link bandwidth automatically.
    int          queueCapacityFlits = 32;
    double       ingressBwGBps      = 0.0;
};

// HBR address range entry (maps [base, base+size) → downstream port)
struct HBREntry {
    uint64_t baseAddr;
    uint64_t sizeBytes;
    uint8_t  dspPort;
};

// PBR routing table entry (port ID → egress port ID)
struct PBREntry {
    uint8_t ingressPort;
    uint8_t egressPort;
};

class CXLSwitch {
public:
    explicit CXLSwitch(const SwitchConfig& cfg);

    // HBR: add an address range → port mapping
    void addHBREntry(uint64_t baseAddr, uint64_t sizeBytes, uint8_t dspPort);

    // PBR: add a port forwarding rule
    void addPBREntry(uint8_t ingressPort, uint8_t egressPort);

    // Route a request. Returns added latency (ns) including queuing.
    // ingressPort: source port index
    // vc: virtual channel (0=CXL.io, 1=CXL.mem, 2=CXL.cache)
    // arrivalNs: simulation time of arrival (0 = unordered/analytical mode)
    double route(uint64_t addr, uint8_t ingressPort, uint8_t& egressPortOut,
                 uint8_t vc = 1, double arrivalNs = 0.0) const;

    double latencyNs() const { return cfg_.latencyNs; }
    int    numHops()   const { return cfg_.numHops;   }
    const SwitchConfig& config() const { return cfg_; }

    // Legacy: minimum hop count for a single switch (kept for test compatibility)
    static constexpr int HOPS = 1;

    // Inject link bandwidth so the queuing model can compute service rate.
    void setLinkBwGBps(double bw) { linkBwGBps_ = bw; }

    // Inject the link FLIT size (bytes) so the queuing model uses the correct
    // service time rather than the hard-coded 256-byte default.
    void setFlitSizeBytes(int n) { flitSizeBytes_ = n; }

    // Queuing statistics accessors
    double   avgQueueDelayNs() const {
        return totalRouted_ ? totalQueueNs_ / totalRouted_ : 0.0;
    }
    uint64_t queuedCount()     const { return queuedCount_; }
    uint64_t totalRouted()     const { return totalRouted_; }

private:
    SwitchConfig cfg_;
    std::vector<HBREntry>  hbrTable_;
    std::vector<PBREntry>  pbrTable_;
    double linkBwGBps_   = 54.4;
    int    flitSizeBytes_ = 256;   // set via setFlitSizeBytes() from link config

    // ── Per-port ingress queue state ──────────────────────────────────────
    // Tracks when each ingress port's current head-of-queue service finishes.
    // New arrivals must wait until max(arrivalNs, portBusyUntilNs_).
    // This is a correct M/D/1 model: deterministic service time, random arrivals.
    mutable std::unordered_map<uint8_t, double> portBusyUntilNs_;

    // Per-VC queue busy-until (indexed by VC: 0=CXL.io, 1=CXL.mem, 2=CXL.cache)
    // Allows VCs to drain independently (HOL blocking prevention).
    mutable std::unordered_map<uint8_t, std::array<double, 3>> vcPortBusyUntilNs_;

    // Queuing stats
    mutable uint64_t totalRouted_   = 0;
    mutable uint64_t queuedCount_   = 0;   // requests that experienced queuing
    mutable double   totalQueueNs_  = 0.0; // accumulated queueing delay
};

}  // namespace cxl
