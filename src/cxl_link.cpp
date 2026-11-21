#include "cxl_link.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <stdexcept>
#include <functional>

namespace cxl {

CXLLink::CXLLink(const LinkConfig& cfg)
    : cfg_(cfg)
{
    bwGBps_   = computeBandwidthGBps(cfg.pcieGen, cfg.pcieLanes);
    // Bundled Port (CXL 4.0): aggregate bandwidth scales with port count.
    // Latency stays at the single-link floor; only flit throughput increases.
    if (cfg.bundledPortEnable && cfg.bundledPortNumPorts > 1) {
        bwGBps_ *= static_cast<double>(cfg.bundledPortNumPorts);
    }
    flitTxNs_ = static_cast<double>(cfg.flitSizeBytes) / bwGBps_;
}

double CXLLink::computeBandwidthGBps(int gen, int lanes) {
    double speedGTs, efficiency;
    if (gen == 7) {
        speedGTs   = 128.0;
        efficiency = 0.85;    // PAM4 ~15% overhead (same encoding as Gen6)
    } else if (gen == 6) {
        speedGTs   = 64.0;
        efficiency = 0.85;    // PAM4 ~15% overhead
    } else if (gen == 5) {
        speedGTs   = 32.0;
        efficiency = 0.9846;  // 128b/130b encoding
    } else if (gen == 4) {
        speedGTs   = 16.0;
        efficiency = 0.9846;
    } else {
        throw std::invalid_argument("Unsupported PCIe generation: " + std::to_string(gen));
    }
    // lanes × GT/s × efficiency / 8 bits-per-byte = GB/s
    return (lanes * speedGTs * efficiency) / 8.0;
}

double CXLLink::enqueueRequest(double arrivalNs, uint32_t sizeBytes,
                               double& creditStallNsOut, bool isWrite,
                               VCType vc) {
    int numFlits = static_cast<int>(
        std::ceil(static_cast<double>(sizeBytes) / cfg_.flitPayloadBytes));

    creditStallNsOut = 0.0;

    // ── Per-VC time-aware sliding-window credit stall ─────────────────────
    // Each VC has an independent credit window.  A stall on VC0 (CXL.io) does
    // NOT delay VC1 (CXL.mem) or VC2 (CXL.cache) — HOL blocking prevention.
    //
    // The credit cap for CXL.mem uses the legacy per-FLIT-type pools
    // (m2sRwd / m2sReq) to maintain behavioral equivalence for existing callers.
    // For CXL.io and CXL.cache the per-VC txCredits cap is used instead.
    {
        VCState& vs = vcState_[static_cast<int>(vc)];

        // Determine the effective credit cap for this VC.
        int cap;
        std::deque<double>* creditDeque = &vs.inflightReturnNs;
        if (vc == VCType::CXL_MEM) {
            // CXL.mem has independent request-without-data and request-with-data
            // pools. Keep separate windows so reads cannot consume write credits,
            // and writes cannot consume read credits.
            cap = isWrite ? cfg_.credits.m2sRwd : cfg_.credits.m2sReq;
            creditDeque = isWrite ? &inflightM2sRwdReturnNs_ : &inflightM2sReqReturnNs_;
        } else {
            cap = cfg_.vcConfig(vc).txCredits;
        }

        // cap = 0 means "credit limiting disabled" (Python/paper convention).
        // Skip all credit accounting; creditStallNsOut remains 0.
        int effectiveCap = std::min(cap, cfg_.maxOutstanding);
        if (effectiveCap > 0) {
            // Drain credits that have returned by arrivalNs.
            while (!creditDeque->empty() && creditDeque->front() <= arrivalNs) {
                creditDeque->pop_front();
            }

            int currentInflight = static_cast<int>(creditDeque->size());
            if (currentInflight >= effectiveCap) {
                // Window full: stall until the oldest outstanding credit returns.
                double oldestReturn = creditDeque->front();
                double stall = std::max(0.0, oldestReturn - arrivalNs);
                // With multiple slots over the limit, add one credit-spacing per extra slot.
                int overshoot = currentInflight - effectiveCap + 1;
                double extraStall = (overshoot > 1)
                    ? (overshoot - 1) * (cfg_.estimatedRttNs / static_cast<double>(effectiveCap))
                    : 0.0;
                stall += extraStall;
                creditStallNsOut      = stall;
                stats_.creditStallNs += stall;

                vs.totalStallNs                      += stall;
                stats_.vcStallNs[static_cast<int>(vc)] += stall;
            }

            // Register this request's credit return time.
            double returnNs = arrivalNs + creditStallNsOut + cfg_.estimatedRttNs;
            creditDeque->push_back(returnNs);
        }
    }

    // ── BW serialisation on the shared physical lane ─────────────────────
    // The physical lane is SHARED across all VCs.  Two flits from different VCs
    // cannot be transmitted simultaneously; we serialise against the global
    // lastReqFlitEndNs_ regardless of which VC is enqueuing.
    double startNs    = std::max(arrivalNs + creditStallNsOut, lastReqFlitEndNs_);
    double txDuration = numFlits * flitTxNs_;
    lastReqFlitEndNs_ = startNs + txDuration;

    // Update per-VC serialization visibility.
    vcState_[static_cast<int>(vc)].lastFlitEndNs = lastReqFlitEndNs_;

    stats_.totalFlitsTx   += numFlits;
    stats_.totalBytesTx   += sizeBytes;   // payload bytes
    stats_.m2sWireBytesTx += static_cast<uint64_t>(numFlits) * cfg_.flitSizeBytes;

    // Legacy counters (kept for enqueueResponse credit-free logic)
    if (vc == VCType::CXL_MEM) {
        int& pool = isWrite ? outstandingM2sRwd_ : outstandingM2sReq_;
        outstanding_++;
        pool++;
        outstandingTypes_.push_back(isWrite);
    }

#ifndef NDEBUG
    // Invariant: inflightReturnNs_ deque must remain sorted (earliest first).
    // If violated, the credit drain logic is broken.
    auto& dbgDq = (vc == VCType::CXL_MEM)
        ? (isWrite ? inflightM2sRwdReturnNs_ : inflightM2sReqReturnNs_)
        : vcState_[static_cast<int>(vc)].inflightReturnNs;
    if (dbgDq.size() >= 2) {
        auto& dq = dbgDq;
        assert(dq.back() >= dq.front() &&
               "CXLLink credit deque ordering violated");
    }
#endif

    // ── VC arbitration overhead ───────────────────────────────────────────
    // The VC-select mux adds a small pipeline stage at the TX scheduler.
    // If vcArbitrationNs > 0 it overrides the per-VC value (backward compat).
    double vcNs = (cfg_.vcArbitrationNs > 0.0)
        ? cfg_.vcArbitrationNs
        : cfg_.vcConfig(vc).arbNs;

    // ── FEC decode latency (Gen 6 PAM4 RS-FEC) ───────────────────────────
    // The device-side FEC decoder must process the received FLIT before the
    // payload is visible to the port logic.  Added on the downstream path.
    double fecNs = cfg_.fecEnable ? cfg_.fecDecodeLatencyNs : 0.0;

    // ── Link-level replay / retry ─────────────────────────────────────────
    // Model rare uncorrectable errors that trigger LTSSM-level retransmission.
    // P(retry) per FLIT is ~1e-10 at nominal signal margin with RS-FEC.
    // When it fires, the sender must re-send from the replay buffer → 1 RTT
    // of additional latency.
    double retryNs = 0.0;
    if (cfg_.retryEnable && cfg_.retryProbPerFlit > 0.0) {
        double p = cfg_.retryProbPerFlit * static_cast<double>(numFlits);
        if (nextUniform() < p) {
            retryNs = cfg_.retryLatencyNs;
            stats_.creditStallNs += retryNs;  // lump into stall bucket for now
        }
    }

    // Arrival at device = flit-TX-done + VC arb + link propagation + FEC decode + retry
    // lastReqFlitEndNs_ is startNs + txDuration; the last bit of the request flit
    // train is not on the wire until then, so propagation starts at that point.
    return lastReqFlitEndNs_ + vcNs + cfg_.effectiveLinkLatencyNs() + fecNs + retryNs;
}

double CXLLink::enqueueResponse(double deviceDoneNs, uint32_t sizeBytes,
                                bool hasData, VCType vc, double extraReturnNs) {
    int numFlits = static_cast<int>(
        std::ceil(static_cast<double>(sizeBytes) / cfg_.flitPayloadBytes));
    double txDuration = numFlits * flitTxNs_;
    double fecNs = cfg_.fecEnable ? cfg_.fecDecodeLatencyNs : 0.0;

    double startNs = std::max(deviceDoneNs, lastRespFlitEndNs_);

    // S2M credit enforcement (CXL 3.1 §4.2.3).
    // S2M_DRS (hasData=true): one credit per read completion (64-byte cacheline).
    // S2M_NDR (hasData=false): one credit per write ACK (no data payload).
    // Credits are consumed when the device transmits the response and returned when
    // the host finishes processing it (= response TX end + return link + FEC + host overhead).
    // limit == 0 means "disabled" (same convention as M2S and Python CreditPool).
    {
        int     limit   = hasData ? cfg_.credits.s2mDrs        : cfg_.credits.s2mNdr;

        if (limit > 0) {
            int&    pool    = hasData ? outstandingS2mDrs_         : outstandingS2mNdr_;
            auto&   dq      = hasData ? inflightS2mDrsReturnNs_    : inflightS2mNdrReturnNs_;

            // Drain credits that have already returned (host processed them by startNs).
            while (!dq.empty() && dq.front() <= startNs) {
                dq.pop_front();
                pool--;
            }

            // Stall if pool exhausted: wait for the oldest inflight response to clear.
            if (pool >= limit && !dq.empty()) {
                double waitUntil = dq.front();
                if (waitUntil > startNs) {
                    stats_.s2mStallNs += (waitUntil - startNs);
                    startNs = waitUntil;
                }
                dq.pop_front();
                pool--;
            }

            // Consume one S2M credit; return time = host-visible completion
            // (response TX + return-link propagation + FEC + host-side IDE + host overhead).
            // extraReturnNs carries the host-side portion so C++ matches Python's release point.
            pool++;
            double creditReturnNs = startNs + txDuration + cfg_.returnLinkLatencyNs() + fecNs
                                    + extraReturnNs;
            dq.push_back(creditReturnNs);
        }

        if (hasData) stats_.s2mDrsConsumed++;
        else         stats_.s2mNdrConsumed++;
    }

    lastRespFlitEndNs_ = startNs + txDuration;
    // Track S2M wire bytes (both read completions and write ACKs occupy full flits on wire).
    stats_.s2mWireBytesTx += static_cast<uint64_t>(numFlits) * cfg_.flitSizeBytes;

    // Release the M2S credit that was consumed for this request (synchronous path).
    if (vc == VCType::CXL_MEM) {
        bool wasWrite = false;
        if (!outstandingTypes_.empty()) {
            wasWrite = outstandingTypes_.front();
            outstandingTypes_.pop_front();
        }
        int& pool = wasWrite ? outstandingM2sRwd_ : outstandingM2sReq_;
        if (pool > 0) pool--;
        if (outstanding_ > 0) outstanding_--;
        if (wasWrite) stats_.m2sRwdReleased++;
        else          stats_.m2sReqReleased++;

#ifndef NDEBUG
        assert(outstanding_ >= 0 && "CXLLink: outstanding_ went negative");
        assert(outstandingM2sReq_ >= 0 && "CXLLink: M2S_REQ credits went negative");
        assert(outstandingM2sRwd_ >= 0 && "CXLLink: M2S_RWD credits went negative");
        assert(outstandingS2mDrs_ >= 0 && "CXLLink: S2M_DRS credits went negative");
        assert(outstandingS2mNdr_ >= 0 && "CXLLink: S2M_NDR credits went negative");
#endif
    }

    return lastRespFlitEndNs_ + cfg_.returnLinkLatencyNs() + fecNs;
}

double CXLLink::nextUniform() const {
    // xorshift32 PRNG — fast, no stdlib dependency, adequate for rare-event sampling
    rngState_ ^= rngState_ << 13;
    rngState_ ^= rngState_ >> 17;
    rngState_ ^= rngState_ << 5;
    return static_cast<double>(rngState_) / static_cast<double>(0xFFFFFFFFu);
}

double CXLLink::upstreamUtilization(double totalSimNs) const {
    // M2S (downstream: host → device) utilization only.
    // Matches Python's _fwd_link.utilization() / utilization_aggregate.
    return m2sUtilization(totalSimNs);
}

double CXLLink::m2sUtilization(double totalSimNs) const {
    if (totalSimNs <= 0 || bwGBps_ <= 0) return 0.0;
    return std::min(1.0, (static_cast<double>(stats_.m2sWireBytesTx) / bwGBps_) / totalSimNs);
}

double CXLLink::s2mUtilization(double totalSimNs) const {
    if (totalSimNs <= 0 || bwGBps_ <= 0) return 0.0;
    return std::min(1.0, (static_cast<double>(stats_.s2mWireBytesTx) / bwGBps_) / totalSimNs);
}

double CXLLink::aggregateUtilization(double totalSimNs) const {
    if (totalSimNs <= 0 || bwGBps_ <= 0) return 0.0;
    // Full-duplex: each direction has bwGBps_ capacity; total wire capacity = 2 × bwGBps_.
    double wireBytes = static_cast<double>(stats_.m2sWireBytesTx + stats_.s2mWireBytesTx);
    return std::min(1.0, (wireBytes / (2.0 * bwGBps_)) / totalSimNs);
}

LatencyBreakdown CXLLink::buildBreakdown(double arrivalNs,
                                          double deviceArrivalNs,
                                          double deviceDoneNs,
                                          double hostDoneNs,
                                          double dramNs,
                                          double rasNs,
                                          double creditStallNs) const {
    LatencyBreakdown bd;
    bd.creditStallNs = creditStallNs;
    bd.hostPortNs    = cfg_.hostPortLatencyNs;
    bd.linkPhysNs    = cfg_.linkPhysLatencyNs;
    bd.devicePortNs  = cfg_.devicePortLatencyNs;
    bd.fecNs         = cfg_.fecEnable ? (cfg_.fecDecodeLatencyNs * 2.0) : 0.0; // round-trip
    bd.rasNs         = rasNs;
    bd.dramNs        = dramNs;

    // Return path: device done → host done (minus any accumulated stalls)
    double returnWindow = hostDoneNs - deviceDoneNs;
    bd.returnNs = returnWindow > 0 ? returnWindow : cfg_.returnLinkLatencyNs();

    (void)arrivalNs;
    (void)deviceArrivalNs;

    return bd;
}

double CXLLink::enqueueRequestDetailed(double arrivalNs, uint32_t sizeBytes,
                                        double& creditStallNsOut, bool /*isWrite*/,
                                        SimEventQueue& evq,
                                        CreditTracker& credits,
                                        FlitSerializer& lane,
                                        VCType vc) {
    int numFlits = static_cast<int>(
        std::ceil(static_cast<double>(sizeBytes) / cfg_.flitPayloadBytes));

    double dispatchNs    = -1.0;
    creditStallNsOut     = 0.0;

    credits.acquire(arrivalNs, [&](double grantNs) {
        creditStallNsOut = std::max(0.0, grantNs - arrivalNs);
        stats_.creditStallNs += creditStallNsOut;
        vcState_[static_cast<int>(vc)].totalStallNs += creditStallNsOut;

        lane.scheduleTx(grantNs, numFlits, [&](double txDoneNs) {
            double vcNs  = cfg_.vcArbitrationNs;
            double fecNs = cfg_.fecEnable ? cfg_.fecDecodeLatencyNs : 0.0;
            double retryNs = 0.0;
            if (cfg_.retryEnable && cfg_.retryProbPerFlit > 0.0) {
                double p = cfg_.retryProbPerFlit * numFlits;
                if (nextUniform() < p) {
                    retryNs = cfg_.retryLatencyNs;
                    stats_.creditStallNs += retryNs;
                }
            }
            dispatchNs = txDoneNs + vcNs + cfg_.effectiveLinkLatencyNs() + fecNs + retryNs;
            stats_.totalFlitsTx += numFlits;
            stats_.totalBytesTx += sizeBytes;
        });
    });

    double deadline = arrivalNs + cfg_.estimatedRttNs + cfg_.retryLatencyNs + 100.0;
    evq.runUntil(deadline);

    if (dispatchNs < 0.0) {
        dispatchNs = arrivalNs + cfg_.effectiveLinkLatencyNs();
    }

    return dispatchNs;
}

double CXLLink::enqueueResponseDetailed(double deviceDoneNs, uint32_t sizeBytes,
                                         bool hasData,
                                         SimEventQueue& evq,
                                         FlitSerializer& respLane) {
    int numFlits = static_cast<int>(
        std::ceil(static_cast<double>(sizeBytes) / cfg_.flitPayloadBytes));

    double completionNs = -1.0;
    respLane.scheduleTx(deviceDoneNs, numFlits, [&](double txDoneNs) {
        double fecNs = cfg_.fecEnable ? cfg_.fecDecodeLatencyNs : 0.0;
        completionNs = txDoneNs + cfg_.returnLinkLatencyNs() + fecNs;
    });

    evq.runUntil(deviceDoneNs + cfg_.estimatedRttNs);

    if (completionNs < 0.0)
        completionNs = deviceDoneNs + cfg_.returnLinkLatencyNs();

    (void)hasData;

    {
        bool wasWrite = false;
        if (!outstandingTypes_.empty()) {
            wasWrite = outstandingTypes_.front();
            outstandingTypes_.pop_front();
        }
        int& pool = wasWrite ? outstandingM2sRwd_ : outstandingM2sReq_;
        if (pool > 0) pool--;
        if (outstanding_ > 0) outstanding_--;
        if (wasWrite) stats_.m2sRwdReleased++;
        else          stats_.m2sReqReleased++;

#ifndef NDEBUG
        assert(outstanding_ >= 0 && "CXLLink: outstanding_ went negative");
        assert(outstandingM2sReq_ >= 0 && "CXLLink: M2S_REQ credits went negative");
        assert(outstandingM2sRwd_ >= 0 && "CXLLink: M2S_RWD credits went negative");
#endif
    }

    return completionNs;
}

void CXLLink::addWireBytes(uint64_t m2sWireBytes, uint64_t s2mWireBytes) {
    stats_.m2sWireBytesTx += m2sWireBytes;
    stats_.s2mWireBytesTx += s2mWireBytes;
}

void CXLLink::addS2mStats(uint64_t drsConsumed, uint64_t ndrConsumed, double stallNs) {
    stats_.s2mDrsConsumed += drsConsumed;
    stats_.s2mNdrConsumed += ndrConsumed;
    stats_.s2mStallNs     += stallNs;
}

}  // namespace cxl
