#include "cpim_hls_types.hpp"

#include "support_oracle_hls.hpp"

namespace fpga_cpim_hls {

void hls_revise_constraint(
    const word_t bit_sup[MAX_CONSTRAINTS * MAX_DOMAIN * MAX_WORDS * 2],
    const ConstraintHls& c, uint8_t dir,
    word_t domains[MAX_VARS][MAX_WORDS], DeleteEventHls* del,
    bool* dwo, uint32_t* words_touched) {
  const uint16_t target_var = dir == 0 ? c.x : c.y;
  const uint16_t target_size = dir == 0 ? c.x_domain_size : c.y_domain_size;
  const uint16_t target_words = word_count(target_size);
  del->var = target_var;
  for (uint16_t w = 0; w < MAX_WORDS; ++w) {
    del->del_words[w] = 0;
  }

  for (uint16_t value = 0; value < MAX_DOMAIN; ++value) {
    if (value >= target_size) {
      break;
    }
    FPGA_CPIM_HLS_PRAGMA(HLS PIPELINE II = 1)
    const word_t bit = word_t{1} << (value % 32u);
    if ((domains[target_var][value / 32u] & bit) == 0) {
      continue;
    }
    if (!hls_support_exists(bit_sup, c, dir, value, domains, words_touched)) {
      del->del_words[value / 32u] |= bit;
    }
  }

  bool empty_after = true;
  for (uint16_t w = 0; w < MAX_WORDS; ++w) {
    if (w >= target_words) {
      break;
    }
    const word_t mask = (w + 1 == target_words) ? last_word_mask(target_size)
                                                : 0xffffffffu;
    if ((domains[target_var][w] & ~del->del_words[w] & mask) != 0) {
      empty_after = false;
    }
  }
  *dwo = empty_after;
}

}  // namespace fpga_cpim_hls
