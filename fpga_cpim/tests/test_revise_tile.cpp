#include "test_util.hpp"

#include "fpga_cpim/revise_tile.hpp"
#include "fpga_cpim/support_oracle.hpp"

using namespace fpga_cpim;

int main() {
  Model model = MakeLessThanModel(4);
  auto domains = fpga_cpim_test::FullDomains(model);
  domains[1].SetSingleton(2);
  const auto original = domains;

  SupportOracle oracle(model);
  ReviseTile tile(model, oracle);
  std::vector<ReviseOutput> out =
      tile.Process(DirtyEvent{0, 0, 0x1u, 0}, domains);
  CHECK_EQ(out.size(), size_t{1});
  CHECK_EQ(out[0].target_var, 0u);
  CHECK_FALSE(out[0].delete_mask.Test(0));
  CHECK_FALSE(out[0].delete_mask.Test(1));
  CHECK_TRUE(out[0].delete_mask.Test(2));
  CHECK_TRUE(out[0].delete_mask.Test(3));
  CHECK_FALSE(out[0].dwo);
  CHECK_TRUE(domains[0] == original[0]);

  std::vector<ReviseOutput> out_y =
      tile.Process(DirtyEvent{0, 0, 0x2u, 0}, domains);
  CHECK_EQ(out_y.size(), size_t{1});
  CHECK_EQ(out_y[0].target_var, 1u);
  CHECK_TRUE(out_y[0].delete_mask.Empty());
  return 0;
}
