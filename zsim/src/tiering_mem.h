#ifndef TIERING_MEM_H_
#define TIERING_MEM_H_

#include "g_std/g_unordered_map.h"
#include <atomic>
#include <unordered_map>
#include <utility>
#include <vector>
#include <unordered_set>
#include <mutex>
#include "ddr_mem.h"
#include "config.h"
#include "page_migrator.h"
#include "none_page_migrator.h"
#include "freqbased_page_migrator.h"
#include "practicalfreqbased_page_migrator.h"
#include "robstallbased_page_migrator.h"
#include "robpressure_weighted_page_migrator.h"
#include "criticality_weighted_page_migrator.h"
#include "qlearning_page_migrator.h"
#include "hybrid_page_migrator.h"
#include "migration_trigger.h"
#include "fixedbased_page_migrator.h"
#include "aol_based_migration_trigger.h"
#include "latency_based_migration_trigger.h"
#include "cxl_mem_zsim.h"
#include "mtrand.h"
#include <deque>

// ----------------------------------------------------------------------------
// FreqThresholdPageMigrator — the frequency-based promotion policy of the
// HPCA'27 revision case study (Sec. VII-B), selected with
// sys.mem.migrationPolicy = "freqThreshold".
//
//   * Accesses are counted per page during an epoch (epoch = numIntervalAccesses
//     accesses to the tiering memory; every request type counts, as in the other
//     migrators).
//   * At the epoch boundary, slow-tier pages with >= hotThreshold accesses in the
//     epoch are candidates, sorted by access count (descending, address ascending).
//     They are promoted in that order while the promotion budget lasts; the budget
//     is the trigger's budget capped by sys.mem.migrationMaxPagesPerInterval
//     (PageMigrator::registerAccess), so at most that many pages move per epoch.
//   * When the fast tier is full, each promotion evicts the least recently
//     promoted fast-tier page (FIFO over arrival in the fast tier; first-touch
//     residents placed by "fastFirst" count as arrivals at time 0).  The evicted
//     page is demoted to the slow tier.
//   * Per-epoch counts are reset at the boundary.  No other demotions happen.
//   * Every promotion/demotion of an epoch is published through PageMigrator::publishMoves() so that
//     TieringMemory can issue the real line traffic (sys.mem.migrationTraffic = "real"); the batch is
//     published atomically at the end of triggerMigration() (PageMigrator::publishMoves).
// ----------------------------------------------------------------------------
class FreqThresholdPageMigrator : public PageMigrator {
    public:
        FreqThresholdPageMigrator(const g_string& _name, PlacementPolicy _pp, uint64_t _numFastMemPages,
                                  uint64_t _numIntervalAccesses, uint64_t _hotThreshold);
        ~FreqThresholdPageMigrator() override { delete rng_; }

        void initStats(AggregateStat* parentStat);   // hides PageMigrator::initStats (non-virtual); called through the typed pointer

        uint64_t getTotalUniquePages() const override { return pages_.size(); }
        uint64_t getFastMemPages() const override { return numFastResident_; }
        uint64_t getSlowMemPages() const override { return pages_.size() - numFastResident_; }

        PageLocation access(uint64_t pageAddress) override;
        void triggerMigration() override;

        uint64_t getHotThreshold() const { return hotThreshold_; }

    private:
        struct PageInfo {
            uint32_t epochCount;    // accesses in the current epoch
            PageLocation loc;
            uint64_t fifoToken;     // arrival token in the fast tier (0 = not resident)
        };
        g_unordered_map<uint64_t, PageInfo> pages_;
        std::vector<uint64_t> touched_;                          // pages with epochCount > 0 this epoch
        std::deque<std::pair<uint64_t, uint64_t>> fastFifo_;     // (page, token), oldest first
        uint64_t nextToken_ = 1;
        uint64_t numFastResident_ = 0;
        const uint64_t hotThreshold_;
        MTRand* rng_ = new MTRand(0xABCDE563F);
        std::mutex lock_;

        Counter numEpochs_, numPromotionEpochs_, numPromotions_, numDemotions_;
        Counter numHotCandidates_, numBudgetLimitedEpochs_, lastEpochPromotions_, lastEpochHotCandidates_;
};

class TieringMemory : public MemObject {
    public:
        virtual ~TieringMemory();
        TieringMemory(Config& _config, uint32_t _lineSize, uint32_t _sysFreqMHz, uint32_t _domain, g_string& _name);

        void initStats(AggregateStat* parentStat);
        const char* getName() {return name.c_str();}

        // Bound phase interface
        uint64_t access(MemReq& req);
        double computeFastMemoryLeadOffLatency() const;
        void exportPageStallCDF(const std::string& path) const;
        void exportPageAccessCDF(const std::string& path) const;
        void exportCrossCDFs() const;
    private:
        void initPageMigrator(Config& config, PlacementPolicy pp, MigrationPolicy mp, MigrationTriggerPolicy mtp, uint64_t migrationIntervalAccesses, uint64_t dramSizeBytes);
        void recordRobHeadStall(Address lineAddr, AccessType type, uint64_t events);
        // Returns the lead-off latency of the slowest (CXL) tier in cycles, or 0 if unknown.
        // Used to auto-calibrate cxlBaselineLatency when not set explicitly in the config.
        double computeSlowMemoryLeadOffLatency() const;
        const uint32_t lineSize;
        const uint64_t pageSize;
        const uint32_t domain;
        const g_string name;

        PageMigrator *pageMigrator = nullptr;
        FreqThresholdPageMigrator* freqThresholdMigrator_ = nullptr;   // typed alias of pageMigrator when policy = freqThreshold
        MigrationTrigger* migrationTrigger = nullptr;
        g_vector<MemObject*> ddrMems;
        g_vector<MemObject*> cxlMems;
        g_vector<CXLMemory*> cxlCtrls;   // parallel to cxlMems; non-null when the slow tier child is type = "CXL"

