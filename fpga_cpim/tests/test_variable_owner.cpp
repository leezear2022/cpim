#include "test_util.hpp"

#include "fpga_cpim/variable_owner.hpp"

using namespace fpga_cpim;

int main() {
  Model model = MakeEqualityModel(4);
  VariableOwner owner(model, 1);

  DomainMask del1(4);
  del1.Set(1);
  OwnerApplyResult r1 = owner.ApplyDeletion(0, 0, del1);
  CHECK_TRUE(r1.changed);
  CHECK_FALSE(r1.dwo);
  CHECK_EQ(r1.deleted_count, 1u);
  CHECK_TRUE(r1.delta_mask.Test(1));

  DomainMask del2(4);
  del2.Set(1);
  del2.Set(2);
  OwnerApplyResult r2 = owner.ApplyDeletion(0, 0, del2);
  CHECK_TRUE(r2.changed);
  CHECK_EQ(r2.deleted_count, 1u);
  CHECK_FALSE(r2.delta_mask.Test(1));
  CHECK_TRUE(r2.delta_mask.Test(2));

  DomainMask del3(4);
  del3.Set(0);
  del3.Set(3);
  OwnerApplyResult r3 = owner.ApplyDeletion(0, 0, del3);
  CHECK_TRUE(r3.dwo);
  CHECK_EQ(r3.deleted_count, 2u);
  return 0;
}
