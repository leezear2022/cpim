#pragma once

#include <cstdint>
#include <vector>

#include "absl/status/statusor.h"

namespace cpim::model {

class IntermediateModel;

struct DeviceInt2 {
  int32_t x = -1;
  int32_t y = -1;
};

struct DeviceUInt2 {
  uint32_t x = 0;
  uint32_t y = 0;
};

struct DeviceUInt3 {
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t z = 0;
};

struct DeviceSubscriptionCSR {
  std::vector<int32_t> offsets;
  std::vector<DeviceUInt3> entries;
};

struct DeviceModelLayout {
  int32_t num_vars = 0;
  int32_t num_constraints = 0;
  int32_t max_dom_size = 0;
  int32_t bit_words = 0;

  std::vector<uint32_t> bit_dom;
  std::vector<int32_t> domain_sizes;
  std::vector<DeviceUInt2> bit_sup;
  std::vector<uint32_t> bit_sup_words;
  std::vector<DeviceInt2> constraint_scopes;
  DeviceSubscriptionCSR subscriptions;

  int32_t bitsup_entries_per_constraint() const {
    return 2 * max_dom_size * bit_words;
  }
};

absl::StatusOr<DeviceModelLayout> BuildDeviceLayoutFromIntermediate(
    const IntermediateModel& model);

}  // namespace cpim::model
