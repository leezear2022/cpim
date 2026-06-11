#include "test_util.hpp"

#include "fpga_cpim/engine.hpp"
#include "fpga_cpim/golden.hpp"

using namespace fpga_cpim;

int main() {
  {
    Model model = MakeEqualityModel(4);
    auto domains = fpga_cpim_test::FullDomains(model);
    PropagationEngine engine(model, EngineConfig{});
    WorldResult sim = engine.RunProbe(domains, 0, 2);
    GoldenResult golden = RunSingletonProbe_Golden(model, domains, 0, 2);
    CHECK_EQ(sim.status, WorldStatus::kOK);
    CHECK_EQ(golden.status, PropStatus::kOK);
  }

  {
    Model model = MakeLessThanModel(4);
    auto domains = fpga_cpim_test::FullDomains(model);
    PropagationEngine engine(model, EngineConfig{});
    WorldResult sim = engine.RunProbe(domains, 0, 3);
    GoldenResult golden = RunSingletonProbe_Golden(model, domains, 0, 3);
    CHECK_EQ(sim.status, WorldStatus::kDWO);
    CHECK_EQ(golden.status, PropStatus::kDWO);
  }

  {
    Model model = MakeLessThanModel(4);
    auto domains = fpga_cpim_test::FullDomains(model);
    EngineConfig cfg;
    cfg.max_events_per_probe = 0;
    PropagationEngine engine(model, cfg);
    WorldResult sim = engine.RunProbe(domains, 0, 3);
    CHECK_EQ(sim.status, WorldStatus::kUNKNOWN);
    CHECK_EQ(sim.deleted_values, 0u);
  }
  return 0;
}
