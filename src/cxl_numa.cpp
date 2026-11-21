// ---------------------------------------------------------------------------
// cxl_numa.cpp — NUMA topology effects and hot/cold page migration modeling.
//
// Models:
//   - Multi-socket NUMA latency penalties (Intel UPI crossings)
//   - Hot/cold page classification via per-page access frequency counters
//   - Page migration engine (CXL tier 2 → local DRAM tier 1)
// ---------------------------------------------------------------------------

#include "cxl_numa.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace cxl {

// ============================================================================
// NUMAModel — constructor
// ============================================================================

NUMAModel::NUMAModel(const NUMAConfig& cfg, const PageMigrationConfig& migCfg)
    : cfg_(cfg), migCfg_(migCfg)
{
    // Validate page size: must be a power of two and at least 64 bytes.
    if (migCfg_.pageBytes == 0 || (migCfg_.pageBytes & (migCfg_.pageBytes - 1)) != 0) {
        throw std::invalid_argument("PageMigrationConfig::pageBytes must be a power of two");
    }
}

// ============================================================================
// numUPIHops
//
// Topology rules:
//   - Single-node (cfg_.nodes empty or numNodes == 1): always 0 hops.
//   - 2-socket: if requesterNode hosts targetDeviceIdx → 0; else → 1.
//   - N-socket (>2): use Manhattan distance on a flat node-id axis as a
//     conservative proxy (real systems may differ, but this is correct for
//     a 1-D ring/bus topology like a dual-socket UPI mesh).
// ============================================================================

int NUMAModel::numUPIHops(int requesterNode, int targetDeviceIdx) const {
    // Single-node: no UPI exists.
    if (cfg_.numNodes <= 1 || cfg_.nodes.empty()) {
        return 0;
    }

    // Find which NUMA node hosts the target CXL device.
    int deviceNode = -1;
    for (const NUMANode& n : cfg_.nodes) {
        if (n.hasCXLDevice && n.cxlDeviceIdx == targetDeviceIdx) {
            deviceNode = n.nodeId;
            break;
        }
    }

    // Device not found in node table — assume local (conservative: no penalty).
    if (deviceNode == -1) {
        return 0;
    }

    if (requesterNode == deviceNode) {
        return 0;
    }

    // For 2-socket systems this is always 1 hop.
    // For larger systems use Manhattan distance along the node-id axis.
    return std::abs(requesterNode - deviceNode);
}

// ============================================================================
// adjustLatency
// ============================================================================

NUMALatencyResult NUMAModel::adjustLatency(int requesterNode,
                                           int targetDeviceIdx,
                                           double baseLatencyNs) const {
    int    hops       = numUPIHops(requesterNode, targetDeviceIdx);
    // Round-trip UPI penalty: hops × one-way latency × 2 (request + response).
    double upiPenalty = static_cast<double>(hops) * cfg_.upiOneWayLatencyNs * 2.0;

    NUMALatencyResult result;
    result.baseLatencyNs  = baseLatencyNs;
    result.numaOffsetNs   = upiPenalty;
    result.totalLatencyNs = baseLatencyNs + upiPenalty;
    result.crossNUMA      = (hops > 0);
    result.numUPIHops     = hops;
    return result;
}

// ============================================================================
// recordAccess
//
// Called on every memory access to a page-aligned address.  Maintains a
// sliding-window access counter.  When the hot threshold is crossed and
// migration is enabled, triggers migratePage() and returns the migration
// overhead so the caller can add it to the request latency.
// ============================================================================

double NUMAModel::recordAccess(uint64_t pageAddr, double nowNs) {
    uint64_t aligned = alignPage(pageAddr);

    // Get or create page state.
    auto it = pages_.find(aligned);
    if (it == pages_.end()) {
        PageState ps;
        ps.pageAddr      = aligned;
        ps.tier          = PageTier::CXL_TIER2;
        ps.accessCount   = 0;
        ps.windowStartNs = nowNs;
        pages_[aligned]  = ps;
        it = pages_.find(aligned);
    }

    PageState& page = it->second;

    // Reset counter if the monitoring window has elapsed.
    if ((nowNs - page.windowStartNs) > migCfg_.monitorWindowNs) {
        page.accessCount   = 0;
        page.windowStartNs = nowNs;
    }

    page.accessCount++;

    // Check if migration should be triggered.
    if (migCfg_.enable
        && page.tier == PageTier::CXL_TIER2
        && page.accessCount >= migCfg_.hotAccessThreshold
        && activeInflight_ < migCfg_.maxInflightMigrations)
    {
        return migratePage(aligned, nowNs);
    }

    return 0.0;
}

