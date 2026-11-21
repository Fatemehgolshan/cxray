#include "criticality_weighted_page_migrator.h"
#include <functional>
#include <algorithm>
#include <cmath>

PageLocation CriticalityWeightedPageMigrator::access(uint64_t pageAddress) {
    std::lock_guard<std::mutex> lock(pageTrackerMutex);
    pageTrackerAccesses.inc();
    globalTimestamp++;

    auto it = this->pageTracker.find(pageAddress);

    PageLocation loc;
    if (it != pageTracker.end()) {
        pageTrackerHits.inc();
        it->second.accessPage(globalTimestamp);
        loc = it->second.currLoc;
    } else {
        // Placement
        if (this->placementPolicy == RANDOM) {
            loc = rng->randInt(1) ? FAST_MEM : SLOW_MEM;
        } else if (this->placementPolicy == FAST_FIRST) {
            loc = numFastAllocs < this->numFastMemPages ? FAST_MEM : SLOW_MEM;
        } else if (this->placementPolicy == SLOW_FIRST) {
            loc = SLOW_MEM;
        } else {
            g_string ppStr = placementPolicyToStr(this->placementPolicy);
            panic("Unknown placement policy: %s", ppStr.c_str());
        }

        numFastAllocs += (loc == FAST_MEM);
        PageMetadata pm(1, 1, 0, 0, loc);
        pm.lastAccessTimestamp = globalTimestamp;
        this->pageTracker[pageAddress] = pm;
    }
    PageMigrator::registerAccess(loc);
    return loc;
}

double CriticalityWeightedPageMigrator::computeCriticalityScore(const PageMetadata& meta) const {
    // Criticality score combines multiple factors:
    // 1. Burstiness - bursty accesses indicate critical sections
    // 2. Synchronization score - co-occurrence with other stalling pages
    // 3. Stall intensity - total stalls

    double burstFactor = meta.burstiness;  // 0-1 range, higher = more bursty
    double syncFactor = std::min(1.0, meta.synchronizationScore);  // 0-1 range
    double stallIntensity = (meta.stallEventCount > 0) ?
                           static_cast<double>(meta.robHeadStallCycles) / meta.stallEventCount : 0.0;

    // Normalize stall intensity (assume 1000 cycles is "high")
    double normalizedIntensity = std::min(1.0, stallIntensity / 1000.0);

    // Combined score: weight all factors
    // Higher weight on synchronization (indicates critical sections/barriers)
    double score = 0.3 * burstFactor + 0.5 * syncFactor + 0.2 * normalizedIntensity;

    return score;
}

void CriticalityWeightedPageMigrator::registerRobHeadLoadStall(uint64_t pageAddress, uint64_t cycles, uint32_t /*coreId*/) {
    if (cycles == 0) return;

    std::lock_guard<std::mutex> lock(pageTrackerMutex);

    auto it = pageTracker.find(pageAddress);
    if (it == pageTracker.end()) return;

    // Detect synchronization: count how many other pages stalled recently
    size_t coStallCount = 0;
    for (const auto& entry : recentStallPages) {
        if (entry.first != pageAddress &&
            globalTimestamp - entry.second < SYNC_WINDOW) {
            coStallCount++;
        }
    }

    // Update synchronization score
    double syncScore = static_cast<double>(coStallCount) / 10.0;  // Normalize
    it->second.synchronizationScore = 0.8 * it->second.synchronizationScore + 0.2 * syncScore;

    // Compute criticality factor
    double criticalityFactor = 1.0 + computeCriticalityScore(it->second);

    // Mark the stall with criticality weighting
    it->second.markRobHeadStall(cycles, criticalityFactor);

    // Update recent stall tracking
    recentStallPages[pageAddress] = globalTimestamp;

    // Clean up old entries
    if (recentStallPages.size() > 100) {
        for (auto iter = recentStallPages.begin(); iter != recentStallPages.end(); ) {
            if (globalTimestamp - iter->second > SYNC_WINDOW * 10) {
                iter = recentStallPages.erase(iter);
            } else {
                ++iter;
            }
        }
    }
}

