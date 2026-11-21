/*
 * CXRayMem — gem5 memory object backed by the CXRay CXL library.
 * See cxray_mem.hh for the behaviour summary.
 */

#include "cxray_mem.hh"

#include <algorithm>

#include "base/logging.hh"
#include "base/trace.hh"
#include "cxl_mem.h"
#include "debug/Drain.hh"
#include "sim/core.hh"
#include "sim/cur_tick.hh"

namespace gem5
{

namespace memory
{

static std::unique_ptr<cxl::CXLMemController>
buildController(const std::string &cfg)
{
    fatal_if(cfg.empty(), "CXRayMem: set the cxray_config parameter "
             "(or the CXRAY_CONFIG environment variable)");
    inform("CXRayMem: CXRay config %s", cfg);
    try {
        return std::make_unique<cxl::CXLMemController>(
            cxl::CXLMemController::fromConfigFile(cfg));
    } catch (const std::exception &e) {
        fatal("CXRayMem: cannot build the CXRay controller: %s", e.what());
    }
}

CXRayMem::CXRayMem(const CXRayMemParams &p) :
    AbstractMemory(p),
    port(name() + ".port", *this),
    ctrl(buildController(p.cxray_config)),
    atomicLatency(Tick(p.atomic_latency_ns * sim_clock::as_int::ns)),
    retryResp(false),
    dequeueEvent([this]{ dequeue(); }, name()),
    stats(this)
{
    // Dump gem5's own statistics first so that hostSeconds does not include
    // CXRay's exit-time bookkeeping (percentile sorts, JSON), then let CXRay
    // report.  The Python atexit dump becomes a no-op (same tick).
    registerExitCallback([this] {
        statistics::dump();
        ctrl->printStats();
        ctrl->writeStatsJson();
    });
}

void
CXRayMem::init()
{
    AbstractMemory::init();
    if (port.isConnected())
        port.sendRangeChange();
}

Tick
CXRayMem::cxrayLatency(PacketPtr pkt)
{
    cxl::TraceRequest req;
    const Tick ns = sim_clock::as_int::ns;
    req.timestampNs = curTick() / ns;
    req.op          = pkt->isWrite() ? cxl::TraceOp::WRITE : cxl::TraceOp::READ;
    req.addr        = pkt->getAddr() & ~Addr(63);
    req.sizeBytes   = 64;
    req.requesterId = pkt->req->requestorId();

    cxl::RequestResult res = ctrl->processRequest(req, curTick() / double(ns));

    if (pkt->isWrite()) stats.writes++; else stats.reads++;
    stats.totalLatency += res.latencyNs;
    // `ns` is a signed Tick (gem5 counts time in picoseconds, 1000 per ns).
    // Multiplying the double by that signed value wraps to a negative latency
    // once the latency exceeds 2^63/1000 ns, and the response is then queued
    // near MaxTick. Seen on the multi-core chase: responses sat at
    // MaxTick/2 + 16 ms while the cores ticked and committed nothing.
    // Convert in double, then clamp.
    double ticks = res.latencyNs * static_cast<double>(ns) + 0.5;
    if (!(ticks > 0.0))
        return 1;
    if (ticks > static_cast<double>(MaxTick / 4))
        return MaxTick / 4;
    return static_cast<Tick>(ticks);
}

Tick
CXRayMem::recvAtomic(PacketPtr pkt)
{
    panic_if(pkt->cacheResponding(), "Should not see packets where cache "
             "is responding");
    access(pkt);
    return atomicLatency;
}

Tick
CXRayMem::recvAtomicBackdoor(PacketPtr pkt, MemBackdoorPtr &_backdoor)
{
    Tick latency = recvAtomic(pkt);
    getBackdoor(_backdoor);
    return latency;
}

void
CXRayMem::recvFunctional(PacketPtr pkt)
{
    pkt->pushLabel(name());
    functionalAccess(pkt);
    // A functional access must also see data still waiting in our queue.
    for (auto p = packetQueue.begin(); p != packetQueue.end(); ++p)
        if (pkt->trySatisfyFunctional(p->pkt))
            break;
    pkt->popLabel();
}

void
CXRayMem::recvMemBackdoorReq(const MemBackdoorReq &req,
                             MemBackdoorPtr &_backdoor)
{
    getBackdoor(_backdoor);
}

bool
CXRayMem::recvTimingReq(PacketPtr pkt)
{
    panic_if(pkt->cacheResponding(), "Should not see packets where cache "
             "is responding");
    panic_if(!(pkt->isRead() || pkt->isWrite()),
             "Should only see read and writes at memory controller, "
             "saw %s to %#llx\n", pkt->cmdString(), pkt->getAddr());

    // The packet reaches us after the crossbar header/payload delay; that
    // delay is added to the response time, CXRay sees arrivals in tick order.
    Tick receive_delay = pkt->headerDelay + pkt->payloadDelay;
    pkt->headerDelay = pkt->payloadDelay = 0;

    Tick latency = cxrayLatency(pkt);   // every request is accepted

    bool needsResponse = pkt->needsResponse();
    access(pkt);
    if (needsResponse) {
        assert(pkt->isResponse());
        Tick when_to_send = curTick() + receive_delay + latency;

        // Insert in send-time order, but never ahead of an earlier packet
        // to the same address (this memory hands out exclusive copies).
        auto i = packetQueue.end();
        while (i != packetQueue.begin()) {
            auto prev = std::prev(i);
            if (when_to_send >= prev->tick || prev->pkt->matchAddr(pkt))
                break;
            i = prev;
        }
        packetQueue.emplace(i, pkt, when_to_send);

        if (!retryResp && !dequeueEvent.scheduled())
            schedule(dequeueEvent, packetQueue.front().tick);
        else if (!retryResp)
            reschedule(dequeueEvent, packetQueue.front().tick, true);
    } else {
        pendingDelete.reset(pkt);
    }
    return true;
}

void
CXRayMem::dequeue()
{
    assert(!packetQueue.empty());
    DeferredPacket deferred_pkt = packetQueue.front();

    retryResp = !port.sendTimingResp(deferred_pkt.pkt);

    if (!retryResp) {
        packetQueue.pop_front();
        if (!packetQueue.empty()) {
            reschedule(dequeueEvent,
                       std::max(packetQueue.front().tick, curTick()), true);
        } else if (drainState() == DrainState::Draining) {
            DPRINTF(Drain, "Draining of CXRayMem complete\n");
            signalDrainDone();
        }
    }
}

void
CXRayMem::recvRespRetry()
{
    assert(retryResp);
    dequeue();
}

Port &
CXRayMem::getPort(const std::string &if_name, PortID idx)
{
    if (if_name != "port")
        return AbstractMemory::getPort(if_name, idx);
    return port;
}

DrainState
CXRayMem::drain()
{
    if (!packetQueue.empty()) {
        DPRINTF(Drain, "CXRayMem queue has requests, waiting to drain\n");
        return DrainState::Draining;
    }
    return DrainState::Drained;
}

CXRayMem::CXRayStats::CXRayStats(statistics::Group *parent)
    : statistics::Group(parent),
    ADD_STAT(reads, statistics::units::Count::get(),
             "Read requests handed to CXRay"),
    ADD_STAT(writes, statistics::units::Count::get(),
             "Write requests handed to CXRay"),
    ADD_STAT(totalLatency, statistics::units::Unspecified::get(),
             "Sum of CXRay-reported request latencies (ns)"),
    ADD_STAT(avgLatency, statistics::units::Unspecified::get(),
             "Mean CXRay-reported request latency (ns)",
             totalLatency / (reads + writes))
{
}

CXRayMem::MemoryPort::MemoryPort(const std::string &_name, CXRayMem &_memory)
    : ResponsePort(_name), mem(_memory)
{
}

AddrRangeList
CXRayMem::MemoryPort::getAddrRanges() const
{
    AddrRangeList ranges;
    ranges.push_back(mem.getAddrRange());
    return ranges;
}

Tick
CXRayMem::MemoryPort::recvAtomic(PacketPtr pkt)
{
    return mem.recvAtomic(pkt);
}

Tick
CXRayMem::MemoryPort::recvAtomicBackdoor(PacketPtr pkt,
                                         MemBackdoorPtr &_backdoor)
{
    return mem.recvAtomicBackdoor(pkt, _backdoor);
}

void
CXRayMem::MemoryPort::recvFunctional(PacketPtr pkt)
{
    mem.recvFunctional(pkt);
}

void
CXRayMem::MemoryPort::recvMemBackdoorReq(const MemBackdoorReq &req,
                                         MemBackdoorPtr &backdoor)
{
    mem.recvMemBackdoorReq(req, backdoor);
}

bool
CXRayMem::MemoryPort::recvTimingReq(PacketPtr pkt)
{
    return mem.recvTimingReq(pkt);
}

void
CXRayMem::MemoryPort::recvRespRetry()
{
    mem.recvRespRetry();
}

} // namespace memory
} // namespace gem5
