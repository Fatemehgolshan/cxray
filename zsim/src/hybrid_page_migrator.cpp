#include "hybrid_page_migrator.h"
#include <algorithm>
#include <functional>

// ---------------------------------------------------------------------------
// initStats
// ---------------------------------------------------------------------------
void HybridPageMigrator::initStats(AggregateStat* parentStat) {
    AggregateStat* s = new AggregateStat();
    s->init(name.c_str(), "HybridPageMigrator stats");

    pageTrackerAccesses_.init("pageTrackerAccesses", "Page-tracker lookups");
    s->append(&pageTrackerAccesses_);
    pageTrackerHits_.init("pageTrackerHits", "Page-tracker hits");
    s->append(&pageTrackerHits_);
    numPageMigrations_.init("numPageMigrations", "Total pages migrated (either dir)");
    s->append(&numPageMigrations_);
    numSwappedIn_.init("numSwappedIn", "Pages promoted CXL→DDR");
    s->append(&numSwappedIn_);
    numSwappedOut_.init("numSwappedOut", "Pages demoted DDR→CXL");
    s->append(&numSwappedOut_);
    numHotPagesStayedIn_.init("numHotPagesStayedIn", "Hot pages that stayed in DDR");
    s->append(&numHotPagesStayedIn_);
    statAvgEmaStall_.init("avgEmaStall", "Avg EMA stall score (×1 fixed-point)");
    s->append(&statAvgEmaStall_);
    statAvgStabilityBonus_.init("avgStabilityBonus", "Avg stability multiplier (×1000 fixed-point)");
    s->append(&statAvgStabilityBonus_);
    statAvgShadowYield_.init("avgShadowStallYield", "Global stall yield used by SSE (×1000 fixed-point)");
    s->append(&statAvgShadowYield_);
    statTotalStallEvents_.init("totalStallEvents", "Cumulative ROB-stall events seen by this migrator");
    s->append(&statTotalStallEvents_);
    statKneeTriggers_.init("kneeTriggers", "Intervals where GKD detected link past knee");
    s->append(&statKneeTriggers_);
    statSuppressionTriggers_.init("suppressionTriggers", "Intervals where SCA applied migration suppression");
    s->append(&statSuppressionTriggers_);

    PageMigrator::initStats(s);
    parentStat->append(s);
}

// ---------------------------------------------------------------------------
// access
// ---------------------------------------------------------------------------
PageLocation HybridPageMigrator::access(uint64_t pageAddress) {
    std::lock_guard<std::mutex> lock(mutex_);
    pageTrackerAccesses_.inc();

    auto it = pageTracker_.find(pageAddress);
    PageLocation loc;

    if (it != pageTracker_.end()) {
        pageTrackerHits_.inc();
        it->second.recordAccess();
        loc = it->second.currLoc;
    } else {
        // Initial placement
        if (placementPolicy == RANDOM) {
            loc = rng_->randInt(1) ? FAST_MEM : SLOW_MEM;
        } else if (placementPolicy == FAST_FIRST) {
            loc = (numFastAllocs_ < numFastMemPages) ? FAST_MEM : SLOW_MEM;
        } else {
            loc = SLOW_MEM; // SLOW_FIRST
        }
        numFastAllocs_ += (loc == FAST_MEM);
        PageMetadata pm(loc);
        pm.accessCount      = 1;
        pm.totalAccessCount = 1;
        pageTracker_[pageAddress] = pm;
    }

    PageMigrator::registerAccess(loc);
    return loc;
}

