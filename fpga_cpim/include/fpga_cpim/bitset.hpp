#ifndef FPGA_CPIM_BITSET_HPP_
#define FPGA_CPIM_BITSET_HPP_

#include <cstdint>
#include <string>
#include <vector>

namespace fpga_cpim {

class DomainMask {
 public:
  DomainMask();
  explicit DomainMask(uint32_t nbits);

  void Resize(uint32_t nbits);
  void Clear();
  void SetAllValid(uint32_t domain_size);
  void SetSingleton(uint32_t value);
  void Set(uint32_t value);
  bool Test(uint32_t value) const;
  bool Empty() const;
  uint32_t Count() const;
  uint32_t WordCount() const;
  uint32_t NBits() const { return nbits_; }
  uint32_t WordAt(uint32_t w) const;
  uint32_t& MutableWordAt(uint32_t w);

  // Monotonic deletion. Returns the number of actually removed bits.
  uint32_t ApplyDeleteMaskWord(uint32_t w, uint32_t delete_mask);
  bool AndNot(const DomainMask& delete_mask);

  std::vector<uint32_t> WordsHex() const;
  bool operator==(const DomainMask& rhs) const;
  bool operator!=(const DomainMask& rhs) const { return !(*this == rhs); }

 private:
  void MaskInvalidHighBits();
  uint32_t WordMask(uint32_t w) const;

  uint32_t nbits_ = 0;
  std::vector<uint32_t> words_;
};

std::string HexWord(uint32_t word);

}  // namespace fpga_cpim

#endif  // FPGA_CPIM_BITSET_HPP_
