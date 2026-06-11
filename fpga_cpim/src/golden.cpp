#include "fpga_cpim/golden.hpp"

#include <deque>
#include <unordered_set>

#include "fpga_cpim/support_oracle.hpp"

namespace fpga_cpim {

namespace {

uint64_t Key(WorldId world, Cid cid) {
  return (static_cast<uint64_t>(world) << 32) | cid;
}

std::vector<bool> MakeAllowedSet(const Model& model,
                                 const std::vector<Cid>& allowed_constraints) {
  std::vector<bool> allowed(model.num_constraints, true);
  if (allowed_constraints.empty()) {
    return allowed;
  }
  std::fill(allowed.begin(), allowed.end(), false);
  for (Cid cid : allowed_constraints) {
    if (cid < allowed.size()) {
      allowed[cid] = true;
    }
  }
  return allowed;
}

uint8_t DirsForChangedVar(const Model& model, Cid cid, VarId var) {
  const BinaryConstraint& c = model.constraints[cid];
  if (c.x == var) {
    return 0x2u;
  }
  if (c.y == var) {
    return 0x1u;
  }
  return 0;
}

void Enqueue(std::deque<Cid>* queue, std::unordered_set<uint64_t>* pending,
             Cid cid) {
  const uint64_t key = Key(0, cid);
  if (pending->insert(key).second) {
    queue->push_back(cid);
  }
}

DomainMask ReviseOneDirection(const Model& model, const SupportOracle& oracle,
                              Cid cid, uint8_t dir,
                              const std::vector<DomainMask>& domains) {
  const BinaryConstraint& c = model.constraints[cid];
  const VarId target_var = (dir == 0) ? c.x : c.y;
  DomainMask delete_mask(model.domain_size[target_var]);
  for (Value value = 0; value < model.domain_size[target_var]; ++value) {
    if (!domains[target_var].Test(value)) {
      continue;
    }
    const SupportResult support =
        oracle.Exists(SupportQuery{cid, dir, value, 0}, domains);
    if (!support.support_exists) {
      delete_mask.Set(value);
    }
  }
  return delete_mask;
}

}  // namespace

GoldenResult EnforceAC_Golden(
    const Model& model,
    std::vector<DomainMask> initial_domains,
    const std::vector<Cid>& initial_frontier) {
  GoldenResult result;
  result.domains = std::move(initial_domains);
  for (const DomainMask& domain : result.domains) {
    if (domain.Empty()) {
      result.status = PropStatus::kDWO;
      return result;
    }
  }

  SupportOracle oracle(model);
  std::deque<Cid> queue;
  std::unordered_set<uint64_t> pending;
  for (Cid cid : initial_frontier) {
    if (cid < model.num_constraints) {
      Enqueue(&queue, &pending, cid);
    }
  }

  while (!queue.empty()) {
    const Cid cid = queue.front();
    queue.pop_front();
    pending.erase(Key(0, cid));
    const BinaryConstraint& c = model.constraints[cid];

    for (uint8_t dir = 0; dir < 2; ++dir) {
      ++result.revise_calls;
      const VarId target_var = (dir == 0) ? c.x : c.y;
      DomainMask delete_mask =
          ReviseOneDirection(model, oracle, cid, dir, result.domains);
      if (delete_mask.Empty()) {
        continue;
      }
      const uint32_t before_count = result.domains[target_var].Count();
      result.domains[target_var].AndNot(delete_mask);
      const uint32_t after_count = result.domains[target_var].Count();
      result.deleted_values += before_count - after_count;
      if (result.domains[target_var].Empty()) {
        result.status = PropStatus::kDWO;
        return result;
      }
      for (Cid next_cid : model.subscription[target_var]) {
        if (DirsForChangedVar(model, next_cid, target_var) != 0) {
          Enqueue(&queue, &pending, next_cid);
        }
      }
    }
  }

  result.status = PropStatus::kOK;
  return result;
}

GoldenResult RunSingletonProbe_Golden(
    const Model& model,
    const std::vector<DomainMask>& base_domains,
    VarId var,
    Value value,
    const std::vector<Cid>& allowed_constraints) {
  GoldenResult result;
  if (var >= model.num_vars || value >= model.domain_size[var] ||
      !base_domains[var].Test(value)) {
    result.status = PropStatus::kDWO;
    result.domains = base_domains;
    return result;
  }

  std::vector<DomainMask> domains = base_domains;
  domains[var].SetSingleton(value);
  std::vector<bool> allowed = MakeAllowedSet(model, allowed_constraints);
  std::vector<Cid> frontier;
  for (Cid cid : model.subscription[var]) {
    if (allowed[cid]) {
      frontier.push_back(cid);
    }
  }

  // Inline AC loop with allowed-constraint propagation filtering.
  result.domains = std::move(domains);
  SupportOracle oracle(model);
  std::deque<Cid> queue;
  std::unordered_set<uint64_t> pending;
  for (Cid cid : frontier) {
    Enqueue(&queue, &pending, cid);
  }
  while (!queue.empty()) {
    const Cid cid = queue.front();
    queue.pop_front();
    pending.erase(Key(0, cid));
    if (!allowed[cid]) {
      continue;
    }
    const BinaryConstraint& c = model.constraints[cid];
    for (uint8_t dir = 0; dir < 2; ++dir) {
      ++result.revise_calls;
      const VarId target_var = (dir == 0) ? c.x : c.y;
      DomainMask delete_mask =
          ReviseOneDirection(model, oracle, cid, dir, result.domains);
      if (delete_mask.Empty()) {
        continue;
      }
      const uint32_t before_count = result.domains[target_var].Count();
      result.domains[target_var].AndNot(delete_mask);
      const uint32_t after_count = result.domains[target_var].Count();
      result.deleted_values += before_count - after_count;
      if (result.domains[target_var].Empty()) {
        result.status = PropStatus::kDWO;
        return result;
      }
      for (Cid next_cid : model.subscription[target_var]) {
        if (allowed[next_cid]) {
          Enqueue(&queue, &pending, next_cid);
        }
      }
    }
  }

  result.status = PropStatus::kOK;
  return result;
}

}  // namespace fpga_cpim
