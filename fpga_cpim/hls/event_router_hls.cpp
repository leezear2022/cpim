#include "cpim_hls_types.hpp"

namespace fpga_cpim_hls {

bool hls_enqueue(DirtyEventHls queue[MAX_QUEUE], uint16_t* tail,
                 DirtyEventHls event) {
  if (*tail >= MAX_QUEUE) {
    return false;
  }
  queue[*tail] = event;
  ++(*tail);
  return true;
}

bool hls_enqueue_partition(
    DirtyEventHls queues[MAX_PARTITIONS][MAX_PARTITION_QUEUE],
    uint16_t heads[MAX_PARTITIONS], uint16_t tails[MAX_PARTITIONS],
    uint16_t counts[MAX_PARTITIONS], uint16_t partition, uint16_t capacity,
    DirtyEventHls event, uint32_t* total_occupancy, uint32_t* queue_peak_total,
    uint16_t* queue_peak_partition) {
  (void)heads;
  if (capacity == 0 || capacity > MAX_PARTITION_QUEUE) {
    capacity = MAX_PARTITION_QUEUE;
  }
  if (partition >= MAX_PARTITIONS) {
    partition = 0;
  }
  if (counts[partition] >= capacity) {
    return false;
  }
  queues[partition][tails[partition]] = event;
  ++tails[partition];
  if (tails[partition] >= capacity) {
    tails[partition] = 0;
  }
  ++counts[partition];
  ++(*total_occupancy);
  if (*total_occupancy > *queue_peak_total) {
    *queue_peak_total = *total_occupancy;
  }
  if (counts[partition] > *queue_peak_partition) {
    *queue_peak_partition = counts[partition];
  }
  return true;
}

bool hls_dequeue_partition(
    DirtyEventHls queues[MAX_PARTITIONS][MAX_PARTITION_QUEUE],
    uint16_t heads[MAX_PARTITIONS], uint16_t tails[MAX_PARTITIONS],
    uint16_t counts[MAX_PARTITIONS], uint16_t* rr_partition,
    uint16_t num_partitions, uint16_t capacity, DirtyEventHls* event,
    uint32_t* total_occupancy) {
  (void)tails;
  if (num_partitions == 0 || num_partitions > MAX_PARTITIONS) {
    num_partitions = MAX_PARTITIONS;
  }
  if (capacity == 0 || capacity > MAX_PARTITION_QUEUE) {
    capacity = MAX_PARTITION_QUEUE;
  }
  for (uint16_t step = 0; step < MAX_PARTITIONS; ++step) {
    if (step >= num_partitions) {
      break;
    }
    uint16_t part =
        static_cast<uint16_t>((*rr_partition + step) % num_partitions);
    if (counts[part] == 0) {
      continue;
    }
    *event = queues[part][heads[part]];
    ++heads[part];
    if (heads[part] >= capacity) {
      heads[part] = 0;
    }
    --counts[part];
    --(*total_occupancy);
    *rr_partition = static_cast<uint16_t>((part + 1u) % num_partitions);
    return true;
  }
  return false;
}

}  // namespace fpga_cpim_hls
