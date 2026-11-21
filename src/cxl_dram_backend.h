#pragma once
#include <cstdint>
#include <string>

namespace cxl {

// ---------------------------------------------------------------------------
// IDRAMBackend — abstract interface for pluggable DRAM simulators.
// CXRay fast mode uses SimpleDRAMBackend (analytical).
// Accurate mode uses DDR5Backend (JEDEC-accurate, arrival-order) or external sims.
// ---------------------------------------------------------------------------

class IDRAMBackend {
public:
    virtual ~IDRAMBackend() = default;

    // Submit a memory request. Returns a request ID.
    virtual uint64_t addRequest(uint64_t addr, bool isWrite, double arrivalNs) = 0;

    // Get completion time (ns) for a submitted request.
    // Returns -1 if not yet complete.
    virtual double getCompletionNs(uint64_t requestId) = 0;

    // Advance simulation to currentNs (for cycle-based backends).
    virtual void tick(double currentNs) = 0;

    // Measured sustained bandwidth (GB/s).
    virtual double getBandwidthGBps() const = 0;

    // Row buffer hit rate [0,1].
    virtual double getRowHitRate() const = 0;

    // Stats counters.
    virtual uint64_t getRowHits()      const = 0;
    virtual uint64_t getRowMisses()    const = 0;
    virtual uint64_t getRowConflicts() const = 0;

    // Human-readable backend name.
    virtual std::string name() const = 0;
};

// ── DRAM timing parameters (exposed for config) ─────────────────────────────
// All defaults correspond to DDR5-4800 CL40 (JEDEC JESD79-5B).

struct DRAMTimingParams {
    // Clock
    double tCK_ns    = 0.41667;  // DDR5-4800: 1/2400MHz = 0.41667 ns

    // Core timing
    double tCL_ns    = 16.67;    // CAS latency  (40 × tCK)
    double tRCD_ns   = 16.67;    // RAS-to-CAS   (40 × tCK)
    double tRP_ns    = 16.67;    // Precharge     (40 × tCK)
    double tRAS_ns   = 36.67;    // Min active time (88 × tCK)
    double tRC_ns    = 53.33;    // Row cycle = tRAS + tRP
    double tWR_ns    = 21.67;    // Write recovery (52 × tCK)
    double tRTP_ns   = 5.0;      // Read-to-precharge (12 × tCK)

    // Bank group timing (DDR5 specific)
    double tRRD_S_ns = 1.67;     // ACT-to-ACT, same bank group  (4 × tCK)
    double tRRD_L_ns = 3.33;     // ACT-to-ACT, diff bank group  (8 × tCK)
    double tCCD_S_ns = 3.33;     // CAS-to-CAS, same bank group  (8 × tCK)
    double tCCD_L_ns = 5.0;      // CAS-to-CAS, diff bank group  (12 × tCK)
    double tWTR_S_ns = 1.67;     // Write-to-read, same bank group (4 × tCK)
    double tWTR_L_ns = 5.0;      // Write-to-read, diff bank group (12 × tCK)
    double tFAW_ns   = 16.67;    // Four-bank activation window   (40 × tCK)

    // Burst
    double tBL_ns    = 1.67;     // Burst length time = BL8/2 × tCK (4 × tCK)

    // Refresh
    double tRFC_ns    = 195.0;   // All-bank refresh cycle (DDR5 16Gb)
    double tRFCpb_ns  = 30.0;    // Per-bank refresh cycle
    double tREFI_ns   = 3900.0;  // Refresh interval (DDR5 = 3.9 µs)
    double tREFIpb_ns = 244.0;   // Per-bank refresh interval

    // Organization
    int numBankGroups    = 4;    // DDR5: 4 bank groups per rank
    int numBanksPerGroup = 4;    // DDR5: 4 banks per group (= 16 banks/rank)

    // Convenience methods
    double rowHitLatNs()       const { return tCL_ns + tBL_ns; }
    double rowMissLatNs()      const { return tRCD_ns + tCL_ns + tBL_ns; }
    double rowConflictLatNs()  const { return tRP_ns + tRCD_ns + tCL_ns + tBL_ns; }
    int    totalBanksPerRank() const { return numBankGroups * numBanksPerGroup; }

