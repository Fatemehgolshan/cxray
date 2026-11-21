#include "tiering_mem.h"
#include <algorithm>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>
#include "log.h"
#include "zsim.h"
#include "weave_mess_mem_ctrl.h"
#include "ddr_mem.h"

TieringMemory::~TieringMemory() {
    
//    exportPageStallCDF("rob_page_stall_cdf.csv");
//    exportPageAccessCDF("page_access_cdf.csv");
    //exportRobAccessMismatch();
    for (MemObject* mem : ddrMems) {
        delete mem;
    }

    for (MemObject* mem : cxlMems) {
        delete mem;
    }
    delete pageMigrator;
    pageMigrator = nullptr;
}

TieringMemory::TieringMemory(Config& _config, uint32_t _lineSize, uint32_t _sysFreqMHz, uint32_t _domain, g_string& _name)
    : lineSize(_lineSize),
      pageSize(_config.get<uint64_t>("sys.mem.pageSize", 4*1024ULL)),
      domain(_domain),
      name(_name) {
        std::string ddr_prefix = "sys.mem.DDR";
        bool ddr_exists = _config.exists(ddr_prefix);

        if (ddr_exists) {
            ddr_prefix += ".";
            uint32_t num_ddr_controllers = _config.get<uint32_t>(ddr_prefix + "controllers", 1);
            this->ddrMems.resize(num_ddr_controllers);

            std::string type = _config.get<const char*>(ddr_prefix + "type", "DDR");

            for (uint32_t c = 0; c < num_ddr_controllers; c++) {
                std::stringstream ss;
                ss << _name << ".DDR" << c;
                g_string mem_name(ss.str().c_str(), StlGlobAlloc<char>());

                if (type == "DDR") {
                    uint32_t ranksPerChannel = _config.get<uint32_t>(ddr_prefix + "ranksPerChannel", 4);
                    uint32_t banksPerRank = _config.get<uint32_t>(ddr_prefix + "banksPerRank", 8);
                    uint32_t pageSize = _config.get<uint32_t>(ddr_prefix + "pageSize", 8192);
                    const char* tech = _config.get<const char*>(ddr_prefix + "tech", "DDR5-2666-CL20");
                    const char* addrMapping = _config.get<const char*>(ddr_prefix + "addrMapping", "rank:col:bank");
                    bool deferWrites = _config.get<bool>(ddr_prefix + "deferWrites", true);
                    bool closedPage = _config.get<bool>(ddr_prefix + "closedPage", true);
                    uint32_t maxRowHits = _config.get<uint32_t>(ddr_prefix + "maxRowHits", 4);
                    uint32_t queueDepth = _config.get<uint32_t>(ddr_prefix + "queueDepth", 16);
                    uint32_t controllerLatency = _config.get<uint32_t>(ddr_prefix + "controllerLatency", 10);

                    this->ddrMems[c] = new DDRMemory(_lineSize, pageSize, ranksPerChannel, banksPerRank,
                                                     _sysFreqMHz, tech, addrMapping, controllerLatency,
                                                     queueDepth, maxRowHits, deferWrites, closedPage,
                                                     _domain, mem_name);
                    info("Created DDR module %d: type=%s, tech=%s, ranks=%d, banks=%d",
                         c, type.c_str(), tech, ranksPerChannel, banksPerRank);

                } else if (type == "WeaveMessMemCtrl") {
                    const char* curveAddressCStr = _config.get<const char*>(ddr_prefix + "curveAddress");
                    g_string curveAddress(curveAddressCStr, StlGlobAlloc<char>());
                    uint32_t curveWindowSize = _config.get<uint32_t>(ddr_prefix + "curveWindowSize", 1000);
                    this->ddrMems[c] = new WeaveMessMemCtrl(curveAddress, curveWindowSize, _sysFreqMHz, _domain, mem_name);
                    info("Created DDR module %d: type=%s, curveAddress=%s, curveWindowSize=%d", c, type.c_str(), curveAddressCStr, curveWindowSize);

                } else {
                    panic("Undefined memory DDR type=%s", type.c_str());
                }
            }

            this->numDDRMems = num_ddr_controllers;
        } else {
            this->numDDRMems = 0;
        }

        std::string cxl_prefix = "sys.mem.CXL";
        bool cxl_exists = _config.exists(cxl_prefix);

        if (cxl_exists) {
            cxl_prefix += ".";
            uint32_t num_cxl_controllers = _config.get<uint32_t>(cxl_prefix + "controllers", 1);
            this->cxlMems.resize(num_cxl_controllers);

            std::string type = _config.get<const char*>(cxl_prefix + "type", "DDR");

            // Read fixed latency offset for CXL (e.g., link traversal latency).
            // A type = "CXL" child models the link itself (CXLMemory), so its default offset is 0.
            this->cxlFixedLatencyOffset = _config.get<uint32_t>(cxl_prefix + "fixedLatencyOffset",
                                                                (type == "CXL") ? 0 : 150);    // ~60ns

            this->cxlCtrls.resize(num_cxl_controllers);
            for (uint32_t c = 0; c < num_cxl_controllers; c++) {
                std::stringstream ss;
                ss << _name << ".CXL" << c;
                g_string mem_name(ss.str().c_str(), StlGlobAlloc<char>());
                this->cxlCtrls[c] = nullptr;

                if (type == "CXL") {
                    // CXRay zsim CXL model: DDRMemory backend (DDR knobs of the CXL sub-block) wrapped by
                    // CXLMemory (CXL knobs of the CXL sub-block; same names and defaults as sys.mem.* in init.cpp).
                    uint32_t ranksPerChannel = _config.get<uint32_t>(cxl_prefix + "ranksPerChannel", 4);
                    uint32_t banksPerRank = _config.get<uint32_t>(cxl_prefix + "banksPerRank", 8);
                    uint32_t pageSize = _config.get<uint32_t>(cxl_prefix + "pageSize", 8192);
                    const char* tech = _config.get<const char*>(cxl_prefix + "tech", "DDR5-2666-CL20");
                    const char* addrMapping = _config.get<const char*>(cxl_prefix + "addrMapping", "rank:col:bank");
                    bool deferWrites = _config.get<bool>(cxl_prefix + "deferWrites", true);
                    bool closedPage = _config.get<bool>(cxl_prefix + "closedPage", true);
                    uint32_t maxRowHits = _config.get<uint32_t>(cxl_prefix + "maxRowHits", 4);
                    uint32_t queueDepth = _config.get<uint32_t>(cxl_prefix + "queueDepth", 16);
                    uint32_t controllerLatency = _config.get<uint32_t>(cxl_prefix + "controllerLatency", 10);
                    DDRMemory* backend = new DDRMemory(_lineSize, pageSize, ranksPerChannel, banksPerRank,
                                                       _sysFreqMHz, tech, addrMapping, controllerLatency,
                                                       queueDepth, maxRowHits, deferWrites, closedPage,
                                                       _domain, mem_name);
                    CXLMemory::Config cxlCfg;
                    cxlCfg.hostOverheadNs = _config.get<uint32_t>(cxl_prefix + "hostOverheadNs", 97);
                    cxlCfg.linkOneWayNs   = _config.get<uint32_t>(cxl_prefix + "linkOneWayNs",   24);
                    cxlCfg.creditLimitReq = _config.get<uint32_t>(cxl_prefix + "creditLimitReq", 32);
                    cxlCfg.creditLimitRwd = _config.get<uint32_t>(cxl_prefix + "creditLimitRwd", 16);
                    cxlCfg.rttNs          = _config.get<uint32_t>(cxl_prefix + "rttNs",           0);
                    cxlCfg.hasSwitch      = _config.get<bool>    (cxl_prefix + "hasSwitch",    false);
                    cxlCfg.switchLatNs    = _config.get<uint32_t>(cxl_prefix + "switchLatNs",    50);
                    cxlCfg.switchHops     = _config.get<uint32_t>(cxl_prefix + "switchHops",      1);
                    cxlCfg.numDevices     = _config.get<uint32_t>(cxl_prefix + "numDevices",      1);
                    cxlCfg.fabricLatNs    = _config.get<uint32_t>(cxl_prefix + "fabricLatNs",     0);
                    cxlCfg.deviceType     = _config.get<uint32_t>(cxl_prefix + "deviceType",      3);
                    cxlCfg.coherenceNs    = _config.get<uint32_t>(cxl_prefix + "coherenceNs",     0);
                    cxlCfg.biRttNs        = _config.get<uint32_t>(cxl_prefix + "biRttNs",         0);
                    cxlCfg.domain         = _domain;
                    CXLMemory* ctrl = new CXLMemory(backend, cxlCfg, _sysFreqMHz, mem_name);
                    this->cxlMems[c] = ctrl;
                    this->cxlCtrls[c] = ctrl;
                    info("Created CXL module %d: type=CXL (CXRay zsim model), tech=%s, ranks=%d, banks=%d, hostOverheadNs=%d linkOneWayNs=%d creditLimitReq=%d creditLimitRwd=%d",
                         c, tech, ranksPerChannel, banksPerRank, cxlCfg.hostOverheadNs, cxlCfg.linkOneWayNs, cxlCfg.creditLimitReq, cxlCfg.creditLimitRwd);

                } else if (type == "DDR") {
                    uint32_t ranksPerChannel = _config.get<uint32_t>(cxl_prefix + "ranksPerChannel", 4);
                    uint32_t banksPerRank = _config.get<uint32_t>(cxl_prefix + "banksPerRank", 8);
                    uint32_t pageSize = _config.get<uint32_t>(cxl_prefix + "pageSize", 8192);
                    const char* tech = _config.get<const char*>(cxl_prefix + "tech", "DDR5-2666-CL20");
                    const char* addrMapping = _config.get<const char*>(cxl_prefix + "addrMapping", "rank:col:bank");
                    bool deferWrites = _config.get<bool>(cxl_prefix + "deferWrites", true);
                    bool closedPage = _config.get<bool>(cxl_prefix + "closedPage", true);
                    uint32_t maxRowHits = _config.get<uint32_t>(cxl_prefix + "maxRowHits", 4);
                    uint32_t queueDepth = _config.get<uint32_t>(cxl_prefix + "queueDepth", 16);
                    uint32_t controllerLatency = _config.get<uint32_t>(cxl_prefix + "controllerLatency", 10);

                    this->cxlMems[c] = new DDRMemory(_lineSize, pageSize, ranksPerChannel, banksPerRank,
                                                     _sysFreqMHz, tech, addrMapping, controllerLatency,
                                                     queueDepth, maxRowHits, deferWrites, closedPage,
                                                     _domain, mem_name);
                    info("Created CXL module %d: type=%s, tech=%s, ranks=%d, banks=%d",
                         c, type.c_str(), tech, ranksPerChannel, banksPerRank);

                } else if (type == "WeaveMessMemCtrl") {
                    const char* curveAddressCStr = _config.get<const char*>(cxl_prefix + "curveAddress", "");
                    g_string curveAddress(curveAddressCStr, StlGlobAlloc<char>());
                    uint32_t curveWindowSize = _config.get<uint32_t>(cxl_prefix + "curveWindowSize", 1000);
                    this->cxlMems[c] = new WeaveMessMemCtrl(curveAddress, curveWindowSize, _sysFreqMHz, _domain, mem_name);
                    info("Created CXL module %d: type=%s, curveAddress=%s, curveWindowSize=%d", c, type.c_str(), curveAddressCStr, curveWindowSize);

                } else {
                    panic("Undefined memory CXL type=%s", type.c_str());
                }
            }

            this->numCXLMems = num_cxl_controllers;
            if (this->cxlFixedLatencyOffset > 0) {
                info("CXL fixed latency offset: %d cycles", this->cxlFixedLatencyOffset);
            }
        } else {
            panic("Num CXL mems cannot be 0");
            this->numCXLMems = 0;
            this->cxlFixedLatencyOffset = 0;
        }

        PlacementPolicy pp = strToPlacementPolicy(_config.get<const char*>("sys.mem.placementPolicy", "fastFirst"));
        MigrationPolicy mp = strToMigrationPolicy(_config.get<const char*>("sys.mem.migrationPolicy", "none"));
        MigrationTriggerPolicy mtp = strToMigrationTriggerPolicy(_config.get<const char*>("sys.mem.migrationTriggerPolicy", "fixed"));
        uint64_t migrationIntervalAccesses = _config.get<uint64_t>("sys.mem.migrationIntervalAccesses", 100000000L);
        // CXL memory (slow) is infinite in size, but DRAM (fast) is limited
        int dramSizeProvided = 0;
        for (const std::string key : {
                "sys.mem.DRAMSizeBytes",
                "sys.mem.DRAMSizeKB",
                "sys.mem.DRAMSizeMB",
                "sys.mem.DRAMSizeGB"
                }) {
            dramSizeProvided += _config.exists(key);
        }

        assert_msg(dramSizeProvided == 1, "More than one DRAM size is provided: %d sizes in the config file", dramSizeProvided);
        uint64_t dramSizeBytes = _config.get<uint64_t>("sys.mem.DRAMSizeBytes", 0);
        if (dramSizeBytes == 0) dramSizeBytes = _config.get<uint64_t>("sys.mem.DRAMSizeKB", 0) * 1024;
        if (dramSizeBytes == 0) dramSizeBytes = _config.get<uint64_t>("sys.mem.DRAMSizeMB", 0) * 1024 * 1024;
        if (dramSizeBytes == 0) dramSizeBytes = _config.get<uint64_t>("sys.mem.DRAMSizeGB", 0) * 1024 * 1024 * 1024;
        assert(dramSizeBytes != 0);

        this->robStallMinThresholdCycles = _config.get<uint64_t>("sys.mem.robStallMinThresholdCycles", 0);
        if (this->robStallMinThresholdCycles > 0) {
            info("ROB stall threshold: %ld cycles (stalls shorter than this are ignored)",
                 robStallMinThresholdCycles);
        }

        // Model A: CXL link bandwidth congestion
        this->cxlBwCongestionEnabled_ = _config.get<bool>("sys.mem.cxlBwCongestionEnabled", false);
        if (this->cxlBwCongestionEnabled_) {
            this->cxlKneeThreshold_        = _config.get<double>("sys.mem.cxlKneeThreshold", 0.7);
            this->cxlPeakCongestionLatency_= _config.get<uint64_t>("sys.mem.cxlPeakCongestionLatency", 1000);
            info("CXL BW congestion model ENABLED: kneeThreshold=%.2f peakLatency=%ld cycles",
                 cxlKneeThreshold_, cxlPeakCongestionLatency_);
        }

        // Model B: CXL head-of-line blocking
        this->cxlHoLBlockingEnabled_ = _config.get<bool>("sys.mem.cxlHoLBlockingEnabled", false);
        if (this->cxlHoLBlockingEnabled_) {
            this->cxlHoLCyclesPerCore_ = _config.get<uint64_t>("sys.mem.cxlHoLCyclesPerCore", 50);
            info("CXL HoL blocking model ENABLED: cyclesPerCore=%ld", cxlHoLCyclesPerCore_);
        }

        // Migration traffic model: "penalty" (default, per-access penalty) or "real" (line requests).
        std::string trafficMode = _config.get<const char*>("sys.mem.migrationTraffic", "penalty");
        if (trafficMode == "real") {
            this->migrationTrafficReal_ = true;
        } else if (trafficMode != "penalty") {
            panic("Invalid sys.mem.migrationTraffic = %s (expected \"penalty\" or \"real\")", trafficMode.c_str());
        }
        this->migrationMaxPagesPerInterval_ = _config.get<uint64_t>("sys.mem.migrationMaxPagesPerInterval", 0);
        this->migrationHotThreshold_        = _config.get<uint64_t>("sys.mem.migrationHotThreshold", 8);
        if (this->migrationTrafficReal_) {
            // Phantom source id for migration requests: extend the CID -> EventRecorder array by one
            // null slot.  Memories are built before cores (InitSystem), so the array is still empty.
            uint32_t n = zinfo->numCores;
            EventRecorder** recs = gm_calloc<EventRecorder*>(n + 1);
            for (uint32_t i = 0; i < n; i++) recs[i] = zinfo->eventRecorders[i];
            recs[n] = nullptr;
            zinfo->eventRecorders = recs;
            this->migSrcId_ = n;
            info("Migration traffic model: real (64 line reads + 64 line writes per page, phantom srcId %d, maxPagesPerInterval=%ld)",
                 this->migSrcId_, this->migrationMaxPagesPerInterval_);
        }

        initPageMigrator(_config, pp, mp, mtp, migrationIntervalAccesses, dramSizeBytes);

        auto prettySize = [](uint64_t bytes) {
            const char* units[] = {"B", "KB", "MB", "GB", "TB"};
            double size = static_cast<double>(bytes);
            int unit = 0;
            while (size >= 1024 && unit < 4) {
                size /= 1024.0;
                ++unit;
            }
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.2f %s", size, units[unit]);
            return std::string(buf);
        };

        info("Initialized tiering memory system with %d DDR mems, %d CXL mems, DDR size: %s, pageSize: %ld, lineSize: %d",
                this->numDDRMems, this->numCXLMems, prettySize(dramSizeBytes).c_str(), this->pageSize, this->lineSize);
    }


