#include "test_util.hpp"

#include "fpga_cpim/event_router.hpp"
#include "fpga_cpim/termination.hpp"

using namespace fpga_cpim;

int main() {
  Model model = MakeEqualityModel(4);
  EventRouter router(model, RouterConfig{});
  TerminationDetector detector;
  CHECK_TRUE(detector.FixedPoint(router));

  detector.OnEventEnqueue();
  CHECK_FALSE(detector.FixedPoint(router));
  detector.OnEventDequeue();
  detector.OnTileStart();
  CHECK_FALSE(detector.FixedPoint(router));
  detector.OnTileDone();
  detector.OnDeleteProduced();
  CHECK_FALSE(detector.FixedPoint(router));
  detector.OnDeleteApplied();
  CHECK_TRUE(detector.FixedPoint(router));

  router.EnqueueConstraint(0, 0, 0x1u);
  CHECK_FALSE(detector.FixedPoint(router));
  router.Pop();
  CHECK_TRUE(detector.FixedPoint(router));
  return 0;
}
