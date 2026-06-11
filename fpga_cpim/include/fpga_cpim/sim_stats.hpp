#ifndef FPGA_CPIM_SIM_STATS_HPP_
#define FPGA_CPIM_SIM_STATS_HPP_

#include <cstdint>
#include <vector>

namespace fpga_cpim {

struct RouterStats {
  uint64_t events_enqueued = 0;
  uint64_t events_deduped = 0;
  uint64_t events_dropped_overflow = 0;
  uint64_t max_queue_occupancy = 0;
  std::vector<uint64_t> occupancy_samples;
};

struct EngineStats {
  uint64_t probes_total = 0;
  uint64_t probes_ok = 0;
  uint64_t probes_dwo = 0;
  uint64_t probes_unknown = 0;
  uint64_t total_revise_calls = 0;
  uint64_t total_events = 0;
  uint64_t total_support_words = 0;
  uint64_t max_queue_occupancy = 0;
};

struct StorageEstimate {
  uint64_t bit_sup_bytes = 0;
  uint64_t domain_state_bytes_per_world = 0;
  uint64_t domain_state_bytes_total = 0;
  uint64_t frontier_bytes_per_world = 0;
  uint64_t router_buffer_bytes = 0;
  uint64_t bram18_estimate = 0;
  uint64_t uram288_estimate = 0;
  bool likely_onchip_fit = false;
};

double Percentile(std::vector<uint64_t> values, double p);
StorageEstimate EstimateStorage(const struct Model& model, uint32_t worlds,
                                uint32_t router_capacity);

}  // namespace fpga_cpim

#endif  // FPGA_CPIM_SIM_STATS_HPP_