void CriticalityWeightedPageMigrator::triggerMigration() {
    float dramOcc = 1.0 * numFastAllocs / numFastMemPages;

    // Compute average criticality
    double totalCriticality = 0.0;
    size_t criticalPageCount = 0;
    for (const auto& entry : pageTracker) {
        if (entry.second.robHeadStallCycles > 0) {
            totalCriticality += computeCriticalityScore(entry.second);
            criticalPageCount++;
        }
    }
    double avgCrit = (criticalPageCount > 0) ? totalCriticality / criticalPageCount : 0.0;
    avgCriticalityScore.inc(static_cast<uint64_t>(avgCrit * 1000));

    info("Migration triggered: dramOcc=%f, avgCriticality=%f, numFastAllocs=%ld",
            dramOcc, avgCrit, numFastAllocs);

    std::vector<uint64_t> allPagesSorted = this->getPagesSorted();
    uint64_t _numHotPagesStayedIn = 0;
    uint64_t _numPageMigrations = 0;
    uint64_t _numSwappedIn = 0;
    uint64_t _numSwappedOut = 0;

    const bool promote = !migrationTrigger || migrationTrigger->promoteDirection();

    std::vector<uint64_t> canDemote;
    if (allPagesSorted.size() > numFastMemPages) {
        canDemote.reserve(allPagesSorted.size() - numFastMemPages);
        for (size_t j = numFastMemPages; j < allPagesSorted.size(); ++j) {
            const uint64_t addr = allPagesSorted[j];
            auto it = pageTracker.find(addr);
            if (it != pageTracker.end() && it->second.currLoc == FAST_MEM) {
                canDemote.push_back(addr);
            }
        }
    }

    for (size_t i = 0; i < allPagesSorted.size(); i++) {
        uint64_t pageAddress = allPagesSorted[i];
        auto pageIt = pageTracker.find(pageAddress);
        if (pageIt == pageTracker.end()) {
            warn("[%s] Page 0x%lx disappeared", name.c_str(), pageAddress);
            continue;
        }
        auto& meta = pageIt->second;

        if (i < numFastMemPages) {  // Hot page
            if (meta.currLoc == FAST_MEM) {
                _numHotPagesStayedIn++;
            } else {
                if (promote && this->consumePromotionBudget()) {
                    if (numFastAllocs >= numFastMemPages) {
                        if (!canDemote.empty()) {
                            const uint64_t victimAddr = canDemote.back();
                            canDemote.pop_back();
                            auto vit = pageTracker.find(victimAddr);
                            if (vit != pageTracker.end() && vit->second.currLoc == FAST_MEM) {
                                vit->second.swapPageOut();
                                _numPageMigrations++;
                                _numSwappedOut++;
                                numFastAllocs--;
                            }
                        }
                    }
                    if (meta.currLoc == SLOW_MEM && numFastAllocs < numFastMemPages) {
                        meta.swapPageIn();
                        _numPageMigrations++;
                        _numSwappedIn++;
                        numFastAllocs++;
                    }
                }
            }
        } else {  // Cold page
            if (meta.currLoc == FAST_MEM) {
                if (promote) {
                    meta.swapPageOut();
                    _numPageMigrations++;
                    _numSwappedOut++;
                    numFastAllocs--;
                } else {
                    if (this->consumePromotionBudget()) {
                        meta.swapPageOut();
                        _numPageMigrations++;
                        _numSwappedOut++;
                        numFastAllocs--;
                    }
                }
            }
        }

        meta.resetAccessCounter();
    }

    this->numHotPagesStayedIn.inc(_numHotPagesStayedIn);
    this->numPageMigrations.inc(_numPageMigrations);
    this->numSwappedIn.inc(_numSwappedIn);
    this->numSwappedOut.inc(_numSwappedOut);

    info("Migration done: numPageMigrations=%ld, numSwappedIn=%ld, numSwappedOut=%ld",
            _numPageMigrations, _numSwappedIn, _numSwappedOut);
}

std::vector<uint64_t> CriticalityWeightedPageMigrator::getPagesSorted() const {
    using CIter = decltype(pageTracker)::const_iterator;

    std::vector<CIter> items;
    items.reserve(pageTracker.size());
    for (auto it = pageTracker.begin(); it != pageTracker.end(); ++it) {
        items.push_back(it);
    }

    // Sort by criticality-weighted stalls (higher = hotter)
    auto better = [](const CIter& a, const CIter& b) {
        const auto& A = a->second;
        const auto& B = b->second;
        if (A.criticalityWeightedStalls != B.criticalityWeightedStalls)
            return A.criticalityWeightedStalls > B.criticalityWeightedStalls;
        if ((A.currLoc == FAST_MEM) != (B.currLoc == FAST_MEM))
            return A.currLoc == FAST_MEM;
        return a->first < b->first;
    };

    std::sort(items.begin(), items.end(), better);

    std::vector<uint64_t> keys;
    keys.reserve(items.size());
    for (const auto& it : items) keys.push_back(it->first);
    return keys;
}
