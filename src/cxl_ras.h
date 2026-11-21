#pragma once
// ---------------------------------------------------------------------------
// cxl_ras.h — CXL 3.1 RAS Engine
//
// Implements a timing-approximate Reliability, Availability, Serviceability subset:
//   • ScrubEngine    — patrol scrub as actual competing DRAM traffic
//   • PPREngine      — sPPR (bank-level) and hPPR (rank-level) repair
//   • ErrorEscalator — CE accumulation, simplified CVME/DBIT escalation
//   • ViralProtocol  — simplified viral containment timing/flag model
//   • PoisonManager  — per-cacheline data poison per CXL 3.1 spec
//   • CorrelatedFaultEngine — spatial/temporal correlated fault model
//   • RASEngine      — orchestrator combining all of the above
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// MODEL FIDELITY NOTES (cxl_ras.h)
//
// Tier: timing-approximate (fast-sim default) | timing-calibrated (vendor profile)
//
// What IS modeled:
//   - Patrol scrub as competing DRAM traffic with bank-conflict contention
//   - sPPR and hPPR: bank/rank blocking with configurable repair durations
//   - CE accumulation, simplified CVME events, DBIT row tracking
//   - Viral containment timing/flag behavior (NORMAL → PENDING → ACTIVE → CONTAINED)
//   - Data poison: per-cacheline poison bit, read/write propagation
//   - Correlated fault models: spatial, temporal aging, burst, row hammer
//
// What is NOT modeled:
//   - Full CXL 3.1 RAS message sequencing (CXL.io RAS transactions)
//   - Host software response to CVME/viral events (OS/hypervisor paths)
//   - Multi-rank scrub scheduling coordination
//   - sPPR/hPPR DQ-level granularity (modeled at bank level only)
//   - ECC syndrome decoding latency (modeled as zero)
//   - DDR5 ECS (Error Check Scrub) mode beyond BW fraction accounting
//   - PMIC (power management) interactions during repair
// ---------------------------------------------------------------------------

#include <cstdint>
#include <deque>
#include <vector>
#include <string>
#include <random>
#include <memory>
#include <unordered_set>
#include <unordered_map>

namespace cxl {

// Forward declaration (defined in ddr5_backend.h)
class DDR5Backend;

// ── Fault model selection ────────────────────────────────────────────────────

enum class FaultModel : uint8_t {
    INDEPENDENT = 0,  // current: Bernoulli per-access (fast mode)
    CORRELATED  = 1,  // realistic: spatial+temporal correlation (detailed mode)
};

struct CorrelatedFaultConfig {
    FaultModel model = FaultModel::INDEPENDENT;

    // Spatial correlation: once a row has a CE, probability of another CE
    // on the SAME row is multiplied by this factor.
    double rowRepeatFactor  = 100.0;   // 100× more likely on same row

    // Temporal correlation (row aging): error rate grows with activation count.
    // errorRate(n) = baseRate * (1 + agingFactor * log2(1 + n / agingThreshold))
    double agingFactor      = 3.0;
    double agingThreshold   = 1e6;    // activations before aging kicks in

    // Burst fault model: single-event upsets cluster in time.
    // After a CE/UCE, P(another error within burstWindowNs) = burstProbability.
    double burstWindowNs    = 1000.0; // 1 µs burst window
    double burstProbability = 0.15;   // 15% chance of follow-on error in window

