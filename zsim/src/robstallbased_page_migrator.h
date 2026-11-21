#ifndef ROB_STALL_BASED_PAGE_MIGRATOR_H_
#define ROB_STALL_BASED_PAGE_MIGRATOR_H_

#include <sstream>

#include "page_migrator.h"
#include "mtrand.h"
#include <mutex>

class RobStallBasedPageMigrator: public PageMigrator {
    public:
        RobStallBasedPageMigrator(const g_string& _name, PlacementPolicy _pp, uint64_t _numFastMemPages, uint64_t _numIntervalAccesses, RobStallMetric _metric)
            : PageMigrator(_name, _pp, _numFastMemPages, _numIntervalAccesses), robMetric(_metric) {
                g_string metricStr = robStallMetricToStr(_metric);
                info("Initialized RobStallBasedPageMigrator: name: %s, metric: %s, numFastMemPages: %ld, numIntervalAccesses: %ld",
                        name.c_str(), metricStr.c_str(), numFastMemPages, numIntervalAccesses);
            }

        ~RobStallBasedPageMigrator() override { delete rng; }

        void initStats(AggregateStat* parentStat) {
            AggregateStat* migratorStats = new AggregateStat();
            migratorStats->init(name.c_str(), "RobStallBasedPageMigrator stats");
            pageTrackerAccesses.init("pageTrackerAccesses", "Number of page tracker accesses"); migratorStats->append(&pageTrackerAccesses);
            pageTrackerHits.init("pageTrackerHits", "Number of page tracker hits"); migratorStats->append(&pageTrackerHits);
            numPageMigrations.init("numPageMigrations", "Total number of pages migrated in either direction"); migratorStats->append(&numPageMigrations);
            numSwappedIn.init("numSwappedIn", "Total number of pages migrated from slow to fast memory"); migratorStats->append(&numSwappedIn);
            numSwappedOut.init("numSwappedOut", "Total number of pages migrated from fast to slow memory"); migratorStats->append(&numSwappedOut);
            numHotPagesStayedIn.init("numHotPagesStayedIn", "Total number of hot pages stayed in the fast memory"); migratorStats->append(&numHotPagesStayedIn);
            // numStallPagesObserved: cumulative sum of (pages with any stall signal) per interval.
            // The Python metric subtracts warmup value for a post-warmup delta.
            numStallPagesObserved.init("numStallPagesObserved",
                "Cumulative count of pages with ROB-stall signal (summed per migration interval)");
            migratorStats->append(&numStallPagesObserved);

            PageMigrator::initStats(migratorStats);
            parentStat->append(migratorStats);
        }
        
        uint64_t getRobStallPages() const {
            uint64_t count = 0;
            for (const auto& entry : pageTracker) {
                if (entry.second.totalRobHeadStallCycles > 0 || entry.second.totalRobHeadStallEvents > 0) count++;
            }
            return count;
        }
        
        // Memory footprint query methods  
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
        void registerRobHeadLoadStall(uint64_t pageAddress, uint64_t events, uint32_t coreId = 0) override;
    private:
        std::vector<uint64_t> getPagesSorted() const;

        struct PageMetadata {
            uint64_t accessCount;           // Since interval
            uint64_t totalAccessCount;      // Since beginning
            uint64_t numSwappedIn;          // Number of times swapped in FAST_MEM since allocation
            uint64_t numSwappedOut;         // Number of times swapped out to SLOW_MEM since allocation
            bool robHeadStalled;            // Whether the page triggered a ROB-head stall in the current interval (for BINARY variant)
            uint64_t robHeadStallCycles;    // Total stall cycles caused by this page in the current interval (for CYCLES variant)
            uint64_t robHeadStallEvents;    // Number of times this page caused ROB stall in the current interval (for COUNT variant)
            uint64_t totalRobHeadStallCycles;   // Total stall cycles since allocation
            uint64_t totalRobHeadStallEvents;   // Total stall events since allocation
            PageLocation currLoc;

            PageMetadata(uint64_t ac, uint64_t tac, uint64_t nsi, uint64_t nso, PageLocation cl)
                : accessCount(ac), totalAccessCount(tac), numSwappedIn(nsi), numSwappedOut(nso),
                  robHeadStalled(false), robHeadStallCycles(0), robHeadStallEvents(0),
                  totalRobHeadStallCycles(0), totalRobHeadStallEvents(0), currLoc(cl) {}

            PageMetadata()
                : accessCount(0L), totalAccessCount(0L), numSwappedIn(0L), numSwappedOut(0L),
                  robHeadStalled(false), robHeadStallCycles(0), robHeadStallEvents(0),
                  totalRobHeadStallCycles(0), totalRobHeadStallEvents(0), currLoc(SLOW_MEM) {}

            void accessPage() {
                accessCount++;
                totalAccessCount++;
            }
            void markRobHeadStall(uint64_t cycles) {
                assert(cycles > 0 && "markRobHeadStall should not be called with 0 cycles");
                robHeadStalled = true;                      // Set flag for BINARY variant
                robHeadStallCycles += cycles;               // Accumulate cycles for CYCLES variant
                robHeadStallEvents++;                       // Increment event count for COUNT variant
                totalRobHeadStallCycles += cycles;
                totalRobHeadStallEvents++;
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
            }
            void resetRobHeadStallFlag() {
                robHeadStalled = false;         // Reset BINARY flag
                robHeadStallCycles = 0L;        // Reset CYCLES counter
                robHeadStallEvents = 0L;        // Reset COUNT/events counter
            }
            std::string to_string() const {
                std::ostringstream oss;
                oss << "accessCount=" << accessCount
                    << ", totalAccessCount=" << totalAccessCount
                    << ", numSwappedIn=" << numSwappedIn
                    << ", numSwappedOut=" << numSwappedOut
                    << ", robHeadStalled=" << robHeadStalled
                    << ", robHeadStallCycles=" << robHeadStallCycles
                    << ", robHeadStallEvents=" << robHeadStallEvents
                    << ", currLoc=" << pageLocationToStr(currLoc).c_str();
                return oss.str();
            }
        };
        g_unordered_map<uint64_t, PageMetadata> pageTracker;

        uint64_t numFastAllocs = 0;
        RobStallMetric robMetric;  // Variant: BINARY, CYCLES, or COUNT

        MTRand *rng = new MTRand(0xABCDE563F);

        Counter pageTrackerAccesses, pageTrackerHits;
        Counter numPageMigrations, numSwappedIn, numSwappedOut, numHotPagesStayedIn;
        Counter numStallPagesObserved;
        std::mutex pageTrackerMutex;
};

#endif // ROB_STALL_BASED_PAGE_MIGRATOR_H_
