#ifndef FREQ_BASED_PAGE_MIGRATOR_H_
#define FREQ_BASED_PAGE_MIGRATOR_H_

#include <sstream>

#include "page_migrator.h"
#include "mtrand.h"
#include <mutex>

class FreqBasedPageMigrator: public PageMigrator {
    public:
        FreqBasedPageMigrator(const g_string& _name, PlacementPolicy _pp, uint64_t _numFastMemPages, uint64_t _numIntervalAccesses)
            : PageMigrator(_name, _pp, _numFastMemPages, _numIntervalAccesses) {
                info("Initialized FreqBasedPageMigrator: name: %s, numFastMemPages: %ld, numIntervalAccesses: %ld",
                        name.c_str(), numFastMemPages, numIntervalAccesses);
            }

        ~FreqBasedPageMigrator() override { delete rng; }

        void initStats(AggregateStat* parentStat) {
            AggregateStat* migratorStats = new AggregateStat();
            migratorStats->init(name.c_str(), "NonePagemigrator stats");
            pageTrackerAccesses.init("pageTrackerAccesses", "Number of page tracker accesses"); migratorStats->append(&pageTrackerAccesses);
            pageTrackerHits.init("pageTrackerHits", "Number of page tracker hits"); migratorStats->append(&pageTrackerHits);
            numPageMigrations.init("numPageMigrations", "Total number of pages migrated in either direction"); migratorStats->append(&numPageMigrations);
            numSwappedIn.init("numSwappedIn", "Total number of pages migrated from slow to fast memory"); migratorStats->append(&numSwappedIn);
            numSwappedOut.init("numSwappedOut", "Total number of pages migrated from fast to slow memory"); migratorStats->append(&numSwappedOut);
            numHotPagesStayedIn.init("numHotPagesStayedIn", "Total number of hot pages stayed in the fast memory"); migratorStats->append(&numHotPagesStayedIn);
            intervalBudget.init("intervalBudget", "Migration budget for each interval"); migratorStats->append(&intervalBudget);
            avgIntervalBudget.init("avgIntervalBudget", "Average migration budget across intervals"); migratorStats->append(&avgIntervalBudget);
            maxIntervalBudget.init("maxIntervalBudget", "Maximum migration budget seen in any interval"); migratorStats->append(&maxIntervalBudget);
            minIntervalBudget.init("minIntervalBudget", "Minimum migration budget seen in any interval"); migratorStats->append(&minIntervalBudget);
            
            // Stats for promotion/demotion tracking
            numPromotionIntervals.init("numPromotionIntervals", "Number of intervals where promotion was enabled"); migratorStats->append(&numPromotionIntervals);
            numDemotionIntervals.init("numDemotionIntervals", "Number of intervals where demotion was enabled"); migratorStats->append(&numDemotionIntervals);

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

    private:
        std::vector<uint64_t> getPagesSorted() const;

        struct PageMetadata {
            uint64_t accessCount;           // Since interval
            uint64_t totalAccessCount;      // Since beginning
            uint64_t numSwappedIn;          // Number of times swapped in FAST_MEM since allocation
            uint64_t numSwappedOut;         // Number of times swapped out to SLOW_MEM since allocation
            PageLocation currLoc;

            PageMetadata(uint64_t ac, uint64_t tac, uint64_t nsi, uint64_t nso, PageLocation cl)
                : accessCount(ac), totalAccessCount(tac), numSwappedIn(nsi), numSwappedOut(nso), currLoc(cl) {}

            PageMetadata()
                : accessCount(0L), totalAccessCount(0L), numSwappedIn(0L), numSwappedOut(0L), currLoc(SLOW_MEM) {}

            void accessPage() {
                accessCount++;
                totalAccessCount++;
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

            std::string to_string() const {
                std::ostringstream oss;
                oss << "accessCount=" << accessCount
                    << ", totalAccessCount=" << totalAccessCount
                    << ", numSwappedIn=" << numSwappedIn
                    << ", numSwappedOut=" << numSwappedOut
                    << ", currLoc=" << pageLocationToStr(currLoc).c_str();
                return oss.str();
            }
        };
        g_unordered_map<uint64_t, PageMetadata> pageTracker;

        uint64_t numFastAllocs = 0;

        MTRand *rng = new MTRand(0xABCDE563F);

        Counter pageTrackerAccesses, pageTrackerHits;
        Counter numPageMigrations, numSwappedIn, numSwappedOut, numHotPagesStayedIn;
        Counter intervalBudget;         // Current interval's budget
        Counter avgIntervalBudget;      // Running average
        Counter maxIntervalBudget;      // Maximum seen
        Counter minIntervalBudget;      // Minimum seen
        uint64_t totalIntervals = 0;    // For average calculation
        
        Counter numPromotionIntervals;  // Intervals with promotion
        Counter numDemotionIntervals; 
        
        std::mutex pageTrackerMutex; 
};

#endif // FREQ_BASED_PAGE_MIGRATOR_H_
