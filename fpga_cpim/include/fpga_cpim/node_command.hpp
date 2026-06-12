#ifndef FPGA_CPIM_NODE_COMMAND_HPP_
#define FPGA_CPIM_NODE_COMMAND_HPP_

#include <cstdint>
#include <vector>

#include "fpga_cpim/bitset.hpp"
#include "fpga_cpim/engine.hpp"
#include "fpga_cpim/model.hpp"
#include "fpga_cpim/world.hpp"

namespace fpga_cpim {

struct Budget {
  uint64_t max_events = 100000;
  uint64_t max_probe_events = 100000;
  uint64_t max_revise = 100000;
  uint64_t max_epochs = 1000;
  uint64_t max_support_words = 1000000;
  uint32_t max_pending_events_per_world = 4096;
  uint32_t max_pending_per_cid = 4;
};

struct NodeCommand {
  enum Mode {
    kRunAC,
    kRunACThenNSACQ,
    kRunBranchProbes
  };

  enum NsacqInit {
    kNone,
    kAllVars,
    kFocalVar,
    kNeighborhoodOfFocal
  };

  Mode mode = kRunAC;
  bool has_assignment = false;
  uint32_t branch_var = 0;
  uint32_t branch_value = 0;

  std::vector<uint32_t> seed_vars;

  NsacqInit nsacq_init = kNone;
  uint32_t focal_var = 0;
  uint32_t nsac_radius = 0;

  Budget budget;
};

enum IncompleteReason : uint32_t {
  kIncompleteNone = 0,
  kAcBudget = 1u << 0,
  kNsacqQueueBudget = 1u << 1,
  kProbeBudget = 1u << 2,
  kVarQueueOverflow = 1u << 3,
  kProbeQueueOverflow = 1u << 4,
  kRouterOverflow = 1u << 5,
  kDeadlockGuard = 1u << 6
};

struct DeletionMask {
  VarId var = 0;
  DomainMask mask;
};

struct NodeStats {
  uint64_t ac_events = 0;
  uint64_t ac_revise_calls = 0;
  uint64_t nsacq_probes = 0;
  uint64_t nsacq_probe_events = 0;
  uint64_t queue_occupancy_max = 0;
  uint64_t router_events_dropped_overflow = 0;
};

struct NodeResult {
  enum Status {
    kDWO,
    kAliveComplete,
    kAliveIncomplete
  };

  Status status = kAliveComplete;
  bool ac_complete = true;
  bool nsacq_complete = true;
  bool has_confirmed_deletions = false;

  std::vector<DeletionMask> confirmed_deletions;
  uint32_t unknown_probe_count = 0;
  uint32_t incomplete_reason_mask = kIncompleteNone;
  NodeStats stats;
  std::vector<DomainMask> final_domains;
};

NodeResult RunNodeCommand(const Model& model, const NodeCommand& command,
                          const std::vector<DomainMask>& base_domains = {},
                          const EngineConfig& base_engine_cfg = EngineConfig{});

}  // namespace fpga_cpim

#endif  // FPGA_CPIM_NODE_COMMAND_HPP_