        // ----------------------------------------------------------------
        // Real migration traffic (sys.mem.migrationTraffic = "real"; default "penalty"
        // keeps the per-access penalty model below).  With "real", every page
        // movement decided at an epoch boundary issues 64 line reads to the source
        // tier and 64 line writes to the destination tier through the tiers'
        // access() paths, tagged CXLMemory::FLAG_MIGRATION, back to back at the
        // cycle of the access that observed the boundary, and migrationCostCyclesPerPage
        // is forced to 0.  The requests use a phantom source id whose event recorder
        // is null, so they take the bound-phase path of the DDR backend (no weave
        // events) and never disturb the demand request's timing record.
        // ----------------------------------------------------------------
        bool     migrationTrafficReal_        = false;
        uint64_t migrationMaxPagesPerInterval_ = 0;   // 0 = unlimited (sys.mem.migrationMaxPagesPerInterval)
        uint64_t migrationHotThreshold_        = 8;   // freqThreshold policy (sys.mem.migrationHotThreshold)
        uint32_t migSrcId_                     = 0;   // phantom srcId (== numCores) used by migration requests
        void issueMigrationTraffic(const std::vector<PageMigrator::PageMove>& moves, uint64_t cycle);
        Counter numMigBatches_, numMigPromotedPages_, numMigDemotedPages_;
        Counter numMigLineReadsCxl_, numMigLineWritesCxl_, numMigLineReadsDdr_, numMigLineWritesDdr_;

        uint32_t numDDRMems, numCXLMems;
        uint32_t cxlFixedLatencyOffset;  // Additional fixed latency for CXL (e.g., link traversal)

        // ROB stall threshold: stalls shorter than this (in cycles) are ignored.
        // 0 = disabled (all stalls recorded, original behaviour).
        uint64_t robStallMinThresholdCycles = 0;

        // Migration overhead modeling:
        // Each migrated 4KB page reads from CXL, consuming link bandwidth.
        // We model this as an additive latency penalty on subsequent CXL accesses.
        uint64_t migrationCostCyclesPerPage_ = 200;  // cycles added to CXL per page migrated
        uint64_t migrationPenaltyPerAccess_  = 0;    // computed after each trigger; applied to CXL
        uint64_t prevNumTriggersFired_       = 0;    // tracks trigger count to detect new triggers

        // ----------------------------------------------------------------
        // Model A: CXL link bandwidth congestion (non-linear latency curve)
        //
        // The CXL link has a finite bandwidth. Past the "knee" (~70% util),
        // queuing delay rises super-linearly with utilization.
        // We use the CXL access ratio (cxlAccesses / totalAccesses) as a
        // proxy for link utilization, and add a quadratic penalty past the knee.
        //
        // Config knobs (sys.mem.*):
        //   cxlBwCongestionEnabled   – enable this model (default: false)
        //   cxlKneeThreshold         – CXL access ratio at knee (default: 0.7)
        //   cxlPeakCongestionLatency – extra cycles at 100% CXL ratio (default: 1000)
        // ----------------------------------------------------------------
        bool     cxlBwCongestionEnabled_     = false;
        double   cxlKneeThreshold_           = 0.7;
        uint64_t cxlPeakCongestionLatency_   = 1000;  // cycles at full saturation
        uint64_t cxlBwCongestionPenalty_     = 0;     // applied to every CXL access; recomputed each interval

        // ----------------------------------------------------------------
        // Model B: CXL head-of-line (HoL) blocking (serialized shared link)
        //
        // All cores share a single CXL link. When N distinct cores access CXL
        // in the same interval, requests queue behind each other, adding
        // serialization latency proportional to the number of concurrent cores.
        //
        // Config knobs (sys.mem.*):
        //   cxlHoLBlockingEnabled  – enable this model (default: false)
        //   cxlHoLCyclesPerCore    – extra cycles per additional active core (default: 50)
        // ----------------------------------------------------------------
        bool     cxlHoLBlockingEnabled_      = false;
        uint64_t cxlHoLCyclesPerCore_        = 50;    // extra cycles per additional core beyond first
        uint64_t cxlHoLPenalty_              = 0;     // applied to every CXL access; recomputed each interval

        // Per-interval tracking (reset at each migration trigger)
        std::atomic<uint64_t> cxlIntervalAccessCount_{0};  // CXL accesses since last trigger
        std::atomic<uint64_t> cxlActiveCoresMask_{0};      // bitmask of cores that hit CXL this interval

        Counter numTotolAccesses;
        Counter numDDRAccesses, numCXLAccesses;
        Counter numRobHeadStallEvents;
        Counter totalMigrationOverheadCycles_;   // total penalty cycles from migration overhead
        Counter totalCxlBwCongestionCycles_;     // total penalty cycles from Model A
        Counter totalCxlHoLBlockingCycles_;      // total penalty cycles from Model B
        
        struct RobStallAggregate {
            uint64_t totalCycles = 0;
            uint64_t loadCycles = 0;
            uint64_t storeCycles = 0;
            uint64_t totalEvents = 0;
            uint64_t loadEvents = 0;
            uint64_t storeEvents = 0;
        };
        static void accumulateRobStallAggregate(RobStallAggregate& agg, AccessType type, uint64_t events);

        mutable std::mutex robStallMutex;
        g_unordered_map<uint64_t, RobStallAggregate> robStallByPage;
        uint64_t totalRobStallCycles = 0;
        std::unordered_map<uint64_t, uint64_t> accessCountByPage;
        uint64_t totalAccessCount = 0;
};

#endif // TIERING_MEM_H_
