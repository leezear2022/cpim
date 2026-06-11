#include <cassert>
#include <cstring>
#include <iostream>

#include "cpim_hls_types.hpp"

using namespace fpga_cpim_hls;

namespace {

void add_pair(word_t bit_sup[MAX_CONSTRAINTS * MAX_DOMAIN * MAX_WORDS * 2],
              const ConstraintHls& c, uint16_t x, uint16_t y) {
  const uint16_t x_words = word_count(c.x_domain_size);
  const uint16_t y_words = word_count(c.y_domain_size);
  bit_sup[c.bit_sup_offset_dir0 + x * y_words + y / 32u] |=
      word_t{1} << (y % 32u);
  bit_sup[c.bit_sup_offset_dir1 + y * x_words + x / 32u] |=
      word_t{1} << (x % 32u);
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
  word_t bit_sup[MAX_CONSTRAINTS * MAX_DOMAIN * MAX_WORDS * 2] = {};
  ConstraintHls constraints[MAX_CONSTRAINTS] = {};
  SubscriptionHls subscriptions[MAX_VARS] = {};
  uint16_t domain_size[MAX_VARS] = {};
  ProbeTaskHls tasks[MAX_WORLDS] = {};
  ResultHls results[MAX_WORLDS] = {};

  domain_size[0] = 4;
  domain_size[1] = 4;
  constraints[0] = ConstraintHls{0, 1, 4, 4, 0, 4};
  for (uint16_t v = 0; v < 4; ++v) {
    add_pair(bit_sup, constraints[0], v, v);
  }
  build_subscriptions(constraints, 1, subscriptions);
  tasks[0] = ProbeTaskHls{1, 0, 2};
  cpim_top_hls(bit_sup, constraints, subscriptions, domain_size, tasks, results,
               ControlHls{2, 1, 4, 100, 100, 100});
  assert(results[0].status == OK);
}

void run_less_than_dwo() {
  word_t bit_sup[MAX_CONSTRAINTS * MAX_DOMAIN * MAX_WORDS * 2] = {};
  ConstraintHls constraints[MAX_CONSTRAINTS] = {};
  SubscriptionHls subscriptions[MAX_VARS] = {};
  uint16_t domain_size[MAX_VARS] = {};
  ProbeTaskHls tasks[MAX_WORLDS] = {};
  ResultHls results[MAX_WORLDS] = {};

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
  cpim_top_hls(bit_sup, constraints, subscriptions, domain_size, tasks, results,
               ControlHls{2, 1, 4, 100, 100, 100});
  assert(results[0].status == DWO);
}

void run_budget_unknown() {
  word_t bit_sup[MAX_CONSTRAINTS * MAX_DOMAIN * MAX_WORDS * 2] = {};
  ConstraintHls constraints[MAX_CONSTRAINTS] = {};
  SubscriptionHls subscriptions[MAX_VARS] = {};
  uint16_t domain_size[MAX_VARS] = {};
  ProbeTaskHls tasks[MAX_WORLDS] = {};
  ResultHls results[MAX_WORLDS] = {};

  domain_size[0] = 4;
  domain_size[1] = 4;
  constraints[0] = ConstraintHls{0, 1, 4, 4, 0, 4};
  for (uint16_t v = 0; v < 4; ++v) {
    add_pair(bit_sup, constraints[0], v, v);
  }
  build_subscriptions(constraints, 1, subscriptions);
  tasks[0] = ProbeTaskHls{1, 0, 2};
  cpim_top_hls(bit_sup, constraints, subscriptions, domain_size, tasks, results,
               ControlHls{2, 1, 4, 0, 100, 100});
  assert(results[0].status == UNKNOWN);
}

}  // namespace

int main() {
  run_equality_ok();
  run_less_than_dwo();
  run_budget_unknown();
  std::cout << "hls_tb ok\n";
  return 0;
}
