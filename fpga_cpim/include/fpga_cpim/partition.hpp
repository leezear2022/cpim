#ifndef FPGA_CPIM_PARTITION_HPP_
#define FPGA_CPIM_PARTITION_HPP_

#include <cstdint>
#include <vector>

#include "fpga_cpim/model.hpp"

namespace fpga_cpim {

struct PartitionConfig {
  uint32_t num_partitions = 4;
  uint32_t max_vars_per_partition = 0;         // 0 means unlimited.
  uint32_t max_constraints_per_partition = 0;  // 0 means unlimited.
};

struct PartitionStats {
  uint32_t num_partitions = 0;
  std::vector<uint32_t> var_partition;
  std::vector<uint32_t> constraint_partition;
  std::vector<uint32_t> vars_per_partition;
  std::vector<uint32_t> constraints_per_partition;
  uint64_t local_events = 0;
  uint64_t cross_events = 0;
  double cross_event_ratio = 0.0;
  uint32_t max_partition_degree = 0;
  uint32_t high_degree_hub_count = 0;
  uint32_t max_var_degree = 0;
};

PartitionStats BuildGreedyPartition(const Model& model, PartitionConfig cfg);

}  // namespace fpga_cpim

#endif  // FPGA_CPIM_PARTITION_HPP_
