#include "fpga_cpim/partition.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "fpga_cpim/sim_stats.hpp"

namespace fpga_cpim {

namespace {

uint32_t ChooseVariablePartition(
    const std::vector<uint32_t>& vars_per_partition,
    const std::vector<uint64_t>& degree_load,
    uint32_t max_vars_per_partition) {
  uint32_t best = 0;
  for (uint32_t part = 1; part < vars_per_partition.size(); ++part) {
    const bool best_full = max_vars_per_partition != 0 &&
                           vars_per_partition[best] >= max_vars_per_partition;
    const bool part_full = max_vars_per_partition != 0 &&
                           vars_per_partition[part] >= max_vars_per_partition;
    if (best_full && !part_full) {
      best = part;
      continue;
    }
    if (part_full) {
      continue;
    }
    if (degree_load[part] < degree_load[best] ||
        (degree_load[part] == degree_load[best] &&
         vars_per_partition[part] < vars_per_partition[best])) {
      best = part;
    }
  }
  return best;
}

uint32_t ChooseConstraintPartition(
    uint32_t px, uint32_t py,
    const std::vector<uint32_t>& constraints_per_partition,
    uint32_t max_constraints_per_partition) {
  if (px == py) {
    return px;
  }
  const bool x_full = max_constraints_per_partition != 0 &&
                      constraints_per_partition[px] >= max_constraints_per_partition;
  const bool y_full = max_constraints_per_partition != 0 &&
                      constraints_per_partition[py] >= max_constraints_per_partition;
  if (x_full && !y_full) {
    return py;
  }
  if (y_full && !x_full) {
    return px;
  }
  return constraints_per_partition[px] <= constraints_per_partition[py] ? px : py;
}

uint32_t FirstNonFullPartition(const std::vector<uint32_t>& vars_per_partition,
                               uint32_t start,
                               uint32_t max_vars_per_partition) {
  if (max_vars_per_partition == 0) {
    return start;
  }
  for (uint32_t offset = 0; offset < vars_per_partition.size(); ++offset) {
    const uint32_t part = (start + offset) % vars_per_partition.size();
    if (vars_per_partition[part] < max_vars_per_partition) {
      return part;
    }
  }
  return start;
}

void AssignDegreePolicy(const Model& model, const PartitionConfig& cfg,
                        PartitionStats* stats) {
  std::vector<uint32_t> order(model.num_vars);
  std::iota(order.begin(), order.end(), 0);
  std::sort(order.begin(), order.end(), [&](uint32_t lhs, uint32_t rhs) {
    const size_t lhs_degree = model.subscription[lhs].size();
    const size_t rhs_degree = model.subscription[rhs].size();
    if (lhs_degree != rhs_degree) {
      return lhs_degree > rhs_degree;
    }
    return lhs < rhs;
  });

  std::vector<uint64_t> degree_load(cfg.num_partitions, 0);
  for (VarId var : order) {
    const uint32_t part = ChooseVariablePartition(
        stats->vars_per_partition, degree_load, cfg.max_vars_per_partition);
    stats->var_partition[var] = part;
    ++stats->vars_per_partition[part];
    degree_load[part] += model.subscription[var].size();
  }
}

void AssignContiguousPolicy(const Model& model, const PartitionConfig& cfg,
                            PartitionStats* stats) {
  const uint32_t chunk =
      std::max<uint32_t>(1, (model.num_vars + cfg.num_partitions - 1) /
                                cfg.num_partitions);
  for (VarId var = 0; var < model.num_vars; ++var) {
    uint32_t part = std::min<uint32_t>(var / chunk, cfg.num_partitions - 1);
    part = FirstNonFullPartition(stats->vars_per_partition, part,
                                 cfg.max_vars_per_partition);
    stats->var_partition[var] = part;
    ++stats->vars_per_partition[part];
  }
}

}  // namespace

const char* PartitionPolicyName(PartitionPolicy policy) {
  switch (policy) {
    case PartitionPolicy::kDegree:
      return "degree";
    case PartitionPolicy::kContiguous:
      return "contiguous";
  }
  return "degree";
}

PartitionStats BuildPartition(const Model& model, PartitionConfig cfg) {
  if (cfg.num_partitions == 0) {
    cfg.num_partitions = 1;
  }

  PartitionStats stats;
  stats.num_partitions = cfg.num_partitions;
  stats.policy = cfg.policy;
  stats.var_partition.assign(model.num_vars, 0);
  stats.constraint_partition.assign(model.num_constraints, 0);
  stats.vars_per_partition.assign(cfg.num_partitions, 0);
  stats.constraints_per_partition.assign(cfg.num_partitions, 0);

  std::vector<uint64_t> degrees;
  degrees.reserve(model.num_vars);
  for (VarId var = 0; var < model.num_vars; ++var) {
    degrees.push_back(model.subscription[var].size());
    stats.max_var_degree =
        std::max<uint32_t>(stats.max_var_degree, model.subscription[var].size());
  }

  if (cfg.policy == PartitionPolicy::kContiguous) {
    AssignContiguousPolicy(model, cfg, &stats);
  } else {
    AssignDegreePolicy(model, cfg, &stats);
  }

  for (Cid cid = 0; cid < model.num_constraints; ++cid) {
    const auto [x, y] = model.scopes[cid];
    const uint32_t px = stats.var_partition[x];
    const uint32_t py = stats.var_partition[y];
    const uint32_t owner = ChooseConstraintPartition(
        px, py, stats.constraints_per_partition,
        cfg.max_constraints_per_partition);
    stats.constraint_partition[cid] = owner;
    ++stats.constraints_per_partition[owner];

    stats.local_events += px == owner ? 1 : 0;
    stats.local_events += py == owner ? 1 : 0;
    stats.cross_events += px == owner ? 0 : 1;
    stats.cross_events += py == owner ? 0 : 1;
  }

  for (uint32_t load : stats.constraints_per_partition) {
    stats.max_partition_degree = std::max(stats.max_partition_degree, load);
  }
  const uint64_t total_events = stats.local_events + stats.cross_events;
  stats.cross_event_ratio =
      total_events == 0 ? 0.0 : static_cast<double>(stats.cross_events) / total_events;

  const double p50 = Percentile(degrees, 50);
  const uint32_t hub_threshold =
      static_cast<uint32_t>(std::max(4.0, std::ceil(p50 * 2.0)));
  for (uint64_t degree : degrees) {
    if (degree >= hub_threshold) {
      ++stats.high_degree_hub_count;
    }
  }
  return stats;
}

PartitionStats BuildGreedyPartition(const Model& model, PartitionConfig cfg) {
  cfg.policy = PartitionPolicy::kDegree;
  return BuildPartition(model, cfg);
}

}  // namespace fpga_cpim
