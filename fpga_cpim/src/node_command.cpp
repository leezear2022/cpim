#include "fpga_cpim/node_command.hpp"

#include <algorithm>
#include <unordered_set>

#include "fpga_cpim/golden.hpp"

namespace fpga_cpim {

namespace {

std::vector<DomainMask> FullDomains(const Model& model) {
  std::vector<DomainMask> domains;
  domains.reserve(model.num_vars);
  for (uint32_t size : model.domain_size) {
    DomainMask mask(size);
    mask.SetAllValid(size);
    domains.push_back(mask);
  }
  return domains;
}

std::vector<Cid> AllConstraints(const Model& model) {
  std::vector<Cid> cids;
  cids.reserve(model.num_constraints);
  for (Cid cid = 0; cid < model.num_constraints; ++cid) {
    cids.push_back(cid);
  }
  return cids;
}

std::vector<Cid> FrontierFromSeeds(const Model& model,
                                   const std::vector<uint32_t>& seed_vars) {
  if (seed_vars.empty()) {
    return AllConstraints(model);
  }
  std::vector<Cid> frontier;
  std::vector<bool> seen(model.num_constraints, false);
  for (uint32_t var : seed_vars) {
    if (var >= model.subscription.size()) {
      continue;
    }
    for (Cid cid : model.subscription[var]) {
      if (!seen[cid]) {
        seen[cid] = true;
        frontier.push_back(cid);
      }
    }
  }
  return frontier;
}

EngineConfig ApplyBudget(EngineConfig cfg, const Budget& budget,
                         bool probe_budget) {
  cfg.max_events_per_probe =
      probe_budget ? budget.max_probe_events : budget.max_events;
  cfg.max_revise_calls_per_probe = budget.max_revise;
  cfg.max_epochs_per_probe = budget.max_epochs;
  cfg.max_support_words_per_probe = budget.max_support_words;
  cfg.router.max_pending_events_per_world = budget.max_pending_events_per_world;
  cfg.router.max_pending_per_cid = budget.max_pending_per_cid;
  return cfg;
}

void AttachWorldStats(const WorldResult& world, NodeStats* stats) {
  stats->ac_events += world.events;
  stats->ac_revise_calls += world.revise_calls;
  stats->queue_occupancy_max =
      std::max(stats->queue_occupancy_max, world.queue_occupancy_max);
  stats->router_events_dropped_overflow +=
      world.router_events_dropped_overflow;
}

void AttachProbeStats(const WorldResult& world, NodeStats* stats) {
  stats->nsacq_probe_events += world.events;
  stats->queue_occupancy_max =
      std::max(stats->queue_occupancy_max, world.queue_occupancy_max);
  stats->router_events_dropped_overflow +=
      world.router_events_dropped_overflow;
}

uint32_t IncompleteFromWorld(const WorldResult& world, uint32_t budget_reason,
                             uint32_t overflow_reason) {
  uint32_t reason = budget_reason;
  if (world.router_events_dropped_overflow > 0) {
    reason |= overflow_reason | kRouterOverflow;
  }
  return reason;
}

bool ApplyAssignment(const Model& model, const NodeCommand& command,
                     std::vector<DomainMask>* domains, NodeResult* result) {
  if (!command.has_assignment) {
    return true;
  }
  if (command.branch_var >= model.num_vars ||
      command.branch_value >= model.domain_size[command.branch_var] ||
      !(*domains)[command.branch_var].Test(command.branch_value)) {
    result->status = NodeResult::kDWO;
    result->final_domains = *domains;
    return false;
  }
  (*domains)[command.branch_var].SetSingleton(command.branch_value);
  return true;
}

std::vector<uint32_t> EffectiveSeedVars(const Model& model,
                                        const NodeCommand& command) {
  std::vector<uint32_t> seeds = command.seed_vars;
  if (seeds.empty() && command.has_assignment &&
      command.branch_var < model.num_vars) {
    seeds.push_back(command.branch_var);
  }
  return seeds;
}

std::vector<bool> BuildNsacAllowedVarSet(const Model& model,
                                         const NodeCommand& command) {
  std::vector<bool> vars(model.num_vars, false);
  switch (command.nsacq_init) {
    case NodeCommand::kNone:
      return vars;
    case NodeCommand::kAllVars:
      std::fill(vars.begin(), vars.end(), true);
      return vars;
    case NodeCommand::kFocalVar:
      if (command.focal_var < model.num_vars) {
        vars[command.focal_var] = true;
      }
      return vars;
    case NodeCommand::kNeighborhoodOfFocal:
      if (command.focal_var >= model.num_vars) {
        return vars;
      }
      vars[command.focal_var] = true;
      if (command.nsac_radius == 0) {
        return vars;
      }
      for (Cid cid : model.subscription[command.focal_var]) {
        const auto [x, y] = model.scopes[cid];
        vars[x] = true;
        vars[y] = true;
      }
      return vars;
  }
  return vars;
}

std::vector<Cid> BuildAllowedConstraints(const Model& model,
                                         const NodeCommand& command) {
  if (command.nsacq_init == NodeCommand::kAllVars ||
      command.nsacq_init == NodeCommand::kNone) {
    return {};
  }
  std::vector<bool> vars = BuildNsacAllowedVarSet(model, command);
  std::vector<Cid> allowed;
  for (Cid cid = 0; cid < model.num_constraints; ++cid) {
    const auto [x, y] = model.scopes[cid];
    if (vars[x] && vars[y]) {
      allowed.push_back(cid);
    }
  }
  return allowed;
}

std::vector<std::pair<VarId, Value>> BuildNsacProbes(
    const Model& model, const NodeCommand& command,
    const std::vector<DomainMask>& domains) {
  std::vector<bool> allowed_vars = BuildNsacAllowedVarSet(model, command);
  std::vector<std::pair<VarId, Value>> probes;
  for (VarId var = 0; var < model.num_vars; ++var) {
    if (!allowed_vars[var]) {
      continue;
    }
    for (Value value = 0; value < model.domain_size[var]; ++value) {
      if (domains[var].Test(value)) {
        probes.push_back({var, value});
      }
    }
  }
  return probes;
}

void AddConfirmedDeletion(VarId var, Value value,
                          std::vector<DeletionMask>* deletions,
                          const Model& model) {
  DeletionMask deletion;
  deletion.var = var;
  deletion.mask = DomainMask(model.domain_size[var]);
  deletion.mask.Set(value);
  deletions->push_back(deletion);
}

std::vector<VarId> ApplyConfirmedDeletions(
    const Model& model, const std::vector<DeletionMask>& deletions,
    std::vector<DomainMask>* domains) {
  std::vector<VarId> changed_vars;
  std::vector<bool> seen(model.num_vars, false);
  std::vector<DomainMask> merged;
  merged.reserve(model.num_vars);
  for (uint32_t size : model.domain_size) {
    merged.emplace_back(size);
  }
  for (const DeletionMask& deletion : deletions) {
    if (deletion.var >= model.num_vars) {
      continue;
    }
    for (uint32_t w = 0; w < deletion.mask.WordCount(); ++w) {
      merged[deletion.var].MutableWordAt(w) |= deletion.mask.WordAt(w);
    }
  }
  for (VarId var = 0; var < model.num_vars; ++var) {
    const uint32_t before = (*domains)[var].Count();
    (*domains)[var].AndNot(merged[var]);
    if ((*domains)[var].Count() != before && !seen[var]) {
      seen[var] = true;
      changed_vars.push_back(var);
    }
  }
  return changed_vars;
}

bool AnyDomainEmpty(const std::vector<DomainMask>& domains) {
  for (const DomainMask& domain : domains) {
    if (domain.Empty()) {
      return true;
    }
  }
  return false;
}

}  // namespace

NodeResult RunNodeCommand(const Model& model, const NodeCommand& command,
                          const std::vector<DomainMask>& base_domains,
                          const EngineConfig& base_engine_cfg) {
  NodeResult result;
  result.final_domains = base_domains.empty() ? FullDomains(model) : base_domains;

  if (command.mode == NodeCommand::kRunBranchProbes) {
    result.status = NodeResult::kAliveIncomplete;
    result.ac_complete = false;
    result.nsacq_complete = false;
    result.incomplete_reason_mask |= kDeadlockGuard;
    return result;
  }

  if (!ApplyAssignment(model, command, &result.final_domains, &result)) {
    return result;
  }

  const std::vector<uint32_t> seeds = EffectiveSeedVars(model, command);
  const std::vector<Cid> frontier = FrontierFromSeeds(model, seeds);
  PropagationEngine ac_engine(model,
                              ApplyBudget(base_engine_cfg, command.budget, false));
  const WorldResult ac_world = ac_engine.RunAC(result.final_domains, frontier);
  AttachWorldStats(ac_world, &result.stats);
  if (ac_world.status == WorldStatus::kUNKNOWN) {
    result.status = NodeResult::kAliveIncomplete;
    result.ac_complete = false;
    result.nsacq_complete = false;
    result.incomplete_reason_mask |=
        IncompleteFromWorld(ac_world, kAcBudget, kVarQueueOverflow);
    return result;
  }
  if (ac_world.status == WorldStatus::kDWO) {
    result.status = NodeResult::kDWO;
    return result;
  }

  GoldenResult ac_golden =
      EnforceAC_Golden(model, result.final_domains, frontier);
  result.final_domains = ac_golden.domains;
  if (ac_golden.status == PropStatus::kDWO) {
    result.status = NodeResult::kDWO;
    return result;
  }

  if (command.mode == NodeCommand::kRunAC) {
    result.status = NodeResult::kAliveComplete;
    result.nsacq_complete = true;
    return result;
  }

  EngineConfig probe_cfg = ApplyBudget(base_engine_cfg, command.budget, true);
  if (command.nsacq_init == NodeCommand::kNeighborhoodOfFocal) {
    probe_cfg.nsac_enabled = true;
    probe_cfg.nsac_radius = command.nsac_radius;
  }
  PropagationEngine probe_engine(model, probe_cfg);
  const std::vector<Cid> golden_allowed =
      BuildAllowedConstraints(model, command);
  const std::vector<std::pair<VarId, Value>> probes =
      BuildNsacProbes(model, command, result.final_domains);
  result.stats.nsacq_probes = probes.size();

  for (const auto& probe : probes) {
    const VarId var = probe.first;
    const Value value = probe.second;
    WorldResult probe_world = probe_engine.RunProbe(result.final_domains, var, value);
    AttachProbeStats(probe_world, &result.stats);
    if (probe_world.status == WorldStatus::kUNKNOWN) {
      ++result.unknown_probe_count;
      result.incomplete_reason_mask |=
          IncompleteFromWorld(probe_world, kProbeBudget, kProbeQueueOverflow);
      continue;
    }
    if (probe_world.status != WorldStatus::kDWO) {
      continue;
    }
    GoldenResult probe_golden = RunSingletonProbe_Golden(
        model, result.final_domains, var, value, golden_allowed);
    if (probe_golden.status == PropStatus::kDWO) {
      AddConfirmedDeletion(var, value, &result.confirmed_deletions, model);
    }
  }

  result.has_confirmed_deletions = !result.confirmed_deletions.empty();
  const std::vector<VarId> changed_vars = ApplyConfirmedDeletions(
      model, result.confirmed_deletions, &result.final_domains);
  if (AnyDomainEmpty(result.final_domains)) {
    result.status = NodeResult::kDWO;
    result.nsacq_complete = result.unknown_probe_count == 0;
    return result;
  }

  if (!changed_vars.empty()) {
    std::vector<uint32_t> rerun_seeds(changed_vars.begin(), changed_vars.end());
    const std::vector<Cid> rerun_frontier = FrontierFromSeeds(model, rerun_seeds);
    WorldResult rerun_world =
        ac_engine.RunAC(result.final_domains, rerun_frontier);
    AttachWorldStats(rerun_world, &result.stats);
    if (rerun_world.status == WorldStatus::kUNKNOWN) {
      result.status = NodeResult::kAliveIncomplete;
      result.ac_complete = false;
      result.incomplete_reason_mask |=
          IncompleteFromWorld(rerun_world, kAcBudget, kVarQueueOverflow);
      return result;
    }
    GoldenResult rerun_golden =
        EnforceAC_Golden(model, result.final_domains, rerun_frontier);
    result.final_domains = rerun_golden.domains;
    if (rerun_world.status == WorldStatus::kDWO ||
        rerun_golden.status == PropStatus::kDWO) {
      result.status = NodeResult::kDWO;
      return result;
    }
  }

  if (result.unknown_probe_count != 0) {
    result.status = NodeResult::kAliveIncomplete;
    result.nsacq_complete = false;
  } else {
    result.status = NodeResult::kAliveComplete;
  }
  return result;
}

}  // namespace fpga_cpim
