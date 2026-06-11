#ifndef FPGA_CPIM_TEST_UTIL_HPP_
#define FPGA_CPIM_TEST_UTIL_HPP_

#include <cstdlib>
#include <iostream>
#include <vector>

#include "fpga_cpim/bitset.hpp"
#include "fpga_cpim/model.hpp"

#define CHECK_TRUE(expr) fpga_cpim_test::Check((expr), #expr, __FILE__, __LINE__)
#define CHECK_FALSE(expr) fpga_cpim_test::Check(!(expr), "!(" #expr ")", __FILE__, __LINE__)
#define CHECK_EQ(a, b) fpga_cpim_test::CheckEq((a), (b), #a, #b, __FILE__, __LINE__)

namespace fpga_cpim_test {

inline void Check(bool ok, const char* expr, const char* file, int line) {
  if (!ok) {
    std::cerr << file << ":" << line << " CHECK failed: " << expr << "\n";
    std::exit(1);
  }
}

template <typename A, typename B>
void CheckEq(const A& a, const B& b, const char* a_expr, const char* b_expr,
             const char* file, int line) {
  if (!(a == b)) {
    std::cerr << file << ":" << line << " CHECK_EQ failed: " << a_expr
              << " != " << b_expr << "\n";
    std::exit(1);
  }
}

inline std::vector<fpga_cpim::DomainMask> FullDomains(
    const fpga_cpim::Model& model) {
  std::vector<fpga_cpim::DomainMask> domains;
  for (uint32_t size : model.domain_size) {
    fpga_cpim::DomainMask mask(size);
    mask.SetAllValid(size);
    domains.push_back(mask);
  }
  return domains;
}

inline std::vector<fpga_cpim::Cid> AllConstraints(
    const fpga_cpim::Model& model) {
  std::vector<fpga_cpim::Cid> cids;
  for (fpga_cpim::Cid cid = 0; cid < model.num_constraints; ++cid) {
    cids.push_back(cid);
  }
  return cids;
}

}  // namespace fpga_cpim_test

#endif  // FPGA_CPIM_TEST_UTIL_HPP_
