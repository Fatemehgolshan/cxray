#include "dram_perf_model_cxray.h"
#include "simulator.h"
#include "config.h"
#include "config.hpp"
#include "stats.h"
#include "shmem_perf.h"
#include "hooks_manager.h"
#include "log.h"

#include "cxl_mem.h"   // CXRay: cxl::CXLMemController, cxl::TraceRequest, cxl::RequestResult

#include <cmath>
#include <cstdio>
#include <exception>
#include <memory>
#include <mutex>
#include <string>

namespace
{
   // Process-wide CXRay state: one controller, one lock, one stats dump.
   struct CXRayShared
   {
      std::mutex lock;
      std::unique_ptr<cxl::CXLMemController> ctrl;
      UInt64 instances = 0;
      UInt64 requests = 0;
      bool stats_written = false;
   };

   CXRayShared& shared()
   {
      static CXRayShared s;
      return s;
   }

   // HOOK_SIM_END callback: CXRay statistics to stdout and to <output_dir>/<run_tag>_stats.json.
   SInt64 cxrayHookSimEnd(UInt64 /*arg*/, UInt64 /*unused*/)
   {
      CXRayShared& s = shared();
      std::lock_guard<std::mutex> guard(s.lock);
      if (s.ctrl && !s.stats_written)
      {
         s.stats_written = true;
         printf("[CXRAY] simulation end: %lu requests through CXRay, writing statistics\n",
                (unsigned long)s.requests);
         fflush(stdout);
         s.ctrl->printStats();
         s.ctrl->writeStatsJson();
         fflush(stdout);
      }
      return 0;
   }
}

DramPerfModelCXRay::DramPerfModelCXRay(core_id_t core_id, UInt32 cache_block_size, const String& suffix)
   : DramPerfModel(core_id, cache_block_size, suffix)
   , m_core_id(core_id)
   , m_total_access_latency(SubsecondTime::Zero())
   , m_total_queueing_delay(SubsecondTime::Zero())
{
   CXRayShared& s = shared();
   {
      std::lock_guard<std::mutex> guard(s.lock);
      if (!s.ctrl)
      {
         LOG_ASSERT_ERROR(Sim()->getCfg()->hasKey("perf_model/dram/cxray_config"),
                          "perf_model/dram/type=cxray requires perf_model/dram/cxray_config=<CXRay .cfg>");
         String path = Sim()->getCfg()->getString("perf_model/dram/cxray_config");
         try
         {
            s.ctrl = std::make_unique<cxl::CXLMemController>(
                        cxl::CXLMemController::fromConfigFile(std::string(path.c_str())));
         }
         catch (const std::exception& e)
         {
            LOG_PRINT_ERROR("CXRay: cannot build controller from %s: %s", path.c_str(), e.what());
         }
         const cxl::SimConfig& c = s.ctrl->config();
         printf("[CXRAY] controller from %s: CXL %s, sim_mode %s, PCIe Gen%d x%d, DRAM backend %s, host overhead %.0f ns\n",
                path.c_str(), c.cxlVersion.c_str(), c.simMode.c_str(), c.link.pcieGen, c.link.pcieLanes,
                c.device.dramBackendType.c_str(), c.hostOverheadNs);
         fflush(stdout);
         Sim()->getHooksManager()->registerHook(HookType::HOOK_SIM_END, cxrayHookSimEnd, 0);
      }
      s.instances++;
   }

   // Same metric names as Sniper's own DRAM models so sim.out's "DRAM summary" reports CXRay's latency.
   registerStatsMetric("dram" + m_suffix, core_id, "total-access-latency", &m_total_access_latency);
   registerStatsMetric("dram" + m_suffix, core_id, "total-queueing-delay", &m_total_queueing_delay);
}

DramPerfModelCXRay::~DramPerfModelCXRay()
{
   CXRayShared& s = shared();
   std::lock_guard<std::mutex> guard(s.lock);
   s.instances--;
}

SubsecondTime
DramPerfModelCXRay::getAccessLatency(SubsecondTime pkt_time, UInt64 pkt_size, core_id_t requester,
                                     IntPtr address, DramCntlrInterface::access_t access_type,
                                     ShmemPerf *perf, bool is_metadata)
{
   // Same gating as Sniper's own models: nothing while performance models are off (fast-forward,
   // warmup) or for non-application requesters.
   if ((!m_enabled) ||
         (requester >= (core_id_t) Config::getSingleton()->getApplicationCores()))
   {
      return SubsecondTime::Zero();
   }

   // Sniper keeps time in femtoseconds; CXRay takes the arrival time in nanoseconds (double).
   const double arrival_ns = static_cast<double>(pkt_time.getFS()) * 1e-6;

   cxl::TraceRequest req;
   req.timestampNs = static_cast<uint64_t>(arrival_ns);
   req.op          = (access_type == DramCntlrInterface::WRITE) ? cxl::TraceOp::WRITE : cxl::TraceOp::READ;
   req.addr        = static_cast<uint64_t>(address) & ~static_cast<uint64_t>(63);   // 64 B line
   req.sizeBytes   = static_cast<uint32_t>(pkt_size);
   req.requesterId = static_cast<uint32_t>(requester);

   cxl::RequestResult res;
   {
      CXRayShared& s = shared();
      std::lock_guard<std::mutex> guard(s.lock);
      res = s.ctrl->processRequest(req, arrival_ns);
      s.requests++;
   }

   // CXRay latencies (ns, double) back to SubsecondTime at femtosecond precision.
   SubsecondTime access_latency = SubsecondTime::FS() *
         static_cast<uint64_t>(std::llround(res.latencyNs * 1e6));
   SubsecondTime credit_stall = SubsecondTime::FS() *
         static_cast<uint64_t>(std::llround(res.creditStallNs * 1e6));

   perf->updateTime(pkt_time);
   perf->updateTime(pkt_time + credit_stall, ShmemPerf::DRAM_QUEUE);
   perf->updateTime(pkt_time + access_latency, ShmemPerf::DRAM_DEVICE);

   m_num_accesses++;
   m_total_access_latency += access_latency;
   m_total_queueing_delay += credit_stall;

   return access_latency;
}
