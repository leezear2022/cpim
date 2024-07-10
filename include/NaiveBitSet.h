//
// Created by lee on 24-7-9.
//

#ifndef NAIVEBITSET_H
#define NAIVEBITSET_H
#include <climits>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "CPIMBase.h"

namespace cpim {
class NaiveBitSet {
 public:
  NaiveBitSet() = default;
  explicit NaiveBitSet(int nbits)
      : bitSize_(nbits),
        intSize_((nbits + BITS_PER_WORD - 1) / BITS_PER_WORD),
        limit_(nbits % BITS_PER_WORD),
        lastMask_(UINT_MAX >> (BITS_PER_WORD - limit_)) {
    words_.resize(intSize_);
  }

  void resize(int nbits) {
    bitSize_ = nbits;
    intSize_ = (nbits + BITS_PER_WORD - 1) / BITS_PER_WORD;
    limit_ = nbits % BITS_PER_WORD;
    lastMask_ = UINT_MAX >> (BITS_PER_WORD - limit_);
    words_.resize(intSize_);
  }

  [[nodiscard]] int intSize() const { return intSize_; }

  [[nodiscard]] int bitSize() const { return bitSize_; }

  void flip() {
    for (int i = 0; i < intSize_; ++i) {
      words_[i] = ~words_[i];
    }
    words_[intSize_ - 1] &= lastMask_;
  }

  void set(int bitIndex) {
    int idx = wordIndex(bitIndex);
    int offset = wordOffset(bitIndex);
    words_[idx] |= 1U << offset;
  }

  void set(const NaiveBitSet& s) {
    for (int i = 0; i < intSize_; ++i) {
      words_[i] = s.words_[i];
    }
  }

  void clear(int bitIndex) {
    words_[wordIndex(bitIndex)] &= ~(1U << wordOffset(bitIndex));
  }

  void clear() { std::fill(words_.begin(), words_.end(), 0U); }

  void singleton(int bitIndex) {
    if (bitIndex >= bitSize_ || bitIndex < 0) {
      throw std::out_of_range("bitIndex out of range");
    }
    // Clear all bits
    clear();
    // Set the specific bit
    int idx = wordIndex(bitIndex);
    int offset = wordOffset(bitIndex);
    words_[idx] = 1U << offset;
  }

  [[nodiscard]] int count() const {
    int total = 0;
    for (int i = 0; i < intSize_; ++i) {
      total += __builtin_popcount(words_[i]);
    }
    return total;
  }

