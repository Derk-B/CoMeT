#ifndef __DRAM_PERF_MODEL_CONSTANT_H__
#define __DRAM_PERF_MODEL_CONSTANT_H__

#include "dram_perf_model.h"
#include "queue_model.h"
#include "fixed_types.h"
#include "subsecond_time.h"
#include "dram_cntlr_interface.h"

#include <vector>

class DramPerfModelConstant : public DramPerfModel
{
   private:
      QueueModel* m_queue_model;
      SubsecondTime m_dram_access_cost;
      SubsecondTime m_dram_access_cost_lowpower;
      ComponentBandwidth m_dram_bandwidth;

      SubsecondTime m_total_queueing_delay;
      SubsecondTime m_total_access_latency;
      SubsecondTime m_total_device_latency;

      // Location-dependent latency (perf_model/dram/location_latency/mode).
      // CACTI's final_latency assumes the farthest layer and bank for every access;
      // m_location_saving[bank] is how much shorter the path to that bank is.
      enum location_mode_t { LOCATION_NONE, LOCATION_TSV, LOCATION_TSV_HORIZONTAL, LOCATION_CORE_TO_BANK };
      location_mode_t m_location_mode;
      std::vector<SubsecondTime> m_location_saving;
      // core_to_bank: latency change in fs (can be negative) for m_location_adjust_fs[requester * m_num_banks + bank]
      std::vector<SInt64> m_location_adjust_fs;
      UInt32 m_num_banks;

      void initLocationLatency(core_id_t core_id);

   public:
      DramPerfModelConstant(core_id_t core_id,
            UInt32 cache_block_size);

      ~DramPerfModelConstant();

      SubsecondTime getAccessLatency(SubsecondTime pkt_time, UInt64 pkt_size, core_id_t requester, IntPtr address, DramCntlrInterface::access_t access_type, ShmemPerf *perf);
};

#endif /* __DRAM_PERF_MODEL_CONSTANT_H__ */