    // Preset factories (implemented as inline methods below)
    static DRAMTimingParams DDR5_4800();
    static DRAMTimingParams DDR5_6400();
    static DRAMTimingParams DDR4_3200();
    static DRAMTimingParams DDR4_2400();
};

// ── Factory method implementations ──────────────────────────────────────────

inline DRAMTimingParams DRAMTimingParams::DDR5_4800() {
    DRAMTimingParams p;
    // All defaults above ARE DDR5-4800 CL40
    return p;
}

inline DRAMTimingParams DRAMTimingParams::DDR5_6400() {
    DRAMTimingParams p;
    double tCK = 0.3125;  // 3200 MHz (DDR5-6400 transfers at 6400 MT/s = 3200 MHz clock)
    p.tCK_ns = tCK;
    p.tCL_ns = 16 * tCK;  p.tRCD_ns = 16 * tCK;  p.tRP_ns = 16 * tCK;
    p.tRAS_ns = 40 * tCK; p.tRC_ns  = 56 * tCK;
    p.tWR_ns  = 24 * tCK; p.tRTP_ns = 12 * tCK;
    p.tRRD_S_ns = 4 * tCK;  p.tRRD_L_ns = 8 * tCK;
    p.tCCD_S_ns = 8 * tCK;  p.tCCD_L_ns = 12 * tCK;
    p.tWTR_S_ns = 4 * tCK;  p.tWTR_L_ns = 16 * tCK;
    p.tFAW_ns   = 32 * tCK; p.tBL_ns    = 4 * tCK;
    p.tRFC_ns   = 195.0; p.tRFCpb_ns = 25.0; p.tREFI_ns = 3900.0;
    return p;
}

inline DRAMTimingParams DRAMTimingParams::DDR4_3200() {
    DRAMTimingParams p;
    double tCK = 0.625;  // 1600 MHz (DDR4-3200 = 3200 MT/s = 1600 MHz clock)
    p.tCK_ns = tCK;
    p.tCL_ns = 14 * tCK;  p.tRCD_ns = 14 * tCK;  p.tRP_ns = 14 * tCK;
    p.tRAS_ns = 32 * tCK; p.tRC_ns  = 46 * tCK;
    p.tWR_ns  = 16 * tCK; p.tRTP_ns = 7.5;
    p.tRRD_S_ns = 4 * tCK;  p.tRRD_L_ns = 6 * tCK;
    p.tCCD_S_ns = 4 * tCK;  p.tCCD_L_ns = 6 * tCK;
    p.tWTR_S_ns = 4 * tCK;  p.tWTR_L_ns = 8 * tCK;
    p.tFAW_ns   = 25 * tCK; p.tBL_ns    = 4 * tCK;
    p.tRFC_ns   = 350.0; p.tRFCpb_ns = 350.0; p.tREFI_ns = 7800.0;
    p.numBankGroups = 4; p.numBanksPerGroup = 4;
    return p;
}

inline DRAMTimingParams DRAMTimingParams::DDR4_2400() {
    DRAMTimingParams p;
    double tCK = 0.833;  // 1200 MHz (DDR4-2400 = 2400 MT/s = 1200 MHz clock)
    p.tCK_ns = tCK;
    p.tCL_ns = 17 * tCK;  p.tRCD_ns = 17 * tCK;  p.tRP_ns = 17 * tCK;
    p.tRAS_ns = 39 * tCK; p.tRC_ns  = 56 * tCK;
    p.tWR_ns  = 20 * tCK; p.tRTP_ns = 7.5;
    p.tRRD_S_ns = 4 * tCK;  p.tRRD_L_ns = 6 * tCK;
    p.tCCD_S_ns = 4 * tCK;  p.tCCD_L_ns = 6 * tCK;
    p.tWTR_S_ns = 4 * tCK;  p.tWTR_L_ns = 8 * tCK;
    p.tFAW_ns   = 30 * tCK; p.tBL_ns    = 4 * tCK;
    p.tRFC_ns   = 350.0; p.tRFCpb_ns = 350.0; p.tREFI_ns = 7800.0;
    p.numBankGroups = 4; p.numBanksPerGroup = 4;
    return p;
}

// ---------------------------------------------------------------------------
// Factory: create backend by name
//   "simple"    → SimpleDRAMBackend (analytical, no cycle-level detail)
//   "ddr5"      → DDR5Backend with DDR5-4800 timing
//   "ddr5_4800" → DDR5Backend with DDR5-4800 timing
//   "ddr5_6400" → DDR5Backend with DDR5-6400 timing
//   "ddr4"      → DDR5Backend with DDR4-3200 timing
//   "ddr4_3200" → DDR5Backend with DDR4-3200 timing
//   "ddr4_2400" → DDR5Backend with DDR4-2400 timing
// ---------------------------------------------------------------------------
IDRAMBackend* createDRAMBackend(
    const std::string& type,
    const DRAMTimingParams& timing,
    int numChannels,
    int numRanks,
    int numBanks,
    const std::string& configFile = "");

}  // namespace cxl
