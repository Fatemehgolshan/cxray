#include "latency_based_migration_trigger.h"
#include "page_migrator.h" // for getNumFastMemPages(), getPageSizeBytes() if needed
#include <algorithm>
#include <cmath>

void LatencyBasedMigrationTrigger::onAccessComplete(PageLocation loc, uint64_t start, uint64_t done) {
    // Only call this for read requests (GETS/GETX).
    const uint64_t lat = (done >= start) ? (done - start) : 0ULL;
    if (loc == /* FAST_MEM */ 0) { // adjust if your enum uses different values
        sum_lat_fast_ += lat;
        ++cnt_fast_;
    } else { // SLOW_MEM
        sum_lat_slow_ += lat;
        ++cnt_slow_;
    }
}

void LatencyBasedMigrationTrigger::onMigrationIntervalBoundary() {
    const uint64_t tot = cnt_fast_ + cnt_slow_;

    // Average read latency per tier (cycles)
    double raw_Ld = (cnt_fast_ > 0) ? static_cast<double>(sum_lat_fast_) / static_cast<double>(cnt_fast_) : 0.0;
    double raw_La = (cnt_slow_ > 0) ? static_cast<double>(sum_lat_slow_) / static_cast<double>(cnt_slow_) : 0.0;

    // Apply EWMA smoothing to reduce noise (paper mentions this)
    if (smooth_Ld_ == 0.0) smooth_Ld_ = raw_Ld; // first measurement
    if (smooth_La_ == 0.0) smooth_La_ = raw_La;
    smooth_Ld_ = (1.0 - ewma_alpha_) * smooth_Ld_ + ewma_alpha_ * raw_Ld;
    smooth_La_ = (1.0 - ewma_alpha_) * smooth_La_ + ewma_alpha_ * raw_La;

    // Use smoothed values for decisions
    Ld_ = smooth_Ld_;
    La_ = smooth_La_;

    info("[LatencyTrigger] Raw latencies - Fast: %.2f, Slow: %.2f", raw_Ld, raw_La);
    info("[LatencyTrigger] Smoothed latencies - Fast: %.2f, Slow: %.2f (α=%.2f)", Ld_, La_, ewma_alpha_);
    info("[LatencyTrigger] Accesses - Fast: %lu, Slow: %lu", cnt_fast_, cnt_slow_);

    // Fraction of reads going to FAST
    p_  = (tot > 0) ? static_cast<double>(cnt_fast_) / static_cast<double>(tot) : 0.0;
    const double Lref = std::max(Ld_, La_);
    const double latencyDiff = std::abs(Ld_ - La_);
    const double balancedThreshold = delta_ * Lref;
    
    info("[LatencyTrigger] Balance check: |%.2f - %.2f| = %.2f vs threshold %.2f (δ=%.3f)", 
         Ld_, La_, latencyDiff, balancedThreshold, delta_);    
    if (Lref <= 0.0 || latencyDiff < balancedThreshold) {
        delta_p_ = 0.0;
        info("[LatencyTrigger] BALANCED - No migration needed (latencies within δ threshold)");
        return;
    }

    // Direction: if FAST is quicker, promote; otherwise demote
    promote_ = (Ld_ < La_);
    info("[LatencyTrigger] Direction: %s (Ld_=%.2f %s La_=%.2f)", 
         promote_ ? "PROMOTE" : "DEMOTE", Ld_, promote_ ? "<" : ">", La_);

    // Colloid Algorithm 2: Binary search watermark update
    if (promote_) {
        p_lo_ = p_;  // LD < LA, so p is a valid lower bound
    } else {
        p_hi_ = p_;  // LD > LA, so p is a valid upper bound  
    }
    // Collapse protection: re-open the far side if needed
    if (p_hi_ < p_lo_ + eps_) {
        if (promote_) {
            p_hi_ = 1.0;  // Reset upper bound - equilibrium likely higher
            info("[LatencyTrigger] Reset p_hi=1.0 (equilibrium shifted higher)");
        } else {
            p_lo_ = 0.0;  // Reset lower bound - equilibrium likely lower  
            info("[LatencyTrigger] Reset p_lo=0.0 (equilibrium shifted lower)");
        }
    }

    const double pmid = 0.5 * (p_lo_ + p_hi_);
    delta_p_ = std::abs(pmid - p_);
    info("[LatencyTrigger] CONVERGENCE: p_lo=%.3f ≤ p*≤ p_hi=%.3f, current p=%.3f, shift=%.3f",
         p_lo_, p_hi_, p_, delta_p_);
    info("[LatencyTrigger] WATERMARK_GAP: %.3f (ε=%.3f), EQUILIBRIUM_DISTANCE: %.1fx",
         p_hi_ - p_lo_, eps_, latencyDiff / balancedThreshold);
}

uint64_t LatencyBasedMigrationTrigger::computePromotionBudget(const PageMigrator& migrator) {
    if (delta_p_ <= 0.0) return 0;

    // Convert Δp·requests to pages.
    const uint64_t tot_reqs = cnt_fast_ + cnt_slow_;
    const double request_shift = delta_p_ * static_cast<double>(tot_reqs);
    const double bytes_to_migrate = request_shift * static_cast<double>(line_bytes_);

    uint64_t dynamic_pages = 0;
    if (page_bytes_ > 0) {
        dynamic_pages = static_cast<uint64_t>(bytes_to_migrate / static_cast<double>(page_bytes_));
    }
    const uint64_t static_limit = (static_cap_pages_ > 0) ? static_cap_pages_ : 1000;

    // COLLOID: Take minimum of dynamic limit and static limit
    uint64_t final_budget = std::min(dynamic_pages, static_limit);
    
    // Ensure at least 1 page if we need to shift and have requests
    if (final_budget == 0 && delta_p_ > 0.0 && tot_reqs > 0) {
        final_budget = 1;
    }
    
    info("[LatencyTrigger] BUDGET: Δp=%.4f × %lu reqs = %.1f req_shift → %.1f bytes → %lu pages (cap: %lu)",
         delta_p_, tot_reqs, request_shift, bytes_to_migrate, dynamic_pages, static_limit);
    info("[LatencyTrigger] FINAL_BUDGET: %lu pages", final_budget);
    
    return final_budget;
}

void LatencyBasedMigrationTrigger::resetInterval() {
    sum_lat_fast_ = sum_lat_slow_ = 0;
    cnt_fast_ = cnt_slow_ = 0;
    // keep p_lo_/p_hi_ across windows for stability
}

