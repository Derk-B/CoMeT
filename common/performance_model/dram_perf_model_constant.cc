#include "dram_perf_model_constant.h"
#include "simulator.h"
#include "config.h"
#include "config.hpp"
#include "stats.h"
#include "shmem_perf.h"
#include "dram_trace_collect.h" // Used to calculate the bank number from an address.

#include <cmath>

#define LOW_POWER 0
#define NORMAL_POWER 1

/*
   This file has been extended to support a low power access latency,
   which will be used when memory DTM is used.
*/

DramPerfModelConstant::DramPerfModelConstant(core_id_t core_id,
      UInt32 cache_block_size):
   DramPerfModel(core_id, cache_block_size),
   m_queue_model(NULL),
   m_dram_bandwidth(8 * Sim()->getCfg()->getFloat("perf_model/dram/per_controller_bandwidth")), // Convert bytes to bits
   m_total_queueing_delay(SubsecondTime::Zero()),
   m_total_access_latency(SubsecondTime::Zero()),
   m_total_device_latency(SubsecondTime::Zero()),
   m_location_mode(LOCATION_NONE),
   m_num_banks(0)
{


   if(Sim()->getCfg()->getBool("perf_model/dram/cacti/enable_cacti"))  {
      m_dram_access_cost = SubsecondTime::FS() * static_cast<uint64_t>(TimeConverter<float>::NStoFS(Sim()->getCfg()->getFloat("perf_model/dram/cacti/final_latency")));
   }
   else {
      m_dram_access_cost = SubsecondTime::FS() * static_cast<uint64_t>(TimeConverter<float>::NStoFS(Sim()->getCfg()->getFloat("perf_model/dram/latency")));
   }

   // Read the low power access cost.
   m_dram_access_cost_lowpower  = SubsecondTime::FS() * static_cast<uint64_t>(TimeConverter<float>::NStoFS(Sim()->getCfg()->getFloat("perf_model/dram/latency_lowpower"))); // Operate in fs for higher precision before converting to uint64_t/SubsecondTime

   if (Sim()->getCfg()->getBool("perf_model/dram/queue_model/enabled"))
   {
      m_queue_model = QueueModel::create("dram-queue", core_id, Sim()->getCfg()->getString("perf_model/dram/queue_model/type"),
                                         m_dram_bandwidth.getRoundedLatency(8 * cache_block_size)); // bytes to bits
   }

   registerStatsMetric("dram", core_id, "total-access-latency", &m_total_access_latency);
   registerStatsMetric("dram", core_id, "total-queueing-delay", &m_total_queueing_delay);
   registerStatsMetric("dram", core_id, "total-device-latency", &m_total_device_latency);

   initLocationLatency(core_id);
}

static SubsecondTime nsToSubsecondTime(double ns)
{
   return SubsecondTime::FS() * static_cast<uint64_t>(TimeConverter<double>::NStoFS(ns));
}