void TieringMemory::initStats(AggregateStat* parentStat) {
    // Root container for all TieringMemory stats
    AggregateStat* memStats = new AggregateStat();
    memStats->init(name.c_str(), "Tiering memory stats");

    // ---- Basic counters ----
    numTotolAccesses.init("accesses", "Number of total accesses"); 
    memStats->append(&numTotolAccesses);

    numDDRAccesses.init("ddrAccesses", "Number of total DDR accesses"); 
    memStats->append(&numDDRAccesses);

    numCXLAccesses.init("cxlAccesses", "Number of total CXL accesses"); 
    memStats->append(&numCXLAccesses);

    numRobHeadStallEvents.init("robHeadStallCycles", "Total ROB head stall cycles observed by tiering memory");
    memStats->append(&numRobHeadStallEvents);
    totalMigrationOverheadCycles_.init("migrationOverheadCycles",
        "Total penalty cycles added to CXL accesses due to migration bandwidth contention");
    memStats->append(&totalMigrationOverheadCycles_);

    totalCxlBwCongestionCycles_.init("cxlBwCongestionCycles",
        "Total penalty cycles added to CXL accesses due to link bandwidth congestion (Model A)");
    memStats->append(&totalCxlBwCongestionCycles_);

    totalCxlHoLBlockingCycles_.init("cxlHoLBlockingCycles",
        "Total penalty cycles added to CXL accesses due to head-of-line blocking (Model B)");
    memStats->append(&totalCxlHoLBlockingCycles_);

    // ---- Real migration traffic (sys.mem.migrationTraffic = "real") ----
    numMigBatches_.init("migBatches", "Epoch boundaries at which >= 1 page moved (real migration traffic)"); memStats->append(&numMigBatches_);
    numMigPromotedPages_.init("migPromotedPages", "Pages promoted slow -> fast (real migration traffic)"); memStats->append(&numMigPromotedPages_);
    numMigDemotedPages_.init("migDemotedPages", "Pages demoted fast -> slow (real migration traffic)"); memStats->append(&numMigDemotedPages_);
    numMigLineReadsCxl_.init("migLineReadsCxl", "Migration line reads issued to the CXL tier"); memStats->append(&numMigLineReadsCxl_);
    numMigLineWritesCxl_.init("migLineWritesCxl", "Migration line writes issued to the CXL tier"); memStats->append(&numMigLineWritesCxl_);
    numMigLineReadsDdr_.init("migLineReadsDdr", "Migration line reads issued to the DDR tier"); memStats->append(&numMigLineReadsDdr_);
    numMigLineWritesDdr_.init("migLineWritesDdr", "Migration line writes issued to the DDR tier"); memStats->append(&numMigLineWritesDdr_);

    // ---- Memory footprint (from pageMigrator's tracker) ----
    auto totalUniquePages = [this]() { return this->pageMigrator->getTotalUniquePages(); };
    auto fastMemPages     = [this]() { return this->pageMigrator->getFastMemPages(); };
    auto slowMemPages     = [this]() { return this->pageMigrator->getSlowMemPages(); };

    auto* uniquePagesStat   = new LambdaStat<decltype(totalUniquePages)>(totalUniquePages);
    uniquePagesStat->init("uniquePages", "Total unique pages accessed");
    memStats->append(uniquePagesStat);

    auto* fastMemPagesStat  = new LambdaStat<decltype(fastMemPages)>(fastMemPages);
    fastMemPagesStat->init("fastMemPages", "Pages currently in fast memory");
    memStats->append(fastMemPagesStat);

    auto* slowMemPagesStat  = new LambdaStat<decltype(slowMemPages)>(slowMemPages);
    slowMemPagesStat->init("slowMemPages", "Pages currently in slow memory");
    memStats->append(slowMemPagesStat);

    // ---- Pass-through children ----
    for (uint32_t c = 0; c < this->numDDRMems; c++) {
        this->ddrMems[c]->initStats(memStats);
    }
    for (uint32_t c = 0; c < this->numCXLMems; c++) {
        this->cxlMems[c]->initStats(memStats);
    }

    // ---- PageMigrator subtree ----
    assert(this->pageMigrator != nullptr);
    if (this->freqThresholdMigrator_) this->freqThresholdMigrator_->initStats(memStats);  // PageMigrator::initStats is non-virtual
    else this->pageMigrator->initStats(memStats);

    /*auto* exportRobStallCdf = makeLambdaStat([this]() -> uint64_t {
        this->exportPageStallCDF("rob_page_stall_cdf.csv");
        return 0;
    });
    exportRobStallCdf->init("robPageStallCdf",
        "Export ROB stall concentration CDF at end of simulation");
    memStats->append(exportRobStallCdf);
*/
 /*   auto* exportAccessCdf = makeLambdaStat([this]() -> uint64_t {
        this->exportPageAccessCDF("page_access_cdf.csv");
        return 0;
    });

    exportAccessCdf->init("pageAccessCdf",
        "Export page access hotness CDF at end of simulation");
    memStats->append(exportAccessCdf);
    
    auto* exportCross = makeLambdaStat([this]() -> uint64_t {
        this->exportCrossCDFs();
        return 0;
    });
    exportCross->init("ABDifference",
        "Export A-B and B-A difference percentages");
    memStats->append(exportCross);
    */
    parentStat->append(memStats);
}