    // Row hammer: activations per interval that trigger hammering errors on adjacent rows.
    bool     rowHammerEnable   = true;
    uint32_t rowHammerThresh   = 32768;   // activations per refresh interval
    double   rowHammerErrProb  = 0.01;    // P(error on adjacent row) when threshold hit
    double   refreshIntervalNs = 3900.0;  // DDR5 tREFI = 3.9 µs
};

// ── RAS timing calibration profiles ─────────────────────────────────────────
// Profiles encode the trade-off between simulation speed and JEDEC/spec realism.
//
// FAST_SIM (default): compressed timings for simulation throughput.
//   PPR durations are ~1000× shorter than JEDEC spec to avoid trace stalls.
//   Suitable for sensitivity analysis and trend studies.
//
// JEDEC_DDR5: full JEDEC-scale timings per JESD79-5B and CXL 3.1 spec.
//   sPPR: ~100 ms per JEDEC (modeled as repair window stall).
//   hPPR: ~10–30 minutes (modeled as long rank block).
//   Use for RAS behavior studies where absolute timing matters.
//
// VENDOR_CALIBRATED: mid-point based on published ASIC measurements.
//   sPPR: ~5 µs (observed on production CXL ASIC with fast repair).
//   hPPR: ~5 ms (optimistic vendor implementation).
//
enum class RASTimingProfile : uint8_t {
    FAST_SIM          = 0,  // default: compressed for simulation speed
    JEDEC_DDR5        = 1,  // full JEDEC JESD79-5B + CXL 3.1 spec timings
    VENDOR_CALIBRATED = 2,  // mid-point from published CXL ASIC measurements
};

// ── Configuration ────────────────────────────────────────────────────────────

struct RASConfig {
    bool     enable                  = true;

    // Active timing profile (informational; set by withProfile()).
    RASTimingProfile timingProfile   = RASTimingProfile::FAST_SIM;

    // Patrol scrub interval between consecutive scrub reads.
    // CXL 3.1 §12.3: scrub must complete full address range within
    // the scrub period (default 24h). 100µs/row ≈ 1 scrub pass/day for 32GB.
    double   patrolScrubIntervalUs   = 100.0;   // [µs] CXL 3.1 §12.3

    // Maximum fraction of DRAM bandwidth consumed by patrol scrub.
    // JEDEC JESD79-5B: scrub bandwidth must not exceed 5% of peak BW.
    double   patrolScrubBwFraction   = 0.05;    // JESD79-5B §3.5

    bool     ddr5ErrScrub            = true;    // DDR5 error-check scrub (ECS)

    // Scrub bank-conflict penalty (ns): added when scrub is on a different page
    // but same bank as the request. Calibrated per DRAM timing.
    // Default (DDR5-4800): ~tRCD/2 = ~8 ns contention estimate.
    double   scrubBankConflictNs     = 8.0;

    // CVME (Correctable Volatile Memory Error) threshold (CXL 3.1 §12.5).
    // 256 CEs in a 1000µs window triggers a CVME event and device notification.
    uint32_t cvmeThreshold           = 256;     // CXL 3.1 §12.5
    double   cvmeWindowUs            = 1000.0;  // [µs] CXL 3.1 §12.5

    // PPR
    bool     pprEnable               = true;
    bool     spprEnable              = true;    // soft/sPPR enable
    bool     hpprEnable              = true;    // hard/hPPR enable

    // sPPR repair duration: per JEDEC, sPPR completes in <100ms.
    // Fast-sim default is 1µs (1000× compressed) to keep trace timing tractable.
    double   spprLatencyUs           = 1.0;     // [µs] fast-sim; JEDEC spec: <100ms

    // hPPR repair duration.
    // JEDEC JESD79-5B §4.6: hPPR requires rank-level offline window, typically
    // 10–30 minutes on DDR5. Fast-sim uses 10ms.
    double   hpprDurationMs          = 10.0;    // [ms] fast-sim; JEDEC spec: 10–30 min

    uint32_t pprMaxRepairs           = 128;     // max sPPR rows per rank (JEDEC)
    uint32_t rowCEThreshold          = 4;       // CEs per row before sPPR trigger

    // Data poison
    bool     dataPoisonEnable        = true;

    // Viral signaling
    bool     viralEnable             = true;

    // Viral propagation: stall added when CXL link enters VIRAL_ACTIVE state.
    // CXL 3.1 §13.2: viral bit propagates upstream within one FLIT round-trip.
    // Modeled as per-request stall rather than link-level state change.
    double   viralPropagationDelayNs = 10.0;    // [ns] CXL 3.1 §13.2
    double   viralStallNs            = 50.0;    // [ns/req] during VIRAL_ACTIVE

    // DBIT (Dynamic Bad Row Tracking)
    bool     dbitEnable              = true;
    uint32_t dbitRowCEThreshold      = 1;       // min CEs to add row to DBIT

    // Error injection (for testing)
    bool     injectEnable            = false;
    double   ceRate                  = 0.0;     // per-access CE probability
    double   uceRate                 = 0.0;     // per-access UCE probability

