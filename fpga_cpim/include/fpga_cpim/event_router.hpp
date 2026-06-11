#ifndef FPGA_CPIM_EVENT_ROUTER_HPP_
#define FPGA_CPIM_EVENT_ROUTER_HPP_

#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>
#include <vector>

#include "fpga_cpim/bitset.hpp"
#include "fpga_cpim/model.hpp"
#include "fpga_cpim/revise_tile.hpp"
#include "fpga_cpim/sim_config.hpp"
#include "fpga_cpim/sim_stats.hpp"

namespace fpga_cpim {

class EventRouter {
 public:
  EventRouter(const Model& model, RouterConfig cfg);

  bool Seed(WorldId world, VarId assigned_var);
  bool EnqueueConstraint(WorldId world, Cid cid, uint8_t dirs);
  bool EnqueueFromDelta(WorldId world, VarId var, const DomainMask& delta);
  std::optional<DirtyEvent> Pop();
  bool Empty() const;
  size_t PendingEventCount() const;
  bool HasOverflow() const { return has_overflow_; }
  bool WorldOverflow(WorldId world) const;
  const RouterStats& Stats() const { return stats_; }

 private:
  uint64_t Key(WorldId world, Cid cid) const;
  void MarkOverflow(WorldId world);
  uint8_t DirsForChangedVar(Cid cid, VarId var) const;

  const Model& model_;
  RouterConfig cfg_;
  std::deque<uint64_t> queue_;
  std::unordered_map<uint64_t, uint8_t> pending_dirs_;
  std::unordered_map<WorldId, uint32_t> pending_per_world_;
  std::unordered_map<uint64_t, uint32_t> pending_per_world_cid_;
  std::vector<bool> overflow_worlds_;
  RouterStats stats_;
  bool has_overflow_ = false;
  uint32_t epoch_ = 0;
};

}  // namespace fpga_cpim

#endif  // FPGA_CPIM_EVENT_ROUTER_HPP_
