#include "fpga_cpim/variable_owner.hpp"

#include <cassert>

namespace fpga_cpim {

VariableOwner::VariableOwner(const Model& model, uint32_t num_worlds)
    : model_(model),
      domains_(num_worlds),
      states_(num_worlds,
              std::vector<OwnerVarState>(model.num_vars, OwnerVarState::kIdle)) {
  for (std::vector<DomainMask>& world : domains_) {
    world.reserve(model_.num_vars);
    for (VarId var = 0; var < model_.num_vars; ++var) {
      DomainMask mask(model_.domain_size[var]);
      mask.SetAllValid(model_.domain_size[var]);
      world.push_back(mask);
    }
  }
}

OwnerApplyResult VariableOwner::ApplyDeletion(WorldId world, VarId var,
                                              const DomainMask& delete_mask) {
  assert(world < domains_.size());
  assert(var < domains_[world].size());
  DomainMask& domain = domains_[world][var];
  OwnerApplyResult result;
  result.world = world;
  result.var = var;
  result.delta_mask = DomainMask(domain.NBits());

  for (uint32_t w = 0; w < domain.WordCount(); ++w) {
    const uint32_t removed = domain.WordAt(w) & delete_mask.WordAt(w);
    if (removed != 0) {
      result.delta_mask.MutableWordAt(w) = removed;
      result.deleted_count += domain.ApplyDeleteMaskWord(w, removed);
    }
  }
  result.changed = result.deleted_count != 0;
  result.dwo = result.changed && domain.Empty();
  return result;
}

OwnerApplyResult VariableOwner::ApplyDeletionBatch(
    WorldId world, VarId var, const std::vector<DomainMask>& delete_masks) {
  assert(var < model_.domain_size.size());
  DomainMask merged(model_.domain_size[var]);
  for (const DomainMask& mask : delete_masks) {
    assert(mask.NBits() == merged.NBits());
    for (uint32_t w = 0; w < merged.WordCount(); ++w) {
      merged.MutableWordAt(w) |= mask.WordAt(w);
    }
  }
  return ApplyDeletion(world, var, merged);
}

std::vector<Cid> VariableOwner::FanoutForDelta(VarId var,
                                               const DomainMask& delta,
                                               Cid skip_cid) const {
  assert(var < model_.subscription.size());
  std::vector<Cid> fanout;
  if (delta.Empty()) {
    return fanout;
  }
  fanout.reserve(model_.subscription[var].size());
  for (Cid cid : model_.subscription[var]) {
    if (cid != skip_cid) {
      fanout.push_back(cid);
    }
  }
  return fanout;
}

void VariableOwner::BeginProcessing(WorldId world, VarId var) {
  assert(world < states_.size());
  assert(var < states_[world].size());
  states_[world][var] = OwnerVarState::kProcessing;
}

bool VariableOwner::NoteRerunRequest(WorldId world, VarId var) {
  assert(world < states_.size());
  assert(var < states_[world].size());
  if (states_[world][var] == OwnerVarState::kProcessing) {
    states_[world][var] = OwnerVarState::kRerunPending;
    return true;
  }
  return states_[world][var] == OwnerVarState::kRerunPending;
}

bool VariableOwner::EndProcessing(WorldId world, VarId var) {
  assert(world < states_.size());
  assert(var < states_[world].size());
  const bool rerun_pending =
      states_[world][var] == OwnerVarState::kRerunPending;
  states_[world][var] = OwnerVarState::kIdle;
  return rerun_pending;
}

OwnerVarState VariableOwner::State(WorldId world, VarId var) const {
  assert(world < states_.size());
  assert(var < states_[world].size());
  return states_[world][var];
}

const DomainMask& VariableOwner::Domain(WorldId world, VarId var) const {
  assert(world < domains_.size());
  assert(var < domains_[world].size());
  return domains_[world][var];
}

std::vector<DomainMask>& VariableOwner::MutableWorldDomains(WorldId world) {
  assert(world < domains_.size());
  return domains_[world];
}

const std::vector<DomainMask>& VariableOwner::WorldDomains(WorldId world) const {
  assert(world < domains_.size());
  return domains_[world];
}

}  // namespace fpga_cpim
