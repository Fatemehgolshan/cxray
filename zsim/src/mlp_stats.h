// mlp_stats.h
#pragma once
#include <cstdint>

struct mlp_stats_t {
    uint64_t last_t = 0;
    uint64_t inflight = 0;
    uint64_t a1_cycles = 0;          // cycles with inflight >= 1
    uint64_t a2_inflight_sum = 0;    // sum(inflight * dt)
    uint64_t a3_reqs = 0;            // arrivals in window

    inline void integrate_to(uint64_t t) {
        // clamp to maintain monotonic time
        if (t < last_t) t = last_t;
        uint64_t dt = t - last_t;
        if (dt) {
            if (inflight) a1_cycles += dt;            // busy cycles
            a2_inflight_sum += inflight * dt;         // request·cycles
            last_t = t;
        }
    }

    inline void on_enqueue(uint64_t t) {
        integrate_to(t);
        inflight++;
        a3_reqs++;
    }

    inline void on_complete(uint64_t t) {
        integrate_to(t);
        if (inflight) inflight--;     // defensive
    }

    inline void flush(uint64_t t) { integrate_to(t); }

    inline void reset_window() { a1_cycles = 0; a2_inflight_sum = 0; a3_reqs = 0; }

    inline double mlp() const {
        return (a1_cycles ? (double)a2_inflight_sum / (double)a1_cycles : 0.0);
    }
    inline double avg_latency_cycles() const {
        return (a3_reqs ? (double)a2_inflight_sum / (double)a3_reqs : 0.0);
    }
    inline double aol_cycles() const {
        return (a3_reqs ? (double)a1_cycles / (double)a3_reqs : 0.0);
    }
};

