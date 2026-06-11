#include "fpga_cpim/sim_stats.hpp"

#include <algorithm>
#include <cmath>

#include "fpga_cpim/model.hpp"

namespace fpga_cpim {

double Percentile(std::vector<uint64_t> values, double p) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const double clamped = std::max(0.0, std::min(100.0, p));
  const double rank = (clamped / 100.0) * (values.size() - 1);
  const size_t lo = static_cast<size_t>(std::floor(rank));
  const size_t hi = static_cast<size_t>(std::ceil(rank));
  if (lo == hi) {
    return static_cast<double>(values[lo]);
  }
  const double t = rank - lo;
  return static_cast<double>(values[lo]) * (1.0 - t) +
         static_cast<double>(values[hi]) * t;
}

StorageEstimate EstimateStorage(const Model& model, uint32_t worlds,
                                uint32_t router_capacity) {
  StorageEstimate estimate;
  estimate.bit_sup_bytes = model.bit_sup_words.size() * sizeof(uint32_t);
  uint64_t domain_words = 0;
  for (uint32_t size : model.domain_size) {
    domain_words += WordCountForBits(size);
  }
  estimate.domain_state_bytes_per_world = domain_words * sizeof(uint32_t);
  estimate.domain_state_bytes_total = estimate.domain_state_bytes_per_world * worlds;
  estimate.frontier_bytes_per_world =
      WordCountForBits(model.num_constraints) * sizeof(uint32_t);
  estimate.router_buffer_bytes = router_capacity * 16ull;
  const uint64_t total_onchip = estimate.bit_sup_bytes +
                                estimate.domain_state_bytes_total +
                                estimate.frontier_bytes_per_world * worlds +
                                estimate.router_buffer_bytes;
  estimate.bram18_estimate = (total_onchip + 2303) / 2304;
  estimate.uram288_estimate = (estimate.bit_sup_bytes + 36863) / 36864;
  estimate.likely_onchip_fit = total_onchip < 4ull * 1024ull * 1024ull;
  return estimate;
}

}  // namespace fpga_cpim
