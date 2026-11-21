#include "robpressure_weighted_page_migrator.h"
#include <functional>
#include <algorithm>

PageLocation RobPressureWeightedPageMigrator::access(uint64_t pageAddress) {
    std::lock_guard<std::mutex> lock(pageTrackerMutex);
    pageTrackerAccesses.inc();
    globalTimestamp++;

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

double RobPressureWeightedPageMigrator::estimateRobPressure() const {
    // Clean up old events outside the window
    uint64_t windowStart = (globalTimestamp > PRESSURE_WINDOW_CYCLES) ?
                           (globalTimestamp - PRESSURE_WINDOW_CYCLES) : 0;

    // Count stall intensity in recent window
    uint64_t recentStallCycles = 0;
    size_t recentStallCount = 0;

    for (auto it = recentStalls.rbegin(); it != recentStalls.rend(); ++it) {
        if (it->timestamp < windowStart) break;
        recentStallCycles += it->cycles;
        recentStallCount++;
    }

    if (recentStallCount == 0) return 0.0;

    // Pressure heuristic: combination of stall density and burst size
    double stallDensity = static_cast<double>(recentStallCycles) / PRESSURE_WINDOW_CYCLES;
    double burstFactor = static_cast<double>(recentStallCount) / MAX_STALL_HISTORY;

    // Normalize to [0, 1] range (clamped)
    double pressure = std::min(1.0, stallDensity * 10.0 + burstFactor * 0.5);
    return pressure;
}

void RobPressureWeightedPageMigrator::registerRobHeadLoadStall(uint64_t pageAddress, uint64_t cycles, uint32_t /*coreId*/) {
    if (cycles == 0) return;

    std::lock_guard<std::mutex> lock(pageTrackerMutex);

    // Track stall for pressure estimation
    recentStalls.push_back({globalTimestamp, cycles});
    if (recentStalls.size() > MAX_STALL_HISTORY) {
        recentStalls.pop_front();
    }

    double robPressure = estimateRobPressure();

    auto it = pageTracker.find(pageAddress);
    if (it == pageTracker.end()) return;

    it->second.markRobHeadStall(cycles, robPressure);
}

void RobPressureWeightedPageMigrator::triggerMigration() {
    float dramOcc = 1.0 * numFastAllocs / numFastMemPages;
    double currentPressure = estimateRobPressure();
    avgRobPressure.inc(static_cast<uint64_t>(currentPressure * 1000));  // Store as fixed-point

    info("Migration triggered: dramOcc=%f, robPressure=%f, numFastAllocs=%ld",
            dramOcc, currentPressure, numFastAllocs);

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

std::vector<uint64_t> RobPressureWeightedPageMigrator::getPagesSorted() const {
    using CIter = decltype(pageTracker)::const_iterator;

    std::vector<CIter> items;
    items.reserve(pageTracker.size());
    for (auto it = pageTracker.begin(); it != pageTracker.end(); ++it) {
        items.push_back(it);
    }

    // Sort by weighted stall cycles (higher = hotter)
    auto better = [](const CIter& a, const CIter& b) {
        const auto& A = a->second;
        const auto& B = b->second;
        if (A.weightedStallCycles != B.weightedStallCycles)
            return A.weightedStallCycles > B.weightedStallCycles;
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
