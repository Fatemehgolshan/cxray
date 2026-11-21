#include "practicalfreqbased_page_migrator.h"

PageLocation PracticalFreqBasedPageMigrator::access(uint64_t pageAddress) {
    std::lock_guard<std::mutex> lock(pageTrackerMutex);
    pageTrackerAccesses.inc();
    auto it = this->pageTracker.find(pageAddress);

    PageLocation loc;
    if (it != pageTracker.end()) {
        pageTrackerHits.inc();
        bool saturated = false;
        it->second.accessPage(saturated);
        if (saturated) {
            numCounterSaturations.inc();
        }
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

void PracticalFreqBasedPageMigrator::triggerMigration() {
    float dramOcc = 1.0 * numFastAllocs / numFastMemPages;
    float dramCoverage = pageTrackerAccesses.get() ?
            (1.0 * numFastAllocs / pageTrackerAccesses.get()) : 0.0;
    float pageTrackerHitRatio = pageTrackerAccesses.get() ?
            (1.0 * pageTrackerHits.get() / pageTrackerAccesses.get()) : 0.0;
    info("Migration is triggered: dramOcc=%f, dramCoverage=%f, pageTrackerHitRatio=%f, numFastAllocs=%ld, numFastMemPages=%ld, pageTrackerAccesses=%ld, pageTrackerHits=%ld",
            dramOcc, dramCoverage, pageTrackerHitRatio, numFastAllocs, numFastMemPages, pageTrackerAccesses.get(), pageTrackerHits.get());

    std::vector<uint64_t> allPagesSorted = this->getPagesSorted();
    uint64_t hottestHotPageAccesses = 0;
    uint64_t coldestHotPageAccesses = UINT64_MAX;
    uint64_t coldestPageAccesses = UINT64_MAX;

    uint64_t _numHotPagesStayedIn = 0;
    uint64_t _numPageMigrations = 0;
    uint64_t _numSwappedIn = 0;
    uint64_t _numSwappedOut = 0;

    const bool promote = !migrationTrigger || migrationTrigger->promoteDirection();

    // Track direction
    if (promote) {
        numPromotionIntervals.inc();
    } else {
        numDemotionIntervals.inc();
    }

    // Track budget
    uint64_t currentBudget = this->getPromotionBudget();
    intervalBudget.inc(currentBudget);

    // Update min/max
    if (totalIntervals == 0 || currentBudget < minIntervalBudget.get()) {
        minIntervalBudget.set(currentBudget);
    }
    if (totalIntervals == 0 || currentBudget > maxIntervalBudget.get()) {
        maxIntervalBudget.set(currentBudget);
    }

    // Update running average
    totalIntervals++;
    uint64_t newAvg = ((avgIntervalBudget.get() * (totalIntervals - 1)) + currentBudget) / totalIntervals;
    avgIntervalBudget.set(newAvg);

    info("Migration interval %lu: direction=%s, budget=%lu, avg=%lu, min=%lu, max=%lu",
         totalIntervals,
         promote ? "promote" : "demote",
         currentBudget,
         newAvg,
         minIntervalBudget.get(),
         maxIntervalBudget.get());

    std::vector<uint64_t> canDemote;
    if (allPagesSorted.size() > numFastMemPages) {
        canDemote.reserve(allPagesSorted.size() - numFastMemPages);
        for (size_t j = numFastMemPages; j < allPagesSorted.size(); ++j) {
            uint64_t addr = allPagesSorted[j];
            auto it = pageTracker.find(addr);
            if (it != pageTracker.end() && it->second.currLoc == FAST_MEM) {
                canDemote.push_back(addr); // later pop_back() gives coldest-first
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
                            // No victim available → skip this promotion
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

            hottestHotPageAccesses = std::max(hottestHotPageAccesses, (uint64_t)meta.accessCount.get());
            coldestHotPageAccesses = std::min(coldestHotPageAccesses, (uint64_t)meta.accessCount.get());

        } else  {    // Cold page
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
            coldestPageAccesses = std::min(coldestPageAccesses, (uint64_t)meta.accessCount.get());
        }

        meta.resetAccessCounter();
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

    info("Miration is done: dramOcc=%f, dramCoverage=%f, hotPagesStayed=%f, hotPagesMovedIn=%f, hottestHotPageAccesses=%ld, coldestHotPageAccesses=%ld, coldestPageAccesses=%ld, numHotPagesStayedIn=%ld, numPageMigrations=%ld, numSwappedIn=%ld, numSwappedOut=%ld",
            dramOcc, dramCoverage, hotPagesStayedFrac, hotPagesMovedInFrac, hottestHotPageAccesses, coldestHotPageAccesses, coldestPageAccesses, _numHotPagesStayedIn, _numPageMigrations, _numSwappedIn, _numSwappedOut);

}

std::vector<uint64_t> PracticalFreqBasedPageMigrator::getPagesSorted() const {
    using CIter = decltype(pageTracker)::const_iterator;

    std::vector<CIter> items;
    items.reserve(pageTracker.size());
    for (auto it = pageTracker.begin(); it != pageTracker.end(); ++it) {
        items.push_back(it);
    }

    auto better = [](const CIter& a, const CIter& b) {
        const auto& A = a->second;
        const auto& B = b->second;
        if (A.accessCount != B.accessCount) return A.accessCount > B.accessCount;
        if ((A.currLoc == FAST_MEM) != (B.currLoc == FAST_MEM))
            return A.currLoc == FAST_MEM;  // FAST_MEM before SLOW_MEM
        return a->first < b->first;        // deterministic tie-breaker
    };

    std::sort(items.begin(), items.end(), better);

    std::vector<uint64_t> keys;
    keys.reserve(items.size());
    for (const auto& it : items) keys.push_back(it->first);
    return keys;
}
