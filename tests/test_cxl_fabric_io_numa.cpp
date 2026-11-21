// test_cxl_fabric_io_numa.cpp
#include "test_helpers.h"
#include "../src/cxl_fabric.h"
#include "../src/cxl_io.h"
#include "../src/cxl_numa.h"
#include "../src/cxl_sim_engine.h"
#include <iostream>

int main() {
    std::cout << "=== test_cxl_fabric_io_numa ===\n";

    // ── CXL Fabric: GIM address translation ──────────────────────────────
    SECTION("GIM translate: 2 devices, 256B gran");
    {
        cxl::GIMConfig gc; gc.enable=true; gc.numDevices=2; gc.interleaveGran=256;
        cxl::GIMModel gim(gc);
        // Block 0 (0-255): device 0
        auto t0 = gim.translate(0);
        CHECK_EQ(t0.deviceId, 0);
        CHECK_EQ(t0.localAddr, 0ULL);
        // Block 1 (256-511): device 1
        auto t1 = gim.translate(256);
        CHECK_EQ(t1.deviceId, 1);
        CHECK_EQ(t1.localAddr, 0ULL);
        // Block 2 (512-767): device 0 again
        auto t2 = gim.translate(512);
        CHECK_EQ(t2.deviceId, 0);
        CHECK_EQ(t2.localAddr, 256ULL);
        std::cout << "  GIM 2-device translate: OK\n";
    }

    SECTION("GIM aggregate BW = N * single");
    {
        cxl::GIMConfig gc; gc.enable=true; gc.numDevices=4;
        cxl::GIMModel gim(gc);
        double agg = gim.aggregateBwGBps(10.0);
        CHECK_NEAR(agg, 40.0, 0.01);
        std::cout << "  GIM aggregate BW 4x: OK\n";
    }

    SECTION("P2P: 1 extra hop latency");
    {
        cxl::P2PConfig pc;
        pc.enable = true;
        pc.numExtraHops = 1;
        pc.extraLinkLatencyNs = 24.0;
        pc.extraSwitchLatNs   = 40.0;
        cxl::P2PModel p2p(pc);
        double lat = p2p.extraLatencyNs(1);
        // 1 * (24*2 + 40) = 88 ns
        CHECK_NEAR(lat, 88.0, 0.1);
        std::cout << "  P2P 1-hop extra latency " << lat << " ns: OK\n";
    }

    SECTION("P2P: BW degradation 1 hop ~88%");
    {
        cxl::P2PConfig pc; pc.enable=true;
        cxl::P2PModel p2p(pc);
        double f = p2p.bwDegradationFactor(1);
        CHECK(f > 0.8 && f < 1.0);
        std::cout << "  P2P BW degradation: " << f << ": OK\n";
    }

    // ── CXL.io latency ───────────────────────────────────────────────────
    SECTION("CXL.io cfgRead latency");
    {
        cxl::CXLIOConfig cfg;
        cfg.linkOneWayLatencyNs  = 30.0;
        cfg.cfgRegisterLookupNs  = 5.0;
        cfg.cfgTlpOverheadNs     = 8.0;
        cxl::CXLIOController io(cfg);
        auto r = io.cfgRead(0.0, 0, 0);
        // 2*30 + 5 + 2*8 = 81 ns
        CHECK_NEAR(r.latencyNs, 81.0, 1.0);
        std::cout << "  cfgRead latency " << r.latencyNs << " ns: OK\n";
    }

    SECTION("CXL.io mmioRead latency");
    {
        cxl::CXLIOConfig cfg;
        cfg.linkOneWayLatencyNs = 30.0;
        cfg.mmioHandlerNs       = 10.0;
        cfg.mmioTlpOverheadNs   = 8.0;
        cxl::CXLIOController io(cfg);
        auto r = io.mmioRead(0.0, 0x1000, 32);
        // 2*30 + 10 + 2*8 = 86 ns
        CHECK_NEAR(r.latencyNs, 86.0, 2.0);
        std::cout << "  mmioRead latency " << r.latencyNs << " ns: OK\n";
    }

    SECTION("CXL.io DMA 4KB latency");
    {
        cxl::CXLIOConfig cfg;
        cfg.dmaDescriptorNs    = 150.0;
        cfg.dmaLinkBwGBps      = 32.0;
        cfg.dmaTlpOverheadFrac = 0.05;
        cxl::CXLIOController io(cfg);
        auto r = io.dmaRead(0.0, 0, 0x1000, 4096);
        // 150 + (4096/32) * 1.05 ≈ 150 + 134.4 = 284.4 ns
        CHECK(r.latencyNs > 200.0 && r.latencyNs < 400.0);
        std::cout << "  DMA 4KB latency " << r.latencyNs << " ns: OK\n";
    }

    SECTION("CXL.io MSI latency");
    {
        cxl::CXLIOConfig cfg;
        cfg.linkOneWayLatencyNs    = 30.0;
        cfg.msiIommuLookupNs       = 20.0;
        cfg.msiInterruptDeliveryNs = 50.0;
        cxl::CXLIOController io(cfg);
        auto r = io.msiInterrupt(0.0, 1);
        // 30 + 20 + 50 = 100 ns
        CHECK_NEAR(r.latencyNs, 100.0, 1.0);
        std::cout << "  MSI latency " << r.latencyNs << " ns: OK\n";
    }

    // ── NUMA model ───────────────────────────────────────────────────────
    SECTION("NUMA single-node: no UPI penalty");
    {
        cxl::NUMAConfig nc; nc.numNodes = 1;
        cxl::PageMigrationConfig mc; mc.enable = false;
        cxl::NUMAModel nm(nc, mc);
        auto r = nm.adjustLatency(0, 0, 218.6);
        CHECK_NEAR(r.numaOffsetNs, 0.0, 0.01);
        CHECK_EQ(r.numUPIHops, 0);
        std::cout << "  NUMA single-node: OK\n";
    }

    SECTION("NUMA 2-socket: cross-socket adds UPI penalty");
    {
        cxl::NUMAConfig nc;
        nc.numNodes = 2;
        nc.upiOneWayLatencyNs = 45.0;
        cxl::NUMANode n0; n0.nodeId=0; n0.hasCXLDevice=true; n0.cxlDeviceIdx=0;
        cxl::NUMANode n1; n1.nodeId=1; n1.hasCXLDevice=true; n1.cxlDeviceIdx=1;
        nc.nodes = {n0, n1};
        cxl::PageMigrationConfig mc; mc.enable = false;
        cxl::NUMAModel nm(nc, mc);
        // CPU on node 0 accessing device on node 1: 1 UPI hop
        auto r = nm.adjustLatency(0, 1, 218.6);
        CHECK_EQ(r.numUPIHops, 1);
        CHECK_NEAR(r.numaOffsetNs, 90.0, 1.0);  // 2 * 45 = 90 ns
        std::cout << "  NUMA 2-socket cross-NUMA offset " << r.numaOffsetNs << " ns: OK\n";
    }

    SECTION("Page migration: hot page triggers migration");
    {
        cxl::NUMAConfig nc; nc.numNodes = 1;
        cxl::PageMigrationConfig mc;
        mc.enable = true;
        mc.hotAccessThreshold = 3;
        mc.monitorWindowNs = 1e9;  // 1 second window
        mc.migrationBwGBps = 20.0;
        mc.pageBytes = 4096;
        cxl::NUMAModel nm(nc, mc);
        uint64_t addr = 0x1000;
        // Access below threshold: no migration
        nm.recordAccess(addr, 0.0);
        nm.recordAccess(addr, 100.0);
        CHECK_EQ(nm.getPageTier(addr), cxl::PageTier::CXL_TIER2);
        // Third access triggers migration
        double overhead = nm.recordAccess(addr, 200.0);
        CHECK(overhead > 0.0);
        // Page should be MIGRATING or LOCAL_DRAM
        auto tier = nm.getPageTier(addr);
        CHECK(tier == cxl::PageTier::MIGRATING || tier == cxl::PageTier::LOCAL_DRAM);
        CHECK_EQ(nm.totalMigrations(), 1ULL);
        std::cout << "  Page migration triggered after " << overhead << " ns overhead: OK\n";
    }

    SECTION("Effective latency: migrated page uses local DRAM latency");
    {
        cxl::NUMAConfig nc; nc.numNodes = 1;
        cxl::PageMigrationConfig mc;
        mc.enable = true;
        mc.hotAccessThreshold = 1;
        mc.monitorWindowNs = 1e9;
        mc.migrationBwGBps = 1000.0;  // instantaneous for test
        mc.migrationOverheadNs = 0.0;
        mc.pageBytes = 4096;
        cxl::NUMAModel nm(nc, mc);
        uint64_t addr = 0x2000;
        // Trigger migration immediately
        nm.recordAccess(addr, 0.0);
        // After migration completes (bw=1000GB/s → 4096B/1000 = 4ns)
        double effectiveLat = nm.effectiveLatency(addr, 100.0, 218.6, 114.0);
        // Should be local DRAM latency after migration
        CHECK_NEAR(effectiveLat, 114.0, 5.0);
        std::cout << "  Migrated page latency " << effectiveLat << " ns: OK\n";
    }

    // ── Detailed mode: DES engine smoke tests ────────────────────────────────────
    SECTION("SimEventQueue: events fire in time order");
    {
        cxl::SimEventQueue evq;
        std::vector<double> fired;
        evq.schedule(30.0, [&]{ fired.push_back(30.0); });
        evq.schedule(10.0, [&]{ fired.push_back(10.0); });
        evq.schedule(20.0, [&]{ fired.push_back(20.0); });
        evq.runAll();
        CHECK_EQ(fired.size(), 3UL);
        CHECK_NEAR(fired[0], 10.0, 0.01);
        CHECK_NEAR(fired[1], 20.0, 0.01);
        CHECK_NEAR(fired[2], 30.0, 0.01);
        std::cout << "  SimEventQueue ordering: OK\n";
    }

    SECTION("CreditTracker: acquire blocks when empty");
    {
        cxl::SimEventQueue evq;
        cxl::CreditTracker ct(evq, 2, 100.0);  // 2 credits, 100ns return
        std::vector<double> grants;
        // Acquire 3 credits: third should stall
        ct.acquire(0.0,   [&](double t){ grants.push_back(t); });
        ct.acquire(0.0,   [&](double t){ grants.push_back(t); });
        ct.acquire(0.0,   [&](double t){ grants.push_back(t); }); // will block
        evq.runUntil(150.0);  // credits auto-return at t=100
        CHECK(grants.size() >= 2);
        // Third grant should be at ~100ns (when first credit auto-returns)
        if (grants.size() >= 3) {
            CHECK(grants[2] >= 99.0);
        }
        std::cout << "  CreditTracker blocking: " << grants.size() << " grants: OK\n";
    }

    SECTION("FlitSerializer: serializes FLITs correctly");
    {
        cxl::SimEventQueue evq;
        cxl::FlitSerializer lane(evq, 2.0);  // 2 ns per FLIT
        std::vector<double> completions;
        lane.scheduleTx(0.0,  4, [&](double t){ completions.push_back(t); }); // 8 ns
        lane.scheduleTx(5.0,  2, [&](double t){ completions.push_back(t); }); // starts at 8, done at 12
        evq.runAll();
        CHECK_EQ(completions.size(), 2UL);
        CHECK_NEAR(completions[0], 8.0,  0.01);  // 4 flits * 2ns
        CHECK_NEAR(completions[1], 12.0, 0.01);  // serialized after first
        std::cout << "  FlitSerializer: " << completions[0] << " ns, " << completions[1] << " ns: OK\n";
    }

    return TEST_SUMMARY();
}
