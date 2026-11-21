#ifndef HYBRID_PAGE_MIGRATOR_H_
#define HYBRID_PAGE_MIGRATOR_H_

/*
 * HybridPageMigrator — a single configurable migrator class backing 13 new
 * page-placement methods.  Each named method (fsc, sys, sse, spfd, cema,
 * clcas, sbs, and their combinations) is a distinct preset of HybridConfig.
 *
 * See docs/migration_ideas.md for design rationale and per-method descriptions.
 */

#include <algorithm>
#include <cmath>
#include <mutex>
#include <sstream>
#include <vector>

#include "g_std/g_unordered_map.h"
#include "mtrand.h"
#include "page_migrator.h"

// ---------------------------------------------------------------------------
// Configuration struct — one instance per named method
// ---------------------------------------------------------------------------
struct HybridConfig {
    // --- Score composition ---
    // FSC: add (alpha * accessCount) to the base stall score
    bool useFrequency       = false;
    // SYS: multiply base stall score by (1 + robStallCycles/accessCount)
    bool useStallYield      = false;
    // SSE: DDR pages get an estimated "shadow stall" score; CXL pages use
    //      observed robStallCycles (or congestionWeightedStall with CLCAS)
    bool useShadowStall     = false;
    // CLCAS: accumulate stall_cycles² / cxlBaselineLatency per stall event
    //        (gives quadratic weight to high-latency / congested stalls)
    bool useCongestionWeight = false;

    // --- Temporal smoothing ---
    // CEMA: use per-page EMA values instead of raw per-interval counters
    bool   useEMA  = false;
    double lambda  = 0.3;    // EMA decay: 0 = pure history, 1 = current only

    // --- Stability ---
    // SBS: multiply score by (1 + gamma * consecutiveIntervals)
    bool     useStabilityBonus      = false;
    double   gamma                  = 0.05;
    uint64_t maxStabilityIntervals  = 10;

    // --- Demotion policy ---
    // SPFD: evict the DDR page with the lowest access count (instead of the
    //       page with the lowest stall score, which is arbitrary for DDR pages)
    bool asymmetricDemotion = false;

    // --- Stall metric selection ---
    // If true: use stall event COUNT instead of stall CYCLES everywhere in scoring.
    // ROB-stall count is more robust than cycles because it is not skewed by
    // occasional long CXL-queue stalls on otherwise-cold pages.
    bool useStallCount = false;

    // --- Stall Variance Awareness (SVA) ---
    // Penalises pages with high stall coefficient-of-variation (congestion victims
    // with bimodal CXL latency) and boosts pages with consistently high stall.
    bool   useStallVariance = false;
    double varianceBeta     = 1.0;  // suppression weight for CV

    // --- Global Knee Detection (GKD) ---
    // When globalStallYield exceeds kneeThreshold (link utilisation past the knee),
    // multiply the effective migration budget to pull utilisation back quickly.
    bool   useKneeDetection        = false;
    double kneeThreshold           = 0.5;   // globalStallYield above this = past knee
    double kneeMigrationMultiplier = 2.0;   // multiply budget when past knee

    // --- Self-Competition Avoidance (SCA) ---
    // When CXL is severely congested, migration reads compete with demand traffic.
    // Suppress migrations to avoid a self-defeating feedback loop.
    bool   useSelfCompetitionAware = false;
    double suppressionThreshold    = 0.8;   // globalStallYield above this = suppress
    double suppressionFactor       = 0.25;  // fraction of budget to retain when suppressing

    // --- Cross-Core Fan-out Weighting (CCF) ---
    // A page stalling N cores simultaneously causes N times the pipeline damage
    // AND head-of-line blocking in the CXL request queue.
    bool   useCoreWeighting = false;
    double coreBeta         = 1.0;  // fan-out weight per additional stalling core

    // --- Numeric parameters ---
    double alpha              = 0.001;   // FSC weight: 1 stall-cycle = alpha effective accesses
    double cxlBaselineLatency = 1000.0;  // baseline CXL latency for CLCAS / SSE normalisation

    // --- Name for logging ---
    const char* methodName = "hybrid";
};