uint64_t TieringMemory::access(MemReq& req) {
    switch (req.type) {
        case PUTS:
        case PUTX:
            *req.state = I;
            break;
        case GETS:
            *req.state = req.is(MemReq::NOEXCL)? S : E;
            break;
        case GETX:
            *req.state = M;
            break;

        default: panic("!?");
    }

    numTotolAccesses.inc();
    uint64_t pageAddress = req.lineAddr / (this->pageSize / this->lineSize);    // This byte offset bits are already shifted away

    {
        std::lock_guard<std::mutex> guard(robStallMutex);   
        accessCountByPage[pageAddress]++;                       
        totalAccessCount++;                                     
    }                                                   

    PageLocation loc = this->pageMigrator->access(pageAddress);

    // --- real migration traffic: issue the line requests of every page batch the migrator published ---
    // (checked on every access; the batch is published atomically at the end of triggerMigration(), so a
    //  consumer sees either nothing or whole batches, and fetchMoves() hands each batch to exactly one thread)
    if (migrationTrafficReal_ && this->pageMigrator->hasPendingMoves()) {
        std::vector<PageMigrator::PageMove> moves;
        this->pageMigrator->fetchMoves(moves);
        if (!moves.empty()) issueMigrationTraffic(moves, req.cycle);
    }

    // Check if a migration trigger just fired; if so recompute all per-access penalties
    // for the next interval.  All models follow the same pattern: compute at the
    // interval boundary, apply as a flat addend on every CXL access until the next
    // boundary.  This mirrors the existing migrationPenaltyPerAccess_ mechanism.
    {
        uint64_t curTriggers = this->pageMigrator->getNumTriggersFired();
        if (curTriggers != prevNumTriggersFired_) {
            prevNumTriggersFired_ = curTriggers;
            uint64_t intervalSz  = this->pageMigrator->getNumIntervalAccesses();

            // --- existing: migration overhead penalty ---
            uint64_t pages = this->pageMigrator->getLastIntervalMigrations();
            migrationPenaltyPerAccess_ = (intervalSz > 0)
                ? (pages * migrationCostCyclesPerPage_) / intervalSz
                : 0;

            // --- Model A: CXL link bandwidth congestion ---
            // CXL access ratio = fraction of interval accesses that went to CXL.
            // Used as a proxy for link utilization.  Past cxlKneeThreshold_, a
            // quadratic penalty is applied (excess^2 × peakLatency).
            if (cxlBwCongestionEnabled_) {
                uint64_t cxlInterval = cxlIntervalAccessCount_.exchange(0, std::memory_order_relaxed);
                if (intervalSz > 0) {
                    double cxlRatio = (double)cxlInterval / (double)intervalSz;
                    if (cxlRatio > cxlKneeThreshold_) {
                        double excess = (cxlRatio - cxlKneeThreshold_) / (1.0 - cxlKneeThreshold_);
                        if (excess > 1.0) excess = 1.0;
                        cxlBwCongestionPenalty_ = (uint64_t)(cxlPeakCongestionLatency_ * excess * excess);
                    } else {
                        cxlBwCongestionPenalty_ = 0;
                    }
                }
            }

            // --- Model B: CXL head-of-line blocking ---
            // Number of distinct cores that accessed CXL this interval is a proxy
            // for the average request queue depth on the shared serialized link.
            // Each additional core beyond the first adds cxlHoLCyclesPerCore_ latency.
            if (cxlHoLBlockingEnabled_) {
                uint64_t mask = cxlActiveCoresMask_.exchange(0, std::memory_order_relaxed);
                int activeCores = __builtin_popcountll(mask);
                cxlHoLPenalty_ = (activeCores > 1)
                    ? cxlHoLCyclesPerCore_ * (uint64_t)(activeCores - 1)
                    : 0;
            }
        }
    }

    if (req.robHeadStallCycles > robStallMinThresholdCycles) {
        //info("[TIERING] ROB stall received: lineAddr=0x%lx, type=%d, cycles=%lu", req.lineAddr, req.type, req.robHeadStallCycles);
        numRobHeadStallEvents.inc(req.robHeadStallCycles);
        recordRobHeadStall(req.lineAddr, req.type, req.robHeadStallCycles);
        // Pass coreId (childId & 63) for CCF cross-core fan-out tracking
        this->pageMigrator->registerRobHeadLoadStall(pageAddress, req.robHeadStallCycles, req.childId & 63);
    }

    if (migrationTrigger) {
        migrationTrigger->onAccessStart(loc, req.cycle);
    }
    const uint64_t start = req.cycle;
    if (loc == FAST_MEM) {
        numDDRAccesses.inc();
        const uint64_t addr = req.lineAddr;
        const uint32_t ddrIdx = (addr / 64) % this->numDDRMems;
        req.lineAddr = (addr / 64 / this->numDDRMems * 64) | (addr % 64);
        const uint64_t resp = this->ddrMems[ddrIdx]->access(req);
        req.lineAddr = addr;
        if (migrationTrigger) migrationTrigger->onAccessComplete(loc, start, resp);
        return resp;

    } else if (loc == SLOW_MEM) {
        numCXLAccesses.inc();

        // Track per-interval CXL access count and active cores for Models A & B.
        if (cxlBwCongestionEnabled_) {
            cxlIntervalAccessCount_.fetch_add(1, std::memory_order_relaxed);
        }
        if (cxlHoLBlockingEnabled_) {
            cxlActiveCoresMask_.fetch_or(1ULL << (req.childId & 63), std::memory_order_relaxed);
        }

        const uint64_t addr = req.lineAddr;
        const uint32_t cxlIdx = (addr / 64) % this->numCXLMems;
        req.lineAddr = (addr / 64 / this->numCXLMems * 64) | (addr % 64);
        const uint64_t resp = this->cxlMems[cxlIdx]->access(req)
                              + this->cxlFixedLatencyOffset
                              + migrationPenaltyPerAccess_
                              + cxlBwCongestionPenalty_
                              + cxlHoLPenalty_;
        req.lineAddr = addr;

        if (migrationPenaltyPerAccess_ > 0) totalMigrationOverheadCycles_.inc(migrationPenaltyPerAccess_);
        if (cxlBwCongestionPenalty_    > 0) totalCxlBwCongestionCycles_.inc(cxlBwCongestionPenalty_);
        if (cxlHoLPenalty_             > 0) totalCxlHoLBlockingCycles_.inc(cxlHoLPenalty_);

        if (migrationTrigger) migrationTrigger->onAccessComplete(loc, start, resp);
        return resp;
    } else {
        g_string locStr = pageLocationToStr(loc);
        panic("Unknown page location: %s", locStr.c_str());
        return 0;
    }
}

