#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "cpim_hls_types.hpp"

using namespace fpga_cpim_hls;

namespace {

constexpr std::size_t kBitSupWords =
    MAX_CONSTRAINTS * MAX_DOMAIN * MAX_WORDS * 2;
constexpr uint16_t kPressureVars = MAX_VARS >= 128 ? 128 : MAX_VARS;
constexpr uint16_t kPressureDomain = MAX_DOMAIN >= 128 ? 128 : MAX_DOMAIN;
constexpr uint16_t kPressurePartitions = MAX_PARTITIONS >= 4 ? 4 : MAX_PARTITIONS;
constexpr uint16_t kMaxCliValues = 16;

enum FixtureKind {
  kFixtureRandom,
  kFixtureChain,
  kFixtureHub
};

struct TestbenchOptions {
  bool pressure_only = false;
  bool show_help = false;
  bool expect_capacity_threshold = HLS_PROFILE_IS_STRESS128;
  const char* expected_profile = nullptr;
  uint16_t tiles[kMaxCliValues] = {1, 2, 4};
  uint16_t tile_count = 3;
  uint16_t capacities[kMaxCliValues] = {64, 128, 256, 512, 1024};
  uint16_t capacity_count = 5;
  FixtureKind fixtures[kMaxCliValues] = {kFixtureRandom};
  uint16_t fixture_count = 1;
};

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

const char* status_name(Status status) {
  switch (status) {
    case OK:
      return "OK";
    case DWO:
      return "DWO";
    case UNKNOWN:
      return "UNKNOWN";
  }
  return "UNKNOWN";
}

bool starts_with(const char* text, const char* prefix) {
  return std::strncmp(text, prefix, std::strlen(prefix)) == 0;
}

bool profile_matches(const char* expected) {
  return expected == nullptr || std::strcmp(expected, HLS_PROFILE_NAME) == 0;
}

uint16_t parse_u16(const char* text) {
  const unsigned long value = std::strtoul(text, nullptr, 10);
  if (value > 65535ul) {
    return 65535u;
  }
  return static_cast<uint16_t>(value);
}

uint16_t parse_u16_list(const char* text, uint16_t values[kMaxCliValues]) {
  uint16_t count = 0;
  const char* cur = text;
  while (*cur != '\0' && count < kMaxCliValues) {
    values[count++] = parse_u16(cur);
    while (*cur != '\0' && *cur != ',') {
      ++cur;
    }
    if (*cur == ',') {
      ++cur;
    }
  }
  return count;
}

bool parse_fixture(const char* begin, const char* end, FixtureKind* fixture) {
  const std::size_t len = static_cast<std::size_t>(end - begin);
  if (len == 6 && std::strncmp(begin, "random", len) == 0) {
    *fixture = kFixtureRandom;
    return true;
  }
  if (len == 5 && std::strncmp(begin, "chain", len) == 0) {
    *fixture = kFixtureChain;
    return true;
  }
  if (len == 3 && std::strncmp(begin, "hub", len) == 0) {
    *fixture = kFixtureHub;
    return true;
  }
  return false;
}

uint16_t parse_fixture_list(const char* text,
                            FixtureKind fixtures[kMaxCliValues]) {
  uint16_t count = 0;
  const char* cur = text;
  while (*cur != '\0' && count < kMaxCliValues) {
    const char* begin = cur;
    while (*cur != '\0' && *cur != ',') {
      ++cur;
    }
    FixtureKind fixture = kFixtureRandom;
    if (parse_fixture(begin, cur, &fixture)) {
      fixtures[count++] = fixture;
    }
    if (*cur == ',') {
      ++cur;
    }
  }
  return count;
}

const char* fixture_name(FixtureKind fixture) {
  switch (fixture) {
    case kFixtureRandom:
      return "random";
    case kFixtureChain:
      return "chain";
    case kFixtureHub:
      return "hub";
  }
  return "random";
}

TestbenchOptions parse_args(int argc, char** argv) {
  TestbenchOptions opt;
  for (int i = 1; i < argc; ++i) {
    const char* arg = argv[i];
    if (std::strcmp(arg, "--pressure-only") == 0) {
      opt.pressure_only = true;
    } else if (std::strcmp(arg, "--help") == 0) {
      opt.show_help = true;
    } else if (starts_with(arg, "--profile=")) {
      opt.expected_profile = arg + std::strlen("--profile=");
    } else if (std::strcmp(arg, "--profile") == 0 && i + 1 < argc) {
      opt.expected_profile = argv[++i];
    } else if (starts_with(arg, "--tiles=")) {
      opt.tile_count = parse_u16_list(arg + std::strlen("--tiles="), opt.tiles);
    } else if (std::strcmp(arg, "--tiles") == 0 && i + 1 < argc) {
      opt.tile_count = parse_u16_list(argv[++i], opt.tiles);
    } else if (starts_with(arg, "--capacity-sweep=")) {
      const char* value = arg + std::strlen("--capacity-sweep=");
      opt.expect_capacity_threshold = false;
      opt.capacity_count =
          std::strcmp(value, "none") == 0 ? 0 : parse_u16_list(value, opt.capacities);
    } else if (starts_with(arg, "--fixtures=")) {
      opt.fixture_count =
          parse_fixture_list(arg + std::strlen("--fixtures="), opt.fixtures);
    } else if (std::strcmp(arg, "--fixtures") == 0 && i + 1 < argc) {
      opt.fixture_count = parse_fixture_list(argv[++i], opt.fixtures);
    } else if (std::strcmp(arg, "--capacity-sweep") == 0 && i + 1 < argc) {
      const char* value = argv[++i];
      opt.expect_capacity_threshold = false;
      opt.capacity_count =
          std::strcmp(value, "none") == 0 ? 0 : parse_u16_list(value, opt.capacities);
    }
  }
  if (opt.tile_count == 0) {
    opt.tiles[0] = 1;
    opt.tile_count = 1;
  }
  if (opt.fixture_count == 0) {
    opt.fixtures[0] = kFixtureRandom;
    opt.fixture_count = 1;
  }
  return opt;
}

void print_help() {
  std::cout
      << "hls_tb [--pressure-only] [--tiles=1,2,4] "
      << "[--capacity-sweep=64,128,256,512,1024|none] "
      << "[--fixtures=chain,random,hub] "
      << "[--profile=" << HLS_PROFILE_NAME << "]\n";
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
  if (MAX_REVISE_TILES > 1) {
    assert(results[0].epochs < results[0].events);
  } else {
    assert(results[0].epochs <= results[0].events);
  }
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

void add_pressure_constraint(word_t bit_sup[kBitSupWords],
                             ConstraintHls constraints[MAX_CONSTRAINTS],
                             uint16_t constraint_partition[MAX_CONSTRAINTS],
                             const uint16_t var_partition[MAX_VARS],
                             uint16_t partition_load[MAX_PARTITIONS],
                             uint16_t* cid, uint16_t x, uint16_t y,
                             uint32_t* offset) {
  if (*cid >= MAX_CONSTRAINTS) {
    return;
  }
  add_equality_constraint(bit_sup, constraints, *cid, x, y, kPressureDomain,
                          offset);
  const uint16_t px = var_partition[x];
  const uint16_t py = var_partition[y];
  const uint16_t owner = partition_load[px] <= partition_load[py] ? px : py;
  constraint_partition[*cid] = owner;
  ++partition_load[owner];
  ++(*cid);
}

uint16_t build_pressure_case(
    FixtureKind fixture,
    word_t bit_sup[kBitSupWords],
    ConstraintHls constraints[MAX_CONSTRAINTS],
    SubscriptionHls subscriptions[MAX_VARS],
    uint16_t domain_size[MAX_VARS],
    uint16_t var_partition[MAX_VARS],
    uint16_t constraint_partition[MAX_CONSTRAINTS],
    ProbeTaskHls tasks[MAX_WORLDS],
    ResultHls results[MAX_WORLDS]) {
  reset_inputs(bit_sup, constraints, subscriptions, domain_size, var_partition,
               constraint_partition, tasks, results);
  uint16_t partition_load[MAX_PARTITIONS] = {};
  for (uint16_t var = 0; var < kPressureVars; ++var) {
    domain_size[var] = kPressureDomain;
    var_partition[var] = static_cast<uint16_t>(var / 32u);
  }

  uint16_t cid = 0;
  uint32_t offset = 0;
  if (fixture == kFixtureChain) {
    for (uint16_t x = 0; x + 1u < kPressureVars; ++x) {
      add_pressure_constraint(bit_sup, constraints, constraint_partition,
                              var_partition, partition_load, &cid, x,
                              static_cast<uint16_t>(x + 1u), &offset);
    }
  } else if (fixture == kFixtureHub) {
    for (uint16_t y = 1; y < kPressureVars; ++y) {
      add_pressure_constraint(bit_sup, constraints, constraint_partition,
                              var_partition, partition_load, &cid, 0, y,
                              &offset);
    }
  } else {
    for (uint16_t x = 0; x < kPressureVars; ++x) {
      for (uint16_t y = static_cast<uint16_t>(x + 1u); y < kPressureVars; ++y) {
        if (((x * 53u + y * 97u + 11u) % 10u) != 0) {
          continue;
        }
        add_pressure_constraint(bit_sup, constraints, constraint_partition,
                                var_partition, partition_load, &cid, x, y,
                                &offset);
      }
    }
  }

  build_subscriptions(constraints, cid, subscriptions);
  tasks[0] = ProbeTaskHls{1, 0, 7};
  return cid;
}

ResultHls run_pressure_case(FixtureKind fixture, uint16_t revise_tiles,
                            uint16_t queue_capacity,
                            uint16_t* constraints_built) {
  word_t bit_sup[kBitSupWords] = {};
  ConstraintHls constraints[MAX_CONSTRAINTS] = {};
  SubscriptionHls subscriptions[MAX_VARS] = {};
  uint16_t domain_size[MAX_VARS] = {};
  uint16_t var_partition[MAX_VARS] = {};
  uint16_t constraint_partition[MAX_CONSTRAINTS] = {};
  ProbeTaskHls tasks[MAX_WORLDS] = {};
  ResultHls results[MAX_WORLDS] = {};
  const uint16_t cid = build_pressure_case(
      fixture,
      bit_sup, constraints, subscriptions, domain_size, var_partition,
      constraint_partition, tasks, results);

  ControlHls control = make_control(kPressureVars, cid, kPressureDomain);
  control.num_partitions = kPressurePartitions;
  control.num_revise_tiles = revise_tiles;
  control.partition_queue_capacity = queue_capacity;
  control.max_events = 300000;
  control.max_revise = 300000;
  control.max_epochs = 300000;
  cpim_top_hls(bit_sup, constraints, subscriptions, domain_size, var_partition,
               constraint_partition, tasks, results, control);
  *constraints_built = cid;
  return results[0];
}

void print_trace_row(const char* prefix, FixtureKind fixture,
                     uint16_t revise_tiles,
                     uint16_t queue_capacity, uint16_t constraints,
                     const ResultHls& result) {
  std::cout << prefix
            << " profile=" << HLS_PROFILE_NAME
            << " graph=" << fixture_name(fixture)
            << " vars=" << kPressureVars
            << " domain=" << kPressureDomain
            << " density=0.10"
            << " partitions=" << kPressurePartitions
            << " tiles=" << revise_tiles
            << " capacity=" << queue_capacity
            << " constraints=" << constraints
            << " status=" << status_name(result.status)
            << " events=" << result.events
            << " epochs=" << result.epochs
            << " tile_steps=" << result.tile_steps
            << " queue_peak_total=" << result.queue_peak_total
            << " queue_peak_partition=" << result.queue_peak_partition
            << " local_events=" << result.local_events
            << " cross_events=" << result.cross_events
            << " deleted_values=" << result.deleted_values
            << " router_overflow=" << result.router_overflow
            << "\n";
}

void run_density_010_pressure_smoke(const TestbenchOptions& opt) {
  for (uint16_t fixture_index = 0; fixture_index < kMaxCliValues; ++fixture_index) {
    if (fixture_index >= opt.fixture_count) {
      break;
    }
    const FixtureKind fixture = opt.fixtures[fixture_index];
    ResultHls previous;
    uint16_t previous_constraints = 0;
    uint16_t previous_tiles = 0;
    bool have_previous = false;
    for (uint16_t i = 0; i < kMaxCliValues; ++i) {
      if (i >= opt.tile_count) {
        break;
      }
      uint16_t constraints = 0;
      const ResultHls result =
          run_pressure_case(fixture, opt.tiles[i], MAX_PARTITION_QUEUE,
                            &constraints);
      print_trace_row("hls_pressure", fixture, opt.tiles[i], MAX_PARTITION_QUEUE,
                      constraints, result);

      if (fixture == kFixtureRandom) {
        if (HLS_PROFILE_IS_STRESS128) {
          assert(constraints >= 750);
          assert(constraints <= 850);
        } else {
          assert(constraints == MAX_CONSTRAINTS);
        }
      } else {
        assert(constraints == 127);
      }
      assert(result.status == OK);
      assert(result.deleted_values > 0);
      assert(result.events > 0);
      assert(result.queue_peak_total > 0);
      assert(result.queue_peak_partition < MAX_PARTITION_QUEUE);
      if (fixture != kFixtureChain) {
        assert(result.cross_events > 0);
      }
      if (have_previous) {
        assert(constraints == previous_constraints);
        assert(result.deleted_values == previous.deleted_values);
        assert(result.events == previous.events);
        if (opt.tiles[i] >= previous_tiles) {
          assert(result.epochs <= previous.epochs);
        }
      }
      previous = result;
      previous_constraints = constraints;
      previous_tiles = opt.tiles[i];
      have_previous = true;
    }
  }
}

void run_capacity_sweep(const TestbenchOptions& opt) {
  if (opt.capacity_count == 0) {
    return;
  }
  const uint16_t revise_tiles = opt.tiles[opt.tile_count - 1u];
  bool saw_unknown = false;
  bool saw_ok = false;
  for (uint16_t fixture_index = 0; fixture_index < kMaxCliValues; ++fixture_index) {
    if (fixture_index >= opt.fixture_count) {
      break;
    }
    const FixtureKind fixture = opt.fixtures[fixture_index];
    for (uint16_t i = 0; i < kMaxCliValues; ++i) {
      if (i >= opt.capacity_count) {
        break;
      }
      uint16_t constraints = 0;
      const ResultHls result =
          run_pressure_case(fixture, revise_tiles, opt.capacities[i],
                            &constraints);
      print_trace_row("hls_capacity", fixture, revise_tiles, opt.capacities[i],
                      constraints, result);
      if (fixture == kFixtureRandom) {
        if (HLS_PROFILE_IS_STRESS128) {
          assert(constraints >= 750);
          assert(constraints <= 850);
        } else {
          assert(constraints == MAX_CONSTRAINTS);
        }
      } else {
        assert(constraints == 127);
      }
      if (result.status == UNKNOWN) {
        saw_unknown = true;
        assert(result.router_overflow == 1);
      }
      if (result.status == OK) {
        saw_ok = true;
        assert(result.router_overflow == 0);
      }
    }
  }
  if (opt.expect_capacity_threshold) {
    assert(saw_unknown);
    assert(saw_ok);
  }
}

}  // namespace

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
int main(int argc, char** argv) {
  const TestbenchOptions opt = parse_args(argc, argv);
  if (opt.show_help) {
    print_help();
    return 0;
  }
  if (!profile_matches(opt.expected_profile)) {
    std::cerr << "profile mismatch: binary=" << HLS_PROFILE_NAME
              << " requested=" << opt.expected_profile << "\n";
    return 2;
  }
  if (!opt.pressure_only) {
    run_equality_ok();
    run_less_than_dwo();
    run_budget_unknown();
    run_multi_tile_partition_smoke();
    run_partition_queue_overflow_unknown();
  }
  run_density_010_pressure_smoke(opt);
  run_capacity_sweep(opt);
  if (!opt.pressure_only) {
    std::cout << "hls_tb ok\n";
  }
  return 0;
}
