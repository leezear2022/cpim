#include "test_util.hpp"

using namespace fpga_cpim;

int main() {
  DomainMask mask(35);
  mask.SetAllValid(35);
  CHECK_EQ(mask.Count(), 35u);
  CHECK_EQ(mask.WordCount(), 2u);
  CHECK_EQ(mask.WordAt(1), 0x7u);

  mask.SetSingleton(34);
  CHECK_TRUE(mask.Test(34));
  CHECK_FALSE(mask.Test(33));
  CHECK_EQ(mask.Count(), 1u);

  mask.SetAllValid(35);
  CHECK_EQ(mask.ApplyDeleteMaskWord(0, 0x3u), 2u);
  CHECK_EQ(mask.Count(), 33u);
  CHECK_EQ(mask.ApplyDeleteMaskWord(1, 0xfffffff8u), 0u);
  CHECK_EQ(mask.WordAt(1), 0x7u);

  DomainMask del(35);
  del.Set(34);
  CHECK_TRUE(mask.AndNot(del));
  CHECK_FALSE(mask.Test(34));
  CHECK_EQ(mask.Count(), 32u);

  DomainMask same(35);
  same.SetAllValid(35);
  same.ApplyDeleteMaskWord(0, 0x3u);
  same.AndNot(del);
  CHECK_TRUE(mask == same);
  return 0;
}
