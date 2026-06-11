#include "solver/metal/metal_sac_batch_probe.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

#include "absl/status/status.h"
#include "absl/strings/str_format.h"
#include "solver/metal/metal_runtime.h"

namespace cpim::solver::metal {
namespace {

constexpr int kStatsActiveNext = 0;
constexpr int kStatsDeletions = 1;
constexpr int kStatsDwo = 2;
constexpr int kStatsUnknown = 3;
constexpr int kStatsInitialActive = 4;
constexpr int kStatsCount = 8;
constexpr int kDwoDebugWordsPerWorld = 6;

std::string ProbeFusionName(MetalSacProbeFusion fusion) {
  return fusion == MetalSacProbeFusion::kBounded ? "bounded" : "none";
}

template <typename T>
absl::StatusOr<MetalBuffer> NewSharedBufferWithVector(
    const MetalRuntime& runtime, const std::vector<T>& values,
    const std::string& label) {
  const void* data = values.empty() ? nullptr : values.data();
  return runtime.NewSharedBufferWithBytes(
      data, values.size() * sizeof(T), label);
}

std::vector<uint8_t> ParamsBytes(const void* params, size_t params_size) {
  std::vector<uint8_t> bytes(params_size);
  if (params != nullptr && params_size > 0) {
    std::memcpy(bytes.data(), params, params_size);
  }
  return bytes;
}

void AddTiming(MetalBatchProbeStats* stats,
               const MetalDispatchTimings& timings,
               int dispatch_count = 1) {
  stats->dispatch_ms += timings.wall_ms;
  stats->dispatch_encode_ms += timings.encode_ms;
  stats->dispatch_wait_ms += timings.wall_ms;
  if (timings.gpu_timing_available) {
    stats->kernel_ms += timings.kernel_ms;
    stats->dispatch_non_kernel_ms +=
        std::max(0.0, timings.wall_ms - timings.kernel_ms);
    stats->gpu_timing_available = true;
  }
  stats->dispatch_count += dispatch_count;
  ++stats->command_buffer_count;
}

struct SacProbeTaskGpu {
  int32_t var_id = -1;
  int32_t value = -1;
  int32_t task_id = -1;
};

struct SacProbeParams {
  int32_t num_worlds = 0;
  int32_t num_vars = 0;
  int32_t num_constraints = 0;
  int32_t max_dom_size = 0;
  int32_t bit_words = 0;
  int32_t activation_mode = 0;
  int32_t current_round = 0;
  int32_t max_probe_rounds = 0;
  int32_t use_allowed_constraint_mask = 0;
  int32_t fused_round_slot = 0;
};

}  // namespace

struct MetalBatchProbeRunner::Impl {
  Impl(const cpim::model::DeviceModelLayout& input_layout,
       std::vector<uint32_t> input_snapshot_bit_dom,
       std::vector<int32_t> input_snapshot_domain_sizes,
       std::vector<MetalSacProbeTask> input_tasks,
       MetalBatchProbeOptions input_options)
      : layout(input_layout),
        snapshot_bit_dom(std::move(input_snapshot_bit_dom)),
        snapshot_domain_sizes(std::move(input_snapshot_domain_sizes)),
        tasks(std::move(input_tasks)),
        options(std::move(input_options)) {}