  [[nodiscard]] bool empty() const {
    for (int i = 0; i < intSize_; ++i) {
      if (words_[i] != 0U) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] bool check(int bitIndex) const {
    int index = wordIndex(bitIndex);
    return index < intSize_ &&
           (words_[index] & 1U << wordOffset(bitIndex)) != 0U;
  }

  void andOp(const NaiveBitSet& s) {
    for (int i = 0; i < intSize_; ++i) {
      words_[i] &= s.words_[i];
    }
  }

  void orOp(const NaiveBitSet& s) {
    for (int i = 0; i < intSize_; ++i) {
      words_[i] |= s.words_[i];
    }
  }

  [[nodiscard]] int nextOneBit(int fromIndex) const {
    if (fromIndex < 0) {
      throw std::out_of_range("fromIndex < 0: " + std::to_string(fromIndex));
    }
    int u = wordIndex(fromIndex);
    if (u >= intSize_) {
      return -1;
    }
    unsigned int word = words_[u] & (~0U << fromIndex);
    while (word == 0) {
      ++u;
      if (u == intSize_) {
        return -1;
      }
      word = words_[u];
    }
    return u * BITS_PER_WORD + __builtin_ctz(word);
  }

  [[nodiscard]] int nextZeroBit(int fromIndex) const {
    if (fromIndex < 0) {
      throw std::out_of_range("fromIndex < 0: " + std::to_string(fromIndex));
    }
    int u = wordIndex(fromIndex);
    if (u >= intSize_) {
      return fromIndex;
    }
    unsigned int word = ~words_[u] & (~0U << fromIndex);
    while (true) {
      if (word != 0) {
        return (u * BITS_PER_WORD) + __builtin_ctz(word);
      }
      if (++u == intSize_) {
        return intSize_ * BITS_PER_WORD;
      }
      word = ~words_[u];
    }
  }

  [[nodiscard]] int prevOneBit(int fromIndex) const {
    if (fromIndex < 0 || fromIndex >= bitSize_) {
      throw std::out_of_range("fromIndex out of range: " +
                              std::to_string(fromIndex));
    }
    int u = wordIndex(fromIndex);
    if (u >= intSize_) {
      return -1;
    }
    unsigned int word = words_[u] & ((1U << (wordOffset(fromIndex) + 1)) - 1);
    while (word == 0) {
      if (u == 0) {
        return -1;
      }
      word = words_[--u];
    }
    return u * BITS_PER_WORD + (BITS_PER_WORD - 1 - __builtin_clz(word));
  }

  [[nodiscard]] int prevZeroBit(int fromIndex) const {
    if (fromIndex < 0 || fromIndex >= bitSize_) {
      throw std::out_of_range("fromIndex out of range: " +
                              std::to_string(fromIndex));
    }
    int u = wordIndex(fromIndex);
    if (u >= intSize_) {
      return fromIndex;
    }
    unsigned int word = ~words_[u] & ((1U << (wordOffset(fromIndex) + 1)) - 1);
    while (word == 0) {
      if (u == 0) {
        return -1;
      }
      word = ~words_[--u];
    }
    return u * BITS_PER_WORD + (BITS_PER_WORD - 1 - __builtin_clz(word));
  }

  // Operator overloads
  NaiveBitSet operator&(const NaiveBitSet& other) const {
    NaiveBitSet result(bitSize_);
    for (int i = 0; i < intSize_; ++i) {
      result.words_[i] = words_[i] & other.words_[i];
    }
    return result;
  }

  NaiveBitSet& operator&=(const NaiveBitSet& other) {
    for (int i = 0; i < intSize_; ++i) {
      words_[i] &= other.words_[i];
    }
    return *this;
  }

  NaiveBitSet operator|(const NaiveBitSet& other) const {
    NaiveBitSet result(bitSize_);
    for (int i = 0; i < intSize_; ++i) {
      result.words_[i] = words_[i] | other.words_[i];
    }
    return result;
  }

  NaiveBitSet& operator|=(const NaiveBitSet& other) {
    for (int i = 0; i < intSize_; ++i) {
      words_[i] |= other.words_[i];
    }
    return *this;
  }

  NaiveBitSet operator^(const NaiveBitSet& other) const {
    NaiveBitSet result(bitSize_);
    for (int i = 0; i < intSize_; ++i) {
      result.words_[i] = words_[i] ^ other.words_[i];
    }
    return result;
  }

  NaiveBitSet& operator^=(const NaiveBitSet& other) {
    for (int i = 0; i < intSize_; ++i) {
      words_[i] ^= other.words_[i];
    }
    return *this;
  }

  NaiveBitSet operator~() const {
    NaiveBitSet result(bitSize_);
    for (int i = 0; i < intSize_; ++i) {
      result.words_[i] = ~words_[i];
    }
    result.words_[intSize_ - 1] &= lastMask_;
    return result;
  }

  [[nodiscard]] std::string toString() const {
    std::ostringstream oss;
    oss << "{";
    int i = nextOneBit(0);
    if (i != -1) {
      oss << i;
      while (true) {
        if (++i < 0) break;
        if ((i = nextOneBit(i)) < 0) break;
        int endOfRun = nextZeroBit(i);
        do {
          oss << ", " << i;
        } while (++i != endOfRun);
      }
    }
    oss << "}";
    return oss.str();
  }

 private:
  std::vector<u32> words_;
  int bitSize_;
  int intSize_;
  int limit_;
  unsigned int lastMask_;

  static constexpr int ADDRESS_BITS_PER_WORD = 5;
  static constexpr int BITS_PER_WORD = 1 << ADDRESS_BITS_PER_WORD;
  static constexpr int BIT_INDEX_MASK = BITS_PER_WORD - 1;

  static int wordIndex(int bitIndex) {
    return bitIndex >> ADDRESS_BITS_PER_WORD;
  }

  static int wordOffset(int bitIndex) { return bitIndex & BIT_INDEX_MASK; }
};
}  // namespace cpim

#endif  // NAIVEBITSET_H
