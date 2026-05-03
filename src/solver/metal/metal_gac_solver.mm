#include "solver/metal/metal_gac_solver.h"

#include <algorithm>
#include <chrono>
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
};

struct CompactParamsMetal {
  int32_t num_constraints = 0;
};

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
  if (timings.gpu_timing_available) {
    stats->kernel_ms += timings.kernel_ms;
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
        runtime.NewSharedBuffer(sizeof(int32_t) * 3, "cpim.gac_stats");
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
    if (effective_frontier == MetalFrontierMode::kCompact ||
        effective_frontier == MetalFrontierMode::kWorklist) {
      auto active_or = runtime.NewSharedBuffer(
          sizeof(int32_t) * static_cast<size_t>(layout.num_constraints),
          "cpim.active_constraints");
      if (!active_or.ok()) return active_or.status();
      active_constraints = std::move(*active_or);
    }

    if (effective_frontier == MetalFrontierMode::kWorklist) {
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

  size_t MutableResetBytes(MetalFrontierMode effective_frontier) const {
    size_t bytes = layout.bit_dom.size() * sizeof(uint32_t);
    bytes += layout.domain_sizes.size() * sizeof(int32_t);
    bytes += initial_frontier.size() * sizeof(int32_t);
    bytes += static_cast<size_t>(layout.num_constraints) * sizeof(int32_t);
    bytes += 3 * sizeof(int32_t);
    if (effective_frontier == MetalFrontierMode::kWorklist) {
      bytes += 2 * static_cast<size_t>(layout.num_constraints) * sizeof(int32_t);
    }
    if (effective_frontier == MetalFrontierMode::kCompact ||
        effective_frontier == MetalFrontierMode::kWorklist) {
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
    std::memset(stats_buffer.contents(), 0, sizeof(int32_t) * 3);
    if (next_active_constraints.valid()) {
      std::memset(next_active_constraints.contents(), 0,
                  sizeof(int32_t) * static_cast<size_t>(layout.num_constraints));
    }
    ClearCompactBuffers();
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
    if (effective_frontier == MetalFrontierMode::kWorklist &&
        initial_active_constraints_buffer.valid() &&
        active_constraints.valid()) {
      copies.push_back({&initial_active_constraints_buffer, &active_constraints,
                        initial_active_constraints.size() * sizeof(int32_t)});
    }

    std::vector<MetalBufferFill> fills;
    fills.push_back({&next_flags,
                     sizeof(int32_t) * static_cast<size_t>(layout.num_constraints),
                     0});
    fills.push_back({&stats_buffer, sizeof(int32_t) * 3, 0});
    if (next_active_constraints.valid()) {
      fills.push_back(
          {&next_active_constraints,
           sizeof(int32_t) * static_cast<size_t>(layout.num_constraints), 0});
    }
    if (compact_stats.valid()) {
      fills.push_back({&compact_stats, sizeof(int32_t), 0});
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
    if (effective_frontier != MetalFrontierMode::kWorklist) {
      std::memset(next_flags.contents(), 0,
                  sizeof(int32_t) * static_cast<size_t>(layout.num_constraints));
    }
    std::memset(stats_buffer.contents(), 0, sizeof(int32_t) * 3);
    if (effective_frontier != MetalFrontierMode::kWorklist &&
        next_active_constraints.valid()) {
      std::memset(next_active_constraints.contents(), 0,
                  sizeof(int32_t) * static_cast<size_t>(layout.num_constraints));
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

  void ClearCompactBuffers() {
    if (compact_stats.valid()) {
      std::memset(compact_stats.contents(), 0, sizeof(int32_t));
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
    result.effective_frontier_mode = FrontierModeName(effective_frontier);
    result.effective_kernel_variant = KernelVariantName(effective_kernel);
    result.effective_bitsup_layout = BitSupLayoutName(effective_bitsup);
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
      }
      active = stats[2];
      ++result.iterations;

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

      if (effective_frontier == MetalFrontierMode::kWorklist) {
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
  int32_t frontier_epoch = 1;

  MetalRuntime runtime;
  MetalPipeline revise_pipeline;
  MetalPipeline compact_pipeline;
  MetalPipeline compact_revise_pipeline;
  MetalPipeline worklist_revise_pipeline;
  MetalPipeline word_flags_pipeline;
  MetalPipeline word_active_pipeline;
  MetalPipeline word_worklist_pipeline;
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
