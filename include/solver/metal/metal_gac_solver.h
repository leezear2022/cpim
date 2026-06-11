#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "model/device_layout.h"

namespace cpim::solver::metal {

enum class MetalReadonlyStorageMode {
  kShared,
  kPrivate,
};

enum class MetalFrontierMode {
  kFlags,
  kCompact,
  kWorklist,
  kCtaWorklist,
  kBulkSyncMask,
  kAuto,
};

enum class MetalRunnerMode {
  kCold,
  kPrepared,
};

enum class MetalKernelVariant {
  kScalar,
  kWordParallel,
  kSimdgroup,
  kAuto,
};

enum class MetalBitSupLayout {
  kPair,
  kDirectional,
  kAuto,
};

enum class MetalResetMode {
  kCpu,
  kBlit,
  kAuto,
};

enum class MetalCtaOwnerMode {
  kModulo,
  kStaticEdgeCut,
  kVeboWeighted,
};

enum class MetalCtaQueueMode {
  kLocalOnly,
  kSpillReplay,
  kBoundedReplay,
};

enum class MetalCtaHandoffMode {
  kPushConstraints,
  kDirtyVarPull,
};

struct MetalGacOptions {
  std::string metallib_path;
  int max_iterations = 10000;
  bool verbose = false;
  MetalRunnerMode runner_mode = MetalRunnerMode::kCold;
  MetalReadonlyStorageMode readonly_storage = MetalReadonlyStorageMode::kShared;
  MetalFrontierMode frontier_mode = MetalFrontierMode::kFlags;
  MetalKernelVariant kernel_variant = MetalKernelVariant::kScalar;
  MetalBitSupLayout bitsup_layout = MetalBitSupLayout::kPair;
  MetalResetMode reset_mode = MetalResetMode::kCpu;
  MetalCtaOwnerMode cta_owner_mode = MetalCtaOwnerMode::kModulo;
  MetalCtaQueueMode cta_queue_mode = MetalCtaQueueMode::kLocalOnly;
  MetalCtaHandoffMode cta_handoff_mode = MetalCtaHandoffMode::kPushConstraints;
  int cta_local_round_budget = 8;
  int cta_replay_round_budget = 8;
  int cta_dirty_pull_min_degree = 0;
};

struct MetalGacStats {
  int iterations = 0;
  int deletions = 0;
  int dispatch_count = 0;
  bool inconsistent = false;
  bool budget_exceeded = false;
  double elapsed_ms = 0.0;
  double solve_ms = 0.0;
  double setup_ms = 0.0;
  double prepare_ms = 0.0;
  double reset_ms = 0.0;
  double reset_dispatch_ms = 0.0;
  double dispatch_ms = 0.0;
  double dispatch_encode_ms = 0.0;
  double dispatch_wait_ms = 0.0;
  double dispatch_non_kernel_ms = 0.0;
  double kernel_ms = 0.0;
  int active_constraints_total = 0;
  int worklist_push_count = 0;
  int worklist_rounds = 0;
  int worklist_epoch_resets = 0;
  int cta_local_rounds = 0;
  int cta_queue_push_count = 0;
  int cta_cross_push_count = 0;
  int cta_overflow_count = 0;
  int cta_queue_overflow_count = 0;
  int cta_budget_spill_count = 0;
  int cta_seed_overflow_count = 0;
  int cta_budget_replay_rounds = 0;
  int cta_budget_replay_drain_count = 0;
  int cta_budget_replay_spill_count = 0;
  int host_round_count = 0;
  int bulk_mask_proposed_deletion_count = 0;
  int bulk_mask_actual_deletion_count = 0;
  int bulk_mask_changed_word_count = 0;
  int bulk_mask_frontier_push_count = 0;
  int bulk_mask_rounds = 0;
  int dirty_var_count = 0;
  int dirty_pull_scan_count = 0;
  int dirty_pull_hit_count = 0;
  int cross_push_avoided_count = 0;
  int dirty_pull_fallback_push_count = 0;
  int owner_local_push_count = 0;
  int owner_cross_push_count = 0;
  int seed_owner_nonempty_count = 0;
  int seed_empty_owner_count = 0;
  int seed_max_owner_load = 0;
  double owner_map_build_ms = 0.0;
  double owner_balance_p95 = 0.0;
  double owner_weight_balance_p95 = 0.0;
  double seed_owner_balance_p95 = 0.0;
  double frontier_density_avg = 0.0;
  bool gpu_timing_available = false;
  std::string device_name;
  std::string runner_mode;
  std::string readonly_storage;
  std::string frontier_mode;
  std::string kernel_variant;
  std::string bitsup_layout;
  std::string reset_mode;
  std::string cta_owner_mode;
  std::string cta_queue_mode;
  std::string cta_handoff_mode;
  int cta_local_round_budget = 0;
  int cta_replay_round_budget = 0;
  int cta_dirty_pull_min_degree = 0;
  std::string effective_frontier_mode;
  std::string effective_kernel_variant;
  std::string effective_bitsup_layout;
  std::string variant_name;
};

class MetalGacSolver {
 public:
  struct Impl;

  MetalGacSolver(const cpim::model::DeviceModelLayout& layout,
                 MetalGacOptions options);
  ~MetalGacSolver();

  MetalGacSolver(MetalGacSolver&&) noexcept;
  MetalGacSolver& operator=(MetalGacSolver&&) noexcept;

  MetalGacSolver(const MetalGacSolver&) = delete;
  MetalGacSolver& operator=(const MetalGacSolver&) = delete;

  absl::StatusOr<MetalGacStats> Run();

  const std::vector<uint32_t>& bit_dom() const;
  const std::vector<int32_t>& domain_sizes() const;

 private:
  std::unique_ptr<Impl> impl_;
};

class MetalPreparedGacRunner {
 public:
  MetalPreparedGacRunner(const cpim::model::DeviceModelLayout& layout,
                         MetalGacOptions options);
  ~MetalPreparedGacRunner();

  MetalPreparedGacRunner(MetalPreparedGacRunner&&) noexcept;
  MetalPreparedGacRunner& operator=(MetalPreparedGacRunner&&) noexcept;

  MetalPreparedGacRunner(const MetalPreparedGacRunner&) = delete;
  MetalPreparedGacRunner& operator=(const MetalPreparedGacRunner&) = delete;

  absl::Status Prepare();
  absl::StatusOr<MetalGacStats> Run();

  const std::vector<uint32_t>& bit_dom() const;
  const std::vector<int32_t>& domain_sizes() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cpim::solver::metal
