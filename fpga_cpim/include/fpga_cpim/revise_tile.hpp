#ifndef FPGA_CPIM_REVISE_TILE_HPP_
#define FPGA_CPIM_REVISE_TILE_HPP_

#include <cstdint>
#include <vector>

#include "fpga_cpim/bitset.hpp"
#include "fpga_cpim/model.hpp"
#include "fpga_cpim/sim_config.hpp"
#include "fpga_cpim/support_oracle.hpp"

namespace fpga_cpim {

struct DirtyEvent {
  WorldId world = 0;
  Cid cid = 0;
  uint8_t pending_dirs = 0;  // bit0: revise x from y, bit1: revise y from x
  uint32_t epoch = 0;
};

struct ReviseOutput {
  WorldId world = 0;
  VarId target_var = 0;
  DomainMask delete_mask;
  bool dwo = false;
  bool overflow = false;
  uint64_t support_words_touched = 0;
  uint64_t support_latency_cycles = 0;
  uint64_t support_bank_conflicts = 0;
  uint64_t support_max_bank_accesses = 0;
  std::vector<uint64_t> support_bank_accesses;
};

class ReviseTile {
 public:
  ReviseTile(const Model& model, const SupportOracle& oracle,
             ReviseTileConfig cfg = {});

  std::vector<ReviseOutput> Process(
      const DirtyEvent& ev,
      const std::vector<DomainMask>& world_domains) const;

 private:
  ReviseOutput ProcessDirection(const DirtyEvent& ev, uint8_t dir_bit,
                                const std::vector<DomainMask>& world_domains) const;

  const Model& model_;
  const SupportOracle& oracle_;
  ReviseTileConfig cfg_;
};

}  // namespace fpga_cpim

#endif  // FPGA_CPIM_REVISE_TILE_HPP_