// ---------------------------------------------------------------------------
// Helper: build the 13 named presets
// ---------------------------------------------------------------------------
namespace HybridPresets {

inline HybridConfig fsc() {
    HybridConfig c;
    c.useFrequency  = true;
    c.methodName    = "fsc";
    return c;
}
inline HybridConfig sys() {
    HybridConfig c;
    c.useStallYield = true;
    c.methodName    = "sys";
    return c;
}
inline HybridConfig sse() {
    HybridConfig c;
    c.useShadowStall = true;
    c.methodName     = "sse";
    return c;
}
inline HybridConfig spfd() {
    HybridConfig c;
    c.asymmetricDemotion = true;
    c.methodName         = "spfd";
    return c;
}
inline HybridConfig cema() {
    HybridConfig c;
    c.useEMA     = true;
    c.methodName = "cema";
    return c;
}
inline HybridConfig clcas() {
    HybridConfig c;
    c.useCongestionWeight = true;
    c.methodName          = "clcas";
    return c;
}
inline HybridConfig sbs() {
    HybridConfig c;
    c.useStabilityBonus = true;
    c.methodName        = "sbs";
    return c;
}
// --- Combinations ---
inline HybridConfig fsc_cema() {
    HybridConfig c;
    c.useFrequency = true;
    c.useEMA       = true;
    c.methodName   = "fsc+cema";
    return c;
}
inline HybridConfig fsc_sbs() {
    HybridConfig c;
    c.useFrequency      = true;
    c.useStabilityBonus = true;
    c.methodName        = "fsc+sbs";
    return c;
}
inline HybridConfig fsc_cema_sbs() {
    HybridConfig c;
    c.useFrequency      = true;
    c.useEMA            = true;
    c.useStabilityBonus = true;
    c.methodName        = "fsc+cema+sbs";
    return c;
}
inline HybridConfig sse_spfd() {
    HybridConfig c;
    c.useShadowStall     = true;
    c.asymmetricDemotion = true;
    c.methodName         = "sse+spfd";
    return c;
}
inline HybridConfig sse_spfd_clcas() {
    HybridConfig c;
    c.useShadowStall      = true;
    c.asymmetricDemotion  = true;
    c.useCongestionWeight = true;
    c.methodName          = "sse+spfd+clcas";
    return c;
}
inline HybridConfig sys_cema() {
    HybridConfig c;
    c.useStallYield = true;
    c.useEMA        = true;
    c.methodName    = "sys+cema";
    return c;
}

// ---------------------------------------------------------------------------
// Count-based variants — use stall event COUNT instead of stall CYCLES.
// These avoid the "one long CXL-queue stall poisons the ranking" failure
// that hurts robStallCycles on resnet50 and similar workloads.
// ---------------------------------------------------------------------------

inline HybridConfig fsc_cnt() {
    HybridConfig c;
    c.useFrequency  = true;
    c.useStallCount = true;
    c.methodName    = "fsc_cnt";
    return c;
}
inline HybridConfig fsc_cnt_sbs() {
    HybridConfig c;
    c.useFrequency      = true;
    c.useStallCount     = true;
    c.useStabilityBonus = true;
    c.methodName        = "fsc_cnt+sbs";
    return c;
}
inline HybridConfig fsc_cnt_cema_sbs() {
    HybridConfig c;
    c.useFrequency      = true;
    c.useStallCount     = true;
    c.useEMA            = true;
    c.useStabilityBonus = true;
    c.methodName        = "fsc_cnt+cema+sbs";
    return c;
}
inline HybridConfig sse_spfd_cnt() {
    // SSE+SPFD with count-based shadow stall estimation:
    //   CXL page score  = stall event count
    //   DDR page score  = accessCount × (CXL event rate)  [shadow estimate]
    //   demotion victim = DDR page with lowest access count (SPFD)
    HybridConfig c;
    c.useShadowStall     = true;
    c.asymmetricDemotion = true;
    c.useStallCount      = true;
    c.methodName         = "sse+spfd+cnt";
    return c;
}

// ---------------------------------------------------------------------------
// New CXL-aware ideas — standalone and combined with FSC-Count
// ---------------------------------------------------------------------------

// --- Standalone idea presets ---
inline HybridConfig sva() {
    HybridConfig c;
    c.useStallVariance = true;
    c.methodName       = "sva";
    return c;
}
inline HybridConfig gkd() {
    HybridConfig c;
    c.useKneeDetection = true;
    c.methodName       = "gkd";
    return c;
}
inline HybridConfig sca() {
    HybridConfig c;
    c.useSelfCompetitionAware = true;
    c.methodName              = "sca";
    return c;
}
inline HybridConfig ccf() {
    HybridConfig c;
    c.useCoreWeighting = true;
    c.methodName       = "ccf";
    return c;
}

// --- FSC-Count + new ideas ---
inline HybridConfig fsc_cnt_sva() {
    HybridConfig c;
    c.useFrequency     = true;
    c.useStallCount    = true;
    c.useStallVariance = true;
    c.methodName       = "fsc_cnt+sva";
    return c;
}
inline HybridConfig fsc_cnt_ccf() {
    HybridConfig c;
    c.useFrequency     = true;
    c.useStallCount    = true;
    c.useCoreWeighting = true;
    c.methodName       = "fsc_cnt+ccf";
    return c;
}
inline HybridConfig fsc_cnt_sva_ccf() {
    HybridConfig c;
    c.useFrequency     = true;
    c.useStallCount    = true;
    c.useStallVariance = true;
    c.useCoreWeighting = true;
    c.methodName       = "fsc_cnt+sva+ccf";
    return c;
}
inline HybridConfig fsc_cnt_sva_sbs() {
    HybridConfig c;
    c.useFrequency      = true;
    c.useStallCount     = true;
    c.useStallVariance  = true;
    c.useStabilityBonus = true;
    c.methodName        = "fsc_cnt+sva+sbs";
    return c;
}
inline HybridConfig fsc_cnt_sva_ccf_sbs() {
    HybridConfig c;
    c.useFrequency      = true;
    c.useStallCount     = true;
    c.useStallVariance  = true;
    c.useCoreWeighting  = true;
    c.useStabilityBonus = true;
    c.methodName        = "fsc_cnt+sva+ccf+sbs";
    return c;
}
inline HybridConfig fsc_cnt_gkd_sca() {
    HybridConfig c;
    c.useFrequency            = true;
    c.useStallCount           = true;
    c.useKneeDetection        = true;
    c.useSelfCompetitionAware = true;
    c.methodName              = "fsc_cnt+gkd+sca";
    return c;
}
inline HybridConfig fsc_cnt_sva_ccf_gkd_sca_sbs() {
    HybridConfig c;
    c.useFrequency            = true;
    c.useStallCount           = true;
    c.useStallVariance        = true;
    c.useCoreWeighting        = true;
    c.useKneeDetection        = true;
    c.useSelfCompetitionAware = true;
    c.useStabilityBonus       = true;
    c.methodName              = "fsc_cnt+sva+ccf+gkd+sca+sbs";
    return c;
}

} // namespace HybridPresets

