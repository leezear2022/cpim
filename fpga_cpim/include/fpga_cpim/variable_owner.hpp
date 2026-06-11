#ifndef FPGA_CPIM_VARIABLE_OWNER_HPP_
#define FPGA_CPIM_VARIABLE_OWNER_HPP_

#include <cstdint>
#include <vector>

#include "fpga_cpim/bitset.hpp"
#include "fpga_cpim/model.hpp"

namespace fpga_cpim {

struct OwnerApplyResult {
  WorldId world = 0;
  VarId var = 0;
  bool changed = false;
  bool dwo = false;
  DomainMask delta_mask;
  uint32_t deleted_count = 0;
};

class VariableOwner {
 public:
  VariableOwner(const Model& model, uint32_t num_worlds);

  OwnerApplyResult ApplyDeletion(WorldId world, VarId var,
                                 const DomainMask& delete_mask);

  const DomainMask& Domain(WorldId world, VarId var) const;
  std::vector<DomainMask>& MutableWorldDomains(WorldId world);
  const std::vector<DomainMask>& WorldDomains(WorldId world) const;
  uint32_t NumWorlds() const { return static_cast<uint32_t>(domains_.size()); }

 private:
  const Model& model_;
  std::vector<std::vector<DomainMask>> domains_;
};

}  // namespace fpga_cpim

#endif  // FPGA_CPIM_VARIABLE_OWNER_HPP_
