// test_cxl_ras.cpp — RAS engine unit tests
//
// Tests: patrol scrub overhead, CE/UCE injection, CVME threshold,
//        PPR stall, data poison propagation, viral signaling.

#include "test_helpers.h"
#include "../src/cxl_ras.h"
#include <iostream>

// Helper: call process() and return extraNs
static double rasProcess(cxl::RASEngine& ras, uint64_t addr, bool isWrite,
                         double dispatchNs, uint32_t sizeBytes) {
    return ras.processLegacy(addr, isWrite, dispatchNs, sizeBytes);
}

static cxl::RASConfig makeFullRASConfig() {
    cxl::RASConfig c;
    c.enable                   = true;
    c.patrolScrubIntervalUs    = 100.0;
    c.patrolScrubBwFraction    = 0.03;
    c.cvmeThreshold            = 3;
    c.pprEnable                = true;
    c.spprEnable               = true;
    c.spprLatencyUs            = 10.0;
    c.dataPoisonEnable         = true;
    c.viralEnable              = true;
    c.injectEnable             = true;
    c.ceRate                   = 0.0;
    c.uceRate                  = 0.0;
    return c;
}

int main() {
    std::cout << "=== test_cxl_ras ===\n";

    SECTION("No RAS overhead when disabled");
    {
        cxl::RASConfig cfg;
        cfg.enable = false;
        cxl::RASEngine ras(cfg, 42);
        double extra = rasProcess(ras, 0x1000, false, 0.0, 64);
        CHECK_EQ(extra, 0.0);
        std::cout << "  RAS disabled → extra = " << extra << " ns\n";
    }

    SECTION("Patrol scrub adds non-negative overhead");
    {
        auto cfg = makeFullRASConfig();
        cfg.patrolScrubBwFraction = 0.05;
        cxl::RASEngine ras(cfg, 42);
        double extra = rasProcess(ras, 0x1000, false, 0.0, 64);
        CHECK_GE(extra, 0.0);
        std::cout << "  Patrol scrub overhead: " << extra << " ns\n";
    }

    SECTION("Manual CE injection creates an error log entry");
    {
        auto cfg = makeFullRASConfig();
        cxl::RASEngine ras(cfg, 42);
        ras.injectCE(0xABCD0000, 1000.0);
        CHECK_EQ(ras.correctedErrors(), 1UL);
        std::cout << "  CE inject: ceCount = " << ras.correctedErrors() << "\n";
    }

    SECTION("Manual UCE injection creates an error log entry");
    {
        auto cfg = makeFullRASConfig();
        cxl::RASEngine ras(cfg, 42);
        ras.injectUCE(0xBEEF0000, 2000.0);
        CHECK_EQ(ras.uncorrectedErrors(), 1UL);
        std::cout << "  UCE inject: uceCount = " << ras.uncorrectedErrors() << "\n";
    }

    SECTION("CVME threshold triggers PPR");
    {
        auto cfg = makeFullRASConfig();
        cfg.cvmeThreshold = 2;
        cxl::RASEngine ras(cfg, 42);
        ras.injectCE(0x10000, 100.0);
        ras.injectCE(0x10000, 200.0);
        // Process request to same address → PPR stall
        double extra = rasProcess(ras, 0x10000, false, 300.0, 64);
        std::cout << "  PPR stall extra = " << extra << " ns, pprCount=" << ras.pprEventCount() << "\n";
        CHECK_GE(extra, 0.0);
        CHECK_EQ(ras.pprEventCount(), 1UL);
    }

    SECTION("Viral signaling set after UCE access");
    {
        auto cfg = makeFullRASConfig();
        cxl::RASEngine ras(cfg, 42);
        CHECK(!ras.isViralActive());
        ras.injectUCE(0x20000, 500.0);
        rasProcess(ras, 0x20000, false, 600.0, 64);
        CHECK(ras.isViralActive());
        std::cout << "  Viral: " << (ras.isViralActive() ? "set" : "clear") << "\n";
    }

    SECTION("Data poison propagates on poisoned address read");
    {
        auto cfg = makeFullRASConfig();
        cxl::RASEngine ras(cfg, 42);
        ras.injectUCE(0x30000, 100.0);
        // After UCE, address should be poisoned
        CHECK(ras.isPoisoned(0x30000));
        std::cout << "  isPoisoned after UCE: " << ras.isPoisoned(0x30000) << "\n";
    }

    SECTION("Clean address is not poisoned");
    {
        auto cfg = makeFullRASConfig();
        cxl::RASEngine ras(cfg, 42);
        CHECK(!ras.isPoisoned(0x40000));
        std::cout << "  Clean address not poisoned: OK\n";
    }

    SECTION("CVME is a global sliding-window counter (crosses address boundaries)");
    {
        // CVME threshold counts ALL CEs in the window, not per-address.
        // 2 CEs to different addresses within window = 1 PPR triggered.
        auto cfg = makeFullRASConfig();
        cfg.cvmeThreshold = 2;
        cfg.cvmeWindowUs  = 1000.0;  // large window
        cxl::RASEngine ras(cfg, 42);
        ras.injectCE(0x10000, 100.0);
        ras.injectCE(0x20000, 200.0);  // different address, same window → triggers PPR
        CHECK_EQ(ras.pprEventCount(), 1UL);
        std::cout << "  Global CVME across addresses: pprCount=" << ras.pprEventCount() << "\n";
    }

    SECTION("RAS overhead is bounded < 100 µs for normal scrub intervals");
    {
        auto cfg = makeFullRASConfig();
        cfg.patrolScrubIntervalUs = 100.0;
        cfg.patrolScrubBwFraction = 0.03;
        cxl::RASEngine ras(cfg, 42);
        double extra = rasProcess(ras, 0x5000, false, 0.0, 64);
        CHECK_LT(extra, 100000.0);
        std::cout << "  Bounded scrub overhead: " << extra << " ns\n";
    }

    SECTION("Error counts correct for multiple injections");
    {
        auto cfg = makeFullRASConfig();
        cxl::RASEngine ras(cfg, 42);
        for (int i = 0; i < 5; i++) ras.injectCE(i * 0x1000, i * 100.0);
        for (int i = 0; i < 3; i++) ras.injectUCE(0xF0000 + i * 0x1000, i * 100.0);
        CHECK_EQ(ras.correctedErrors(),   5UL);
        CHECK_EQ(ras.uncorrectedErrors(), 3UL);
        std::cout << "  CE=" << ras.correctedErrors()
                  << "  UCE=" << ras.uncorrectedErrors() << "\n";
    }

    SECTION("PPR busy-until grows with each repair");
    {
        auto cfg = makeFullRASConfig();
        cfg.cvmeThreshold = 1;    // PPR after every CE
        cfg.spprLatencyUs = 10.0;
        cxl::RASEngine ras(cfg, 42);
        ras.injectCE(0x10000, 0.0);
        rasProcess(ras, 0x10000, false, 0.0, 64);
        double busy1 = ras.pprBusyUntilNs();
        ras.injectCE(0x20000, busy1 + 1.0);
        rasProcess(ras, 0x20000, false, busy1 + 1.0, 64);
        double busy2 = ras.pprBusyUntilNs();
        CHECK_GT(busy2, busy1);
        std::cout << "  PPR busy grows: " << busy1 << " → " << busy2 << " ns\n";
    }

    return TEST_SUMMARY();
}
