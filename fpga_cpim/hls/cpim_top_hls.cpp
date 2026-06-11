#include "cpim_hls_types.hpp"

namespace fpga_cpim_hls {

namespace {

uint8_t dirs_for_var(const ConstraintHls& c, uint16_t var) {
  if (c.x == var) {
    return 0x2u;
  }
  if (c.y == var) {
    return 0x1u;
  }
  return 0;
}

uint16_t active_partitions(ControlHls control) {
  if (control.num_partitions == 0) {
    return 1;
  }
  return control.num_partitions > MAX_PARTITIONS ? MAX_PARTITIONS
                                                 : control.num_partitions;
}

uint16_t active_revise_tiles(ControlHls control) {
  if (control.num_revise_tiles == 0) {
    return 1;
  }
  return control.num_revise_tiles > MAX_REVISE_TILES ? MAX_REVISE_TILES
                                                     : control.num_revise_tiles;
}

uint16_t active_queue_capacity(ControlHls control) {
  if (control.partition_queue_capacity == 0 ||
      control.partition_queue_capacity > MAX_PARTITION_QUEUE) {
    return MAX_PARTITION_QUEUE;
  }
  return control.partition_queue_capacity;
}

uint16_t normalize_partition(uint16_t partition, uint16_t num_partitions) {
  return partition < num_partitions ? partition : 0;
}

void enqueue_from_var(const ConstraintHls constraints[MAX_CONSTRAINTS],
                      const SubscriptionHls subscriptions[MAX_VARS],
                      const uint16_t var_partition[MAX_VARS],
                      const uint16_t constraint_partition[MAX_CONSTRAINTS],
                      uint16_t var, uint16_t world,
                      DirtyEventHls queues[MAX_PARTITIONS][MAX_PARTITION_QUEUE],
                      uint16_t heads[MAX_PARTITIONS],
                      uint16_t tails[MAX_PARTITIONS],
                      uint16_t counts[MAX_PARTITIONS],
                      uint16_t num_partitions, uint16_t queue_capacity,
                      bool* overflow, ResultHls* result,
                      uint32_t* total_occupancy) {
  const SubscriptionHls& sub = subscriptions[var];
  const uint16_t source_partition =
      normalize_partition(var_partition[var], num_partitions);
  for (uint16_t i = 0; i < MAX_CONSTRAINTS; ++i) {
    if (i >= sub.count) {
      break;
    }
    const uint16_t cid = sub.cids[i];
    const uint8_t dirs = dirs_for_var(constraints[cid], var);
    if (dirs == 0) {
      continue;
    }
    const uint16_t target_partition =
        normalize_partition(constraint_partition[cid], num_partitions);
    if (target_partition == source_partition) {
      ++result->local_events;
    } else {
      ++result->cross_events;
    }
    if (!hls_enqueue_partition(queues, heads, tails, counts, target_partition,
                               queue_capacity, DirtyEventHls{world, cid, dirs},
                               total_occupancy, &result->queue_peak_total,
                               &result->queue_peak_partition)) {
      *overflow = true;
      result->router_overflow = 1;
      return;
    }
  }
}

}  // namespace

void cpim_top_hls(
    const word_t bit_sup[MAX_CONSTRAINTS * MAX_DOMAIN * MAX_WORDS * 2],
    const ConstraintHls constraints[MAX_CONSTRAINTS],
    const SubscriptionHls subscriptions[MAX_VARS],
    const uint16_t domain_size[MAX_VARS],
    const uint16_t var_partition[MAX_VARS],
    const uint16_t constraint_partition[MAX_CONSTRAINTS],
    const ProbeTaskHls tasks[MAX_WORLDS],
    ResultHls results[MAX_WORLDS],
    ControlHls control) {
  FPGA_CPIM_HLS_PRAGMA(HLS DATAFLOW)
  const uint16_t num_partitions = active_partitions(control);
  const uint16_t num_revise_tiles = active_revise_tiles(control);
  const uint16_t queue_capacity = active_queue_capacity(control);
  for (uint16_t world = 0; world < MAX_WORLDS; ++world) {
    ResultHls result;
    if (tasks[world].active == 0) {
      result.status = OK;
      results[world] = result;
      continue;
    }

    word_t domains[MAX_VARS][MAX_WORDS] = {};
    for (uint16_t var = 0; var < MAX_VARS; ++var) {
      if (var >= control.num_vars) {
        break;
      }
      const uint16_t words = word_count(domain_size[var]);
      for (uint16_t w = 0; w < MAX_WORDS; ++w) {
        if (w >= words) {
          break;
        }
        domains[var][w] = (w + 1 == words) ? last_word_mask(domain_size[var])
                                           : 0xffffffffu;
      }
    }

    const uint16_t singleton_var = tasks[world].var;
    const uint16_t singleton_value = tasks[world].value;
    if (singleton_var >= control.num_vars ||
        singleton_value >= domain_size[singleton_var]) {
      result.status = UNKNOWN;
      results[world] = result;
      continue;
    }
    for (uint16_t w = 0; w < MAX_WORDS; ++w) {
      domains[singleton_var][w] = 0;
    }
    domains[singleton_var][singleton_value / 32u] =
        word_t{1} << (singleton_value % 32u);

    DirtyEventHls queues[MAX_PARTITIONS][MAX_PARTITION_QUEUE] = {};
    uint16_t heads[MAX_PARTITIONS] = {};
    uint16_t tails[MAX_PARTITIONS] = {};
    uint16_t counts[MAX_PARTITIONS] = {};
    uint16_t rr_partition = 0;
    uint32_t total_occupancy = 0;
    bool overflow = false;
    enqueue_from_var(constraints, subscriptions, var_partition,
                     constraint_partition, singleton_var, world, queues, heads,
                     tails, counts, num_partitions, queue_capacity, &overflow,
                     &result, &total_occupancy);

    while (total_occupancy != 0) {
      if (overflow) {
        break;
      }
      ++result.epochs;
      if (result.epochs > control.max_epochs) {
        overflow = true;
        break;
      }
      bool did_work = false;
      for (uint16_t tile = 0; tile < MAX_REVISE_TILES; ++tile) {
        if (tile >= num_revise_tiles) {
          break;
        }
        DirtyEventHls ev;
        if (!hls_dequeue_partition(queues, heads, tails, counts, &rr_partition,
                                   num_partitions, queue_capacity, &ev,
                                   &total_occupancy)) {
          break;
        }
        did_work = true;
        ++result.tile_steps;
        ++result.events;
        if (result.events > control.max_events) {
          overflow = true;
          break;
        }
        if (ev.cid >= control.num_constraints) {
          continue;
        }
        const ConstraintHls& c = constraints[ev.cid];
        for (uint8_t dir = 0; dir < 2; ++dir) {
          const uint8_t bit = dir == 0 ? 0x1u : 0x2u;
          if ((ev.dirs & bit) == 0) {
            continue;
          }
          ++result.revise_calls;
          if (result.revise_calls > control.max_revise) {
            overflow = true;
            break;
          }
          DeleteEventHls del;
          del.world = world;
          bool dwo_predicted = false;
          uint32_t words_touched = 0;
          hls_revise_constraint(bit_sup, c, dir, domains, &del, &dwo_predicted,
                                &words_touched);
          uint32_t deleted = 0;
          const uint16_t target_size = domain_size[del.var];
          const bool dwo = hls_apply_delete(domains, del.var, target_size,
                                            del.del_words, &deleted);
          result.deleted_values += deleted;
          if (dwo) {
            result.status = DWO;
            results[world] = result;
            goto next_world;
          }
          if (deleted != 0) {
            enqueue_from_var(constraints, subscriptions, var_partition,
                             constraint_partition, del.var, world, queues,
                             heads, tails, counts, num_partitions,
                             queue_capacity, &overflow, &result,
                             &total_occupancy);
          }
          if (overflow) {
            break;
          }
        }
        if (overflow) {
          break;
        }
      }
      if (!did_work) {
        break;
      }
    }
    result.status = overflow ? UNKNOWN : OK;
    results[world] = result;
  next_world:
    continue;
  }
}

}  // namespace fpga_cpim_hls