// ---------------------------------------------------------------------------
// registerRobHeadLoadStall
// ---------------------------------------------------------------------------
void HybridPageMigrator::registerRobHeadLoadStall(uint64_t pageAddress, uint64_t stall_cycles, uint32_t coreId) {
    if (stall_cycles == 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pageTracker_.find(pageAddress);
    if (it == pageTracker_.end()) return;
    it->second.recordStall(stall_cycles, cfg_.cxlBaselineLatency);
    // CCF: record which core contributed a stall on this page
    if (cfg_.useCoreWeighting) {
        it->second.stallingCoreMask |= (1ULL << (coreId & 63));
    }
    // Always increment the cumulative event counter for diagnostics
    statTotalStallEvents_.inc();
}

// ---------------------------------------------------------------------------
// computeGlobalStallYield (for SSE)
// ---------------------------------------------------------------------------
double HybridPageMigrator::computeGlobalStallYield() const {
    double totalStall  = 0.0;
    double totalAccess = 0.0;
    for (const auto& kv : pageTracker_) {
        const auto& m = kv.second;
        if (m.currLoc == SLOW_MEM && m.accessCount > 0) {
            if (cfg_.useEMA) {
                // EMA path: ema_stall holds smoothed cycles (or events when useStallCount)
                totalStall  += m.ema_stall;
                totalAccess += m.ema_access;
            } else if (cfg_.useStallCount) {
                totalStall  += (double)m.robStallEvents;
                totalAccess += (double)m.accessCount;
            } else {
                totalStall  += (double)m.robStallCycles;
                totalAccess += (double)m.accessCount;
            }
        }
    }
    if (cfg_.useStallCount && !cfg_.useEMA) {
        // Count mode: yield = stall events per access (dimensionless; no cxlBaseline factor).
        // Used by SSE to estimate DDR page score as: accessCount × eventRate.
        if (totalAccess <= 0.0) return 0.01;
        return totalStall / totalAccess;
    } else {
        // Cycle mode: yield = stall_cycles / (access × baseline)
        if (totalAccess <= 0.0 || cfg_.cxlBaselineLatency <= 0.0) return 0.01;
        return totalStall / (totalAccess * cfg_.cxlBaselineLatency);
    }
}

// ---------------------------------------------------------------------------
// computeScore — the heart of the scoring logic
// ---------------------------------------------------------------------------
double HybridPageMigrator::computeScore(const PageMetadata& meta,
                                         double globalStallYield) const {
    // Choose raw or EMA values.
    // When useStallCount=true, effStall holds stall event count instead of cycles.
    double effAccess = cfg_.useEMA ? meta.ema_access
                                   : (double)meta.accessCount;
    double effStall;
    if (cfg_.useEMA) {
        // ema_stall was updated from the appropriate raw signal (cycles or events)
        effStall = meta.ema_stall;
    } else if (cfg_.useStallCount) {
        effStall = (double)meta.robStallEvents;
    } else {
        effStall = (double)meta.robStallCycles;
    }
    // Congestion-weighted stall (CLCAS). In count mode, fall back to raw event count.
    double effCongStall;
    if (cfg_.useEMA) {
        effCongStall = meta.ema_stall;
    } else if (cfg_.useStallCount) {
        effCongStall = (double)meta.robStallEvents;
    } else {
        effCongStall = meta.congestionWeightedStall;
    }

    double score = 0.0;

    if (cfg_.useShadowStall) {
        // SSE: unified scoring — DDR pages get shadow-stall estimate
        if (meta.currLoc == SLOW_MEM) {
            // CXL page: use observed stall (or congestion-weighted)
            score = cfg_.useCongestionWeight ? effCongStall : effStall;
        } else {
            // DDR page: estimate what it would contribute if in CXL.
            // In count mode: score = accesses × (events per CXL access)  [no baseline factor].
            // In cycle mode: score = accesses × yield × baseline          [as before].
            if (cfg_.useStallCount) {
                score = effAccess * globalStallYield;
            } else {
                score = effAccess * globalStallYield * cfg_.cxlBaselineLatency;
            }
        }
    } else {
        // Base stall score
        if (cfg_.useCongestionWeight) {
            score = effCongStall;
        } else {
            score = effStall;
        }

        // SYS: multiply by stall yield factor
        if (cfg_.useStallYield && effAccess > 0.0) {
            double stallYield = effStall / effAccess;
            score *= (1.0 + stallYield);
        }

        // FSC: add frequency component
        if (cfg_.useFrequency) {
            score += cfg_.alpha * effAccess;
        }
    }

    // SBS: apply stability multiplier to all score types
    if (cfg_.useStabilityBonus && meta.consecutiveIntervals > 0) {
        double bonus = 1.0 + cfg_.gamma *
                       std::min((double)meta.consecutiveIntervals,
                                (double)cfg_.maxStabilityIntervals);
        score *= bonus;
    }

    // SVA: suppress pages with high stall variance (congestion victims).
    // Pages where stall latency is bimodal (short + very long) have high CV.
    // ema_stall and ema_stall_sq are maintained whenever useStallVariance is true.
    if (cfg_.useStallVariance && score > 0.0) {
        double e_x2 = meta.ema_stall_sq;
        double e_x  = meta.ema_stall;
        double variance = e_x2 - e_x * e_x;
        if (variance < 0.0) variance = 0.0;
        double cv = std::sqrt(variance) / (e_x + 1e-6);
        score /= (1.0 + cfg_.varianceBeta * cv);
    }

    // CCF: boost pages that stalled multiple cores simultaneously.
    // stallingCoreMask is a per-interval bitmask reset by resetIntervalCounters().
    if (cfg_.useCoreWeighting && meta.stallingCoreMask != 0) {
        int numCores = __builtin_popcountll(meta.stallingCoreMask);
        score *= (1.0 + cfg_.coreBeta * numCores);
    }

    return score;
}

// ---------------------------------------------------------------------------
// getPagesSorted
// ---------------------------------------------------------------------------
std::vector<uint64_t>
HybridPageMigrator::getPagesSorted(double globalStallYield) const {
    using CIter = decltype(pageTracker_)::const_iterator;
    std::vector<CIter> items;
    items.reserve(pageTracker_.size());
    for (auto it = pageTracker_.begin(); it != pageTracker_.end(); ++it)
        items.push_back(it);

    std::stable_sort(items.begin(), items.end(),
        [&](const CIter& a, const CIter& b) {
            double sa = computeScore(a->second, globalStallYield);
            double sb = computeScore(b->second, globalStallYield);
            if (sa != sb) return sa > sb;
            // Tie-breaker 1: prefer FAST_MEM (reduce unnecessary migrations)
            if (a->second.currLoc != b->second.currLoc)
                return a->second.currLoc == FAST_MEM;
            // Tie-breaker 2: deterministic
            return a->first < b->first;
        });

    std::vector<uint64_t> keys;
    keys.reserve(items.size());
    for (const auto& it : items) keys.push_back(it->first);
    return keys;
}

// ---------------------------------------------------------------------------
// buildCanDemote — list of DDR pages eligible for demotion
// For SPFD: ALL DDR pages, ordered so pop_back gives lowest access count.
// Default: DDR pages in "cold" positions of the stall-sorted order.
// ---------------------------------------------------------------------------
std::vector<uint64_t>
HybridPageMigrator::buildCanDemote(const std::vector<uint64_t>& sortedPages) const {
    if (cfg_.asymmetricDemotion) {
        // SPFD: all DDR pages, descending by access count so pop_back = lowest access
        using AP = std::pair<uint64_t, uint64_t>; // (addr, accessCount)
        std::vector<AP> ddrByAccess;
        ddrByAccess.reserve(numFastAllocs_);
        for (const auto& kv : pageTracker_) {
            if (kv.second.currLoc == FAST_MEM) {
                uint64_t ac = cfg_.useEMA
                    ? (uint64_t)kv.second.ema_access
                    : kv.second.accessCount;
                ddrByAccess.push_back({kv.first, ac});
            }
        }
        // Sort descending so pop_back gives the page with the LOWEST access count
        std::sort(ddrByAccess.begin(), ddrByAccess.end(),
                  [](const AP& a, const AP& b) { return a.second > b.second; });
        std::vector<uint64_t> result;
        result.reserve(ddrByAccess.size());
        for (const auto& ap : ddrByAccess) result.push_back(ap.first);
        return result;
    } else {
        // Default: DDR pages found in "cold" positions (index >= numFastMemPages)
        std::vector<uint64_t> result;
        if (sortedPages.size() > numFastMemPages) {
            result.reserve(sortedPages.size() - numFastMemPages);
            for (size_t j = numFastMemPages; j < sortedPages.size(); ++j) {
                auto it = pageTracker_.find(sortedPages[j]);
                if (it != pageTracker_.end() && it->second.currLoc == FAST_MEM)
                    result.push_back(sortedPages[j]);
            }
        }
        return result;
    }
}

// ---------------------------------------------------------------------------
// triggerMigration
// ---------------------------------------------------------------------------
void HybridPageMigrator::triggerMigration() {
    // Step 1: Update EMA values from the just-completed interval BEFORE sorting.
    // When useStallCount=true, smooth stall events; otherwise smooth stall cycles.
    // SVA also requires ema_stall and ema_stall_sq even without useEMA.
    if (cfg_.useEMA || cfg_.useStallVariance) {
        for (auto& kv : pageTracker_) {
            auto& m = kv.second;
            double rawStall = cfg_.useStallCount
                ? (double)m.robStallEvents
                : (double)m.robStallCycles;
            m.ema_stall = (1.0 - cfg_.lambda) * m.ema_stall + cfg_.lambda * rawStall;
            if (cfg_.useEMA) {
                m.ema_access = (1.0 - cfg_.lambda) * m.ema_access + cfg_.lambda * (double)m.accessCount;
            }
            if (cfg_.useStallVariance) {
                m.ema_stall_sq = (1.0 - cfg_.lambda) * m.ema_stall_sq + cfg_.lambda * rawStall * rawStall;
            }
        }
    }

    // Step 2: Compute global stall yield (used by SSE)
    const double globalStallYield = computeGlobalStallYield();

    info("[%s] triggerMigration: numFastAllocs=%ld / %ld, globalStallYield=%.4f",
         cfg_.methodName, numFastAllocs_, numFastMemPages, globalStallYield);

    // Step 2.5: GKD — past the knee → increase budget; SCA — severe congestion → suppress.
    // Also enforce the global migration page cap.
    {
        uint64_t effectiveBudget = promotionBudget;
        const uint64_t kUnlimited = std::numeric_limits<uint64_t>::max();

        if (cfg_.useKneeDetection && globalStallYield > cfg_.kneeThreshold) {
            if (effectiveBudget != kUnlimited) {
                effectiveBudget = (uint64_t)(effectiveBudget * cfg_.kneeMigrationMultiplier);
            }
            statKneeTriggers_.inc();
        }

        if (cfg_.useSelfCompetitionAware && globalStallYield > cfg_.suppressionThreshold) {
            // Cap unlimited budget to DRAM pages before scaling down
            if (effectiveBudget == kUnlimited) effectiveBudget = numFastMemPages;
            effectiveBudget = (uint64_t)(effectiveBudget * cfg_.suppressionFactor);
            statSuppressionTriggers_.inc();
        }

        // Apply global migration page cap
        effectiveBudget = std::min(effectiveBudget, migrationPageCap_);
        promotionBudget = effectiveBudget;
    }

    // Step 3: Sort all pages by score (descending)
    std::vector<uint64_t> sortedPages = getPagesSorted(globalStallYield);

    // Step 4: Build demotion-victim list
    std::vector<uint64_t> canDemote = buildCanDemote(sortedPages);

    // Step 5: Execute migrations
    const bool promote = !migrationTrigger || migrationTrigger->promoteDirection();

    uint64_t nStayedIn = 0, nMigrations = 0, nSwappedIn = 0, nSwappedOut = 0;
    double   sumStabilityBonus = 0.0;
    uint64_t countStabilityBonus = 0;

    for (size_t i = 0; i < sortedPages.size(); ++i) {
        const uint64_t addr = sortedPages[i];
        auto pageIt = pageTracker_.find(addr);
        if (pageIt == pageTracker_.end()) continue;
        auto& meta = pageIt->second;

        if (i < numFastMemPages) {
            // --- Hot page (should be in DDR) ---
            if (meta.currLoc == FAST_MEM) {
                nStayedIn++;
            } else {
                // Promote from CXL → DDR
                if (promote && consumePromotionBudget()) {
                    // Need to make room first if DDR is full
                    if (numFastAllocs_ >= numFastMemPages) {
                        if (!canDemote.empty()) {
                            const uint64_t victimAddr = canDemote.back();
                            canDemote.pop_back();
                            auto vit = pageTracker_.find(victimAddr);
                            if (vit != pageTracker_.end() &&
                                vit->second.currLoc == FAST_MEM) {
                                vit->second.swapOut();
                                nMigrations++;
                                nSwappedOut++;
                                numFastAllocs_--;
                            }
                        }
                    }
                    if (meta.currLoc == SLOW_MEM && numFastAllocs_ < numFastMemPages) {
                        meta.swapIn();
                        nMigrations++;
                        nSwappedIn++;
                        numFastAllocs_++;
                    }
                }
            }
        } else {
            // --- Cold page (should be in CXL) ---
            if (meta.currLoc == FAST_MEM) {
                if (promote) {
                    meta.swapOut();
                    nMigrations++;
                    nSwappedOut++;
                    numFastAllocs_--;
                } else if (consumePromotionBudget()) {
                    meta.swapOut();
                    nMigrations++;
                    nSwappedOut++;
                    numFastAllocs_--;
                }
            }
        }

        // Increment stability counter for pages that stayed in their location
        // (swapIn/swapOut already reset consecutiveIntervals to 0)
        meta.consecutiveIntervals++;

        // Track stats
        if (cfg_.useStabilityBonus) {
            double bonus = 1.0 + cfg_.gamma *
                           std::min((double)meta.consecutiveIntervals,
                                    (double)cfg_.maxStabilityIntervals);
            sumStabilityBonus += bonus;
            countStabilityBonus++;
        }

        // Reset per-interval counters (EMA values are preserved)
        meta.resetIntervalCounters();
    }

    // Step 6: Record migration count for overhead modeling in TieringMemory
    this->lastIntervalMigrations_ = nSwappedIn + nSwappedOut;

    // Step 7: Update aggregate stats
    numHotPagesStayedIn_.inc(nStayedIn);
    numPageMigrations_.inc(nMigrations);
    numSwappedIn_.inc(nSwappedIn);
    numSwappedOut_.inc(nSwappedOut);

    if (cfg_.useEMA) {
        // Record average EMA stall for diagnostics
        double sumEma = 0.0;
        for (const auto& kv : pageTracker_) sumEma += kv.second.ema_stall;
        uint64_t n = pageTracker_.size();
        if (n > 0) statAvgEmaStall_.inc((uint64_t)(sumEma / n));
    }
    if (cfg_.useStabilityBonus && countStabilityBonus > 0) {
        statAvgStabilityBonus_.inc(
            (uint64_t)(sumStabilityBonus / countStabilityBonus * 1000.0));
    }
    if (cfg_.useShadowStall) {
        statAvgShadowYield_.inc((uint64_t)(globalStallYield * 1000.0));
    }

    info("[%s] done: swappedIn=%ld, swappedOut=%ld, stayedIn=%ld, "
         "numFastAllocs=%ld, migrations=%ld",
         cfg_.methodName, nSwappedIn, nSwappedOut, nStayedIn,
         numFastAllocs_, nMigrations);
}