    // Correlated fault model configuration
    CorrelatedFaultConfig correlatedFaults;

    // Factory: build a RASConfig with timing constants set from the named profile.
    static RASConfig withProfile(RASTimingProfile p) {
        RASConfig c;
        c.timingProfile = p;
        switch (p) {
        case RASTimingProfile::FAST_SIM:
            // Defaults already set above
            c.scrubBankConflictNs = 8.0;
            break;
        case RASTimingProfile::JEDEC_DDR5:
            // JESD79-5B and CXL 3.1 §12.x realistic timings
            c.patrolScrubIntervalUs   = 100.0;
            c.spprLatencyUs           = 100000.0;   // sPPR: ~100ms per JEDEC
            c.hpprDurationMs          = 15000.0;    // hPPR: ~15 minutes (in ms)
            c.pprMaxRepairs           = 128;
            c.rowCEThreshold          = 4;
            c.viralPropagationDelayNs = 50.0;
            c.viralStallNs            = 200.0;      // realistic viral containment stall
            c.cvmeThreshold           = 64;         // stricter CVME window
            c.cvmeWindowUs            = 100.0;
            c.scrubBankConflictNs     = 16.67;      // tRCD for DDR5-4800 CL40
            break;
        case RASTimingProfile::VENDOR_CALIBRATED:
            // Mid-point from published CXL ASIC measurements
            c.patrolScrubIntervalUs   = 100.0;
            c.spprLatencyUs           = 5.0;        // 5 µs: fast sPPR on CXL ASIC
            c.hpprDurationMs          = 5.0;        // 5 ms: optimistic hPPR
            c.pprMaxRepairs           = 256;        // higher capacity
            c.rowCEThreshold          = 2;          // lower threshold → earlier PPR
            c.viralPropagationDelayNs = 15.0;
            c.viralStallNs            = 80.0;
            c.cvmeThreshold           = 128;
            c.cvmeWindowUs            = 500.0;
            c.scrubBankConflictNs     = 10.0;
            break;
        }
        return c;
    }
};

// ── Event log structs ────────────────────────────────────────────────────────

struct PPREvent {
    double   timeNs;
    uint64_t rowAddr;
    double   durationNs;
    bool     isHPPR;       // false = sPPR, true = hPPR
    int      rankId;
    int      bankId;
};

struct ErrorEvent {
    double   timeNs;
    uint64_t addr;
    bool     correctable;
};

struct PoisonEvent {
    double      timeNs;
    uint64_t    addr;
    std::string eventType;  // "inject" | "read" | "write_propagate"
};

struct ViralEvent {
    double      timeNs;
    std::string componentId;
    std::string event;  // "viral_pending" | "viral_active" | "contained"
};

// ── Viral protocol state ─────────────────────────────────────────────────────

enum class ViralState {
    NORMAL,
    VIRAL_PENDING,
    VIRAL_ACTIVE,
    CONTAINED
};

// ============================================================================
// ScrubEngine — patrol scrub modeled as competing DRAM traffic
// ============================================================================

class ScrubEngine {
public:
    explicit ScrubEngine(const RASConfig& cfg)
        : cfg_(cfg), scrubAddr_(0), lastScrubNs_(0.0), totalScrubBytes_(0) {}

    /// Compute extra latency due to scrub-vs-request bank contention.
    /// If a scrub read is active on the same bank as `addr`, returns wait.
    /// Also advances the scrub position and schedules the next scrub read.
    double getContentionNs(uint64_t addr, double nowNs, DDR5Backend* dram);

    /// Fraction of address space scrubbed so far.
    double scrubCoverage(uint64_t totalDramBytes) const {
        if (totalDramBytes == 0) return 0.0;
        return std::min(1.0, static_cast<double>(totalScrubBytes_) / totalDramBytes);
    }

