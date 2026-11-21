#pragma once
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <string>

namespace cxl {

// ── NUMA Node description ─────────────────────────────────────────────────────

struct NUMANode {
    int      nodeId          = 0;
    bool     hasCXLDevice    = false;   // true if a CXL device is attached here
    int      cxlDeviceIdx    = -1;      // index into device array (-1 = no CXL)
    double   localDDR5BwGBps = 64.0;   // local DDR5 bandwidth (GB/s)
    double   localDDR5LatNs  = 114.0;  // local DDR5 unloaded latency (ns, SPR measured)
};

// ── NUMA interconnect topology ────────────────────────────────────────────────

struct NUMAConfig {
    int    numNodes              = 1;       // total NUMA nodes (1 = single socket)
    double upiOneWayLatencyNs   = 45.0;    // Intel UPI one-way latency (ns)
    double upiFullBwGBps        = 64.0;    // UPI bidirectional BW per direction (GB/s)
    // Per-node descriptions. If empty, assume single-node.
    std::vector<NUMANode> nodes;
};

// ── Page placement and migration config ───────────────────────────────────────

struct PageMigrationConfig {
    bool     enable               = false;
    uint32_t pageBytes            = 4096;    // page size (bytes)
    uint32_t hotAccessThreshold   = 10;      // accesses/window to classify as hot
    uint32_t coldAccessThreshold  = 2;       // accesses/window to classify as cold
    double   monitorWindowNs      = 10e6;    // monitoring window (10ms = 10e6 ns)
    double   migrationBwGBps      = 20.0;    // effective migration bandwidth (GB/s)
                                             // (limited by CXL BW + DRAM write BW)
    double   migrationOverheadNs  = 500.0;   // per-page overhead beyond transfer time
    uint32_t maxInflightMigrations= 4;       // max concurrent page migrations
};

// ── Access latency result considering NUMA ────────────────────────────────────

struct NUMALatencyResult {
    double baseLatencyNs   = 0.0;   // CXL device latency (from link model)
    double numaOffsetNs    = 0.0;   // UPI crossing penalty (0 if local)
    double totalLatencyNs  = 0.0;   // baseLatencyNs + numaOffsetNs
    bool   crossNUMA       = false; // true if access crossed a NUMA domain
    int    numUPIHops      = 0;     // number of UPI hops
};

// ── Per-page migration state ──────────────────────────────────────────────────

enum class PageTier : uint8_t { CXL_TIER2 = 0, LOCAL_DRAM = 1, MIGRATING = 2 };

struct PageState {
    uint64_t  pageAddr       = 0;
    PageTier  tier           = PageTier::CXL_TIER2;
    uint32_t  accessCount    = 0;
    double    windowStartNs  = 0.0;
    double    migrationDoneNs= 0.0;  // set when migration completes
};

// ── NUMA topology model ───────────────────────────────────────────────────────

class NUMAModel {
public:
    NUMAModel(const NUMAConfig& cfg, const PageMigrationConfig& migCfg);

    // Compute NUMA-adjusted latency for an access.
    // requesterNode: NUMA node of the requesting CPU (0-indexed)
    // targetDeviceIdx: CXL device index being accessed
    // baseLatencyNs: CXL link+device latency from the CXLMemController
    NUMALatencyResult adjustLatency(int requesterNode, int targetDeviceIdx,
                                    double baseLatencyNs) const;

    // Returns the number of UPI hops between requesterNode and the node
    // that hosts targetDeviceIdx. Returns 0 if same node.
    int numUPIHops(int requesterNode, int targetDeviceIdx) const;

    // ── Page migration tracking ───────────────────────────────────────────

    // Record an access to a page. Returns migration latency overhead (0 if no migration).
    // nowNs: current simulation time
    double recordAccess(uint64_t pageAddr, double nowNs);

    // Query current tier for a page
    PageTier getPageTier(uint64_t pageAddr) const;

    // Start migrating a hot page from CXL to local DRAM.
    // Returns the migration completion time (ns).
    double migratePage(uint64_t pageAddr, double nowNs);

    // Returns the effective latency for accessing a page, considering its tier.
    // If the page is in LOCAL_DRAM, returns localDramLatNs.
    // If in CXL, returns cxlLatNs (+ NUMA offset).
    // If MIGRATING, returns max(cxlLatNs, migrationDoneNs - nowNs) + cxlLatNs.
    double effectiveLatency(uint64_t pageAddr, double nowNs,
                            double cxlLatNs, double localDramLatNs) const;

    // Stats
    uint64_t totalMigrations()      const { return totalMigrations_; }
    uint64_t hotPageCount()         const;
    uint64_t coldPageCount()        const;
    double   avgMigrationLatencyNs()const;

    const NUMAConfig&          numaConfig()  const { return cfg_; }
    const PageMigrationConfig& migConfig()   const { return migCfg_; }

private:
    NUMAConfig          cfg_;
    PageMigrationConfig migCfg_;

    // Page state table: pageAlignedAddr → PageState
    mutable std::unordered_map<uint64_t, PageState> pages_;

    // Migration tracking
    uint64_t         totalMigrations_ = 0;
    double           totalMigrationNs_= 0.0;
    mutable uint32_t activeInflight_  = 0;

    uint64_t alignPage(uint64_t addr) const {
        return addr & ~(static_cast<uint64_t>(migCfg_.pageBytes) - 1);
    }
};

}  // namespace cxl
