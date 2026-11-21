#include "robstallbased_page_migrator.h"
#include <functional>
PageLocation RobStallBasedPageMigrator::access(uint64_t pageAddress) {
    std::lock_guard<std::mutex> lock(pageTrackerMutex);  
    pageTrackerAccesses.inc();
    auto it = this->pageTracker.find(pageAddress);

    PageLocation loc;
    if (it != pageTracker.end()) {
        pageTrackerHits.inc();
        it->second.accessPage();
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
        this->pageTracker[pageAddress] = pm;
    }
    PageMigrator::registerAccess(loc);
    return loc;
}

void RobStallBasedPageMigrator::triggerMigration() {
    float dramOcc = 1.0 * numFastAllocs / numFastMemPages;
    float dramCoverage = pageTrackerAccesses.get() ?
            (1.0 * numFastAllocs / pageTrackerAccesses.get()) : 0.0;
    float pageTrackerHitRatio = pageTrackerAccesses.get() ?
            (1.0 * pageTrackerHits.get() / pageTrackerAccesses.get()) : 0.0;
    info("Migration is triggered: dramOcc=%f, dramCoverage=%f, pageTrackerHitRatio=%f, numFastAllocs=%ld, numFastMemPages=%ld, pageTrackerAccesses=%ld, pageTrackerHits=%ld",
            dramOcc, dramCoverage, pageTrackerHitRatio, numFastAllocs, numFastMemPages, pageTrackerAccesses.get(), pageTrackerHits.get());

    // Count pages that had any ROB-stall signal this interval (BEFORE the sort loop resets flags).
    {
        uint64_t nStallPages = 0;
        for (const auto& entry : pageTracker) {
            if (entry.second.robHeadStalled || entry.second.robHeadStallEvents > 0) {
                nStallPages++;
            }
        }
        this->numStallPagesObserved.inc(nStallPages);
    }

    std::vector<uint64_t> allPagesSorted = this->getPagesSorted();
    uint64_t hottestHotPageAccesses = 0;
    uint64_t coldestHotPageAccesses = UINT64_MAX;
    uint64_t coldestPageAccesses = UINT64_MAX;

    uint64_t _numHotPagesStayedIn = 0;
    uint64_t _numPageMigrations = 0;
    uint64_t _numSwappedIn = 0;
    uint64_t _numSwappedOut = 0;

    const bool promote = !migrationTrigger || migrationTrigger->promoteDirection();
    // Apply migration bandwidth cap
    promotionBudget = std::min(promotionBudget, migrationPageCap_);

    std::vector<uint64_t> canDemote;
    if (allPagesSorted.size() > numFastMemPages) {
        canDemote.reserve(allPagesSorted.size() - numFastMemPages);
        for (size_t j = numFastMemPages; j < allPagesSorted.size(); ++j) {
            const uint64_t addr = allPagesSorted[j];
            auto it = pageTracker.find(addr);
            if (it != pageTracker.end() && it->second.currLoc == FAST_MEM) {
                canDemote.push_back(addr); // pop_back() yields the coldest first by construction
            }
        }
    }

    for (size_t i = 0; i < allPagesSorted.size(); i++) {
        uint64_t pageAddress = allPagesSorted[i];
        auto pageIt = pageTracker.find(pageAddress);
        if (pageIt == pageTracker.end()) {
            warn("[%s] Page 0x%lx disappeared from pageTracker between sorting and migration", name.c_str(), pageAddress);
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
                        } else {
                            // No victim available → skip this promotion this window
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

            hottestHotPageAccesses = std::max(hottestHotPageAccesses, meta.accessCount);
            coldestHotPageAccesses = std::min(coldestHotPageAccesses, meta.accessCount);

        } else {    // Cold page
            if (meta.currLoc == FAST_MEM) {
                if (promote) {
                    // NEW: structural demotion (no budget) to restore top-K
                    meta.swapPageOut();
                    _numPageMigrations++;
                    _numSwappedOut++;
                    numFastAllocs--;
                } else {
                    // NEW: demote-mode: spend budget to shed FAST load
                    if (this->consumePromotionBudget()) {
                        meta.swapPageOut();
                        _numPageMigrations++;
                        _numSwappedOut++;
                        numFastAllocs--;
                    } else {
                        // optional: break; // stop early once demotion budget exhausted
                    }
                }
            }
            coldestPageAccesses = std::min(coldestPageAccesses, meta.accessCount);
        }

        meta.resetAccessCounter();
        meta.resetRobHeadStallFlag();    
    }

    dramOcc = 1.0 * numFastAllocs / numFastMemPages;
    dramCoverage = pageTrackerAccesses.get() ?
            (1.0 * numFastAllocs / pageTrackerAccesses.get()) : 0.0;
    uint64_t totalHotPagesConsidered = _numHotPagesStayedIn + _numSwappedIn;
    float hotPagesStayedFrac = totalHotPagesConsidered ?
            (1.0 * _numHotPagesStayedIn / totalHotPagesConsidered) : 0.0;
    float hotPagesMovedInFrac = totalHotPagesConsidered ?
            (1.0 * _numSwappedIn / totalHotPagesConsidered) : 0.0;

    this->numHotPagesStayedIn.inc(_numHotPagesStayedIn);
    this->numPageMigrations.inc(_numPageMigrations);
    this->numSwappedIn.inc(_numSwappedIn);
    this->numSwappedOut.inc(_numSwappedOut);
    this->lastIntervalMigrations_ = _numSwappedIn + _numSwappedOut;

    info("Migration is done: dramOcc=%f, dramCoverage=%f, hotPagesStayed=%f, hotPagesMovedIn=%f, hottestHotPageAccesses=%ld, coldestHotPageAccesses=%ld, coldestPageAccesses=%ld, numHotPagesStayedIn=%ld, numPageMigrations=%ld, numSwappedIn=%ld, numSwappedOut=%ld",
            dramOcc, dramCoverage, hotPagesStayedFrac, hotPagesMovedInFrac, hottestHotPageAccesses, coldestHotPageAccesses, coldestPageAccesses, _numHotPagesStayedIn, _numPageMigrations, _numSwappedIn, _numSwappedOut);

}