    uint64_t scrubAddr()    const { return scrubAddr_; }
    double   lastScrubNs()  const { return lastScrubNs_; }

private:
    const RASConfig& cfg_;
    uint64_t scrubAddr_;           // next scrub position (advanced after each read)
    uint64_t scrubActiveAddr_ = 0; // address of the in-flight scrub read (for contention check)
    double   lastScrubNs_;         // when last scrub read was scheduled
    uint64_t totalScrubBytes_;     // total bytes scrubbed (for coverage)
    double   scrubDoneNs_ = 0.0;   // when the current scrub read completes
};

// ============================================================================
// PPREngine — sPPR and hPPR post-package repair
// ============================================================================

class PPREngine {
public:
    explicit PPREngine(const RASConfig& cfg) : cfg_(cfg) {}

    /// Trigger sPPR for a specific bank.  Calls dram->setPPRBankBlock().
    /// Returns the repair duration in ns.
    double triggerSPPR(uint64_t addr, double nowNs, DDR5Backend* dram);

    /// Trigger hPPR for a rank.  Calls dram->setHPPRRankBlock().
    /// Returns the repair duration in ns.
    double triggerHPPR(int rankId, double nowNs, DDR5Backend* dram);

    bool isBankBlocked(int globalBankId, double nowNs) const;
    bool isRankBlocked(int rankId, double nowNs) const;

    bool   spprCapacityExhausted() const { return spprCount_ >= cfg_.pprMaxRepairs; }
    bool   hpprTriggered() const { return hpprCount_ > 0; }

    const std::vector<PPREvent>& log() const { return log_; }
    uint64_t spprCount() const { return spprCount_; }
    uint64_t hpprCount() const { return hpprCount_; }

    double pprBusyUntilNs() const { return pprBusyUntilNs_; }

private:
    const RASConfig& cfg_;
    std::unordered_map<int, double> bankBlockUntil_;   // globalBankId → block end
    std::unordered_map<int, double> rankBlockUntil_;   // rankId        → block end
    double   pprBusyUntilNs_ = 0.0;
    uint64_t spprCount_      = 0;
    uint64_t hpprCount_      = 0;
    std::vector<PPREvent> log_;
};

// ============================================================================
// ErrorEscalator — CE accumulation, simplified CVME/DBIT escalation
// ============================================================================

class ErrorEscalator {
public:
    explicit ErrorEscalator(const RASConfig& cfg) : cfg_(cfg) {}

    /// Record a correctable error.  Returns true if CVME threshold was crossed.
    bool recordCE(uint64_t addr, double timeNs);

    /// Record an uncorrectable error.  Returns true (always escalates).
    bool recordUCE(uint64_t addr, double timeNs);

    /// Check whether a row should be tracked in DBIT.
    bool isDbitRow(uint64_t addr) const;

    uint64_t ceCount()  const { return ceCount_; }
    uint64_t uceCount() const { return uceCount_; }
    uint64_t dbitRows() const { return dbitRows_.size(); }

    const std::vector<ErrorEvent>& log() const { return errorLog_; }

private:
    const RASConfig& cfg_;

    // Global CE / UCE counters
    uint64_t ceCount_  = 0;
    uint64_t uceCount_ = 0;

    // Sliding CVME window (timestamps of recent CEs)
    std::deque<double> ceWindow_;

    // Per-cacheline CE count (for row-level tracking)
    std::unordered_map<uint64_t, uint32_t> clCECount_;

    // Per-rank CE count (simplified: rank 0 only in single-rank config)
    std::unordered_map<int, uint32_t> rankCECount_;

    // DBIT: rows with CEs exceeding threshold
    std::unordered_set<uint64_t> dbitRows_;

    std::vector<ErrorEvent> errorLog_;
};

// ============================================================================
// ViralProtocol — simplified viral containment timing/flag model
// ============================================================================

class ViralProtocol {
public:
    explicit ViralProtocol(const RASConfig& cfg)
        : cfg_(cfg), state_(ViralState::NORMAL) {}

    /// Called on UCE: begin viral transition.
    void onUCE(double timeNs);

    /// Called per request: apply viral stall if VIRAL_ACTIVE.
    /// Returns extra latency (ns).
    double process(double nowNs);

    /// Set viral bit in response FLIT header?
    bool viralBitSet() const {
        return state_ == ViralState::VIRAL_ACTIVE ||
               state_ == ViralState::VIRAL_PENDING;
    }

    bool isActive() const { return state_ == ViralState::VIRAL_ACTIVE; }
    bool isContained() const { return state_ == ViralState::CONTAINED; }

