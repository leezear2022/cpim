#include <metal_stdlib>

using namespace metal;

struct DeviceInt2 {
  int x;
  int y;
};

struct DeviceUInt2 {
  uint x;
  uint y;
};

struct DeviceUInt3 {
  uint x;
  uint y;
  uint z;
};

struct GacParams {
  int num_constraints;
  int max_dom_size;
  int bit_words;
  int num_vars;
  int active_count;
  int frontier_epoch;
  int cta_count;
  int cta_queue_capacity;
  int cta_local_round_budget;
  int cta_replay_round_budget;
  int threads_per_cta;
  int cta_queue_mode;
  int cta_handoff_mode;
  int cta_dirty_pull_min_degree;
};

struct CompactParams {
  int num_constraints;
};

inline void enqueue_next_flag(int target,
                              device const int* sub_offsets,
                              device const DeviceUInt3* sub_entries,
                              device atomic_int* next_flags,
                              device atomic_int* stats,
                              constant GacParams& params) {
  const int begin = sub_offsets[target];
  const int end = sub_offsets[target + 1];
  for (int i = begin; i < end; ++i) {
    const uint next_cid = sub_entries[i].z;
    if (next_cid >= static_cast<uint>(params.num_constraints)) {
      continue;
    }
    const int previous =
        atomic_exchange_explicit(&next_flags[next_cid], 1, memory_order_relaxed);
    if (previous == 0) {
      atomic_fetch_add_explicit(&stats[2], 1, memory_order_relaxed);
    }
  }
}

inline void enqueue_next_worklist(int target,
                                  device const int* sub_offsets,
                                  device const DeviceUInt3* sub_entries,
                                  device atomic_int* next_epochs,
                                  device int* next_active_constraints,
                                  device atomic_int* stats,
                                  constant GacParams& params) {
  const int begin = sub_offsets[target];
  const int end = sub_offsets[target + 1];
  for (int i = begin; i < end; ++i) {
    const uint next_cid = sub_entries[i].z;
    if (next_cid >= static_cast<uint>(params.num_constraints)) {
      continue;
    }
    const int previous = atomic_exchange_explicit(
        &next_epochs[next_cid], params.frontier_epoch, memory_order_relaxed);
    if (previous != params.frontier_epoch) {
      const int slot = atomic_fetch_add_explicit(
          &stats[2], 1, memory_order_relaxed);
      next_active_constraints[slot] = static_cast<int>(next_cid);
    }
  }
}

inline void enqueue_next_bulk_worklist(int target,
                                       device const int* sub_offsets,
                                       device const DeviceUInt3* sub_entries,
                                       device atomic_int* next_epochs,
                                       device int* next_active_constraints,
                                       device atomic_int* stats,
                                       constant GacParams& params) {
  const int begin = sub_offsets[target];
  const int end = sub_offsets[target + 1];
  for (int i = begin; i < end; ++i) {
    const uint next_cid = sub_entries[i].z;
    if (next_cid >= static_cast<uint>(params.num_constraints)) {
      continue;
    }
    const int previous = atomic_exchange_explicit(
        &next_epochs[next_cid], params.frontier_epoch, memory_order_relaxed);
    if (previous != params.frontier_epoch) {
      const int slot = atomic_fetch_add_explicit(
          &stats[2], 1, memory_order_relaxed);
      next_active_constraints[slot] = static_cast<int>(next_cid);
      atomic_fetch_add_explicit(&stats[17], 1, memory_order_relaxed);
    }
  }
}

inline void enqueue_global_constraint(int cid,
                                      device atomic_int* next_epochs,
                                      device atomic_int* stats,
                                      device int* next_active_constraints,
                                      constant GacParams& params) {
  if (cid < 0 || cid >= params.num_constraints) {
    return;
  }
  const int previous = atomic_exchange_explicit(
      &next_epochs[cid], params.frontier_epoch, memory_order_relaxed);
  if (previous == params.frontier_epoch) {
    return;
  }
  const int slot = atomic_fetch_add_explicit(&stats[2], 1, memory_order_relaxed);
  if (slot < params.num_constraints) {
    next_active_constraints[slot] = cid;
  } else {
    atomic_fetch_add_explicit(&stats[6], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&stats[9], 1, memory_order_relaxed);
  }
}

inline int cta_owner_for_constraint(int cid,
                                    device const int* owner_of_constraint,
                                    constant GacParams& params) {
  int owner = owner_of_constraint[cid];
  if (owner < 0 || owner >= params.cta_count) {
    owner = cid % params.cta_count;
  }
  return owner;
}

inline void enqueue_next_cta_or_global(int target,
                                       int cta_id,
                                       int stamp,
                                       device const int* sub_offsets,
                                       device const DeviceUInt3* sub_entries,
                                       device int* cta_next_queue,
                                       device atomic_int* cta_next_tail,
                                       device atomic_int* cta_stamps,
                                       device atomic_int* global_next_epochs,
                                       device atomic_int* stats,
                                       device int* global_next_active,
                                       device const int* owner_of_constraint,
                                       constant GacParams& params) {
  const int begin = sub_offsets[target];
  const int end = sub_offsets[target + 1];
  for (int i = begin; i < end; ++i) {
    const int next_cid = static_cast<int>(sub_entries[i].z);
    if (next_cid < 0 || next_cid >= params.num_constraints) {
      continue;
    }
    const int owner =
        cta_owner_for_constraint(next_cid, owner_of_constraint, params);
    if (owner != cta_id) {
      enqueue_global_constraint(next_cid, global_next_epochs, stats,
                                global_next_active, params);
      atomic_fetch_add_explicit(&stats[5], 1, memory_order_relaxed);
      atomic_fetch_add_explicit(&stats[8], 1, memory_order_relaxed);
      continue;
    }

    const int stamp_index = cta_id * params.cta_queue_capacity + next_cid;
    const int previous = atomic_exchange_explicit(
        &cta_stamps[stamp_index], stamp, memory_order_relaxed);
    if (previous == stamp) {
      continue;
    }
    const int slot = atomic_fetch_add_explicit(
        &cta_next_tail[cta_id], 1, memory_order_relaxed);
    if (slot < params.cta_queue_capacity) {
      cta_next_queue[cta_id * params.cta_queue_capacity + slot] = next_cid;
      atomic_fetch_add_explicit(&stats[4], 1, memory_order_relaxed);
      atomic_fetch_add_explicit(&stats[7], 1, memory_order_relaxed);
    } else {
      atomic_fetch_add_explicit(&stats[6], 1, memory_order_relaxed);
      atomic_fetch_add_explicit(&stats[9], 1, memory_order_relaxed);
      enqueue_global_constraint(next_cid, global_next_epochs, stats,
                                global_next_active, params);
    }
  }
}

