#include "cxl_mem.h"
#include "cxl_dram_backend.h"
#include "ddr5_backend.h"
#include "cxl_fabric.h"
#include "cxl_io.h"
#include "cxl_numa.h"
#include "dramsim3_backend.h"
#include "ramulator2_backend.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

// Optional: libconfig++ for .cfg parsing.  If not available, a minimal
// key=value parser is used instead.
#ifdef HAVE_LIBCONFIG
#  include <libconfig.h++>
#endif

namespace cxl {

// ---------------------------------------------------------------------------
// SimStats helpers
// ---------------------------------------------------------------------------

double SimStats::bandwidthGBps() const {
    if (simDurationNs <= 0) return 0.0;
    double totalBytes = static_cast<double>(totalRequests) * 64.0;  // cacheline
    return totalBytes / simDurationNs;  // bytes/ns = GB/s
}

double SimStats::percentile(double p) const {
    if (latencySamples.empty()) return 0.0;
    auto sorted = latencySamples;
    std::sort(sorted.begin(), sorted.end());
    size_t idx = static_cast<size_t>(p * (sorted.size() - 1));
    return sorted[idx];
}

// ---------------------------------------------------------------------------
// Default / version profiles
// ---------------------------------------------------------------------------

static void applyVersionDefaults(SimConfig& cfg) {
    if (cfg.cxlVersion == "1.1") {
        // PCIe Gen 5 NRZ, 528-bit (66-byte) FLITs, no FEC
        if (cfg.link.pcieGen == 0)    cfg.link.pcieGen    = 5;
        if (cfg.link.pcieLanes == 0)  cfg.link.pcieLanes  = 8;
        cfg.link.hostPortLatencyNs   = 15.0;
        cfg.link.linkPhysLatencyNs   = 3.0;
        cfg.link.devicePortLatencyNs = 12.0;
        cfg.link.linkLatencyNs       = 0.0;
        cfg.link.flitSizeBytes       = 66;    // 528-bit CXL FLIT
        cfg.link.flitPayloadBytes    = 64;
        cfg.link.fecEnable           = false;
        cfg.link.fecDecodeLatencyNs  = 0.0;
        cfg.link.retryProbPerFlit    = 1e-12; // NRZ BER ~1e-15/bit → negligible
        cfg.link.estimatedRttNs      = 220.0;
        cfg.link.vcArbitrationNs     = 1.0;
        cfg.link.credits             = {32, 16, 32, 32};
        cfg.hasSwitch                = false;
        cfg.ras.enable               = false;
        cfg.device.coherenceOverheadNs = 0.0;
        cfg.device.ideEnable         = false;
        cfg.hostOverheadNs           = 97.0;
        cfg.fabric.p2p.enable        = false;
        cfg.fabric.gim.enable        = false;
        cfg.io.linkOneWayLatencyNs   = 30.0;
        cfg.io.dmaLinkBwGBps         = 31.5;  // PCIe Gen5×8 ≈ 31.5 GB/s

    } else if (cfg.cxlVersion == "2.0") {
        // PCIe Gen 5 NRZ, 528-bit FLITs, no FEC, DDR5-4800 backing
        if (cfg.link.pcieGen == 0)    cfg.link.pcieGen    = 5;
        if (cfg.link.pcieLanes == 0)  cfg.link.pcieLanes  = 8;
        cfg.link.hostPortLatencyNs   = 15.0;
        cfg.link.linkPhysLatencyNs   = 3.0;
        cfg.link.devicePortLatencyNs = 12.0;
        cfg.link.linkLatencyNs       = 0.0;
        cfg.link.flitSizeBytes       = 66;
        cfg.link.flitPayloadBytes    = 64;
        cfg.link.fecEnable           = false;
        cfg.link.fecDecodeLatencyNs  = 0.0;
        cfg.link.retryProbPerFlit    = 1e-12;
        cfg.link.estimatedRttNs      = 220.0;
        cfg.link.vcArbitrationNs     = 1.0;
        cfg.link.credits             = {32, 16, 32, 32};
        cfg.hasSwitch                = false;
        cfg.sw.mode                  = RoutingMode::HBR;
        cfg.sw.latencyNs             = 50.0;
        cfg.ras.enable               = false;
        cfg.device.coherenceOverheadNs = 0.0;
        cfg.device.ideEnable         = false;
        cfg.hostOverheadNs           = 97.0;
        cfg.fabric.p2p.enable        = false;
        cfg.fabric.gim.enable        = false;
        cfg.io.linkOneWayLatencyNs   = 30.0;
        cfg.io.dmaLinkBwGBps         = 31.5;  // PCIe Gen5×8 ≈ 31.5 GB/s

    } else if (cfg.cxlVersion == "3.0") {
        // PCIe Gen 6 PAM4, 256-byte FLITs, RS-FEC decode 4 ns/direction
        // Faster link ports offset by FEC: net unloaded latency ≈ Gen 5 + ~2 ns
        if (cfg.link.pcieGen == 0)    cfg.link.pcieGen    = 6;
        if (cfg.link.pcieLanes == 0)  cfg.link.pcieLanes  = 8;
        cfg.link.hostPortLatencyNs   = 12.0;
        cfg.link.linkPhysLatencyNs   = 2.0;
        cfg.link.devicePortLatencyNs = 10.0;
        cfg.link.linkLatencyNs       = 0.0;
        cfg.link.flitSizeBytes       = 256;   // PCIe 6.0 Flit Mode
        cfg.link.flitPayloadBytes    = 236;   // 256 - FEC parity (14B) - header (6B)
        cfg.link.fecEnable           = true;
        cfg.link.fecDecodeLatencyNs  = 4.0;   // RS-FEC decode pipeline ≈ 4 ns
        cfg.link.retryProbPerFlit    = 1e-10; // PAM4 post-FEC uncorrectable ≈ 1e-10/FLIT
        cfg.link.estimatedRttNs      = 215.0;
        cfg.link.vcArbitrationNs     = 1.5;   // CXL.io + CXL.mem + CXL.cache VC mux
        cfg.link.credits             = {32, 16, 32, 32};
        cfg.hasSwitch                = false;
        cfg.sw.mode                  = RoutingMode::PBR;
        cfg.sw.latencyNs             = 40.0;
        cfg.sw.numHops               = 1;
        cfg.ras.enable               = false;
        cfg.device.coherenceOverheadNs = 0.0;
        cfg.device.ideEnable         = false;
        cfg.hostOverheadNs           = 97.0;
        cfg.fabric.p2p.enable        = false;
        cfg.fabric.p2p.extraLinkLatencyNs = 24.0;  // Gen6 link
        cfg.fabric.p2p.extraSwitchLatNs   = 40.0;
        cfg.fabric.gim.enable        = false;
        cfg.fabric.gim.numDevices    = 2;
        cfg.fabric.gim.interleaveGran = 256;
        cfg.io.linkOneWayLatencyNs   = 24.0;   // Gen6 link
        cfg.io.dmaLinkBwGBps         = 32.0;   // PCIe6×8 with DMA overhead

    } else if (cfg.cxlVersion == "4.0") {
        // PCIe Gen 7 PAM4, 256-byte FLITs, RS-FEC, 128 GT/s — doubles Gen6 bandwidth
        // Halves port-serialization latency; same IDE + RAS profile as CXL 3.1
        if (cfg.link.pcieGen == 0)    cfg.link.pcieGen    = 7;
        if (cfg.link.pcieLanes == 0)  cfg.link.pcieLanes  = 8;
        cfg.link.hostPortLatencyNs   = 6.0;   // halved from Gen6's 12 ns
        cfg.link.linkPhysLatencyNs   = 2.0;
        cfg.link.devicePortLatencyNs = 5.0;   // halved from Gen6's 10 ns
        cfg.link.linkLatencyNs       = 0.0;
        cfg.link.flitSizeBytes       = 256;
        cfg.link.flitPayloadBytes    = 236;
        cfg.link.fecEnable           = true;
        cfg.link.fecDecodeLatencyNs  = 4.0;
        cfg.link.retryProbPerFlit    = 1e-10;
        cfg.link.estimatedRttNs      = 208.0;
        cfg.link.vcArbitrationNs     = 1.5;
        cfg.link.credits             = {32, 16, 32, 32};
        cfg.hasSwitch                = false;
        cfg.sw.mode                  = RoutingMode::PBR;
        cfg.sw.latencyNs             = 40.0;
        cfg.sw.numHops               = 1;
        cfg.ras.enable               = true;
        cfg.device.ideEnable         = true;
        cfg.device.ideLatencyNs      = 8.0;
        cfg.device.coherenceOverheadNs = 3.0;
        cfg.hostOverheadNs           = 97.0;
        cfg.fabric.p2p.enable        = false;
        cfg.fabric.p2p.extraLinkLatencyNs = 16.0;  // Gen7 link (halved)
        cfg.fabric.p2p.extraSwitchLatNs   = 40.0;
        cfg.fabric.gim.enable        = false;
        cfg.fabric.gim.numDevices    = 2;
        cfg.fabric.gim.interleaveGran = 256;
        cfg.io.linkOneWayLatencyNs   = 13.0;   // Gen7 link
        cfg.io.dmaLinkBwGBps         = 109.0;  // PCIe7×8 ≈ 109 GB/s
        cfg.device.bi.enable         = true;
        cfg.device.bi.biRttNs        = 200.0;  // reduced from 230: faster Gen7 link
        cfg.device.tsp.enable        = cfg.device.ideEnable;

    } else {  // 3.1 (default)
        // PCIe Gen 6 PAM4, 256-byte FLITs, RS-FEC, RAS subset + IDE + coherence overhead
        if (cfg.link.pcieGen == 0)    cfg.link.pcieGen    = 6;
        if (cfg.link.pcieLanes == 0)  cfg.link.pcieLanes  = 8;
        cfg.link.hostPortLatencyNs   = 12.0;
        cfg.link.linkPhysLatencyNs   = 2.0;
        cfg.link.devicePortLatencyNs = 10.0;
        cfg.link.linkLatencyNs       = 0.0;
        cfg.link.flitSizeBytes       = 256;
        cfg.link.flitPayloadBytes    = 236;
        cfg.link.fecEnable           = true;
        cfg.link.fecDecodeLatencyNs  = 4.0;
        cfg.link.retryProbPerFlit    = 1e-10;
        cfg.link.estimatedRttNs      = 230.0;  // slightly higher due to IDE path
        cfg.link.vcArbitrationNs     = 1.5;
        cfg.link.credits             = {32, 16, 32, 32};
        cfg.hasSwitch                = false;
        cfg.sw.mode                  = RoutingMode::PBR;
        cfg.sw.latencyNs             = 40.0;
        cfg.sw.numHops               = 1;
        cfg.ras.enable               = true;
        // IDE: AES-256-GCM mandatory for CXL 3.1 multi-tenant (TSP + IDE §4.5)
        cfg.device.ideEnable         = true;
        cfg.device.ideLatencyNs      = 8.0;
        // Coherence: host DCOH snoop-filter lookup on every CXL.mem request
        cfg.device.coherenceOverheadNs = 3.0;
        cfg.hostOverheadNs           = 97.0;
        cfg.fabric.p2p.enable        = false;
        cfg.fabric.p2p.extraLinkLatencyNs = 24.0;  // Gen6 link
        cfg.fabric.p2p.extraSwitchLatNs   = 40.0;
        cfg.fabric.gim.enable        = false;
        cfg.fabric.gim.numDevices    = 2;
        cfg.fabric.gim.interleaveGran = 256;
        cfg.io.linkOneWayLatencyNs   = 24.0;   // Gen6 link
        cfg.io.dmaLinkBwGBps         = 32.0;   // PCIe6×8 with DMA overhead
        // BI: enabled by default for CXL 3.1
        cfg.device.bi.enable         = true;
        cfg.device.bi.biRttNs        = 230.0;  // slightly higher RTT due to IDE overhead
        // TSP: enabled when IDE is enabled
        cfg.device.tsp.enable        = cfg.device.ideEnable;
    }
}

// ---------------------------------------------------------------------------
// Minimal key=value config parser
// ---------------------------------------------------------------------------

static void parseKV(SimConfig& cfg, const std::string& key, const std::string& val) {
    auto i = [&](){ return std::stoi(val); };
    auto d = [&](){ return std::stod(val); };
    auto b = [&](){ return val == "true" || val == "1" || val == "yes"; };

    if      (key == "cxl_version")            cfg.cxlVersion             = val;
    else if (key == "sim_mode")               cfg.simMode                = val;
    else if (key == "has_switch")             cfg.hasSwitch              = b();
    else if (key == "num_devices")            cfg.numDevices             = i();
    else if (key == "host_overhead_ns")       cfg.hostOverheadNs         = d();
    // device
    else if (key == "device.type")            cfg.device.type            = i();
    else if (key == "device.queue_depth")     cfg.device.queueDepth      = (uint32_t)i();
    else if (key == "device.controller_latency_ns") cfg.device.controllerLatencyNs = d();
    else if (key == "device.hdm_size_gb")     cfg.device.hdmSizeGB       = (uint64_t)i();
    else if (key == "device.num_channels")    cfg.device.numChannels     = i();
    else if (key == "device.num_ranks")       cfg.device.numRanks        = i();
    else if (key == "device.num_banks")       cfg.device.numBanks        = i();
    else if (key == "device.dram_backend")    cfg.device.dramBackendType = val;
    else if (key == "device.dram_type") {
        // Convenience: set backend type and pick timing preset
        cfg.device.dramBackendType = val;
        if      (val == "ddr5_4800") cfg.device.dramTiming = DRAMTimingParams::DDR5_4800();
        else if (val == "ddr5_6400") cfg.device.dramTiming = DRAMTimingParams::DDR5_6400();
        else if (val == "ddr4_3200") cfg.device.dramTiming = DRAMTimingParams::DDR4_3200();
        else if (val == "ddr4_2400") cfg.device.dramTiming = DRAMTimingParams::DDR4_2400();
        else if (val == "ddr5")      cfg.device.dramTiming = DRAMTimingParams::DDR5_4800();
        else if (val == "ddr4")      cfg.device.dramTiming = DRAMTimingParams::DDR4_3200();
    }
    else if (key == "device.dram_config_file") cfg.device.dramConfigFile = val;
    else if (key == "device.row_buffer_policy") cfg.device.rowBufferPolicy = val;
    // timing
    else if (key == "dram.tCK_ns")            cfg.device.dramTiming.tCK_ns  = d();
    else if (key == "dram.tCL_ns")            cfg.device.dramTiming.tCL_ns  = d();
    else if (key == "dram.tRCD_ns")           cfg.device.dramTiming.tRCD_ns = d();
    else if (key == "dram.tRP_ns")            cfg.device.dramTiming.tRP_ns  = d();
    // link (split fields)
    else if (key == "link.host_port_latency_ns")   cfg.link.hostPortLatencyNs   = d();
    else if (key == "link.link_phys_latency_ns")   cfg.link.linkPhysLatencyNs   = d();
    else if (key == "link.device_port_latency_ns") cfg.link.devicePortLatencyNs = d();
    else if (key == "link.pcie_gen")          cfg.link.pcieGen           = i();
    else if (key == "link.pcie_lanes")        cfg.link.pcieLanes         = i();
    else if (key == "link.latency_ns")        cfg.link.linkLatencyNs     = d();
    else if (key == "link.max_outstanding")   cfg.link.maxOutstanding    = i();
    // link FEC + retry
    else if (key == "link.fec_enable")             cfg.link.fecEnable            = b();
    else if (key == "link.fec_decode_latency_ns")  cfg.link.fecDecodeLatencyNs   = d();
    else if (key == "link.retry_prob_per_flit")    cfg.link.retryProbPerFlit     = d();
    else if (key == "link.retry_latency_ns")       cfg.link.retryLatencyNs       = d();
    else if (key == "link.estimated_rtt_ns")       cfg.link.estimatedRttNs       = d();
    else if (key == "link.vc_arbitration_ns")      cfg.link.vcArbitrationNs      = d();
    else if (key == "link.flit_size_bytes")        cfg.link.flitSizeBytes        = i();
    else if (key == "link.flit_payload_bytes")     cfg.link.flitPayloadBytes     = i();
    // link credit pools
    else if (key == "link.credits.m2s_req")   cfg.link.credits.m2sReq    = i();
    else if (key == "link.credits.m2s_rwd")   cfg.link.credits.m2sRwd    = i();
    else if (key == "link.credits.s2m_ndr")   cfg.link.credits.s2mNdr    = i();
    else if (key == "link.credits.s2m_drs")   cfg.link.credits.s2mDrs    = i();
    // switch
    else if (key == "switch.latency_ns")      cfg.sw.latencyNs           = d();
    else if (key == "switch.num_hops")        cfg.sw.numHops             = i();
    else if (key == "switch.queue_capacity_flits") cfg.sw.queueCapacityFlits = i();
    else if (key == "switch.mode") {
        cfg.sw.mode = (val == "pbr") ? RoutingMode::PBR : RoutingMode::HBR;
    }
    // device security / coherence
    else if (key == "device.coherence_overhead_ns") cfg.device.coherenceOverheadNs = d();
    else if (key == "device.ide_enable")       cfg.device.ideEnable       = b();
    else if (key == "device.ide_latency_ns")   cfg.device.ideLatencyNs    = d();
    // RAS
    else if (key == "ras.enable")             cfg.ras.enable             = b();
    else if (key == "ras.scrub_interval_us")  cfg.ras.patrolScrubIntervalUs = d();
    else if (key == "ras.patrol_scrub_interval_us") cfg.ras.patrolScrubIntervalUs = d();
    else if (key == "ras.patrol_scrub_bw_fraction") cfg.ras.patrolScrubBwFraction = d();
    else if (key == "ras.cvme_threshold")     cfg.ras.cvmeThreshold      = (uint32_t)i();
    else if (key == "ras.ppr_enable")         cfg.ras.pprEnable          = b();
    else if (key == "ras.sppr_enable")        cfg.ras.spprEnable         = b();
    else if (key == "ras.hppr_enable")        cfg.ras.hpprEnable         = b();
    else if (key == "ras.ppr_latency_us")     cfg.ras.spprLatencyUs      = d();
    else if (key == "ras.data_poison")        cfg.ras.dataPoisonEnable   = b();
    else if (key == "ras.viral")              cfg.ras.viralEnable        = b();
    else if (key == "ras.dbit_enable")        cfg.ras.dbitEnable         = b();
    else if (key == "ras.inject_enable")      cfg.ras.injectEnable       = b();
    else if (key == "ras.ce_rate")            cfg.ras.ceRate             = d();
    else if (key == "ras.uce_rate")           cfg.ras.uceRate            = d();
    // Correlated RAS fault model
    else if (key == "ras.fault_model") {
        cfg.ras.correlatedFaults.model =
            (val == "correlated") ? cxl::FaultModel::CORRELATED
                                  : cxl::FaultModel::INDEPENDENT;
    }
    else if (key == "ras.row_repeat_factor")  cfg.ras.correlatedFaults.rowRepeatFactor  = d();
    else if (key == "ras.aging_factor")       cfg.ras.correlatedFaults.agingFactor       = d();
    else if (key == "ras.burst_prob")         cfg.ras.correlatedFaults.burstProbability  = d();
    else if (key == "ras.row_hammer_enable")  cfg.ras.correlatedFaults.rowHammerEnable   = b();
    else if (key == "ras.row_hammer_thresh")  cfg.ras.correlatedFaults.rowHammerThresh   = static_cast<uint32_t>(i());
    // Fabric
    else if (key == "fabric.p2p_enable")         cfg.fabric.p2p.enable              = b();
    else if (key == "fabric.p2p_extra_hops")      cfg.fabric.p2p.numExtraHops        = i();
    else if (key == "fabric.p2p_extra_link_ns")   cfg.fabric.p2p.extraLinkLatencyNs  = d();
    else if (key == "fabric.p2p_extra_switch_ns") cfg.fabric.p2p.extraSwitchLatNs    = d();
    else if (key == "fabric.gim_enable")          cfg.fabric.gim.enable              = b();
    else if (key == "fabric.gim_num_devices")     cfg.fabric.gim.numDevices          = i();
    else if (key == "fabric.gim_interleave_gran") cfg.fabric.gim.interleaveGran      = static_cast<uint64_t>(d());
    else if (key == "fabric.gim_arbitration_ns")  cfg.fabric.gim.arbitrationNs       = d();
    // Bundled Port (CXL 4.0)
    else if (key == "bundled_port.enable")        cfg.link.bundledPortEnable         = b();
    else if (key == "bundled_port.num_ports")     cfg.link.bundledPortNumPorts       = i();
    // NUMA
    else if (key == "numa.num_nodes")             cfg.numa.numNodes                  = i();
    else if (key == "numa.upi_latency_ns")        cfg.numa.upiOneWayLatencyNs        = d();
    else if (key == "numa.requester_node")        cfg.requesterNUMANode              = i();
    // Migration
    else if (key == "migration.enable")           cfg.migration.enable               = b();
    else if (key == "migration.hot_threshold")    cfg.migration.hotAccessThreshold   = static_cast<uint32_t>(i());
    else if (key == "migration.cold_threshold")   cfg.migration.coldAccessThreshold  = static_cast<uint32_t>(i());
    else if (key == "migration.bw_gbps")          cfg.migration.migrationBwGBps      = d();
    // BI (back-invalidation)
    else if (key == "device.bi_enable")           cfg.device.bi.enable               = b();
    else if (key == "device.bi_rtt_ns")           cfg.device.bi.biRttNs              = d();
    // TSP
    else if (key == "device.tsp_enable")          cfg.device.tsp.enable              = b();
    else if (key == "device.tsp_key_rotation_mb") cfg.device.tsp.keyRotationBytes    = static_cast<uint64_t>(d() * 1024*1024);
    // injection mode
    else if (key == "inject.open_loop")      cfg.injectOpenLoop         = b();
    // simulation limits
    else if (key == "warmup_requests")        cfg.warmupRequests         = (uint64_t)i();
    else if (key == "max_requests")           cfg.maxRequests            = (uint64_t)i();
    else if (key == "output_dir")             cfg.outputDir              = val;
    else if (key == "run_tag")                cfg.runTag                 = val;
    else if (key == "stats_interval_ns")      cfg.statsIntervalNs        = d();
    // Python-engine-only keys: silently ignore rather than warn
    else if (key.substr(0, 10) == "multihost." ||
             key.substr(0, 10) == "migration." ||
             key == "numa.num_nodes" ||
             key == "dram.backend") { /* Python-only */ }
    else {
        std::fprintf(stderr, "[CXRay] Warning: unknown config key '%s' (ignored)\n", key.c_str());
    }
}

// Read config file into a key→value map (strips comments, whitespace, quotes).
static std::map<std::string, std::string> readConfigKV(const std::string& path) {
    std::map<std::string, std::string> kv;
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot open config file: " + path);

    auto trim = [](std::string& s) {
        size_t start = s.find_first_not_of(" \t\r\n\"");
        size_t end   = s.find_last_not_of(" \t\r\n\";");
        s = (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
    };

    std::string line;
    while (std::getline(f, line)) {
        auto comment = line.find('#');
        if (comment != std::string::npos) line = line.substr(0, comment);
        auto semi = line.find("//");
        if (semi != std::string::npos) line = line.substr(0, semi);

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        trim(key);
        trim(val);
        if (key.empty() || val.empty()) continue;
        kv[key] = val;
    }
    return kv;
}

[[maybe_unused]] static SimConfig parseConfigFileKV(const std::string& path) {
    SimConfig cfg;
    for (auto& [key, val] : readConfigKV(path))
        parseKV(cfg, key, val);
    return cfg;
}

// ---------------------------------------------------------------------------
// CXLMemController
// ---------------------------------------------------------------------------

CXLMemController::CXLMemController(const SimConfig& cfg) : cfg_(cfg) {
    buildTopology_();
}

CXLMemController CXLMemController::fromConfigFile(const std::string& cfgPath) {
    auto kv = readConfigKV(cfgPath);

    // Apply version defaults first so user values can override them.
    SimConfig cfg;
    if (kv.count("cxl_version")) cfg.cxlVersion = kv.at("cxl_version");
    applyVersionDefaults(cfg);

    // Overlay every explicit user key on top of the defaults.
    for (auto& [key, val] : kv)
        parseKV(cfg, key, val);

    if (cfg.link.pcieGen == 0)   cfg.link.pcieGen   = 5;
    if (cfg.link.pcieLanes == 0) cfg.link.pcieLanes = 8;
    return CXLMemController(cfg);
}

void CXLMemController::buildTopology_() {
    link_ = std::make_unique<CXLLink>(cfg_.link);

    if (cfg_.hasSwitch) {
        sw_ = std::make_unique<CXLSwitch>(cfg_.sw);
        // Propagate link parameters so the switch queuing model uses the right values
        sw_->setLinkBwGBps(link_->bandwidthGBps());
        sw_->setFlitSizeBytes(cfg_.link.flitSizeBytes);
        if (cfg_.numDevices == 1) {
            sw_->addHBREntry(0, UINT64_MAX, 0);
        }
    }

    // GIM: when GIM is enabled, the device-vector size must match the GIM device count.
    // If the user did not explicitly set num_devices, derive it from fabric.gim_num_devices.
    if (cfg_.fabric.gim.enable && cfg_.numDevices <= 1) {
        cfg_.numDevices = cfg_.fabric.gim.numDevices;
    }

    devices_.clear();
    for (int i = 0; i < std::max(1, cfg_.numDevices); i++) {
        devices_.push_back(
            std::make_unique<CXLDevice>(cfg_.device, cfg_.ras, /*seed=*/42 + i));
    }

    // Fabric (P2P + GIM)
    if (cfg_.fabric.p2p.enable || cfg_.fabric.gim.enable) {
        fabric_ = std::make_unique<CXLFabric>(cfg_.fabric);
    }

    // CXL.io controller
    ioCtrl_ = std::make_unique<CXLIOController>(cfg_.io);

    // NUMA model
    if (cfg_.numa.numNodes > 1 || cfg_.migration.enable) {
        numa_ = std::make_unique<NUMAModel>(cfg_.numa, cfg_.migration);
    }
}

// ---------------------------------------------------------------------------
// processRequest — single-request simulation with breakdown tracking
// ---------------------------------------------------------------------------

RequestResult CXLMemController::processRequest(const TraceRequest& req, double arrivalNs) {
    bool isWrite = (req.op == TraceOp::WRITE);

    // 1. Host TX overhead: host controller processing before placing flits on wire.
    // Applied before forward link serialization to match Python's pipeline model.
    double hostDispatchNs = arrivalNs + cfg_.hostOverheadNs;

    double creditStallNs = 0.0;
    double deviceArrivalNs = link_->enqueueRequest(
        hostDispatchNs, req.sizeBytes, creditStallNs, isWrite);

    // 1b. GIM address translation (if fabric_ and GIM enabled)
    double gimLatencyOffset = 0.0;
    int    gimDeviceIdx     = 0;
    uint64_t effectiveAddr  = req.addr;
    if (fabric_ && fabric_->hasGIM()) {
        auto tr = fabric_->translateGIM(req.addr);
        // Add per-device offset plus fixed fabric arbitration overhead.
        gimLatencyOffset = tr.latencyOffsetNs + fabric_->config().gim.arbitrationNs;
        // Route to the correct physical device and use the translated local address
        gimDeviceIdx = std::min(tr.deviceId, static_cast<int>(devices_.size()) - 1);
        effectiveAddr = tr.localAddr;
    }

    // 1c. P2P extra latency (if fabric_ and P2P)
    double p2pExtra = 0.0;
    if (fabric_ && fabric_->hasP2P()) {
        p2pExtra = fabric_->p2pExtraLatencyNs(cfg_.fabric.p2p.numExtraHops);
        stats_.p2pExtraLatencyNs += p2pExtra;
    }

    // Apply fabric latency offsets to device arrival time
    deviceArrivalNs += gimLatencyOffset + p2pExtra;

    // 2. Optional switch (pass arrival time so the queuing model is exercised)
    if (sw_) {
        uint8_t egressPort = 0;
        double swLatency   = sw_->route(req.addr, 0, egressPort, /*vc=*/1,
                                        deviceArrivalNs);
        deviceArrivalNs   += swLatency;
    }

    // 3. Device processing (controller + RAS + DRAM)
    bool rowHit = false;
    M2SReq m2s;
    m2s.opcode    = isWrite ? M2SOpcode::MemWr : M2SOpcode::MemRd;
    m2s.addr      = effectiveAddr;
    m2s.tag       = static_cast<uint8_t>(req.requesterId & 0xFF);
    m2s.arrivalNs = static_cast<uint64_t>(deviceArrivalNs);
    m2s.poison    = req.flagPoison;

    auto devResult = devices_[gimDeviceIdx]->processRequestDetailed(m2s, deviceArrivalNs, rowHit);
    double deviceDoneNs = deviceArrivalNs + devResult.totalNs;
    // Use devResult.coherenceNs in the breakdown (accumulated below)
    accBreakdown_.coherenceNs += devResult.coherenceNs;

    // 3b. Optional switch — return path (symmetric cut-through per hop).
    // Must mirror the forward traversal so each hop adds latency in both directions.
    if (sw_) {
        uint8_t egressPort = 0;
        deviceDoneNs += sw_->route(req.addr, 0, egressPort, /*vc=*/1, deviceDoneNs);
    }

    // 4. Response → Link RX
    // Write ACK is S2M_NDR (non-data response): serialize only a small FLIT header.
    // Read completion is S2M_DRS (data response): full cacheline payload.
    uint32_t respSizeBytes = isWrite ? 4u : req.sizeBytes;
    // extraReturnNs: host-side latency after link RX for S2M credit release timing.
    // host_overhead is now on the forward path, so only IDE decode counts here.
    double hostExtraNs = (cfg_.device.ideEnable ? cfg_.device.ideLatencyNs : 0.0);
    double hostDoneNs = link_->enqueueResponse(deviceDoneNs, respSizeBytes, !isWrite,
                                               VCType::CXL_MEM, hostExtraNs);

    // 4b. IDE host-side decode (return direction): host AES-GCM verifies the
    //     response payload before making data visible.
    if (cfg_.device.ideEnable) {
        hostDoneNs += cfg_.device.ideLatencyNs;
    }

    // host_overhead is applied on the forward path (step 1 above) and is thus
    // already included in the timing chain; do not add it again here.

    // 5b. Compute total latency before NUMA adjustment
    double totalLatNs = hostDoneNs - arrivalNs;

    // 5c. NUMA adjustment (after computing total latency)
    if (numa_) {
        auto nr = numa_->adjustLatency(cfg_.requesterNUMANode, 0, totalLatNs);
        double numaOffset = nr.numaOffsetNs;
        totalLatNs = nr.totalLatencyNs;
        stats_.numaOffsetNs += numaOffset;

        // Page migration — add the overhead to this request's latency
        double migrationOverhead = numa_->recordAccess(req.addr, arrivalNs);
        if (migrationOverhead > 0) {
            totalLatNs += migrationOverhead;
            stats_.pageMigrations++;
        }
    }

    // 5d. NUMA-aware effective latency (use effectiveLatency for pages that have migrated)
    if (numa_) {
        double localDramLat = cfg_.numa.nodes.empty() ? 114.0 : cfg_.numa.nodes[0].localDDR5LatNs;
        totalLatNs = numa_->effectiveLatency(req.addr, arrivalNs, totalLatNs, localDramLat);
    }

    // 6. Accumulate stats
    double totalLatency = totalLatNs;
    stats_.totalRequests++;
    if (isWrite) stats_.writes++;
    else         stats_.reads++;
    if (rowHit) stats_.rowHits++;
    else        stats_.rowMisses++;
    stats_.totalLatencyNs     += totalLatency;
    stats_.totalCreditStallNs += creditStallNs;
    if (totalLatency < stats_.minLatencyNs) stats_.minLatencyNs = totalLatency;
    if (totalLatency > stats_.maxLatencyNs) stats_.maxLatencyNs = totalLatency;
    stats_.latencySamples.push_back(totalLatency);
    lastArrivalNs_ = std::max(lastArrivalNs_, arrivalNs);

    // 7. Accumulate latency breakdown (approximate)
    accBreakdown_.hostPortNs    += cfg_.link.hostPortLatencyNs;
    accBreakdown_.linkPhysNs    += cfg_.link.linkPhysLatencyNs * 2.0;   // round-trip
    accBreakdown_.devicePortNs  += cfg_.link.devicePortLatencyNs;
    accBreakdown_.fecNs         += cfg_.link.fecEnable
                                   ? (cfg_.link.fecDecodeLatencyNs * 2.0) : 0.0;
    accBreakdown_.creditStallNs += creditStallNs;
    // Note: accBreakdown_.coherenceNs already accumulated from devResult.coherenceNs above
    accBreakdown_.rasNs         += devResult.rasNs;
    accBreakdown_.ideNs         += cfg_.device.ideEnable ? cfg_.device.ideLatencyNs : 0.0;
    // DRAM + controller share devResult.totalNs; strip out non-DRAM components
    double nonDram = cfg_.device.controllerLatencyNs
                   + devResult.coherenceNs
                   + devResult.rasNs
                   + (cfg_.device.ideEnable ? cfg_.device.ideLatencyNs : 0.0);
    double dramTime = devResult.totalNs - nonDram;
    if (dramTime < 0) dramTime = 0;
    accBreakdown_.dramNs   += dramTime;
    accBreakdown_.returnNs += cfg_.link.returnLinkLatencyNs()
                            + (cfg_.link.fecEnable ? cfg_.link.fecDecodeLatencyNs : 0.0);

    RequestResult result;
    result.timestampNs   = static_cast<uint64_t>(arrivalNs);
    // Use adjusted latency (after NUMA/migration) so closed-loop scheduling
    // sees the requester-visible completion, not the raw link completion.
    result.completeNs    = static_cast<uint64_t>(arrivalNs + totalLatency);
    result.latencyNs     = totalLatency;
    result.op            = req.op;
    result.addr          = req.addr;
    result.requesterId   = req.requesterId;
    result.rowHit        = rowHit;
    result.creditStallNs = creditStallNs;
    return result;
}

// ---------------------------------------------------------------------------
// runTrace
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Helper: parse trace file into request vector (reused by fast and detailed)
// ---------------------------------------------------------------------------

static std::vector<TraceRequest> parseTraceFile(
        const std::string& tracePath,
        uint64_t warmupRequests, uint64_t maxRequests,
        std::vector<std::pair<std::string, uint64_t>>& markers)
{
    std::ifstream f(tracePath);
    if (!f) throw std::runtime_error("Cannot open trace: " + tracePath);

    std::vector<TraceRequest> result;
    uint64_t lineNum = 0;
    uint64_t reqCount = 0;

    std::string line;
    while (std::getline(f, line)) {
        lineNum++;
        if (line.empty()) continue;

        if (line[0] == '#') {
            // Store raw marker line for later injection
            markers.push_back({line, lineNum});
            continue;
        }

        std::istringstream iss(line);
        TraceRequest req{};
        uint64_t tsNs;
        std::string opStr;
        uint64_t addr;
        uint32_t size;
        uint32_t rid;

        if (!(iss >> tsNs >> opStr >> std::hex >> addr >> std::dec >> size >> rid)) {
            std::cerr << "Warning: malformed trace line " << lineNum << ": " << line << "\n";
            continue;
        }

        req.timestampNs  = tsNs;
        req.op = (opStr == "W" || opStr == "WRITE" || opStr == "w")
                 ? TraceOp::WRITE : TraceOp::READ;
        req.addr         = addr;
        req.sizeBytes    = size;
        req.requesterId  = rid;
        std::string extra;
        if (iss >> extra && extra == "poison") req.flagPoison = true;

        reqCount++;
        if (maxRequests > 0 && reqCount > maxRequests + warmupRequests) break;
        if (reqCount <= warmupRequests) continue; // skip warmup for collection

        result.push_back(req);
    }
    return result;
}

SimStats CXLMemController::runTrace(const std::string& tracePath) {
    stats_        = SimStats{};
    accBreakdown_ = LatencyBreakdown{};
    lastCompletionNs_ = 0.0;

    // ── Phase 1: warmup (fast path, results discarded) ────────────────────
    if (cfg_.warmupRequests > 0) {
        std::ifstream fw(tracePath);
        if (!fw) throw std::runtime_error("Cannot open trace: " + tracePath);
        uint64_t warmCount = 0;
        std::unordered_map<uint32_t, double> warmReadyNs;
        std::string wline;
        while (std::getline(fw, wline) && warmCount < cfg_.warmupRequests) {
            if (wline.empty() || wline[0] == '#') continue;
            std::istringstream iss(wline);
            TraceRequest req{};
            uint64_t tsNs; std::string opStr; uint64_t addr; uint32_t size, rid;
            if (!(iss >> tsNs >> opStr >> std::hex >> addr >> std::dec >> size >> rid)) continue;
            req.timestampNs = tsNs;
            req.op = (opStr == "W" || opStr == "WRITE" || opStr == "w") ? TraceOp::WRITE : TraceOp::READ;
            req.addr = addr; req.sizeBytes = size; req.requesterId = rid;
            double readyNs = warmReadyNs.count(rid) ? warmReadyNs[rid] : 0.0;
            double effectiveArrNs = std::max(static_cast<double>(tsNs), readyNs);
            auto res = processRequest(req, effectiveArrNs);
            warmReadyNs[rid] = res.completeNs;
            warmCount++;
        }
        // Reset stats after warmup
        stats_        = SimStats{};
        accBreakdown_ = LatencyBreakdown{};
        lastCompletionNs_ = 0.0;
    }

    // ── Phase 2: load all requests ────────────────────────────────────────
    std::vector<std::pair<std::string, uint64_t>> markers;
    // warmup already processed; collect the remaining requests
    std::vector<TraceRequest> allRequests = parseTraceFile(
        tracePath, cfg_.warmupRequests, cfg_.maxRequests, markers);

    // Parse RAS markers into a sorted pending list — injection happens at
    // simulation time, not eagerly before the loop.  Injecting before
    // simulation would allow a marker at ts=500000 ns to block requests that
    // have not arrived yet (isBankBlocked checks blockUntil > nowNs).
    std::vector<PendingMarker> pendingMarkers;
    for (auto& [mline, mlineno] : markers) {
        (void)mlineno;
        PendingMarker pm;
        auto tsPos = mline.find("ts=");
        if (tsPos != std::string::npos) pm.ts = std::stoull(mline.substr(tsPos + 3));

        if (mline.find("PPR_MARKER") != std::string::npos) {
            pm.isPPR  = true;
            pm.isHard = (mline.find("hard_ppr") != std::string::npos);
            auto addrPos = mline.find("target_row=0x");
            if (addrPos != std::string::npos)
                pm.addr = std::stoull(mline.substr(addrPos + 13), nullptr, 16);
            pendingMarkers.push_back(pm);
        } else if (mline.find("CE_INJECT") != std::string::npos) {
            pm.isPPR = false;
            auto addrPos = mline.find("addr=0x");
            if (addrPos != std::string::npos)
                pm.addr = std::stoull(mline.substr(addrPos + 7), nullptr, 16);
            pendingMarkers.push_back(pm);
        }
    }
    std::sort(pendingMarkers.begin(), pendingMarkers.end(),
              [](const PendingMarker& a, const PendingMarker& b){ return a.ts < b.ts; });

    // Helper: fire all markers whose timestamp <= nowNs.
    // Routes each marker to the device responsible for the marker address
    // (GIM interleave translation), so PPR/CE events target the correct DRAM.
    size_t nextMarker = 0;
    auto injectDueMarkers = [&](double nowNs) {
        while (nextMarker < pendingMarkers.size() &&
               pendingMarkers[nextMarker].ts <= static_cast<uint64_t>(nowNs)) {
            const auto& pm = pendingMarkers[nextMarker++];
            if (!devices_.empty() && cfg_.ras.enable) {
                int devIdx = 0;
                uint64_t localAddr = pm.addr;
                if (fabric_ && fabric_->hasGIM()) {
                    auto tr = fabric_->translateGIM(pm.addr);
                    devIdx    = std::min(tr.deviceId, static_cast<int>(devices_.size()) - 1);
                    localAddr = tr.localAddr;
                }
                auto* ddr5 = dynamic_cast<DDR5Backend*>(devices_[devIdx]->dram());
                if (pm.isPPR)
                    devices_[devIdx]->ras().injectPPR(localAddr, static_cast<double>(pm.ts), pm.isHard, ddr5);
                else
                    devices_[devIdx]->ras().injectCE(localAddr, static_cast<double>(pm.ts));
            }
        }
    };

    // ── Dispatch to detailed or fast mode ────────────────────────────────
    if (cfg_.simMode == "detailed") {
        return runTraceDetailed_(allRequests, pendingMarkers);
    }

    // ── Fast (analytical) mode ────────────────────────────────────────────
    double firstArrival = -1.0;
    std::unordered_map<uint32_t, double> requesterReadyNs;

    for (const auto& req : allRequests) {
        uint32_t rid = req.requesterId;
        double readyNs = (!cfg_.injectOpenLoop && requesterReadyNs.count(rid))
                         ? requesterReadyNs[rid] : 0.0;
        double effectiveArrivalNs = std::max(static_cast<double>(req.timestampNs), readyNs);

        // Inject any RAS markers whose time has arrived before processing the request
        injectDueMarkers(effectiveArrivalNs);

        if (firstArrival < 0) firstArrival = effectiveArrivalNs;
        auto res = processRequest(req, effectiveArrivalNs);
        requesterReadyNs[rid] = res.completeNs;
        if (res.completeNs > lastCompletionNs_) lastCompletionNs_ = res.completeNs;
    }

    // Compute simulation duration using last *completion* time (accurate for BW calc)
    if (firstArrival >= 0) {
        stats_.simDurationNs = lastCompletionNs_ - firstArrival;
        if (stats_.simDurationNs <= 0) stats_.simDurationNs = 1.0;
    }

    // Average latency breakdown
    if (stats_.totalRequests > 0) {
        double n = static_cast<double>(stats_.totalRequests);
        stats_.latencyBreakdown.hostPortNs    = accBreakdown_.hostPortNs    / n;
        stats_.latencyBreakdown.linkPhysNs    = accBreakdown_.linkPhysNs    / n;
        stats_.latencyBreakdown.devicePortNs  = accBreakdown_.devicePortNs  / n;
        stats_.latencyBreakdown.fecNs         = accBreakdown_.fecNs         / n;
        stats_.latencyBreakdown.dramNs        = accBreakdown_.dramNs        / n;
        stats_.latencyBreakdown.rasNs         = accBreakdown_.rasNs         / n;
        stats_.latencyBreakdown.coherenceNs   = accBreakdown_.coherenceNs   / n;
        stats_.latencyBreakdown.ideNs         = accBreakdown_.ideNs         / n;
        stats_.latencyBreakdown.returnNs      = accBreakdown_.returnNs      / n;
        stats_.latencyBreakdown.creditStallNs = accBreakdown_.creditStallNs / n;
    }

    // Link utilization (wire-based: full flit occupancy, both M2S and S2M directions)
    stats_.linkUtilizationFrac = link_->upstreamUtilization(stats_.simDurationNs);
    // Include S2M response-credit stall in headline stall total.
    stats_.totalCreditStallNs += link_->stats().s2mStallNs;

    // Effective bandwidth: min(raw, link × 95%)
    double linkBwGBps = link_->bandwidthGBps();
    double rawBw      = stats_.bandwidthGBps();
    stats_.effectiveBwGBps = std::min(rawBw, linkBwGBps * 0.95);

    return stats_;
}

// ---------------------------------------------------------------------------
// runTraceDetailed_ — DES-engine (event-driven) simulation
// ---------------------------------------------------------------------------

SimStats CXLMemController::runTraceDetailed_(const std::vector<TraceRequest>& requests,
                                             const std::vector<PendingMarker>& markers) {
    SimEventQueue evq;

    // cap = 0 means credit limiting disabled; use a large sentinel so acquire()
    // always fires immediately without branching in the hot path.
    static constexpr int kUnlimitedCredits = 1 << 20;
    int m2sReqCap = cfg_.link.credits.m2sReq;
    int m2sRwdCap = cfg_.link.credits.m2sRwd;
    CreditTracker m2sReqCredits(evq, m2sReqCap > 0 ? m2sReqCap : kUnlimitedCredits,
                                 cfg_.link.estimatedRttNs);
    CreditTracker m2sRwdCredits(evq, m2sRwdCap > 0 ? m2sRwdCap : kUnlimitedCredits,
                                 cfg_.link.estimatedRttNs);

    // S2M response credit pools (DRS = read completions, NDR = write ACKs).
    // returnNs = 0.0: disable CreditTracker auto-return; credits are released
    // explicitly after each response traverses the return link.
    int s2mDrsCap = cfg_.link.credits.s2mDrs;
    int s2mNdrCap = cfg_.link.credits.s2mNdr;
    CreditTracker s2mDrsCredits(evq, s2mDrsCap > 0 ? s2mDrsCap : kUnlimitedCredits, 0.0);
    CreditTracker s2mNdrCredits(evq, s2mNdrCap > 0 ? s2mNdrCap : kUnlimitedCredits, 0.0);

    FlitSerializer reqLane (evq, link_->flitTxTimeNs());
    FlitSerializer respLane(evq, link_->flitTxTimeNs());

    // Detailed-mode S2M accounting (reported to LinkStats after evq.runAll()).
    uint64_t s2mDrsConsumed = 0;
    uint64_t s2mNdrConsumed = 0;
    double   s2mTotalStallNs = 0.0;

    // One DRAM queue per physical device — required for multi-device GIM correctness.
    std::vector<std::unique_ptr<DetailedDRAMQueue>> dramQueues;
    for (auto& dev : devices_)
        dramQueues.push_back(std::make_unique<DetailedDRAMQueue>(evq, *dev->dram()));

    // Schedule RAS marker injections as timed events so they fire at the correct
    // simulation time rather than before the first request has even arrived.
    // Route each marker to the physical device selected by GIM address translation.
    for (const auto& pm : markers) {
        evq.schedule(static_cast<double>(pm.ts), [this, pm]() {
            if (!devices_.empty() && cfg_.ras.enable) {
                int devIdx = 0;
                uint64_t localAddr = pm.addr;
                if (fabric_ && fabric_->hasGIM()) {
                    auto tr = fabric_->translateGIM(pm.addr);
                    devIdx    = std::min(tr.deviceId, static_cast<int>(devices_.size()) - 1);
                    localAddr = tr.localAddr;
                }
                auto* ddr5 = dynamic_cast<DDR5Backend*>(devices_[devIdx]->dram());
                if (pm.isPPR)
                    devices_[devIdx]->ras().injectPPR(localAddr, static_cast<double>(pm.ts),
                                                       pm.isHard, ddr5);
                else
                    devices_[devIdx]->ras().injectCE(localAddr, static_cast<double>(pm.ts));
            }
        }, /*priority=*/0);  // priority 0: fires before request arrivals at same time
    }

    double firstArrival = -1.0;

    // Per-requester scheduling.
    // Closed-loop (injectOpenLoop=false): dispatch at max(timestamp, prev_completion).
    // Open-loop  (injectOpenLoop=true):  dispatch at trace timestamp directly.
    //
    // Build per-requester queues and schedule the first request of each.
    // The completion callback for request N chains to request N+1 for that rid.
    std::unordered_map<uint32_t, std::queue<TraceRequest>> perRidQueues;
    for (const auto& req : requests)
        perRidQueues[req.requesterId].push(req);

    using ScheduleFn = std::function<void(uint32_t, double)>;
    auto scheduleFnPtr = std::make_shared<ScheduleFn>();

    *scheduleFnPtr = [&, scheduleFnPtr](uint32_t rid, double readyNs) {
        auto it = perRidQueues.find(rid);
        if (it == perRidQueues.end() || it->second.empty()) return;
        TraceRequest req = it->second.front();
        it->second.pop();

        double tsNs = static_cast<double>(req.timestampNs);
        double arrivalNs = cfg_.injectOpenLoop ? tsNs : std::max(tsNs, readyNs);
        if (firstArrival < 0) firstArrival = arrivalNs;

        evq.schedule(arrivalNs, [this, req, &evq, scheduleFnPtr,
                                  &m2sReqCredits, &m2sRwdCredits,
                                  &s2mDrsCredits, &s2mNdrCredits,
                                  s2mDrsCap, s2mNdrCap,
                                  &s2mDrsConsumed, &s2mNdrConsumed, &s2mTotalStallNs,
                                  &reqLane, &respLane, &dramQueues,
                                  arrivalNs]() mutable {
            bool isWrite = (req.op == TraceOp::WRITE);
            CreditTracker& credits = isWrite ? m2sRwdCredits : m2sReqCredits;

            int numFlits = static_cast<int>(
                std::ceil(static_cast<double>(req.sizeBytes) /
                          static_cast<double>(cfg_.link.flitPayloadBytes)));

            // Stage 1: host TX overhead before credit acquisition, then acquire credit.
            // host_overhead is on the forward path (matching Python); the credit is
            // requested at arrivalNs + hostOverheadNs so the link sees the correct
            // dispatch time and serialization contention is accurate under load.
            double hostDispatchNs = arrivalNs + cfg_.hostOverheadNs;
            credits.acquire(hostDispatchNs,
                [this, req, &evq, &reqLane, &respLane, &dramQueues,
                 &s2mDrsCredits, &s2mNdrCredits, s2mDrsCap, s2mNdrCap,
                 &s2mDrsConsumed, &s2mNdrConsumed, &s2mTotalStallNs,
                 isWrite, arrivalNs, numFlits, scheduleFnPtr](double grantNs) {

                double creditStallNs = std::max(0.0, grantNs - (arrivalNs + cfg_.hostOverheadNs));

                // Stage 2: serialize FLITs on request lane → fires onComplete event
                reqLane.scheduleTx(grantNs, numFlits,
                    [this, req, &evq, &respLane, &dramQueues,
                     &s2mDrsCredits, &s2mNdrCredits, s2mDrsCap, s2mNdrCap,
                     &s2mDrsConsumed, &s2mNdrConsumed, &s2mTotalStallNs,
                     isWrite, arrivalNs, creditStallNs, numFlits, scheduleFnPtr](double txDoneNs) {

                    // Accumulate M2S wire bytes (full flit occupancy).
                    link_->addWireBytes(
                        static_cast<uint64_t>(numFlits) * cfg_.link.flitSizeBytes, 0);

                    double vcNs  = cfg_.link.vcArbitrationNs;
                    double fecNs = cfg_.link.fecEnable ? cfg_.link.fecDecodeLatencyNs : 0.0;
                    double deviceArrivalNs = txDoneNs + vcNs
                                           + cfg_.link.effectiveLinkLatencyNs() + fecNs;

                    if (sw_) {
                        uint8_t ep = 0;
                        // Pass txDoneNs so the time-based switch queue model fires
                        deviceArrivalNs += sw_->route(req.addr, 0, ep, /*vc=*/1, txDoneNs);
                    }

                    // Stage 2b: GIM translation (mirrors fast-path processRequest logic)
                    int    gimDevIdx    = 0;
                    uint64_t devAddr    = req.addr;
                    if (fabric_ && fabric_->hasGIM()) {
                        auto tr  = fabric_->translateGIM(req.addr);
                        // Add per-device offset + fabric arbitration (mirrors fast path)
                        deviceArrivalNs += tr.latencyOffsetNs
                                         + fabric_->config().gim.arbitrationNs;
                        gimDevIdx = std::min(tr.deviceId,
                                            static_cast<int>(devices_.size()) - 1);
                        devAddr   = tr.localAddr;
                    }

                    // Stage 2c: pre-DRAM overhead (controller + coherence + RAS + DCOH).
                    // Compute this before submitting to DRAM so the DRAM receives the
                    // correct start time and no second DRAM request is issued.
                    M2SReq m2s;
                    m2s.opcode    = isWrite ? M2SOpcode::MemWr : M2SOpcode::MemRd;
                    m2s.addr      = devAddr;
                    m2s.tag       = static_cast<uint8_t>(req.requesterId & 0xFF);
                    m2s.arrivalNs = static_cast<uint64_t>(deviceArrivalNs);
                    m2s.poison    = req.flagPoison;
                    double coherenceNs = 0.0;
                    double preDRAMNs   = devices_[gimDevIdx]->computePreDRAMNs(
                                             m2s, deviceArrivalNs, coherenceNs);
                    double dramStartNs = deviceArrivalNs + preDRAMNs;

                    // Stage 3: DRAM access on the correct per-device queue
                    dramQueues[gimDevIdx]->submit(devAddr, isWrite, dramStartNs,
                        [this, req, &evq, &respLane,
                         &s2mDrsCredits, &s2mNdrCredits,
                         &s2mDrsConsumed, &s2mNdrConsumed, &s2mTotalStallNs,
                         dramStartNs, creditStallNs, isWrite, arrivalNs, scheduleFnPtr]
                        (double dramDoneNs) mutable {

                        // Row-hit heuristic: compare DRAM service latency to
                        // the row-hit threshold from the configured timing params.
                        double dramLatNs  = dramDoneNs - dramStartNs;
                        double hitThresh  = cfg_.device.dramTiming.rowHitLatNs()
                                         + cfg_.device.dramTiming.tCCD_L_ns + 2.0;
                        bool rowHit = (dramLatNs <= hitThresh);

                        // Device is done with DRAM.  IDE decode happens host-side
                        // after the return link traversal (same placement as fast path).
                        double deviceDoneNs = dramDoneNs;

                        // Return switch traversal (mirrors fast-path C4 fix)
                        if (sw_) {
                            uint8_t ep = 0;
                            deviceDoneNs += sw_->route(req.addr, 0, ep, /*vc=*/1,
                                                        deviceDoneNs);
                        }

                        // Stage 4: acquire S2M credit, then serialize response FLITs.
                        // Write ACK (NDR) has no data payload; use 4 B header only.
                        uint32_t rspBytes = isWrite ? 4u : req.sizeBytes;
                        int rspFlits = static_cast<int>(
                            std::ceil(static_cast<double>(rspBytes) /
                                      cfg_.link.flitPayloadBytes));
                        uint64_t s2mWireBytes =
                            static_cast<uint64_t>(rspFlits) * cfg_.link.flitSizeBytes;
                        CreditTracker* s2mPool = isWrite ? &s2mNdrCredits : &s2mDrsCredits;

                        s2mPool->acquire(deviceDoneNs,
                            [this, s2mPool, rspFlits, creditStallNs, isWrite, rowHit,
                             arrivalNs, deviceDoneNs, scheduleFnPtr, rid = req.requesterId,
                             s2mWireBytes, &respLane,
                             &s2mDrsConsumed, &s2mNdrConsumed, &s2mTotalStallNs](double s2mGrantNs) {

                            double s2mStallNs = std::max(0.0, s2mGrantNs - deviceDoneNs);
                            // Track S2M consumption and stall for JSON reporting.
                            if (isWrite) ++s2mNdrConsumed; else ++s2mDrsConsumed;
                            s2mTotalStallNs += s2mStallNs;

                            respLane.scheduleTx(s2mGrantNs, rspFlits,
                                [this, s2mPool, creditStallNs, s2mStallNs, isWrite, rowHit,
                                 arrivalNs, scheduleFnPtr, rid, s2mWireBytes](double respTxDoneNs) {

                                double fecNs = cfg_.link.fecEnable
                                             ? cfg_.link.fecDecodeLatencyNs : 0.0;
                                double returnNs = cfg_.link.returnLinkLatencyNs() + fecNs;
                                double ideNs = cfg_.device.ideEnable ? cfg_.device.ideLatencyNs : 0.0;
                                // host_overhead is on the forward path; S2M credit releases at
                                // response-link-rx + IDE (no additional host overhead here).
                                s2mPool->release(respTxDoneNs + returnNs + ideNs);
                                link_->addWireBytes(0, s2mWireBytes);

                                double hostDoneNs = respTxDoneNs + returnNs + ideNs;
                                double totalLatNs = hostDoneNs - arrivalNs;

                                if (numa_) {
                                    auto nr = numa_->adjustLatency(
                                        cfg_.requesterNUMANode, 0, totalLatNs);
                                    totalLatNs = nr.totalLatencyNs;
                                    stats_.numaOffsetNs += nr.numaOffsetNs;
                                }

                                stats_.totalRequests++;
                                if (isWrite) stats_.writes++;
                                else         stats_.reads++;
                                if (rowHit)  stats_.rowHits++;
                                else         stats_.rowMisses++;
                                stats_.totalLatencyNs     += totalLatNs;
                                stats_.totalCreditStallNs += creditStallNs + s2mStallNs;
                                if (totalLatNs < stats_.minLatencyNs)
                                    stats_.minLatencyNs = totalLatNs;
                                if (totalLatNs > stats_.maxLatencyNs)
                                    stats_.maxLatencyNs = totalLatNs;
                                stats_.latencySamples.push_back(totalLatNs);
                                lastCompletionNs_ = std::max(lastCompletionNs_, hostDoneNs);

                                // Closed-loop: schedule next request for this requester
                                (*scheduleFnPtr)(rid, hostDoneNs);
                            });
                        });
                    });
                });
            });
        }, /*priority=*/1);
    };  // end of *scheduleFnPtr lambda

    // Kick off the first request for each requester
    for (auto& [rid, _] : perRidQueues)
        (*scheduleFnPtr)(rid, 0.0);

    evq.runAll();
    *scheduleFnPtr = ScheduleFn{};  // break self-referential shared_ptr cycle

    if (firstArrival >= 0) {
        stats_.simDurationNs = lastCompletionNs_ - firstArrival;
        if (stats_.simDurationNs <= 0) stats_.simDurationNs = 1.0;
    }

    // Report detailed-mode S2M credit stats to LinkStats for JSON output.
    link_->addS2mStats(s2mDrsConsumed, s2mNdrConsumed, s2mTotalStallNs);

    // Link utilization: wire bytes were accumulated via link_->addWireBytes() above.
    stats_.linkUtilizationFrac = link_->upstreamUtilization(stats_.simDurationNs);

    double linkBwGBps = link_->bandwidthGBps();
    double rawBw      = stats_.bandwidthGBps();
    stats_.effectiveBwGBps = std::min(rawBw, linkBwGBps * 0.95);

    return stats_;
}

// ---------------------------------------------------------------------------
// printStats
// ---------------------------------------------------------------------------

void CXLMemController::printStats() const {
    const auto& s = stats_;
    std::cout << "\n=== CXRay Simulation Results ===\n";
    std::cout << "  Run tag        : " << (cfg_.runTag.empty() ? "(none)" : cfg_.runTag) << "\n";
    std::cout << "  CXL version    : " << cfg_.cxlVersion << "\n";
    std::cout << "  Requests       : " << s.totalRequests
              << "  (R=" << s.reads << " W=" << s.writes << ")\n";
    std::cout << "  Mean latency   : " << std::fixed << std::setprecision(2)
              << s.meanLatencyNs() << " ns\n";
    std::cout << "  Min latency    : " << s.minLatencyNs << " ns\n";
    std::cout << "  Max latency    : " << s.maxLatencyNs << " ns\n";
    std::cout << "  P50 latency    : " << s.p50LatencyNs() << " ns\n";
    std::cout << "  P95 latency    : " << s.p95LatencyNs() << " ns\n";
    std::cout << "  P99 latency    : " << s.p99LatencyNs() << " ns\n";
    std::cout << "  Row hit rate   : " << std::setprecision(1)
              << s.rowHitRate() * 100.0 << "%\n";
    std::cout << "  Bandwidth      : " << std::setprecision(3)
              << s.bandwidthGBps() << " GB/s\n";
    std::cout << "  Effective BW   : " << s.effectiveBwGBps << " GB/s\n";
    std::cout << "  Link util      : " << std::setprecision(1)
              << s.linkUtilizationFrac * 100.0 << "%\n";
    std::cout << "  Credit stalls  : " << std::setprecision(2)
              << s.totalCreditStallNs << " ns total\n";

    // Latency breakdown — not accumulated in detailed mode (DES events bypass the analytical path)
    if (cfg_.simMode == "detailed") {
        std::cout << "  Latency breakdown: unavailable in detailed mode\n";
    } else {
        const auto& bd = s.latencyBreakdown;
        std::cout << "  Latency breakdown (avg per request):\n";
        std::cout << "    Host port    : " << std::setprecision(2) << bd.hostPortNs   << " ns\n";
        std::cout << "    Link phys    : " << bd.linkPhysNs    << " ns\n";
        std::cout << "    Device port  : " << bd.devicePortNs  << " ns\n";
        if (bd.fecNs > 0)
            std::cout << "    FEC decode   : " << bd.fecNs << " ns\n";
        std::cout << "    Coherence    : " << bd.coherenceNs   << " ns\n";
        std::cout << "    DRAM         : " << bd.dramNs        << " ns\n";
        std::cout << "    RAS          : " << bd.rasNs         << " ns\n";
        if (bd.ideNs > 0)
            std::cout << "    IDE (AES)    : " << bd.ideNs << " ns\n";
        std::cout << "    Return path  : " << bd.returnNs      << " ns\n";
        std::cout << "    Credit stall : " << bd.creditStallNs << " ns\n";
    }
    if (stats_.p2pExtraLatencyNs > 0)
        std::cout << "    P2P overhead : " << stats_.p2pExtraLatencyNs / stats_.totalRequests << " ns/req\n";
    if (stats_.numaOffsetNs > 0)
        std::cout << "    NUMA offset  : " << stats_.numaOffsetNs / stats_.totalRequests << " ns/req\n";
    if (stats_.pageMigrations > 0)
        std::cout << "  Page migrations: " << stats_.pageMigrations << "\n";

    // DRAM sub-stats: aggregate across all devices (GIM uses multiple devices)
    if (!devices_.empty()) {
        uint64_t totalHits = 0, totalMisses = 0, totalConflicts = 0;
        uint64_t totalCE = 0, totalUCE = 0, totalSPPR = 0, totalHPPR = 0;
        uint64_t totalPoison = 0, totalDBIT = 0;
        double   totalBW = 0.0;
        bool anyViral = false;
        for (const auto& devPtr : devices_) {
            const auto& dev = *devPtr;
            totalHits      += dev.dramRowHits();
            totalMisses    += dev.dramRowMisses();
            totalConflicts += dev.dramRowConflicts();
            totalBW        += dev.dramBandwidthGBps();
            const auto& ras = dev.ras();
            totalCE    += ras.correctedErrors();
            totalUCE   += ras.uncorrectedErrors();
            totalSPPR  += ras.sPPREvents();
            totalHPPR  += ras.hPPREvents();
            totalPoison += ras.poisonedLines();
            totalDBIT   += ras.dbitRows();
            anyViral    |= ras.isViralActive();
        }
        if (devices_.size() > 1)
            std::cout << "  DRAM devices   : " << devices_.size() << " (GIM aggregate)\n";
        std::cout << "  DRAM row hits  : " << totalHits << "\n";
        std::cout << "  DRAM row miss  : " << totalMisses << "\n";
        std::cout << "  DRAM row conf  : " << totalConflicts << "\n";
        std::cout << "  DRAM BW (agg)  : " << totalBW << " GB/s\n";
        std::cout << "  RAS: CE events : " << totalCE << "\n";
        std::cout << "  RAS: UCE events: " << totalUCE << "\n";
        std::cout << "  RAS: sPPR      : " << totalSPPR << "\n";
        std::cout << "  RAS: hPPR      : " << totalHPPR << "\n";
        std::cout << "  RAS: Viral     : " << (anyViral ? "YES" : "no") << "\n";
        std::cout << "  RAS: Poisoned  : " << totalPoison << " lines\n";
        std::cout << "  RAS: DBIT rows : " << totalDBIT << "\n";
    }
    std::cout << "================================\n\n";
}

// ---------------------------------------------------------------------------
// printResolvedConfig
// ---------------------------------------------------------------------------

void CXLMemController::printResolvedConfig() const {
    const auto& c = cfg_;
    auto fmtD = [](const std::string& lbl, double v, bool isDefault, const std::string& unit="ns") {
        std::cout << "  " << std::left << std::setw(40) << lbl
                  << " = " << std::fixed << std::setprecision(2) << v << " " << unit
                  << "  [" << (isDefault ? "DEFAULT" : "USER") << "]\n";
    };
    auto fmtI = [](const std::string& lbl, int64_t v, bool isDefault) {
        std::cout << "  " << std::left << std::setw(40) << lbl
                  << " = " << v
                  << "  [" << (isDefault ? "DEFAULT" : "USER") << "]\n";
    };
    auto fmtS = [](const std::string& lbl, const std::string& v, bool isDefault) {
        std::cout << "  " << std::left << std::setw(40) << lbl
                  << " = " << v
                  << "  [" << (isDefault ? "DEFAULT" : "USER") << "]\n";
    };

    std::cout << "\n=== CXRay Resolved Configuration ===\n";
    fmtS("cxl_version",              c.cxlVersion,       false);
    fmtS("sim_mode",                 c.simMode,          c.simMode == "fast");
    fmtI("num_devices",              c.numDevices,       c.numDevices == 1);
    fmtI("device.type",              c.device.type,      c.device.type == 3);
    fmtI("device.hdm_size_gb",       c.device.hdmSizeGB, c.device.hdmSizeGB == 128);
    fmtD("device.controller_latency_ns", c.device.controllerLatencyNs,
         c.device.controllerLatencyNs == 50.0);
    fmtI("device.num_channels",      c.device.numChannels,  c.device.numChannels == 1);
    fmtI("device.num_ranks",         c.device.numRanks,     c.device.numRanks == 2);
    fmtI("device.num_banks",         c.device.numBanks,     c.device.numBanks == 16);
    fmtS("device.dram_backend",      c.device.dramBackendType, c.device.dramBackendType == "simple");
    fmtD("dram.tCK_ns",              c.device.dramTiming.tCK_ns,  true);
    fmtD("dram.tCL_ns",              c.device.dramTiming.tCL_ns,  true);
    fmtD("dram.tRCD_ns",             c.device.dramTiming.tRCD_ns, true);
    fmtD("dram.tRP_ns",              c.device.dramTiming.tRP_ns,  true);
    fmtD("dram.tRAS_ns",             c.device.dramTiming.tRAS_ns, true);
    fmtI("link.pcie_gen",            c.link.pcieGen,       false);
    fmtI("link.pcie_lanes",          c.link.pcieLanes,     false);
    fmtD("link.host_port_latency_ns", c.link.hostPortLatencyNs,  false);
    fmtD("link.link_phys_latency_ns", c.link.linkPhysLatencyNs,  false);
    fmtD("link.device_port_latency_ns", c.link.devicePortLatencyNs, false);
    fmtD("host_overhead_ns",         c.hostOverheadNs,     c.hostOverheadNs == 97.0);
    fmtS("inject.open_loop",         c.injectOpenLoop ? "true" : "false", c.injectOpenLoop);
    fmtI("link.max_outstanding",     c.link.maxOutstanding, c.link.maxOutstanding == 256);
    {
        double bw = CXLLink::computeBandwidthGBps(c.link.pcieGen, c.link.pcieLanes);
        if (c.link.bundledPortEnable && c.link.bundledPortNumPorts > 1)
            bw *= c.link.bundledPortNumPorts;
        std::cout << "  " << std::left << std::setw(40) << "link.bandwidth_gbps [COMPUTED]"
                  << " = " << std::fixed << std::setprecision(2) << bw << " GB/s\n";
    }
    fmtS("bundled_port.enable",       c.link.bundledPortEnable ? "true" : "false",
                                       !c.link.bundledPortEnable);
    if (c.link.bundledPortEnable) {
        fmtI("bundled_port.num_ports", c.link.bundledPortNumPorts, false);
    }
    fmtS("fabric.gim_enable",         c.fabric.gim.enable ? "true" : "false",
                                       !c.fabric.gim.enable);
    if (c.fabric.gim.enable) {
        fmtI("fabric.gim_num_devices",    c.fabric.gim.numDevices,    false);
        fmtD("fabric.gim_arbitration_ns", c.fabric.gim.arbitrationNs, c.fabric.gim.arbitrationNs == 0.0);
        fmtI("num_devices [DERIVED]",     c.numDevices, false);
    }
    fmtS("has_switch",               c.hasSwitch ? "true" : "false", false);
    if (c.hasSwitch) {
        fmtD("switch.latency_ns",    c.sw.latencyNs, false);
        fmtS("switch.mode",          c.sw.mode == RoutingMode::PBR ? "pbr" : "hbr", false);
    }
    fmtS("ras.enable",               c.ras.enable ? "true" : "false", false);
    if (c.ras.enable) {
        fmtD("ras.scrub_interval_us", c.ras.patrolScrubIntervalUs, false, "us");
        fmtS("ras.sppr_enable",      c.ras.spprEnable ? "true" : "false", false);
        fmtS("ras.hppr_enable",      c.ras.hpprEnable ? "true" : "false", false);
        fmtS("ras.data_poison",      c.ras.dataPoisonEnable ? "true" : "false", false);
        fmtS("ras.viral",            c.ras.viralEnable ? "true" : "false", false);
        fmtS("ras.dbit_enable",      c.ras.dbitEnable ? "true" : "false", false);
    }
    fmtI("warmup_requests",          c.warmupRequests,  c.warmupRequests == 0);
    fmtI("max_requests",             c.maxRequests,     c.maxRequests == 0);
    fmtS("output_dir",               c.outputDir,       c.outputDir == "results/");
    std::cout << "=====================================\n\n";
}

// ---------------------------------------------------------------------------
// writeStatsJson
// ---------------------------------------------------------------------------

void CXLMemController::writeStatsJson() const {
    const auto& s = stats_;
    const auto& c = cfg_;

    std::string tag  = c.runTag.empty() ? "cxray" : c.runTag;
    std::string path = c.outputDir + "/" + tag + "_stats.json";

    std::filesystem::create_directories(cfg_.outputDir);

    std::ofstream f(path);
    if (!f) {
        std::cerr << "Warning: cannot write stats to " << path << "\n";
        return;
    }

    f << std::fixed << std::setprecision(4);
    f << "{\n";
    f << "  \"run_tag\":                \"" << tag << "\",\n";
    f << "  \"cxl_version\":            \"" << c.cxlVersion << "\",\n";
    f << "  \"sim_mode\":               \"" << c.simMode << "\",\n";
    f << "  \"total_requests\":         " << s.totalRequests << ",\n";
    f << "  \"reads\":                  " << s.reads << ",\n";
    f << "  \"writes\":                 " << s.writes << ",\n";
    f << "  \"mean_latency_ns\":        " << s.meanLatencyNs() << ",\n";
    f << "  \"min_latency_ns\":         " << s.minLatencyNs << ",\n";
    f << "  \"max_latency_ns\":         " << s.maxLatencyNs << ",\n";
    f << "  \"p50_latency_ns\":         " << s.p50LatencyNs() << ",\n";
    f << "  \"p95_latency_ns\":         " << s.p95LatencyNs() << ",\n";
    f << "  \"p99_latency_ns\":         " << s.p99LatencyNs() << ",\n";
    f << "  \"row_hit_rate\":           " << s.rowHitRate() << ",\n";
    f << "  \"bandwidth_gbps\":         " << s.bandwidthGBps() << ",\n";
    f << "  \"effective_bw_gbps\":      " << s.effectiveBwGBps << ",\n";
    // link_utilization_frac: M2S (host→device) wire utilization; matches Python utilization_aggregate.
    f << "  \"link_utilization_frac\":  " << s.linkUtilizationFrac << ",\n";
    if (link_) {
        const auto& ls = link_->stats();
        double simNs = s.simDurationNs;
        f << "  \"m2s_link_utilization_frac\":              " << link_->m2sUtilization(simNs) << ",\n";
        f << "  \"s2m_link_utilization_frac\":              " << link_->s2mUtilization(simNs) << ",\n";
        f << "  \"aggregate_full_duplex_utilization_frac\": " << link_->aggregateUtilization(simNs) << ",\n";
        f << "  \"m2s_wire_bytes\":         " << ls.m2sWireBytesTx << ",\n";
        f << "  \"s2m_wire_bytes\":         " << ls.s2mWireBytesTx << ",\n";
        f << "  \"m2s_credit_stall_ns\":    " << (s.totalCreditStallNs - ls.s2mStallNs) << ",\n";
        f << "  \"s2m_credit_stall_ns\":    " << ls.s2mStallNs << ",\n";
    }
    f << "  \"credit_stall_ns\":        " << s.totalCreditStallNs << ",\n";
    f << "  \"sim_duration_ns\":        " << s.simDurationNs << ",\n";
    // Latency breakdown: only available in fast mode; null in detailed mode.
    if (c.simMode == "detailed") {
        f << "  \"latency_breakdown\": null,\n";
    } else {
        f << "  \"latency_breakdown\": {\n";
        const auto& bd = s.latencyBreakdown;
        f << "    \"host_port_ns\":         " << bd.hostPortNs    << ",\n";
        f << "    \"link_phys_ns\":         " << bd.linkPhysNs    << ",\n";
        f << "    \"device_port_ns\":       " << bd.devicePortNs  << ",\n";
        f << "    \"fec_ns\":               " << bd.fecNs         << ",\n";
        f << "    \"coherence_ns\":         " << bd.coherenceNs   << ",\n";
        f << "    \"dram_ns\":              " << bd.dramNs        << ",\n";
        f << "    \"ras_ns\":               " << bd.rasNs         << ",\n";
        f << "    \"ide_ns\":               " << bd.ideNs         << ",\n";
        f << "    \"return_ns\":            " << bd.returnNs      << ",\n";
        f << "    \"credit_stall_ns\":      " << bd.creditStallNs << ",\n";
        f << "    \"retry_ns\":             " << bd.retryNs       << "\n";
        f << "  },\n";
    }
    f << "  \"p2p_extra_latency_ns\":   " << s.p2pExtraLatencyNs << ",\n";
    f << "  \"numa_offset_ns\":         " << s.numaOffsetNs << ",\n";
    f << "  \"page_migrations\":        " << s.pageMigrations;

    // RAS stats: aggregate across all devices (GIM uses multiple devices)
    if (!devices_.empty()) {
        uint64_t totalCE = 0, totalUCE = 0, totalSPPR = 0, totalHPPR = 0;
        uint64_t totalPoison = 0, totalViral = 0, totalDBIT = 0;
        double   totalScrub = 0.0;
        for (const auto& devPtr : devices_) {
            const auto& ras = devPtr->ras();
            totalCE     += ras.correctedErrors();
            totalUCE    += ras.uncorrectedErrors();
            totalSPPR   += ras.sPPREvents();
            totalHPPR   += ras.hPPREvents();
            totalPoison += ras.poisonedLines();
            totalViral  += ras.viralEvents();
            totalDBIT   += ras.dbitRows();
            totalScrub  += ras.scrubCoverage();
        }
        f << ",\n";
        f << "  \"ras\": {\n";
        f << "    \"corrected_errors\":    " << totalCE     << ",\n";
        f << "    \"uncorrected_errors\":  " << totalUCE    << ",\n";
        f << "    \"sppr_events\":         " << totalSPPR   << ",\n";
        f << "    \"hppr_events\":         " << totalHPPR   << ",\n";
        f << "    \"poisoned_lines\":      " << totalPoison << ",\n";
        f << "    \"viral_events\":        " << totalViral  << ",\n";
        f << "    \"dbit_rows\":           " << totalDBIT   << ",\n";
        f << "    \"scrub_coverage\":      " << totalScrub  << "\n";
        f << "  }\n";
    } else {
        f << "\n";
    }

    // S2M credit pool stats (link-level; zero stall expected when s2m_drs >= m2s_req)
    if (link_) {
        const auto& ls = link_->stats();
        f << ",\n";
        f << "  \"s2m_credits\": {\n";
        f << "    \"drs_consumed\":  " << ls.s2mDrsConsumed << ",\n";
        f << "    \"ndr_consumed\":  " << ls.s2mNdrConsumed << ",\n";
        f << "    \"stall_ns\":      " << ls.s2mStallNs     << "\n";
        f << "  }\n";
    }

    f << "}\n";

    std::cout << "Stats written to: " << path << "\n";
}

}  // namespace cxl