void TieringMemory::initPageMigrator(Config& config, PlacementPolicy pp, MigrationPolicy mp, MigrationTriggerPolicy mtp, uint64_t migrationIntervalAccesses, uint64_t dramSizeBytes) {
    g_string ppStr = placementPolicyToStr(pp);
    g_string mpStr = migrationPolicyToStr(mp);
    g_string mtpStr = migrationTriggerPolicyToStr(mtp);
    uint64_t numDramPages = dramSizeBytes / this->pageSize;

    // Migration overhead modeling
    this->migrationCostCyclesPerPage_ = config.get<uint64_t>("sys.mem.migrationCostCyclesPerPage", 200);
    if (this->migrationCostCyclesPerPage_ > 0) {
        info("Migration overhead model: %ld cycles per 4KB page migrated",
             this->migrationCostCyclesPerPage_);
    }

    // Migration bandwidth cap: convert MB → pages
    uint64_t migrationBandwidthCapMB = config.get<uint64_t>("sys.mem.migrationBandwidthCapMB", 1024);
    uint64_t migrationPageCap = (migrationBandwidthCapMB * 1024ULL * 1024ULL) / this->pageSize;
    info("Migration bandwidth cap: %ld MB = %ld pages per interval",
         migrationBandwidthCapMB, migrationPageCap);
    if (mp == NONE) {
        this->pageMigrator = new NonePageMigrator(this->name + ".NonePageMigrator", pp, numDramPages, migrationIntervalAccesses);

    } else if (mp == FREQ_THRESHOLD) {
        this->freqThresholdMigrator_ = new FreqThresholdPageMigrator(this->name + ".FreqThresholdPageMigrator", pp, numDramPages,
                                                                     migrationIntervalAccesses, this->migrationHotThreshold_);
        this->pageMigrator = this->freqThresholdMigrator_;

    } else if (mp == FREQ_BASED) {
        this->pageMigrator = new FreqBasedPageMigrator(this->name + ".FreqBasedPageMigrator", pp, numDramPages, migrationIntervalAccesses);

    } else if (mp == PRACTICAL_FREQ_BASED) {
        this->pageMigrator = new PracticalFreqBasedPageMigrator(this->name + ".PracticalFreqBasedPageMigrator", pp, numDramPages, migrationIntervalAccesses);

    } else if (mp == ROB_STALL_BASED) {
        // Read ROB stall metric variant from config (default: robStallCycles)
        RobStallMetric robMetric = strToRobStallMetric(
            config.get<const char*>("sys.mem.robStallMetric", "robStallCycles")
        );
        g_string metricStr = robStallMetricToStr(robMetric);
        this->pageMigrator = new RobStallBasedPageMigrator(
            this->name + ".RobStallBasedPageMigrator", pp, numDramPages, migrationIntervalAccesses, robMetric
        );
        info("ROB stall metric variant: %s", metricStr.c_str());

    } else if (mp == ROB_PRESSURE_WEIGHTED) {
        this->pageMigrator = new RobPressureWeightedPageMigrator(
            this->name + ".RobPressureWeightedPageMigrator", pp, numDramPages, migrationIntervalAccesses
        );

    } else if (mp == CRITICALITY_WEIGHTED) {
        this->pageMigrator = new CriticalityWeightedPageMigrator(
            this->name + ".CriticalityWeightedPageMigrator", pp, numDramPages, migrationIntervalAccesses
        );

    } else if (mp == QLEARNING) {
        this->pageMigrator = new QLearningPageMigrator(
            this->name + ".QLearningPageMigrator", pp, numDramPages, migrationIntervalAccesses
        );

    } else if (mp == HYBRID_FSC || mp == HYBRID_SYS || mp == HYBRID_SSE ||
               mp == HYBRID_SPFD || mp == HYBRID_CEMA || mp == HYBRID_CLCAS ||
               mp == HYBRID_SBS || mp == HYBRID_FSC_CEMA || mp == HYBRID_FSC_SBS ||
               mp == HYBRID_FSC_CEMA_SBS || mp == HYBRID_SSE_SPFD ||
               mp == HYBRID_SSE_SPFD_CLCAS || mp == HYBRID_SYS_CEMA ||
               // Count-based variants
               mp == HYBRID_FSC_CNT || mp == HYBRID_FSC_CNT_SBS ||
               mp == HYBRID_FSC_CNT_CEMA_SBS || mp == HYBRID_SSE_SPFD_CNT ||
               // New CXL-aware ideas
               mp == HYBRID_SVA || mp == HYBRID_GKD || mp == HYBRID_SCA || mp == HYBRID_CCF ||
               mp == HYBRID_FSC_CNT_SVA || mp == HYBRID_FSC_CNT_CCF ||
               mp == HYBRID_FSC_CNT_SVA_CCF || mp == HYBRID_FSC_CNT_SVA_SBS ||
               mp == HYBRID_FSC_CNT_SVA_CCF_SBS || mp == HYBRID_FSC_CNT_GKD_SCA ||
               mp == HYBRID_FSC_CNT_SVA_CCF_GKD_SCA_SBS) {

        // Read optional tuning parameters
        double fscAlpha     = config.get<double>("sys.mem.fscAlpha",    0.001);
        double emaLambda    = config.get<double>("sys.mem.emaLambda",   0.3);
        double stabGamma    = config.get<double>("sys.mem.stabilityGamma", 0.05);
        // cxlBaselineLatency = 0 means "auto-calibrate from the actual CXL lead-off latency"
        double cxlBaseline  = config.get<double>("sys.mem.cxlBaselineLatency", 0.0);

        if (cxlBaseline <= 0.0) {
            cxlBaseline = computeSlowMemoryLeadOffLatency();
            if (cxlBaseline <= 0.0) {
                cxlBaseline = 1000.0;  // fallback for simple-DDR configs without MESS
            }
            info("Auto-calibrated cxlBaselineLatency = %.0f cycles", cxlBaseline);
        }

        HybridConfig hcfg;
        switch (mp) {
            case HYBRID_FSC:              hcfg = HybridPresets::fsc();              break;
            case HYBRID_SYS:              hcfg = HybridPresets::sys();              break;
            case HYBRID_SSE:              hcfg = HybridPresets::sse();              break;
            case HYBRID_SPFD:             hcfg = HybridPresets::spfd();             break;
            case HYBRID_CEMA:             hcfg = HybridPresets::cema();             break;
            case HYBRID_CLCAS:            hcfg = HybridPresets::clcas();            break;
            case HYBRID_SBS:              hcfg = HybridPresets::sbs();              break;
            case HYBRID_FSC_CEMA:         hcfg = HybridPresets::fsc_cema();         break;
            case HYBRID_FSC_SBS:          hcfg = HybridPresets::fsc_sbs();          break;
            case HYBRID_FSC_CEMA_SBS:     hcfg = HybridPresets::fsc_cema_sbs();     break;
            case HYBRID_SSE_SPFD:         hcfg = HybridPresets::sse_spfd();         break;
            case HYBRID_SSE_SPFD_CLCAS:   hcfg = HybridPresets::sse_spfd_clcas();   break;
            case HYBRID_SYS_CEMA:         hcfg = HybridPresets::sys_cema();         break;
            // Count-based variants
            case HYBRID_FSC_CNT:          hcfg = HybridPresets::fsc_cnt();          break;
            case HYBRID_FSC_CNT_SBS:      hcfg = HybridPresets::fsc_cnt_sbs();      break;
            case HYBRID_FSC_CNT_CEMA_SBS: hcfg = HybridPresets::fsc_cnt_cema_sbs(); break;
            case HYBRID_SSE_SPFD_CNT:     hcfg = HybridPresets::sse_spfd_cnt();     break;
            // New CXL-aware ideas
            case HYBRID_SVA:                    hcfg = HybridPresets::sva();                         break;
            case HYBRID_GKD:                    hcfg = HybridPresets::gkd();                         break;
            case HYBRID_SCA:                    hcfg = HybridPresets::sca();                         break;
            case HYBRID_CCF:                    hcfg = HybridPresets::ccf();                         break;
            case HYBRID_FSC_CNT_SVA:            hcfg = HybridPresets::fsc_cnt_sva();                 break;
            case HYBRID_FSC_CNT_CCF:            hcfg = HybridPresets::fsc_cnt_ccf();                 break;
            case HYBRID_FSC_CNT_SVA_CCF:        hcfg = HybridPresets::fsc_cnt_sva_ccf();             break;
            case HYBRID_FSC_CNT_SVA_SBS:        hcfg = HybridPresets::fsc_cnt_sva_sbs();             break;
            case HYBRID_FSC_CNT_SVA_CCF_SBS:    hcfg = HybridPresets::fsc_cnt_sva_ccf_sbs();         break;
            case HYBRID_FSC_CNT_GKD_SCA:        hcfg = HybridPresets::fsc_cnt_gkd_sca();             break;
            case HYBRID_FSC_CNT_SVA_CCF_GKD_SCA_SBS: hcfg = HybridPresets::fsc_cnt_sva_ccf_gkd_sca_sbs(); break;
            default: panic("Unexpected hybrid policy %d", mp);
        }
        // Apply user-configured parameters
        hcfg.alpha              = fscAlpha;
        hcfg.lambda             = emaLambda;
        hcfg.gamma              = stabGamma;
        hcfg.cxlBaselineLatency = cxlBaseline;

        g_string migratorName = (this->name + ".") + hcfg.methodName;
        this->pageMigrator = new HybridPageMigrator(
            migratorName, pp, numDramPages, migrationIntervalAccesses, hcfg
        );
        info("Created HybridPageMigrator[%s]: alpha=%.4f lambda=%.2f gamma=%.2f "
             "cxlBaseline=%.0f useStallCount=%d migrationPageCap=%ld",
             hcfg.methodName, hcfg.alpha, hcfg.lambda, hcfg.gamma,
             hcfg.cxlBaselineLatency, (int)hcfg.useStallCount, migrationPageCap);

    } else {
        panic("Invalid migration policy %s", mpStr.c_str());

    }
    // Apply migration page cap to all migrators (after creation)
    if (this->pageMigrator) {
        this->pageMigrator->setMigrationPageCap(migrationPageCap);
        // Per-epoch promotion cap (pages; 0 = unlimited) — applied to the promotion budget of every migrator
        this->pageMigrator->setMaxPagesPerInterval(this->migrationMaxPagesPerInterval_);
        if (this->migrationMaxPagesPerInterval_ > 0) {
            info("Migration per-epoch promotion cap: %ld pages", this->migrationMaxPagesPerInterval_);
        }
        if (this->migrationTrafficReal_) {
            this->pageMigrator->enableMoveLog(true);
            // Real traffic replaces the per-page penalty model.
            if (this->migrationCostCyclesPerPage_ != 0) {
                info("migrationTraffic = real: forcing migrationCostCyclesPerPage from %ld to 0", this->migrationCostCyclesPerPage_);
                this->migrationCostCyclesPerPage_ = 0;
            }
        }
    }

    if (mtp == FIXED) {
        this->migrationTrigger = new FixedMigrationTrigger();
    } else if (mtp == LATENCY_BASED) {
        const double eps   = config.get<double>("sys.mem.latencyEps", 0.01);   // ε: reset watermarks if p_lo/p_hi too close (default 0.01)
        const double delta = config.get<double>("sys.mem.latencyDelta", 0.05); // δ: treat tiers as balanced if latencies differ < δ (default 0.05)
        const uint64_t staticCap = config.get<uint64_t>("sys.mem.staticCapPages", 0); // max pages to migrate per interval (0 = cap by DRAM pages)
        //budget = Δp × requests (in cache lines) → must convert to bytes, then to pages → needs page/line sizes.
        const uint32_t lineBytes = this->lineSize;
        const uint64_t pageBytes = this->pageSize;
        this->migrationTrigger = new LatencyBasedMigrationTrigger(
                       eps, delta, staticCap, lineBytes, pageBytes);
    } else if (mtp == AOL_BASED) {
        double aolLow = config.get<double>("sys.mem.aolLowCycles", 0.0);
        double aolHigh = config.get<double>("sys.mem.aolHighCycles", 0.0);
        if (aolLow <= 0.0) {
            aolLow = computeFastMemoryLeadOffLatency();
        }
        if (aolHigh <= 0.0 && aolLow > 0.0) {
            aolHigh = aolLow * 2.0;
        }
        if (aolLow <= 0.0 || aolHigh <= 0.0 || aolHigh <= aolLow) {
            panic("Invalid AOL thresholds: low=%.2f high=%.2f", aolLow, aolHigh);
        }
        this->migrationTrigger = new AolBasedMigrationTrigger(aolLow, aolHigh); //budget = % of DRAM pages → no need for page/line sizes.
    } else {
        panic("Invalid migration trigger policy %s", mtpStr.c_str());
    }

    if (migrationTrigger && pageMigrator) {
        pageMigrator->setMigrationTrigger(migrationTrigger);
    }
        

    info("Initialized page migrator %s with placement policy %s, placement trigger policy %s, migration interval accesses %ld, fast memory pages %ld",
            mpStr.c_str(), ppStr.c_str(), mtpStr.c_str(), migrationIntervalAccesses, numDramPages);
}