  absl::Status Prepare() {
    if (layout.num_vars <= 0 || layout.num_constraints < 0 ||
        layout.bit_words <= 0 || layout.max_dom_size <= 0) {
      return absl::InvalidArgumentError("Metal batch probe requires a valid layout");
    }
    if (snapshot_bit_dom.size() != layout.bit_dom.size()) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "snapshot bit_dom size mismatch: got %zu want %zu",
          snapshot_bit_dom.size(), layout.bit_dom.size()));
    }
    if (snapshot_domain_sizes.size() != layout.domain_sizes.size()) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "snapshot domain_sizes size mismatch: got %zu want %zu",
          snapshot_domain_sizes.size(), layout.domain_sizes.size()));
    }
    if (!options.allowed_constraints.empty() &&
        options.allowed_constraints.size() !=
            static_cast<size_t>(layout.num_constraints)) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "allowed_constraints size mismatch: got %zu want %d",
          options.allowed_constraints.size(), layout.num_constraints));
    }
    if (tasks.empty()) {
      statuses_result.clear();
      return absl::OkStatus();
    }
    auto runtime_or = MetalRuntime::CreateDefault();
    if (!runtime_or.ok()) return runtime_or.status();
    runtime = std::move(*runtime_or);
    device_name = runtime.device_name();

    auto init_or = runtime.LoadComputePipeline(options.metallib_path,
                                               "sac_probe_init_kernel");
    if (!init_or.ok()) return init_or.status();
    init_pipeline = std::move(*init_or);
    auto revise_or = runtime.LoadComputePipeline(options.metallib_path,
                                                 "sac_probe_revise_kernel");
    if (!revise_or.ok()) return revise_or.status();
    revise_pipeline = std::move(*revise_or);
    auto frontier_or = runtime.LoadComputePipeline(options.metallib_path,
                                                   "sac_probe_frontier_kernel");
    if (!frontier_or.ok()) return frontier_or.status();
    frontier_pipeline = std::move(*frontier_or);
    auto clear_active_or = runtime.LoadComputePipeline(
        options.metallib_path, "sac_probe_clear_active_counts_kernel");
    if (!clear_active_or.ok()) return clear_active_or.status();
    clear_active_pipeline = std::move(*clear_active_or);
    auto frontier_fused_or = runtime.LoadComputePipeline(
        options.metallib_path, "sac_probe_frontier_fused_kernel");
    if (!frontier_fused_or.ok()) return frontier_fused_or.status();
    frontier_fused_pipeline = std::move(*frontier_fused_or);
    auto mark_or = runtime.LoadComputePipeline(options.metallib_path,
                                               "sac_probe_mark_unknown_kernel");
    if (!mark_or.ok()) return mark_or.status();
    mark_unknown_pipeline = std::move(*mark_or);

    return InitBuffers();
  }

  absl::Status InitBuffers() {
    auto snapshot_bit_dom_or = NewSharedBufferWithVector(
        runtime, snapshot_bit_dom, "cpim.sac.snapshot_bit_dom");
    if (!snapshot_bit_dom_or.ok()) return snapshot_bit_dom_or.status();
    snapshot_bit_dom_buffer = std::move(*snapshot_bit_dom_or);

    auto snapshot_domain_sizes_or = NewSharedBufferWithVector(
        runtime, snapshot_domain_sizes, "cpim.sac.snapshot_domain_sizes");
    if (!snapshot_domain_sizes_or.ok()) return snapshot_domain_sizes_or.status();
    snapshot_domain_sizes_buffer = std::move(*snapshot_domain_sizes_or);

    auto bit_sup_words_or = NewSharedBufferWithVector(
        runtime, layout.bit_sup_words, "cpim.sac.bit_sup_words");
    if (!bit_sup_words_or.ok()) return bit_sup_words_or.status();
    bit_sup_words_buffer = std::move(*bit_sup_words_or);

    auto scopes_or = NewSharedBufferWithVector(
        runtime, layout.constraint_scopes, "cpim.sac.constraint_scopes");
    if (!scopes_or.ok()) return scopes_or.status();
    scopes_buffer = std::move(*scopes_or);

    auto sub_offsets_or = NewSharedBufferWithVector(
        runtime, layout.subscriptions.offsets, "cpim.sac.subscription_offsets");
    if (!sub_offsets_or.ok()) return sub_offsets_or.status();
    sub_offsets_buffer = std::move(*sub_offsets_or);

    auto sub_entries_or = NewSharedBufferWithVector(
        runtime, layout.subscriptions.entries, "cpim.sac.subscription_entries");
    if (!sub_entries_or.ok()) return sub_entries_or.status();
    sub_entries_buffer = std::move(*sub_entries_or);

    std::vector<int32_t> allowed = options.allowed_constraints;
    if (allowed.empty()) {
      allowed.assign(static_cast<size_t>(layout.num_constraints), 1);
    }
    auto allowed_or = NewSharedBufferWithVector(
        runtime, allowed, "cpim.sac.allowed_constraints");
    if (!allowed_or.ok()) return allowed_or.status();
    allowed_constraints_buffer = std::move(*allowed_or);
    allowed_constraints_count = 0;
    for (int32_t value : allowed) {
      if (value != 0) ++allowed_constraints_count;
    }

    std::vector<SacProbeTaskGpu> gpu_tasks;
    gpu_tasks.reserve(tasks.size());
    for (const MetalSacProbeTask& task : tasks) {
      gpu_tasks.push_back(SacProbeTaskGpu{task.var_id, task.value, task.task_id});
    }
    auto tasks_or =
        NewSharedBufferWithVector(runtime, gpu_tasks, "cpim.sac.tasks");
    if (!tasks_or.ok()) return tasks_or.status();
    tasks_buffer = std::move(*tasks_or);

    const size_t worlds = tasks.size();
    const size_t bit_dom_words =
        worlds * static_cast<size_t>(layout.num_vars) * layout.bit_words;
    const size_t domain_sizes_count =
        worlds * static_cast<size_t>(layout.num_vars);
    const size_t frontier_count =
        worlds * static_cast<size_t>(std::max(1, layout.num_constraints));
    auto bit_dom_or = runtime.NewSharedBuffer(
        bit_dom_words * sizeof(uint32_t), "cpim.sac.world_bit_dom");
    if (!bit_dom_or.ok()) return bit_dom_or.status();
    world_bit_dom_buffer = std::move(*bit_dom_or);

    auto domain_sizes_or = runtime.NewSharedBuffer(
        domain_sizes_count * sizeof(int32_t), "cpim.sac.world_domain_sizes");
    if (!domain_sizes_or.ok()) return domain_sizes_or.status();
    world_domain_sizes_buffer = std::move(*domain_sizes_or);

    auto current_or = runtime.NewSharedBuffer(
        frontier_count * sizeof(int32_t), "cpim.sac.current_frontier");
    if (!current_or.ok()) return current_or.status();
    current_frontier_buffer = std::move(*current_or);

    auto next_or = runtime.NewSharedBuffer(
        frontier_count * sizeof(int32_t), "cpim.sac.next_frontier");
    if (!next_or.ok()) return next_or.status();
    next_frontier_buffer = std::move(*next_or);

    auto statuses_or = runtime.NewSharedBuffer(
        worlds * sizeof(int32_t), "cpim.sac.statuses");
    if (!statuses_or.ok()) return statuses_or.status();
    statuses_buffer = std::move(*statuses_or);

    auto dwo_debug_or = runtime.NewSharedBuffer(
        worlds * kDwoDebugWordsPerWorld * sizeof(int32_t),
        "cpim.sac.dwo_debug");
    if (!dwo_debug_or.ok()) return dwo_debug_or.status();
    dwo_debug_buffer = std::move(*dwo_debug_or);

    const size_t active_count_slots =
        static_cast<size_t>(std::max(1, options.fusion_rounds));
    auto active_counts_or = runtime.NewSharedBuffer(
        active_count_slots * sizeof(int32_t), "cpim.sac.active_counts");
    if (!active_counts_or.ok()) return active_counts_or.status();
    active_counts_buffer = std::move(*active_counts_or);

    auto stats_or = runtime.NewSharedBuffer(kStatsCount * sizeof(int32_t),
                                            "cpim.sac.stats");
    if (!stats_or.ok()) return stats_or.status();
    stats_buffer = std::move(*stats_or);

    statuses_result.assign(tasks.size(), MetalSacProbeStatus::kUnknown);
    return absl::OkStatus();
  }

  SacProbeParams Params(int current_round, int fused_round_slot = 0) const {
    SacProbeParams params;
    params.num_worlds = static_cast<int32_t>(tasks.size());
    params.num_vars = layout.num_vars;
    params.num_constraints = layout.num_constraints;
    params.max_dom_size = layout.max_dom_size;
    params.bit_words = layout.bit_words;
    params.activation_mode =
        options.activation_mode == MetalSacActivationMode::kFull ? 1 : 0;
    params.current_round = current_round;
    params.max_probe_rounds = options.budget.max_probe_rounds;
    params.use_allowed_constraint_mask =
        options.allowed_constraints.empty() ? 0 : 1;
    params.fused_round_slot = fused_round_slot;
    return params;
  }

  absl::Status Dispatch(const MetalPipeline& pipeline,
                        const std::vector<MetalBufferBinding>& bindings,
                        const SacProbeParams& params,
                        uint64_t grid_size,
                        MetalBatchProbeStats* stats) const {
    if (grid_size == 0) return absl::OkStatus();
    auto timing_or = runtime.Dispatch1D(
        pipeline, bindings, &params, sizeof(params), 10, grid_size,
        static_cast<uint64_t>(std::max(1, options.threads_per_threadgroup)));
    if (!timing_or.ok()) return timing_or.status();
    AddTiming(stats, *timing_or);
    return absl::OkStatus();
  }

  MetalComputeDispatch1D DispatchCommand(
      const MetalPipeline& pipeline,
      std::vector<MetalBufferBinding> bindings,
      const SacProbeParams& params,
      uint64_t grid_size) const {
    MetalComputeDispatch1D command;
    command.pipeline = &pipeline;
    command.bindings = std::move(bindings);
    command.params = ParamsBytes(&params, sizeof(params));
    command.params_index = 10;
    command.grid_size = grid_size;
    command.threads_per_threadgroup =
        static_cast<uint64_t>(std::max(1, options.threads_per_threadgroup));
    return command;
  }

  absl::Status DispatchBatch(std::vector<MetalComputeDispatch1D> commands,
                             MetalBatchProbeStats* stats) const {
    commands.erase(
        std::remove_if(commands.begin(), commands.end(),
                       [](const MetalComputeDispatch1D& command) {
                         return command.grid_size == 0;
                       }),
        commands.end());
    if (commands.empty()) return absl::OkStatus();
    const int dispatch_count = static_cast<int>(commands.size());
    auto timing_or = runtime.Dispatch1DBatch(commands);
    if (!timing_or.ok()) return timing_or.status();
    AddTiming(stats, *timing_or, dispatch_count);
    return absl::OkStatus();
  }

  uint64_t InitGrid() const {
    return std::max(
        static_cast<uint64_t>(tasks.size()) *
            static_cast<uint64_t>(std::max(1, layout.num_constraints)),
        std::max(static_cast<uint64_t>(tasks.size()) *
                     static_cast<uint64_t>(layout.num_vars) *
                     static_cast<uint64_t>(layout.bit_words),
                 static_cast<uint64_t>(tasks.size()) *
                     static_cast<uint64_t>(layout.num_vars)));
  }

  uint64_t ReviseGrid() const {
    return static_cast<uint64_t>(tasks.size()) *
           static_cast<uint64_t>(std::max(1, layout.num_constraints)) * 2u *
           static_cast<uint64_t>(layout.bit_words);
  }

  uint64_t FrontierGrid() const {
    return static_cast<uint64_t>(tasks.size()) *
           static_cast<uint64_t>(std::max(1, layout.num_constraints));
  }

  std::vector<MetalBufferBinding> InitBindings() const {
    return {
        {0, &snapshot_bit_dom_buffer},
        {1, &snapshot_domain_sizes_buffer},
        {2, &tasks_buffer},
        {3, &scopes_buffer},
        {4, &world_bit_dom_buffer},
        {5, &world_domain_sizes_buffer},
        {6, &current_frontier_buffer},
        {7, &next_frontier_buffer},
        {8, &statuses_buffer},
        {9, &stats_buffer},
        {11, &allowed_constraints_buffer},
        {12, &dwo_debug_buffer},
    };
  }

  std::vector<MetalBufferBinding> ReviseBindings() const {
    return {
        {0, &world_bit_dom_buffer},
        {1, &world_domain_sizes_buffer},
        {2, &bit_sup_words_buffer},
        {3, &scopes_buffer},
        {4, &sub_offsets_buffer},
        {5, &sub_entries_buffer},
        {6, &current_frontier_buffer},
        {7, &next_frontier_buffer},
        {8, &statuses_buffer},
        {9, &stats_buffer},
        {11, &allowed_constraints_buffer},
        {12, &dwo_debug_buffer},
    };
  }

  std::vector<MetalBufferBinding> FrontierBindings() const {
    return {
        {0, &current_frontier_buffer},
        {1, &next_frontier_buffer},
        {2, &statuses_buffer},
        {3, &stats_buffer},
    };
  }

  std::vector<MetalBufferBinding> ClearActiveBindings() const {
    return {
        {0, &active_counts_buffer},
    };
  }

  std::vector<MetalBufferBinding> FrontierFusedBindings() const {
    return {
        {0, &current_frontier_buffer},
        {1, &next_frontier_buffer},
        {2, &statuses_buffer},
        {3, &stats_buffer},
        {4, &active_counts_buffer},
    };
  }

  void FinishResults(MetalBatchProbeStats* stats) {
    const int32_t* raw_status =
        static_cast<const int32_t*>(statuses_buffer.contents());
    statuses_result.clear();
    statuses_result.reserve(tasks.size());
    for (size_t i = 0; i < tasks.size(); ++i) {
      MetalSacProbeStatus probe_status = MetalSacProbeStatus::kOk;
      if (raw_status[i] == static_cast<int32_t>(MetalSacProbeStatus::kDwo)) {
        probe_status = MetalSacProbeStatus::kDwo;
      } else if (raw_status[i] ==
                 static_cast<int32_t>(MetalSacProbeStatus::kUnknown)) {
        probe_status = MetalSacProbeStatus::kUnknown;
      }
      statuses_result.push_back(probe_status);
      if (probe_status == MetalSacProbeStatus::kDwo) {
        ++stats->dwo_count;
      } else if (probe_status == MetalSacProbeStatus::kUnknown) {
        ++stats->unknown_count;
      } else {
        ++stats->ok_count;
      }
    }
    if (options.collect_world_domains) {
      const size_t bit_dom_words =
          tasks.size() * static_cast<size_t>(layout.num_vars) * layout.bit_words;
      const size_t domain_sizes_count =
          tasks.size() * static_cast<size_t>(layout.num_vars);
      const uint32_t* raw_bit_dom =
          static_cast<const uint32_t*>(world_bit_dom_buffer.contents());
      const int32_t* raw_domain_sizes =
          static_cast<const int32_t*>(world_domain_sizes_buffer.contents());
      world_bit_dom_result.assign(raw_bit_dom, raw_bit_dom + bit_dom_words);
      world_domain_sizes_result.assign(raw_domain_sizes,
                                       raw_domain_sizes + domain_sizes_count);
      const int32_t* raw_dwo_debug =
          static_cast<const int32_t*>(dwo_debug_buffer.contents());
      dwo_debug_words_result.assign(
          raw_dwo_debug,
          raw_dwo_debug + tasks.size() * kDwoDebugWordsPerWorld);
    } else {
      world_bit_dom_result.clear();
      world_domain_sizes_result.clear();
      dwo_debug_words_result.clear();
    }

    stats->device_name = device_name;
    if (stats->elapsed_ms > 0.0) {
      stats->probes_per_sec =
          static_cast<double>(stats->probes) / (stats->elapsed_ms / 1000.0);
    }
    if (stats->probes > 0) {
      stats->dispatch_per_probe =
          static_cast<double>(stats->dispatch_count) / stats->probes;
      stats->command_buffer_per_probe =
          static_cast<double>(stats->command_buffer_count) / stats->probes;
      stats->non_kernel_per_probe =
          stats->dispatch_non_kernel_ms / stats->probes;
    }
  }

  absl::Status RunUnfused(MetalBatchProbeStats* stats) {
    const SacProbeParams init_params = Params(0);
    absl::Status status =
        Dispatch(init_pipeline, InitBindings(), init_params, InitGrid(), stats);
    if (!status.ok()) return status;

    const int32_t* stats_words =
        static_cast<const int32_t*>(stats_buffer.contents());
    int active = stats_words[kStatsInitialActive];
    stats->active_frontier_total += active;
    const int max_rounds = std::max(0, options.budget.max_probe_rounds);
    while (active > 0 && stats->rounds < max_rounds) {
      const SacProbeParams round_params = Params(stats->rounds);
      status = Dispatch(revise_pipeline, ReviseBindings(), round_params,
                        ReviseGrid(), stats);
      if (!status.ok()) return status;

      std::memset(stats_buffer.contents(), 0, sizeof(int32_t));
      status = Dispatch(frontier_pipeline, FrontierBindings(), round_params,
                        FrontierGrid(), stats);
      if (!status.ok()) return status;

      ++stats->rounds;
      active =
          static_cast<const int32_t*>(stats_buffer.contents())[kStatsActiveNext];
      stats->active_frontier_total += active;
    }

    if (active > 0) {
      stats->budget_exceeded = true;
      const SacProbeParams mark_params = Params(stats->rounds);
      status = Dispatch(mark_unknown_pipeline, FrontierBindings(), mark_params,
                        static_cast<uint64_t>(tasks.size()), stats);
      if (!status.ok()) return status;
    }
    return absl::OkStatus();
  }

  absl::Status RunBounded(MetalBatchProbeStats* stats) {
    const int max_rounds = std::max(0, options.budget.max_probe_rounds);
    const int fusion_rounds = std::max(1, options.fusion_rounds);
    stats->fusion_rounds = fusion_rounds;
    int active = 0;
    bool initialized = false;

    while (stats->rounds < max_rounds || !initialized) {
      const int remaining_rounds = std::max(0, max_rounds - stats->rounds);
      const int segment_rounds =
          initialized ? std::min(fusion_rounds, remaining_rounds)
                      : std::min(fusion_rounds, max_rounds);
      if (segment_rounds <= 0 && initialized) {
        break;
      }

      std::vector<MetalComputeDispatch1D> commands;
      commands.reserve(static_cast<size_t>(1 + segment_rounds * 3));
      if (!initialized) {
        commands.push_back(DispatchCommand(init_pipeline, InitBindings(),
                                           Params(0), InitGrid()));
      }
      for (int slot = 0; slot < segment_rounds; ++slot) {
        const int round = stats->rounds + slot;
        const SacProbeParams round_params = Params(round, slot);
        commands.push_back(DispatchCommand(revise_pipeline, ReviseBindings(),
                                           round_params, ReviseGrid()));
        commands.push_back(DispatchCommand(clear_active_pipeline,
                                           ClearActiveBindings(), round_params,
                                           1));
        commands.push_back(DispatchCommand(
            frontier_fused_pipeline, FrontierFusedBindings(), round_params,
            FrontierGrid()));
      }

      absl::Status status = DispatchBatch(std::move(commands), stats);
      if (!status.ok()) return status;
      stats->fused_rounds_encoded += segment_rounds;

      const int32_t* active_counts =
          static_cast<const int32_t*>(active_counts_buffer.contents());
      if (!initialized) {
        const int initial_active =
            static_cast<const int32_t*>(stats_buffer.contents())[kStatsInitialActive];
        stats->active_frontier_total += initial_active;
        initialized = true;
        if (initial_active == 0) {
          stats->fused_rounds_wasted += segment_rounds;
          active = 0;
          break;
        }
      }
      if (segment_rounds <= 0) {
        break;
      }

      bool converged = false;
      for (int slot = 0; slot < segment_rounds; ++slot) {
        active = active_counts[slot];
        stats->active_frontier_total += active;
        ++stats->rounds;
        if (active == 0) {
          stats->fused_rounds_wasted += segment_rounds - slot - 1;
          converged = true;
          break;
        }
      }
      if (converged) {
        break;
      }
    }

    if (active > 0 && stats->rounds >= max_rounds) {
      stats->budget_exceeded = true;
      const SacProbeParams mark_params = Params(stats->rounds);
      absl::Status status =
          Dispatch(mark_unknown_pipeline, FrontierBindings(), mark_params,
                   static_cast<uint64_t>(tasks.size()), stats);
      if (!status.ok()) return status;
    }
    return absl::OkStatus();
  }

  absl::StatusOr<MetalBatchProbeStats> Run() {
    MetalBatchProbeStats stats;
    stats.probes = static_cast<int>(tasks.size());
    stats.fusion_rounds =
        options.probe_fusion == MetalSacProbeFusion::kBounded
            ? std::max(1, options.fusion_rounds)
            : 0;
    stats.allowed_constraints_count = static_cast<int>(
        options.allowed_constraints.empty()
            ? layout.num_constraints
            : std::count_if(options.allowed_constraints.begin(),
                            options.allowed_constraints.end(),
                            [](int32_t value) { return value != 0; }));
    if (tasks.empty()) {
      stats.device_name = device_name;
      return stats;
    }
    absl::Status status = Prepare();
    if (!status.ok()) return status;

    std::memset(stats_buffer.contents(), 0, kStatsCount * sizeof(int32_t));
    const auto start = std::chrono::steady_clock::now();

    if (options.probe_fusion == MetalSacProbeFusion::kBounded) {
      status = RunBounded(&stats);
    } else {
      status = RunUnfused(&stats);
    }
    if (!status.ok()) return status;

    const auto end = std::chrono::steady_clock::now();
    stats.elapsed_ms =
        std::chrono::duration<double, std::milli>(end - start).count();
    FinishResults(&stats);
    return stats;
  }

  const cpim::model::DeviceModelLayout& layout;
  std::vector<uint32_t> snapshot_bit_dom;
  std::vector<int32_t> snapshot_domain_sizes;
  std::vector<MetalSacProbeTask> tasks;
  MetalBatchProbeOptions options;
  MetalRuntime runtime;
  std::string device_name;
  MetalPipeline init_pipeline;
  MetalPipeline revise_pipeline;
  MetalPipeline frontier_pipeline;
  MetalPipeline clear_active_pipeline;
  MetalPipeline frontier_fused_pipeline;
  MetalPipeline mark_unknown_pipeline;
  MetalBuffer snapshot_bit_dom_buffer;
  MetalBuffer snapshot_domain_sizes_buffer;
  MetalBuffer bit_sup_words_buffer;
  MetalBuffer scopes_buffer;
  MetalBuffer sub_offsets_buffer;
  MetalBuffer sub_entries_buffer;
  MetalBuffer allowed_constraints_buffer;
  MetalBuffer tasks_buffer;
  MetalBuffer world_bit_dom_buffer;
  MetalBuffer world_domain_sizes_buffer;
  MetalBuffer current_frontier_buffer;
  MetalBuffer next_frontier_buffer;
  MetalBuffer statuses_buffer;
  MetalBuffer dwo_debug_buffer;
  MetalBuffer active_counts_buffer;
  MetalBuffer stats_buffer;
  std::vector<MetalSacProbeStatus> statuses_result;
  std::vector<uint32_t> world_bit_dom_result;
  std::vector<int32_t> world_domain_sizes_result;
  std::vector<int32_t> dwo_debug_words_result;
  int allowed_constraints_count = 0;
};