/*
   Location-dependent latency, based on the CACTI-3DD delays that are already part of final_latency:
   - final_latency = t_RC + TSV_latency + burst, where t_RC contains the TSV delay of the full stack once
     (command up) and TSV_latency adds it once more (data down). So 2 * TSV_latency is the vertical delay
     to the farthest layer. A layer that is n of banks_in_z TSV crossings away gets n / banks_in_z of it.
   - t_RC also contains the row address bus from the die centre to the farthest bank: the center stripe
     (x direction) plus the bank bus (y direction). CACTI uses repeated wires, whose delay is linear in
     length, so a bank gets each part scaled by its distance from the die centre relative to the farthest bank.
   The farthest bank keeps exactly final_latency, so mode = none, tsv and tsv_horizontal differ only in how
   much of CACTI's worst-case path is replaced by the path to the bank that is actually accessed.

   core_to_bank (type_of_stack = 3D only, cores on top of the memory): the path starts at the requesting core
   instead of the die centre. The signal goes down through the TSVs below the core (same vertical term as tsv)
   and then horizontally from the core's position to the bank. The core grid and bank grid cover the same die
   area. The horizontal delay per bank pitch is CACTI's worst-case wire delay divided by the number of bank
   pitches from the die centre to the farthest bank, so a core in a corner accessing the opposite corner is
   slower than CACTI's worst case, and a core accessing the bank below it is faster.
*/
void DramPerfModelConstant::initLocationLatency(core_id_t core_id)
{
   String mode = Sim()->getCfg()->getString("perf_model/dram/location_latency/mode");
   if (mode == "none")
      return;
   else if (mode == "tsv")
      m_location_mode = LOCATION_TSV;
   else if (mode == "tsv_horizontal")
      m_location_mode = LOCATION_TSV_HORIZONTAL;
   else if (mode == "core_to_bank")
      m_location_mode = LOCATION_CORE_TO_BANK;
   else
      LOG_PRINT_ERROR("Unknown perf_model/dram/location_latency/mode '%s' (valid: none, tsv, tsv_horizontal, core_to_bank)", mode.c_str());

   LOG_ASSERT_ERROR(Sim()->getCfg()->getBool("perf_model/dram/cacti/enable_cacti"),
                    "perf_model/dram/location_latency/mode = %s requires CACTI (perf_model/dram/cacti/enable_cacti)", mode.c_str());

   String stack = Sim()->getCfg()->getStringArray("memory/type_of_stack", core_id);
   LOG_ASSERT_ERROR(stack == "3Dmem" || stack == "2.5D" || stack == "3D",
                    "perf_model/dram/location_latency/mode = %s requires a stacked memory (type_of_stack is %s)", mode.c_str(), stack.c_str());

   UInt32 num_banks = Sim()->getCfg()->getInt("memory/num_banks");
   UInt32 banks_in_x = Sim()->getCfg()->getInt("memory/banks_in_x");
   UInt32 banks_in_y = Sim()->getCfg()->getInt("memory/banks_in_y");
   UInt32 banks_in_z = Sim()->getCfg()->getInt("memory/banks_in_z");
   UInt32 banks_per_layer = banks_in_x * banks_in_y;
   LOG_ASSERT_ERROR(banks_per_layer * banks_in_z == num_banks,
                    "memory/num_banks (%u) != banks_in_x * banks_in_y * banks_in_z (%u)", num_banks, banks_per_layer * banks_in_z);

   double tsv_worst = 2 * Sim()->getCfg()->getFloat("perf_model/dram/cacti/TSV_latency");
   double stripe_worst = 0, bank_bus_worst = 0;
   if (m_location_mode == LOCATION_TSV_HORIZONTAL || m_location_mode == LOCATION_CORE_TO_BANK)
   {
      LOG_ASSERT_ERROR(Sim()->getCfg()->hasKey("perf_model/dram/cacti/horizontal_center_stripe_delay"),
                       "CACTI output has no horizontal wire delays; rebuild CACTI (make -C cacti)");
      stripe_worst = Sim()->getCfg()->getFloat("perf_model/dram/cacti/horizontal_center_stripe_delay");
      bank_bus_worst = Sim()->getCfg()->getFloat("perf_model/dram/cacti/horizontal_bank_bus_delay");
   }

   // Distance of bank i (0..n-1) from the die centre, relative to the farthest bank. With a single bank
   // in a direction, every access uses the full CACTI wire.
   auto relativeDistance = [](UInt32 i, UInt32 n) {
      return (n <= 1) ? 1.0 : fabs(i + 0.5 - n / 2.0) / ((n - 1) / 2.0);
   };

   m_location_saving.resize(num_banks);
   for (UInt32 bank = 0; bank < num_banks; ++bank)
   {
      // Same bank numbering as the thermal floorplan: bank = z * banks_in_x * banks_in_y + x * banks_in_y + y
      UInt32 layer = bank / banks_per_layer;
      UInt32 x = (bank % banks_per_layer) / banks_in_y;
      UInt32 y = bank % banks_in_y;

      // 3Dmem/2.5D: logic die below layer 0. 3D: cores on top of the highest layer.
      UInt32 crossings = (stack == "3D") ? (banks_in_z - layer) : (layer + 1);
      double saving = tsv_worst * (1.0 - double(crossings) / banks_in_z);

      if (m_location_mode == LOCATION_TSV_HORIZONTAL)
         saving += stripe_worst * (1.0 - relativeDistance(x, banks_in_x))
                 + bank_bus_worst * (1.0 - relativeDistance(y, banks_in_y));

      m_location_saving[bank] = nsToSubsecondTime(saving);
   }

   if (m_location_mode != LOCATION_CORE_TO_BANK)
      return;

   LOG_ASSERT_ERROR(stack == "3D", "perf_model/dram/location_latency/mode = core_to_bank requires type_of_stack = 3D "
                    "(cores stacked on the memory); with %s the cores are not on the stack", stack.c_str());

   UInt32 cores_in_x = Sim()->getCfg()->getInt("memory/cores_in_x");
   UInt32 cores_in_y = Sim()->getCfg()->getInt("memory/cores_in_y");
   UInt32 num_cores = Config::getSingleton()->getApplicationCores();
   LOG_ASSERT_ERROR(Sim()->getCfg()->getInt("memory/cores_in_z") == 1 && cores_in_x * cores_in_y == num_cores,
                    "core_to_bank needs one core layer with cores_in_x * cores_in_y (%u) == number of cores (%u)",
                    cores_in_x * cores_in_y, num_cores);

   // Horizontal wire delay per bank pitch: CACTI's worst case covers (n - 1) / 2 pitches from the die centre.
   double per_pitch_x = (banks_in_x > 1) ? stripe_worst / ((banks_in_x - 1) / 2.0) : 0;
   double per_pitch_y = (banks_in_y > 1) ? bank_bus_worst / ((banks_in_y - 1) / 2.0) : 0;
   double cacti_worst = tsv_worst + stripe_worst + bank_bus_worst;

   m_num_banks = num_banks;
   m_location_adjust_fs.resize(num_cores * num_banks);
   for (UInt32 core = 0; core < num_cores; ++core)
   {
      // Same core numbering as the thermal floorplan: core = x * cores_in_y + y. Core centre in bank pitches.
      double core_x = ((core / cores_in_y) + 0.5) * banks_in_x / cores_in_x - 0.5;
      double core_y = ((core % cores_in_y) + 0.5) * banks_in_y / cores_in_y - 0.5;

      for (UInt32 bank = 0; bank < num_banks; ++bank)
      {
         UInt32 layer = bank / banks_per_layer;
         UInt32 x = (bank % banks_per_layer) / banks_in_y;
         UInt32 y = bank % banks_in_y;

         double vertical = tsv_worst * double(banks_in_z - layer) / banks_in_z;
         double horizontal = per_pitch_x * fabs(x - core_x) + per_pitch_y * fabs(y - core_y);
         double adjust_ns = vertical + horizontal - cacti_worst;

         m_location_adjust_fs[core * num_banks + bank] = static_cast<SInt64>(TimeConverter<double>::NStoFS(adjust_ns));
      }
   }
}

