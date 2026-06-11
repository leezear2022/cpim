#include "solver/metal/metal_gac_solver.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "solver/metal/metal_runtime.h"

namespace cpim::solver::metal {
namespace {

struct GacParamsMetal {
  int32_t num_constraints = 0;
  int32_t max_dom_size = 0;
  int32_t bit_words = 0;
  int32_t num_vars = 0;
  int32_t active_count = 0;
  int32_t frontier_epoch = 0;
  int32_t cta_count = 0;
  int32_t cta_queue_capacity = 0;
  int32_t cta_local_round_budget = 0;
  int32_t cta_replay_round_budget = 0;
  int32_t threads_per_cta = 0;
  int32_t cta_queue_mode = 0;
  int32_t cta_handoff_mode = 0;
  int32_t cta_dirty_pull_min_degree = 0;
};

struct CompactParamsMetal {
  int32_t num_constraints = 0;
};

constexpr int kStatsWords = 23;
constexpr int kCtaLocalRoundBudget = 8;
constexpr int kMaxCtaRoundBudget = 256;
constexpr int kMaxCtaCount = 32;
constexpr int kCtaQueueModeLocalOnly = 0;
constexpr int kCtaQueueModeSpillReplay = 1;
constexpr int kCtaQueueModeBoundedReplay = 2;
constexpr int kCtaHandoffModePushConstraints = 0;
constexpr int kCtaHandoffModeDirtyVarPull = 1;

template <typename T>
absl::StatusOr<MetalBuffer> NewBufferWithVector(
    const MetalRuntime& runtime, const std::vector<T>& values,
    const std::string& label, MetalReadonlyStorageMode storage_mode) {
  const size_t bytes = values.size() * sizeof(T);
  const void* data = values.empty() ? nullptr : values.data();
  if (storage_mode == MetalReadonlyStorageMode::kPrivate) {
    return runtime.NewPrivateBufferWithBytes(data, bytes, label);
  }
  return runtime.NewSharedBufferWithBytes(data, bytes, label);
}

template <typename T>
absl::StatusOr<MetalBuffer> NewSharedBufferWithVector(
    const MetalRuntime& runtime, const std::vector<T>& values,
    const std::string& label) {
  return NewBufferWithVector(runtime, values, label,
                             MetalReadonlyStorageMode::kShared);
}

void AddTiming(MetalGacStats* stats, const MetalDispatchTimings& timings) {
  stats->dispatch_ms += timings.wall_ms;
  stats->dispatch_encode_ms += timings.encode_ms;
  stats->dispatch_wait_ms += timings.wall_ms;
  if (timings.gpu_timing_available) {
    stats->kernel_ms += timings.kernel_ms;
    stats->dispatch_non_kernel_ms +=
        std::max(0.0, timings.wall_ms - timings.kernel_ms);
    stats->gpu_timing_available = true;
  }
}

std::string RunnerModeName(MetalRunnerMode mode) {
  return mode == MetalRunnerMode::kPrepared ? "prepared" : "cold";
}

std::string ReadonlyStorageName(MetalReadonlyStorageMode mode) {
  return mode == MetalReadonlyStorageMode::kPrivate ? "private" : "shared";
}

std::string FrontierModeName(MetalFrontierMode mode) {
  switch (mode) {
    case MetalFrontierMode::kCompact:
      return "compact";
    case MetalFrontierMode::kWorklist:
      return "worklist";
    case MetalFrontierMode::kCtaWorklist:
      return "cta_worklist";
    case MetalFrontierMode::kBulkSyncMask:
      return "bulk_sync_mask";
    case MetalFrontierMode::kAuto:
      return "auto";
    case MetalFrontierMode::kFlags:
    default:
      return "flags";
  }
}

std::string KernelVariantName(MetalKernelVariant variant) {
  switch (variant) {
    case MetalKernelVariant::kWordParallel:
      return "word_parallel";
    case MetalKernelVariant::kSimdgroup:
      return "simdgroup";
    case MetalKernelVariant::kAuto:
      return "auto";
    case MetalKernelVariant::kScalar:
    default:
      return "scalar";
  }
}

std::string BitSupLayoutName(MetalBitSupLayout layout) {
  switch (layout) {
    case MetalBitSupLayout::kDirectional:
      return "directional";
    case MetalBitSupLayout::kAuto:
      return "auto";
    case MetalBitSupLayout::kPair:
    default:
      return "pair";
  }
}

std::string ResetModeName(MetalResetMode mode) {
  switch (mode) {
    case MetalResetMode::kBlit:
      return "blit";
    case MetalResetMode::kAuto:
      return "auto";
    case MetalResetMode::kCpu:
    default:
      return "cpu";
  }
}

std::string CtaOwnerModeName(MetalCtaOwnerMode mode) {
  switch (mode) {
    case MetalCtaOwnerMode::kVeboWeighted:
      return "vebo_weighted";
    case MetalCtaOwnerMode::kStaticEdgeCut:
      return "static_edge_cut";
    case MetalCtaOwnerMode::kModulo:
    default:
      return "modulo";
  }
}

std::string CtaQueueModeName(MetalCtaQueueMode mode) {
  switch (mode) {
    case MetalCtaQueueMode::kBoundedReplay:
      return "bounded_replay";
    case MetalCtaQueueMode::kSpillReplay:
      return "spill_replay";
    case MetalCtaQueueMode::kLocalOnly:
    default:
      return "local_only";
  }
}

std::string CtaHandoffModeName(MetalCtaHandoffMode mode) {
  switch (mode) {
    case MetalCtaHandoffMode::kDirtyVarPull:
      return "dirty_var_pull";
    case MetalCtaHandoffMode::kPushConstraints:
    default:
      return "push_constraints";
  }
}

int32_t CtaQueueModeValue(MetalCtaQueueMode mode) {
  switch (mode) {
    case MetalCtaQueueMode::kBoundedReplay:
      return kCtaQueueModeBoundedReplay;
    case MetalCtaQueueMode::kSpillReplay:
      return kCtaQueueModeSpillReplay;
    case MetalCtaQueueMode::kLocalOnly:
    default:
      return kCtaQueueModeLocalOnly;
  }
}

int32_t CtaHandoffModeValue(MetalCtaHandoffMode mode) {
  switch (mode) {
    case MetalCtaHandoffMode::kDirtyVarPull:
      return kCtaHandoffModeDirtyVarPull;
    case MetalCtaHandoffMode::kPushConstraints:
    default:
      return kCtaHandoffModePushConstraints;
  }
}

int CtaLocalRoundBudget(const MetalGacOptions& options) {
  return std::clamp(options.cta_local_round_budget, 1, kMaxCtaRoundBudget);
}

int CtaReplayRoundBudget(const MetalGacOptions& options) {
  if (options.cta_queue_mode != MetalCtaQueueMode::kBoundedReplay) {
    return 0;
  }
  return std::clamp(options.cta_replay_round_budget, 0, kMaxCtaRoundBudget);
}

int CtaEpochIncrement(const MetalGacOptions& options) {
  return CtaLocalRoundBudget(options) + CtaReplayRoundBudget(options) + 2;
}

MetalBitSupLayout RequestedBitSupLayout(const MetalGacOptions& options) {
  return options.bitsup_layout == MetalBitSupLayout::kAuto
             ? MetalBitSupLayout::kDirectional
             : options.bitsup_layout;
}

bool HasDirectionalBitSup(const MetalGacOptions& options) {
  return RequestedBitSupLayout(options) == MetalBitSupLayout::kDirectional;
}

MetalKernelVariant EffectiveKernelVariant(const MetalGacOptions& options,
                                          int /*bit_words*/) {
  if (!HasDirectionalBitSup(options)) {
    return MetalKernelVariant::kScalar;
  }
  if (options.frontier_mode == MetalFrontierMode::kBulkSyncMask) {
    return MetalKernelVariant::kWordParallel;
  }
  if (options.frontier_mode == MetalFrontierMode::kCtaWorklist) {
    return MetalKernelVariant::kWordParallel;
  }
  switch (options.kernel_variant) {
    case MetalKernelVariant::kWordParallel:
    case MetalKernelVariant::kSimdgroup:
      return MetalKernelVariant::kWordParallel;
    case MetalKernelVariant::kAuto:
      return MetalKernelVariant::kScalar;
    case MetalKernelVariant::kScalar:
    default:
      return MetalKernelVariant::kScalar;
  }
}

MetalFrontierMode EffectiveFrontierMode(const MetalGacOptions& options,
                                        int /*num_constraints*/,
                                        int /*max_dom_size*/) {
  if (options.frontier_mode == MetalFrontierMode::kCompact) {
    return MetalFrontierMode::kCompact;
  }
  if (options.frontier_mode == MetalFrontierMode::kWorklist) {
    return HasDirectionalBitSup(options) ? MetalFrontierMode::kWorklist
                                         : MetalFrontierMode::kFlags;
  }
  if (options.frontier_mode == MetalFrontierMode::kCtaWorklist) {
    return HasDirectionalBitSup(options) ? MetalFrontierMode::kCtaWorklist
                                         : MetalFrontierMode::kFlags;
  }
  if (options.frontier_mode == MetalFrontierMode::kBulkSyncMask) {
    return HasDirectionalBitSup(options) ? MetalFrontierMode::kBulkSyncMask
                                         : MetalFrontierMode::kFlags;
  }
  if (options.frontier_mode == MetalFrontierMode::kAuto) {
    return MetalFrontierMode::kFlags;
  }
  return MetalFrontierMode::kFlags;
}

MetalBitSupLayout EffectiveBitSupLayoutForPath(
    const MetalGacOptions& options,
    MetalFrontierMode effective_frontier,
    MetalKernelVariant effective_kernel) {
  if (!HasDirectionalBitSup(options)) {
    return MetalBitSupLayout::kPair;
  }
  if (effective_frontier == MetalFrontierMode::kWorklist ||
      effective_frontier == MetalFrontierMode::kCtaWorklist ||
      effective_frontier == MetalFrontierMode::kBulkSyncMask ||
      effective_kernel == MetalKernelVariant::kWordParallel) {
    return MetalBitSupLayout::kDirectional;
  }
  return MetalBitSupLayout::kPair;
}

MetalResetMode EffectiveResetMode(const MetalGacOptions& options,
                                  size_t reset_bytes) {
  if (options.reset_mode == MetalResetMode::kCpu ||
      options.reset_mode == MetalResetMode::kBlit) {
    return options.reset_mode;
  }
  return reset_bytes >= 64 * 1024 ? MetalResetMode::kBlit
                                  : MetalResetMode::kCpu;
}

bool UsesActiveConstraints(MetalFrontierMode frontier) {
  return frontier == MetalFrontierMode::kCompact ||
         frontier == MetalFrontierMode::kWorklist ||
         frontier == MetalFrontierMode::kCtaWorklist ||
         frontier == MetalFrontierMode::kBulkSyncMask;
}

bool UsesNextActiveConstraints(MetalFrontierMode frontier) {
  return frontier == MetalFrontierMode::kWorklist ||
         frontier == MetalFrontierMode::kCtaWorklist ||
         frontier == MetalFrontierMode::kBulkSyncMask;
}

bool UsesEpochFrontier(MetalFrontierMode frontier) {
  return frontier == MetalFrontierMode::kWorklist ||
         frontier == MetalFrontierMode::kBulkSyncMask;
}

std::string PathName(MetalFrontierMode frontier,
                     MetalKernelVariant kernel,
                     MetalBitSupLayout bitsup_layout) {
  return FrontierModeName(frontier) + "+" + KernelVariantName(kernel) + "+" +
         BitSupLayoutName(bitsup_layout);
}

std::string VariantName(const MetalGacOptions& options,
                        int num_constraints,
                        int max_dom_size,
                        int bit_words) {
  const MetalFrontierMode effective_frontier =
      EffectiveFrontierMode(options, num_constraints, max_dom_size);
  const MetalKernelVariant effective_kernel =
      EffectiveKernelVariant(options, bit_words);
  const MetalBitSupLayout effective_bitsup =
      EffectiveBitSupLayoutForPath(options, effective_frontier, effective_kernel);
  const std::string requested =
      PathName(options.frontier_mode, options.kernel_variant, options.bitsup_layout);
  const std::string effective =
      PathName(effective_frontier, effective_kernel, effective_bitsup);
  if (requested == effective) {
    return effective;
  }
  return requested + "->" + effective;
}

}  // namespace

struct MetalGacSolver::Impl {
  explicit Impl(const cpim::model::DeviceModelLayout& input_layout,
                MetalGacOptions input_options)
      : layout(input_layout),
        options(std::move(input_options)),
        bit_dom_result(input_layout.bit_dom),
        domain_sizes_result(input_layout.domain_sizes) {}

