#ifndef FPGA_CPIM_SUPPORT_ORACLE_HPP_
#define FPGA_CPIM_SUPPORT_ORACLE_HPP_

#include <cstdint>
#include <vector>

#include "fpga_cpim/bitset.hpp"
#include "fpga_cpim/model.hpp"
#include "fpga_cpim/sim_config.hpp"

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
  uint64_t latency_cycles = 0;
  uint64_t bank_conflicts = 0;
  uint32_t max_bank_id = 0;
  std::vector<uint64_t> bank_accesses;
};

class SupportOracle {
 public:
  explicit SupportOracle(const Model& model, SupportOracleConfig cfg = {});

  SupportResult Exists(
      const SupportQuery& q,
      const std::vector<DomainMask>& world_domains) const;

  uint32_t BankCount() const { return cfg_.num_banks; }

 private:
  uint32_t BankId(uint64_t bit_sup_word_index) const;
  void RecordBankAccess(uint64_t bit_sup_word_index, SupportResult* result) const;
  void FinalizeLatency(SupportResult* result) const;

  const Model& model_;
  SupportOracleConfig cfg_;
};

}  // namespace fpga_cpim

#endif  // FPGA_CPIM_SUPPORT_ORACLE_HPP_
