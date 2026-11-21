/*
 * CXRayMem — gem5 memory object backed by the CXRay CXL library.
 *
 * Every read or write that reaches this object (an LLC miss or writeback in a
 * classic-cache system) becomes one cxl::TraceRequest.  CXRay returns the
 * request latency; the response is sent back at curTick() + that latency.
 * The whole address range of this memory is CXL-attached memory, and CXRay's
 * host_overhead_ns is charged on top of gem5's cache hierarchy (same semantics
 * as the paper's zsim mode).
 *
 *   timing mode     : functional access on arrival, CXRay processRequest(),
 *                     response queued until CXRay's completion time.
 *   atomic mode     : functional access + fixed atomic_latency_ns, no CXRay
 *                     call (fast-forward phases leave CXRay statistics clean).
 *   functional mode : functional access only.
 *
 * At exit the CXRay statistics go to simout (printStats) and to the JSON file
 * named by output_dir/run_tag in the .cfg (writeStatsJson).
 */

#ifndef __CXRAY_MEM_HH__
#define __CXRAY_MEM_HH__

#include <list>
#include <memory>

#include "base/statistics.hh"
#include "mem/abstract_mem.hh"
#include "mem/port.hh"
#include "params/CXRayMem.hh"

namespace cxl { class CXLMemController; }

namespace gem5
{

namespace memory
{

class CXRayMem : public AbstractMemory
{
  private:
    /** A response waiting for CXRay's completion time. */
    struct DeferredPacket
    {
        const Tick tick;
        const PacketPtr pkt;
        DeferredPacket(PacketPtr _pkt, Tick _tick) : tick(_tick), pkt(_pkt) {}
    };

    class MemoryPort : public ResponsePort
    {
      private:
        CXRayMem &mem;

      public:
        MemoryPort(const std::string &_name, CXRayMem &_memory);

      protected:
        Tick recvAtomic(PacketPtr pkt) override;
        Tick recvAtomicBackdoor(PacketPtr pkt,
                                MemBackdoorPtr &_backdoor) override;
        void recvFunctional(PacketPtr pkt) override;
        void recvMemBackdoorReq(const MemBackdoorReq &req,
                                MemBackdoorPtr &backdoor) override;
        bool recvTimingReq(PacketPtr pkt) override;
        void recvRespRetry() override;
        AddrRangeList getAddrRanges() const override;
    };

    MemoryPort port;

    /** The CXRay controller built from the .cfg file. */
    std::unique_ptr<cxl::CXLMemController> ctrl;

    /** Latency returned in atomic mode (ticks). */
    const Tick atomicLatency;

    /** Responses ordered by send time; same-address order is preserved. */
    std::list<DeferredPacket> packetQueue;

    /** Waiting for the requestor to accept a response we failed to send. */
    bool retryResp;

    void dequeue();
    EventFunctionWrapper dequeueEvent;

    /** Hold a packet that needs no response until the next call. */
    std::unique_ptr<Packet> pendingDelete;

    /** Ask CXRay for the latency of this packet; returns ticks. */
    Tick cxrayLatency(PacketPtr pkt);

    struct CXRayStats : public statistics::Group
    {
        CXRayStats(statistics::Group *parent);
        statistics::Scalar reads;
        statistics::Scalar writes;
        statistics::Scalar totalLatency;
        statistics::Formula avgLatency;
    } stats;

  public:
    CXRayMem(const CXRayMemParams &p);

    DrainState drain() override;

    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;
    void init() override;

  protected:
    Tick recvAtomic(PacketPtr pkt);
    Tick recvAtomicBackdoor(PacketPtr pkt, MemBackdoorPtr &_backdoor);
    void recvFunctional(PacketPtr pkt);
    void recvMemBackdoorReq(const MemBackdoorReq &req,
                            MemBackdoorPtr &backdoor);
    bool recvTimingReq(PacketPtr pkt);
    void recvRespRetry();
};

} // namespace memory
} // namespace gem5

#endif // __CXRAY_MEM_HH__
