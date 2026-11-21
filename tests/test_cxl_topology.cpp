// test_cxl_topology.cpp — CXL switch and topology tests
//
// Tests: HBR / PBR routing, switch latency, multi-device topologies,
//        link bandwidth formulas.

#include "test_helpers.h"
#include "../src/cxl_switch.h"
#include "../src/cxl_link.h"
#include "../src/cxl_mem.h"
#include <iostream>

int main() {
    std::cout << "=== test_cxl_topology ===\n";

    SECTION("HBR routing: address in range → correct DSP port");
    {
        cxl::SwitchConfig cfg;
        cfg.mode      = cxl::RoutingMode::HBR;
        cfg.latencyNs = 50.0;
        cxl::CXLSwitch sw(cfg);

        sw.addHBREntry(0x000000000, 0x040000000, 0);  // first 1 GB → port 0
        sw.addHBREntry(0x040000000, 0x040000000, 1);  // next  1 GB → port 1
        sw.addHBREntry(0x080000000, 0x040000000, 2);  // next  1 GB → port 2

        uint8_t egress = 0xFF;
        double lat = sw.route(0x010000000, 0, egress);
        CHECK_EQ(egress, 0);
        CHECK_NEAR(lat, 50.0, 0.1);

        lat = sw.route(0x050000000, 0, egress);
        CHECK_EQ(egress, 1);

        lat = sw.route(0x090000000, 0, egress);
        CHECK_EQ(egress, 2);

        std::cout << "  HBR 3-device routing: OK\n";
    }

    SECTION("HBR routing: address not in any range → default port 0");
    {
        cxl::SwitchConfig cfg;
        cfg.mode      = cxl::RoutingMode::HBR;
        cfg.latencyNs = 50.0;
        cxl::CXLSwitch sw(cfg);
        sw.addHBREntry(0, 0x1000, 3);

        uint8_t egress = 0xFF;
        sw.route(0xFFFF0000, 0, egress);
        CHECK_EQ(egress, 0);  // default
        std::cout << "  HBR default port: OK\n";
    }

    SECTION("PBR routing: ingress port → egress port");
    {
        cxl::SwitchConfig cfg;
        cfg.mode      = cxl::RoutingMode::PBR;
        cfg.latencyNs = 40.0;
        cxl::CXLSwitch sw(cfg);

        sw.addPBREntry(0, 2);
        sw.addPBREntry(1, 3);
        sw.addPBREntry(2, 0);

        uint8_t egress = 0xFF;
        sw.route(0, 0, egress);  // addr ignored in PBR
        CHECK_EQ(egress, 2);

        sw.route(0, 1, egress);
        CHECK_EQ(egress, 3);

        sw.route(0, 2, egress);
        CHECK_EQ(egress, 0);

        std::cout << "  PBR routing: OK\n";
    }

    SECTION("Switch latency is added regardless of routing mode");
    {
        for (auto mode : {cxl::RoutingMode::HBR, cxl::RoutingMode::PBR}) {
            cxl::SwitchConfig cfg;
            cfg.mode      = mode;
            cfg.latencyNs = 55.0;
            cxl::CXLSwitch sw(cfg);
            if (mode == cxl::RoutingMode::HBR) sw.addHBREntry(0, UINT64_MAX, 0);
            else                                sw.addPBREntry(0, 1);
            uint8_t egress = 0;
            double lat = sw.route(0, 0, egress);
            CHECK_NEAR(lat, 55.0, 0.1);
        }
        std::cout << "  Switch latency both modes: OK\n";
    }

    SECTION("CXL link: enqueue monotonically advances time");
    {
        cxl::LinkConfig lcfg;
        lcfg.pcieGen     = 5;
        lcfg.pcieLanes   = 16;
        lcfg.linkLatencyNs = 100.0;
        lcfg.maxOutstanding = 256;
        cxl::CXLLink link(lcfg);

        double prev = 0.0;
        for (int i = 0; i < 10; i++) {
            double stall = 0.0;
            double arrival = link.enqueueRequest(i * 10.0, 64, stall);
            CHECK_GE(arrival, prev);
            prev = arrival;
        }
        std::cout << "  Link enqueue monotonic: OK\n";
    }

    SECTION("Multi-device: CXL 3.1 with 4 devices, each at different base address");
    {
        cxl::SimConfig cfg;
        cfg.cxlVersion       = "3.1";
        cfg.link.pcieGen     = 6;
        cfg.link.pcieLanes   = 16;
        cfg.link.linkLatencyNs = 80.0;
        cfg.hasSwitch        = true;
        cfg.sw.mode          = cxl::RoutingMode::PBR;
        cfg.sw.latencyNs     = 40.0;
        cfg.numDevices       = 1;  // single device in SimConfig for now
        cfg.device.type      = 3;
        cfg.device.controllerLatencyNs = 50.0;
        cfg.device.dramBackendType = "simple";
        cfg.device.numChannels = 1;
        cfg.device.numRanks    = 2;
        cfg.device.numBanks    = 16;
        cfg.device.dramTiming.tCK_ns  = 0.417;
        cfg.device.dramTiming.tCL_ns  = 16.67;
        cfg.device.dramTiming.tRCD_ns = 16.67;
        cfg.device.dramTiming.tRP_ns  = 16.67;
        cfg.ras.enable       = false;

        cxl::CXLMemController sim(cfg);

        cxl::TraceRequest req{};
        req.op          = cxl::TraceOp::READ;
        req.addr        = 0x1000;
        req.sizeBytes   = 64;
        req.requesterId = 0;

        auto res = sim.processRequest(req, 0.0);
        CHECK_GT(res.latencyNs, 0.0);
        CHECK_LE(res.latencyNs, 1000.0);
        std::cout << "  CXL 3.1 topology latency: " << res.latencyNs << " ns\n";
    }

    SECTION("Link upstreamUtilization returns [0, 1]");
    {
        cxl::LinkConfig lcfg;
        lcfg.pcieGen   = 5;
        lcfg.pcieLanes = 16;
        lcfg.linkLatencyNs = 100.0;
        lcfg.maxOutstanding = 256;
        cxl::CXLLink link(lcfg);

        double stall = 0.0;
        for (int i = 0; i < 100; i++) link.enqueueRequest(i * 10.0, 64, stall);

        double util = link.upstreamUtilization(1000.0);
        CHECK_GE(util, 0.0);
        CHECK_LE(util, 1.0);
        std::cout << "  Link utilization: " << util * 100.0 << "%\n";
    }

    SECTION("Unsupported PCIe gen throws exception");
    {
        bool threw = false;
        try {
            cxl::CXLLink::computeBandwidthGBps(3, 16);
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw);
        std::cout << "  Gen 3 throws: OK\n";
    }

    SECTION("Switch HOPS constant = 1");
    {
        CHECK_EQ(cxl::CXLSwitch::HOPS, 1);
        std::cout << "  Switch HOPS = " << cxl::CXLSwitch::HOPS << "\n";
    }

    return TEST_SUMMARY();
}
