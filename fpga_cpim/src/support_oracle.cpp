#include "fpga_cpim/support_oracle.hpp"

#include <cassert>

namespace fpga_cpim {

SupportOracle::SupportOracle(const Model& model) : model_(model) {}

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

  if (q.value >= row_count || world_domains[other_var].Empty()) {
    return {};
  }

  SupportResult result;
  for (uint32_t w = 0; w < other_words; ++w) {
    ++result.words_touched;
    const uint32_t support_word =
        model_.bit_sup_words[offset + static_cast<uint64_t>(q.value) * other_words + w];
    if ((support_word & world_domains[other_var].WordAt(w)) != 0) {
      result.support_exists = true;
      return result;
    }
  }
  return result;
}

}  // namespace fpga_cpim