inline void enqueue_next_cta_or_dirty_pull(int target,
                                           int cta_id,
                                           int stamp,
                                           device const int* sub_offsets,
                                           device const DeviceUInt3* sub_entries,
                                           device int* cta_next_queue,
                                           device atomic_int* cta_next_tail,
                                           device atomic_int* cta_stamps,
                                           device atomic_int* global_next_epochs,
                                           device atomic_int* stats,
                                           device int* global_next_active,
                                           device const int* owner_of_constraint,
                                           device atomic_int* dirty_var_epochs,
                                           constant GacParams& params) {
  const int begin = sub_offsets[target];
  const int end = sub_offsets[target + 1];
  for (int i = begin; i < end; ++i) {
    const int next_cid = static_cast<int>(sub_entries[i].z);
    if (next_cid < 0 || next_cid >= params.num_constraints) {
      continue;
    }
    const int owner =
        cta_owner_for_constraint(next_cid, owner_of_constraint, params);
    if (owner != cta_id) {
      if (params.cta_handoff_mode == 1 &&
          end - begin >= params.cta_dirty_pull_min_degree) {
        const int previous = atomic_exchange_explicit(
            &dirty_var_epochs[target], params.frontier_epoch,
            memory_order_relaxed);
        if (previous != params.frontier_epoch) {
          atomic_fetch_add_explicit(&stats[18], 1, memory_order_relaxed);
        }
        atomic_fetch_add_explicit(&stats[21], 1, memory_order_relaxed);
      } else {
        enqueue_global_constraint(next_cid, global_next_epochs, stats,
                                  global_next_active, params);
        atomic_fetch_add_explicit(&stats[5], 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&stats[8], 1, memory_order_relaxed);
        if (params.cta_handoff_mode == 1) {
          atomic_fetch_add_explicit(&stats[22], 1, memory_order_relaxed);
        }
      }
      continue;
    }

    const int stamp_index = cta_id * params.cta_queue_capacity + next_cid;
    const int previous = atomic_exchange_explicit(
        &cta_stamps[stamp_index], stamp, memory_order_relaxed);
    if (previous == stamp) {
      continue;
    }
    const int slot = atomic_fetch_add_explicit(
        &cta_next_tail[cta_id], 1, memory_order_relaxed);
    if (slot < params.cta_queue_capacity) {
      cta_next_queue[cta_id * params.cta_queue_capacity + slot] = next_cid;
      atomic_fetch_add_explicit(&stats[4], 1, memory_order_relaxed);
      atomic_fetch_add_explicit(&stats[7], 1, memory_order_relaxed);
    } else {
      atomic_fetch_add_explicit(&stats[6], 1, memory_order_relaxed);
      atomic_fetch_add_explicit(&stats[9], 1, memory_order_relaxed);
      enqueue_global_constraint(next_cid, global_next_epochs, stats,
                                global_next_active, params);
    }
  }
}

inline uint revise_word_directional(
    int cid,
    int dir,
    int target_word_index,
    device atomic_uint* bit_dom,
    device atomic_int* domain_sizes,
    device const uint* bit_sup_words,
    device const DeviceInt2* scopes,
    constant GacParams& params,
    thread int& target) {
  const DeviceInt2 scope = scopes[cid];
  if (scope.x < 0 || scope.y < 0) {
    target = -1;
    return 0u;
  }

  target = (dir == 0) ? scope.x : scope.y;
  const int source = (dir == 0) ? scope.y : scope.x;
  if (target < 0 || target >= params.num_vars ||
      source < 0 || source >= params.num_vars ||
      target_word_index >= params.bit_words) {
    target = -1;
    return 0u;
  }

  const int target_word = target * params.bit_words + target_word_index;
  const uint current_word =
      atomic_load_explicit(&bit_dom[target_word], memory_order_relaxed);
  if (current_word == 0u) {
    return 0u;
  }

  uint deletion_mask = 0u;
  const int source_base = source * params.bit_words;
  const int value_begin = target_word_index * 32;
  const int value_end = min(value_begin + 32, params.max_dom_size);
  for (int value = value_begin; value < value_end; ++value) {
    const uint mask = 1u << static_cast<uint>(value & 31);
    if ((current_word & mask) == 0u) {
      continue;
    }

    const int sup_base =
        ((cid * 2 + dir) * params.max_dom_size + value) * params.bit_words;
    bool supported = false;
    for (int word = 0; word < params.bit_words; ++word) {
      const uint support_word = bit_sup_words[sup_base + word];
      const uint domain_word =
          atomic_load_explicit(&bit_dom[source_base + word],
                               memory_order_relaxed);
      if ((support_word & domain_word) != 0u) {
        supported = true;
        break;
      }
    }
    if (!supported) {
      deletion_mask |= mask;
    }
  }

  if (deletion_mask == 0u) {
    return 0u;
  }
  const uint old_word = atomic_fetch_and_explicit(
      &bit_dom[target_word], ~deletion_mask, memory_order_relaxed);
  return old_word & deletion_mask;
}

kernel void gac_revise_kernel(device atomic_uint* bit_dom [[buffer(0)]],
                              device atomic_int* domain_sizes [[buffer(1)]],
                              device const DeviceUInt2* bit_sup [[buffer(2)]],
                              device const DeviceInt2* scopes [[buffer(3)]],
                              device const int* sub_offsets [[buffer(4)]],
                              device const DeviceUInt3* sub_entries [[buffer(5)]],
                              device atomic_int* current_flags [[buffer(6)]],
                              device atomic_int* next_flags [[buffer(7)]],
                              device atomic_int* stats [[buffer(8)]],
                              constant GacParams& params [[buffer(9)]],
                              uint gid [[thread_position_in_grid]]) {
  const uint values_per_constraint = static_cast<uint>(2 * params.max_dom_size);
  const uint total_tasks = static_cast<uint>(params.num_constraints) *
                           values_per_constraint;
  if (gid >= total_tasks) {
    return;
  }

  const int cid = static_cast<int>(gid / values_per_constraint);
  const int local = static_cast<int>(gid % values_per_constraint);
  const int dir = local / params.max_dom_size;
  const int value = local % params.max_dom_size;

  if (atomic_load_explicit(&current_flags[cid], memory_order_relaxed) == 0) {
    return;
  }

  const DeviceInt2 scope = scopes[cid];
  if (scope.x < 0 || scope.y < 0) {
    return;
  }

  const int target = (dir == 0) ? scope.x : scope.y;
  const int source = (dir == 0) ? scope.y : scope.x;
  if (target < 0 || target >= params.num_vars ||
      source < 0 || source >= params.num_vars) {
    return;
  }

  const uint mask = 1u << static_cast<uint>(value & 31);
  const int target_word = target * params.bit_words + value / 32;
  const uint current_word =
      atomic_load_explicit(&bit_dom[target_word], memory_order_relaxed);
  if ((current_word & mask) == 0u) {
    return;
  }

  const int sup_base =
      ((cid * 2 + dir) * params.max_dom_size + value) * params.bit_words;
  const int source_base = source * params.bit_words;
  bool supported = false;
  for (int word = 0; word < params.bit_words; ++word) {
    const DeviceUInt2 support = bit_sup[sup_base + word];
    const uint support_word = (dir == 0) ? support.x : support.y;
    const uint domain_word =
        atomic_load_explicit(&bit_dom[source_base + word], memory_order_relaxed);
    if ((support_word & domain_word) != 0u) {
      supported = true;
      break;
    }
  }

  if (supported) {
    return;
  }

  const uint old_word = atomic_fetch_and_explicit(
      &bit_dom[target_word], ~mask, memory_order_relaxed);
  if ((old_word & mask) == 0u) {
    return;
  }

  atomic_fetch_add_explicit(&stats[0], 1, memory_order_relaxed);
  const int remaining =
      atomic_fetch_sub_explicit(&domain_sizes[target], 1, memory_order_relaxed) - 1;
  if (remaining == 0) {
    atomic_store_explicit(&stats[1], 1, memory_order_relaxed);
  }

  const int begin = sub_offsets[target];
  const int end = sub_offsets[target + 1];
  for (int i = begin; i < end; ++i) {
    const uint next_cid = sub_entries[i].z;
    if (next_cid >= static_cast<uint>(params.num_constraints)) {
      continue;
    }
    const int previous =
        atomic_exchange_explicit(&next_flags[next_cid], 1, memory_order_relaxed);
    if (previous == 0) {
      atomic_fetch_add_explicit(&stats[2], 1, memory_order_relaxed);
    }
  }
}