// ============================================================================
// migratePage
//
// Initiates a page migration from CXL tier 2 to local DRAM tier 1.
// The migration cost is:
//   transferNs = pageBytes / migrationBwGBps  (GB/s → ns conversion)
//   totalCost  = transferNs + migrationOverheadNs
//
// GB/s → ns for pageBytes bytes:
//   transferNs = (pageBytes / (bwGBps × 1e9)) × 1e9
//              = pageBytes / bwGBps
// (pageBytes is in bytes, bwGBps is in bytes/ns when scaled by 1/1e9 × 1e9)
// ============================================================================

double NUMAModel::migratePage(uint64_t pageAddr, double nowNs) {
    uint64_t aligned = alignPage(pageAddr);

    auto it = pages_.find(aligned);
    if (it == pages_.end()) {
        // Page not yet tracked; create it.
        PageState ps;
        ps.pageAddr      = aligned;
        ps.tier          = PageTier::CXL_TIER2;
        ps.accessCount   = 0;
        ps.windowStartNs = nowNs;
        pages_[aligned]  = ps;
        it = pages_.find(aligned);
    }

    PageState& page = it->second;

    // Only migrate pages that are currently on CXL tier 2.
    if (page.tier != PageTier::CXL_TIER2) {
        return 0.0;
    }

    // Transfer time: pageBytes / (migrationBwGBps GB/s).
    // 1 GB/s = 1 byte/ns, so transferNs = pageBytes / migrationBwGBps.
    double transferNs = static_cast<double>(migCfg_.pageBytes)
                        / migCfg_.migrationBwGBps;
    double totalCostNs = transferNs + migCfg_.migrationOverheadNs;

    page.tier             = PageTier::MIGRATING;
    page.migrationDoneNs  = nowNs + totalCostNs;

    activeInflight_++;
    totalMigrations_++;
    totalMigrationNs_ += totalCostNs;

    return totalCostNs;
}

// ============================================================================
// getPageTier
// ============================================================================

PageTier NUMAModel::getPageTier(uint64_t pageAddr) const {
    uint64_t aligned = alignPage(pageAddr);
    auto it = pages_.find(aligned);
    if (it == pages_.end()) {
        return PageTier::CXL_TIER2;  // unknown pages default to CXL tier 2
    }
    return it->second.tier;
}

// ============================================================================
// effectiveLatency
//
// Returns the latency for accessing a page given the current simulation time
// and both the CXL and local DRAM latencies.  Side-effects: completes any
// pending migration whose deadline has passed.
// ============================================================================

double NUMAModel::effectiveLatency(uint64_t pageAddr, double nowNs,
                                   double cxlLatNs, double localDramLatNs) const {
    uint64_t aligned = alignPage(pageAddr);

    auto it = pages_.find(aligned);
    if (it == pages_.end()) {
        // Untracked page — treat as CXL tier 2.
        return cxlLatNs;
    }

    PageState& page = it->second;  // mutable via mutable pages_

    switch (page.tier) {
        case PageTier::LOCAL_DRAM:
            return localDramLatNs;

        case PageTier::MIGRATING:
            if (nowNs >= page.migrationDoneNs) {
                // Migration complete: transition to local DRAM.
                page.tier = PageTier::LOCAL_DRAM;
                if (activeInflight_ > 0) {
                    activeInflight_--;
                }
                return localDramLatNs;
            } else {
                // Migration still in flight: access stalls on CXL.
                return cxlLatNs;
            }

        case PageTier::CXL_TIER2:
        default:
            return cxlLatNs;
    }
}

// ============================================================================
// Stats helpers
// ============================================================================

uint64_t NUMAModel::hotPageCount() const {
    uint64_t count = 0;
    for (const auto& kv : pages_) {
        if (kv.second.accessCount >= migCfg_.hotAccessThreshold) {
            count++;
        }
    }
    return count;
}

uint64_t NUMAModel::coldPageCount() const {
    uint64_t count = 0;
    for (const auto& kv : pages_) {
        if (kv.second.accessCount <= migCfg_.coldAccessThreshold) {
            count++;
        }
    }
    return count;
}

double NUMAModel::avgMigrationLatencyNs() const {
    if (totalMigrations_ == 0) return 0.0;
    return totalMigrationNs_ / static_cast<double>(totalMigrations_);
}

}  // namespace cxl
