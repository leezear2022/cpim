#include "test_util.hpp"

#include "fpga_cpim/engine.hpp"

using namespace fpga_cpim;

int main() {
  Model model = MakeLessThanModel(4);
  auto domains = fpga_cpim_test::FullDomains(model);

  {
    EngineConfig cfg;
    cfg.router.max_pending_events_per_world = 0;
    PropagationEngine engine(model, cfg);
    WorldResult result = engine.RunProbe(domains, 0, 3);
    CHECK_EQ(result.status, WorldStatus::kUNKNOWN);
    CHECK_EQ(result.deleted_values, 0u);
  }

  {
    EngineConfig cfg;
    cfg.max_epochs_per_probe = 0;
    PropagationEngine engine(model, cfg);
    WorldResult result = engine.RunProbe(domains, 0, 3);
    CHECK_EQ(result.status, WorldStatus::kUNKNOWN);
    CHECK_EQ(result.deleted_values, 0u);
  }

  {
    EngineConfig cfg;
    cfg.max_events_per_probe = 0;
    PropagationEngine engine(model, cfg);
    WorldResult result = engine.RunProbe(domains, 0, 3);
    CHECK_EQ(result.status, WorldStatus::kUNKNOWN);
    CHECK_EQ(result.deleted_values, 0u);
  }

  {
    EngineConfig cfg;
    cfg.revise_tile.max_values_per_revise = 0;
    PropagationEngine engine(model, cfg);
    WorldResult result = engine.RunProbe(domains, 0, 3);
    CHECK_EQ(result.status, WorldStatus::kUNKNOWN);
    CHECK_EQ(result.deleted_values, 0u);
  }
  return 0;
}
