#ifndef FPGA_CPIM_HLS_TYPES_HPP_
#define FPGA_CPIM_HLS_TYPES_HPP_

#include <cstdint>

namespace fpga_cpim_hls {

#define FPGA_CPIM_HLS_PROFILE_STRESS128 0
#define FPGA_CPIM_HLS_PROFILE_Z7020_SMALL 1
#define FPGA_CPIM_HLS_PROFILE_Z7020_PROBE2 2

#ifndef FPGA_CPIM_HLS_PROFILE
#define FPGA_CPIM_HLS_PROFILE FPGA_CPIM_HLS_PROFILE_STRESS128
#endif

#if FPGA_CPIM_HLS_PROFILE == FPGA_CPIM_HLS_PROFILE_Z7020_SMALL
constexpr int MAX_VARS = 128;
constexpr int MAX_CONSTRAINTS = 512;
constexpr int MAX_DOMAIN = 32;
constexpr int MAX_WORDS = 1;
constexpr int MAX_WORLDS = 1;
constexpr int MAX_QUEUE = 1024;
constexpr int MAX_PARTITIONS = 4;
constexpr int MAX_PARTITION_QUEUE = 512;
constexpr int MAX_REVISE_TILES = 1;
constexpr const char* HLS_PROFILE_NAME = "z7020_small";
constexpr bool HLS_PROFILE_IS_STRESS128 = false;
#elif FPGA_CPIM_HLS_PROFILE == FPGA_CPIM_HLS_PROFILE_Z7020_PROBE2
constexpr int MAX_VARS = 128;
constexpr int MAX_CONSTRAINTS = 256;
constexpr int MAX_DOMAIN = 32;
constexpr int MAX_WORDS = 1;
constexpr int MAX_WORLDS = 2;
constexpr int MAX_QUEUE = 1024;
constexpr int MAX_PARTITIONS = 4;
constexpr int MAX_PARTITION_QUEUE = 512;
constexpr int MAX_REVISE_TILES = 1;
constexpr const char* HLS_PROFILE_NAME = "z7020_probe2";
constexpr bool HLS_PROFILE_IS_STRESS128 = false;
#elif FPGA_CPIM_HLS_PROFILE == FPGA_CPIM_HLS_PROFILE_STRESS128
constexpr int MAX_VARS = 256;
constexpr int MAX_CONSTRAINTS = 1024;
constexpr int MAX_DOMAIN = 128;
constexpr int MAX_WORDS = 4;
constexpr int MAX_WORLDS = 4;
constexpr int MAX_QUEUE = 4096;
constexpr int MAX_PARTITIONS = 8;
constexpr int MAX_PARTITION_QUEUE = 1024;
constexpr int MAX_REVISE_TILES = 4;
constexpr const char* HLS_PROFILE_NAME = "stress128";
constexpr bool HLS_PROFILE_IS_STRESS128 = true;
#else
#error "Unsupported FPGA_CPIM_HLS_PROFILE"
#endif

using word_t = uint32_t;

#if defined(__SYNTHESIS__)
#define FPGA_CPIM_HLS_PRAGMA(x) _Pragma(#x)
#else
#define FPGA_CPIM_HLS_PRAGMA(x)
#endif

enum Status : uint8_t {
  OK = 0,
  DWO = 1,
  UNKNOWN = 2
};

struct ConstraintHls {
  uint16_t x = 0;
  uint16_t y = 0;
  uint16_t x_domain_size = 0;
  uint16_t y_domain_size = 0;
  uint32_t bit_sup_offset_dir0 = 0;
  uint32_t bit_sup_offset_dir1 = 0;
};

struct SubscriptionHls {
  uint16_t count = 0;
  uint16_t cids[MAX_CONSTRAINTS] = {};
};

struct DirtyEventHls {
  uint16_t world = 0;
  uint16_t cid = 0;
  uint8_t dirs = 0;
};

struct DeleteEventHls {
  uint16_t world = 0;
  uint16_t var = 0;
  word_t del_words[MAX_WORDS] = {};
};

struct ProbeTaskHls {
  uint8_t active = 0;
  uint16_t var = 0;
  uint16_t value = 0;
};

struct ResultHls {
  Status status = OK;
  uint32_t events = 0;
  uint32_t revise_calls = 0;
  uint32_t deleted_values = 0;
  uint32_t epochs = 0;
  uint32_t tile_steps = 0;
  uint32_t local_events = 0;
  uint32_t cross_events = 0;
  uint32_t queue_peak_total = 0;
  uint16_t queue_peak_partition = 0;
  uint16_t router_overflow = 0;
};

struct ControlHls {
  uint16_t num_vars = 0;
  uint16_t num_constraints = 0;
  uint16_t max_domain_size = 0;
  uint32_t max_events = 100000;
  uint32_t max_revise = 100000;
  uint32_t max_epochs = 1000;
  uint16_t num_partitions = 1;
  uint16_t num_revise_tiles = 1;
  uint16_t partition_queue_capacity = MAX_PARTITION_QUEUE;
};

inline uint16_t word_count(uint16_t bits) {
  return static_cast<uint16_t>((bits + 31u) / 32u);
}

inline word_t last_word_mask(uint16_t bits) {
  const uint16_t rem = bits % 32u;
  if (bits == 0) {
    return 0;
  }
  if (rem == 0) {
    return 0xffffffffu;
  }
  return (word_t{1} << rem) - 1u;
}

void cpim_top_hls(
    const word_t bit_sup[MAX_CONSTRAINTS * MAX_DOMAIN * MAX_WORDS * 2],
    const ConstraintHls constraints[MAX_CONSTRAINTS],
    const SubscriptionHls subscriptions[MAX_VARS],
    const uint16_t domain_size[MAX_VARS],
    const uint16_t var_partition[MAX_VARS],
    const uint16_t constraint_partition[MAX_CONSTRAINTS],
    const ProbeTaskHls tasks[MAX_WORLDS],
    ResultHls results[MAX_WORLDS],
    ControlHls control);

bool hls_enqueue(DirtyEventHls queue[MAX_QUEUE], uint16_t* tail,
                 DirtyEventHls event);

bool hls_enqueue_partition(
    DirtyEventHls queues[MAX_PARTITIONS][MAX_PARTITION_QUEUE],
    uint16_t heads[MAX_PARTITIONS], uint16_t tails[MAX_PARTITIONS],
    uint16_t counts[MAX_PARTITIONS], uint16_t partition, uint16_t capacity,
    DirtyEventHls event, uint32_t* total_occupancy, uint32_t* queue_peak_total,
    uint16_t* queue_peak_partition);

bool hls_dequeue_partition(
    DirtyEventHls queues[MAX_PARTITIONS][MAX_PARTITION_QUEUE],
    uint16_t heads[MAX_PARTITIONS], uint16_t tails[MAX_PARTITIONS],
    uint16_t counts[MAX_PARTITIONS], uint16_t* rr_partition,
    uint16_t num_partitions, uint16_t capacity, DirtyEventHls* event,
    uint32_t* total_occupancy);

bool hls_apply_delete(word_t domains[MAX_VARS][MAX_WORDS], uint16_t var,
                      uint16_t domain_bits, const word_t del_words[MAX_WORDS],
                      uint32_t* deleted_count);

void hls_revise_constraint(
    const word_t bit_sup[MAX_CONSTRAINTS * MAX_DOMAIN * MAX_WORDS * 2],
    const ConstraintHls& c, uint8_t dir,
    word_t domains[MAX_VARS][MAX_WORDS], DeleteEventHls* del,
    bool* dwo, uint32_t* words_touched);

}  // namespace fpga_cpim_hls

#endif  // FPGA_CPIM_HLS_TYPES_HPP_
