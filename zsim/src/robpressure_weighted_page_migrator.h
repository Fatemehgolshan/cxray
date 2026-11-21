#ifndef ROB_PRESSURE_WEIGHTED_PAGE_MIGRATOR_H_
#define ROB_PRESSURE_WEIGHTED_PAGE_MIGRATOR_H_

#include <sstream>
#include <queue>
#include <deque>
#include "page_migrator.h"
#include "mtrand.h"
#include <mutex>

// ROB Pressure Weighted Page Migrator
// Key idea: Weight stalls by inferred ROB pressure
// - Higher ROB pressure (more backed up) → more critical stalls
// - Infer pressure from stall bursts and intensity

class RobPressureWeightedPageMigrator: public PageMigrator {
    public:
        RobPressureWeightedPageMigrator(const g_string& _name, PlacementPolicy _pp, uint64_t _numFastMemPages, uint64_t _numIntervalAccesses)
            : PageMigrator(_name, _pp, _numFastMemPages, _numIntervalAccesses) {
                info("Initialized RobPressureWeightedPageMigrator: name: %s, numFastMemPages: %ld, numIntervalAccesses: %ld",
                        name.c_str(), numFastMemPages, numIntervalAccesses);
            }

        ~RobPressureWeightedPageMigrator() override { delete rng; }

        void initStats(AggregateStat* parentStat) {
            AggregateStat* migratorStats = new AggregateStat();
            migratorStats->init(name.c_str(), "RobPressureWeightedPageMigrator stats");
            pageTrackerAccesses.init("pageTrackerAccesses", "Number of page tracker accesses");
            migratorStats->append(&pageTrackerAccesses);
            pageTrackerHits.init("pageTrackerHits", "Number of page tracker hits");
            migratorStats->append(&pageTrackerHits);
            numPageMigrations.init("numPageMigrations", "Total number of pages migrated in either direction");
            migratorStats->append(&numPageMigrations);
            numSwappedIn.init("numSwappedIn", "Total number of pages migrated from slow to fast memory");
            migratorStats->append(&numSwappedIn);
            numSwappedOut.init("numSwappedOut", "Total number of pages migrated from fast to slow memory");
            migratorStats->append(&numSwappedOut);
            numHotPagesStayedIn.init("numHotPagesStayedIn", "Total number of hot pages stayed in the fast memory");
            migratorStats->append(&numHotPagesStayedIn);
            avgRobPressure.init("avgRobPressure", "Average ROB pressure (0-1)");
            migratorStats->append(&avgRobPressure);

            PageMigrator::initStats(migratorStats);
            parentStat->append(migratorStats);
        }

        uint64_t getTotalUniquePages() const override { return pageTracker.size(); }
        uint64_t getFastMemPages() const override {
            uint64_t count = 0;
            for (const auto& entry : pageTracker) {
                if (entry.second.currLoc == FAST_MEM) count++;
            }
            return count;
        }
        uint64_t getSlowMemPages() const override {
            uint64_t count = 0;
            for (const auto& entry : pageTracker) {
                if (entry.second.currLoc == SLOW_MEM) count++;
            }
            return count;
        }

        PageLocation access(uint64_t pageAddress) override;
        void triggerMigration() override;
        void registerRobHeadLoadStall(uint64_t pageAddress, uint64_t cycles, uint32_t coreId = 0) override;

    private:
        std::vector<uint64_t> getPagesSorted() const;
        double estimateRobPressure() const;

        struct StallEvent {
            uint64_t timestamp;
            uint64_t cycles;
        };

        struct PageMetadata {
            uint64_t accessCount;
            uint64_t totalAccessCount;
            uint64_t numSwappedIn;
            uint64_t numSwappedOut;
            uint64_t robHeadStallCycles;
            double weightedStallCycles;  // Cycles weighted by ROB pressure
            PageLocation currLoc;

            PageMetadata(uint64_t ac, uint64_t tac, uint64_t nsi, uint64_t nso, PageLocation cl)
                : accessCount(ac), totalAccessCount(tac), numSwappedIn(nsi), numSwappedOut(nso),
                  robHeadStallCycles(0), weightedStallCycles(0.0), currLoc(cl) {}

            PageMetadata()
                : accessCount(0L), totalAccessCount(0L), numSwappedIn(0L), numSwappedOut(0L),
                  robHeadStallCycles(0), weightedStallCycles(0.0), currLoc(SLOW_MEM) {}

            void accessPage() {
                accessCount++;
                totalAccessCount++;
            }

            void markRobHeadStall(uint64_t cycles, double robPressure) {
                robHeadStallCycles += cycles;
                // Weight by ROB pressure: higher pressure = more critical
                double weight = 1.0 + robPressure;  // 1.0 to 2.0 range
                weightedStallCycles += cycles * weight;
            }

            void swapPageIn() {
                numSwappedIn++;
                assert(currLoc == SLOW_MEM);
                currLoc = FAST_MEM;
            }

            void swapPageOut() {
                numSwappedOut++;
                assert(currLoc == FAST_MEM);
                currLoc = SLOW_MEM;
            }

            void resetAccessCounter() {
                accessCount = 0L;
                robHeadStallCycles = 0L;
                weightedStallCycles = 0.0;
            }
        };

        g_unordered_map<uint64_t, PageMetadata> pageTracker;
        uint64_t numFastAllocs = 0;
        MTRand *rng = new MTRand(0xABCDE563F);

        // ROB pressure tracking
        std::deque<StallEvent> recentStalls;
        static constexpr size_t MAX_STALL_HISTORY = 1000;
        static constexpr uint64_t PRESSURE_WINDOW_CYCLES = 10000;
        uint64_t globalTimestamp = 0;

        Counter pageTrackerAccesses, pageTrackerHits;
        Counter numPageMigrations, numSwappedIn, numSwappedOut, numHotPagesStayedIn;
        Counter avgRobPressure;
        std::mutex pageTrackerMutex;
};

#endif // ROB_PRESSURE_WEIGHTED_PAGE_MIGRATOR_H_
