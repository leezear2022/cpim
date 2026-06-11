#include "test_util.hpp"

#include "fpga_cpim/event_router.hpp"

using namespace fpga_cpim;

int main() {
  Model model = MakeEqualityModel(4);
  EventRouter router(model, RouterConfig{});
  CHECK_TRUE(router.Seed(0, 0));
  auto ev = router.Pop();
  CHECK_TRUE(ev.has_value());
  CHECK_EQ(ev->cid, 0u);
  CHECK_EQ(ev->pending_dirs, 0x2u);
  CHECK_TRUE(router.Empty());

  CHECK_TRUE(router.EnqueueConstraint(0, 0, 0x1u));
  CHECK_TRUE(router.EnqueueConstraint(0, 0, 0x2u));
  CHECK_EQ(router.Stats().events_deduped, 1u);
  ev = router.Pop();
  CHECK_TRUE(ev.has_value());
  CHECK_EQ(ev->pending_dirs, 0x3u);

  DomainMask delta(4);
  delta.Set(1);
  CHECK_TRUE(router.EnqueueFromDelta(0, 1, delta));
  ev = router.Pop();
  CHECK_TRUE(ev.has_value());
  CHECK_EQ(ev->pending_dirs, 0x1u);

  RouterConfig tiny;
  tiny.max_pending_events_per_world = 0;
  EventRouter overflow(model, tiny);
  CHECK_FALSE(overflow.EnqueueConstraint(0, 0, 0x1u));
  CHECK_TRUE(overflow.HasOverflow());
  CHECK_TRUE(overflow.WorldOverflow(0));

  Model hub = MakeSyntheticModel(SyntheticConfig{"hub", 8, 4, 0.0, 0.5, 0, 7});
  EventRouter hub_router(hub, RouterConfig{});
  CHECK_TRUE(hub_router.Seed(0, 0));
  CHECK_EQ(hub_router.PendingEventCount(), size_t{7});
  return 0;
}
