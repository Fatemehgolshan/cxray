#include "aol_based_migration_trigger.h"
#include "page_migrator.h"

void AolBasedMigrationTrigger::accrue_until_(uint64_t now) {
    if (last_event_cycle_ == 0) { last_event_cycle_ = now; return; }
    uint64_t delta = now - last_event_cycle_;
    cycles_ += delta;
    if (outstanding_ > 0) {
        a1_cycles_with_outstanding_ += delta;
    }
    last_event_cycle_ = now;
}

void AolBasedMigrationTrigger::onAccessStart(PageLocation /*loc*/, uint64_t start) {
    // Normally protect with a mutex if multi-threaded.
    accrue_until_(start);
    ++outstanding_;
    ++a3_total_reqs_;
}

void AolBasedMigrationTrigger::onAccessComplete(PageLocation /*loc*/, uint64_t /*start*/, uint64_t done) {
    // Normally protect with a mutex if multi-threaded.
    accrue_until_(done);
    if (outstanding_ > 0) --outstanding_;
}

void AolBasedMigrationTrigger::onMigrationIntervalBoundary() {
    // Normally protect with a mutex if multi-threaded.
    accrue_until_(last_event_cycle_);

    last_aol_ = (a3_total_reqs_ > 0)
        ? static_cast<double>(a1_cycles_with_outstanding_) / static_cast<double>(a3_total_reqs_)
        : 0.0;

    last_scale_ = scale_from_aol_(last_aol_, aol_low_, aol_high_);
}

uint64_t AolBasedMigrationTrigger::computePromotionBudget(const PageMigrator& migrator) {
    uint64_t base = migrator.getNumFastMemPages();
    double cap = last_scale_;
    if (cap <= 0.0) return 0;
    uint64_t budget = static_cast<uint64_t>(cap * static_cast<double>(base));
    return budget ? budget : (last_scale_ > 0.0 ? 1 : 0);
}

void AolBasedMigrationTrigger::resetInterval() {
    // Normally protect with a mutex if multi-threaded.
    cycles_ = 0;
    a1_cycles_with_outstanding_ = 0;
    a3_total_reqs_ = 0;
}

double AolBasedMigrationTrigger::scale_from_aol_(double aol, double low, double high) {
    if (aol <= low)  return 0.0;
    if (aol >= high) return 1.0;
    return (aol - low) / (high - low);
}

