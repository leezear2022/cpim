#include "test_util.hpp"

#include "fpga_cpim/support_oracle.hpp"

using namespace fpga_cpim;

int main() {
  {
    Model model = MakeEqualityModel(4);
    auto domains = fpga_cpim_test::FullDomains(model);
    domains[1].SetSingleton(2);
    SupportOracle oracle(model);
    SupportResult hit = oracle.Exists(SupportQuery{0, 0, 2, 0}, domains);
    SupportResult miss = oracle.Exists(SupportQuery{0, 0, 1, 0}, domains);
    CHECK_TRUE(hit.support_exists);
    CHECK_FALSE(miss.support_exists);
    CHECK_EQ(hit.words_touched, 1u);
  }

  {
    Model model = MakeEmptyModel({40, 40});
    AddBinaryConstraint(&model, 0, 1, {{0, 35}});
    auto domains = fpga_cpim_test::FullDomains(model);
    SupportOracle oracle(model);
    SupportResult later = oracle.Exists(SupportQuery{0, 0, 0, 0}, domains);
    CHECK_TRUE(later.support_exists);
    CHECK_EQ(later.words_touched, 2u);

    domains[1].Clear();
    SupportResult zero = oracle.Exists(SupportQuery{0, 0, 0, 0}, domains);
    CHECK_FALSE(zero.support_exists);
    CHECK_EQ(zero.words_touched, 0u);
  }
  return 0;
}
