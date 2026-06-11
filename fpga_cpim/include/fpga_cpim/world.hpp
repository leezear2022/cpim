#ifndef FPGA_CPIM_WORLD_HPP_
#define FPGA_CPIM_WORLD_HPP_

#include <cstdint>
#include <vector>

#include "fpga_cpim/bitset.hpp"
#include "fpga_cpim/model.hpp"

namespace fpga_cpim {

enum class WorldStatus {
  kOK,
  kDWO,
  kUNKNOWN,
  kBudgetExceeded,
  kOverflow
};

struct ProbeTask {
  WorldId world = 0;
  VarId var = 0;
  Value value = 0;
  bool nsac_enabled = false;
  uint32_t nsac_radius = 0;
};

struct WorldResult {
  WorldId world = 0;
  VarId var = 0;
  Value value = 0;
  WorldStatus status = WorldStatus::kOK;
  uint64_t revise_calls = 0;
  uint64_t events = 0;
  uint64_t support_words_touched = 0;
  uint64_t epochs = 0;
  uint64_t deleted_values = 0;
  double queue_occupancy_p50 = 0.0;
  double queue_occupancy_p95 = 0.0;
  uint64_t queue_occupancy_max = 0;
  uint64_t router_events_enqueued = 0;
  uint64_t router_events_deduped = 0;
  uint64_t router_events_dropped_overflow = 0;
};

class WorldManager {
 public:
  explicit WorldManager(const Model& model, uint32_t num_worlds);

  void InitProbe(WorldId world, const std::vector<DomainMask>& base_domains,
                 VarId singleton_var, Value singleton_value);
  void MarkUnknown(WorldId world);
  void MarkDWO(WorldId world);

  const std::vector<DomainMask>& Domains(WorldId world) const;
  WorldStatus Status(WorldId world) const;

 private:
  const Model& model_;
  std::vector<std::vector<DomainMask>> domains_;
  std::vector<WorldStatus> statuses_;
};

const char* WorldStatusName(WorldStatus status);

}  // namespace fpga_cpim

#endif  // FPGA_CPIM_WORLD_HPP_
