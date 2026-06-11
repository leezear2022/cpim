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

void enqueue_from_var(const ConstraintHls constraints[MAX_CONSTRAINTS],
                      const SubscriptionHls subscriptions[MAX_VARS],
                      uint16_t var, uint16_t world,
                      DirtyEventHls queue[MAX_QUEUE], uint16_t* tail,
                      bool* overflow) {
  const SubscriptionHls& sub = subscriptions[var];
  for (uint16_t i = 0; i < MAX_CONSTRAINTS; ++i) {
    if (i >= sub.count) {
      break;
    }
    const uint16_t cid = sub.cids[i];
    const uint8_t dirs = dirs_for_var(constraints[cid], var);
    if (dirs == 0) {
      continue;
    }
    if (!hls_enqueue(queue, tail, DirtyEventHls{world, cid, dirs})) {
      *overflow = true;
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
    const ProbeTaskHls tasks[MAX_WORLDS],
    ResultHls results[MAX_WORLDS],
    ControlHls control) {
  FPGA_CPIM_HLS_PRAGMA(HLS DATAFLOW)
  for (uint16_t world = 0; world < MAX_WORLDS; ++world) {
    if (world >= MAX_WORLDS) {
      break;
    }
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
    for (uint16_t w = 0; w < MAX_WORDS; ++w) {
      domains[singleton_var][w] = 0;
    }
    domains[singleton_var][singleton_value / 32u] =
        word_t{1} << (singleton_value % 32u);

    DirtyEventHls queue[MAX_QUEUE] = {};
    uint16_t head = 0;
    uint16_t tail = 0;
    bool overflow = false;
    enqueue_from_var(constraints, subscriptions, singleton_var, world, queue, &tail,
                     &overflow);

    while (head < tail) {
      if (overflow) {
        break;
      }
      ++result.events;
      if (result.events > control.max_events) {
        overflow = true;
        break;
      }
      const DirtyEventHls ev = queue[head++];
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
          enqueue_from_var(constraints, subscriptions, del.var, world, queue, &tail,
                           &overflow);
        }
      }
    }
    result.status = overflow ? UNKNOWN : OK;
    results[world] = result;
  next_world:
    continue;
  }
}

}  // namespace fpga_cpim_hls
