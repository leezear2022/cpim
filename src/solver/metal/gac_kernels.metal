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