kernel void gac_revise_worklist_kernel(
                              device atomic_uint* bit_dom [[buffer(0)]],
                              device atomic_int* domain_sizes [[buffer(1)]],
                              device const uint* bit_sup_words [[buffer(2)]],
                              device const DeviceInt2* scopes [[buffer(3)]],
                              device const int* sub_offsets [[buffer(4)]],
                              device const DeviceUInt3* sub_entries [[buffer(5)]],
                              device const int* active_constraints [[buffer(6)]],
                              device atomic_int* next_flags [[buffer(7)]],
                              device atomic_int* stats [[buffer(8)]],
                              device int* next_active_constraints [[buffer(9)]],
                              constant GacParams& params [[buffer(10)]],
                              uint gid [[thread_position_in_grid]]) {
  const uint values_per_constraint = static_cast<uint>(2 * params.max_dom_size);
  const uint total_tasks = static_cast<uint>(params.active_count) *
                           values_per_constraint;
  if (gid >= total_tasks) {
    return;
  }

  const int active_index = static_cast<int>(gid / values_per_constraint);
  const int cid = active_constraints[active_index];
  const int local = static_cast<int>(gid % values_per_constraint);
  const int dir = local / params.max_dom_size;
  const int value = local % params.max_dom_size;

  if (cid < 0 || cid >= params.num_constraints) {
    return;
  }

  const DeviceInt2 scope = scopes[cid];
  if (scope.x < 0 || scope.y < 0) {
    return;
  }

  const int target = (dir == 0) ? scope.x : scope.y;
  const int source = (dir == 0) ? scope.y : scope.x;
  if (target < 0 || target >= params.num_vars ||
      source < 0 || source >= params.num_vars) {
    return;
  }

  const uint mask = 1u << static_cast<uint>(value & 31);
  const int target_word = target * params.bit_words + value / 32;
  const uint current_word =
      atomic_load_explicit(&bit_dom[target_word], memory_order_relaxed);
  if ((current_word & mask) == 0u) {
    return;
  }

  const int sup_base =
      ((cid * 2 + dir) * params.max_dom_size + value) * params.bit_words;
  const int source_base = source * params.bit_words;
  bool supported = false;
  for (int word = 0; word < params.bit_words; ++word) {
    const uint support_word = bit_sup_words[sup_base + word];
    const uint domain_word =
        atomic_load_explicit(&bit_dom[source_base + word], memory_order_relaxed);
    if ((support_word & domain_word) != 0u) {
      supported = true;
      break;
    }
  }

  if (supported) {
    return;
  }

  const uint old_word = atomic_fetch_and_explicit(
      &bit_dom[target_word], ~mask, memory_order_relaxed);
  if ((old_word & mask) == 0u) {
    return;
  }

  atomic_fetch_add_explicit(&stats[0], 1, memory_order_relaxed);
  const int remaining =
      atomic_fetch_sub_explicit(&domain_sizes[target], 1, memory_order_relaxed) - 1;
  if (remaining == 0) {
    atomic_store_explicit(&stats[1], 1, memory_order_relaxed);
  }

  enqueue_next_worklist(target, sub_offsets, sub_entries, next_flags,
                        next_active_constraints, stats, params);
}

kernel void gac_revise_word_flags_kernel(
                              device atomic_uint* bit_dom [[buffer(0)]],
                              device atomic_int* domain_sizes [[buffer(1)]],
                              device const uint* bit_sup_words [[buffer(2)]],
                              device const DeviceInt2* scopes [[buffer(3)]],
                              device const int* sub_offsets [[buffer(4)]],
                              device const DeviceUInt3* sub_entries [[buffer(5)]],
                              device atomic_int* current_flags [[buffer(6)]],
                              device atomic_int* next_flags [[buffer(7)]],
                              device atomic_int* stats [[buffer(8)]],
                              constant GacParams& params [[buffer(9)]],
                              uint gid [[thread_position_in_grid]]) {
  const uint words_per_constraint = static_cast<uint>(2 * params.bit_words);
  const uint total_tasks = static_cast<uint>(params.num_constraints) *
                           words_per_constraint;
  if (gid >= total_tasks) {
    return;
  }

  const int cid = static_cast<int>(gid / words_per_constraint);
  if (atomic_load_explicit(&current_flags[cid], memory_order_relaxed) == 0) {
    return;
  }
  const int local = static_cast<int>(gid % words_per_constraint);
  const int dir = local / params.bit_words;
  const int target_word_index = local % params.bit_words;
  int target = -1;
  const uint actual_deleted = revise_word_directional(
      cid, dir, target_word_index, bit_dom, domain_sizes, bit_sup_words,
      scopes, params, target);
  if (actual_deleted == 0u || target < 0) {
    return;
  }

  const int deletion_count = popcount(actual_deleted);
  atomic_fetch_add_explicit(&stats[0], deletion_count, memory_order_relaxed);
  const int remaining =
      atomic_fetch_sub_explicit(&domain_sizes[target], deletion_count,
                                memory_order_relaxed) - deletion_count;
  if (remaining == 0) {
    atomic_store_explicit(&stats[1], 1, memory_order_relaxed);
  }
  enqueue_next_flag(target, sub_offsets, sub_entries, next_flags, stats, params);
}

kernel void gac_revise_word_active_kernel(
                              device atomic_uint* bit_dom [[buffer(0)]],
                              device atomic_int* domain_sizes [[buffer(1)]],
                              device const uint* bit_sup_words [[buffer(2)]],
                              device const DeviceInt2* scopes [[buffer(3)]],
                              device const int* sub_offsets [[buffer(4)]],
                              device const DeviceUInt3* sub_entries [[buffer(5)]],
                              device const int* active_constraints [[buffer(6)]],
                              device atomic_int* next_flags [[buffer(7)]],
                              device atomic_int* stats [[buffer(8)]],
                              constant GacParams& params [[buffer(9)]],
                              uint gid [[thread_position_in_grid]]) {
  const uint words_per_constraint = static_cast<uint>(2 * params.bit_words);
  const uint total_tasks = static_cast<uint>(params.active_count) *
                           words_per_constraint;
  if (gid >= total_tasks) {
    return;
  }

  const int active_index = static_cast<int>(gid / words_per_constraint);
  const int cid = active_constraints[active_index];
  if (cid < 0 || cid >= params.num_constraints) {
    return;
  }
  const int local = static_cast<int>(gid % words_per_constraint);
  const int dir = local / params.bit_words;
  const int target_word_index = local % params.bit_words;
  int target = -1;
  const uint actual_deleted = revise_word_directional(
      cid, dir, target_word_index, bit_dom, domain_sizes, bit_sup_words,
      scopes, params, target);
  if (actual_deleted == 0u || target < 0) {
    return;
  }

  const int deletion_count = popcount(actual_deleted);
  atomic_fetch_add_explicit(&stats[0], deletion_count, memory_order_relaxed);
  const int remaining =
      atomic_fetch_sub_explicit(&domain_sizes[target], deletion_count,
                                memory_order_relaxed) - deletion_count;
  if (remaining == 0) {
    atomic_store_explicit(&stats[1], 1, memory_order_relaxed);
  }
  enqueue_next_flag(target, sub_offsets, sub_entries, next_flags, stats, params);
}