    ViralState state() const { return state_; }
    const std::vector<ViralEvent>& log() const { return log_; }
    uint64_t eventCount() const { return log_.size(); }

private:
    const RASConfig& cfg_;
    ViralState       state_;
    double           pendingSince_ = 0.0;
    std::vector<ViralEvent> log_;
};

// ============================================================================
// PoisonManager — per-cacheline data poison
// ============================================================================

class PoisonManager {
public:
    explicit PoisonManager(const RASConfig& cfg) : cfg_(cfg) {}

    /// Inject poison for the cacheline containing addr (on UCE).
    void inject(uint64_t addr, double timeNs);

    /// Check for poison on read; returns extra latency if poisoned.
    /// Sets berrOut = true if the BERR bit should be set in S2M_DRS FLIT.
    double onRead(uint64_t addr, double timeNs, bool& berrOut);

    /// Handle write to poisoned line: propagate poison to destination.
    double onWrite(uint64_t addr, double timeNs);

    /// Clear poison from the cacheline containing addr (called after successful
    /// sPPR/hPPR repair — the repaired row is no longer poisoned).
    void clear(uint64_t addr, double timeNs);

    bool isPoisoned(uint64_t addr) const;
    uint64_t poisonedLines() const { return poisonedAddrs_.size(); }

    const std::vector<PoisonEvent>& log() const { return log_; }

private:
    const RASConfig& cfg_;
    std::unordered_set<uint64_t> poisonedAddrs_;   // cacheline-aligned addrs
    std::unordered_set<uint64_t> loggedPoisonReads_; // dedup: only log first read per cacheline
    std::vector<PoisonEvent>     log_;
};

// ============================================================================
// CorrelatedFaultEngine — spatial + temporal correlated fault model
// ============================================================================

class CorrelatedFaultEngine {
public:
    explicit CorrelatedFaultEngine(const RASConfig& cfg, uint32_t seed = 0xABCD1234u);

    struct FaultResult {
        bool     ce        = false;   // correctable error occurred
        bool     uce       = false;   // uncorrectable error occurred
        double   extraNs   = 0.0;     // latency impact (e.g. scrub-wait on error row)
        uint64_t faultAddr = 0;       // address of fault (may differ from request addr)
    };

    /// Evaluate whether this access causes an error, using the configured fault model.
    /// addr: physical address being accessed
    /// isWrite: true for writes
    /// nowNs: current simulation time
    /// activationCount: how many times this row has been activated (for aging)
    FaultResult evaluate(uint64_t addr, bool isWrite, double nowNs,
                         uint64_t activationCount = 0);

    /// Record that row `rowAddr` has been activated (for row-hammer tracking).
    void recordActivation(uint64_t rowAddr, double nowNs);

    /// Returns true if row hammer threshold has been crossed for `rowAddr`.
    bool rowHammerTriggered(uint64_t rowAddr, double nowNs) const;

    uint64_t totalCEs()  const { return totalCEs_; }
    uint64_t totalUCEs() const { return totalUCEs_; }

private:
    const RASConfig& cfg_;
    std::mt19937_64  rng_;

    // Per-row CE count (for spatial correlation)
    std::unordered_map<uint64_t, uint32_t> rowCECount_;

    // Per-row activation count (for aging + row hammer)
    struct RowActivity {
        uint64_t activations       = 0;
        double   windowStartNs     = 0.0;
        uint64_t windowActivations = 0;
    };
    std::unordered_map<uint64_t, RowActivity> rowActivity_;

    // Burst state: time of last error, for burst probability
    double lastErrorNs_ = -1e18;

    // Stats
    uint64_t totalCEs_  = 0;
    uint64_t totalUCEs_ = 0;

    // Helpers
    uint64_t rowAddr(uint64_t addr) const { return addr >> 13; }  // 8KB rows
    double   effectiveRate(uint64_t addr, bool isWrite, uint64_t activations) const;
    bool     inBurstWindow(double nowNs) const;
    uint64_t adjacentRow(uint64_t rAddr, int offset) const;
};

// ============================================================================
// RASEngine — orchestrator
// ============================================================================

class RASEngine {
public:
    explicit RASEngine(const RASConfig& cfg, uint32_t seed = 42);

