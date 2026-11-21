#ifndef PAGE_MIGRATOR_H_
#define PAGE_MIGRATOR_H_
#include <cstdint>
#include <limits>
#include <vector>
#include "log.h"
#include "stats.h"
#include "migration_trigger.h"
#include <string>
#include <algorithm>
#include <stdexcept>
#include <mutex>
#include <atomic>
#include "g_std/g_string.h"
#include "g_std/g_unordered_map.h"

enum PlacementPolicy { FAST_FIRST, SLOW_FIRST, RANDOM };
enum MigrationPolicy {
    NONE, FREQ_BASED, PRACTICAL_FREQ_BASED, ROB_STALL_BASED,
    ROB_PRESSURE_WEIGHTED, CRITICALITY_WEIGHTED, QLEARNING,
    // --- New hybrid methods (see docs/migration_ideas.md) ---
    HYBRID_FSC,           // Frequency-Stall Composite
    HYBRID_SYS,           // Stall Yield Score
    HYBRID_SSE,           // Shadow Stall Estimation
    HYBRID_SPFD,          // Stall-Promote Frequency-Demote
    HYBRID_CEMA,          // Cross-Interval EMA
    HYBRID_CLCAS,         // CXL Link Congestion Amplified Score
    HYBRID_SBS,           // Stability-Boosted Score
    HYBRID_FSC_CEMA,      // FSC + CEMA
    HYBRID_FSC_SBS,       // FSC + SBS
    HYBRID_FSC_CEMA_SBS,  // FSC + CEMA + SBS  (Combination A)
    HYBRID_SSE_SPFD,      // SSE + SPFD
    HYBRID_SSE_SPFD_CLCAS,// SSE + SPFD + CLCAS  (Combination B)
    HYBRID_SYS_CEMA,      // SYS + CEMA
    // --- Count-based variants (stall event count instead of stall cycles) ---
    HYBRID_FSC_CNT,           // FSC + stall count
    HYBRID_FSC_CNT_SBS,       // FSC + stall count + SBS
    HYBRID_FSC_CNT_CEMA_SBS,  // FSC + stall count + CEMA + SBS (full count combo)
    HYBRID_SSE_SPFD_CNT,      // SSE + SPFD + stall count
    // --- New CXL-aware ideas ---
    HYBRID_SVA,                      // Stall Variance Aware
    HYBRID_GKD,                      // Global Knee Detection
    HYBRID_SCA,                      // Self-Competition Aware
    HYBRID_CCF,                      // Cross-Core Fan-out
    // FSC-Count + new ideas (primary combinations)
    HYBRID_FSC_CNT_SVA,              // fsc_cnt + variance
    HYBRID_FSC_CNT_CCF,              // fsc_cnt + core fan-out
    HYBRID_FSC_CNT_SVA_CCF,          // fsc_cnt + variance + core fan-out
    HYBRID_FSC_CNT_SVA_SBS,          // fsc_cnt + variance + stability
    HYBRID_FSC_CNT_SVA_CCF_SBS,      // fsc_cnt + variance + core + stability (primary paper combo)
    HYBRID_FSC_CNT_GKD_SCA,          // fsc_cnt + knee + suppression (link-level combo)
    HYBRID_FSC_CNT_SVA_CCF_GKD_SCA_SBS, // full kitchen sink
    // --- HPCA'27 revision case study: threshold + per-epoch cap + FIFO eviction ---
    FREQ_THRESHOLD,                  // promote pages with >= N accesses per epoch, <= cap pages/epoch, evict least recently promoted
};
enum PageLocation: int { FAST_MEM = 0, SLOW_MEM = 1};    // Two tiers
enum MigrationTriggerPolicy{ FIXED, AOL_BASED, LATENCY_BASED};
enum RobStallMetric { ROB_STALL_BINARY, ROB_STALL_CYCLES, ROB_STALL_COUNT };
inline g_string to_lower(const g_string& s) {
    g_string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return out;
}