DramPerfModelConstant::~DramPerfModelConstant()
{
   if (m_queue_model)
   {
     delete m_queue_model;
      m_queue_model = NULL;
   }
}

SubsecondTime
DramPerfModelConstant::getAccessLatency(SubsecondTime pkt_time, UInt64 pkt_size, core_id_t requester, IntPtr address, DramCntlrInterface::access_t access_type, ShmemPerf *perf)
{
   // pkt_size is in 'Bytes'
   // m_dram_bandwidth is in 'Bits per clock cycle'
   if ((!m_enabled) ||
         (requester >= (core_id_t) Config::getSingleton()->getApplicationCores()))
   {
      return SubsecondTime::Zero();
   }

   SubsecondTime processing_time = m_dram_bandwidth.getRoundedLatency(8 * pkt_size); // bytes to bits

   // Compute Queue Delay
   SubsecondTime queue_delay;
   if (m_queue_model)
   {
      queue_delay = m_queue_model->computeQueueDelay(pkt_time, processing_time, requester);
   }
   else
   {
      queue_delay = SubsecondTime::Zero();
   }

   UInt32 bank_nr = get_address_bank(address, requester);
   int bank_mode = Sim()->m_bank_modes[bank_nr];
   dram_xyz_t xyz = get_address_xyz(address, requester, bank_nr);

   SubsecondTime base_latency;

   // Distinguish between dram power modes.
   if (bank_mode == LOW_POWER) // Low power mode
   {
      base_latency = m_dram_access_cost_lowpower;
   }
   else // Normal power mode.
   {
      base_latency = m_dram_access_cost;
   }

   double xyz_factor = computeXYZLatencyFactor(xyz);
   SubsecondTime effective_latency = base_latency * xyz_factor;

   // The low-power latency is a fixed config value, not derived from CACTI, so it is not location dependent.
   if (bank_mode != LOW_POWER && m_location_mode == LOCATION_CORE_TO_BANK)
   {
      SInt64 adjust_fs = m_location_adjust_fs.at(requester * m_num_banks + bank_nr);
      if (adjust_fs >= 0)
         effective_latency += SubsecondTime::FS() * static_cast<uint64_t>(adjust_fs);
      else
         effective_latency -= SubsecondTime::FS() * static_cast<uint64_t>(-adjust_fs);
   }
   else if (m_location_mode != LOCATION_NONE && bank_mode != LOW_POWER && bank_nr < m_location_saving.size())
      effective_latency -= m_location_saving[bank_nr];

   SubsecondTime access_latency = queue_delay + processing_time + effective_latency;

   perf->updateTime(pkt_time);
   perf->updateTime(pkt_time + queue_delay, ShmemPerf::DRAM_QUEUE);
   perf->updateTime(pkt_time + queue_delay + processing_time, ShmemPerf::DRAM_BUS);
   perf->updateTime(pkt_time + queue_delay + processing_time + effective_latency, ShmemPerf::DRAM_DEVICE);

   // Update Memory Counters
   m_num_accesses ++;
   m_total_access_latency += access_latency;
   m_total_queueing_delay += queue_delay;
   m_total_device_latency += effective_latency;

   return access_latency;
}