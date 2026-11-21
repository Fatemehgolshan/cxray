#include "qlearning_page_migrator.h"
#include <functional>
#include <algorithm>
#include <cmath>

PageLocation QLearningPageMigrator::access(uint64_t pageAddress) {
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

QLearningPageMigrator::AccessFreqBin QLearningPageMigrator::discretizeAccessFreq(uint64_t accessCount) const {
    // Discretize access frequency into bins
    if (accessCount == 0) return FREQ_VERY_LOW;
    if (accessCount < 10) return FREQ_LOW;
    if (accessCount < 100) return FREQ_MEDIUM;
    return FREQ_HIGH;
}

QLearningPageMigrator::StallBin QLearningPageMigrator::discretizeStall(uint64_t stallCycles) const {
    // Discretize stall cycles into bins
    if (stallCycles == 0) return STALL_NONE;
    if (stallCycles < 1000) return STALL_LOW;
    if (stallCycles < 10000) return STALL_MEDIUM;
    return STALL_HIGH;
}

size_t QLearningPageMigrator::discretizeState(uint64_t accessCount, uint64_t stallCycles, PageLocation loc) const {
    AccessFreqBin freqBin = discretizeAccessFreq(accessCount);
    StallBin stallBin = discretizeStall(stallCycles);
    size_t locBin = (loc == FAST_MEM) ? 0 : 1;

    // Encode as: freqBin * (NUM_STALL_BINS * 2) + stallBin * 2 + locBin
    size_t state = freqBin * (NUM_STALL_BINS * 2) + stallBin * 2 + locBin;
    assert(state < NUM_STATES);
    return state;
}

QLearningPageMigrator::Action QLearningPageMigrator::selectAction(size_t state) {
    // Epsilon-greedy action selection
    if (rng->rand() < epsilon) {
        // Explore: random action
        return static_cast<Action>(rng->randInt(NUM_ACTIONS - 1));
    } else {
        // Exploit: best action based on Q-table
        Action bestAction = ACTION_KEEP_SLOW;
        double bestQ = qTable[state][ACTION_KEEP_SLOW];
        for (size_t a = 1; a < NUM_ACTIONS; ++a) {
            if (qTable[state][a] > bestQ) {
                bestQ = qTable[state][a];
                bestAction = static_cast<Action>(a);
            }
        }
        return bestAction;
    }
}

double QLearningPageMigrator::computeReward(const PageMetadata& meta) const {
    // Reward = reduction in stall cycles compared to previous interval
    double stallReduction = static_cast<double>(meta.previousStallCycles) -
                           static_cast<double>(meta.robHeadStallCycles);

    // Penalize migrations slightly to avoid thrashing
    double migrationPenalty = 0.0;
    if (meta.previousLoc != meta.currLoc) {
        migrationPenalty = 100.0;  // Small penalty for migration
    }

    // Normalize reward (positive for improvement, negative for degradation)
    double reward = stallReduction - migrationPenalty;
    return reward / 1000.0;  // Scale down
}

void QLearningPageMigrator::updateQValue(size_t state, Action action, double reward, size_t nextState) {
    // Q-learning update: Q(s,a) = Q(s,a) + alpha * (reward + gamma * max_a' Q(s',a') - Q(s,a))
    double maxNextQ = qTable[nextState][0];
    for (size_t a = 1; a < NUM_ACTIONS; ++a) {
        maxNextQ = std::max(maxNextQ, qTable[nextState][a]);
    }

    double tdTarget = reward + DISCOUNT_FACTOR * maxNextQ;
    double tdError = tdTarget - qTable[state][action];
    qTable[state][action] += LEARNING_RATE * tdError;
}

void QLearningPageMigrator::registerRobHeadLoadStall(uint64_t pageAddress, uint64_t cycles, uint32_t /*coreId*/) {
    if (cycles == 0) return;

    std::lock_guard<std::mutex> lock(pageTrackerMutex);
    auto it = pageTracker.find(pageAddress);
    if (it == pageTracker.end()) return;

    it->second.markRobHeadStall(cycles);
}

void QLearningPageMigrator::triggerMigration() {
    float dramOcc = 1.0 * numFastAllocs / numFastMemPages;

    // Calculate total reward from last interval
    uint64_t totalStallCyclesCurrent = 0;
    for (const auto& entry : pageTracker) {
        totalStallCyclesCurrent += entry.second.robHeadStallCycles;
    }

    int64_t globalReward = static_cast<int64_t>(totalStallCyclesPrevious) -
                          static_cast<int64_t>(totalStallCyclesCurrent);
    totalReward.inc(std::abs(globalReward) / 1000);

    info("Q-Learning Migration triggered: dramOcc=%f, epsilon=%f, globalReward=%ld, numFastAllocs=%ld",
            dramOcc, epsilon, globalReward, numFastAllocs);

    std::vector<uint64_t> allPagesSorted = this->getPagesSorted();
    uint64_t _numHotPagesStayedIn = 0;
    uint64_t _numPageMigrations = 0;
    uint64_t _numSwappedIn = 0;
    uint64_t _numSwappedOut = 0;

    const bool promote = !migrationTrigger || migrationTrigger->promoteDirection();

    // Q-learning based migration decisions
    for (auto& entry : pageTracker) {
        auto& meta = entry.second;

        // Get current state
        size_t currentState = discretizeState(meta.accessCount, meta.robHeadStallCycles, meta.currLoc);

        // Select action using Q-learning policy
        Action action = selectAction(currentState);

        // Apply action
        bool shouldPromote = false;
        bool shouldDemote = false;

        switch (action) {
            case ACTION_KEEP_FAST:
                // Try to keep in fast memory
                if (meta.currLoc == SLOW_MEM) shouldPromote = true;
                break;
            case ACTION_MOVE_SLOW:
                // Move to slow memory
                if (meta.currLoc == FAST_MEM) shouldDemote = true;
                break;
            case ACTION_MOVE_FAST:
                // Move to fast memory
                if (meta.currLoc == SLOW_MEM) shouldPromote = true;
                break;
            case ACTION_KEEP_SLOW:
                // Try to keep in slow memory
                if (meta.currLoc == FAST_MEM) shouldDemote = true;
                break;
            default:
                break;
        }

        // Execute action with budget constraints
        if (shouldPromote && promote && this->consumePromotionBudget()) {
            if (meta.currLoc == SLOW_MEM && numFastAllocs < numFastMemPages) {
                meta.swapPageIn();
                _numPageMigrations++;
                _numSwappedIn++;
                numFastAllocs++;
            }
        } else if (shouldDemote) {
            if (meta.currLoc == FAST_MEM) {
                if (promote || this->consumePromotionBudget()) {
                    meta.swapPageOut();
                    _numPageMigrations++;
                    _numSwappedOut++;
                    numFastAllocs--;
                }
            }
        } else if (meta.currLoc == FAST_MEM) {
            _numHotPagesStayedIn++;
        }

        // Compute reward and update Q-value
        double reward = computeReward(meta);
        size_t nextState = discretizeState(meta.accessCount, meta.robHeadStallCycles, meta.currLoc);
        updateQValue(meta.lastState, meta.lastAction, reward, nextState);

        // Update state for next iteration
        meta.lastState = currentState;
        meta.lastAction = action;

        // Prepare for next interval
        meta.prepareForNextInterval();
    }

    // Decay exploration rate
    epsilon = std::max(MIN_EPSILON, epsilon * EPSILON_DECAY);
    explorationRate.inc(static_cast<uint64_t>(epsilon * 1000));

    this->numHotPagesStayedIn.inc(_numHotPagesStayedIn);
    this->numPageMigrations.inc(_numPageMigrations);
    this->numSwappedIn.inc(_numSwappedIn);
    this->numSwappedOut.inc(_numSwappedOut);

    totalStallCyclesPrevious = totalStallCyclesCurrent;
    episodeCount++;

    info("Q-Learning Migration done: numPageMigrations=%ld, numSwappedIn=%ld, numSwappedOut=%ld, epsilon=%f",
            _numPageMigrations, _numSwappedIn, _numSwappedOut, epsilon);
}

std::vector<uint64_t> QLearningPageMigrator::getPagesSorted() const {
    using CIter = decltype(pageTracker)::const_iterator;

    std::vector<CIter> items;
    items.reserve(pageTracker.size());
    for (auto it = pageTracker.begin(); it != pageTracker.end(); ++it) {
        items.push_back(it);
    }

    // Sort by learned Q-value for staying in FAST memory
    auto better = [this](const CIter& a, const CIter& b) {
        const auto& A = a->second;
        const auto& B = b->second;

        // Get Q-value for keeping in FAST memory
        size_t stateA = discretizeState(A.accessCount, A.robHeadStallCycles, FAST_MEM);
        size_t stateB = discretizeState(B.accessCount, B.robHeadStallCycles, FAST_MEM);

        double qValueA = qTable[stateA][ACTION_KEEP_FAST];
        double qValueB = qTable[stateB][ACTION_KEEP_FAST];

        if (qValueA != qValueB)
            return qValueA > qValueB;
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