std::vector<uint64_t> RobStallBasedPageMigrator::getPagesSorted() const {
    using CIter = decltype(pageTracker)::const_iterator;

    std::vector<CIter> items;
    items.reserve(pageTracker.size());
    for (auto it = pageTracker.begin(); it != pageTracker.end(); ++it) {
        items.push_back(it);
    }

    // Define sorting comparator based on the ROB stall metric variant
    std::function<bool(const CIter&, const CIter&)> better;

    switch (robMetric) {
        case ROB_STALL_BINARY: {
            // Variant 1: Binary flag - pages that caused stall are hot
            // Tie-breaker: prefer pages already in FAST_MEM, then by address
            better = [](const CIter& a, const CIter& b) {
                const auto& A = a->second;
                const auto& B = b->second;
                // Primary: pages with stall flag set
                if (A.robHeadStalled != B.robHeadStalled)
                    return A.robHeadStalled && !B.robHeadStalled;
                // Tie-breaker 1: prefer pages already in FAST_MEM to reduce migrations
                if ((A.currLoc == FAST_MEM) != (B.currLoc == FAST_MEM))
                    return A.currLoc == FAST_MEM;
                // Tie-breaker 2: deterministic by address
                return a->first < b->first;
            };
            break;
        }

        case ROB_STALL_CYCLES: {
            // Variant 2: Sort by accumulated stall cycles (highest first)
            better = [](const CIter& a, const CIter& b) {
                const auto& A = a->second;
                const auto& B = b->second;
                // Primary: pages with more stall cycles
                if (A.robHeadStallCycles != B.robHeadStallCycles)
                    return A.robHeadStallCycles > B.robHeadStallCycles;
                // Tie-breaker 1: prefer pages already in FAST_MEM
                if ((A.currLoc == FAST_MEM) != (B.currLoc == FAST_MEM))
                    return A.currLoc == FAST_MEM;
                // Tie-breaker 2: deterministic by address
                return a->first < b->first;
            };
            break;
        }

        case ROB_STALL_COUNT: {
            // Variant 3: Sort by number of stall events (highest first)
            better = [](const CIter& a, const CIter& b) {
                const auto& A = a->second;
                const auto& B = b->second;
                // Primary: pages with more stall events
                if (A.robHeadStallEvents != B.robHeadStallEvents)
                    return A.robHeadStallEvents > B.robHeadStallEvents;
                // Tie-breaker 1: prefer pages already in FAST_MEM
                if ((A.currLoc == FAST_MEM) != (B.currLoc == FAST_MEM))
                    return A.currLoc == FAST_MEM;
                // Tie-breaker 2: deterministic by address
                return a->first < b->first;
            };
            break;
        }

        default:
            panic("Unknown ROB stall metric: %d", robMetric);
    }

    std::sort(items.begin(), items.end(), better);

    std::vector<uint64_t> keys;
    keys.reserve(items.size());
    for (const auto& it : items) keys.push_back(it->first);
    return keys;
}
void RobStallBasedPageMigrator::registerRobHeadLoadStall(uint64_t pageAddress, uint64_t events, uint32_t /*coreId*/) {
    if (events == 0) return;
    std::lock_guard<std::mutex> lock(pageTrackerMutex);
    auto it = pageTracker.find(pageAddress);
    if (it == pageTracker.end()) return;
    it->second.markRobHeadStall(events);
}