kernel void gac_revise_word_worklist_kernel(
                              device atomic_uint* bit_dom [[buffer(0)]],
                              device atomic_int* domain_sizes [[buffer(1)]],
                              device const uint* bit_sup_words [[buffer(2)]],
                              device const DeviceInt2* scopes [[buffer(3)]],
                              device const int* sub_offsets [[buffer(4)]],
                              device const DeviceUInt3* sub_entries [[buffer(5)]],
                              device const int* active_constraints [[buffer(6)]],
                              device atomic_int* next_flags [[buffer(7)]],
                              device atomic_int* stats [[buffer(8)]],
                              device int* next_active_constraints [[buffer(9)]],
                              constant GacParams& params [[buffer(10)]],
                              uint gid [[thread_position_in_grid]]) {
  const uint words_per_constraint = static_cast<uint>(2 * params.bit_words);
  const uint total_tasks = static_cast<uint>(params.active_count) *
                           words_per_constraint;
  if (gid >= total_tasks) {
    return;
  }

  const int active_index = static_cast<int>(gid / words_per_constraint);
  const int cid = active_constraints[active_index];
  if (cid < 0 || cid >= params.num_constraints) {
    return;
  }
  const int local = static_cast<int>(gid % words_per_constraint);
  const int dir = local / params.bit_words;
  const int target_word_index = local % params.bit_words;
  int target = -1;
  const uint actual_deleted = revise_word_directional(
      cid, dir, target_word_index, bit_dom, domain_sizes, bit_sup_words,
      scopes, params, target);
  if (actual_deleted == 0u || target < 0) {
    return;
  }

  const int deletion_count = popcount(actual_deleted);
  atomic_fetch_add_explicit(&stats[0], deletion_count, memory_order_relaxed);
  const int remaining =
      atomic_fetch_sub_explicit(&domain_sizes[target], deletion_count,
                                memory_order_relaxed) - deletion_count;
  if (remaining == 0) {
    atomic_store_explicit(&stats[1], 1, memory_order_relaxed);
  }
  enqueue_next_worklist(target, sub_offsets, sub_entries, next_flags,
                        next_active_constraints, stats, params);
}

kernel void gac_revise_bulk_mask_kernel(
                              device atomic_uint* bit_dom [[buffer(0)]],
                              device const uint* bit_sup_words [[buffer(1)]],
                              device const DeviceInt2* scopes [[buffer(2)]],
                              device const int* active_constraints [[buffer(3)]],
                              device atomic_uint* delete_masks [[buffer(4)]],
                              device atomic_int* stats [[buffer(5)]],
                              constant GacParams& params [[buffer(6)]],
                              uint gid [[thread_position_in_grid]]) {
  const uint words_per_constraint = static_cast<uint>(2 * params.bit_words);
  const uint total_tasks = static_cast<uint>(params.active_count) *
                           words_per_constraint;
  if (gid >= total_tasks) {
    return;
  }

  const int active_index = static_cast<int>(gid / words_per_constraint);
  const int cid = active_constraints[active_index];
  if (cid < 0 || cid >= params.num_constraints) {
    return;
  }

  const int local = static_cast<int>(gid % words_per_constraint);
  const int dir = local / params.bit_words;
  const int target_word_index = local % params.bit_words;
  const DeviceInt2 scope = scopes[cid];
  if (scope.x < 0 || scope.y < 0) {
    return;
  }

  const int target = (dir == 0) ? scope.x : scope.y;
  const int source = (dir == 0) ? scope.y : scope.x;
  if (target < 0 || target >= params.num_vars ||
      source < 0 || source >= params.num_vars ||
      target_word_index >= params.bit_words) {
    return;
  }

  const int target_word = target * params.bit_words + target_word_index;
  const uint current_word =
      atomic_load_explicit(&bit_dom[target_word], memory_order_relaxed);
  if (current_word == 0u) {
    return;
  }

  uint deletion_mask = 0u;
  const int source_base = source * params.bit_words;
  const int value_begin = target_word_index * 32;
  const int value_end = min(value_begin + 32, params.max_dom_size);
  for (int value = value_begin; value < value_end; ++value) {
    const uint mask = 1u << static_cast<uint>(value & 31);
    if ((current_word & mask) == 0u) {
      continue;
    }

    const int sup_base =
        ((cid * 2 + dir) * params.max_dom_size + value) * params.bit_words;
    bool supported = false;
    for (int word = 0; word < params.bit_words; ++word) {
      const uint support_word = bit_sup_words[sup_base + word];
      const uint domain_word =
          atomic_load_explicit(&bit_dom[source_base + word],
                               memory_order_relaxed);
      if ((support_word & domain_word) != 0u) {
        supported = true;
        break;
      }
    }
    if (!supported) {
      deletion_mask |= mask;
    }
  }

  if (deletion_mask == 0u) {
    return;
  }
  atomic_fetch_or_explicit(&delete_masks[target_word], deletion_mask,
                           memory_order_relaxed);
  atomic_fetch_add_explicit(&stats[14], popcount(deletion_mask),
                            memory_order_relaxed);
}

kernel void gac_apply_bulk_mask_kernel(
                              device atomic_uint* bit_dom [[buffer(0)]],
                              device atomic_int* domain_sizes [[buffer(1)]],
                              device atomic_uint* delete_masks [[buffer(2)]],
                              device const int* sub_offsets [[buffer(3)]],
                              device const DeviceUInt3* sub_entries [[buffer(4)]],
                              device atomic_int* next_flags [[buffer(5)]],
                              device atomic_int* stats [[buffer(6)]],
                              device int* next_active_constraints [[buffer(7)]],
                              constant GacParams& params [[buffer(8)]],
                              uint gid [[thread_position_in_grid]]) {
  const uint total_tasks = static_cast<uint>(params.num_vars) *
                           static_cast<uint>(params.bit_words);
  if (gid >= total_tasks) {
    return;
  }

  const int target = static_cast<int>(gid / static_cast<uint>(params.bit_words));
  const uint proposed_mask =
      atomic_exchange_explicit(&delete_masks[gid], 0u, memory_order_relaxed);
  if (target < 0 || target >= params.num_vars || proposed_mask == 0u) {
    return;
  }

  const uint old_word = atomic_fetch_and_explicit(
      &bit_dom[gid], ~proposed_mask, memory_order_relaxed);
  const uint actual_deleted = old_word & proposed_mask;
  if (actual_deleted == 0u) {
    return;
  }

  const int deletion_count = popcount(actual_deleted);
  atomic_fetch_add_explicit(&stats[0], deletion_count, memory_order_relaxed);
  atomic_fetch_add_explicit(&stats[15], deletion_count, memory_order_relaxed);
  atomic_fetch_add_explicit(&stats[16], 1, memory_order_relaxed);
  const int remaining =
      atomic_fetch_sub_explicit(&domain_sizes[target], deletion_count,
                                memory_order_relaxed) - deletion_count;
  if (remaining == 0) {
    atomic_store_explicit(&stats[1], 1, memory_order_relaxed);
  }
  enqueue_next_bulk_worklist(target, sub_offsets, sub_entries, next_flags,
                             next_active_constraints, stats, params);
}