MetalBatchProbeRunner::MetalBatchProbeRunner(
    const cpim::model::DeviceModelLayout& layout,
    const std::vector<uint32_t>& snapshot_bit_dom,
    const std::vector<int32_t>& snapshot_domain_sizes,
    std::vector<MetalSacProbeTask> tasks,
    MetalBatchProbeOptions options)
    : impl_(std::make_unique<Impl>(
          layout, snapshot_bit_dom, snapshot_domain_sizes, std::move(tasks),
          std::move(options))) {}

MetalBatchProbeRunner::~MetalBatchProbeRunner() = default;
MetalBatchProbeRunner::MetalBatchProbeRunner(MetalBatchProbeRunner&&) noexcept =
    default;
MetalBatchProbeRunner& MetalBatchProbeRunner::operator=(
    MetalBatchProbeRunner&&) noexcept = default;

absl::StatusOr<MetalBatchProbeStats> MetalBatchProbeRunner::Run() {
  return impl_->Run();
}

const std::vector<MetalSacProbeStatus>& MetalBatchProbeRunner::statuses() const {
  return impl_->statuses_result;
}

const std::vector<uint32_t>& MetalBatchProbeRunner::world_bit_dom() const {
  return impl_->world_bit_dom_result;
}

const std::vector<int32_t>& MetalBatchProbeRunner::world_domain_sizes() const {
  return impl_->world_domain_sizes_result;
}

const std::vector<int32_t>& MetalBatchProbeRunner::dwo_debug_words() const {
  return impl_->dwo_debug_words_result;
}

}  // namespace cpim::solver::metal
