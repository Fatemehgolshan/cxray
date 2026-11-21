#include "cxl_switch.h"
#include <algorithm>
#include <stdexcept>

namespace cxl {

CXLSwitch::CXLSwitch(const SwitchConfig& cfg) : cfg_(cfg) {}

void CXLSwitch::addHBREntry(uint64_t baseAddr, uint64_t sizeBytes, uint8_t dspPort) {
    hbrTable_.push_back({baseAddr, sizeBytes, dspPort});
}

void CXLSwitch::addPBREntry(uint8_t ingressPort, uint8_t egressPort) {
    pbrTable_.push_back({ingressPort, egressPort});
}

double CXLSwitch::route(uint64_t addr, uint8_t ingressPort, uint8_t& egressPortOut,
                         uint8_t vc, double arrivalNs) const {
    // ── Egress port lookup ────────────────────────────────────────────────
    egressPortOut = 0;
    if (cfg_.mode == RoutingMode::HBR) {
        for (const auto& e : hbrTable_) {
            if (addr >= e.baseAddr && addr < e.baseAddr + e.sizeBytes) {
                egressPortOut = e.dspPort;
                break;
            }
        }
    } else {
        for (const auto& e : pbrTable_) {
            if (e.ingressPort == ingressPort) {
                egressPortOut = e.egressPort;
                break;
            }
        }
    }

    // ── Base switch latency (cut-through + arbitration × hops) ───────────
    double baseNs = cfg_.latencyNs * static_cast<double>(cfg_.numHops);

    // ── Per-port ingress queuing (M/D/1 with time-based state) ────────────
    double queueNs = 0.0;
    if (cfg_.queueCapacityFlits > 0 && linkBwGBps_ > 0.0) {
        // Service time for one FLIT at this link rate
        double flitBytes = static_cast<double>(flitSizeBytes_);
        // Better: use per-port BW
        double effectiveBw = (cfg_.ingressBwGBps > 0.0) ? cfg_.ingressBwGBps : linkBwGBps_;
        double serviceTxNs = flitBytes / effectiveBw;

        // When arrivalNs > 0: use time-based queue state
        if (arrivalNs > 0.0) {
            auto vcKey = static_cast<uint8_t>(vc % 3);
            // Get or create per-port per-VC busy-until
            auto& vcBusy = vcPortBusyUntilNs_[ingressPort];
            double& busyUntil = vcBusy[vcKey];

            // Head-of-queue: this request starts service at max(arrival, busyUntil)
            double serviceStartNs = std::max(arrivalNs, busyUntil);
            double serviceEndNs   = serviceStartNs + serviceTxNs;

            // Queuing delay = time spent waiting before service starts
            queueNs = serviceStartNs - arrivalNs;

            // Check queue capacity: if we've been waiting too long, cap at capacity
            double maxWaitNs = cfg_.queueCapacityFlits * serviceTxNs;
            if (queueNs > maxWaitNs) {
                // Queue full: model as tail-drop (request still gets sent
                // but with full queue delay — in a real switch it would be dropped)
                queueNs = maxWaitNs;
                serviceEndNs = arrivalNs + queueNs + serviceTxNs;
            }

            busyUntil = serviceEndNs;
        } else {
            // Analytical mode (arrivalNs == 0): use legacy occupancy approximation
            auto& busy = portBusyUntilNs_[ingressPort];
            // If port was recently used, queue delay = one service time
            // (conservatively assumes back-to-back arrivals)
            if (busy > 0.0) {
                queueNs = serviceTxNs;  // one FLIT of queuing
            }
            busy = serviceTxNs;  // mark port busy for one service time
        }

        if (queueNs > 0.0) queuedCount_++;
        totalQueueNs_ += queueNs;
    }

    totalRouted_++;
    return baseNs + queueNs;
}

}  // namespace cxl