inline g_string placementPolicyToStr(PlacementPolicy p) {
    switch (p) {
        case FAST_FIRST: return "fastFirst";
        case SLOW_FIRST: return "slowFirst";
        case RANDOM:     return "random";
    }

    panic("Unknown placement policy: %d", p);
}

inline PlacementPolicy strToPlacementPolicy(const g_string& s) {
    g_string lower = to_lower(s);
    if (lower == "fastfirst") return FAST_FIRST;
    if (lower == "slowfirst") return SLOW_FIRST;
    if (lower == "random")    return RANDOM;

    panic("Unknown placement policy: %s", s.c_str());
}

inline g_string migrationPolicyToStr(MigrationPolicy m) {
    switch (m) {
        case NONE:                  return "none";
        case FREQ_BASED:            return "freqBased";
        case PRACTICAL_FREQ_BASED:  return "practicalFreqBased";
        case ROB_STALL_BASED:       return "robStallBased";
        case ROB_PRESSURE_WEIGHTED: return "robPressureWeighted";
        case CRITICALITY_WEIGHTED:  return "criticalityWeighted";
        case QLEARNING:             return "qlearning";
        // Hybrid methods
        case HYBRID_FSC:            return "fsc";
        case HYBRID_SYS:            return "sys";
        case HYBRID_SSE:            return "sse";
        case HYBRID_SPFD:           return "spfd";
        case HYBRID_CEMA:           return "cema";
        case HYBRID_CLCAS:          return "clcas";
        case HYBRID_SBS:            return "sbs";
        case HYBRID_FSC_CEMA:       return "fsc+cema";
        case HYBRID_FSC_SBS:        return "fsc+sbs";
        case HYBRID_FSC_CEMA_SBS:   return "fsc+cema+sbs";
        case HYBRID_SSE_SPFD:       return "sse+spfd";
        case HYBRID_SSE_SPFD_CLCAS:     return "sse+spfd+clcas";
        case HYBRID_SYS_CEMA:           return "sys+cema";
        // Count-based variants
        case HYBRID_FSC_CNT:            return "fsc_cnt";
        case HYBRID_FSC_CNT_SBS:        return "fsc_cnt+sbs";
        case HYBRID_FSC_CNT_CEMA_SBS:   return "fsc_cnt+cema+sbs";
        case HYBRID_SSE_SPFD_CNT:       return "sse+spfd+cnt";
        // New CXL-aware ideas
        case HYBRID_SVA:                return "sva";
        case HYBRID_GKD:                return "gkd";
        case HYBRID_SCA:                return "sca";
        case HYBRID_CCF:                return "ccf";
        case HYBRID_FSC_CNT_SVA:        return "fsc_cnt+sva";
        case HYBRID_FSC_CNT_CCF:        return "fsc_cnt+ccf";
        case HYBRID_FSC_CNT_SVA_CCF:    return "fsc_cnt+sva+ccf";
        case HYBRID_FSC_CNT_SVA_SBS:    return "fsc_cnt+sva+sbs";
        case HYBRID_FSC_CNT_SVA_CCF_SBS:        return "fsc_cnt+sva+ccf+sbs";
        case HYBRID_FSC_CNT_GKD_SCA:            return "fsc_cnt+gkd+sca";
        case HYBRID_FSC_CNT_SVA_CCF_GKD_SCA_SBS: return "fsc_cnt+sva+ccf+gkd+sca+sbs";
        case FREQ_THRESHOLD:            return "freqThreshold";
    }

    panic("Unknown migration policy: %d", m);
}

