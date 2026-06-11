#ifndef FPGA_CPIM_GOLDEN_HPP_
#define FPGA_CPIM_GOLDEN_HPP_

#include <cstdint>
#include <vector>

#include "fpga_cpim/bitset.hpp"
#include "fpga_cpim/model.hpp"

namespace fpga_cpim {

enum class PropStatus { kOK, kDWO, kUNKNOWN };

struct GoldenResult {
  PropStatus status = PropStatus::kOK;
  std::vector<DomainMask> domains;
  uint64_t revise_calls = 0;
  uint64_t deleted_values = 0;
};

GoldenResult EnforceAC_Golden(
    const Model& model,
    std::vector<DomainMask> initial_domains,
    const std::vector<Cid>& initial_frontier);

GoldenResult RunSingletonProbe_Golden(
    const Model& model,
    const std::vector<DomainMask>& base_domains,
    VarId var,
    Value value,
    const std::vector<Cid>& allowed_constraints = {});

}  // namespace fpga_cpim

#endif  // FPGA_CPIM_GOLDEN_HPP_