// Issue the line traffic of a batch of page movements decided at an epoch boundary.
// Promotion (slow -> fast): 64 line reads from the CXL tier + 64 line writes to the DDR tier.
// Demotion  (fast -> slow): 64 line reads from the DDR tier + 64 line writes to the CXL tier.
// All requests are tagged CXLMemory::FLAG_MIGRATION, carry the phantom source id (no timing
// record, bound-phase DDR path) and are issued back to back at `cycle`, the cycle of the demand
// access that observed the boundary; the CXL credit pools serialise them and record the burst.
void TieringMemory::issueMigrationTraffic(const std::vector<PageMigrator::PageMove>& moves, uint64_t cycle) {
    const uint64_t linesPerPage = this->pageSize / this->lineSize;
    MESIState dummyState = I;

    auto issue = [&](g_vector<MemObject*>& mems, uint32_t numMems, uint64_t lineAddr, AccessType type) {
        const uint32_t idx = (lineAddr / 64) % numMems;
        MemReq mreq = {(lineAddr / 64 / numMems * 64) | (lineAddr % 64), type, 0, &dummyState, cycle,
                       nullptr, I, this->migSrcId_, CXLMemory::FLAG_MIGRATION, 0};
        mems[idx]->access(mreq);
    };

    for (CXLMemory* ctrl : this->cxlCtrls) if (ctrl) ctrl->migrationBatchBegin();

    uint64_t promoted = 0, demoted = 0;
    for (const PageMigrator::PageMove& mv : moves) {
        const uint64_t firstLine = mv.page * linesPerPage;
        if (mv.promote) {
            promoted++;
            for (uint64_t l = 0; l < linesPerPage; l++) {
                issue(this->cxlMems, this->numCXLMems, firstLine + l, GETS);
                issue(this->ddrMems, this->numDDRMems, firstLine + l, PUTX);
            }
            numMigLineReadsCxl_.inc(linesPerPage);
            numMigLineWritesDdr_.inc(linesPerPage);
        } else {
            demoted++;
            for (uint64_t l = 0; l < linesPerPage; l++) {
                issue(this->ddrMems, this->numDDRMems, firstLine + l, GETS);
                issue(this->cxlMems, this->numCXLMems, firstLine + l, PUTX);
            }
            numMigLineReadsDdr_.inc(linesPerPage);
            numMigLineWritesCxl_.inc(linesPerPage);
        }
    }

    for (CXLMemory* ctrl : this->cxlCtrls) if (ctrl) ctrl->migrationBatchEnd();

    numMigBatches_.inc();
    numMigPromotedPages_.inc(promoted);
    numMigDemotedPages_.inc(demoted);
}