inline MigrationPolicy strToMigrationPolicy(const g_string& s) {
    g_string lower = to_lower(s);
    if (lower == "none")                    return NONE;
    if (lower == "freqbased")               return FREQ_BASED;
    if (lower == "practicalfreqbased")      return PRACTICAL_FREQ_BASED;
    if (lower == "robstallbased")           return ROB_STALL_BASED;
    if (lower == "robpressureweighted")     return ROB_PRESSURE_WEIGHTED;
    if (lower == "criticalityweighted")     return CRITICALITY_WEIGHTED;
    if (lower == "qlearning")               return QLEARNING;
    // Hybrid methods
    if (lower == "fsc")                     return HYBRID_FSC;
    if (lower == "sys")                     return HYBRID_SYS;
    if (lower == "sse")                     return HYBRID_SSE;
    if (lower == "spfd")                    return HYBRID_SPFD;
    if (lower == "cema")                    return HYBRID_CEMA;
    if (lower == "clcas")                   return HYBRID_CLCAS;
    if (lower == "sbs")                     return HYBRID_SBS;
    if (lower == "fsc+cema")                return HYBRID_FSC_CEMA;
    if (lower == "fsc+sbs")                 return HYBRID_FSC_SBS;
    if (lower == "fsc+cema+sbs")            return HYBRID_FSC_CEMA_SBS;
    if (lower == "sse+spfd")                return HYBRID_SSE_SPFD;
    if (lower == "sse+spfd+clcas")          return HYBRID_SSE_SPFD_CLCAS;
    if (lower == "sys+cema")                return HYBRID_SYS_CEMA;
    // Count-based variants
    if (lower == "fsc_cnt")                 return HYBRID_FSC_CNT;
    if (lower == "fsc_cnt+sbs")             return HYBRID_FSC_CNT_SBS;
    if (lower == "fsc_cnt+cema+sbs")        return HYBRID_FSC_CNT_CEMA_SBS;
    if (lower == "sse+spfd+cnt")            return HYBRID_SSE_SPFD_CNT;
    // New CXL-aware ideas
    if (lower == "sva")                     return HYBRID_SVA;
    if (lower == "gkd")                     return HYBRID_GKD;
    if (lower == "sca")                     return HYBRID_SCA;
    if (lower == "ccf")                     return HYBRID_CCF;
    if (lower == "fsc_cnt+sva")             return HYBRID_FSC_CNT_SVA;
    if (lower == "fsc_cnt+ccf")             return HYBRID_FSC_CNT_CCF;
    if (lower == "fsc_cnt+sva+ccf")         return HYBRID_FSC_CNT_SVA_CCF;
    if (lower == "fsc_cnt+sva+sbs")         return HYBRID_FSC_CNT_SVA_SBS;
    if (lower == "fsc_cnt+sva+ccf+sbs")     return HYBRID_FSC_CNT_SVA_CCF_SBS;
    if (lower == "fsc_cnt+gkd+sca")         return HYBRID_FSC_CNT_GKD_SCA;
    if (lower == "fsc_cnt+sva+ccf+gkd+sca+sbs") return HYBRID_FSC_CNT_SVA_CCF_GKD_SCA_SBS;
    if (lower == "freqthreshold")           return FREQ_THRESHOLD;

    panic("Unknown migration policy: %s", s.c_str());
}

inline g_string migrationTriggerPolicyToStr(MigrationTriggerPolicy mt) {
    switch (mt) {
        case FIXED:              return "fixed";
        case AOL_BASED:        return "aolBased";
        case LATENCY_BASED:   return "latencyBased";
    }

    panic("Unknown migration trigger policy: %d", mt);
}

inline MigrationTriggerPolicy strToMigrationTriggerPolicy(const g_string& s) {
    g_string lower = to_lower(s);
    if (lower == "fixed")            return FIXED;
    if (lower == "aolbased")       return AOL_BASED;
    if (lower == "latencybased")   return LATENCY_BASED;

    panic("Unknown migration trigger policy: %s", s.c_str());
}

inline g_string pageLocationToStr(PageLocation loc) {
    switch (loc) {
        case FAST_MEM: return "fast";
        case SLOW_MEM: return "slow";
    }

    panic("Unknown page location: %d", loc);
}

