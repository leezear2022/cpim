#include "fpga_cpim/support_oracle.hpp"

#include <algorithm>
#include <cassert>

namespace fpga_cpim {

SupportOracle::SupportOracle(const Model& model, SupportOracleConfig cfg)
    : model_(model), cfg_(cfg) {
  if (cfg_.num_banks == 0) {
    cfg_.num_banks = 1;
  }
}

SupportResult SupportOracle::Exists(
    const SupportQuery& q,
    const std::vector<DomainMask>& world_domains) const {
  assert(q.cid < model_.constraints.size());
  const BinaryConstraint& c = model_.constraints[q.cid];
  const VarId other_var = (q.dir == 0) ? c.y : c.x;
  const uint32_t row_count = (q.dir == 0) ? c.x_domain_size : c.y_domain_size;
  const uint32_t other_domain_size = (q.dir == 0) ? c.y_domain_size : c.x_domain_size;
  const uint32_t other_words = WordCountForBits(other_domain_size);
  const uint64_t offset = (q.dir == 0) ? c.bit_sup_offset_dir0 : c.bit_sup_offset_dir1;

  SupportResult result;
  result.bank_accesses.assign(cfg_.num_banks, 0);
  if (q.value >= row_count || world_domains[other_var].Empty()) {
    return result;
  }

  for (uint32_t w = 0; w < other_words; ++w) {
    ++result.words_touched;
    const uint64_t support_index =
        offset + static_cast<uint64_t>(q.value) * other_words + w;
    RecordBankAccess(support_index, &result);
    const uint32_t support_word = model_.bit_sup_words[support_index];
    if ((support_word & world_domains[other_var].WordAt(w)) != 0) {
      result.support_exists = true;
      FinalizeLatency(&result);
      return result;
    }
  }
  FinalizeLatency(&result);
  return result;
}

uint32_t SupportOracle::BankId(uint64_t bit_sup_word_index) const {
  return static_cast<uint32_t>(bit_sup_word_index % cfg_.num_banks);
}

void SupportOracle::RecordBankAccess(uint64_t bit_sup_word_index,
                                     SupportResult* result) const {
  const uint32_t bank = BankId(bit_sup_word_index);
  ++result->bank_accesses[bank];
  result->max_bank_id = std::max(result->max_bank_id, bank);
}

void SupportOracle::FinalizeLatency(SupportResult* result) const {
  uint32_t active_banks = 0;
  for (uint64_t accesses : result->bank_accesses) {
    if (accesses != 0) {
      ++active_banks;
    }
  }
  result->bank_conflicts =
      result->words_touched > active_banks ? result->words_touched - active_banks : 0;
  result->latency_cycles =
      static_cast<uint64_t>(result->words_touched) * cfg_.base_latency_cycles +
      result->bank_conflicts * cfg_.conflict_penalty_cycles;
}

}  // namespace fpga_cpim