kernel void gac_revise_cta_worklist_kernel(
                              device atomic_uint* bit_dom [[buffer(0)]],
                              device atomic_int* domain_sizes [[buffer(1)]],
                              device const uint* bit_sup_words [[buffer(2)]],
                              device const DeviceInt2* scopes [[buffer(3)]],
                              device const int* sub_offsets [[buffer(4)]],
                              device const DeviceUInt3* sub_entries [[buffer(5)]],
                              device int* cta_queue_a [[buffer(6)]],
                              device int* cta_queue_b [[buffer(7)]],
                              device atomic_int* cta_tail_a [[buffer(8)]],
                              device atomic_int* cta_tail_b [[buffer(9)]],
                              device atomic_int* cta_stamps [[buffer(10)]],
                              device atomic_int* global_next_flags [[buffer(11)]],
                              device atomic_int* stats [[buffer(12)]],
                              device int* global_next_active [[buffer(13)]],
                              device const int* owner_of_constraint [[buffer(14)]],
                              device atomic_int* dirty_var_epochs [[buffer(15)]],
                              constant GacParams& params [[buffer(16)]],
                              uint tid [[thread_position_in_threadgroup]],
                              uint cta_gid [[threadgroup_position_in_grid]]) {
  const int cta_id = static_cast<int>(cta_gid);
  if (cta_id >= params.cta_count || params.cta_queue_capacity <= 0) {
    return;
  }

  int rounds_done = 0;
  for (int local_round = 0;
       local_round < params.cta_local_round_budget;
       ++local_round) {
    const bool use_a = (local_round & 1) == 0;
    device int* cur_queue = use_a ? cta_queue_a : cta_queue_b;
    device int* next_queue = use_a ? cta_queue_b : cta_queue_a;
    device atomic_int* cur_tail = use_a ? cta_tail_a : cta_tail_b;
    device atomic_int* next_tail = use_a ? cta_tail_b : cta_tail_a;

    if (tid == 0) {
      atomic_store_explicit(&next_tail[cta_id], 0, memory_order_relaxed);
    }
    threadgroup_barrier(mem_flags::mem_device);

    const int tail =
        atomic_load_explicit(&cur_tail[cta_id], memory_order_relaxed);
    if (tail <= 0) {
      break;
    }

    const int tasks_per_constraint = 2 * params.bit_words;
    const int max_tail = min(tail, params.cta_queue_capacity);
    for (int head = 0; head < max_tail; ++head) {
      const int cid = cur_queue[cta_id * params.cta_queue_capacity + head];
      if (cid < 0 || cid >= params.num_constraints ||
          cta_owner_for_constraint(cid, owner_of_constraint, params) != cta_id) {
        continue;
      }

      for (int local = static_cast<int>(tid);
           local < tasks_per_constraint;
           local += params.threads_per_cta) {
        const int dir = local / params.bit_words;
        const int target_word_index = local % params.bit_words;
        int target = -1;
        const uint actual_deleted = revise_word_directional(
            cid, dir, target_word_index, bit_dom, domain_sizes, bit_sup_words,
            scopes, params, target);
        if (actual_deleted == 0u || target < 0) {
          continue;
        }

        const int deletion_count = popcount(actual_deleted);
        atomic_fetch_add_explicit(&stats[0], deletion_count,
                                  memory_order_relaxed);
        const int remaining =
            atomic_fetch_sub_explicit(&domain_sizes[target], deletion_count,
                                      memory_order_relaxed) - deletion_count;
        if (remaining == 0) {
          atomic_store_explicit(&stats[1], 1, memory_order_relaxed);
        }
        enqueue_next_cta_or_dirty_pull(target, cta_id,
                                       params.frontier_epoch + local_round + 1,
                                       sub_offsets, sub_entries, next_queue,
                                       next_tail, cta_stamps, global_next_flags,
                                       stats, global_next_active,
                                       owner_of_constraint, dirty_var_epochs,
                                       params);
      }
      threadgroup_barrier(mem_flags::mem_device);
    }

    if (tail > params.cta_queue_capacity && tid == 0) {
      atomic_fetch_add_explicit(&stats[6], 1, memory_order_relaxed);
      atomic_fetch_add_explicit(&stats[9], 1, memory_order_relaxed);
    }
    if (tid == 0) {
      atomic_store_explicit(&cur_tail[cta_id], 0, memory_order_relaxed);
      atomic_fetch_add_explicit(&stats[3], 1, memory_order_relaxed);
    }
    threadgroup_barrier(mem_flags::mem_device);
    ++rounds_done;
  }

  int total_rounds_done = rounds_done;
  bool replay_started = false;
  if (rounds_done >= params.cta_local_round_budget &&
      params.cta_queue_mode == 2 &&
      params.cta_replay_round_budget > 0) {
    const bool pending_in_a = (rounds_done & 1) == 0;
    device atomic_int* pending_tail = pending_in_a ? cta_tail_a : cta_tail_b;
    const int pending =
        atomic_load_explicit(&pending_tail[cta_id], memory_order_relaxed);
    replay_started = pending > 0;
  }

  if (replay_started) {
    for (int replay_round = 0;
         replay_round < params.cta_replay_round_budget;
         ++replay_round) {
      const int absolute_round = params.cta_local_round_budget + replay_round;
      const bool use_a = (absolute_round & 1) == 0;
      device int* cur_queue = use_a ? cta_queue_a : cta_queue_b;
      device int* next_queue = use_a ? cta_queue_b : cta_queue_a;
      device atomic_int* cur_tail = use_a ? cta_tail_a : cta_tail_b;
      device atomic_int* next_tail = use_a ? cta_tail_b : cta_tail_a;

      if (tid == 0) {
        atomic_store_explicit(&next_tail[cta_id], 0, memory_order_relaxed);
      }
      threadgroup_barrier(mem_flags::mem_device);

      const int tail =
          atomic_load_explicit(&cur_tail[cta_id], memory_order_relaxed);
      if (tail <= 0) {
        break;
      }

      const int tasks_per_constraint = 2 * params.bit_words;
      const int max_tail = min(tail, params.cta_queue_capacity);
      for (int head = 0; head < max_tail; ++head) {
        const int cid = cur_queue[cta_id * params.cta_queue_capacity + head];
        if (cid < 0 || cid >= params.num_constraints ||
            cta_owner_for_constraint(cid, owner_of_constraint, params) != cta_id) {
          continue;
        }

        for (int local = static_cast<int>(tid);
             local < tasks_per_constraint;
             local += params.threads_per_cta) {
          const int dir = local / params.bit_words;
          const int target_word_index = local % params.bit_words;
          int target = -1;
          const uint actual_deleted = revise_word_directional(
              cid, dir, target_word_index, bit_dom, domain_sizes, bit_sup_words,
              scopes, params, target);
          if (actual_deleted == 0u || target < 0) {
            continue;
          }

          const int deletion_count = popcount(actual_deleted);
          atomic_fetch_add_explicit(&stats[0], deletion_count,
                                    memory_order_relaxed);
          const int remaining =
              atomic_fetch_sub_explicit(&domain_sizes[target], deletion_count,
                                        memory_order_relaxed) - deletion_count;
          if (remaining == 0) {
            atomic_store_explicit(&stats[1], 1, memory_order_relaxed);
          }
          enqueue_next_cta_or_dirty_pull(
              target, cta_id, params.frontier_epoch + absolute_round + 1,
              sub_offsets, sub_entries, next_queue, next_tail, cta_stamps,
              global_next_flags, stats, global_next_active, owner_of_constraint,
              dirty_var_epochs, params);
        }
        threadgroup_barrier(mem_flags::mem_device);
      }

      if (tail > params.cta_queue_capacity && tid == 0) {
        atomic_fetch_add_explicit(&stats[6], 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&stats[9], 1, memory_order_relaxed);
      }
      if (tid == 0) {
        atomic_store_explicit(&cur_tail[cta_id], 0, memory_order_relaxed);
        atomic_fetch_add_explicit(&stats[3], 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&stats[11], 1, memory_order_relaxed);
      }
      threadgroup_barrier(mem_flags::mem_device);
      ++total_rounds_done;
    }
  }

  if (rounds_done >= params.cta_local_round_budget) {
    const bool pending_in_a = (total_rounds_done & 1) == 0;
    device int* pending_queue = pending_in_a ? cta_queue_a : cta_queue_b;
    device atomic_int* pending_tail = pending_in_a ? cta_tail_a : cta_tail_b;
    const int pending =
        atomic_load_explicit(&pending_tail[cta_id], memory_order_relaxed);
    const int max_pending = min(pending, params.cta_queue_capacity);
    for (int i = static_cast<int>(tid); i < max_pending;
         i += params.threads_per_cta) {
      const int cid = pending_queue[cta_id * params.cta_queue_capacity + i];
      enqueue_global_constraint(cid, global_next_flags, stats,
                                global_next_active, params);
    }
    if (pending > 0 && tid == 0) {
      atomic_fetch_add_explicit(&stats[6], 1, memory_order_relaxed);
      if (params.cta_queue_mode == 1 || params.cta_queue_mode == 2) {
        atomic_fetch_add_explicit(&stats[10], 1, memory_order_relaxed);
      }
      if (params.cta_queue_mode == 2) {
        atomic_fetch_add_explicit(&stats[13], 1, memory_order_relaxed);
      }
    } else if (replay_started && tid == 0) {
      atomic_fetch_add_explicit(&stats[12], 1, memory_order_relaxed);
    }
  }
}

