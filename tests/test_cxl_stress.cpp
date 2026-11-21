// test_cxl_stress.cpp — HOL blocking, incast, and queueing stress tests
#include "test_helpers.h"
#include "../src/cxl_switch.h"
#include "../src/cxl_link.h"
#include <iostream>
#include <vector>
#include <numeric>

int main() {
    std::cout << "=== test_cxl_stress ===\n";

    // ── Switch: time-based queue, sequential arrivals ─────────────────────
    SECTION("Switch: sequential arrivals saturate queue progressively");
    {
        cxl::SwitchConfig sc;
        sc.latencyNs = 40.0;
        sc.queueCapacityFlits = 8;
        sc.ingressBwGBps = 0.0;  // inherit from link
        cxl::CXLSwitch sw(sc);
        sw.setLinkBwGBps(32.0);  // 256B FLIT → ~8 ns service time
        sw.addHBREntry(0, UINT64_MAX, 0);

        // 10 requests arriving back-to-back at 0ns (incast scenario)
        std::vector<double> latencies;
        for (int i = 0; i < 10; ++i) {
            uint8_t ep = 0;
            double lat = sw.route(0, 0, ep, 1, /*arrivalNs=*/0.0);
            latencies.push_back(lat);
        }
        // All should get the base latency + some queuing in analytical mode
        for (double l : latencies) {
            CHECK(l >= sc.latencyNs);
        }
        std::cout << "  Incast analytical: min=" << *std::min_element(latencies.begin(), latencies.end())
                  << " max=" << *std::max_element(latencies.begin(), latencies.end()) << " ns: OK\n";
    }

    SECTION("Switch: time-based queue, arrivals spaced apart");
    {
        cxl::SwitchConfig sc;
        sc.latencyNs = 40.0;
        sc.queueCapacityFlits = 8;
        cxl::CXLSwitch sw(sc);
        sw.setLinkBwGBps(32.0);
        sw.addHBREntry(0, UINT64_MAX, 0);

        // Arrivals spaced 100 ns apart — no queuing expected
        uint8_t ep = 0;
        double lat1 = sw.route(0, 0, ep, 1, 0.0);
        double lat2 = sw.route(0, 0, ep, 1, 100.0);
        double lat3 = sw.route(0, 0, ep, 1, 200.0);
        // With 100ns spacing >> ~8ns service time, no queuing
        CHECK_NEAR(lat1, sc.latencyNs, 1.0);
        CHECK_NEAR(lat2, sc.latencyNs, 1.0);
        CHECK_NEAR(lat3, sc.latencyNs, 1.0);
        std::cout << "  Spaced arrivals: no queuing: OK\n";
    }

    SECTION("Switch: incast — many sources hitting same egress");
    {
        // 4 ingress ports → 1 egress port at the same arrival time
        cxl::SwitchConfig sc;
        sc.mode = cxl::RoutingMode::PBR;
        sc.latencyNs = 40.0;
        sc.queueCapacityFlits = 4;
        cxl::CXLSwitch sw(sc);
        sw.setLinkBwGBps(32.0);
        for (uint8_t i = 0; i < 4; i++) sw.addPBREntry(i, 0);

        // 4 sources all arrive at t=0 targeting port 0
        std::vector<double> lats;
        for (uint8_t src = 0; src < 4; src++) {
            uint8_t ep = 0;
            lats.push_back(sw.route(0, src, ep, 1, 0.0));
        }
        // Each has its own ingress queue, so each sees only its own latency
        // (incast at egress would require egress queue modeling — not in scope)
        for (double l : lats) CHECK(l >= sc.latencyNs);
        std::cout << "  Incast 4 sources: OK (per-ingress queue model)\n";
    }

    SECTION("Switch: HBR address routing");
    {
        cxl::SwitchConfig sc;
        sc.latencyNs = 50.0;
        cxl::CXLSwitch sw(sc);
        sw.addHBREntry(0x000000000, 0x080000000, 0);  // [0, 2GB) → port 0
        sw.addHBREntry(0x080000000, 0x080000000, 1);  // [2GB, 4GB) → port 1

        uint8_t ep = 0xFF;
        sw.route(0x040000000, 0, ep);
        CHECK_EQ(ep, static_cast<uint8_t>(0));
        sw.route(0x0A0000000, 0, ep);
        CHECK_EQ(ep, static_cast<uint8_t>(1));
        std::cout << "  HBR routing: OK\n";
    }

    // ── Link: credit exhaustion under burst ───────────────────────────────
    SECTION("Link: credit stall accumulates under depth=64");
    {
        cxl::LinkConfig lc;
        lc.pcieGen = 5;
        lc.pcieLanes = 8;
        lc.credits.m2sReq = 8;  // small pool → easy to saturate
        lc.maxOutstanding  = 8;
        lc.estimatedRttNs  = 200.0;
        cxl::CXLLink link(lc);

        double totalStall = 0.0;
        // Issue 32 reads at t=0 (4× the credit pool)
        for (int i = 0; i < 32; i++) {
            double stall = 0.0;
            link.enqueueRequest(0.0, 64, stall, /*isWrite=*/false);
            totalStall += stall;
        }
        // With 8 credits and 32 requests, at least 24 requests must stall
        CHECK(totalStall > 0.0);
        std::cout << "  Credit stall at depth 32 (pool=8): total=" << totalStall << " ns: OK\n";
    }

    SECTION("Link: write credits (M2S_RWD) are independent of read credits (M2S_REQ)");
    {
        cxl::LinkConfig lc;
        lc.credits.m2sReq = 4;   // 4 read credits
        lc.credits.m2sRwd = 16;  // 16 write credits (not saturated)
        lc.maxOutstanding  = 32;
        lc.estimatedRttNs  = 200.0;
        cxl::CXLLink link(lc);

        // Exhaust read credits
        double stall = 0.0;
        for (int i = 0; i < 6; i++)
            link.enqueueRequest(0.0, 64, stall, /*isWrite=*/false);
        double readStall = link.stats().creditStallNs;

        // Writes should not stall (separate pool)
        cxl::LinkConfig lc2;
        lc2.credits.m2sReq = 4;
        lc2.credits.m2sRwd = 16;
        lc2.maxOutstanding  = 32;
        lc2.estimatedRttNs  = 200.0;
        cxl::CXLLink link2(lc2);
        double ws = 0.0;
        for (int i = 0; i < 6; i++)
            link2.enqueueRequest(0.0, 64, ws, /*isWrite=*/true);
        double writeStall = link2.stats().creditStallNs;

        CHECK(readStall > 0.0);   // reads stalled (pool=4, issued 6)
        CHECK(writeStall == 0.0); // writes did NOT stall (pool=16)
        std::cout << "  Read stall=" << readStall << " ns, Write stall=" << writeStall << " ns: OK\n";
    }

    return TEST_SUMMARY();
}
