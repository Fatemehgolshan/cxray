#pragma once
#include "cxl_protocol.h"
#include "cxl_link.h"
#include "cxl_switch.h"
#include "cxl_device.h"
#include "cxl_fabric.h"
#include "cxl_io.h"
#include "cxl_numa.h"
#include "cxl_sim_engine.h"
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>

namespace cxl {

// ---------------------------------------------------------------------------
// SimConfig — full resolved configuration for one simulation run.
// Parsed from libconfig++ .cfg files or the built-in key=value parser.
// ---------------------------------------------------------------------------

struct SimConfig {
    // Version / profile
    std::string  cxlVersion     = "3.1";   // "1.1", "2.0", "3.0", "3.1"
    std::string  simMode        = "fast";  // "fast" | "detailed"

    // Topology
    bool         hasSwitch      = false;
    int          numDevices     = 1;

    // Per-device (all devices same config for now)
    DeviceConfig device;

    // Link
    LinkConfig   link;

    // Switch (if hasSwitch)
    SwitchConfig sw;

    // RAS
    RASConfig    ras;

    // Host overhead: LLC-miss + host-MC overhead (added to reported latency
    // to match MLC measurements on real systems, e.g., SPR DDR5 ≈ 97 ns).
    double       hostOverheadNs  = 97.0;

    // Injection mode: open-loop (true) honors trace timestamps directly;
    // closed-loop (false) dispatches each requester at max(timestamp, prev_completion).
    // Open-loop is the correct default for pre-recorded traces; closed-loop is only
    // appropriate for synthetic workloads where the trace inter-issue time is a target rate.
    bool         injectOpenLoop  = true;

    // Simulation limits
    uint64_t     warmupRequests = 0;
    uint64_t     maxRequests    = 0;   // 0 = unlimited (run full trace)

    // Output
    std::string  outputDir      = "results/";
    std::string  runTag         = "";

    // Stats interval (ns); 0 = no periodic dumps
    double       statsIntervalNs = 0.0;

    // Fabric (P2P + GIM) — CXL 3.0+
    FabricConfig fabric;

    // CXL.io controller config
    CXLIOConfig  io;

    // NUMA topology + page migration
    NUMAConfig   numa;
    PageMigrationConfig migration;

    // Which NUMA node the requester is on (0-indexed)
    int          requesterNUMANode = 0;
};

// ---------------------------------------------------------------------------
// SimStats — accumulated per-request statistics
// ---------------------------------------------------------------------------

struct SimStats {
    uint64_t totalRequests    = 0;
    uint64_t reads            = 0;
    uint64_t writes           = 0;
    uint64_t rowHits          = 0;
    uint64_t rowMisses        = 0;

    double   totalLatencyNs   = 0.0;
    double   minLatencyNs     = 1e18;
    double   maxLatencyNs     = 0.0;

    double   totalCreditStallNs = 0.0;
    double   simDurationNs      = 0.0;  // wallclock of simulation (last - first)

    // Per-component latency decomposition (averaged over all requests)
    LatencyBreakdown latencyBreakdown;

    // Link utilization and effective bandwidth
    double   linkUtilizationFrac = 0.0;
    double   effectiveBwGBps     = 0.0;

    // New breakdown fields (already in LatencyBreakdown for per-request; these are aggregates)
    double   p2pExtraLatencyNs    = 0.0;  // total P2P overhead across all requests
    double   numaOffsetNs         = 0.0;  // total NUMA penalty
    uint64_t pageMigrations       = 0;    // total page migrations triggered

    // Percentile helpers (populated after simulation)
    std::vector<double> latencySamples;

    double meanLatencyNs()  const { return totalRequests ? totalLatencyNs / totalRequests : 0; }
    double rowHitRate()     const { return totalRequests ? (double)rowHits / totalRequests : 0; }
    double bandwidthGBps()  const;
    double p50LatencyNs()   const { return percentile(0.50); }
    double p95LatencyNs()   const { return percentile(0.95); }
    double p99LatencyNs()   const { return percentile(0.99); }
    double percentile(double p) const;
};

// ---------------------------------------------------------------------------
// CXLMemController — ties Link + Switch + Device together.
// Drives requests from a trace or from a zsim MemObject callback.
// ---------------------------------------------------------------------------

class CXLMemController {
public:
    // Construct from already-built config
    explicit CXLMemController(const SimConfig& cfg);

    // Parse a libconfig++ .cfg file and build everything
    static CXLMemController fromConfigFile(const std::string& cfgPath);

    // --------------- Trace-driven simulation ---------------

    // Run a full trace file; returns stats.
    SimStats runTrace(const std::string& tracePath);

    // Process a single request (for incremental / callback use).
    // arrivalNs: simulation time the request arrives at host port.
    RequestResult processRequest(const TraceRequest& req, double arrivalNs);

    // --------------- Stats / accessors ---------------

    const SimStats&     stats()  const { return stats_; }
    const SimConfig&    config() const { return cfg_; }
    CXLDevice&          device(int idx = 0) { return *devices_.at(idx); }
    const CXLDevice&    device(int idx = 0) const { return *devices_.at(idx); }

    // Dump stats to stdout in human-readable form
    void printStats() const;

    // Dump a fully-resolved config (all knobs, with [DEFAULT]/[COMPUTED] annotations)
    void printResolvedConfig() const;

    // Write stats JSON to outputDir
    void writeStatsJson() const;

private:
    SimConfig                            cfg_;
    std::unique_ptr<CXLLink>             link_;
    std::unique_ptr<CXLSwitch>           sw_;     // nullptr if no switch
    std::vector<std::unique_ptr<CXLDevice>> devices_;
    std::unique_ptr<CXLFabric>           fabric_;   // nullptr if no fabric features
    std::unique_ptr<CXLIOController>     ioCtrl_;   // CXL.io controller
    std::unique_ptr<NUMAModel>           numa_;     // nullptr if single-node

    SimStats   stats_;
    double     lastArrivalNs_    = 0.0;
    double     lastCompletionNs_ = 0.0;

    // Accumulated per-component latency (for breakdown averaging)
    LatencyBreakdown accBreakdown_;

    // Internal helpers
    void buildTopology_();
    static SimConfig parseConfigFile_(const std::string& path);
    static void applyVersionDefaults_(SimConfig& cfg);
    static void computeDerivedFields_(SimConfig& cfg);

    // RAS marker descriptor — carries a timestamp-stamped injection command.
    struct PendingMarker {
        uint64_t ts      = 0;
        bool     isPPR   = false;
        bool     isHard  = false;
        uint64_t addr    = 0;
    };

    SimStats runTraceDetailed_(const std::vector<TraceRequest>& requests,
                               const std::vector<PendingMarker>& markers);
};

}  // namespace cxl