  absl::Status Prepare() {
    if (prepared) {
      return absl::OkStatus();
    }
    const auto start = std::chrono::steady_clock::now();
    absl::Status status = InitMetal();
    if (!status.ok()) return status;
    status = InitBuffers();
    if (!status.ok()) return status;
    prepared = true;
    const auto end = std::chrono::steady_clock::now();
    last_prepare_ms =
        std::chrono::duration<double, std::milli>(end - start).count();
    return absl::OkStatus();
  }

  absl::Status InitMetal() {
    auto runtime_or = MetalRuntime::CreateDefault();
    if (!runtime_or.ok()) return runtime_or.status();
    runtime = std::move(*runtime_or);
    device_name = runtime.device_name();

    auto pipeline_or =
        runtime.LoadComputePipeline(options.metallib_path, "gac_revise_kernel");
    if (!pipeline_or.ok()) return pipeline_or.status();
    revise_pipeline = std::move(*pipeline_or);
    const MetalFrontierMode effective_frontier =
        EffectiveFrontierMode(
            options, layout.num_constraints, layout.max_dom_size);
    const MetalKernelVariant effective_kernel =
        EffectiveKernelVariant(options, layout.bit_words);
    if (effective_frontier == MetalFrontierMode::kCompact) {
      auto compact_or = runtime.LoadComputePipeline(
          options.metallib_path, "gac_compact_frontier_kernel");
      if (!compact_or.ok()) return compact_or.status();
      compact_pipeline = std::move(*compact_or);

      auto compact_revise_or = runtime.LoadComputePipeline(
          options.metallib_path, "gac_revise_compact_kernel");
      if (!compact_revise_or.ok()) return compact_revise_or.status();
      compact_revise_pipeline = std::move(*compact_revise_or);
    }
    if (effective_frontier == MetalFrontierMode::kWorklist) {
      auto worklist_or = runtime.LoadComputePipeline(
          options.metallib_path, "gac_revise_worklist_kernel");
      if (!worklist_or.ok()) return worklist_or.status();
      worklist_revise_pipeline = std::move(*worklist_or);
    }
    if (effective_frontier == MetalFrontierMode::kCtaWorklist) {
      auto cta_worklist_or = runtime.LoadComputePipeline(
          options.metallib_path, "gac_revise_cta_worklist_kernel");
      if (!cta_worklist_or.ok()) return cta_worklist_or.status();
      cta_worklist_pipeline = std::move(*cta_worklist_or);
    }
    if (effective_frontier == MetalFrontierMode::kBulkSyncMask) {
      auto bulk_revise_or = runtime.LoadComputePipeline(
          options.metallib_path, "gac_revise_bulk_mask_kernel");
      if (!bulk_revise_or.ok()) return bulk_revise_or.status();
      bulk_mask_revise_pipeline = std::move(*bulk_revise_or);

      auto bulk_apply_or = runtime.LoadComputePipeline(
          options.metallib_path, "gac_apply_bulk_mask_kernel");
      if (!bulk_apply_or.ok()) return bulk_apply_or.status();
      bulk_mask_apply_pipeline = std::move(*bulk_apply_or);
    }
    if (effective_kernel == MetalKernelVariant::kWordParallel) {
      auto word_flags_or = runtime.LoadComputePipeline(
          options.metallib_path, "gac_revise_word_flags_kernel");
      if (!word_flags_or.ok()) return word_flags_or.status();
      word_flags_pipeline = std::move(*word_flags_or);

      auto word_active_or = runtime.LoadComputePipeline(
          options.metallib_path, "gac_revise_word_active_kernel");
      if (!word_active_or.ok()) return word_active_or.status();
      word_active_pipeline = std::move(*word_active_or);

      auto word_worklist_or = runtime.LoadComputePipeline(
          options.metallib_path, "gac_revise_word_worklist_kernel");
      if (!word_worklist_or.ok()) return word_worklist_or.status();
      word_worklist_pipeline = std::move(*word_worklist_or);
    }
    return absl::OkStatus();
  }

