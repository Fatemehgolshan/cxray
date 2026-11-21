#pragma once
#include <cstdint>
#include <functional>
#include <queue>
#include <vector>
#include <memory>

namespace cxl {

// ── Discrete event queue ─────────────────────────────────────────────────────
// Events are ordered by time (min-heap). At equal times, lower priority value
// fires first (use priority=0 for urgent completions, priority=1 for arrivals).

struct SimEvent {
    double   time;
    int      priority = 1;
    uint64_t seqNo    = 0;          // tie-break on same time+priority
    std::function<void()> callback;

    bool operator>(const SimEvent& o) const {
        if (time != o.time) return time > o.time;
        if (priority != o.priority) return priority > o.priority;
        return seqNo > o.seqNo;
    }
};

class SimEventQueue {
public:
    SimEventQueue() = default;

    // Schedule a callback at the given simulation time.
    // priority: 0=completion (fires before arrivals at same time), 1=arrival
    void schedule(double time, std::function<void()> cb, int priority = 1);

    // Process all events up to (and including) endTime.
    void runUntil(double endTime);

    // Process all remaining events.
    void runAll();

    // Current simulation time (time of last processed event).
    double now() const { return now_; }

    // Number of pending events.
    size_t pending() const { return queue_.size(); }

    // Reset: clear all events and reset time to 0.
    void reset();

private:
    using PQ = std::priority_queue<SimEvent,
                                   std::vector<SimEvent>,
                                   std::greater<SimEvent>>;
    PQ       queue_;
    double   now_   = 0.0;
    uint64_t seq_   = 0;
};

// ── Token-based credit tracker ────────────────────────────────────────────────
// Models a finite pool of credit tokens (e.g. M2S_REQ credits in CXL.mem).
// When a consumer tries to take a credit and none are available, it blocks.
// When a credit is returned, the oldest waiter is unblocked immediately.
//
// Usage:
//   CreditTracker credits(evq, 32, rttNs);
//   credits.acquire(arrivalNs, [](double grantNs){ /* send FLIT at grantNs */ });
//   // ... RTT later:
//   credits.release(completionNs);

class CreditTracker {
public:
    // evq       : shared event queue (must outlive this object)
    // capacity  : total credits in pool
    // returnNs  : how long after issue a credit is automatically returned
    //             (used when explicit release() is not called in time)
    CreditTracker(SimEventQueue& evq, int capacity, double returnNs);

    // Try to acquire a credit. If available, calls onGrant(now) immediately (as an event
    // at arrivalNs). If not available, enqueues and calls onGrant when a credit frees up.
    void acquire(double arrivalNs,
                 std::function<void(double grantNs)> onGrant);

    // Return one credit at the given time.
    void release(double releaseNs);

    // Available credits right now (snapshot, not stable across event boundaries).
    int available() const { return available_; }
    int capacity()  const { return capacity_; }

    // Cumulative stall time across all acquire() calls.
    double totalStallNs() const { return totalStallNs_; }

private:
    SimEventQueue& evq_;
    int    capacity_;
    double returnNs_;
    int    available_;

    struct Waiter {
        double arrivalNs;
        std::function<void(double)> onGrant;
    };
    std::queue<Waiter> waiters_;
    double totalStallNs_ = 0.0;
};

// ── FLIT serializer ───────────────────────────────────────────────────────────
// Models a single TX lane: FLITs are serialized one after another.
// Multiple requests calling scheduleTx() queue up on the same physical lane.
//
// Usage:
//   FlitSerializer lane(evq, flitTxNs);
//   lane.scheduleTx(arrivalNs, numFlits,
//                   [](double txDoneNs){ /* response or credit handling */ });

class FlitSerializer {
public:
    FlitSerializer(SimEventQueue& evq, double flitTxNs);

    // Schedule numFlits worth of transmission on this lane.
    // Starts at max(arrivalNs, laneAvailableNs). Calls onComplete with the
    // time the last FLIT exits the TX serializer.
    void scheduleTx(double arrivalNs, int numFlits,
                    std::function<void(double txDoneNs)> onComplete);

    // Next time this lane is free.
    double availableNs() const { return laneAvailableNs_; }

    // Total FLITs transmitted.
    uint64_t totalFlits() const { return totalFlits_; }

private:
    SimEventQueue& evq_;
    double   flitTxNs_;
    double   laneAvailableNs_ = 0.0;
    uint64_t totalFlits_      = 0;
};

// ── DRAM request tracker ──────────────────────────────────────────────────────
// Wraps an IDRAMBackend in an event-driven interface.
// Polls via tick events to detect request completions.

class IDRAMBackend;  // forward decl (defined in cxl_dram_backend.h)

class DetailedDRAMQueue {
public:
    DetailedDRAMQueue(SimEventQueue& evq, IDRAMBackend& backend,
                      double pollIntervalNs = 1.0);

    // Submit a DRAM request. Calls onComplete(completionNs) when done.
    void submit(uint64_t addr, bool isWrite, double arrivalNs,
                std::function<void(double completionNs)> onComplete);

    // Number of in-flight DRAM requests.
    int inflight() const { return static_cast<int>(pending_.size()); }

private:
    SimEventQueue& evq_;
    IDRAMBackend&  backend_;
    double         pollIntervalNs_;

    struct PendingReq {
        uint64_t reqId;
        double   submittedNs;
        std::function<void(double)> onComplete;
    };
    std::vector<PendingReq> pending_;
    bool pollScheduled_ = false;

    void schedulePoll(double fromNs);
    void onPoll(double nowNs);
};

// ── Request pipeline state ────────────────────────────────────────────────────
// Tracks one in-flight request through the full CXL pipeline in detailed mode.
// Filled in stage by stage as events fire.

struct PipelineRequest {
    uint64_t id;
    double   arrivalNs       = 0;
    double   creditGrantNs   = 0;
    double   txStartNs       = 0;
    double   txDoneNs        = 0;
    double   deviceArrivalNs = 0;
    double   dramStartNs     = 0;
    double   dramDoneNs      = 0;
    double   respTxDoneNs    = 0;
    double   hostDoneNs      = 0;
    double   creditStallNs   = 0;
    bool     rowHit          = false;
    bool     done            = false;
};

}  // namespace cxl
