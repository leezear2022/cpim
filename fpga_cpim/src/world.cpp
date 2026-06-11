#include "fpga_cpim/world.hpp"

#include <cassert>

namespace fpga_cpim {

WorldManager::WorldManager(const Model& model, uint32_t num_worlds)
    : model_(model), domains_(num_worlds), statuses_(num_worlds, WorldStatus::kOK) {}

void WorldManager::InitProbe(WorldId world,
                             const std::vector<DomainMask>& base_domains,
                             VarId singleton_var, Value singleton_value) {
  assert(world < domains_.size());
  assert(base_domains.size() == model_.num_vars);
  domains_[world] = base_domains;
  domains_[world][singleton_var].SetSingleton(singleton_value);
  statuses_[world] = WorldStatus::kOK;
}

void WorldManager::MarkUnknown(WorldId world) {
  assert(world < statuses_.size());
  statuses_[world] = WorldStatus::kUNKNOWN;
}

void WorldManager::MarkDWO(WorldId world) {
  assert(world < statuses_.size());
  statuses_[world] = WorldStatus::kDWO;
}

const std::vector<DomainMask>& WorldManager::Domains(WorldId world) const {
  assert(world < domains_.size());
  return domains_[world];
}

WorldStatus WorldManager::Status(WorldId world) const {
  assert(world < statuses_.size());
  return statuses_[world];
}

const char* WorldStatusName(WorldStatus status) {
  switch (status) {
    case WorldStatus::kOK:
      return "OK";
    case WorldStatus::kDWO:
      return "DWO";
    case WorldStatus::kUNKNOWN:
      return "UNKNOWN";
    case WorldStatus::kBudgetExceeded:
      return "BUDGET_EXCEEDED";
    case WorldStatus::kOverflow:
      return "OVERFLOW";
  }
  return "UNKNOWN";
}

}  // namespace fpga_cpim