// ---------------------------------------------------------------------------
// HybridPageMigrator
// ---------------------------------------------------------------------------
class HybridPageMigrator : public PageMigrator {
public:
    HybridPageMigrator(const g_string& name,
                       PlacementPolicy pp,
                       uint64_t        numFastMemPages,
                       uint64_t        numIntervalAccesses,
                       HybridConfig    cfg)
        : PageMigrator(name, pp, numFastMemPages, numIntervalAccesses),
          cfg_(cfg) {
        info("Initialized HybridPageMigrator[%s]: numFastMemPages=%ld "
             "numIntervalAccesses=%ld alpha=%.4f lambda=%.2f gamma=%.2f",
             cfg_.methodName, numFastMemPages, numIntervalAccesses,
             cfg_.alpha, cfg_.lambda, cfg_.gamma);
    }

    ~HybridPageMigrator() override { delete rng_; }

    // ---- PageMigrator interface ----
    PageLocation access(uint64_t pageAddress) override;
    void         triggerMigration() override;
    void         registerRobHeadLoadStall(uint64_t pageAddress, uint64_t stall_cycles, uint32_t coreId = 0) override;

    // ---- Memory footprint queries ----
    uint64_t getTotalUniquePages() const override { return pageTracker_.size(); }
    uint64_t getFastMemPages() const override {
        uint64_t n = 0;
        for (const auto& kv : pageTracker_) n += (kv.second.currLoc == FAST_MEM);
        return n;
    }
    uint64_t getSlowMemPages() const override {
        uint64_t n = 0;
        for (const auto& kv : pageTracker_) n += (kv.second.currLoc == SLOW_MEM);
        return n;
    }