kernel void gac_compact_frontier_kernel(device const atomic_int* current_flags [[buffer(0)]],
                                        device int* active_constraints [[buffer(1)]],
                                        device atomic_int* compact_stats [[buffer(2)]],
                                        constant CompactParams& params [[buffer(3)]],
                                        uint gid [[thread_position_in_grid]]) {
  if (gid >= static_cast<uint>(params.num_constraints)) {
    return;
  }
  if (atomic_load_explicit(&current_flags[gid], memory_order_relaxed) == 0) {
    return;
  }
  const int slot = atomic_fetch_add_explicit(
      &compact_stats[0], 1, memory_order_relaxed);
  active_constraints[slot] = static_cast<int>(gid);
}

kernel void gac_revise_compact_kernel(device atomic_uint* bit_dom [[buffer(0)]],
                                      device atomic_int* domain_sizes [[buffer(1)]],
                                      device const DeviceUInt2* bit_sup [[buffer(2)]],
                                      device const DeviceInt2* scopes [[buffer(3)]],
                                      device const int* sub_offsets [[buffer(4)]],
                                      device const DeviceUInt3* sub_entries [[buffer(5)]],
                                      device const int* active_constraints [[buffer(6)]],
                                      device atomic_int* next_flags [[buffer(7)]],
                                      device atomic_int* stats [[buffer(8)]],
                                      constant GacParams& params [[buffer(9)]],
                                      uint gid [[thread_position_in_grid]]) {
  const uint values_per_constraint = static_cast<uint>(2 * params.max_dom_size);
  const uint total_tasks = static_cast<uint>(params.active_count) *
                           values_per_constraint;
  if (gid >= total_tasks) {
    return;
  }

  const int active_index = static_cast<int>(gid / values_per_constraint);
  const int cid = active_constraints[active_index];
  const int local = static_cast<int>(gid % values_per_constraint);
  const int dir = local / params.max_dom_size;
  const int value = local % params.max_dom_size;

  if (cid < 0 || cid >= params.num_constraints) {
    return;
  }

  const DeviceInt2 scope = scopes[cid];
  if (scope.x < 0 || scope.y < 0) {
    return;
  }

  const int target = (dir == 0) ? scope.x : scope.y;
  const int source = (dir == 0) ? scope.y : scope.x;
  if (target < 0 || target >= params.num_vars ||
      source < 0 || source >= params.num_vars) {
    return;
  }

  const uint mask = 1u << static_cast<uint>(value & 31);
  const int target_word = target * params.bit_words + value / 32;
  const uint current_word =
      atomic_load_explicit(&bit_dom[target_word], memory_order_relaxed);
  if ((current_word & mask) == 0u) {
    return;
  }

  const int sup_base =
      ((cid * 2 + dir) * params.max_dom_size + value) * params.bit_words;
  const int source_base = source * params.bit_words;
  bool supported = false;
  for (int word = 0; word < params.bit_words; ++word) {
    const DeviceUInt2 support = bit_sup[sup_base + word];
    const uint support_word = (dir == 0) ? support.x : support.y;
    const uint domain_word =
        atomic_load_explicit(&bit_dom[source_base + word], memory_order_relaxed);
    if ((support_word & domain_word) != 0u) {
      supported = true;
      break;
    }
  }

  if (supported) {
    return;
  }

  const uint old_word = atomic_fetch_and_explicit(
      &bit_dom[target_word], ~mask, memory_order_relaxed);
  if ((old_word & mask) == 0u) {
    return;
  }

  atomic_fetch_add_explicit(&stats[0], 1, memory_order_relaxed);
  const int remaining =
      atomic_fetch_sub_explicit(&domain_sizes[target], 1, memory_order_relaxed) - 1;
  if (remaining == 0) {
    atomic_store_explicit(&stats[1], 1, memory_order_relaxed);
  }

  const int begin = sub_offsets[target];
  const int end = sub_offsets[target + 1];
  for (int i = begin; i < end; ++i) {
    const uint next_cid = sub_entries[i].z;
    if (next_cid >= static_cast<uint>(params.num_constraints)) {
      continue;
    }
    const int previous =
        atomic_exchange_explicit(&next_flags[next_cid], 1, memory_order_relaxed);
    if (previous == 0) {
      atomic_fetch_add_explicit(&stats[2], 1, memory_order_relaxed);
    }
  }
}

struct SacProbeTask {
  int var_id;
  int value;
  int task_id;
};

struct SacProbeParams {
  int num_worlds;
  int num_vars;
  int num_constraints;
  int max_dom_size;
  int bit_words;
  int activation_mode;
  int current_round;
  int max_probe_rounds;
  int use_allowed_constraint_mask;
  int fused_round_slot;
};

inline uint sac_tail_mask(int bits) {
  if (bits <= 0) {
    return 0u;
  }
  if (bits >= 32) {
    return 0xFFFFFFFFu;
  }
  return (1u << static_cast<uint>(bits)) - 1u;
}

inline void sac_enqueue_subscriptions(int world,
                                      int target,
                                      device const int* sub_offsets,
                                      device const DeviceUInt3* sub_entries,
                                      device const int* allowed_constraints,
                                      device atomic_int* next_frontier,
                                      device atomic_int* stats,
                                      constant SacProbeParams& params) {
  if (target < 0 || target + 1 >= params.num_vars + 1) {
    return;
  }
  const int begin = sub_offsets[target];
  const int end = sub_offsets[target + 1];
  const int world_frontier_base = world * params.num_constraints;
  for (int i = begin; i < end; ++i) {
    const int cid = static_cast<int>(sub_entries[i].z);
    if (cid < 0 || cid >= params.num_constraints) {
      continue;
    }
    if (params.use_allowed_constraint_mask != 0 &&
        allowed_constraints[cid] == 0) {
      continue;
    }
    const int previous = atomic_exchange_explicit(
        &next_frontier[world_frontier_base + cid], 1, memory_order_relaxed);
    if (previous == 0) {
      atomic_fetch_add_explicit(&stats[0], 1, memory_order_relaxed);
    }
  }
}

