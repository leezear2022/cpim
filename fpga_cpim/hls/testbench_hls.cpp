#include <cassert>
#include <cstddef>
#include <cstring>
#include <iostream>

#include "cpim_hls_types.hpp"

using namespace fpga_cpim_hls;

namespace {

constexpr std::size_t kBitSupWords =
    MAX_CONSTRAINTS * MAX_DOMAIN * MAX_WORDS * 2;

void reset_inputs(word_t bit_sup[kBitSupWords],
                  ConstraintHls constraints[MAX_CONSTRAINTS],
                  SubscriptionHls subscriptions[MAX_VARS],
                  uint16_t domain_size[MAX_VARS],
                  uint16_t var_partition[MAX_VARS],
                  uint16_t constraint_partition[MAX_CONSTRAINTS],
                  ProbeTaskHls tasks[MAX_WORLDS],
                  ResultHls results[MAX_WORLDS]) {
  std::memset(bit_sup, 0, sizeof(word_t) * kBitSupWords);
  std::memset(constraints, 0, sizeof(ConstraintHls) * MAX_CONSTRAINTS);
  std::memset(subscriptions, 0, sizeof(SubscriptionHls) * MAX_VARS);
  std::memset(domain_size, 0, sizeof(uint16_t) * MAX_VARS);
  std::memset(var_partition, 0, sizeof(uint16_t) * MAX_VARS);
  std::memset(constraint_partition, 0,
              sizeof(uint16_t) * MAX_CONSTRAINTS);
  std::memset(tasks, 0, sizeof(ProbeTaskHls) * MAX_WORLDS);
  std::memset(results, 0, sizeof(ResultHls) * MAX_WORLDS);
}

ControlHls make_control(uint16_t vars, uint16_t constraints, uint16_t domain) {
  ControlHls control;
  control.num_vars = vars;
  control.num_constraints = constraints;
  control.max_domain_size = domain;
  control.max_events = 100000;
  control.max_revise = 100000;
  control.max_epochs = 100000;
  control.num_partitions = 1;
  control.num_revise_tiles = 1;
  control.partition_queue_capacity = MAX_PARTITION_QUEUE;
  return control;
}

void add_pair(word_t bit_sup[kBitSupWords], const ConstraintHls& c,
              uint16_t x, uint16_t y) {
  const uint16_t x_words = word_count(c.x_domain_size);
  const uint16_t y_words = word_count(c.y_domain_size);
  bit_sup[c.bit_sup_offset_dir0 + x * y_words + y / 32u] |=
      word_t{1} << (y % 32u);
  bit_sup[c.bit_sup_offset_dir1 + y * x_words + x / 32u] |=
      word_t{1} << (x % 32u);
}

void add_equality_constraint(word_t bit_sup[kBitSupWords],
                             ConstraintHls constraints[MAX_CONSTRAINTS],
                             uint16_t cid, uint16_t x, uint16_t y,
                             uint16_t domain, uint32_t* next_offset) {
  const uint16_t words = word_count(domain);
  constraints[cid] =
      ConstraintHls{x, y, domain, domain, *next_offset,
                    *next_offset + static_cast<uint32_t>(domain * words)};
  *next_offset += static_cast<uint32_t>(domain * words * 2u);
  for (uint16_t value = 0; value < domain; ++value) {
    add_pair(bit_sup, constraints[cid], value, value);
  }
}

void build_subscriptions(const ConstraintHls constraints[MAX_CONSTRAINTS],
                         uint16_t num_constraints,
                         SubscriptionHls subscriptions[MAX_VARS]) {
  for (uint16_t cid = 0; cid < num_constraints; ++cid) {
    const ConstraintHls& c = constraints[cid];
    subscriptions[c.x].cids[subscriptions[c.x].count++] = cid;
    subscriptions[c.y].cids[subscriptions[c.y].count++] = cid;
  }
}

void run_equality_ok() {
  word_t bit_sup[kBitSupWords] = {};
  ConstraintHls constraints[MAX_CONSTRAINTS] = {};
  SubscriptionHls subscriptions[MAX_VARS] = {};
  uint16_t domain_size[MAX_VARS] = {};
  uint16_t var_partition[MAX_VARS] = {};
  uint16_t constraint_partition[MAX_CONSTRAINTS] = {};
  ProbeTaskHls tasks[MAX_WORLDS] = {};
  ResultHls results[MAX_WORLDS] = {};
  reset_inputs(bit_sup, constraints, subscriptions, domain_size, var_partition,
               constraint_partition, tasks, results);

  domain_size[0] = 4;
  domain_size[1] = 4;
  uint32_t offset = 0;
  add_equality_constraint(bit_sup, constraints, 0, 0, 1, 4, &offset);
  build_subscriptions(constraints, 1, subscriptions);
  tasks[0] = ProbeTaskHls{1, 0, 2};
  cpim_top_hls(bit_sup, constraints, subscriptions, domain_size, var_partition,
               constraint_partition, tasks, results, make_control(2, 1, 4));
  assert(results[0].status == OK);
}

void run_less_than_dwo() {
  word_t bit_sup[kBitSupWords] = {};
  ConstraintHls constraints[MAX_CONSTRAINTS] = {};
  SubscriptionHls subscriptions[MAX_VARS] = {};
  uint16_t domain_size[MAX_VARS] = {};
  uint16_t var_partition[MAX_VARS] = {};
  uint16_t constraint_partition[MAX_CONSTRAINTS] = {};
  ProbeTaskHls tasks[MAX_WORLDS] = {};
  ResultHls results[MAX_WORLDS] = {};
  reset_inputs(bit_sup, constraints, subscriptions, domain_size, var_partition,
               constraint_partition, tasks, results);

  domain_size[0] = 4;
  domain_size[1] = 4;
  constraints[0] = ConstraintHls{0, 1, 4, 4, 0, 4};
  for (uint16_t x = 0; x < 4; ++x) {
    for (uint16_t y = 0; y < 4; ++y) {
      if (x < y) {
        add_pair(bit_sup, constraints[0], x, y);
      }
    }
  }
  build_subscriptions(constraints, 1, subscriptions);
  tasks[0] = ProbeTaskHls{1, 0, 3};
  cpim_top_hls(bit_sup, constraints, subscriptions, domain_size, var_partition,
               constraint_partition, tasks, results, make_control(2, 1, 4));
  assert(results[0].status == DWO);
}

void run_budget_unknown() {
  word_t bit_sup[kBitSupWords] = {};
  ConstraintHls constraints[MAX_CONSTRAINTS] = {};
  SubscriptionHls subscriptions[MAX_VARS] = {};
  uint16_t domain_size[MAX_VARS] = {};
  uint16_t var_partition[MAX_VARS] = {};
  uint16_t constraint_partition[MAX_CONSTRAINTS] = {};
  ProbeTaskHls tasks[MAX_WORLDS] = {};
  ResultHls results[MAX_WORLDS] = {};
  reset_inputs(bit_sup, constraints, subscriptions, domain_size, var_partition,
               constraint_partition, tasks, results);

  domain_size[0] = 4;
  domain_size[1] = 4;
  uint32_t offset = 0;
  add_equality_constraint(bit_sup, constraints, 0, 0, 1, 4, &offset);
  build_subscriptions(constraints, 1, subscriptions);
  tasks[0] = ProbeTaskHls{1, 0, 2};
  ControlHls control = make_control(2, 1, 4);
  control.max_events = 0;
  cpim_top_hls(bit_sup, constraints, subscriptions, domain_size, var_partition,
               constraint_partition, tasks, results, control);
  assert(results[0].status == UNKNOWN);
}

void run_multi_tile_partition_smoke() {
  word_t bit_sup[kBitSupWords] = {};
  ConstraintHls constraints[MAX_CONSTRAINTS] = {};
  SubscriptionHls subscriptions[MAX_VARS] = {};
  uint16_t domain_size[MAX_VARS] = {};
  uint16_t var_partition[MAX_VARS] = {};
  uint16_t constraint_partition[MAX_CONSTRAINTS] = {};
  ProbeTaskHls tasks[MAX_WORLDS] = {};
  ResultHls results[MAX_WORLDS] = {};
  reset_inputs(bit_sup, constraints, subscriptions, domain_size, var_partition,
               constraint_partition, tasks, results);

  for (uint16_t var = 0; var < 4; ++var) {
    domain_size[var] = 4;
    var_partition[var] = var;
  }
  uint32_t offset = 0;
  add_equality_constraint(bit_sup, constraints, 0, 0, 1, 4, &offset);
  add_equality_constraint(bit_sup, constraints, 1, 0, 2, 4, &offset);
  add_equality_constraint(bit_sup, constraints, 2, 0, 3, 4, &offset);
  constraint_partition[0] = 1;
  constraint_partition[1] = 2;
  constraint_partition[2] = 3;
  build_subscriptions(constraints, 3, subscriptions);
  tasks[0] = ProbeTaskHls{1, 0, 2};

  ControlHls control = make_control(4, 3, 4);
  control.num_partitions = 4;
  control.num_revise_tiles = 2;
  cpim_top_hls(bit_sup, constraints, subscriptions, domain_size, var_partition,
               constraint_partition, tasks, results, control);

  assert(results[0].status == OK);
  assert(results[0].events >= 3);
  assert(results[0].epochs < results[0].events);
  assert(results[0].cross_events >= 3);
  assert(results[0].queue_peak_total >= 3);
  assert(results[0].queue_peak_partition == 1);
}

void run_partition_queue_overflow_unknown() {
  word_t bit_sup[kBitSupWords] = {};
  ConstraintHls constraints[MAX_CONSTRAINTS] = {};
  SubscriptionHls subscriptions[MAX_VARS] = {};
  uint16_t domain_size[MAX_VARS] = {};
  uint16_t var_partition[MAX_VARS] = {};
  uint16_t constraint_partition[MAX_CONSTRAINTS] = {};
  ProbeTaskHls tasks[MAX_WORLDS] = {};
  ResultHls results[MAX_WORLDS] = {};
  reset_inputs(bit_sup, constraints, subscriptions, domain_size, var_partition,
               constraint_partition, tasks, results);

  for (uint16_t var = 0; var < 4; ++var) {
    domain_size[var] = 4;
  }
  uint32_t offset = 0;
  add_equality_constraint(bit_sup, constraints, 0, 0, 1, 4, &offset);
  add_equality_constraint(bit_sup, constraints, 1, 0, 2, 4, &offset);
  add_equality_constraint(bit_sup, constraints, 2, 0, 3, 4, &offset);
  constraint_partition[0] = 1;
  constraint_partition[1] = 1;
  constraint_partition[2] = 1;
  build_subscriptions(constraints, 3, subscriptions);
  tasks[0] = ProbeTaskHls{1, 0, 2};

  ControlHls control = make_control(4, 3, 4);
  control.num_partitions = 2;
  control.partition_queue_capacity = 1;
  cpim_top_hls(bit_sup, constraints, subscriptions, domain_size, var_partition,
               constraint_partition, tasks, results, control);
  assert(results[0].status == UNKNOWN);
  assert(results[0].router_overflow == 1);
}

void run_density_010_pressure_smoke() {
  word_t bit_sup[kBitSupWords] = {};
  ConstraintHls constraints[MAX_CONSTRAINTS] = {};
  SubscriptionHls subscriptions[MAX_VARS] = {};
  uint16_t domain_size[MAX_VARS] = {};
  uint16_t var_partition[MAX_VARS] = {};
  uint16_t constraint_partition[MAX_CONSTRAINTS] = {};
  ProbeTaskHls tasks[MAX_WORLDS] = {};
  ResultHls results[MAX_WORLDS] = {};
  reset_inputs(bit_sup, constraints, subscriptions, domain_size, var_partition,
               constraint_partition, tasks, results);

  constexpr uint16_t kVars = 128;
  constexpr uint16_t kDomain = 128;
  constexpr uint16_t kPartitions = 4;
  uint16_t partition_load[MAX_PARTITIONS] = {};
  for (uint16_t var = 0; var < kVars; ++var) {
    domain_size[var] = kDomain;
    var_partition[var] = static_cast<uint16_t>(var / 32u);
  }

  uint16_t cid = 0;
  uint32_t offset = 0;
  for (uint16_t x = 0; x < kVars; ++x) {
    for (uint16_t y = static_cast<uint16_t>(x + 1u); y < kVars; ++y) {
      if (((x * 53u + y * 97u + 11u) % 10u) != 0) {
        continue;
      }
      if (cid >= MAX_CONSTRAINTS) {
        break;
      }
      add_equality_constraint(bit_sup, constraints, cid, x, y, kDomain, &offset);
      const uint16_t px = var_partition[x];
      const uint16_t py = var_partition[y];
      const uint16_t owner = partition_load[px] <= partition_load[py] ? px : py;
      constraint_partition[cid] = owner;
      ++partition_load[owner];
      ++cid;
    }
  }

  build_subscriptions(constraints, cid, subscriptions);
  tasks[0] = ProbeTaskHls{1, 0, 7};
  ControlHls control = make_control(kVars, cid, kDomain);
  control.num_partitions = kPartitions;
  control.num_revise_tiles = 4;
  control.partition_queue_capacity = MAX_PARTITION_QUEUE;
  control.max_events = 300000;
  control.max_revise = 300000;
  control.max_epochs = 300000;
  cpim_top_hls(bit_sup, constraints, subscriptions, domain_size, var_partition,
               constraint_partition, tasks, results, control);

  assert(cid >= 750);
  assert(cid <= 850);
  assert(results[0].status == OK);
  assert(results[0].deleted_values > 0);
  assert(results[0].events > 0);
  assert(results[0].cross_events > 0);
  assert(results[0].queue_peak_total > 0);
  assert(results[0].queue_peak_partition < MAX_PARTITION_QUEUE);
}

}  // namespace

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
int main() {
  run_equality_ok();
  run_less_than_dwo();
  run_budget_unknown();
  run_multi_tile_partition_smoke();
  run_partition_queue_overflow_unknown();
  run_density_010_pressure_smoke();
  std::cout << "hls_tb ok\n";
  return 0;
}
