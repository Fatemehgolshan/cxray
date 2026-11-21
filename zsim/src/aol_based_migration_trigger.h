#ifndef AOLBASED_MIGRATION_TRIGGER_H_
#define AOLBASED_MIGRATION_TRIGGER_H_

#include <cstdint>
#include "migration_trigger.h"

class PageMigrator;

class AolBasedMigrationTrigger final : public MigrationTrigger {
public:
    AolBasedMigrationTrigger(double aol_low_cycles, double aol_high_cycles)
        : aol_low_(aol_low_cycles), aol_high_(aol_high_cycles) {}

    void onAccessStart(PageLocation loc, uint64_t start) override;
    void onAccessComplete(PageLocation loc, uint64_t start, uint64_t done) override;
    void onMigrationIntervalBoundary() override;
    uint64_t computePromotionBudget(const PageMigrator& migrator) override;
    void resetInterval() override;

private:
    // ---- Config ----
    const double aol_low_;
    const double aol_high_;

    // ---- Interval accumulators ----
    uint64_t cycles_ = 0;
    uint64_t a1_cycles_with_outstanding_ = 0;
    uint64_t a3_total_reqs_ = 0;

    uint64_t outstanding_ = 0;
    uint64_t last_event_cycle_ = 0;

    double last_aol_ = 0.0;
    double last_scale_ = 1.0;

    // Note: No std::mutex here.
    // If TieringMemory::access() is called from multiple cores/threads
    // simultaneously, these shared counters can have data races.
    // In a multi-threaded simulator, you must protect updates with a lock
    // (e.g., std::mutex) or use atomics. For now, we omit it for simplicity.

    void accrue_until_(uint64_t now);
    static double scale_from_aol_(double aol, double low, double high);
};
#endif //AOLBASED_MIGRATION_TRIGGER_H_