inline PageLocation strToPageLocation(const g_string& s) {
    g_string lower = to_lower(s);
    if (lower == "fast") return FAST_MEM;
    if (lower == "slow") return SLOW_MEM;

    panic("Unknown page location: %s", s.c_str());
}

inline g_string robStallMetricToStr(RobStallMetric m) {
    switch (m) {
        case ROB_STALL_BINARY: return "robStallBinary";
        case ROB_STALL_CYCLES: return "robStallCycles";
        case ROB_STALL_COUNT:  return "robStallCount";
    }

    panic("Unknown ROB stall metric: %d", m);
}

inline RobStallMetric strToRobStallMetric(const g_string& s) {
    g_string lower = to_lower(s);
    if (lower == "robstallbinary") return ROB_STALL_BINARY;
    if (lower == "robstallcycles") return ROB_STALL_CYCLES;
    if (lower == "robstallcount")  return ROB_STALL_COUNT;

    panic("Unknown ROB stall metric: %s", s.c_str());
}

class PageMigrator {
    public:
        PageMigrator(const g_string& _name, PlacementPolicy _pp, uint64_t _numFastMemPages, uint64_t _numIntervalAccesses)
            : name(_name), placementPolicy(_pp), numFastMemPages(_numFastMemPages), numIntervalAccesses(_numIntervalAccesses) {
                assert(numIntervalAccesses > 0);
                assert(numFastMemPages > 0);
                info("Initialized PageMigrator: name: %s, numFastMemPages: %ld, numIntervalAccesses: %ld",
                        name.c_str(), numFastMemPages, numIntervalAccesses);
            }

        virtual ~PageMigrator() = default;

        // One page movement decided at an epoch boundary (promote: slow -> fast; else fast -> slow).
        // Migrators that support real migration traffic (TieringMemory sys.mem.migrationTraffic = "real")
        // publish every epoch's movements with publishMoves(); TieringMemory drains them with fetchMoves() and issues
        // the corresponding line requests.
        struct PageMove {
            uint64_t page;
            bool promote;
        };

        void initStats(AggregateStat* parentStat) {
            AggregateStat* migratorStats = new AggregateStat();
            migratorStats->init(name.c_str(), "PageMigrator stats");
            numAccesses.init("numAccesses", "Number of accesses"); migratorStats->append(&numAccesses);
            numMigrationTriggers.init("numMigrationTriggers", "Number of migration triggers"); migratorStats->append(&numMigrationTriggers);
            numFastMemAccesses.init("numFastMemAccesses", "Number of fast memory accesses"); migratorStats->append(&numFastMemAccesses);
            numSlowMemAccesses.init("numSlowMemAccesses", "Number of slow memory accesses"); migratorStats->append(&numSlowMemAccesses);

            parentStat->append(migratorStats);
        }

        const char* getName() { return name.c_str(); }

        virtual PageLocation access(uint64_t pageAddress) = 0;
        virtual void triggerMigration() {}
        virtual void registerRobHeadLoadStall(uint64_t pageAddress, uint64_t events, uint32_t coreId = 0) {}
        // Memory footprint query methods (pure virtual - each migrator implements based on its pageTracker)
        virtual uint64_t getTotalUniquePages() const = 0;
        virtual uint64_t getFastMemPages() const = 0;
        virtual uint64_t getSlowMemPages() const = 0;
        
