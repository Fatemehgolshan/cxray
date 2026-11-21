#ifndef LATENCY_BASED_MIGRATION_TRIGGER_H
#define LATENCY_BASED_MIGRATION_TRIGGER_H

#include <cstdint>
#include "migration_trigger.h"

class PageMigrator;

class LatencyBasedMigrationTrigger : public MigrationTrigger {
public:
    LatencyBasedMigrationTrigger(
            double eps, double delta,
            uint64_t static_cap_pages,
            uint32_t line_bytes,
            uint64_t page_bytes)
            :eps_(eps), delta_(delta),
            static_cap_pages_(static_cap_pages),
            line_bytes_(line_bytes), page_bytes_(page_bytes) {}

    void onAccessComplete(PageLocation loc, uint64_t start, uint64_t done) override;
    void onMigrationIntervalBoundary() override;
    uint64_t computePromotionBudget(const PageMigrator& migrator) override;
    // Direction this window: true = promote (SLOW→FAST), false = demote (FAST→SLOW).
    bool promoteDirection() const override { return promote_; }
    void resetInterval() override;
    
private:
    // ---- Config ----
    // Per-window accumulators
    uint64_t sum_lat_fast_ = 0;
    uint64_t sum_lat_slow_ = 0;
    uint64_t cnt_fast_ = 0;
    uint64_t cnt_slow_ = 0;

    double ewma_alpha_ = 0.1;  // smoothing factor
    double smooth_Ld_ = 0.0;   // smoothed fast tier latency
    double smooth_La_ = 0.0;   // smoothed slow tier latency

    // Derived signals
    double Ld_ = 0.0;       // avg read latency for FAST
    double La_ = 0.0;       // avg read latency for SLOW
    double p_ = 0.0;        // fraction of reads going to FAST
    bool   promote_ = true;  // direction flag
    double delta_p_ = 0.0;  // target shift in access probability

    // Watermarks and thresholds
    double p_lo_ = 0.0;
    double p_hi_ = 1.0;
    const double eps_;      // collapse threshold
    const double delta_;    // balanced band

    // Caps / sizes
    const uint64_t static_cap_pages_;
    const uint32_t line_bytes_;
    const uint64_t page_bytes_;

};
#endif //AOLBASED_MIGRATION_TRIGGER_H_
