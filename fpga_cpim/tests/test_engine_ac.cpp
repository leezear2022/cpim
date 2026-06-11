#include "test_util.hpp"

#include "fpga_cpim/engine.hpp"
#include "fpga_cpim/golden.hpp"

using namespace fpga_cpim;

int main() {
  {
    Model model = MakeLessThanModel(4);
    auto domains = fpga_cpim_test::FullDomains(model);
    GoldenResult golden =
        EnforceAC_Golden(model, domains, fpga_cpim_test::AllConstraints(model));
    PropagationEngine engine(model, EngineConfig{});
    WorldResult sim =
        engine.RunAC(domains, fpga_cpim_test::AllConstraints(model));
    CHECK_EQ(static_cast<int>(golden.status == PropStatus::kDWO),
             static_cast<int>(sim.status == WorldStatus::kDWO));
    CHECK_EQ(golden.deleted_values, sim.deleted_values);
    CHECK_EQ(sim.status, WorldStatus::kOK);
  }

  for (uint32_t seed = 1; seed <= 5; ++seed) {
    SyntheticConfig cfg;
    cfg.vars = 6;
    cfg.domain = 8;
    cfg.density = 0.4;
    cfg.tightness = 0.4;
    cfg.seed = seed;
    Model model = MakeSyntheticModel(cfg);
    auto domains = fpga_cpim_test::FullDomains(model);
    GoldenResult golden =
        EnforceAC_Golden(model, domains, fpga_cpim_test::AllConstraints(model));
    PropagationEngine engine(model, EngineConfig{});
    WorldResult sim =
        engine.RunAC(domains, fpga_cpim_test::AllConstraints(model));
    CHECK_FALSE(sim.status == WorldStatus::kUNKNOWN);
    CHECK_EQ(static_cast<int>(golden.status == PropStatus::kDWO),
             static_cast<int>(sim.status == WorldStatus::kDWO));
  }
  return 0;
}