    // ── Per-request processing ────────────────────────────────────────────

    /// Process one request through all RAS components.
    /// Returns a Result summarizing all RAS effects on this request.
    struct Result {
        double extraNs     = 0.0;   // total latency overhead from RAS
        bool   berr        = false; // set BERR bit in S2M_DRS FLIT response
        bool   viral       = false; // viral bit in response FLIT
        bool   poisonRead  = false; // read returned poisoned data
    };

    Result process(uint64_t addr, bool isWrite, double dispatchNs,
                   uint32_t sizeBytes, DDR5Backend* dram = nullptr);

    // Legacy interface (no DDR5Backend pointer, for SimpleDRAMBackend use)
    // Returns only the extraNs latency overhead.
    double processLegacy(uint64_t addr, bool isWrite, double dispatchNs,
                         uint32_t sizeBytes) {
        auto r = process(addr, isWrite, dispatchNs, sizeBytes, nullptr);
        return r.extraNs;
    }

    // ── Manual injection ─────────────────────────────────────────────────
    void injectCE(uint64_t addr, double timeNs);
    void injectUCE(uint64_t addr, double timeNs);
    /// Directly trigger a PPR repair (sPPR or hPPR) at the given time.
    void injectPPR(uint64_t addr, double timeNs, bool hard = false,
                   DDR5Backend* dram = nullptr);

    // Record row activation for row-hammer tracking (call once per DRAM row activation).
    void recordActivation(uint64_t addr, double nowNs);

    // ── Queries ──────────────────────────────────────────────────────────
    bool isPoisoned(uint64_t addr) const;
    bool isViralActive()    const { return viral_.isActive(); }
    bool isBankBlocked(int globalBankId, double nowNs) const;
    bool isRankBlocked(int rankId,       double nowNs) const;

    // Legacy compatibility
    double pprBusyUntilNs() const { return ppr_.pprBusyUntilNs(); }

    // ── Statistics ───────────────────────────────────────────────────────
    uint64_t correctedErrors()   const { return escalator_.ceCount(); }
    uint64_t uncorrectedErrors() const { return escalator_.uceCount(); }
    uint64_t sPPREvents()        const { return ppr_.spprCount(); }
    uint64_t hPPREvents()        const { return ppr_.hpprCount(); }
    uint64_t poisonedLines()     const { return poison_.poisonedLines(); }
    uint64_t viralEvents()       const { return viral_.eventCount(); }
    uint64_t dbitRows()          const { return escalator_.dbitRows(); }
    double   scrubCoverage()     const;  // fraction of DRAM scrubbed

    // Legacy compatibility aliases
    uint64_t pprEventCount()     const { return sPPREvents() + hPPREvents(); }
    uint64_t poisonedLineCount() const { return poisonedLines(); }
    uint64_t viralEventCount()   const { return viralEvents(); }
    double   scrubBwOverhead()   const { return cfg_.enable ? cfg_.patrolScrubBwFraction : 0.0; }

    // ── Log accessors ────────────────────────────────────────────────────
    const std::vector<ErrorEvent>&  errorLog()  const { return escalator_.log(); }
    const std::vector<PPREvent>&    pprLog()    const { return ppr_.log(); }
    const std::vector<PoisonEvent>& poisonLog() const { return poison_.log(); }
    const std::vector<ViralEvent>&  viralLog()  const { return viral_.log(); }

    const RASConfig& config() const { return cfg_; }

private:
    void triggerCVME(uint64_t addr, double timeNs, DDR5Backend* dram);
    void triggerPPR (uint64_t addr, double timeNs, DDR5Backend* dram);
    void triggerViral(double timeNs);

    RASConfig        cfg_;
    std::mt19937_64  rng_;

    ScrubEngine      scrub_;
    PPREngine        ppr_;
    ErrorEscalator   escalator_;
    ViralProtocol    viral_;
    PoisonManager    poison_;

    // Correlated fault engine (non-null only in CORRELATED mode)
    std::unique_ptr<CorrelatedFaultEngine> corrFaults_;

    // Approximate total DRAM size for scrub coverage calculation
    uint64_t totalDramBytes_ = 128ULL * 1024 * 1024 * 1024;  // 128 GB default
};

}  // namespace cxl
