#include "cxl_sim_engine.h"
#include "cxl_dram_backend.h"

#include <algorithm>
#include <cassert>
#include <stdexcept>

namespace cxl {

// ── SimEventQueue ─────────────────────────────────────────────────────────────

void SimEventQueue::schedule(double time, std::function<void()> cb, int priority) {
    SimEvent ev;
    ev.time     = time;
    ev.priority = priority;
    ev.seqNo    = seq_++;
    ev.callback = std::move(cb);
    queue_.push(std::move(ev));
}

void SimEventQueue::runUntil(double endTime) {
    while (!queue_.empty() && queue_.top().time <= endTime) {
        SimEvent ev = queue_.top();
        queue_.pop();
        now_ = ev.time;
        ev.callback();
    }
}

void SimEventQueue::runAll() {
    while (!queue_.empty()) {
        SimEvent ev = queue_.top();
        queue_.pop();
        now_ = ev.time;
        ev.callback();
    }
}

void SimEventQueue::reset() {
    // Clear the priority queue by swapping with an empty one.
    PQ empty;
    queue_.swap(empty);
    now_ = 0.0;
    seq_ = 0;
}

// ── CreditTracker ─────────────────────────────────────────────────────────────

CreditTracker::CreditTracker(SimEventQueue& evq, int capacity, double returnNs)
    : evq_(evq)
    , capacity_(capacity)
    , returnNs_(returnNs)
    , available_(capacity)
{}

void CreditTracker::acquire(double arrivalNs,
                             std::function<void(double grantNs)> onGrant) {
    if (available_ > 0) {
        --available_;

        // Schedule grant event at arrivalNs (priority=1, arrival).
        evq_.schedule(arrivalNs, [onGrant, arrivalNs]() {
            onGrant(arrivalNs);
        }, 1);

        // Schedule automatic credit return only when returnNs_ > 0.
        // Pass returnNs_ = 0 to disable auto-return and rely on explicit release().
        if (returnNs_ > 0.0) {
            double returnTime = arrivalNs + returnNs_;
            evq_.schedule(returnTime, [this, returnTime]() {
                release(returnTime);
            }, 0);
        }
    } else {
        // No credits available — enqueue waiter.
        Waiter w;
        w.arrivalNs = arrivalNs;
        w.onGrant   = std::move(onGrant);
        waiters_.push(std::move(w));
    }
}

void CreditTracker::release(double releaseNs) {
    if (!waiters_.empty()) {
        Waiter w = std::move(waiters_.front());
        waiters_.pop();

        double stall = releaseNs - w.arrivalNs;
        if (stall > 0.0) {
            totalStallNs_ += stall;
        }

        // Grant the credit to the waiter at releaseNs (priority=0, completion).
        auto onGrant = std::move(w.onGrant);
        evq_.schedule(releaseNs, [onGrant, releaseNs]() {
            onGrant(releaseNs);
        }, 0);

        // Schedule auto-return for this granted credit (if auto-return is enabled).
        if (returnNs_ > 0.0) {
            double returnTime = releaseNs + returnNs_;
            evq_.schedule(returnTime, [this, returnTime]() {
                release(returnTime);
            }, 0);
        }
    } else {
        // No waiters — return credit to pool, capped at capacity.
        available_ = std::min(available_ + 1, capacity_);
    }
}

// ── FlitSerializer ────────────────────────────────────────────────────────────

FlitSerializer::FlitSerializer(SimEventQueue& evq, double flitTxNs)
    : evq_(evq)
    , flitTxNs_(flitTxNs)
{}

void FlitSerializer::scheduleTx(double arrivalNs, int numFlits,
                                 std::function<void(double txDoneNs)> onComplete) {
    double startNs  = std::max(arrivalNs, laneAvailableNs_);
    double doneNs   = startNs + static_cast<double>(numFlits) * flitTxNs_;
    laneAvailableNs_ = doneNs;
    totalFlits_     += static_cast<uint64_t>(numFlits);

    evq_.schedule(doneNs, [onComplete, doneNs]() {
        onComplete(doneNs);
    }, 0);  // priority=0: completion fires before arrivals at same time
}

// ── DetailedDRAMQueue ─────────────────────────────────────────────────────────

DetailedDRAMQueue::DetailedDRAMQueue(SimEventQueue& evq, IDRAMBackend& backend,
                                     double pollIntervalNs)
    : evq_(evq)
    , backend_(backend)
    , pollIntervalNs_(pollIntervalNs)
{}

void DetailedDRAMQueue::submit(uint64_t addr, bool isWrite, double arrivalNs,
                                std::function<void(double completionNs)> onComplete) {
    uint64_t reqId = backend_.addRequest(addr, isWrite, arrivalNs);

    PendingReq pr;
    pr.reqId       = reqId;
    pr.submittedNs = arrivalNs;
    pr.onComplete  = std::move(onComplete);
    pending_.push_back(std::move(pr));

    if (!pollScheduled_) {
        schedulePoll(arrivalNs + pollIntervalNs_);
    }
}

void DetailedDRAMQueue::schedulePoll(double fromNs) {
    pollScheduled_ = true;
    evq_.schedule(fromNs, [this, fromNs]() {
        onPoll(fromNs);
    }, 1);
}

void DetailedDRAMQueue::onPoll(double nowNs) {
    // Advance the backend to the current simulation time.
    backend_.tick(nowNs);

    // Check each pending request for completion.
    auto it = pending_.begin();
    while (it != pending_.end()) {
        double completionNs = backend_.getCompletionNs(it->reqId);
        if (completionNs >= 0.0) {
            auto cb = std::move(it->onComplete);
            it = pending_.erase(it);
            cb(completionNs);
        } else {
            ++it;
        }
    }

    // Schedule the next poll if there are still pending requests.
    if (!pending_.empty()) {
        schedulePoll(nowNs + pollIntervalNs_);
    } else {
        pollScheduled_ = false;
    }
}

}  // namespace cxl