// ----------------------------------------------------------------------------
// FreqThresholdPageMigrator
// ----------------------------------------------------------------------------

FreqThresholdPageMigrator::FreqThresholdPageMigrator(const g_string& _name, PlacementPolicy _pp, uint64_t _numFastMemPages,
                                                     uint64_t _numIntervalAccesses, uint64_t _hotThreshold)
    : PageMigrator(_name, _pp, _numFastMemPages, _numIntervalAccesses), hotThreshold_(_hotThreshold) {
    info("Initialized FreqThresholdPageMigrator: name: %s, numFastMemPages: %ld, numIntervalAccesses: %ld, hotThreshold: %ld",
         name.c_str(), numFastMemPages, numIntervalAccesses, hotThreshold_);
}

void FreqThresholdPageMigrator::initStats(AggregateStat* parentStat) {
    AggregateStat* migratorStats = new AggregateStat();
    migratorStats->init(name.c_str(), "FreqThresholdPageMigrator stats");
    numEpochs_.init("numEpochs", "Epoch boundaries seen"); migratorStats->append(&numEpochs_);
    numPromotionEpochs_.init("numPromotionEpochs", "Epochs with >= 1 promotion"); migratorStats->append(&numPromotionEpochs_);
    numPromotions_.init("numPromotions", "Pages promoted slow -> fast"); migratorStats->append(&numPromotions_);
    numDemotions_.init("numDemotions", "Pages demoted fast -> slow (evictions)"); migratorStats->append(&numDemotions_);
    numHotCandidates_.init("numHotCandidates", "Slow-tier pages with >= threshold accesses, summed over epochs"); migratorStats->append(&numHotCandidates_);
    numBudgetLimitedEpochs_.init("numBudgetLimitedEpochs", "Epochs in which the promotion cap/budget left hot candidates unpromoted"); migratorStats->append(&numBudgetLimitedEpochs_);
    lastEpochPromotions_.init("lastEpochPromotions", "Promotions in the most recent epoch"); migratorStats->append(&lastEpochPromotions_);
    lastEpochHotCandidates_.init("lastEpochHotCandidates", "Hot candidates in the most recent epoch"); migratorStats->append(&lastEpochHotCandidates_);
    PageMigrator::initStats(migratorStats);
    parentStat->append(migratorStats);
}