        void registerAccess(PageLocation loc) {
            numAccesses.inc();

            assert(loc == FAST_MEM || loc == SLOW_MEM);
            if (loc == FAST_MEM) numFastMemAccesses.inc();
            if (loc == SLOW_MEM) numSlowMemAccesses.inc();

            if (numAccesses.get() % numIntervalAccesses == 0) {
                numMigrationTriggers.inc();
                numTriggersFired_++;
               if (migrationTrigger) {
                    migrationTrigger->onMigrationIntervalBoundary();
                    promotionBudget = migrationTrigger->computePromotionBudget(*this);
                } else {
                    promotionBudget = std::numeric_limits<uint64_t>::max(); // unlimited
                }
                // Per-epoch promotion cap in pages (sys.mem.migrationMaxPagesPerInterval; 0 = unlimited).
                if (maxPagesPerInterval_ > 0 && promotionBudget > maxPagesPerInterval_) promotionBudget = maxPagesPerInterval_;
                triggerMigration();
                // + reset for next window
                promotionBudget = std::numeric_limits<uint64_t>::max();
                if (migrationTrigger) migrationTrigger->resetInterval();
            }
        }
        void setMigrationTrigger(MigrationTrigger* t) { migrationTrigger = t; }
        uint64_t getNumFastMemPages() const { return numFastMemPages; }
        uint64_t getPromotionBudget() const { return promotionBudget; }
        bool consumePromotionBudget() {
            if (promotionBudget == std::numeric_limits<uint64_t>::max()) return true; // unlimited
            if (promotionBudget == 0) return false;
            --promotionBudget;
            return true;
        }
        // Migration overhead / cap accessors
        uint64_t getLastIntervalMigrations() const { return lastIntervalMigrations_; }
        uint64_t getNumTriggersFired() const { return numTriggersFired_; }
        uint64_t getNumIntervalAccesses() const { return numIntervalAccesses; }
        void setMigrationPageCap(uint64_t cap) { migrationPageCap_ = cap; }
        // Per-epoch promotion cap in pages (0 = unlimited); applied to promotionBudget at every trigger.
        void setMaxPagesPerInterval(uint64_t cap) { maxPagesPerInterval_ = cap; }
        uint64_t getMaxPagesPerInterval() const { return maxPagesPerInterval_; }
        // Movement log (see PageMove). Enabled by TieringMemory when migration traffic is real.
        // A migrator publishes the whole batch of an epoch boundary at once (publishMoves), so a
        // consumer sees either nothing or a complete batch; hasPendingMoves() is a lock-free check.
        void enableMoveLog(bool en) { moveLogEnabled_ = en; }
        bool moveLogEnabled() const { return moveLogEnabled_; }
        bool hasPendingMoves() const { return movesPending_.load(std::memory_order_acquire); }
        void fetchMoves(std::vector<PageMove>& out) {
            std::lock_guard<std::mutex> g(moveLogLock_);
            out.swap(moveLog_);
            moveLog_.clear();
            movesPending_.store(false, std::memory_order_release);
        }
    protected:
        void publishMoves(std::vector<PageMove>& batch) {
            if (!moveLogEnabled_ || batch.empty()) return;
            std::lock_guard<std::mutex> g(moveLogLock_);
            moveLog_.insert(moveLog_.end(), batch.begin(), batch.end());
            movesPending_.store(true, std::memory_order_release);
        }
        const g_string name;
        const PlacementPolicy placementPolicy;
        const uint64_t numFastMemPages;
        const uint64_t numIntervalAccesses;
        MigrationTrigger* migrationTrigger = nullptr;
        uint64_t promotionBudget = std::numeric_limits<uint64_t>::max();
        // Migration overhead tracking (set by triggerMigration() in derived classes)
        uint64_t lastIntervalMigrations_ = 0;
        // Cap on total pages migrated per interval (promotions + demotions)
        uint64_t migrationPageCap_ = std::numeric_limits<uint64_t>::max();
        // Per-epoch promotion cap (pages; 0 = unlimited) and the movement log
        uint64_t maxPagesPerInterval_ = 0;
        bool moveLogEnabled_ = false;
        std::mutex moveLogLock_;
        std::vector<PageMove> moveLog_;
        std::atomic<bool> movesPending_{false};
    private:
        Counter numAccesses, numMigrationTriggers;
        Counter numFastMemAccesses, numSlowMemAccesses;
        uint64_t numTriggersFired_ = 0;  // incremented each time triggerMigration fires
};

#endif // PAGE_MIGRATOR_H_
