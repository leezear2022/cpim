#include "fpga_cpim/event_router.hpp"

#include <algorithm>
#include <cassert>

namespace fpga_cpim {

EventRouter::EventRouter(const Model& model, RouterConfig cfg)
    : model_(model), cfg_(cfg) {}

bool EventRouter::Seed(WorldId world, VarId assigned_var) {
  assert(assigned_var < model_.subscription.size());
  bool ok = true;
  for (Cid cid : model_.subscription[assigned_var]) {
    ok = EnqueueConstraint(world, cid, DirsForChangedVar(cid, assigned_var)) && ok;
  }
  return ok;
}

bool EventRouter::EnqueueConstraint(WorldId world, Cid cid, uint8_t dirs) {
  if (dirs == 0) {
    return true;
  }
  assert(cid < model_.constraints.size());
  if (world >= overflow_worlds_.size()) {
    overflow_worlds_.resize(world + 1, false);
  }
  const uint64_t key = Key(world, cid);
  if (cfg_.dedup) {
    auto it = pending_dirs_.find(key);
    if (it != pending_dirs_.end()) {
      it->second |= dirs;
      ++stats_.events_deduped;
      return true;
    }
  }

  if (pending_per_world_[world] >= cfg_.max_pending_events_per_world ||
      pending_per_world_cid_[key] >= cfg_.max_pending_per_cid) {
    MarkOverflow(world);
    ++stats_.events_dropped_overflow;
    return cfg_.overflow_policy == OverflowPolicy::kDropButMarkUnknown;
  }

  queue_.push_back(key);
  pending_dirs_[key] = dirs;
  ++pending_per_world_[world];
  ++pending_per_world_cid_[key];
  ++stats_.events_enqueued;
  stats_.max_queue_occupancy =
      std::max<uint64_t>(stats_.max_queue_occupancy, queue_.size());
  stats_.occupancy_samples.push_back(queue_.size());
  return true;
}

bool EventRouter::EnqueueFromDelta(WorldId world, VarId var,
                                   const DomainMask& delta) {
  if (delta.Empty()) {
    return true;
  }
  bool ok = true;
  for (Cid cid : model_.subscription[var]) {
    ok = EnqueueConstraint(world, cid, DirsForChangedVar(cid, var)) && ok;
  }
  return ok;
}

std::optional<DirtyEvent> EventRouter::Pop() {
  while (!queue_.empty()) {
    const uint64_t key = queue_.front();
    queue_.pop_front();
    auto it = pending_dirs_.find(key);
    if (it == pending_dirs_.end()) {
      continue;
    }
    const WorldId world = static_cast<WorldId>(key >> 32);
    const Cid cid = static_cast<Cid>(key & 0xffffffffu);
    const uint8_t dirs = it->second;
    pending_dirs_.erase(it);
    if (pending_per_world_[world] > 0) {
      --pending_per_world_[world];
    }
    if (pending_per_world_cid_[key] > 0) {
      --pending_per_world_cid_[key];
    }
    return DirtyEvent{world, cid, dirs, epoch_++};
  }
  return std::nullopt;
}

bool EventRouter::Empty() const {
  return pending_dirs_.empty();
}

size_t EventRouter::PendingEventCount() const {
  return pending_dirs_.size();
}

bool EventRouter::WorldOverflow(WorldId world) const {
  return world < overflow_worlds_.size() && overflow_worlds_[world];
}

uint64_t EventRouter::Key(WorldId world, Cid cid) const {
  return (static_cast<uint64_t>(world) << 32) | cid;
}

void EventRouter::MarkOverflow(WorldId world) {
  has_overflow_ = true;
  if (world >= overflow_worlds_.size()) {
    overflow_worlds_.resize(world + 1, false);
  }
  overflow_worlds_[world] = true;
}

uint8_t EventRouter::DirsForChangedVar(Cid cid, VarId var) const {
  const BinaryConstraint& c = model_.constraints[cid];
  if (c.x == var) {
    return 0x2u;  // x changed, revise y from x.
  }
  if (c.y == var) {
    return 0x1u;  // y changed, revise x from y.
  }
  return 0;
}

}  // namespace fpga_cpim