PageLocation FreqThresholdPageMigrator::access(uint64_t pageAddress) {
    std::lock_guard<std::mutex> guard(lock_);
    PageLocation loc;
    auto it = pages_.find(pageAddress);
    if (it != pages_.end()) {
        PageInfo& pi = it->second;
        if (pi.epochCount == 0) touched_.push_back(pageAddress);
        pi.epochCount++;
        loc = pi.loc;
    } else {
        // Placement of a first-touch page
        if (placementPolicy == RANDOM) {
            loc = rng_->randInt(1) ? FAST_MEM : SLOW_MEM;
            if (loc == FAST_MEM && numFastResident_ >= numFastMemPages) loc = SLOW_MEM;
        } else if (placementPolicy == FAST_FIRST) {
            loc = (numFastResident_ < numFastMemPages) ? FAST_MEM : SLOW_MEM;
        } else if (placementPolicy == SLOW_FIRST) {
            loc = SLOW_MEM;
        } else {
            g_string ppStr = placementPolicyToStr(placementPolicy);
            panic("Unknown placement policy: %s", ppStr.c_str());
        }
        PageInfo pi = {1, loc, 0};
        if (loc == FAST_MEM) {
            pi.fifoToken = nextToken_++;
            fastFifo_.push_back(std::make_pair(pageAddress, pi.fifoToken));
            numFastResident_++;
        }
        pages_[pageAddress] = pi;
        touched_.push_back(pageAddress);
    }
    PageMigrator::registerAccess(loc);   // may call triggerMigration() (lock_ is held; publishMoves uses its own lock)
    return loc;
}

void FreqThresholdPageMigrator::triggerMigration() {
    numEpochs_.inc();

    // Candidates: slow-tier pages with >= threshold accesses this epoch, hottest first.
    std::vector<std::pair<uint64_t, uint64_t>> cand;   // (count, page)
    for (uint64_t page : touched_) {
        auto it = pages_.find(page);
        if (it == pages_.end()) continue;
        if (it->second.loc == SLOW_MEM && it->second.epochCount >= hotThreshold_) {
            cand.push_back(std::make_pair((uint64_t)it->second.epochCount, page));
        }
    }
    std::sort(cand.begin(), cand.end(), [](const std::pair<uint64_t, uint64_t>& a, const std::pair<uint64_t, uint64_t>& b) {
        if (a.first != b.first) return a.first > b.first;
        return a.second < b.second;
    });
    numHotCandidates_.inc(cand.size());
    lastEpochHotCandidates_.set(cand.size());

    uint64_t promoted = 0, demoted = 0;
    bool budgetLimited = false;
    std::vector<PageMove> batch;
    for (const auto& c : cand) {
        if (!consumePromotionBudget()) { budgetLimited = true; break; }
        // Make room: evict the least recently promoted / placed fast-tier page.
        if (numFastResident_ >= numFastMemPages) {
            bool evicted = false;
            while (!fastFifo_.empty()) {
                std::pair<uint64_t, uint64_t> victim = fastFifo_.front();
                fastFifo_.pop_front();
                auto vit = pages_.find(victim.first);
                if (vit != pages_.end() && vit->second.loc == FAST_MEM && vit->second.fifoToken == victim.second) {
                    vit->second.loc = SLOW_MEM;
                    vit->second.fifoToken = 0;
                    numFastResident_--;
                    demoted++;
                    batch.push_back({victim.first, false});
                    evicted = true;
                    break;
                }
            }
            if (!evicted) break;   // no victim available; stop promoting
        }
        auto pit = pages_.find(c.second);
        assert(pit != pages_.end() && pit->second.loc == SLOW_MEM);
        pit->second.loc = FAST_MEM;
        pit->second.fifoToken = nextToken_++;
        fastFifo_.push_back(std::make_pair(c.second, pit->second.fifoToken));
        numFastResident_++;
        promoted++;
        batch.push_back({c.second, true});
    }
    publishMoves(batch);

    numPromotions_.inc(promoted);
    numDemotions_.inc(demoted);
    lastEpochPromotions_.set(promoted);
    if (promoted) numPromotionEpochs_.inc();
    if (budgetLimited) numBudgetLimitedEpochs_.inc();
    lastIntervalMigrations_ = promoted + demoted;   // consumed by the penalty model when migrationTraffic = penalty

    // Reset per-epoch counts.
    for (uint64_t page : touched_) {
        auto it = pages_.find(page);
        if (it != pages_.end()) it->second.epochCount = 0;
    }
    touched_.clear();

    info("[%s] epoch %ld: %ld hot candidates (>= %ld accesses), promoted %ld, demoted %ld, fast resident %ld/%ld%s",
         name.c_str(), numEpochs_.get(), (uint64_t)cand.size(), hotThreshold_, promoted, demoted,
         numFastResident_, numFastMemPages, budgetLimited ? " (budget-limited)" : "");
}

double TieringMemory::computeFastMemoryLeadOffLatency() const {
    double bestLatency = 0.0;
    for (MemObject* mo : ddrMems) {
        auto* ctrl = dynamic_cast<WeaveMessMemCtrl*>(mo);
        if (!ctrl) continue;
        double leadOff = static_cast<double>(ctrl->getLeadOffLatency());
        if (leadOff > 0.0 && (bestLatency == 0.0 || leadOff < bestLatency)) {
            bestLatency = leadOff;
        }
    }
    return bestLatency;
}

double TieringMemory::computeSlowMemoryLeadOffLatency() const {
    // Query each CXL controller for its lead-off (head-of-queue) latency and add
    // the fixed CXL link-traversal offset to get the total observed slow-tier latency.
    double bestLatency = 0.0;
    for (MemObject* mo : cxlMems) {
        auto* ctrl = dynamic_cast<WeaveMessMemCtrl*>(mo);
        if (!ctrl) continue;
        double leadOff = static_cast<double>(ctrl->getLeadOffLatency()) + cxlFixedLatencyOffset;
        if (leadOff > 0.0 && (bestLatency == 0.0 || leadOff < bestLatency)) {
            bestLatency = leadOff;
        }
    }
    return bestLatency;
}