  absl::Status InitBuffers() {
    auto bit_dom_or =
        NewSharedBufferWithVector(runtime, layout.bit_dom, "cpim.bit_dom");
    if (!bit_dom_or.ok()) return bit_dom_or.status();
    bit_dom = std::move(*bit_dom_or);

    auto initial_bit_dom_or = NewSharedBufferWithVector(
        runtime, layout.bit_dom, "cpim.initial_bit_dom");
    if (!initial_bit_dom_or.ok()) return initial_bit_dom_or.status();
    initial_bit_dom = std::move(*initial_bit_dom_or);

    auto domain_sizes_or = NewSharedBufferWithVector(
        runtime, layout.domain_sizes, "cpim.domain_sizes");
    if (!domain_sizes_or.ok()) return domain_sizes_or.status();
    domain_sizes = std::move(*domain_sizes_or);

    auto initial_domain_sizes_or = NewSharedBufferWithVector(
        runtime, layout.domain_sizes, "cpim.initial_domain_sizes");
    if (!initial_domain_sizes_or.ok()) {
      return initial_domain_sizes_or.status();
    }
    initial_domain_sizes = std::move(*initial_domain_sizes_or);

    auto bit_sup_or = NewBufferWithVector(
        runtime, layout.bit_sup, "cpim.bit_sup", options.readonly_storage);
    if (!bit_sup_or.ok()) return bit_sup_or.status();
    bit_sup = std::move(*bit_sup_or);

    auto bit_sup_words_or = NewBufferWithVector(
        runtime, layout.bit_sup_words, "cpim.bit_sup_words",
        options.readonly_storage);
    if (!bit_sup_words_or.ok()) return bit_sup_words_or.status();
    bit_sup_words = std::move(*bit_sup_words_or);

    auto scopes_or = NewBufferWithVector(
        runtime, layout.constraint_scopes, "cpim.scopes",
        options.readonly_storage);
    if (!scopes_or.ok()) return scopes_or.status();
    scopes = std::move(*scopes_or);

    auto sub_offsets_or = NewBufferWithVector(
        runtime, layout.subscriptions.offsets, "cpim.subscription_offsets",
        options.readonly_storage);
    if (!sub_offsets_or.ok()) return sub_offsets_or.status();
    sub_offsets = std::move(*sub_offsets_or);

    auto sub_entries_or = NewBufferWithVector(
        runtime, layout.subscriptions.entries, "cpim.subscription_entries",
        options.readonly_storage);
    if (!sub_entries_or.ok()) return sub_entries_or.status();
    sub_entries = std::move(*sub_entries_or);

    initial_frontier.assign(layout.num_constraints, 0);
    for (int cid = 0; cid < layout.num_constraints; ++cid) {
      if (layout.constraint_scopes[cid].x >= 0) {
        initial_frontier[cid] = 1;
      }
    }

    auto current_flags_or = NewSharedBufferWithVector(
        runtime, initial_frontier, "cpim.current_frontier");
    if (!current_flags_or.ok()) return current_flags_or.status();
    current_flags = std::move(*current_flags_or);

    auto initial_frontier_or = NewSharedBufferWithVector(
        runtime, initial_frontier, "cpim.initial_frontier");
    if (!initial_frontier_or.ok()) return initial_frontier_or.status();
    initial_frontier_buffer = std::move(*initial_frontier_or);

    auto next_flags_or = runtime.NewSharedBuffer(
        sizeof(int32_t) * static_cast<size_t>(layout.num_constraints),
        "cpim.next_frontier");
    if (!next_flags_or.ok()) return next_flags_or.status();
    next_flags = std::move(*next_flags_or);

    auto stats_buffer_or =
        runtime.NewSharedBuffer(sizeof(int32_t) * kStatsWords, "cpim.gac_stats");
    if (!stats_buffer_or.ok()) return stats_buffer_or.status();
    stats_buffer = std::move(*stats_buffer_or);

    initial_active_constraints.clear();
    initial_active_constraints.reserve(layout.num_constraints);
    for (int cid = 0; cid < layout.num_constraints; ++cid) {
      if (initial_frontier[cid] != 0) {
        initial_active_constraints.push_back(cid);
      }
    }

    const MetalFrontierMode effective_frontier =
        EffectiveFrontierMode(
            options, layout.num_constraints, layout.max_dom_size);
    if (UsesActiveConstraints(effective_frontier)) {
      auto active_or = runtime.NewSharedBuffer(
          sizeof(int32_t) * static_cast<size_t>(layout.num_constraints),
          "cpim.active_constraints");
      if (!active_or.ok()) return active_or.status();
      active_constraints = std::move(*active_or);
    }

    if (UsesNextActiveConstraints(effective_frontier)) {
      auto next_active_or = runtime.NewSharedBuffer(
          sizeof(int32_t) * static_cast<size_t>(layout.num_constraints),
          "cpim.next_active_constraints");
      if (!next_active_or.ok()) return next_active_or.status();
      next_active_constraints = std::move(*next_active_or);

      auto initial_active_or = NewSharedBufferWithVector(
          runtime, initial_active_constraints,
          "cpim.initial_active_constraints");
      if (!initial_active_or.ok()) return initial_active_or.status();
      initial_active_constraints_buffer = std::move(*initial_active_or);

      auto compact_stats_or =
          runtime.NewSharedBuffer(sizeof(int32_t), "cpim.compact_stats");
      if (!compact_stats_or.ok()) return compact_stats_or.status();
      compact_stats = std::move(*compact_stats_or);
      if (effective_frontier == MetalFrontierMode::kCtaWorklist) {
        cta_count = CtaCount();
        cta_queue_capacity = std::max(1, layout.num_constraints);
        BuildCtaOwnerMap();
        auto cta_owner_map_or = NewSharedBufferWithVector(
            runtime, cta_owner_map, "cpim.cta_owner_map");
        if (!cta_owner_map_or.ok()) return cta_owner_map_or.status();
        cta_owner_map_buffer = std::move(*cta_owner_map_or);

        auto dirty_var_epochs_or = runtime.NewSharedBuffer(
            sizeof(int32_t) * static_cast<size_t>(layout.num_vars),
            "cpim.cta_dirty_var_epochs");
        if (!dirty_var_epochs_or.ok()) return dirty_var_epochs_or.status();
        cta_dirty_var_epochs = std::move(*dirty_var_epochs_or);

        const size_t queue_entries =
            static_cast<size_t>(cta_count) * cta_queue_capacity;
        auto cta_queue_a_or = runtime.NewSharedBuffer(
            sizeof(int32_t) * queue_entries, "cpim.cta_queue_a");
        if (!cta_queue_a_or.ok()) return cta_queue_a_or.status();
        cta_queue_a = std::move(*cta_queue_a_or);

        auto cta_queue_b_or = runtime.NewSharedBuffer(
            sizeof(int32_t) * queue_entries, "cpim.cta_queue_b");
        if (!cta_queue_b_or.ok()) return cta_queue_b_or.status();
        cta_queue_b = std::move(*cta_queue_b_or);

        auto cta_stamps_or = runtime.NewSharedBuffer(
            sizeof(int32_t) * queue_entries, "cpim.cta_stamps");
        if (!cta_stamps_or.ok()) return cta_stamps_or.status();
        cta_stamps = std::move(*cta_stamps_or);

        auto cta_tail_a_or = runtime.NewSharedBuffer(
            sizeof(int32_t) * static_cast<size_t>(cta_count),
            "cpim.cta_tail_a");
        if (!cta_tail_a_or.ok()) return cta_tail_a_or.status();
        cta_tail_a = std::move(*cta_tail_a_or);

        auto cta_tail_b_or = runtime.NewSharedBuffer(
            sizeof(int32_t) * static_cast<size_t>(cta_count),
            "cpim.cta_tail_b");
        if (!cta_tail_b_or.ok()) return cta_tail_b_or.status();
        cta_tail_b = std::move(*cta_tail_b_or);
      }
      if (effective_frontier == MetalFrontierMode::kBulkSyncMask) {
        auto delete_masks_or = runtime.NewSharedBuffer(
            sizeof(uint32_t) * layout.bit_dom.size(), "cpim.bulk_delete_masks");
        if (!delete_masks_or.ok()) return delete_masks_or.status();
        bulk_delete_masks = std::move(*delete_masks_or);
      }
    } else if (effective_frontier == MetalFrontierMode::kCompact) {
      auto compact_stats_or =
          runtime.NewSharedBuffer(sizeof(int32_t), "cpim.compact_stats");
      if (!compact_stats_or.ok()) return compact_stats_or.status();
      compact_stats = std::move(*compact_stats_or);
    }
    return absl::OkStatus();
  }

  int InitialActiveCount() const {
    int active = 0;
    for (int value : initial_frontier) {
      active += value == 0 ? 0 : 1;
    }
    return active;
  }

  int CtaCount() const {
    return std::max(1, std::min(kMaxCtaCount, layout.num_constraints));
  }

  int OwnerForConstraint(int cid) const {
    if (cid >= 0 && cid < static_cast<int>(cta_owner_map.size())) {
      const int owner = cta_owner_map[cid];
      if (owner >= 0 && owner < cta_count) {
        return owner;
      }
    }
    return cta_count <= 0 ? 0 : cid % cta_count;
  }

  double Percentile(std::vector<double> values, double q) const {
    if (values.empty()) {
      return 0.0;
    }
    std::sort(values.begin(), values.end());
    const double scaled = q * static_cast<double>(values.size() - 1);
    const size_t index = static_cast<size_t>(std::ceil(scaled));
    return values[std::min(index, values.size() - 1)];
  }

  std::vector<int32_t> BuildModuloOwnerMap() const {
    std::vector<int32_t> owners(layout.num_constraints, 0);
    for (int cid = 0; cid < layout.num_constraints; ++cid) {
      owners[cid] = cta_count <= 0 ? 0 : cid % cta_count;
    }
    return owners;
  }

  std::vector<int32_t> BuildStaticEdgeCutOwnerMap() const {
    std::vector<int32_t> owners(layout.num_constraints, -1);
    std::vector<int32_t> owner_load(cta_count, 0);
    std::vector<int32_t> seen(layout.num_constraints, -1);
    std::vector<int32_t> affinity(cta_count, 0);
    const int target_load =
        std::max(1, (layout.num_constraints + cta_count - 1) / cta_count);
    const int soft_limit =
        std::max(1, static_cast<int>(std::ceil(1.25 * target_load)));

    for (int cid = 0; cid < layout.num_constraints; ++cid) {
      const cpim::model::DeviceInt2 scope = layout.constraint_scopes[cid];
      if (scope.x < 0 || scope.y < 0) {
        owners[cid] = cta_count <= 0 ? 0 : cid % cta_count;
        continue;
      }

      std::fill(affinity.begin(), affinity.end(), 0);
      const int vars[2] = {scope.x, scope.y};
      for (int var : vars) {
        if (var < 0 || var + 1 >=
                           static_cast<int>(layout.subscriptions.offsets.size())) {
          continue;
        }
        const int begin = layout.subscriptions.offsets[var];
        const int end = layout.subscriptions.offsets[var + 1];
        for (int entry = begin; entry < end; ++entry) {
          const int neighbor =
              static_cast<int>(layout.subscriptions.entries[entry].z);
          if (neighbor < 0 || neighbor >= layout.num_constraints ||
              neighbor == cid || seen[neighbor] == cid) {
            continue;
          }
          seen[neighbor] = cid;
          const int owner = owners[neighbor];
          if (owner >= 0 && owner < cta_count) {
            ++affinity[owner];
          }
        }
      }

      int best_owner = -1;
      for (int owner = 0; owner < cta_count; ++owner) {
        if (owner_load[owner] >= soft_limit) {
          continue;
        }
        if (best_owner < 0 || affinity[owner] > affinity[best_owner] ||
            (affinity[owner] == affinity[best_owner] &&
             owner_load[owner] < owner_load[best_owner])) {
          best_owner = owner;
        }
      }
      if (best_owner < 0) {
        best_owner = 0;
        for (int owner = 1; owner < cta_count; ++owner) {
          if (affinity[owner] > affinity[best_owner] ||
              (affinity[owner] == affinity[best_owner] &&
               owner_load[owner] < owner_load[best_owner])) {
            best_owner = owner;
          }
        }
      }

      owners[cid] = best_owner;
      ++owner_load[best_owner];
    }

    return owners;
  }

