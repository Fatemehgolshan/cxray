#ifndef CRITICALITY_WEIGHTED_PAGE_MIGRATOR_H_
#define CRITICALITY_WEIGHTED_PAGE_MIGRATOR_H_

#include <sstream>
#include <unordered_map>
#include "page_migrator.h"
#include "mtrand.h"
#include <mutex>

// Criticality Weighted Page Migrator
// Key idea: Weight stalls by instruction criticality
// - Focus on pages near synchronization/barriers (inferred from access patterns)
// - Pages with bursty, synchronized access patterns are more critical
// - Weight stalls by temporal clustering and inter-page synchronization

class CriticalityWeightedPageMigrator: public PageMigrator {
    public:
        CriticalityWeightedPageMigrator(const g_string& _name, PlacementPolicy _pp, uint64_t _numFastMemPages, uint64_t _numIntervalAccesses)
            : PageMigrator(_name, _pp, _numFastMemPages, _numIntervalAccesses) {
                info("Initialized CriticalityWeightedPageMigrator: name: %s, numFastMemPages: %ld, numIntervalAccesses: %ld",
                        name.c_str(), numFastMemPages, numIntervalAccesses);
            }

        ~CriticalityWeightedPageMigrator() override { delete rng; }

        void initStats(AggregateStat* parentStat) {
            AggregateStat* migratorStats = new AggregateStat();
            migratorStats->init(name.c_str(), "CriticalityWeightedPageMigrator stats");
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
            avgCriticalityScore.init("avgCriticalityScore", "Average criticality score");
            migratorStats->append(&avgCriticalityScore);

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
        struct PageMetadata;  // Forward declaration

        std::vector<uint64_t> getPagesSorted() const;
        double computeCriticalityScore(const PageMetadata& meta) const;

        struct PageMetadata {
            uint64_t accessCount;
            uint64_t totalAccessCount;
            uint64_t numSwappedIn;
            uint64_t numSwappedOut;
            uint64_t robHeadStallCycles;
            uint64_t lastAccessTimestamp;
            uint64_t stallEventCount;

            // Criticality indicators
            double burstiness;               // Temporal clustering of accesses
            double synchronizationScore;     // Co-occurrence with other pages
            double criticalityWeightedStalls; // Stalls weighted by criticality

            PageLocation currLoc;

            PageMetadata(uint64_t ac, uint64_t tac, uint64_t nsi, uint64_t nso, PageLocation cl)
                : accessCount(ac), totalAccessCount(tac), numSwappedIn(nsi), numSwappedOut(nso),
                  robHeadStallCycles(0), lastAccessTimestamp(0), stallEventCount(0),
                  burstiness(0.0), synchronizationScore(0.0), criticalityWeightedStalls(0.0),
                  currLoc(cl) {}

            PageMetadata()
                : accessCount(0L), totalAccessCount(0L), numSwappedIn(0L), numSwappedOut(0L),
                  robHeadStallCycles(0), lastAccessTimestamp(0), stallEventCount(0),
                  burstiness(0.0), synchronizationScore(0.0), criticalityWeightedStalls(0.0),
                  currLoc(SLOW_MEM) {}

            void accessPage(uint64_t timestamp) {
                accessCount++;
                totalAccessCount++;

                // Update burstiness based on inter-access time
                if (lastAccessTimestamp > 0) {
                    uint64_t interAccessTime = timestamp - lastAccessTimestamp;
                    // Shorter inter-access time = higher burstiness
                    double newBurst = (interAccessTime < 1000) ? 1.0 : (1000.0 / interAccessTime);
                    burstiness = 0.9 * burstiness + 0.1 * newBurst;  // Exponential smoothing
                }
                lastAccessTimestamp = timestamp;
            }

            void markRobHeadStall(uint64_t cycles, double criticalityFactor) {
                robHeadStallCycles += cycles;
                stallEventCount++;
                criticalityWeightedStalls += cycles * criticalityFactor;
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
                stallEventCount = 0L;
                criticalityWeightedStalls = 0.0;
                // Don't reset burstiness/synchronizationScore - they're long-term indicators
            }
        };

        g_unordered_map<uint64_t, PageMetadata> pageTracker;
        uint64_t numFastAllocs = 0;
        MTRand *rng = new MTRand(0xABCDE563F);

        // Synchronization detection
        uint64_t globalTimestamp = 0;
        std::unordered_map<uint64_t, uint64_t> recentStallPages;  // Page -> last stall timestamp
        static constexpr uint64_t SYNC_WINDOW = 100;  // Cycles to consider as synchronized

        Counter pageTrackerAccesses, pageTrackerHits;
        Counter numPageMigrations, numSwappedIn, numSwappedOut, numHotPagesStayedIn;
        Counter avgCriticalityScore;
        std::mutex pageTrackerMutex;
};

#endif // CRITICALITY_WEIGHTED_PAGE_MIGRATOR_H_
