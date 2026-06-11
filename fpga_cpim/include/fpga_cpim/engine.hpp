#ifndef FPGA_CPIM_ENGINE_HPP_
#define FPGA_CPIM_ENGINE_HPP_

#include <cstdint>
#include <utility>
#include <vector>

#include "fpga_cpim/model.hpp"
#include "fpga_cpim/sim_config.hpp"
#include "fpga_cpim/sim_stats.hpp"
#include "fpga_cpim/world.hpp"

namespace fpga_cpim {

class EventRouter;

struct EngineConfig {
  uint32_t num_worlds = 4;

  uint64_t max_events_per_probe = 100000;
  uint64_t max_revise_calls_per_probe = 100000;
  uint64_t max_epochs_per_probe = 1000;
  uint64_t max_support_words_per_probe = 1000000;

  bool nsac_enabled = false;
  uint32_t nsac_radius = 0;
  RouterConfig router;
  ReviseTileConfig revise_tile;
  SupportOracleConfig support_oracle;
};

class PropagationEngine {
 public:
  PropagationEngine(const Model& model, EngineConfig cfg);

  WorldResult RunAC(const std::vector<DomainMask>& initial_domains,
                    const std::vector<Cid>& initial_frontier) const;

  WorldResult RunProbe(const std::vector<DomainMask>& base_domains, VarId var,
                       Value value) const;

  std::vector<WorldResult> RunProbeBatch(
      const std::vector<DomainMask>& base_domains,
      const std::vector<std::pair<VarId, Value>>& probes) const;

 private:
  std::vector<bool> BuildAllowedConstraints(VarId focal_var) const;
  bool IsAllowed(const std::vector<bool>& allowed, Cid cid) const;
  bool EnqueueFromVar(EventRouter* router, WorldId world, VarId var,
                      const std::vector<bool>& allowed) const;

  const Model& model_;
  EngineConfig cfg_;
};

}  // namespace fpga_cpim

#endif  // FPGA_CPIM_ENGINE_HPP_
