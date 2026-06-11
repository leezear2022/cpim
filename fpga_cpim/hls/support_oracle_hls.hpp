#ifndef FPGA_CPIM_SUPPORT_ORACLE_HLS_HPP_
#define FPGA_CPIM_SUPPORT_ORACLE_HLS_HPP_

#include "cpim_hls_types.hpp"

namespace fpga_cpim_hls {

inline bool hls_support_exists(
    const word_t bit_sup[MAX_CONSTRAINTS * MAX_DOMAIN * MAX_WORDS * 2],
    const ConstraintHls& c, uint8_t dir, uint16_t value,
    word_t domains[MAX_VARS][MAX_WORDS], uint32_t* words_touched) {
  const uint16_t other_var = dir == 0 ? c.y : c.x;
  const uint16_t row_count = dir == 0 ? c.x_domain_size : c.y_domain_size;
  const uint16_t other_size = dir == 0 ? c.y_domain_size : c.x_domain_size;
  const uint16_t other_words = word_count(other_size);
  const uint32_t offset = dir == 0 ? c.bit_sup_offset_dir0 : c.bit_sup_offset_dir1;
  if (value >= row_count) {
    return false;
  }
  for (uint16_t w = 0; w < MAX_WORDS; ++w) {
    if (w >= other_words) {
      break;
    }
    FPGA_CPIM_HLS_PRAGMA(HLS PIPELINE II = 1)
    ++(*words_touched);
    const word_t support_word = bit_sup[offset + value * other_words + w];
    const word_t domain_word =
        domains[other_var][w] & (w + 1 == other_words ? last_word_mask(other_size)
                                                       : 0xffffffffu);
    if ((support_word & domain_word) != 0) {
      return true;
    }
  }
  return false;
}

}  // namespace fpga_cpim_hls

#endif  // FPGA_CPIM_SUPPORT_ORACLE_HLS_HPP_