kernel void sac_probe_init_kernel(
    device const uint* snapshot_bit_dom [[buffer(0)]],
    device const int* snapshot_domain_sizes [[buffer(1)]],
    device const SacProbeTask* tasks [[buffer(2)]],
    device const DeviceInt2* scopes [[buffer(3)]],
    device atomic_uint* world_bit_dom [[buffer(4)]],
    device atomic_int* world_domain_sizes [[buffer(5)]],
    device atomic_int* current_frontier [[buffer(6)]],
    device atomic_int* next_frontier [[buffer(7)]],
    device atomic_int* statuses [[buffer(8)]],
    device atomic_int* stats [[buffer(9)]],
    constant SacProbeParams& params [[buffer(10)]],
    device const int* allowed_constraints [[buffer(11)]],
    device atomic_int* dwo_debug [[buffer(12)]],
    uint gid [[thread_position_in_grid]]) {
  const int world_count = params.num_worlds;
  const int domain_words_per_world = params.num_vars * params.bit_words;
  const int total_domain_words = world_count * domain_words_per_world;
  const int total_domain_sizes = world_count * params.num_vars;
  const int frontier_per_world = params.num_constraints;
  const int total_frontier = world_count * frontier_per_world;

  if (static_cast<int>(gid) < total_domain_words) {
    const int world = static_cast<int>(gid) / domain_words_per_world;
    const int word = static_cast<int>(gid) % domain_words_per_world;
    const int var = word / params.bit_words;
    const int local_word = word % params.bit_words;
    uint value = snapshot_bit_dom[word];
    const SacProbeTask task = tasks[world];
    if (task.var_id >= 0 && task.var_id < params.num_vars &&
        task.value >= 0 && task.value < params.max_dom_size &&
        var == task.var_id) {
      const int value_word = task.value / 32;
      const uint value_mask = 1u << static_cast<uint>(task.value & 31);
      const uint snapshot_word =
          snapshot_bit_dom[task.var_id * params.bit_words + value_word];
      if ((snapshot_word & value_mask) != 0u) {
        value = (local_word == value_word) ? value_mask : 0u;
      }
    }
    atomic_store_explicit(&world_bit_dom[gid], value, memory_order_relaxed);
  }
  if (static_cast<int>(gid) < total_domain_sizes) {
    const int world = static_cast<int>(gid) / params.num_vars;
    const int var = static_cast<int>(gid) % params.num_vars;
    int size = snapshot_domain_sizes[var];
    const SacProbeTask task = tasks[world];
    if (task.var_id >= 0 && task.var_id < params.num_vars &&
        task.value >= 0 && task.value < params.max_dom_size &&
        var == task.var_id) {
      const int value_word = task.value / 32;
      const uint value_mask = 1u << static_cast<uint>(task.value & 31);
      const uint snapshot_word =
          snapshot_bit_dom[task.var_id * params.bit_words + value_word];
      if ((snapshot_word & value_mask) != 0u) {
        size = 1;
      }
    }
    atomic_store_explicit(&world_domain_sizes[gid], size,
                          memory_order_relaxed);
  }
  if (static_cast<int>(gid) < total_frontier) {
    atomic_store_explicit(&current_frontier[gid], 0, memory_order_relaxed);
    atomic_store_explicit(&next_frontier[gid], 0, memory_order_relaxed);
  }
  if (static_cast<int>(gid) < world_count) {
    atomic_store_explicit(&statuses[gid], 0, memory_order_relaxed);
    const int debug_base = static_cast<int>(gid) * 6;
    for (int word = 0; word < 6; ++word) {
      atomic_store_explicit(&dwo_debug[debug_base + word], -1,
                            memory_order_relaxed);
    }
  }

  if (static_cast<int>(gid) >= total_frontier) {
    return;
  }
  const int world = static_cast<int>(gid) / params.num_constraints;
  const int cid = static_cast<int>(gid) % params.num_constraints;
  const bool constraint_allowed =
      params.use_allowed_constraint_mask == 0 || allowed_constraints[cid] != 0;
  const SacProbeTask task = tasks[world];
  if (task.var_id < 0 || task.var_id >= params.num_vars ||
      task.value < 0 || task.value >= params.max_dom_size) {
    if (cid == 0) {
      atomic_store_explicit(&statuses[world], 2, memory_order_relaxed);
    }
    return;
  }

  const int value_word = task.value / 32;
  const uint value_mask = 1u << static_cast<uint>(task.value & 31);
  const uint snapshot_word =
      snapshot_bit_dom[task.var_id * params.bit_words + value_word];
  if ((snapshot_word & value_mask) == 0u) {
    if (cid == 0) {
      atomic_store_explicit(&statuses[world], 1, memory_order_relaxed);
      const int debug_base = world * 6;
      atomic_store_explicit(&dwo_debug[debug_base + 0], task.var_id,
                            memory_order_relaxed);
      atomic_store_explicit(&dwo_debug[debug_base + 1], -1,
                            memory_order_relaxed);
      atomic_store_explicit(&dwo_debug[debug_base + 2], -1,
                            memory_order_relaxed);
      atomic_store_explicit(&dwo_debug[debug_base + 3], 0,
                            memory_order_relaxed);
      atomic_store_explicit(&dwo_debug[debug_base + 4], 0,
                            memory_order_relaxed);
      atomic_store_explicit(&dwo_debug[debug_base + 5], params.current_round,
                            memory_order_relaxed);
    }
    return;
  }

  bool active = constraint_allowed && params.activation_mode != 0;
  if (!active) {
    const DeviceInt2 scope = scopes[cid];
    active = constraint_allowed &&
             (scope.x == task.var_id || scope.y == task.var_id);
  }
  if (active) {
    atomic_store_explicit(&current_frontier[world * params.num_constraints + cid],
                          1, memory_order_relaxed);
    atomic_fetch_add_explicit(&stats[4], 1, memory_order_relaxed);
  }
}

