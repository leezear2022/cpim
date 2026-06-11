#include "cpim_hls_types.hpp"

namespace fpga_cpim_hls {

namespace {

uint32_t popcount32(word_t v) {
  uint32_t count = 0;
  while (v != 0) {
    v &= (v - 1u);
    ++count;
  }
  return count;
}

}  // namespace

bool hls_apply_delete(word_t domains[MAX_VARS][MAX_WORDS], uint16_t var,
                      uint16_t domain_bits, const word_t del_words[MAX_WORDS],
                      uint32_t* deleted_count) {
  bool any = false;
  bool empty = true;
  const uint16_t words = word_count(domain_bits);
  for (uint16_t w = 0; w < MAX_WORDS; ++w) {
    if (w >= words) {
      break;
    }
    FPGA_CPIM_HLS_PRAGMA(HLS PIPELINE II = 1)
    const word_t mask = (w + 1 == words) ? last_word_mask(domain_bits) : 0xffffffffu;
    const word_t removed = domains[var][w] & del_words[w] & mask;
    if (removed != 0) {
      any = true;
      *deleted_count += popcount32(removed);
      domains[var][w] &= ~removed;
    }
    domains[var][w] &= mask;
    if (domains[var][w] != 0) {
      empty = false;
    }
  }
  return any && empty;
}

}  // namespace fpga_cpim_hls
