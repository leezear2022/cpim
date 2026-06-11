#include "test_util.hpp"

#include "fpga_cpim/engine.hpp"
#include "fpga_cpim/golden.hpp"

using namespace fpga_cpim;

int main() {
  Model model = MakeEmptyModel({4, 4, 4});
  std::vector<std::pair<Value, Value>> eq;
  for (Value v = 0; v < 4; ++v) {
    eq.push_back({v, v});
  }
  AddBinaryConstraint(&model, 0, 1, eq);
  std::vector<std::pair<Value, Value>> lt;
  for (Value x = 0; x < 4; ++x) {
    for (Value y = 0; y < 4; ++y) {
      if (x < y) {
        lt.push_back({x, y});
      }
    }
  }
  AddBinaryConstraint(&model, 1, 2, lt);
  auto domains = fpga_cpim_test::FullDomains(model);

  EngineConfig full_cfg;
  PropagationEngine full_engine(model, full_cfg);
  WorldResult full = full_engine.RunProbe(domains, 0, 3);
  CHECK_EQ(full.status, WorldStatus::kDWO);

  EngineConfig nsac_cfg;
  nsac_cfg.nsac_enabled = true;
  nsac_cfg.nsac_radius = 1;
  PropagationEngine nsac_engine(model, nsac_cfg);
  WorldResult nsac = nsac_engine.RunProbe(domains, 0, 3);
  GoldenResult nsac_golden = RunSingletonProbe_Golden(model, domains, 0, 3, {0});
  CHECK_EQ(nsac.status, WorldStatus::kOK);
  CHECK_EQ(nsac_golden.status, PropStatus::kOK);
  CHECK_FALSE(nsac.status == WorldStatus::kDWO &&
              nsac_golden.status != PropStatus::kDWO);
  return 0;
}
