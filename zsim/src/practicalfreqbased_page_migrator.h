#ifndef PRACTICAL_FREQ_BASED_PAGE_MIGRATOR_H_
#define PRACTICAL_FREQ_BASED_PAGE_MIGRATOR_H_

#include <sstream>

#include "page_migrator.h"
#include "mtrand.h"
#include <mutex>

// Compile-time configurable bit width for the saturating counter
#ifndef ACCESS_COUNTER_BITS
#define ACCESS_COUNTER_BITS 10
#endif

class PracticalFreqBasedPageMigrator: public PageMigrator {
    public:
        PracticalFreqBasedPageMigrator(const g_string& _name, PlacementPolicy _pp, uint64_t _numFastMemPages, uint64_t _numIntervalAccesses)
            : PageMigrator(_name, _pp, _numFastMemPages, _numIntervalAccesses) {
                info("Initialized PracticalFreqBasedPageMigrator: name: %s, numFastMemPages: %ld, numIntervalAccesses: %ld, counterBits: %d, counterMax: %u",
                        name.c_str(), numFastMemPages, numIntervalAccesses, ACCESS_COUNTER_BITS, SaturatingCounter::getMaxValue());
            }

        ~PracticalFreqBasedPageMigrator() override { delete rng; }

        void initStats(AggregateStat* parentStat) {
            AggregateStat* migratorStats = new AggregateStat();
            migratorStats->init(name.c_str(), "PracticalFreqBasedPageMigrator stats");
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

            // Stats specific to practical implementation
            numCounterSaturations.init("numCounterSaturations", "Number of times access counter saturated"); migratorStats->append(&numCounterSaturations);

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

        // Saturating counter implementation
        class SaturatingCounter {
        private:
            uint32_t value;
            static constexpr uint32_t MAX_VALUE = (1U << ACCESS_COUNTER_BITS) - 1;

        public:
            SaturatingCounter() : value(0) {}
            explicit SaturatingCounter(uint32_t v) : value(v > MAX_VALUE ? MAX_VALUE : v) {}

            void increment(bool& saturated) {
                if (value < MAX_VALUE) {
                    value++;
                    saturated = false;
                } else {
                    saturated = true;
                }
            }

            void reset() { value = 0; }

            uint32_t get() const { return value; }

            static uint32_t getMaxValue() { return MAX_VALUE; }

            bool operator>(const SaturatingCounter& other) const {
                return value > other.value;
            }

            bool operator!=(const SaturatingCounter& other) const {
                return value != other.value;
            }
        };

        struct PageMetadata {
            SaturatingCounter accessCount;      // Since interval - now using saturating counter
            uint64_t totalAccessCount;          // Since beginning - kept as uint64_t
            uint64_t numSwappedIn;              // Number of times swapped in FAST_MEM since allocation
            uint64_t numSwappedOut;             // Number of times swapped out to SLOW_MEM since allocation
            PageLocation currLoc;

            PageMetadata(uint32_t ac, uint64_t tac, uint64_t nsi, uint64_t nso, PageLocation cl)
                : accessCount(ac), totalAccessCount(tac), numSwappedIn(nsi), numSwappedOut(nso), currLoc(cl) {}

            PageMetadata()
                : accessCount(0), totalAccessCount(0L), numSwappedIn(0L), numSwappedOut(0L), currLoc(SLOW_MEM) {}

            bool accessPage(bool& saturated) {
                accessCount.increment(saturated);
                totalAccessCount++;
                return saturated;
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
                accessCount.reset();
            }

            std::string to_string() const {
                std::ostringstream oss;
                oss << "accessCount=" << accessCount.get()
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
        Counter numCounterSaturations;  // Track saturation events

        std::mutex pageTrackerMutex;
};

#endif // PRACTICAL_FREQ_BASED_PAGE_MIGRATOR_H_
