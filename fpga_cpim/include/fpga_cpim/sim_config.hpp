#ifndef FPGA_CPIM_SIM_CONFIG_HPP_
#define FPGA_CPIM_SIM_CONFIG_HPP_

#include <cstdint>

namespace fpga_cpim {

enum class OverflowPolicy {
  kReturnUnknown,
  kDropButMarkUnknown
};

struct RouterConfig {
  uint32_t max_pending_events_per_world = 4096;
  uint32_t max_pending_per_cid = 4;
  bool dedup = true;
  OverflowPolicy overflow_policy = OverflowPolicy::kReturnUnknown;
};

struct ReviseTileConfig {
  uint32_t max_values_per_revise = 1u << 20;
  uint32_t max_words_per_revise = 1u << 24;
};

}  // namespace fpga_cpim

#endif  // FPGA_CPIM_SIM_CONFIG_HPP_
