#ifndef __DRAM_PERF_MODEL_CXRAY_H__
#define __DRAM_PERF_MODEL_CXRAY_H__

// DramPerfModelCXRay — Sniper DRAM performance model that hands every memory access to the
// CXRay CXL memory simulator (libcxray).  The whole simulated memory is one CXL device: each
// LLC miss becomes a cxl::TraceRequest, CXRay returns the end-to-end CXL latency (host
// overhead + link + device + DRAM), and Sniper charges it on top of its cache hierarchy.
//
// Select with   perf_model/dram/type = cxray
// Configure     perf_model/dram/cxray_config = <path to a CXRay .cfg, e.g. configs/cxl_3.1.cfg>
//
// One cxl::CXLMemController is shared by all DRAM controllers of the process (Sniper creates one
// DramPerfModel per controller) and every call is serialized through a mutex, because CXRay's
// fast mode is single-threaded and assumes arrivals in non-decreasing simulated time.  Sniper's
// barrier keeps cores within one quantum of each other (100 ns by default), so arrivals can be
// out of order by at most that much; this is a modelling caveat, not a correctness problem.
//
// At simulation end (HOOK_SIM_END) CXRay's printStats() goes to stdout (sim.stdout) and
// writeStatsJson() writes <output_dir>/<run_tag>_stats.json as set in the CXRay .cfg.

#include "dram_perf_model.h"
#include "fixed_types.h"
#include "subsecond_time.h"
#include "dram_cntlr_interface.h"

class ShmemPerf;

class DramPerfModelCXRay : public DramPerfModel
{
   private:
      core_id_t m_core_id;
      SubsecondTime m_total_access_latency;    // dram.total-access-latency (what sim.out reports)
      SubsecondTime m_total_queueing_delay;    // dram.total-queueing-delay = CXRay credit stalls

   public:
      DramPerfModelCXRay(core_id_t core_id, UInt32 cache_block_size, const String& suffix = "");
      ~DramPerfModelCXRay();

      SubsecondTime getAccessLatency(SubsecondTime pkt_time, UInt64 pkt_size, core_id_t requester,
                                     IntPtr address, DramCntlrInterface::access_t access_type,
                                     ShmemPerf *perf, bool is_metadata) override;
};

#endif /* __DRAM_PERF_MODEL_CXRAY_H__ */
