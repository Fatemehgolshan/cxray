#ifndef QLEARNING_PAGE_MIGRATOR_H_
#define QLEARNING_PAGE_MIGRATOR_H_

#include <sstream>
#include <vector>
#include <array>
#include "page_migrator.h"
#include "mtrand.h"
#include <mutex>

// Q-Learning Page Migrator
// Key idea: Learn optimal page placement policy via reinforcement learning
// - State: Page characteristics (access frequency, stall history, location)
// - Action: Keep in FAST vs move to SLOW (or vice versa)
// - Reward: Performance improvement (stall reduction)
// - Use lightweight tabular Q-learning with discretized states

class QLearningPageMigrator: public PageMigrator {
    public:
        QLearningPageMigrator(const g_string& _name, PlacementPolicy _pp, uint64_t _numFastMemPages, uint64_t _numIntervalAccesses)
            : PageMigrator(_name, _pp, _numFastMemPages, _numIntervalAccesses) {
                // Initialize Q-table
                for (auto& stateActions : qTable) {
                    stateActions.fill(0.0);  // Optimistic initialization
                }
                info("Initialized QLearningPageMigrator: name: %s, numFastMemPages: %ld, numIntervalAccesses: %ld",
                        name.c_str(), numFastMemPages, numIntervalAccesses);
            }

        ~QLearningPageMigrator() override { delete rng; }

        void initStats(AggregateStat* parentStat) {
            AggregateStat* migratorStats = new AggregateStat();
            migratorStats->init(name.c_str(), "QLearningPageMigrator stats");
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
            totalReward.init("totalReward", "Cumulative reward");
            migratorStats->append(&totalReward);
            explorationRate.init("explorationRate", "Current exploration rate (epsilon * 1000)");
            migratorStats->append(&explorationRate);

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
        enum Action { ACTION_KEEP_FAST = 0, ACTION_MOVE_SLOW = 1, ACTION_MOVE_FAST = 2, ACTION_KEEP_SLOW = 3, NUM_ACTIONS = 4 };

        // State discretization
        enum AccessFreqBin { FREQ_VERY_LOW = 0, FREQ_LOW = 1, FREQ_MEDIUM = 2, FREQ_HIGH = 3, NUM_FREQ_BINS = 4 };
        enum StallBin { STALL_NONE = 0, STALL_LOW = 1, STALL_MEDIUM = 2, STALL_HIGH = 3, NUM_STALL_BINS = 4 };

        static constexpr size_t NUM_STATES = NUM_FREQ_BINS * NUM_STALL_BINS * 2;  // 2 for current location

        struct PageMetadata {
            uint64_t accessCount;
            uint64_t totalAccessCount;
            uint64_t numSwappedIn;
            uint64_t numSwappedOut;
            uint64_t robHeadStallCycles;
            uint64_t previousStallCycles;
            PageLocation currLoc;
            PageLocation previousLoc;

            // Q-learning state
            size_t lastState;
            Action lastAction;

            PageMetadata(uint64_t ac, uint64_t tac, uint64_t nsi, uint64_t nso, PageLocation cl)
                : accessCount(ac), totalAccessCount(tac), numSwappedIn(nsi), numSwappedOut(nso),
                  robHeadStallCycles(0), previousStallCycles(0), currLoc(cl), previousLoc(cl),
                  lastState(0), lastAction(ACTION_KEEP_SLOW) {}

            PageMetadata()
                : accessCount(0L), totalAccessCount(0L), numSwappedIn(0L), numSwappedOut(0L),
                  robHeadStallCycles(0), previousStallCycles(0), currLoc(SLOW_MEM), previousLoc(SLOW_MEM),
                  lastState(0), lastAction(ACTION_KEEP_SLOW) {}

            void accessPage() {
                accessCount++;
                totalAccessCount++;
            }

            void markRobHeadStall(uint64_t cycles) {
                robHeadStallCycles += cycles;
            }

            void swapPageIn() {
                numSwappedIn++;
                assert(currLoc == SLOW_MEM);
                previousLoc = currLoc;
                currLoc = FAST_MEM;
            }

            void swapPageOut() {
                numSwappedOut++;
                assert(currLoc == FAST_MEM);
                previousLoc = currLoc;
                currLoc = SLOW_MEM;
            }

            void prepareForNextInterval() {
                accessCount = 0L;
                previousStallCycles = robHeadStallCycles;
                robHeadStallCycles = 0L;
            }
        };

        size_t discretizeState(uint64_t accessCount, uint64_t stallCycles, PageLocation loc) const;
        AccessFreqBin discretizeAccessFreq(uint64_t accessCount) const;
        StallBin discretizeStall(uint64_t stallCycles) const;
        Action selectAction(size_t state);
        double computeReward(const PageMetadata& meta) const;
        void updateQValue(size_t state, Action action, double reward, size_t nextState);

        std::vector<uint64_t> getPagesSorted() const;

        g_unordered_map<uint64_t, PageMetadata> pageTracker;
        uint64_t numFastAllocs = 0;
        MTRand *rng = new MTRand(0xABCDE563F);

        // Q-learning parameters
        static constexpr double LEARNING_RATE = 0.1;      // Alpha
        static constexpr double DISCOUNT_FACTOR = 0.9;    // Gamma
        double epsilon = 0.3;                              // Exploration rate (decays over time)
        static constexpr double EPSILON_DECAY = 0.9995;
        static constexpr double MIN_EPSILON = 0.05;

        // Q-table: [state][action] -> Q-value
        std::array<std::array<double, NUM_ACTIONS>, NUM_STATES> qTable;

        uint64_t totalStallCyclesPrevious = 0;
        uint64_t episodeCount = 0;

        Counter pageTrackerAccesses, pageTrackerHits;
        Counter numPageMigrations, numSwappedIn, numSwappedOut, numHotPagesStayedIn;
        Counter totalReward;
        Counter explorationRate;
        std::mutex pageTrackerMutex;
};

#endif // QLEARNING_PAGE_MIGRATOR_H_
