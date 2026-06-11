#include "fpga_cpim/revise_tile.hpp"

#include <algorithm>
#include <cassert>

namespace fpga_cpim {

ReviseTile::ReviseTile(const Model& model, const SupportOracle& oracle,
                       ReviseTileConfig cfg)
    : model_(model), oracle_(oracle), cfg_(cfg) {}

std::vector<ReviseOutput> ReviseTile::Process(
    const DirtyEvent& ev,
    const std::vector<DomainMask>& world_domains) const {
  std::vector<ReviseOutput> outputs;
  if ((ev.pending_dirs & 0x1u) != 0) {
    outputs.push_back(ProcessDirection(ev, 0, world_domains));
  }
  if ((ev.pending_dirs & 0x2u) != 0) {
    outputs.push_back(ProcessDirection(ev, 1, world_domains));
  }
  return outputs;
}

ReviseOutput ReviseTile::ProcessDirection(
    const DirtyEvent& ev, uint8_t dir_bit,
    const std::vector<DomainMask>& world_domains) const {
  assert(ev.cid < model_.constraints.size());
  const BinaryConstraint& c = model_.constraints[ev.cid];
  const VarId target_var = (dir_bit == 0) ? c.x : c.y;
  const uint32_t target_size = model_.domain_size[target_var];

  ReviseOutput out;
  out.world = ev.world;
  out.target_var = target_var;
  out.delete_mask = DomainMask(target_size);
  out.support_bank_accesses.assign(oracle_.BankCount(), 0);

  uint32_t values_seen = 0;
  uint64_t words_seen = 0;
  for (Value value = 0; value < target_size; ++value) {
    if (!world_domains[target_var].Test(value)) {
      continue;
    }
    ++values_seen;
    if (values_seen > cfg_.max_values_per_revise) {
      out.overflow = true;
      return out;
    }
    const SupportResult support = oracle_.Exists(
        SupportQuery{ev.cid, dir_bit, value, ev.world}, world_domains);
    words_seen += support.words_touched;
    out.support_words_touched += support.words_touched;
    out.support_latency_cycles += support.latency_cycles;
    out.support_bank_conflicts += support.bank_conflicts;
    if (out.support_bank_accesses.size() < support.bank_accesses.size()) {
      out.support_bank_accesses.resize(support.bank_accesses.size(), 0);
    }
    for (uint32_t bank = 0; bank < support.bank_accesses.size(); ++bank) {
      out.support_bank_accesses[bank] += support.bank_accesses[bank];
      out.support_max_bank_accesses =
          std::max(out.support_max_bank_accesses, out.support_bank_accesses[bank]);
    }
    if (words_seen > cfg_.max_words_per_revise) {
      out.overflow = true;
      return out;
    }
    if (!support.support_exists) {
      out.delete_mask.Set(value);
    }
  }

  DomainMask after = world_domains[target_var];
  after.AndNot(out.delete_mask);
  out.dwo = after.Empty();
  return out;
}

}  // namespace fpga_cpim
