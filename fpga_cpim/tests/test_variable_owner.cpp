#include "test_util.hpp"

#include "fpga_cpim/event_router.hpp"
#include "fpga_cpim/variable_owner.hpp"

using namespace fpga_cpim;

namespace {

Model MakeThreeVarFanoutModel() {
  Model model = MakeEmptyModel({4, 4, 4});
  std::vector<std::pair<Value, Value>> eq;
  for (Value value = 0; value < 4; ++value) {
    eq.push_back({value, value});
  }
  AddBinaryConstraint(&model, 0, 1, eq);
  AddBinaryConstraint(&model, 0, 2, eq);
  return model;
}

DomainMask DeleteMask(uint32_t domain_size, std::initializer_list<Value> values) {
  DomainMask mask(domain_size);
  for (Value value : values) {
    mask.Set(value);
  }
  return mask;
}

}  // namespace

int main() {
  Model model = MakeEqualityModel(4);
  VariableOwner owner(model, 1);

  DomainMask del1 = DeleteMask(4, {1});
  OwnerApplyResult r1 = owner.ApplyDeletion(0, 0, del1);
  CHECK_TRUE(r1.changed);
  CHECK_FALSE(r1.dwo);
  CHECK_EQ(r1.deleted_count, 1u);
  CHECK_TRUE(r1.delta_mask.Test(1));

  DomainMask del2 = DeleteMask(4, {1, 2});
  OwnerApplyResult r2 = owner.ApplyDeletion(0, 0, del2);
  CHECK_TRUE(r2.changed);
  CHECK_EQ(r2.deleted_count, 1u);
  CHECK_FALSE(r2.delta_mask.Test(1));
  CHECK_TRUE(r2.delta_mask.Test(2));

  DomainMask del3 = DeleteMask(4, {0, 3});
  OwnerApplyResult r3 = owner.ApplyDeletion(0, 0, del3);
  CHECK_TRUE(r3.dwo);
  CHECK_EQ(r3.deleted_count, 2u);

  OwnerApplyResult r4 = owner.ApplyDeletion(0, 0, del3);
  CHECK_FALSE(r4.changed);
  CHECK_FALSE(r4.dwo);

  VariableOwner batch_owner(model, 1);
  OwnerApplyResult disjoint = batch_owner.ApplyDeletionBatch(
      0, 0, {DeleteMask(4, {1}), DeleteMask(4, {2})});
  CHECK_TRUE(disjoint.changed);
  CHECK_FALSE(disjoint.dwo);
  CHECK_EQ(disjoint.deleted_count, 2u);
  CHECK_TRUE(disjoint.delta_mask.Test(1));
  CHECK_TRUE(disjoint.delta_mask.Test(2));

  VariableOwner overlap_owner(model, 1);
  OwnerApplyResult overlap = overlap_owner.ApplyDeletionBatch(
      0, 0, {DeleteMask(4, {1, 2}), DeleteMask(4, {2, 3})});
  CHECK_TRUE(overlap.changed);
  CHECK_EQ(overlap.deleted_count, 3u);
  CHECK_TRUE(overlap.delta_mask.Test(1));
  CHECK_TRUE(overlap.delta_mask.Test(2));
  CHECK_TRUE(overlap.delta_mask.Test(3));

  batch_owner.BeginProcessing(0, 0);
  CHECK_TRUE(batch_owner.State(0, 0) == OwnerVarState::kProcessing);
  CHECK_TRUE(batch_owner.NoteRerunRequest(0, 0));
  CHECK_TRUE(batch_owner.State(0, 0) == OwnerVarState::kRerunPending);
  CHECK_TRUE(batch_owner.EndProcessing(0, 0));
  CHECK_TRUE(batch_owner.State(0, 0) == OwnerVarState::kIdle);

  Model fanout_model = MakeThreeVarFanoutModel();
  VariableOwner fanout_owner(fanout_model, 1);
  OwnerApplyResult fanout_delta = fanout_owner.ApplyDeletionBatch(
      0, 0, {DeleteMask(4, {1}), DeleteMask(4, {2})});
  std::vector<Cid> fanout =
      fanout_owner.FanoutForDelta(0, fanout_delta.delta_mask);
  CHECK_EQ(fanout.size(), 2u);

  EventRouter router(fanout_model, RouterConfig{});
  CHECK_TRUE(router.EnqueueFromDelta(0, 0, fanout_delta.delta_mask));
  CHECK_TRUE(router.EnqueueFromDelta(0, 0, fanout_delta.delta_mask));
  CHECK_EQ(router.Stats().events_enqueued, 2u);
  CHECK_EQ(router.Stats().events_deduped, 2u);
  CHECK_EQ(router.PendingEventCount(), 2u);
  return 0;
}