  std::vector<int32_t> VariableDegrees() const {
    std::vector<int32_t> degrees(layout.num_vars, 0);
    for (int var = 0; var < layout.num_vars; ++var) {
      if (var + 1 < static_cast<int>(layout.subscriptions.offsets.size())) {
        degrees[var] =
            layout.subscriptions.offsets[var + 1] - layout.subscriptions.offsets[var];
      }
    }
    return degrees;
  }

  std::vector<int32_t> ConstraintWeights(
      const std::vector<int32_t>& variable_degrees) const {
    std::vector<int32_t> weights(layout.num_constraints, 1);
    const int bit_weight = std::max(1, layout.bit_words);
    for (int cid = 0; cid < layout.num_constraints; ++cid) {
      const cpim::model::DeviceInt2 scope = layout.constraint_scopes[cid];
      if (scope.x < 0 || scope.y < 0 ||
          scope.x >= static_cast<int>(variable_degrees.size()) ||
          scope.y >= static_cast<int>(variable_degrees.size())) {
        weights[cid] = bit_weight;
        continue;
      }
      const int degree_sum =
          std::max(1, variable_degrees[scope.x] + variable_degrees[scope.y]);
      weights[cid] = std::max(1, bit_weight * degree_sum);
    }
    return weights;
  }

  std::vector<int32_t> BuildVeboWeightedOwnerMap() const {
    std::vector<int32_t> owners(layout.num_constraints, -1);
    std::vector<int32_t> owner_count(cta_count, 0);
    std::vector<int64_t> owner_weight(cta_count, 0);
    std::vector<int32_t> seen(layout.num_constraints, -1);
    std::vector<int32_t> affinity(cta_count, 0);
    const std::vector<int32_t> variable_degrees = VariableDegrees();
    const std::vector<int32_t> constraint_weights =
        ConstraintWeights(variable_degrees);

    int64_t total_weight = 0;
    for (int weight : constraint_weights) {
      total_weight += std::max(1, weight);
    }
    const double target_count =
        static_cast<double>(std::max(1, layout.num_constraints)) /
        static_cast<double>(std::max(1, cta_count));
    const double target_weight =
        static_cast<double>(std::max<int64_t>(1, total_weight)) /
        static_cast<double>(std::max(1, cta_count));
    const int soft_count =
        std::max(1, static_cast<int>(std::ceil(1.25 * target_count)));
    const int64_t soft_weight =
        std::max<int64_t>(1, static_cast<int64_t>(std::ceil(1.25 * target_weight)));

    std::vector<int32_t> vars(layout.num_vars, 0);
    for (int var = 0; var < layout.num_vars; ++var) {
      vars[var] = var;
    }
    std::sort(vars.begin(), vars.end(), [&](int lhs, int rhs) {
      if (variable_degrees[lhs] != variable_degrees[rhs]) {
        return variable_degrees[lhs] > variable_degrees[rhs];
      }
      return lhs < rhs;
    });

    auto choose_owner = [&](int cid) {
      std::fill(affinity.begin(), affinity.end(), 0);
      const cpim::model::DeviceInt2 scope = layout.constraint_scopes[cid];
      const int vars_for_constraint[2] = {scope.x, scope.y};
      for (int var : vars_for_constraint) {
        if (var < 0 ||
            var + 1 >= static_cast<int>(layout.subscriptions.offsets.size())) {
          continue;
        }
        const int begin = layout.subscriptions.offsets[var];
        const int end = layout.subscriptions.offsets[var + 1];
        for (int entry = begin; entry < end; ++entry) {
          const int neighbor =
              static_cast<int>(layout.subscriptions.entries[entry].z);
          if (neighbor < 0 || neighbor >= layout.num_constraints ||
              neighbor == cid || seen[neighbor] == cid) {
            continue;
          }
          seen[neighbor] = cid;
          const int owner = owners[neighbor];
          if (owner >= 0 && owner < cta_count) {
            ++affinity[owner];
          }
        }
      }

      const int weight = std::max(1, constraint_weights[cid]);
      int best_owner = -1;
      for (int owner = 0; owner < cta_count; ++owner) {
        const bool under_soft =
            owner_count[owner] < soft_count &&
            owner_weight[owner] + weight <= soft_weight;
        const bool best_under_soft =
            best_owner >= 0 &&
            owner_count[best_owner] < soft_count &&
            owner_weight[best_owner] + weight <= soft_weight;
        if (best_owner < 0 || (under_soft && !best_under_soft) ||
            (under_soft == best_under_soft &&
             affinity[owner] > affinity[best_owner]) ||
            (under_soft == best_under_soft &&
             affinity[owner] == affinity[best_owner] &&
             owner_weight[owner] < owner_weight[best_owner]) ||
            (under_soft == best_under_soft &&
             affinity[owner] == affinity[best_owner] &&
             owner_weight[owner] == owner_weight[best_owner] &&
             owner_count[owner] < owner_count[best_owner]) ||
            (under_soft == best_under_soft &&
             affinity[owner] == affinity[best_owner] &&
             owner_weight[owner] == owner_weight[best_owner] &&
             owner_count[owner] == owner_count[best_owner] &&
             owner < best_owner)) {
          best_owner = owner;
        }
      }
      return best_owner < 0 ? 0 : best_owner;
    };

    auto assign_constraint = [&](int cid) {
      if (cid < 0 || cid >= layout.num_constraints || owners[cid] >= 0) {
        return;
      }
      const int owner = choose_owner(cid);
      owners[cid] = owner;
      ++owner_count[owner];
      owner_weight[owner] += std::max(1, constraint_weights[cid]);
    };

    for (int var : vars) {
      if (var + 1 >= static_cast<int>(layout.subscriptions.offsets.size())) {
        continue;
      }
      const int begin = layout.subscriptions.offsets[var];
      const int end = layout.subscriptions.offsets[var + 1];
      std::vector<int32_t> local_constraints;
      local_constraints.reserve(static_cast<size_t>(std::max(0, end - begin)));
      for (int entry = begin; entry < end; ++entry) {
        const int cid = static_cast<int>(layout.subscriptions.entries[entry].z);
        if (cid >= 0 && cid < layout.num_constraints && owners[cid] < 0) {
          local_constraints.push_back(cid);
        }
      }
      std::sort(local_constraints.begin(), local_constraints.end(),
                [&](int lhs, int rhs) {
                  if (constraint_weights[lhs] != constraint_weights[rhs]) {
                    return constraint_weights[lhs] > constraint_weights[rhs];
                  }
                  return lhs < rhs;
                });
      for (int cid : local_constraints) {
        assign_constraint(cid);
      }
    }
    for (int cid = 0; cid < layout.num_constraints; ++cid) {
      assign_constraint(cid);
    }
    return owners;
  }

  void RefreshOwnerBalanceStats() {
    std::vector<int32_t> owner_load(cta_count, 0);
    const std::vector<int32_t> variable_degrees = VariableDegrees();
    const std::vector<int32_t> constraint_weights =
        ConstraintWeights(variable_degrees);
    std::vector<int64_t> owner_weight(cta_count, 0);
    for (int cid = 0; cid < static_cast<int>(cta_owner_map.size()); ++cid) {
      const int owner = OwnerForConstraint(cid);
      if (owner >= 0 && owner < cta_count) {
        ++owner_load[owner];
        if (cid < static_cast<int>(constraint_weights.size())) {
          owner_weight[owner] += std::max(1, constraint_weights[cid]);
        }
      }
    }
    const double target_load =
        static_cast<double>(std::max(1, layout.num_constraints)) /
        static_cast<double>(std::max(1, cta_count));
    std::vector<double> ratios;
    ratios.reserve(owner_load.size());
    for (int load : owner_load) {
      ratios.push_back(static_cast<double>(load) / target_load);
    }
    owner_balance_p95 = Percentile(std::move(ratios), 0.95);

    int64_t total_weight = 0;
    for (int64_t weight : owner_weight) {
      total_weight += weight;
    }
    const double target_weight =
        static_cast<double>(std::max<int64_t>(1, total_weight)) /
        static_cast<double>(std::max(1, cta_count));
    std::vector<double> weight_ratios;
    weight_ratios.reserve(owner_weight.size());
    for (int64_t weight : owner_weight) {
      weight_ratios.push_back(static_cast<double>(weight) / target_weight);
    }
    owner_weight_balance_p95 = Percentile(std::move(weight_ratios), 0.95);
  }