void TieringMemory::recordRobHeadStall(Address lineAddr, AccessType type, uint64_t events) {
    if (!events) return;

    const uint64_t baseAddr = lineAddr * lineSize;
    const uint64_t pageNumber = baseAddr / pageSize;

    std::lock_guard<std::mutex> guard(robStallMutex);


    RobStallAggregate& perPage = robStallByPage[pageNumber];
    accumulateRobStallAggregate(perPage, type, events);

    totalRobStallCycles += events;
}
void TieringMemory::accumulateRobStallAggregate(RobStallAggregate& agg, AccessType type, uint64_t events) {
    agg.totalCycles += events;
    agg.totalEvents++;

    switch (type) {
        case GETS:
            agg.loadCycles += events;
            agg.loadEvents++;
            break;
        case GETX:
            agg.storeCycles += events;
            agg.storeEvents++;
            break;
        default:
            break;
    }
}

/*
void TieringMemory::exportPageAccessCDF(const std::string& path) const {
    std::vector<uint64_t> accesses;
    uint64_t totalAccesses = 0;

    {
        std::lock_guard<std::mutex> lock(robStallMutex);
        for (const auto& kv : accessCountByPage) {
            accesses.push_back(kv.second);
            totalAccesses += kv.second;
        }
    }

    if (accesses.empty() || totalAccesses == 0) return;

    std::sort(accesses.begin(), accesses.end(), std::greater<uint64_t>());

    FILE* f = fopen(path.c_str(), "w");
    if (!f) return;

    fprintf(f, "percent_pages,percent_access\n");

    const int pctBuckets[] = {10,20,30,40,50,60,70,80,90,100};
    size_t N = accesses.size();

    for (int pct : pctBuckets) {
        size_t cutoff = (N * pct + 99) / 100;
        if (cutoff > N) cutoff = N;

        uint64_t sum = 0;
        for (size_t i = 0; i < cutoff; i++)
            sum += accesses[i];

        double accessPct = (double)sum * 100.0 / totalAccesses;
        fprintf(f, "%d,%.2f\n", pct, accessPct);
    }

    fclose(f);
}


void TieringMemory::exportPageStallCDF(const std::string& path) const {
    std::vector<std::pair<uint64_t,uint64_t>> pages;
    uint64_t totalCycles = 0;

    {
        std::lock_guard<std::mutex> lock(robStallMutex);
        totalCycles = totalRobStallCycles;
        pages.reserve(robStallByPage.size());
        for (const auto& kv : robStallByPage) {
            if (kv.second.totalCycles == 0) continue;
            pages.emplace_back(kv.first, kv.second.totalCycles);
        }
    }

    if (pages.empty() || totalCycles == 0) return;
    std::sort(pages.begin(), pages.end(),
          [](const std::pair<uint64_t,uint64_t>& a,
             const std::pair<uint64_t,uint64_t>& b) {
                return a.second > b.second;
          });

    FILE* f = fopen(path.c_str(), "w");
    if (!f) return;
    fprintf(f,
        "percent_pages,percent_stall\n"
    );
    const int pctBuckets[] = {10,20,30,40,50,60,70,80,90,100};

    size_t N = pages.size();

    for (int pct : pctBuckets) {
        size_t cutoff = (N * pct + 99) / 100;
        if (cutoff > N) cutoff = N;

        uint64_t sum = 0;
        for (size_t i = 0; i < cutoff; i++)
            sum += pages[i].second;

        double stallPct = (double)sum * 100.0 / totalCycles;
        

        fprintf(f,
            "%d,%.2f\n",
            pct, stallPct);
    }

    fclose(f);
}
*/


/*void TieringMemory::exportCrossCDFs() const {
    const int pctBuckets[] = {10,20,30,40,50,60,70,80,90,100};

    // Full lists over the accessed-page universe:
    // accList: (page, accesses)
    // robList: (page, stall_cycles)  -- includes zeros
    std::vector<std::pair<uint64_t, uint64_t>> accList;
    std::vector<std::pair<uint64_t, uint64_t>> robList;

    uint64_t totalAcc = 0;

    {
        std::lock_guard<std::mutex> lock(robStallMutex);

        accList.reserve(accessCountByPage.size());
        robList.reserve(accessCountByPage.size());

        for (const auto& kv : accessCountByPage) {
            uint64_t page = kv.first;
            uint64_t acc  = kv.second;

            totalAcc += acc;
            accList.emplace_back(page, acc);

            // stall = 0 if not present
            uint64_t stall = 0;
            auto it = robStallByPage.find(page);
            if (it != robStallByPage.end()) {
                stall = it->second.totalCycles;
            }
            robList.emplace_back(page, stall);
        }
    }

    if (accList.empty() || totalAcc == 0) return;

    // Sort by access hotness (descending)
    std::sort(accList.begin(), accList.end(),
        [](const std::pair<uint64_t,uint64_t>& a,
           const std::pair<uint64_t,uint64_t>& b) {
            return a.second > b.second;
        }
    );

    // Sort by stall hotness (descending)
    std::sort(robList.begin(), robList.end(),
        [](const std::pair<uint64_t,uint64_t>& a,
           const std::pair<uint64_t,uint64_t>& b) {
            return a.second > b.second;
        }
    );

    // Universe size is exactly the number of accessed pages
    const size_t U = accList.size();
    if (U == 0) return;

    FILE* f_access_minus_stall = fopen("access_minus_stall.csv", "w");
    FILE* f_stall_minus_access = fopen("stall_minus_access.csv", "w");
    if (!f_access_minus_stall || !f_stall_minus_access) return;

    fprintf(f_access_minus_stall, "percent_pages,diff_percent\n");
    fprintf(f_stall_minus_access, "percent_pages,diff_percent\n");

    std::unordered_set<uint64_t> access_hot_pages;
    std::unordered_set<uint64_t> stall_hot_pages;

    for (int pct : pctBuckets) {
        size_t cutoff = (U * pct + 99) / 100;  // ceil(U*pct/100)
        if (cutoff == 0) cutoff = 1;
        if (cutoff > U) cutoff = U;

        access_hot_pages.clear();
        stall_hot_pages.clear();

        // Access-hot = top cutoff pages by access
        for (size_t i = 0; i < cutoff; i++) {
            access_hot_pages.insert(accList[i].first);
        }

        // Stall-hot = top cutoff pages by ROB stall cycles
        for (size_t i = 0; i < cutoff; i++) {
            stall_hot_pages.insert(robList[i].first);
        }

        // |AccessHot - StallHot|
        size_t access_minus_stall_cnt = 0;
        for (uint64_t page : access_hot_pages) {
            if (stall_hot_pages.count(page) == 0) access_minus_stall_cnt++;
        }

        // |StallHot - AccessHot|
        size_t stall_minus_access_cnt = 0;
        for (uint64_t page : stall_hot_pages) {
            if (access_hot_pages.count(page) == 0) stall_minus_access_cnt++;
        }

        // diff percent
        double access_minus_stall_pct =
            (double)access_minus_stall_cnt * 100.0 / (double)access_hot_pages.size();

        double stall_minus_access_pct =
            (double)stall_minus_access_cnt * 100.0 / (double)stall_hot_pages.size();

        fprintf(f_access_minus_stall, "%d,%.2f\n", pct, access_minus_stall_pct);
        fprintf(f_stall_minus_access, "%d,%.2f\n", pct, stall_minus_access_pct);
    }

    fclose(f_access_minus_stall);
    fclose(f_stall_minus_access);
}
*/

