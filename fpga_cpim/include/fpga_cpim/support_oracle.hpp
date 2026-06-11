#ifndef FPGA_CPIM_SUPPORT_ORACLE_HPP_
#define FPGA_CPIM_SUPPORT_ORACLE_HPP_

#include <cstdint>
#include <vector>

#include "fpga_cpim/bitset.hpp"
#include "fpga_cpim/model.hpp"

namespace fpga_cpim {

struct SupportQuery {
  Cid cid = 0;
  uint8_t dir = 0;
  uint32_t value = 0;
  WorldId world = 0;
};

struct SupportResult {
  bool support_exists = false;
  uint32_t words_touched = 0;
};

class SupportOracle {
 public:
  explicit SupportOracle(const Model& model);

  SupportResult Exists(
      const SupportQuery& q,
      const std::vector<DomainMask>& world_domains) const;

 private:
  const Model& model_;
};

}  // namespace fpga_cpim

#endif  // FPGA_CPIM_SUPPORT_ORACLE_HPP_