  void BuildCtaOwnerMap() {
    const auto start = std::chrono::steady_clock::now();
    switch (options.cta_owner_mode) {
      case MetalCtaOwnerMode::kVeboWeighted:
        cta_owner_map = BuildVeboWeightedOwnerMap();
        break;
      case MetalCtaOwnerMode::kStaticEdgeCut:
        cta_owner_map = BuildStaticEdgeCutOwnerMap();
        break;
      case MetalCtaOwnerMode::kModulo:
      default:
        cta_owner_map = BuildModuloOwnerMap();
        break;
    }
    RefreshOwnerBalanceStats();
    const auto end = std::chrono::steady_clock::now();
    owner_map_build_ms =
        std::chrono::duration<double, std::milli>(end - start).count();
  }

  size_t MutableResetBytes(MetalFrontierMode effective_frontier) const {
    size_t bytes = layout.bit_dom.size() * sizeof(uint32_t);
    bytes += layout.domain_sizes.size() * sizeof(int32_t);
    bytes += initial_frontier.size() * sizeof(int32_t);
    bytes += static_cast<size_t>(layout.num_constraints) * sizeof(int32_t);
    bytes += kStatsWords * sizeof(int32_t);
    if (UsesNextActiveConstraints(effective_frontier)) {
      bytes += 2 * static_cast<size_t>(layout.num_constraints) * sizeof(int32_t);
    }
    if (effective_frontier == MetalFrontierMode::kBulkSyncMask) {
      bytes += layout.bit_dom.size() * sizeof(uint32_t);
    }
    if (effective_frontier == MetalFrontierMode::kCtaWorklist) {
      const size_t queue_entries =
          static_cast<size_t>(CtaCount()) * std::max(1, layout.num_constraints);
      bytes += 3 * queue_entries * sizeof(int32_t);
      bytes += 2 * static_cast<size_t>(CtaCount()) * sizeof(int32_t);
      bytes += static_cast<size_t>(layout.num_vars) * sizeof(int32_t);
    }
    if (effective_frontier == MetalFrontierMode::kCompact ||
        effective_frontier == MetalFrontierMode::kWorklist ||
        effective_frontier == MetalFrontierMode::kBulkSyncMask) {
      bytes += sizeof(int32_t);
    }
    return bytes;
  }

  double ResetMutableStateCpu() {
    const auto start = std::chrono::steady_clock::now();
    frontier_epoch = 1;
    if (!layout.bit_dom.empty()) {
      std::memcpy(bit_dom.contents(), layout.bit_dom.data(),
                  layout.bit_dom.size() * sizeof(uint32_t));
    }
    if (!layout.domain_sizes.empty()) {
      std::memcpy(domain_sizes.contents(), layout.domain_sizes.data(),
                  layout.domain_sizes.size() * sizeof(int32_t));
    }
    if (!initial_frontier.empty()) {
      std::memcpy(current_flags.contents(), initial_frontier.data(),
                  initial_frontier.size() * sizeof(int32_t));
    }
    if (!initial_active_constraints.empty() && active_constraints.valid()) {
      std::memcpy(active_constraints.contents(), initial_active_constraints.data(),
                  initial_active_constraints.size() * sizeof(int32_t));
    }
    std::memset(next_flags.contents(), 0,
                sizeof(int32_t) * static_cast<size_t>(layout.num_constraints));
    std::memset(stats_buffer.contents(), 0, sizeof(int32_t) * kStatsWords);
    if (next_active_constraints.valid()) {
      std::memset(next_active_constraints.contents(), 0,
                  sizeof(int32_t) * static_cast<size_t>(layout.num_constraints));
    }
    ClearCompactBuffers();
    ClearCtaBuffers();
    ClearBulkBuffers();
    const auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count();
  }

  absl::Status ResetMutableStateBlit(MetalGacStats* result,
                                     MetalFrontierMode effective_frontier) {
    frontier_epoch = 1;
    std::vector<MetalBufferCopy> copies;
    copies.push_back({&initial_bit_dom, &bit_dom,
                      layout.bit_dom.size() * sizeof(uint32_t)});
    copies.push_back({&initial_domain_sizes, &domain_sizes,
                      layout.domain_sizes.size() * sizeof(int32_t)});
    copies.push_back({&initial_frontier_buffer, &current_flags,
                      initial_frontier.size() * sizeof(int32_t)});
    if (UsesNextActiveConstraints(effective_frontier) &&
        initial_active_constraints_buffer.valid() &&
        active_constraints.valid()) {
      copies.push_back({&initial_active_constraints_buffer, &active_constraints,
                        initial_active_constraints.size() * sizeof(int32_t)});
    }

    std::vector<MetalBufferFill> fills;
    fills.push_back({&next_flags,
                     sizeof(int32_t) * static_cast<size_t>(layout.num_constraints),
                     0});
    fills.push_back({&stats_buffer, sizeof(int32_t) * kStatsWords, 0});
    if (next_active_constraints.valid()) {
      fills.push_back(
          {&next_active_constraints,
           sizeof(int32_t) * static_cast<size_t>(layout.num_constraints), 0});
    }
    if (compact_stats.valid()) {
      fills.push_back({&compact_stats, sizeof(int32_t), 0});
    }
    if (effective_frontier == MetalFrontierMode::kCtaWorklist) {
      if (cta_queue_a.valid()) {
        fills.push_back({&cta_queue_a, cta_queue_a.size_bytes(), 0});
      }
      if (cta_queue_b.valid()) {
        fills.push_back({&cta_queue_b, cta_queue_b.size_bytes(), 0});
      }
      if (cta_stamps.valid()) {
        fills.push_back({&cta_stamps, cta_stamps.size_bytes(), 0});
      }
      if (cta_tail_a.valid()) {
        fills.push_back({&cta_tail_a, cta_tail_a.size_bytes(), 0});
      }
      if (cta_tail_b.valid()) {
        fills.push_back({&cta_tail_b, cta_tail_b.size_bytes(), 0});
      }
      if (cta_dirty_var_epochs.valid()) {
        fills.push_back(
            {&cta_dirty_var_epochs, cta_dirty_var_epochs.size_bytes(), 0});
      }
    }
    if (effective_frontier == MetalFrontierMode::kBulkSyncMask &&
        bulk_delete_masks.valid()) {
      fills.push_back({&bulk_delete_masks, bulk_delete_masks.size_bytes(), 0});
    }

    auto timings_or = runtime.BlitCopyAndFill(copies, fills);
    if (!timings_or.ok()) return timings_or.status();
    result->reset_ms = timings_or->wall_ms;
    result->reset_dispatch_ms = timings_or->wall_ms;
    return absl::OkStatus();
  }

  absl::Status ResetMutableState(MetalGacStats* result,
                                 MetalFrontierMode effective_frontier) {
    const MetalResetMode effective_reset =
        EffectiveResetMode(options, MutableResetBytes(effective_frontier));
    result->reset_mode = ResetModeName(effective_reset);
    if (effective_reset == MetalResetMode::kBlit) {
      return ResetMutableStateBlit(result, effective_frontier);
    }
    result->reset_ms = ResetMutableStateCpu();
    result->reset_dispatch_ms = 0.0;
    return absl::OkStatus();
  }

  void ClearIterationBuffers(MetalFrontierMode effective_frontier) {
    if (!UsesNextActiveConstraints(effective_frontier)) {
      std::memset(next_flags.contents(), 0,
                  sizeof(int32_t) * static_cast<size_t>(layout.num_constraints));
    }
    std::memset(stats_buffer.contents(), 0, sizeof(int32_t) * kStatsWords);
    if (!UsesNextActiveConstraints(effective_frontier) &&
        next_active_constraints.valid()) {
      std::memset(next_active_constraints.contents(), 0,
                  sizeof(int32_t) * static_cast<size_t>(layout.num_constraints));
    }
    if (effective_frontier == MetalFrontierMode::kBulkSyncMask) {
      ClearBulkBuffers();
    }
  }

  void AdvanceWorklistEpoch(MetalGacStats* result) {
    if (frontier_epoch >= std::numeric_limits<int32_t>::max() - 1) {
      std::memset(next_flags.contents(), 0,
                  sizeof(int32_t) * static_cast<size_t>(layout.num_constraints));
      frontier_epoch = 1;
      ++result->worklist_epoch_resets;
    }
    ++frontier_epoch;
  }

  void AdvanceCtaWorklistEpoch(MetalGacStats* result) {
    const int epoch_increment = CtaEpochIncrement(options);
    if (frontier_epoch >=
        std::numeric_limits<int32_t>::max() - epoch_increment - 2) {
      std::memset(next_flags.contents(), 0,
                  sizeof(int32_t) * static_cast<size_t>(layout.num_constraints));
      if (cta_stamps.valid()) {
        std::memset(cta_stamps.contents(), 0, cta_stamps.size_bytes());
      }
      if (cta_dirty_var_epochs.valid()) {
        std::memset(cta_dirty_var_epochs.contents(), 0,
                    cta_dirty_var_epochs.size_bytes());
      }
      frontier_epoch = 1;
      ++result->worklist_epoch_resets;
    }
    frontier_epoch += epoch_increment;
  }

  void ClearCompactBuffers() {
    if (compact_stats.valid()) {
      std::memset(compact_stats.contents(), 0, sizeof(int32_t));
    }
  }