kernel void sac_probe_revise_kernel(
    device atomic_uint* world_bit_dom [[buffer(0)]],
    device atomic_int* world_domain_sizes [[buffer(1)]],
    device const uint* bit_sup_words [[buffer(2)]],
    device const DeviceInt2* scopes [[buffer(3)]],
    device const int* sub_offsets [[buffer(4)]],
    device const DeviceUInt3* sub_entries [[buffer(5)]],
    device const atomic_int* current_frontier [[buffer(6)]],
    device atomic_int* next_frontier [[buffer(7)]],
    device atomic_int* statuses [[buffer(8)]],
    device atomic_int* stats [[buffer(9)]],
    constant SacProbeParams& params [[buffer(10)]],
    device const int* allowed_constraints [[buffer(11)]],
    device atomic_int* dwo_debug [[buffer(12)]],
    uint gid [[thread_position_in_grid]]) {
  const int tasks_per_world = params.num_constraints * 2 * params.bit_words;
  if (tasks_per_world <= 0) {
    return;
  }
  const int world = static_cast<int>(gid) / tasks_per_world;
  if (world < 0 || world >= params.num_worlds) {
    return;
  }
  if (atomic_load_explicit(&statuses[world], memory_order_relaxed) != 0) {
    return;
  }

  const int local = static_cast<int>(gid) % tasks_per_world;
  const int cid = local / (2 * params.bit_words);
  if (params.use_allowed_constraint_mask != 0 &&
      allowed_constraints[cid] == 0) {
    return;
  }
  const int rem = local % (2 * params.bit_words);
  const int dir = rem / params.bit_words;
  const int target_word_index = rem % params.bit_words;
  const int frontier_index = world * params.num_constraints + cid;
  if (atomic_load_explicit(&current_frontier[frontier_index],
                           memory_order_relaxed) == 0) {
    return;
  }

  const DeviceInt2 scope = scopes[cid];
  if (scope.x < 0 || scope.y < 0) {
    return;
  }
  const int target = (dir == 0) ? scope.x : scope.y;
  const int source = (dir == 0) ? scope.y : scope.x;
  if (target < 0 || target >= params.num_vars ||
      source < 0 || source >= params.num_vars ||
      target_word_index >= params.bit_words) {
    return;
  }

  const int domain_words_per_world = params.num_vars * params.bit_words;
  const int world_domain_base = world * domain_words_per_world;
  const int target_word =
      world_domain_base + target * params.bit_words + target_word_index;
  const uint current_word =
      atomic_load_explicit(&world_bit_dom[target_word], memory_order_relaxed);
  if (current_word == 0u) {
    return;
  }

  const int source_base = world_domain_base + source * params.bit_words;
  const int value_begin = target_word_index * 32;
  const int value_end = min(value_begin + 32, params.max_dom_size);
  uint deletion_mask = 0u;
  for (int value = value_begin; value < value_end; ++value) {
    const uint mask = 1u << static_cast<uint>(value & 31);
    if ((current_word & mask) == 0u) {
      continue;
    }
    const int sup_base =
        ((cid * 2 + dir) * params.max_dom_size + value) * params.bit_words;
    bool supported = false;
    for (int word = 0; word < params.bit_words; ++word) {
      const uint support_word = bit_sup_words[sup_base + word];
      const uint domain_word =
          atomic_load_explicit(&world_bit_dom[source_base + word],
                               memory_order_relaxed);
      if ((support_word & domain_word) != 0u) {
        supported = true;
        break;
      }
    }
    if (!supported) {
      deletion_mask |= mask;
    }
  }

  if (deletion_mask == 0u) {
    return;
  }
  const uint old_word = atomic_fetch_and_explicit(
      &world_bit_dom[target_word], ~deletion_mask, memory_order_relaxed);
  const uint actual_mask = old_word & deletion_mask;
  if (actual_mask == 0u) {
    return;
  }
  const int deletion_count = popcount(actual_mask);
  atomic_fetch_add_explicit(&stats[1], deletion_count, memory_order_relaxed);
  const int old_size = atomic_fetch_sub_explicit(
      &world_domain_sizes[world * params.num_vars + target], deletion_count,
      memory_order_relaxed);
  if (old_size <= deletion_count) {
    const int previous = atomic_exchange_explicit(&statuses[world], 1,
                                                  memory_order_relaxed);
    if (previous == 0) {
      const int debug_base = world * 6;
      atomic_store_explicit(&dwo_debug[debug_base + 0], target,
                            memory_order_relaxed);
      atomic_store_explicit(&dwo_debug[debug_base + 1], cid,
                            memory_order_relaxed);
      atomic_store_explicit(&dwo_debug[debug_base + 2], dir,
                            memory_order_relaxed);
      atomic_store_explicit(&dwo_debug[debug_base + 3], old_size,
                            memory_order_relaxed);
      atomic_store_explicit(&dwo_debug[debug_base + 4], deletion_count,
                            memory_order_relaxed);
      atomic_store_explicit(&dwo_debug[debug_base + 5], params.current_round,
                            memory_order_relaxed);
      atomic_fetch_add_explicit(&stats[2], 1, memory_order_relaxed);
    }
    return;
  }
  sac_enqueue_subscriptions(world, target, sub_offsets, sub_entries,
                            allowed_constraints,
                            next_frontier, stats, params);
}

kernel void sac_probe_frontier_kernel(
    device atomic_int* current_frontier [[buffer(0)]],
    device atomic_int* next_frontier [[buffer(1)]],
    device const atomic_int* statuses [[buffer(2)]],
    device atomic_int* stats [[buffer(3)]],
    constant SacProbeParams& params [[buffer(10)]],
    uint gid [[thread_position_in_grid]]) {
  const int total_frontier = params.num_worlds * params.num_constraints;
  if (static_cast<int>(gid) >= total_frontier) {
    return;
  }
  const int world = static_cast<int>(gid) / params.num_constraints;
  const bool alive =
      atomic_load_explicit(&statuses[world], memory_order_relaxed) == 0;
  const int next = alive ? atomic_exchange_explicit(
                              &next_frontier[gid], 0, memory_order_relaxed)
                         : 0;
  atomic_store_explicit(&current_frontier[gid], next, memory_order_relaxed);
  if (next != 0) {
    atomic_fetch_add_explicit(&stats[0], 1, memory_order_relaxed);
  }
}

kernel void sac_probe_clear_active_counts_kernel(
    device atomic_int* active_counts [[buffer(0)]],
    constant SacProbeParams& params [[buffer(10)]],
    uint gid [[thread_position_in_grid]]) {
  if (gid != 0) {
    return;
  }
  if (params.fused_round_slot < 0) {
    return;
  }
  atomic_store_explicit(&active_counts[params.fused_round_slot], 0,
                        memory_order_relaxed);
}

kernel void sac_probe_frontier_fused_kernel(
    device atomic_int* current_frontier [[buffer(0)]],
    device atomic_int* next_frontier [[buffer(1)]],
    device const atomic_int* statuses [[buffer(2)]],
    device atomic_int* stats [[buffer(3)]],
    device atomic_int* active_counts [[buffer(4)]],
    constant SacProbeParams& params [[buffer(10)]],
    uint gid [[thread_position_in_grid]]) {
  const int total_frontier = params.num_worlds * params.num_constraints;
  if (static_cast<int>(gid) >= total_frontier) {
    return;
  }
  const int world = static_cast<int>(gid) / params.num_constraints;
  const bool alive =
      atomic_load_explicit(&statuses[world], memory_order_relaxed) == 0;
  const int next = alive ? atomic_exchange_explicit(
                              &next_frontier[gid], 0, memory_order_relaxed)
                         : 0;
  atomic_store_explicit(&current_frontier[gid], next, memory_order_relaxed);
  if (next != 0) {
    atomic_fetch_add_explicit(&active_counts[params.fused_round_slot], 1,
                              memory_order_relaxed);
  }
}

kernel void sac_probe_mark_unknown_kernel(
    device atomic_int* current_frontier [[buffer(0)]],
    device atomic_int* next_frontier [[buffer(1)]],
    device atomic_int* statuses [[buffer(2)]],
    device atomic_int* stats [[buffer(3)]],
    constant SacProbeParams& params [[buffer(10)]],
    uint gid [[thread_position_in_grid]]) {
  const int world = static_cast<int>(gid);
  if (world < 0 || world >= params.num_worlds) {
    return;
  }
  if (atomic_load_explicit(&statuses[world], memory_order_relaxed) != 0) {
    return;
  }
  bool active = false;
  const int base = world * params.num_constraints;
  for (int cid = 0; cid < params.num_constraints; ++cid) {
    if (atomic_load_explicit(&current_frontier[base + cid],
                             memory_order_relaxed) != 0 ||
        atomic_load_explicit(&next_frontier[base + cid],
                             memory_order_relaxed) != 0) {
      active = true;
      break;
    }
  }
  if (active) {
    atomic_store_explicit(&statuses[world], 2, memory_order_relaxed);
    atomic_fetch_add_explicit(&stats[3], 1, memory_order_relaxed);
  }
}