    // ---- Stats ----
    void initStats(AggregateStat* parentStat);

private:
    // -----------------------------------------------------------------------
    // Per-page metadata — carries fields for ALL method variants
    // -----------------------------------------------------------------------
    struct PageMetadata {
        // ---- Per-interval counters (reset each interval) ----
        uint64_t accessCount             = 0;
        uint64_t robStallCycles          = 0;
        uint64_t robStallEvents          = 0;   // count of stall events (for useStallCount mode)
        double   congestionWeightedStall = 0.0; // CLCAS: sum(cycles² / baseline)

        // ---- Lifetime accumulators ----
        uint64_t totalAccessCount    = 0;
        uint64_t totalRobStallCycles = 0;
        uint64_t totalRobStallEvents = 0;

        // ---- EMA state (persistent across intervals) ----
        double ema_stall    = 0.0; // CEMA — stores smoothed cycles (or events when useStallCount)
        double ema_stall_sq = 0.0; // SVA  — EMA of stall² (persistent, for variance computation)
        double ema_access   = 0.0; // CEMA + FSC+CEMA

        // ---- Stability state (persistent) ----
        uint64_t consecutiveIntervals = 0; // SBS

        // ---- CCF: per-interval bitmask of cores that stalled on this page ----
        uint64_t stallingCoreMask = 0;  // reset each interval

        // ---- Migration accounting ----
        uint64_t    numSwappedIn  = 0;
        uint64_t    numSwappedOut = 0;
        PageLocation currLoc      = SLOW_MEM;

        // Convenience constructors
        PageMetadata() = default;
        explicit PageMetadata(PageLocation loc) : currLoc(loc) {}

        void recordAccess() {
            accessCount++;
            totalAccessCount++;
        }
        void recordStall(uint64_t cycles, double cxlBaseline) {
            robStallCycles += cycles;
            totalRobStallCycles += cycles;
            robStallEvents++;
            totalRobStallEvents++;
            // CLCAS: quadratic-weighted contribution
            if (cxlBaseline > 0.0)
                congestionWeightedStall += (double)cycles * ((double)cycles / cxlBaseline);
        }
        void swapIn() {
            assert(currLoc == SLOW_MEM);
            currLoc = FAST_MEM;
            numSwappedIn++;
            consecutiveIntervals = 0;
        }
        void swapOut() {
            assert(currLoc == FAST_MEM);
            currLoc = SLOW_MEM;
            numSwappedOut++;
            consecutiveIntervals = 0;
        }
        void resetIntervalCounters() {
            accessCount             = 0;
            robStallCycles          = 0;
            robStallEvents          = 0;
            congestionWeightedStall = 0.0;
            stallingCoreMask        = 0;   // CCF: reset per interval
            // ema_stall, ema_stall_sq, ema_access are persistent — NOT reset here
        }
    };

    // -----------------------------------------------------------------------
    // Scoring helpers
    // -----------------------------------------------------------------------

    // Compute globalStallYield = total_stall / (total_CXL_access * baseline)
    // Used by SSE to estimate shadow stall for DDR pages.
    double computeGlobalStallYield() const;

    // Score for one page given current context
    double computeScore(const PageMetadata& meta, double globalStallYield) const;

    // Sort all pages descending by score; return addresses in that order
    std::vector<uint64_t> getPagesSorted(double globalStallYield) const;

    // Build the demotion-victim list (for SPFD: sorted by access count asc)
    std::vector<uint64_t> buildCanDemote(const std::vector<uint64_t>& sortedPages) const;

    // -----------------------------------------------------------------------
    // Members
    // -----------------------------------------------------------------------
    HybridConfig cfg_;
    g_unordered_map<uint64_t, PageMetadata> pageTracker_;
    uint64_t     numFastAllocs_ = 0;
    MTRand*      rng_           = new MTRand(0xABCDE563F);
    std::mutex   mutex_;

    // ---- Stats counters ----
    Counter pageTrackerAccesses_, pageTrackerHits_;
    Counter numPageMigrations_, numSwappedIn_, numSwappedOut_, numHotPagesStayedIn_;
    Counter statAvgEmaStall_;        // EMA-based methods
    Counter statAvgStabilityBonus_;  // SBS-based methods
    Counter statAvgShadowYield_;     // SSE-based methods (stored ×1000 fixed-point)
    Counter statTotalStallEvents_;   // count-based methods: cumulative stall events seen
    Counter statKneeTriggers_;       // GKD: intervals where knee was detected
    Counter statSuppressionTriggers_; // SCA: intervals where suppression was applied
};

#endif // HYBRID_PAGE_MIGRATOR_H_