  void ClearCtaBuffers() {
    if (cta_queue_a.valid()) {
      std::memset(cta_queue_a.contents(), 0, cta_queue_a.size_bytes());
    }
    if (cta_queue_b.valid()) {
      std::memset(cta_queue_b.contents(), 0, cta_queue_b.size_bytes());
    }
    if (cta_tail_a.valid()) {
      std::memset(cta_tail_a.contents(), 0, cta_tail_a.size_bytes());
    }
    if (cta_tail_b.valid()) {
      std::memset(cta_tail_b.contents(), 0, cta_tail_b.size_bytes());
    }
    if (cta_stamps.valid()) {
      std::memset(cta_stamps.contents(), 0, cta_stamps.size_bytes());
    }
    if (cta_dirty_var_epochs.valid()) {
      std::memset(cta_dirty_var_epochs.contents(), 0,
                  cta_dirty_var_epochs.size_bytes());
    }
  }

  void ClearBulkBuffers() {
    if (bulk_delete_masks.valid()) {
      std::memset(bulk_delete_masks.contents(), 0, bulk_delete_masks.size_bytes());
    }
  }

  void SeedCtaQueues(int active_count, MetalGacStats* result) {
    if (!cta_queue_a.valid() || !cta_tail_a.valid() || active_count <= 0) {
      return;
    }
    std::memset(cta_queue_a.contents(), 0, cta_queue_a.size_bytes());
    std::memset(cta_queue_b.contents(), 0, cta_queue_b.size_bytes());
    std::memset(cta_tail_a.contents(), 0, cta_tail_a.size_bytes());
    std::memset(cta_tail_b.contents(), 0, cta_tail_b.size_bytes());

    const int32_t* active =
        static_cast<const int32_t*>(active_constraints.contents());
    int32_t* queue = static_cast<int32_t*>(cta_queue_a.contents());
    int32_t* tails = static_cast<int32_t*>(cta_tail_a.contents());
    int seed_overflow = 0;
    for (int i = 0; i < active_count; ++i) {
      const int cid = active[i];
      if (cid < 0 || cid >= layout.num_constraints) {
        continue;
      }
      const int owner = OwnerForConstraint(cid);
      const int slot = tails[owner]++;
      if (slot < cta_queue_capacity) {
        queue[owner * cta_queue_capacity + slot] = cid;
      } else {
        ++seed_overflow;
      }
    }
    if (result == nullptr) {
      return;
    }
    int nonempty = 0;
    int max_load = 0;
    std::vector<double> load_ratios;
    load_ratios.reserve(static_cast<size_t>(cta_count));
    const double target_load =
        static_cast<double>(std::max(1, active_count)) /
        static_cast<double>(std::max(1, cta_count));
    for (int owner = 0; owner < cta_count; ++owner) {
      const int load = tails[owner];
      if (load > 0) {
        ++nonempty;
      }
      max_load = std::max(max_load, load);
      load_ratios.push_back(static_cast<double>(load) / target_load);
    }
    result->seed_owner_nonempty_count += nonempty;
    result->seed_empty_owner_count += std::max(0, cta_count - nonempty);
    result->seed_max_owner_load = std::max(result->seed_max_owner_load, max_load);
    result->seed_owner_balance_p95 = std::max(
        result->seed_owner_balance_p95, Percentile(std::move(load_ratios), 0.95));
    result->cta_seed_overflow_count += seed_overflow;
    result->cta_overflow_count += seed_overflow;
  }

  void PullDirtyVarsToNextActive() {
    if (options.cta_handoff_mode != MetalCtaHandoffMode::kDirtyVarPull ||
        !cta_dirty_var_epochs.valid() || !next_flags.valid() ||
        !next_active_constraints.valid()) {
      return;
    }
    const int32_t* dirty =
        static_cast<const int32_t*>(cta_dirty_var_epochs.contents());
    int32_t* epochs = static_cast<int32_t*>(next_flags.contents());
    int32_t* next = static_cast<int32_t*>(next_active_constraints.contents());
    int32_t* stats = static_cast<int32_t*>(stats_buffer.contents());
    for (int var = 0; var < layout.num_vars; ++var) {
      if (dirty[var] != frontier_epoch) {
        continue;
      }
      const int begin = layout.subscriptions.offsets[var];
      const int end = layout.subscriptions.offsets[var + 1];
      for (int i = begin; i < end; ++i) {
        ++stats[19];
        const int cid = static_cast<int>(layout.subscriptions.entries[i].z);
        if (cid < 0 || cid >= layout.num_constraints) {
          continue;
        }
        if (epochs[cid] == frontier_epoch) {
          continue;
        }
        epochs[cid] = frontier_epoch;
        if (stats[2] < layout.num_constraints) {
          next[stats[2]] = cid;
          ++stats[2];
          ++stats[20];
        } else {
          ++stats[6];
          ++stats[9];
        }
      }
    }
  }

  uint64_t ThreadsPerGroup(const MetalPipeline& selected_pipeline) const {
    const uint64_t max_threads =
        std::min<uint64_t>(selected_pipeline.max_threads_per_threadgroup(), 256);
    return std::max<uint64_t>(max_threads, 1);
  }

  absl::StatusOr<MetalDispatchTimings> DispatchFlagRevise() {
    GacParamsMetal params;
    params.num_constraints = layout.num_constraints;
    params.max_dom_size = layout.max_dom_size;
    params.bit_words = layout.bit_words;
    params.num_vars = layout.num_vars;
    params.active_count = layout.num_constraints;

    const uint64_t total_tasks = static_cast<uint64_t>(layout.num_constraints) *
                                 2u *
                                 static_cast<uint64_t>(layout.max_dom_size);
    return runtime.Dispatch1D(
        revise_pipeline,
        {{0, &bit_dom},
         {1, &domain_sizes},
         {2, &bit_sup},
         {3, &scopes},
         {4, &sub_offsets},
         {5, &sub_entries},
         {6, &current_flags},
         {7, &next_flags},
         {8, &stats_buffer}},
        &params, sizeof(params), 9, total_tasks, ThreadsPerGroup(revise_pipeline));
  }

  absl::StatusOr<MetalDispatchTimings> DispatchWordFlagRevise() {
    GacParamsMetal params;
    params.num_constraints = layout.num_constraints;
    params.max_dom_size = layout.max_dom_size;
    params.bit_words = layout.bit_words;
    params.num_vars = layout.num_vars;
    params.active_count = layout.num_constraints;

    const uint64_t total_tasks = static_cast<uint64_t>(layout.num_constraints) *
                                 2u *
                                 static_cast<uint64_t>(layout.bit_words);
    return runtime.Dispatch1D(
        word_flags_pipeline,
        {{0, &bit_dom},
         {1, &domain_sizes},
         {2, &bit_sup_words},
         {3, &scopes},
         {4, &sub_offsets},
         {5, &sub_entries},
         {6, &current_flags},
         {7, &next_flags},
         {8, &stats_buffer}},
        &params, sizeof(params), 9, total_tasks,
        ThreadsPerGroup(word_flags_pipeline));
  }

  absl::StatusOr<MetalDispatchTimings> DispatchCompactFrontier() {
    CompactParamsMetal params;
    params.num_constraints = layout.num_constraints;
    return runtime.Dispatch1D(
        compact_pipeline,
        {{0, &current_flags},
         {1, &active_constraints},
         {2, &compact_stats}},
        &params, sizeof(params), 3,
        static_cast<uint64_t>(layout.num_constraints),
        ThreadsPerGroup(compact_pipeline));
  }

  absl::StatusOr<MetalDispatchTimings> DispatchCompactRevise(int active_count) {
    GacParamsMetal params;
    params.num_constraints = layout.num_constraints;
    params.max_dom_size = layout.max_dom_size;
    params.bit_words = layout.bit_words;
    params.num_vars = layout.num_vars;
    params.active_count = active_count;

    const uint64_t total_tasks = static_cast<uint64_t>(active_count) *
                                 2u *
                                 static_cast<uint64_t>(layout.max_dom_size);
    return runtime.Dispatch1D(
        compact_revise_pipeline,
        {{0, &bit_dom},
         {1, &domain_sizes},
         {2, &bit_sup},
         {3, &scopes},
         {4, &sub_offsets},
         {5, &sub_entries},
         {6, &active_constraints},
         {7, &next_flags},
         {8, &stats_buffer}},
        &params, sizeof(params), 9, total_tasks,
        ThreadsPerGroup(compact_revise_pipeline));
  }

  absl::StatusOr<MetalDispatchTimings> DispatchWordActiveRevise(
      int active_count) {
    GacParamsMetal params;
    params.num_constraints = layout.num_constraints;
    params.max_dom_size = layout.max_dom_size;
    params.bit_words = layout.bit_words;
    params.num_vars = layout.num_vars;
    params.active_count = active_count;

    const uint64_t total_tasks = static_cast<uint64_t>(active_count) *
                                 2u *
                                 static_cast<uint64_t>(layout.bit_words);
    return runtime.Dispatch1D(
        word_active_pipeline,
        {{0, &bit_dom},
         {1, &domain_sizes},
         {2, &bit_sup_words},
         {3, &scopes},
         {4, &sub_offsets},
         {5, &sub_entries},
         {6, &active_constraints},
         {7, &next_flags},
         {8, &stats_buffer}},
        &params, sizeof(params), 9, total_tasks,
        ThreadsPerGroup(word_active_pipeline));
  }

