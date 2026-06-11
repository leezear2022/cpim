#include "test_util.hpp"

#include "fpga_cpim/partition.hpp"

using namespace fpga_cpim;

int main() {
  {
    SyntheticConfig cfg;
    cfg.graph = "chain";
    cfg.vars = 8;
    cfg.domain = 4;
    cfg.tightness = 0.2;
    Model model = MakeSyntheticModel(cfg);
    PartitionStats stats = BuildGreedyPartition(model, PartitionConfig{2, 4, 0});
    CHECK_EQ(stats.num_partitions, 2u);
    CHECK_EQ(stats.var_partition.size(), size_t{8});
    CHECK_EQ(stats.constraint_partition.size(), size_t{7});
    CHECK_EQ(stats.vars_per_partition[0] + stats.vars_per_partition[1], 8u);
    CHECK_EQ(stats.constraints_per_partition[0] + stats.constraints_per_partition[1],
             7u);
    CHECK_TRUE(stats.cross_event_ratio >= 0.0);
    CHECK_TRUE(stats.cross_event_ratio <= 1.0);
  }

  {
    SyntheticConfig cfg;
    cfg.graph = "hub";
    cfg.vars = 16;
    cfg.domain = 4;
    cfg.tightness = 0.2;
    Model model = MakeSyntheticModel(cfg);
    PartitionStats stats = BuildGreedyPartition(model, PartitionConfig{4, 0, 0});
    CHECK_EQ(stats.num_partitions, 4u);
    CHECK_TRUE(stats.cross_events > 0);
    CHECK_TRUE(stats.cross_event_ratio > 0.0);
    CHECK_TRUE(stats.high_degree_hub_count >= 1u);
    CHECK_EQ(stats.max_var_degree, 15u);
  }

  {
    SyntheticConfig cfg;
    cfg.graph = "chain";
    cfg.vars = 8;
    cfg.domain = 4;
    Model model = MakeSyntheticModel(cfg);
    PartitionConfig pcfg;
    pcfg.num_partitions = 4;
    pcfg.policy = PartitionPolicy::kContiguous;
    PartitionStats stats = BuildPartition(model, pcfg);
    CHECK_EQ(stats.policy, PartitionPolicy::kContiguous);
    CHECK_EQ(stats.var_partition[0], 0u);
    CHECK_EQ(stats.var_partition[1], 0u);
    CHECK_EQ(stats.var_partition[2], 1u);
    CHECK_EQ(stats.var_partition[3], 1u);
    CHECK_EQ(stats.var_partition[4], 2u);
    CHECK_EQ(stats.var_partition[5], 2u);
    CHECK_EQ(stats.var_partition[6], 3u);
    CHECK_EQ(stats.var_partition[7], 3u);
  }
  return 0;
}
