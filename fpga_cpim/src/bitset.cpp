#include "fpga_cpim/bitset.hpp"

#include <cassert>
#include <iomanip>
#include <sstream>

#include "fpga_cpim/model.hpp"

namespace fpga_cpim {

namespace {

uint32_t PopCount(uint32_t v) {
#if defined(__GNUC__) || defined(__clang__)
  return static_cast<uint32_t>(__builtin_popcount(v));
#else
  uint32_t count = 0;
  while (v != 0) {
    v &= (v - 1);
    ++count;
  }
  return count;
#endif
}

}  // namespace

DomainMask::DomainMask() = default;

DomainMask::DomainMask(uint32_t nbits) {
  Resize(nbits);
}

void DomainMask::Resize(uint32_t nbits) {
  nbits_ = nbits;
  words_.assign(WordCountForBits(nbits), 0);
}

void DomainMask::Clear() {
  for (uint32_t& word : words_) {
    word = 0;
  }
}

void DomainMask::SetAllValid(uint32_t domain_size) {
  assert(domain_size <= nbits_);
  Clear();
  for (uint32_t bit = 0; bit < domain_size; ++bit) {
    Set(bit);
  }
  MaskInvalidHighBits();
}

void DomainMask::SetSingleton(uint32_t value) {
  assert(value < nbits_);
  Clear();
  Set(value);
}

void DomainMask::Set(uint32_t value) {
  assert(value < nbits_);
  words_[value / 32] |= (uint32_t{1} << (value % 32));
}

bool DomainMask::Test(uint32_t value) const {
  if (value >= nbits_) {
    return false;
  }
  return (words_[value / 32] & (uint32_t{1} << (value % 32))) != 0;
}

bool DomainMask::Empty() const {
  for (uint32_t word : words_) {
    if (word != 0) {
      return false;
    }
  }
  return true;
}

uint32_t DomainMask::Count() const {
  uint32_t total = 0;
  for (uint32_t w = 0; w < words_.size(); ++w) {
    total += PopCount(words_[w] & WordMask(w));
  }
  return total;
}

uint32_t DomainMask::WordCount() const {
  return static_cast<uint32_t>(words_.size());
}

uint32_t DomainMask::WordAt(uint32_t w) const {
  assert(w < words_.size());
  return words_[w] & WordMask(w);
}

uint32_t& DomainMask::MutableWordAt(uint32_t w) {
  assert(w < words_.size());
  return words_[w];
}

uint32_t DomainMask::ApplyDeleteMaskWord(uint32_t w, uint32_t delete_mask) {
  assert(w < words_.size());
  const uint32_t valid_delete = delete_mask & WordMask(w);
  const uint32_t removed = words_[w] & valid_delete;
  words_[w] &= ~removed;
  MaskInvalidHighBits();
  return PopCount(removed);
}

bool DomainMask::AndNot(const DomainMask& delete_mask) {
  assert(nbits_ == delete_mask.nbits_);
  bool changed = false;
  for (uint32_t w = 0; w < words_.size(); ++w) {
    const uint32_t before = words_[w];
    words_[w] &= ~(delete_mask.WordAt(w));
    words_[w] &= WordMask(w);
    changed = changed || before != words_[w];
  }
  return changed;
}

std::vector<uint32_t> DomainMask::WordsHex() const {
  std::vector<uint32_t> words;
  words.reserve(words_.size());
  for (uint32_t w = 0; w < words_.size(); ++w) {
    words.push_back(WordAt(w));
  }
  return words;
}

bool DomainMask::operator==(const DomainMask& rhs) const {
  if (nbits_ != rhs.nbits_ || words_.size() != rhs.words_.size()) {
    return false;
  }
  for (uint32_t w = 0; w < words_.size(); ++w) {
    if (WordAt(w) != rhs.WordAt(w)) {
      return false;
    }
  }
  return true;
}

void DomainMask::MaskInvalidHighBits() {
  if (!words_.empty()) {
    words_.back() &= LastWordMask(nbits_);
  }
}

uint32_t DomainMask::WordMask(uint32_t w) const {
  if (w + 1 != words_.size()) {
    return 0xffffffffu;
  }
  return LastWordMask(nbits_);
}

std::string HexWord(uint32_t word) {
  std::ostringstream os;
  os << "0x" << std::hex << std::setw(8) << std::setfill('0') << word;
  return os.str();
}

}  // namespace fpga_cpim
