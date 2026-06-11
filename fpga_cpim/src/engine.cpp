#include "fpga_cpim/engine.hpp"

#include <algorithm>
#include <unordered_set>

#include "fpga_cpim/event_router.hpp"
#include "fpga_cpim/revise_tile.hpp"
#include "fpga_cpim/support_oracle.hpp"
#include "fpga_cpim/variable_owner.hpp"

namespace fpga_cpim {

namespace {

void AttachRouterStats(const EventRouter& router, WorldResult* result) {
  const RouterStats& stats = router.Stats();
  result->queue_occupancy_p50 = Percentile(stats.occupancy_samples, 50);
  result->queue_occupancy_p95 = Percentile(stats.occupancy_samples, 95);
  result->queue_occupancy_max = stats.max_queue_occupancy;
  result->router_events_enqueued = stats.events_enqueued;
  result->router_events_deduped = stats.events_deduped;
  result->router_events_dropped_overflow = stats.events_dropped_overflow;
}

}  // namespace

PropagationEngine::PropagationEngine(const Model& model, EngineConfig cfg)
    : model_(model), cfg_(cfg) {}

WorldResult PropagationEngine::RunAC(
    const std::vector<DomainMask>& initial_domains,
    const std::vector<Cid>& initial_frontier) const {
  WorldResult result;
  VariableOwner owner(model_, 1);
  owner.MutableWorldDomains(0) = initial_domains;

  EventRouter router(model_, cfg_.router);
  std::vector<bool> allowed(model_.num_constraints, true);
  for (Cid cid : initial_frontier) {
    if (cid < model_.num_constraints &&
        !router.EnqueueConstraint(0, cid, 0x3u)) {
      result.status = WorldStatus::kUNKNOWN;
      AttachRouterStats(router, &result);
      return result;
    }
  }
  if (router.HasOverflow()) {
    result.status = WorldStatus::kUNKNOWN;
    AttachRouterStats(router, &result);
    return result;
  }

  SupportOracle oracle(model_);
  ReviseTile tile(model_, oracle, cfg_.revise_tile);
  while (!router.Empty()) {
    ++result.epochs;
    if (result.epochs > cfg_.max_epochs_per_probe) {
      result.status = WorldStatus::kUNKNOWN;
      AttachRouterStats(router, &result);
      return result;
    }
    const size_t epoch_events = router.PendingEventCount();
    for (size_t i = 0; i < epoch_events; ++i) {
      std::optional<DirtyEvent> ev = router.Pop();
      if (!ev.has_value()) {
        break;
      }
      ++result.events;
      if (result.events > cfg_.max_events_per_probe) {
        result.status = WorldStatus::kUNKNOWN;
        AttachRouterStats(router, &result);
        return result;
      }
      const uint32_t dir_count =
          ((ev->pending_dirs & 0x1u) ? 1u : 0u) +
          ((ev->pending_dirs & 0x2u) ? 1u : 0u);
      result.revise_calls += dir_count;
      if (result.revise_calls > cfg_.max_revise_calls_per_probe) {
        result.status = WorldStatus::kUNKNOWN;
        AttachRouterStats(router, &result);
        return result;
      }
      std::vector<ReviseOutput> outputs = tile.Process(*ev, owner.WorldDomains(0));
      bool overflow = false;
      uint64_t words = 0;
      for (const ReviseOutput& output : outputs) {
        overflow = overflow || output.overflow;
        words += output.support_words_touched;
      }
      if (overflow) {
        result.status = WorldStatus::kUNKNOWN;
        AttachRouterStats(router, &result);
        return result;
      }
      result.support_words_touched += words;
      if (result.support_words_touched > cfg_.max_support_words_per_probe) {
        result.status = WorldStatus::kUNKNOWN;
        AttachRouterStats(router, &result);
        return result;
      }
      for (const ReviseOutput& output : outputs) {
        if (output.delete_mask.Empty()) {
          continue;
        }
        OwnerApplyResult applied =
            owner.ApplyDeletion(output.world, output.target_var, output.delete_mask);
        if (!applied.changed) {
          continue;
        }
        result.deleted_values += applied.deleted_count;
        if (applied.dwo) {
          result.status = WorldStatus::kDWO;
          AttachRouterStats(router, &result);
          return result;
        }
        if (!EnqueueFromVar(&router, output.world, output.target_var, allowed) ||
            router.HasOverflow()) {
          result.status = WorldStatus::kUNKNOWN;
          AttachRouterStats(router, &result);
          return result;
        }
      }
    }
  }
  result.status = WorldStatus::kOK;
  AttachRouterStats(router, &result);
  return result;
}

WorldResult PropagationEngine::RunProbe(
    const std::vector<DomainMask>& base_domains, VarId var, Value value) const {
  WorldResult result;
  result.world = 0;
  result.var = var;
  result.value = value;

  if (var >= model_.num_vars || value >= model_.domain_size[var] ||
      !base_domains[var].Test(value)) {
    result.status = WorldStatus::kDWO;
    return result;
  }

  VariableOwner owner(model_, 1);
  owner.MutableWorldDomains(0) = base_domains;
  owner.MutableWorldDomains(0)[var].SetSingleton(value);

  EventRouter router(model_, cfg_.router);
  const std::vector<bool> allowed = BuildAllowedConstraints(var);
  if (!EnqueueFromVar(&router, 0, var, allowed) || router.HasOverflow()) {
    result.status = WorldStatus::kUNKNOWN;
    AttachRouterStats(router, &result);
    return result;
  }

  SupportOracle oracle(model_);
  ReviseTile tile(model_, oracle, cfg_.revise_tile);

  while (!router.Empty()) {
    ++result.epochs;
    if (result.epochs > cfg_.max_epochs_per_probe) {
      result.status = WorldStatus::kUNKNOWN;
      AttachRouterStats(router, &result);
      return result;
    }

    const size_t epoch_events = router.PendingEventCount();
    for (size_t i = 0; i < epoch_events; ++i) {
      std::optional<DirtyEvent> ev = router.Pop();
      if (!ev.has_value()) {
        break;
      }
      if (!IsAllowed(allowed, ev->cid)) {
        continue;
      }
      ++result.events;
      if (result.events > cfg_.max_events_per_probe) {
        result.status = WorldStatus::kUNKNOWN;
        AttachRouterStats(router, &result);
        return result;
      }

      const uint32_t dir_count =
          ((ev->pending_dirs & 0x1u) ? 1u : 0u) +
          ((ev->pending_dirs & 0x2u) ? 1u : 0u);
      result.revise_calls += dir_count;
      if (result.revise_calls > cfg_.max_revise_calls_per_probe) {
        result.status = WorldStatus::kUNKNOWN;
        AttachRouterStats(router, &result);
        return result;
      }

      std::vector<ReviseOutput> outputs = tile.Process(*ev, owner.WorldDomains(0));
      bool overflow = false;
      uint64_t new_support_words = 0;
      for (const ReviseOutput& output : outputs) {
        overflow = overflow || output.overflow;
        new_support_words += output.support_words_touched;
      }
      if (overflow) {
        result.status = WorldStatus::kUNKNOWN;
        AttachRouterStats(router, &result);
        return result;
      }
      result.support_words_touched += new_support_words;
      if (result.support_words_touched > cfg_.max_support_words_per_probe) {
        result.status = WorldStatus::kUNKNOWN;
        AttachRouterStats(router, &result);
        return result;
      }

      for (const ReviseOutput& output : outputs) {
        if (output.delete_mask.Empty()) {
          continue;
        }
        OwnerApplyResult applied =
            owner.ApplyDeletion(output.world, output.target_var, output.delete_mask);
        if (!applied.changed) {
          continue;
        }
        result.deleted_values += applied.deleted_count;
        if (applied.dwo) {
          result.status = WorldStatus::kDWO;
          AttachRouterStats(router, &result);
          return result;
        }
        if (!EnqueueFromVar(&router, output.world, output.target_var, allowed) ||
            router.HasOverflow()) {
          result.status = WorldStatus::kUNKNOWN;
          AttachRouterStats(router, &result);
          return result;
        }
      }
    }
  }

  result.status = WorldStatus::kOK;
  AttachRouterStats(router, &result);
  return result;
}

std::vector<WorldResult> PropagationEngine::RunProbeBatch(
    const std::vector<DomainMask>& base_domains,
    const std::vector<std::pair<VarId, Value>>& probes) const {
  std::vector<WorldResult> results;
  results.reserve(probes.size());
  for (size_t i = 0; i < probes.size(); ++i) {
    WorldResult result = RunProbe(base_domains, probes[i].first, probes[i].second);
    result.world = static_cast<WorldId>(i % std::max(1u, cfg_.num_worlds));
    results.push_back(result);
  }
  return results;
}

std::vector<bool> PropagationEngine::BuildAllowedConstraints(VarId focal_var) const {
  std::vector<bool> allowed(model_.num_constraints, true);
  if (!cfg_.nsac_enabled || cfg_.nsac_radius == 0) {
    return allowed;
  }
  std::fill(allowed.begin(), allowed.end(), false);
  std::vector<bool> allowed_vars(model_.num_vars, false);
  allowed_vars[focal_var] = true;
  for (Cid cid : model_.subscription[focal_var]) {
    const auto [x, y] = model_.scopes[cid];
    allowed_vars[x] = true;
    allowed_vars[y] = true;
  }
  for (Cid cid = 0; cid < model_.num_constraints; ++cid) {
    const auto [x, y] = model_.scopes[cid];
    allowed[cid] = allowed_vars[x] && allowed_vars[y];
  }
  return allowed;
}

bool PropagationEngine::IsAllowed(const std::vector<bool>& allowed, Cid cid) const {
  return allowed.empty() || cid >= allowed.size() || allowed[cid];
}

bool PropagationEngine::EnqueueFromVar(EventRouter* router, WorldId world, VarId var,
                                       const std::vector<bool>& allowed) const {
  bool ok = true;
  for (Cid cid : model_.subscription[var]) {
    if (!IsAllowed(allowed, cid)) {
      continue;
    }
    const BinaryConstraint& c = model_.constraints[cid];
    uint8_t dirs = 0;
    if (c.x == var) {
      dirs = 0x2u;
    } else if (c.y == var) {
      dirs = 0x1u;
    }
    ok = router->EnqueueConstraint(world, cid, dirs) && ok;
  }
  return ok;
}

}  // namespace fpga_cpim