  absl::StatusOr<MetalDispatchTimings> DispatchWorklistRevise(
      int active_count, int frontier_epoch_value) {
    GacParamsMetal params;
    params.num_constraints = layout.num_constraints;
    params.max_dom_size = layout.max_dom_size;
    params.bit_words = layout.bit_words;
    params.num_vars = layout.num_vars;
    params.active_count = active_count;
    params.frontier_epoch = frontier_epoch_value;

    const uint64_t total_tasks = static_cast<uint64_t>(active_count) *
                                 2u *
                                 static_cast<uint64_t>(layout.max_dom_size);
    return runtime.Dispatch1D(
        worklist_revise_pipeline,
        {{0, &bit_dom},
         {1, &domain_sizes},
         {2, &bit_sup_words},
         {3, &scopes},
         {4, &sub_offsets},
         {5, &sub_entries},
         {6, &active_constraints},
         {7, &next_flags},
         {8, &stats_buffer},
         {9, &next_active_constraints}},
        &params, sizeof(params), 10, total_tasks,
        ThreadsPerGroup(worklist_revise_pipeline));
  }

  absl::StatusOr<MetalDispatchTimings> DispatchWordWorklistRevise(
      int active_count, int frontier_epoch_value) {
    GacParamsMetal params;
    params.num_constraints = layout.num_constraints;
    params.max_dom_size = layout.max_dom_size;
    params.bit_words = layout.bit_words;
    params.num_vars = layout.num_vars;
    params.active_count = active_count;
    params.frontier_epoch = frontier_epoch_value;

    const uint64_t total_tasks = static_cast<uint64_t>(active_count) *
                                 2u *
                                 static_cast<uint64_t>(layout.bit_words);
    return runtime.Dispatch1D(
        word_worklist_pipeline,
        {{0, &bit_dom},
         {1, &domain_sizes},
         {2, &bit_sup_words},
         {3, &scopes},
         {4, &sub_offsets},
         {5, &sub_entries},
         {6, &active_constraints},
         {7, &next_flags},
         {8, &stats_buffer},
         {9, &next_active_constraints}},
        &params, sizeof(params), 10, total_tasks,
        ThreadsPerGroup(word_worklist_pipeline));
  }

  absl::StatusOr<MetalDispatchTimings> DispatchCtaWorklistRevise(
      int active_count, int frontier_epoch_value) {
    const uint64_t threads_per_group = ThreadsPerGroup(cta_worklist_pipeline);
    GacParamsMetal params;
    params.num_constraints = layout.num_constraints;
    params.max_dom_size = layout.max_dom_size;
    params.bit_words = layout.bit_words;
    params.num_vars = layout.num_vars;
    params.active_count = active_count;
    params.frontier_epoch = frontier_epoch_value;
    params.cta_count = cta_count;
    params.cta_queue_capacity = cta_queue_capacity;
    params.cta_local_round_budget = CtaLocalRoundBudget(options);
    params.cta_replay_round_budget = CtaReplayRoundBudget(options);
    params.threads_per_cta = static_cast<int32_t>(threads_per_group);
    params.cta_queue_mode = CtaQueueModeValue(options.cta_queue_mode);
    params.cta_handoff_mode = CtaHandoffModeValue(options.cta_handoff_mode);
    params.cta_dirty_pull_min_degree =
        std::max(0, options.cta_dirty_pull_min_degree);

    return runtime.Dispatch1D(
        cta_worklist_pipeline,
        {{0, &bit_dom},
         {1, &domain_sizes},
         {2, &bit_sup_words},
         {3, &scopes},
         {4, &sub_offsets},
         {5, &sub_entries},
         {6, &cta_queue_a},
         {7, &cta_queue_b},
         {8, &cta_tail_a},
         {9, &cta_tail_b},
         {10, &cta_stamps},
         {11, &next_flags},
         {12, &stats_buffer},
         {13, &next_active_constraints},
         {14, &cta_owner_map_buffer},
         {15, &cta_dirty_var_epochs}},
        &params, sizeof(params), 16,
        static_cast<uint64_t>(cta_count) * threads_per_group,
        threads_per_group);
  }

  absl::StatusOr<MetalDispatchTimings> DispatchBulkMaskRevise(int active_count) {
    GacParamsMetal params;
    params.num_constraints = layout.num_constraints;
    params.max_dom_size = layout.max_dom_size;
    params.bit_words = layout.bit_words;
    params.num_vars = layout.num_vars;
    params.active_count = active_count;

    const uint64_t total_tasks = static_cast<uint64_t>(active_count) *
                                 2u *
                                 static_cast<uint64_t>(layout.bit_words);
    return runtime.Dispatch1D(
        bulk_mask_revise_pipeline,
        {{0, &bit_dom},
         {1, &bit_sup_words},
         {2, &scopes},
         {3, &active_constraints},
         {4, &bulk_delete_masks},
         {5, &stats_buffer}},
        &params, sizeof(params), 6, total_tasks,
        ThreadsPerGroup(bulk_mask_revise_pipeline));
  }

  absl::StatusOr<MetalDispatchTimings> DispatchBulkMaskApply(
      int frontier_epoch_value) {
    GacParamsMetal params;
    params.num_constraints = layout.num_constraints;
    params.max_dom_size = layout.max_dom_size;
    params.bit_words = layout.bit_words;
    params.num_vars = layout.num_vars;
    params.frontier_epoch = frontier_epoch_value;

    const uint64_t total_tasks =
        static_cast<uint64_t>(layout.num_vars) *
        static_cast<uint64_t>(layout.bit_words);
    return runtime.Dispatch1D(
        bulk_mask_apply_pipeline,
        {{0, &bit_dom},
         {1, &domain_sizes},
         {2, &bulk_delete_masks},
         {3, &sub_offsets},
         {4, &sub_entries},
         {5, &next_flags},
         {6, &stats_buffer},
         {7, &next_active_constraints}},
        &params, sizeof(params), 8, total_tasks,
        ThreadsPerGroup(bulk_mask_apply_pipeline));
  }

  void CopyResultsBack() {
    bit_dom_result.resize(layout.bit_dom.size());
    domain_sizes_result.resize(layout.domain_sizes.size());
    if (!bit_dom_result.empty()) {
      std::memcpy(bit_dom_result.data(), bit_dom.contents(),
                  bit_dom_result.size() * sizeof(uint32_t));
    }
    if (!domain_sizes_result.empty()) {
      std::memcpy(domain_sizes_result.data(), domain_sizes.contents(),
                  domain_sizes_result.size() * sizeof(int32_t));
    }
  }

