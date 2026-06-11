#include "fpga_cpim/variable_owner.hpp"

#include <cassert>

namespace fpga_cpim {

VariableOwner::VariableOwner(const Model& model, uint32_t num_worlds)
    : model_(model), domains_(num_worlds) {
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
  result.dwo = domain.Empty();
  return result;
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