  absl::StatusOr<MetalGacStats> Run(MetalRunnerMode runner_mode) {
    MetalGacStats result;
    const auto total_start = std::chrono::steady_clock::now();
    const bool was_prepared = prepared;
    absl::Status status = Prepare();
    if (!status.ok()) return status;
    result.prepare_ms = was_prepared ? 0.0 : last_prepare_ms;
    const MetalFrontierMode effective_frontier =
        EffectiveFrontierMode(
            options, layout.num_constraints, layout.max_dom_size);
    const MetalKernelVariant effective_kernel =
        EffectiveKernelVariant(options, layout.bit_words);
    const MetalBitSupLayout effective_bitsup =
        EffectiveBitSupLayoutForPath(options, effective_frontier, effective_kernel);
    status = ResetMutableState(&result, effective_frontier);
    if (!status.ok()) return status;

    result.device_name = device_name;
    result.runner_mode = RunnerModeName(runner_mode);
    result.readonly_storage = ReadonlyStorageName(options.readonly_storage);
    result.frontier_mode = FrontierModeName(options.frontier_mode);
    result.kernel_variant = KernelVariantName(options.kernel_variant);
    result.bitsup_layout = BitSupLayoutName(options.bitsup_layout);
    result.reset_mode = ResetModeName(options.reset_mode);
    result.cta_owner_mode = CtaOwnerModeName(options.cta_owner_mode);
    result.cta_queue_mode = CtaQueueModeName(options.cta_queue_mode);
    result.cta_handoff_mode = CtaHandoffModeName(options.cta_handoff_mode);
    result.cta_local_round_budget = CtaLocalRoundBudget(options);
    result.cta_replay_round_budget = CtaReplayRoundBudget(options);
    result.cta_dirty_pull_min_degree =
        std::max(0, options.cta_dirty_pull_min_degree);
    result.effective_frontier_mode = FrontierModeName(effective_frontier);
    result.effective_kernel_variant = KernelVariantName(effective_kernel);
    result.effective_bitsup_layout = BitSupLayoutName(effective_bitsup);
    result.owner_map_build_ms = owner_map_build_ms;
    result.owner_balance_p95 = owner_balance_p95;
    result.owner_weight_balance_p95 = owner_weight_balance_p95;
    result.variant_name =
        VariantName(options, layout.num_constraints, layout.max_dom_size,
                    layout.bit_words);
    result.setup_ms = result.prepare_ms + result.reset_ms;
    int active = InitialActiveCount();
    const int max_iterations = std::max(1, options.max_iterations);
    const int density_denominator = std::max(1, layout.num_constraints);

    while (active > 0) {
      if (result.iterations >= max_iterations) {
        result.budget_exceeded = true;
        break;
      }

      ClearIterationBuffers(effective_frontier);
      result.active_constraints_total += active;
      if (effective_frontier == MetalFrontierMode::kCompact) {
        ClearCompactBuffers();
        auto compact_timings_or = DispatchCompactFrontier();
        if (!compact_timings_or.ok()) return compact_timings_or.status();
        AddTiming(&result, *compact_timings_or);
        ++result.dispatch_count;

        int32_t* compact = static_cast<int32_t*>(compact_stats.contents());
        active = compact[0];
        if (active == 0) {
          break;
        }

        auto revise_timings_or =
            effective_kernel == MetalKernelVariant::kWordParallel
                ? DispatchWordActiveRevise(active)
                : DispatchCompactRevise(active);
        if (!revise_timings_or.ok()) return revise_timings_or.status();
        AddTiming(&result, *revise_timings_or);
        ++result.dispatch_count;
      } else if (effective_frontier == MetalFrontierMode::kWorklist) {
        AdvanceWorklistEpoch(&result);
        auto timings_or =
            effective_kernel == MetalKernelVariant::kWordParallel
                ? DispatchWordWorklistRevise(active, frontier_epoch)
                : DispatchWorklistRevise(active, frontier_epoch);
        if (!timings_or.ok()) return timings_or.status();
        AddTiming(&result, *timings_or);
        ++result.dispatch_count;
      } else if (effective_frontier == MetalFrontierMode::kCtaWorklist) {
        AdvanceCtaWorklistEpoch(&result);
        SeedCtaQueues(active, &result);
        auto timings_or = DispatchCtaWorklistRevise(active, frontier_epoch);
        if (!timings_or.ok()) return timings_or.status();
        AddTiming(&result, *timings_or);
        ++result.dispatch_count;
        PullDirtyVarsToNextActive();
      } else if (effective_frontier == MetalFrontierMode::kBulkSyncMask) {
        AdvanceWorklistEpoch(&result);
        auto revise_timings_or = DispatchBulkMaskRevise(active);
        if (!revise_timings_or.ok()) return revise_timings_or.status();
        AddTiming(&result, *revise_timings_or);
        ++result.dispatch_count;

        auto apply_timings_or = DispatchBulkMaskApply(frontier_epoch);
        if (!apply_timings_or.ok()) return apply_timings_or.status();
        AddTiming(&result, *apply_timings_or);
        ++result.dispatch_count;
      } else {
        auto timings_or =
            effective_kernel == MetalKernelVariant::kWordParallel
                ? DispatchWordFlagRevise()
                : DispatchFlagRevise();
        if (!timings_or.ok()) return timings_or.status();
        AddTiming(&result, *timings_or);
        ++result.dispatch_count;
      }

      int32_t* stats = static_cast<int32_t*>(stats_buffer.contents());
      result.deletions += stats[0];
      result.inconsistent = result.inconsistent || (stats[1] != 0);
      if (effective_frontier == MetalFrontierMode::kWorklist) {
        result.worklist_push_count += stats[2];
        ++result.worklist_rounds;
      } else if (effective_frontier == MetalFrontierMode::kCtaWorklist) {
        result.worklist_push_count += stats[2];
        ++result.worklist_rounds;
        result.cta_local_rounds += stats[3];
        result.cta_queue_push_count += stats[4];
        result.cta_cross_push_count += stats[5];
        result.cta_overflow_count += stats[6];
        result.owner_local_push_count += stats[7];
        result.owner_cross_push_count += stats[8];
        result.cta_queue_overflow_count += stats[9];
        result.cta_budget_spill_count += stats[10];
        result.cta_budget_replay_rounds += stats[11];
        result.cta_budget_replay_drain_count += stats[12];
        result.cta_budget_replay_spill_count += stats[13];
        result.dirty_var_count += stats[18];
        result.dirty_pull_scan_count += stats[19];
        result.dirty_pull_hit_count += stats[20];
        result.cross_push_avoided_count += stats[21];
        result.dirty_pull_fallback_push_count += stats[22];
      } else if (effective_frontier == MetalFrontierMode::kBulkSyncMask) {
        result.worklist_push_count += stats[2];
        ++result.worklist_rounds;
        result.bulk_mask_proposed_deletion_count += stats[14];
        result.bulk_mask_actual_deletion_count += stats[15];
        result.bulk_mask_changed_word_count += stats[16];
        result.bulk_mask_frontier_push_count += stats[17];
        ++result.bulk_mask_rounds;
      }
      active = stats[2];
      ++result.iterations;
      ++result.host_round_count;

      if (options.verbose) {
        std::cout << "[MetalGAC] iter=" << result.iterations
                  << " deletions_this_iter=" << stats[0]
                  << " active_next=" << active
                  << " dispatch_ms_total=" << result.dispatch_ms
                  << " kernel_ms_total=" << result.kernel_ms
                  << " variant=" << result.variant_name
                  << " inconsistent=" << (result.inconsistent ? "true" : "false")
                  << std::endl;
      }

      if (UsesNextActiveConstraints(effective_frontier)) {
        std::swap(active_constraints, next_active_constraints);
      } else {
        std::swap(current_flags, next_flags);
      }

      if (result.inconsistent) {
        break;
      }
    }

    const auto total_end = std::chrono::steady_clock::now();
    result.elapsed_ms =
        std::chrono::duration<double, std::milli>(total_end - total_start).count();
    result.solve_ms = result.reset_ms + result.dispatch_ms;
    if (result.iterations > 0) {
      result.frontier_density_avg =
          static_cast<double>(result.active_constraints_total) /
          static_cast<double>(result.iterations * density_denominator);
    }
    CopyResultsBack();
    return result;
  }

  cpim::model::DeviceModelLayout layout;
  MetalGacOptions options;
  std::vector<uint32_t> bit_dom_result;
  std::vector<int32_t> domain_sizes_result;
  std::vector<int32_t> initial_frontier;
  std::vector<int32_t> initial_active_constraints;
  std::string device_name;
  bool prepared = false;
  double last_prepare_ms = 0.0;
  double owner_map_build_ms = 0.0;
  double owner_balance_p95 = 0.0;
  double owner_weight_balance_p95 = 0.0;
  int32_t frontier_epoch = 1;
  int32_t cta_count = 1;
  int32_t cta_queue_capacity = 1;
  std::vector<int32_t> cta_owner_map;

  MetalRuntime runtime;
  MetalPipeline revise_pipeline;
  MetalPipeline compact_pipeline;
  MetalPipeline compact_revise_pipeline;
  MetalPipeline worklist_revise_pipeline;
  MetalPipeline word_flags_pipeline;
  MetalPipeline word_active_pipeline;
  MetalPipeline word_worklist_pipeline;
  MetalPipeline cta_worklist_pipeline;
  MetalPipeline bulk_mask_revise_pipeline;
  MetalPipeline bulk_mask_apply_pipeline;
  MetalBuffer bit_dom;
  MetalBuffer initial_bit_dom;
  MetalBuffer domain_sizes;
  MetalBuffer initial_domain_sizes;
  MetalBuffer bit_sup;
  MetalBuffer bit_sup_words;
  MetalBuffer scopes;
  MetalBuffer sub_offsets;
  MetalBuffer sub_entries;
  MetalBuffer current_flags;
  MetalBuffer initial_frontier_buffer;
  MetalBuffer next_flags;
  MetalBuffer stats_buffer;
  MetalBuffer active_constraints;
  MetalBuffer initial_active_constraints_buffer;
  MetalBuffer next_active_constraints;
  MetalBuffer compact_stats;
  MetalBuffer cta_queue_a;
  MetalBuffer cta_queue_b;
  MetalBuffer cta_tail_a;
  MetalBuffer cta_tail_b;
  MetalBuffer cta_stamps;
  MetalBuffer cta_owner_map_buffer;
  MetalBuffer cta_dirty_var_epochs;
  MetalBuffer bulk_delete_masks;
};

MetalGacSolver::MetalGacSolver(const cpim::model::DeviceModelLayout& layout,
                               MetalGacOptions options)
    : impl_(std::make_unique<Impl>(layout, std::move(options))) {}

MetalGacSolver::~MetalGacSolver() = default;

MetalGacSolver::MetalGacSolver(MetalGacSolver&&) noexcept = default;

MetalGacSolver& MetalGacSolver::operator=(MetalGacSolver&&) noexcept = default;

absl::StatusOr<MetalGacStats> MetalGacSolver::Run() {
  return impl_->Run(MetalRunnerMode::kCold);
}

const std::vector<uint32_t>& MetalGacSolver::bit_dom() const {
  return impl_->bit_dom_result;
}

const std::vector<int32_t>& MetalGacSolver::domain_sizes() const {
  return impl_->domain_sizes_result;
}

struct MetalPreparedGacRunner::Impl {
  Impl(const cpim::model::DeviceModelLayout& layout, MetalGacOptions options)
      : solver_impl(layout, std::move(options)) {}

  MetalGacSolver::Impl solver_impl;
};

MetalPreparedGacRunner::MetalPreparedGacRunner(
    const cpim::model::DeviceModelLayout& layout, MetalGacOptions options)
    : impl_(std::make_unique<Impl>(layout, std::move(options))) {}

MetalPreparedGacRunner::~MetalPreparedGacRunner() = default;

MetalPreparedGacRunner::MetalPreparedGacRunner(
    MetalPreparedGacRunner&&) noexcept = default;

MetalPreparedGacRunner& MetalPreparedGacRunner::operator=(
    MetalPreparedGacRunner&&) noexcept = default;

absl::Status MetalPreparedGacRunner::Prepare() {
  return impl_->solver_impl.Prepare();
}

absl::StatusOr<MetalGacStats> MetalPreparedGacRunner::Run() {
  return impl_->solver_impl.Run(MetalRunnerMode::kPrepared);
}

const std::vector<uint32_t>& MetalPreparedGacRunner::bit_dom() const {
  return impl_->solver_impl.bit_dom_result;
}

const std::vector<int32_t>& MetalPreparedGacRunner::domain_sizes() const {
  return impl_->solver_impl.domain_sizes_result;
}

}  // namespace cpim::solver::metal
