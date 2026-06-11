#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <unordered_set>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "glog/logging.h"

#include "model/device_layout.h"
#include "model/intermediate_model.h"
#include "model/model_normalizer.h"
#include "model/xcsp_parser.h"
#include "solver/metal/metal_gac_solver.h"
#include "solver/metal/metal_sac_batch_probe.h"

ABSL_FLAG(std::string, input, "", "Path to XCSP2/3 XML instance");
ABSL_FLAG(std::string, metallib, "",
          "Path to metal_gac.metallib; empty means next to this executable");
ABSL_FLAG(int, runs, 10, "Measured Metal SAC batch probe runs");
ABSL_FLAG(int, warmup, 2, "Warmup Metal SAC batch probe runs");
ABSL_FLAG(int, probe_limit, 0,
          "Max singleton probes per run; 0 means all remaining values");
ABSL_FLAG(std::string, activation_mode, "neighbor",
          "Probe activation mode: neighbor or full");
ABSL_FLAG(std::string, sac_mode, "batch_probe",
          "SAC benchmark mode: batch_probe, nsacq, sacq_adj, or sacq_full");
ABSL_FLAG(int, max_probe_rounds, 10000,
          "Maximum batch probe rounds before UNKNOWN");
ABSL_FLAG(int, max_sac_batches, 10000,
          "Maximum host-side SAC batches before queue budget is exceeded");
ABSL_FLAG(int, outer_queue_budget, 0,
          "Maximum SAC queue pops; 0 means unbounded");
ABSL_FLAG(bool, verify, true, "Verify measured probe statuses against CPU reference");
ABSL_FLAG(int, verify_probe_limit, 0,
          "Max probes to verify; 0 means all probes");
ABSL_FLAG(bool, dwo_forensics, true,
          "Collect rejected-DWO forensic counters when verifying NSACQ");
ABSL_FLAG(std::string, probe_fusion, "none",
          "Probe command-buffer fusion mode: none or bounded");
ABSL_FLAG(int, fusion_rounds, 4,
          "Bounded fusion rounds per command buffer segment");
ABSL_FLAG(std::string, csv, "", "Optional per-run CSV output path");

namespace {

constexpr int kBitsPerWord = 32;

absl::StatusOr<cpim::model::IntermediateModel> LoadNormalizedModel(
    const std::string& input) {
  auto parser = cpim::model::XcspParser::Create(cpim::model::ParserType::kLibXml2);
  if (!parser) {
    return absl::InternalError("Failed to create XCSP parser");
  }
  auto model_or = parser->Parse(input);
  if (!model_or.ok()) {
    auto manifest_or = parser->LoadBenchManifest(input);
    if (manifest_or.ok()) {
      const cpim::model::BenchFileInfo* first_file = nullptr;
      for (const auto& entry : *manifest_or) {
        if (!entry.files.empty()) {
          first_file = &entry.files.front();
          break;
        }
      }
      if (first_file == nullptr) {
        return absl::NotFoundError("Bench manifest contains no XCSP files");
      }
      std::cout << "[MetalSAC] Manifest input resolved to "
                << first_file->path.string() << "\n";
      model_or = parser->Parse(first_file->path);
    }
  }
  if (!model_or.ok()) return model_or.status();
  cpim::model::ModelNormalizer normalizer;
  auto normalized_or = normalizer.Normalize(*model_or);
  if (!normalized_or.ok()) return normalized_or.status();
  return std::move(*normalized_or);
}

std::string DefaultMetallibPath(const char* argv0) {
  std::filesystem::path exe_path = std::filesystem::absolute(argv0);
  return (exe_path.parent_path() / "metal_gac.metallib").string();
}

absl::StatusOr<cpim::solver::metal::MetalSacActivationMode>
ParseActivationMode(const std::string& value) {
  if (value == "neighbor") {
    return cpim::solver::metal::MetalSacActivationMode::kNeighbor;
  }
  if (value == "full") {
    return cpim::solver::metal::MetalSacActivationMode::kFull;
  }
  return absl::InvalidArgumentError(
      "activation_mode must be 'neighbor' or 'full'");
}

absl::StatusOr<cpim::solver::metal::MetalSacMode> ParseSacMode(
    const std::string& value) {
  if (value == "batch_probe") {
    return cpim::solver::metal::MetalSacMode::kBatchProbe;
  }
  if (value == "nsacq") {
    return cpim::solver::metal::MetalSacMode::kNsacq;
  }
  if (value == "sacq_adj") {
    return cpim::solver::metal::MetalSacMode::kSacqAdj;
  }
  if (value == "sacq_full") {
    return cpim::solver::metal::MetalSacMode::kSacqFull;
  }
  return absl::InvalidArgumentError(
      "sac_mode must be batch_probe, nsacq, sacq_adj, or sacq_full");
}

absl::StatusOr<cpim::solver::metal::MetalSacProbeFusion> ParseProbeFusion(
    const std::string& value) {
  if (value == "none") {
    return cpim::solver::metal::MetalSacProbeFusion::kNone;
  }
  if (value == "bounded") {
    return cpim::solver::metal::MetalSacProbeFusion::kBounded;
  }
  return absl::InvalidArgumentError(
      "probe_fusion must be none or bounded");
}

std::string ActivationModeName(
    cpim::solver::metal::MetalSacActivationMode mode) {
  return mode == cpim::solver::metal::MetalSacActivationMode::kFull
             ? "full"
             : "neighbor";
}

std::string SacModeName(cpim::solver::metal::MetalSacMode mode) {
  switch (mode) {
    case cpim::solver::metal::MetalSacMode::kNsacq:
      return "nsacq";
    case cpim::solver::metal::MetalSacMode::kSacqAdj:
      return "sacq_adj";
    case cpim::solver::metal::MetalSacMode::kSacqFull:
      return "sacq_full";
    case cpim::solver::metal::MetalSacMode::kBatchProbe:
    default:
      return "batch_probe";
  }
}

std::string ProbeFusionName(cpim::solver::metal::MetalSacProbeFusion fusion) {
  return fusion == cpim::solver::metal::MetalSacProbeFusion::kBounded
             ? "bounded"
             : "none";
}

std::string ProbeStatusName(cpim::solver::metal::MetalSacProbeStatus status) {
  switch (status) {
    case cpim::solver::metal::MetalSacProbeStatus::kDwo:
      return "dwo";
    case cpim::solver::metal::MetalSacProbeStatus::kUnknown:
      return "unknown";
    case cpim::solver::metal::MetalSacProbeStatus::kOk:
    default:
      return "ok";
  }
}

bool BitTest(const std::vector<uint32_t>& words, int base, int value) {
  const int word = value / kBitsPerWord;
  const int bit = value % kBitsPerWord;
  return ((words[base + word] >> bit) & 1u) != 0u;
}

void BitClear(std::vector<uint32_t>* words, int base, int value) {
  const int word = value / kBitsPerWord;
  const int bit = value % kBitsPerWord;
  (*words)[base + word] &= ~(1u << bit);
}

bool BitClearIfPresent(std::vector<uint32_t>* words, int base, int value) {
  if (!BitTest(*words, base, value)) return false;
  BitClear(words, base, value);
  return true;
}

uint64_t HashCombine(uint64_t hash, uint64_t value) {
  hash ^= value;
  hash *= 1099511628211ull;
  return hash;
}

uint64_t HashWords(const std::vector<uint32_t>& words) {
  uint64_t hash = 1469598103934665603ull;
  for (uint32_t value : words) {
    hash = HashCombine(hash, value);
  }
  return hash;
}

uint64_t HashInts(const std::vector<int32_t>& values) {
  uint64_t hash = 1469598103934665603ull;
  for (int32_t value : values) {
    hash = HashCombine(hash, static_cast<uint32_t>(value));
  }
  return hash;
}

int PopcountDomain(const cpim::model::DeviceModelLayout& layout,
                   const std::vector<uint32_t>& words,
                   int base) {
  int count = 0;
  for (int value = 0; value < layout.max_dom_size; ++value) {
    if (BitTest(words, base, value)) ++count;
  }
  return count;
}

void EnqueueSubscriptions(const cpim::model::DeviceModelLayout& layout,
                          int var,
                          const std::vector<int32_t>* allowed_constraints,
                          std::vector<int>* con_pre) {
  if (var < 0 || var + 1 >= static_cast<int>(layout.subscriptions.offsets.size())) {
    return;
  }
  const int begin = layout.subscriptions.offsets[var];
  const int end = layout.subscriptions.offsets[var + 1];
  for (int i = begin; i < end; ++i) {
    const int cid = static_cast<int>(layout.subscriptions.entries[i].z);
    if (cid >= 0 && cid < layout.num_constraints) {
      if (allowed_constraints != nullptr &&
          !allowed_constraints->empty() &&
          (*allowed_constraints)[cid] == 0) {
        continue;
      }
      (*con_pre)[cid] = 1;
    }
  }
}

int CpuReviseDirection(const cpim::model::DeviceModelLayout& layout,
                       int cid,
                       int dir,
                       std::vector<uint32_t>* bit_dom,
                       std::vector<int32_t>* domain_sizes,
                       const std::vector<int32_t>* allowed_constraints,
                       std::vector<int>* con_pre,
                       bool* dwo) {
  const cpim::model::DeviceInt2 scope = layout.constraint_scopes[cid];
  const int target = dir == 0 ? scope.x : scope.y;
  const int source = dir == 0 ? scope.y : scope.x;
  if (target < 0 || target >= layout.num_vars ||
      source < 0 || source >= layout.num_vars) {
    return 0;
  }
  const int target_base = target * layout.bit_words;
  const int source_base = source * layout.bit_words;
  int deletions = 0;
  for (int value = 0; value < layout.max_dom_size; ++value) {
    if (!BitTest(*bit_dom, target_base, value)) continue;
    const int sup_base =
        ((cid * 2 + dir) * layout.max_dom_size + value) * layout.bit_words;
    bool supported = false;
    for (int word = 0; word < layout.bit_words; ++word) {
      if ((layout.bit_sup_words[sup_base + word] &
           (*bit_dom)[source_base + word]) != 0u) {
        supported = true;
        break;
      }
    }
    if (!supported) {
      BitClear(bit_dom, target_base, value);
      --(*domain_sizes)[target];
      ++deletions;
      if ((*domain_sizes)[target] == 0) {
        *dwo = true;
        return deletions;
      }
    }
  }
  if (deletions > 0) {
    EnqueueSubscriptions(layout, target, allowed_constraints, con_pre);
  }
  return deletions;
}

cpim::solver::metal::MetalSacProbeStatus CpuProbeStatus(
    const cpim::model::DeviceModelLayout& layout,
    const std::vector<uint32_t>& snapshot_bit_dom,
    const std::vector<int32_t>& snapshot_domain_sizes,
    const cpim::solver::metal::MetalSacProbeTask& task,
    cpim::solver::metal::MetalSacActivationMode activation_mode,
    const std::vector<int32_t>* allowed_constraints,
    int max_rounds) {
  if (task.var_id < 0 || task.var_id >= layout.num_vars ||
      task.value < 0 || task.value >= layout.max_dom_size) {
    return cpim::solver::metal::MetalSacProbeStatus::kUnknown;
  }
  std::vector<uint32_t> bit_dom = snapshot_bit_dom;
  std::vector<int32_t> domain_sizes = snapshot_domain_sizes;
  const int var_base = task.var_id * layout.bit_words;
  if (!BitTest(bit_dom, var_base, task.value)) {
    return cpim::solver::metal::MetalSacProbeStatus::kDwo;
  }
  for (int word = 0; word < layout.bit_words; ++word) {
    bit_dom[var_base + word] = 0u;
  }
  bit_dom[var_base + task.value / kBitsPerWord] =
      1u << static_cast<uint32_t>(task.value % kBitsPerWord);
  domain_sizes[task.var_id] = 1;

  std::vector<int> con_pre(layout.num_constraints, 0);
  if (activation_mode == cpim::solver::metal::MetalSacActivationMode::kFull) {
    if (allowed_constraints != nullptr && !allowed_constraints->empty()) {
      for (int cid = 0; cid < layout.num_constraints; ++cid) {
        con_pre[cid] = (*allowed_constraints)[cid] != 0 ? 1 : 0;
      }
    } else {
      std::fill(con_pre.begin(), con_pre.end(), 1);
    }
  } else {
    EnqueueSubscriptions(layout, task.var_id, allowed_constraints, &con_pre);
  }

  int rounds = 0;
  while (std::any_of(con_pre.begin(), con_pre.end(), [](int v) { return v != 0; })) {
    if (rounds >= max_rounds) {
      return cpim::solver::metal::MetalSacProbeStatus::kUnknown;
    }
    std::vector<int> active;
    for (int cid = 0; cid < layout.num_constraints; ++cid) {
      if (con_pre[cid] != 0) active.push_back(cid);
      con_pre[cid] = 0;
    }
    bool dwo = false;
    for (int cid : active) {
      CpuReviseDirection(layout, cid, 0, &bit_dom, &domain_sizes,
                         allowed_constraints, &con_pre, &dwo);
      if (dwo) return cpim::solver::metal::MetalSacProbeStatus::kDwo;
      CpuReviseDirection(layout, cid, 1, &bit_dom, &domain_sizes,
                         allowed_constraints, &con_pre, &dwo);
      if (dwo) return cpim::solver::metal::MetalSacProbeStatus::kDwo;
    }
    ++rounds;
  }
  return cpim::solver::metal::MetalSacProbeStatus::kOk;
}

std::vector<cpim::solver::metal::MetalSacProbeTask> BuildProbeTasks(
    const cpim::model::DeviceModelLayout& layout,
    const std::vector<uint32_t>& snapshot_bit_dom,
    int probe_limit) {
  std::vector<cpim::solver::metal::MetalSacProbeTask> tasks;
  const int limit = std::max(0, probe_limit);
  int task_id = 0;
  for (int var = 0; var < layout.num_vars; ++var) {
    const int base = var * layout.bit_words;
    for (int value = 0; value < layout.max_dom_size; ++value) {
      if (!BitTest(snapshot_bit_dom, base, value)) continue;
      tasks.push_back({var, value, task_id++});
      if (limit > 0 && static_cast<int>(tasks.size()) >= limit) {
        return tasks;
      }
    }
  }
  return tasks;
}

int CountDomainValues(const std::vector<int32_t>& domain_sizes) {
  return std::accumulate(domain_sizes.begin(), domain_sizes.end(), 0);
}

cpim::model::DeviceModelLayout LayoutWithSnapshot(
    const cpim::model::DeviceModelLayout& layout,
    const std::vector<uint32_t>& bit_dom,
    const std::vector<int32_t>& domain_sizes) {
  cpim::model::DeviceModelLayout seeded = layout;
  seeded.bit_dom = bit_dom;
  seeded.domain_sizes = domain_sizes;
  return seeded;
}

absl::StatusOr<cpim::solver::metal::MetalGacStats> RunStableGac(
    const cpim::model::DeviceModelLayout& layout,
    const std::string& metallib,
    std::vector<uint32_t>* bit_dom,
    std::vector<int32_t>* domain_sizes) {
  cpim::solver::metal::MetalGacOptions gac_options;
  gac_options.metallib_path = metallib;
  gac_options.runner_mode = cpim::solver::metal::MetalRunnerMode::kPrepared;
  gac_options.frontier_mode = cpim::solver::metal::MetalFrontierMode::kFlags;
  gac_options.kernel_variant = cpim::solver::metal::MetalKernelVariant::kScalar;
  gac_options.bitsup_layout = cpim::solver::metal::MetalBitSupLayout::kPair;
  cpim::solver::metal::MetalGacSolver gac(layout, gac_options);
  auto stats_or = gac.Run();
  if (!stats_or.ok()) return stats_or.status();
  *bit_dom = gac.bit_dom();
  *domain_sizes = gac.domain_sizes();
  return *stats_or;
}

std::vector<int> ChangedVars(const cpim::model::DeviceModelLayout& layout,
                             const std::vector<uint32_t>& before,
                             const std::vector<uint32_t>& after) {
  std::vector<int> vars;
  for (int var = 0; var < layout.num_vars; ++var) {
    const int base = var * layout.bit_words;
    bool changed = false;
    for (int word = 0; word < layout.bit_words; ++word) {
      if (before[base + word] != after[base + word]) {
        changed = true;
        break;
      }
    }
    if (changed) vars.push_back(var);
  }
  return vars;
}

std::vector<int> AdjacentVars(const cpim::model::DeviceModelLayout& layout,
                              const std::vector<int>& seed_vars) {
  std::vector<int> out;
  std::vector<char> seen(static_cast<size_t>(layout.num_vars), 0);
  auto add = [&](int var) {
    if (var < 0 || var >= layout.num_vars || seen[var]) return;
    seen[var] = 1;
    out.push_back(var);
  };
  for (int var : seed_vars) {
    add(var);
    if (var < 0 ||
        var + 1 >= static_cast<int>(layout.subscriptions.offsets.size())) {
      continue;
    }
    const int begin = layout.subscriptions.offsets[var];
    const int end = layout.subscriptions.offsets[var + 1];
    for (int i = begin; i < end; ++i) {
      const auto entry = layout.subscriptions.entries[i];
      add(static_cast<int>(entry.x));
      add(static_cast<int>(entry.y));
    }
  }
  return out;
}

std::vector<int32_t> ConstraintMaskForVars(
    const cpim::model::DeviceModelLayout& layout,
    const std::vector<int>& vars,
    cpim::solver::metal::MetalSacMode mode) {
  std::vector<int32_t> mask(static_cast<size_t>(layout.num_constraints), 0);
  if (mode == cpim::solver::metal::MetalSacMode::kBatchProbe ||
      mode == cpim::solver::metal::MetalSacMode::kNsacq ||
      mode == cpim::solver::metal::MetalSacMode::kSacqFull) {
    return {};
  }
  for (int var : vars) {
    if (var < 0 ||
        var + 1 >= static_cast<int>(layout.subscriptions.offsets.size())) {
      continue;
    }
    const int begin = layout.subscriptions.offsets[var];
    const int end = layout.subscriptions.offsets[var + 1];
    for (int i = begin; i < end; ++i) {
      const int cid = static_cast<int>(layout.subscriptions.entries[i].z);
      if (cid >= 0 && cid < layout.num_constraints) mask[cid] = 1;
    }
  }
  return mask;
}

void EnqueueRemainingValues(const cpim::model::DeviceModelLayout& layout,
                            const std::vector<uint32_t>& bit_dom,
                            const std::vector<int>& vars,
                            std::deque<cpim::solver::metal::MetalSacProbeTask>* queue,
                            std::vector<char>* queued,
                            int* queue_push_count,
                            int* next_task_id) {
  for (int var : vars) {
    if (var < 0 || var >= layout.num_vars) continue;
    const int base = var * layout.bit_words;
    for (int value = 0; value < layout.max_dom_size; ++value) {
      const int key = var * layout.max_dom_size + value;
      if ((*queued)[key] || !BitTest(bit_dom, base, value)) continue;
      (*queued)[key] = 1;
      queue->push_back({var, value, (*next_task_id)++});
      ++(*queue_push_count);
    }
  }
}

std::vector<int> AllVars(const cpim::model::DeviceModelLayout& layout) {
  std::vector<int> vars(static_cast<size_t>(layout.num_vars));
  std::iota(vars.begin(), vars.end(), 0);
  return vars;
}

struct RunRecord {
  int run = 0;
  cpim::solver::metal::MetalGacStats gac_stats;
  cpim::solver::metal::MetalBatchProbeStats probe_stats;
  int nsacq_batches = 0;
  int nsacq_gac_runs = 0;
  int nsacq_gac_deletions = 0;
  int nsacq_deleted_values = 0;
  int nsacq_raw_dwo_count = 0;
  int nsacq_confirmed_dwo_count = 0;
  int nsacq_rejected_dwo_count = 0;
  double nsacq_raw_dwo_precision = 0.0;
  int nsacq_queue_push_count = 0;
  int nsacq_queue_pop_count = 0;
  int nsacq_final_queue_size = 0;
  int nsacq_final_domain_values = 0;
  double nsacq_elapsed_ms = 0.0;
  bool nsacq_queue_budget_exceeded = false;
  int dwo_forensic_checked = 0;
  int dwo_domain_size_popcount_mismatch_count = 0;
  int dwo_rejected_empty_domain_count = 0;
  int dwo_rejected_nonempty_domain_count = 0;
  int dwo_first_rejected_var = -1;
  int dwo_first_rejected_value = -1;
  int dwo_first_rejected_empty_var = -1;
  int dwo_first_rejected_empty_popcount = -1;
  int dwo_first_rejected_empty_domain_size = -1;
  int dwo_first_rejected_status_var = -1;
  int dwo_first_rejected_status_cid = -1;
  int dwo_first_rejected_status_dir = -1;
  int dwo_first_rejected_status_old_size = -1;
  int dwo_first_rejected_status_deletion_count = -1;
  int dwo_first_rejected_status_round = -1;
  uint64_t dwo_first_rejected_snapshot_hash = 0;
  uint64_t dwo_first_rejected_allowed_hash = 0;
  int verify_checked = 0;
  int verify_mismatches = 0;
  bool verified = false;
};

void AnalyzeRejectedDwo(
    const cpim::model::DeviceModelLayout& layout,
    const std::vector<cpim::solver::metal::MetalSacProbeTask>& tasks,
    size_t task_index,
    const std::vector<uint32_t>& world_bit_dom,
    const std::vector<int32_t>& world_domain_sizes,
    const std::vector<int32_t>& dwo_debug_words,
    const std::vector<uint32_t>& snapshot_bit_dom,
    const std::vector<int32_t>& allowed_constraints,
    RunRecord* record) {
  const size_t world_count = tasks.size();
  const size_t bit_dom_words_per_world =
      static_cast<size_t>(layout.num_vars) * layout.bit_words;
  const size_t domain_sizes_per_world = static_cast<size_t>(layout.num_vars);
  if (task_index >= world_count ||
      world_bit_dom.size() < world_count * bit_dom_words_per_world ||
      world_domain_sizes.size() < world_count * domain_sizes_per_world) {
    return;
  }
  ++record->dwo_forensic_checked;
  const size_t bit_dom_base = task_index * bit_dom_words_per_world;
  const size_t size_base = task_index * domain_sizes_per_world;
  bool has_empty_domain = false;
  int first_empty_var = -1;
  int first_empty_popcount = -1;
  int first_empty_size = -1;
  for (int var = 0; var < layout.num_vars; ++var) {
    const int word_base =
        static_cast<int>(bit_dom_base) + var * layout.bit_words;
    const int pop = PopcountDomain(layout, world_bit_dom, word_base);
    const int size = world_domain_sizes[size_base + var];
    if (pop != size) {
      ++record->dwo_domain_size_popcount_mismatch_count;
    }
    if (!has_empty_domain && pop == 0) {
      has_empty_domain = true;
      first_empty_var = var;
      first_empty_popcount = pop;
      first_empty_size = size;
    }
  }
  if (has_empty_domain) {
    ++record->dwo_rejected_empty_domain_count;
  } else {
    ++record->dwo_rejected_nonempty_domain_count;
  }
  if (record->dwo_first_rejected_var < 0) {
    record->dwo_first_rejected_var = tasks[task_index].var_id;
    record->dwo_first_rejected_value = tasks[task_index].value;
    record->dwo_first_rejected_empty_var = first_empty_var;
    record->dwo_first_rejected_empty_popcount = first_empty_popcount;
    record->dwo_first_rejected_empty_domain_size = first_empty_size;
    constexpr size_t kDwoDebugWordsPerWorld = 6;
    const size_t debug_base = task_index * kDwoDebugWordsPerWorld;
    if (dwo_debug_words.size() >= debug_base + kDwoDebugWordsPerWorld) {
      record->dwo_first_rejected_status_var = dwo_debug_words[debug_base + 0];
      record->dwo_first_rejected_status_cid = dwo_debug_words[debug_base + 1];
      record->dwo_first_rejected_status_dir = dwo_debug_words[debug_base + 2];
      record->dwo_first_rejected_status_old_size =
          dwo_debug_words[debug_base + 3];
      record->dwo_first_rejected_status_deletion_count =
          dwo_debug_words[debug_base + 4];
      record->dwo_first_rejected_status_round = dwo_debug_words[debug_base + 5];
    }
    record->dwo_first_rejected_snapshot_hash = HashWords(snapshot_bit_dom);
    record->dwo_first_rejected_allowed_hash = HashInts(allowed_constraints);
  }
}

void WriteCsv(const std::string& path,
              const std::string& input,
              const cpim::model::DeviceModelLayout& layout,
              cpim::solver::metal::MetalSacMode sac_mode,
              cpim::solver::metal::MetalSacActivationMode activation_mode,
              cpim::solver::metal::MetalSacProbeFusion probe_fusion,
              int probe_limit,
              int max_probe_rounds,
              int fusion_rounds,
              int max_sac_batches,
              int outer_queue_budget,
              const std::vector<RunRecord>& records) {
  if (path.empty()) return;
  std::filesystem::path csv_path(path);
  if (csv_path.has_parent_path()) {
    std::filesystem::create_directories(csv_path.parent_path());
  }
  std::ofstream out(path);
  out << "input,run,device,num_vars,num_constraints,max_dom_size,bit_words,"
      << "sac_mode,activation_mode,probe_fusion,probe_limit,"
      << "max_probe_rounds,fusion_rounds,"
      << "max_sac_batches,outer_queue_budget,"
      << "gac_solve_ms,gac_iterations,gac_deletions,gac_inconsistent,"
      << "probes,ok_count,dwo_count,unknown_count,rounds,dispatch_count,"
      << "command_buffer_count,"
      << "active_frontier_total,allowed_constraints_count,budget_exceeded,"
      << "elapsed_ms,dispatch_ms,"
      << "dispatch_encode_ms,dispatch_wait_ms,dispatch_non_kernel_ms,kernel_ms,"
      << "probes_per_sec,dispatch_per_probe,command_buffer_per_probe,"
      << "non_kernel_per_probe,"
      << "stats_fusion_rounds,fused_rounds_encoded,fused_rounds_wasted,"
      << "nsacq_batches,nsacq_gac_runs,nsacq_gac_deletions,"
      << "nsacq_deleted_values,nsacq_raw_dwo_count,"
      << "nsacq_confirmed_dwo_count,nsacq_rejected_dwo_count,"
      << "nsacq_raw_dwo_precision,"
      << "nsacq_queue_push_count,nsacq_queue_pop_count,"
      << "nsacq_final_queue_size,nsacq_final_domain_values,nsacq_elapsed_ms,"
      << "nsacq_queue_budget_exceeded,"
      << "dwo_forensic_checked,dwo_domain_size_popcount_mismatch_count,"
      << "dwo_rejected_empty_domain_count,dwo_rejected_nonempty_domain_count,"
      << "dwo_first_rejected_var,dwo_first_rejected_value,"
      << "dwo_first_rejected_empty_var,dwo_first_rejected_empty_popcount,"
      << "dwo_first_rejected_empty_domain_size,"
      << "dwo_first_rejected_status_var,dwo_first_rejected_status_cid,"
      << "dwo_first_rejected_status_dir,dwo_first_rejected_status_old_size,"
      << "dwo_first_rejected_status_deletion_count,"
      << "dwo_first_rejected_status_round,"
      << "dwo_first_rejected_snapshot_hash,dwo_first_rejected_allowed_hash,"
      << "gpu_timing_available,verify_checked,verify_mismatches,verified\n";
  for (const RunRecord& record : records) {
    const auto& s = record.probe_stats;
    const auto& g = record.gac_stats;
    out << input << ","
        << record.run << ","
        << s.device_name << ","
        << layout.num_vars << ","
        << layout.num_constraints << ","
        << layout.max_dom_size << ","
        << layout.bit_words << ","
        << SacModeName(sac_mode) << ","
        << ActivationModeName(activation_mode) << ","
        << ProbeFusionName(probe_fusion) << ","
        << probe_limit << ","
        << max_probe_rounds << ","
        << fusion_rounds << ","
        << max_sac_batches << ","
        << outer_queue_budget << ","
        << g.solve_ms << ","
        << g.iterations << ","
        << g.deletions << ","
        << (g.inconsistent ? "true" : "false") << ","
        << s.probes << ","
        << s.ok_count << ","
        << s.dwo_count << ","
        << s.unknown_count << ","
        << s.rounds << ","
        << s.dispatch_count << ","
        << s.command_buffer_count << ","
        << s.active_frontier_total << ","
        << s.allowed_constraints_count << ","
        << (s.budget_exceeded ? "true" : "false") << ","
        << s.elapsed_ms << ","
        << s.dispatch_ms << ","
        << s.dispatch_encode_ms << ","
        << s.dispatch_wait_ms << ","
        << s.dispatch_non_kernel_ms << ","
        << s.kernel_ms << ","
        << s.probes_per_sec << ","
        << s.dispatch_per_probe << ","
        << s.command_buffer_per_probe << ","
        << s.non_kernel_per_probe << ","
        << s.fusion_rounds << ","
        << s.fused_rounds_encoded << ","
        << s.fused_rounds_wasted << ","
        << record.nsacq_batches << ","
        << record.nsacq_gac_runs << ","
        << record.nsacq_gac_deletions << ","
        << record.nsacq_deleted_values << ","
        << record.nsacq_raw_dwo_count << ","
        << record.nsacq_confirmed_dwo_count << ","
        << record.nsacq_rejected_dwo_count << ","
        << record.nsacq_raw_dwo_precision << ","
        << record.nsacq_queue_push_count << ","
        << record.nsacq_queue_pop_count << ","
        << record.nsacq_final_queue_size << ","
        << record.nsacq_final_domain_values << ","
        << record.nsacq_elapsed_ms << ","
        << (record.nsacq_queue_budget_exceeded ? "true" : "false") << ","
        << record.dwo_forensic_checked << ","
        << record.dwo_domain_size_popcount_mismatch_count << ","
        << record.dwo_rejected_empty_domain_count << ","
        << record.dwo_rejected_nonempty_domain_count << ","
        << record.dwo_first_rejected_var << ","
        << record.dwo_first_rejected_value << ","
        << record.dwo_first_rejected_empty_var << ","
        << record.dwo_first_rejected_empty_popcount << ","
        << record.dwo_first_rejected_empty_domain_size << ","
        << record.dwo_first_rejected_status_var << ","
        << record.dwo_first_rejected_status_cid << ","
        << record.dwo_first_rejected_status_dir << ","
        << record.dwo_first_rejected_status_old_size << ","
        << record.dwo_first_rejected_status_deletion_count << ","
        << record.dwo_first_rejected_status_round << ","
        << record.dwo_first_rejected_snapshot_hash << ","
        << record.dwo_first_rejected_allowed_hash << ","
        << (s.gpu_timing_available ? "true" : "false") << ","
        << record.verify_checked << ","
        << record.verify_mismatches << ","
        << (record.verified ? "true" : "false") << "\n";
  }
}

int VerifyProbeStatuses(
    const cpim::model::DeviceModelLayout& layout,
    const std::vector<uint32_t>& bit_dom,
    const std::vector<int32_t>& domain_sizes,
    const std::vector<cpim::solver::metal::MetalSacProbeTask>& tasks,
    const std::vector<cpim::solver::metal::MetalSacProbeStatus>& statuses,
    cpim::solver::metal::MetalSacActivationMode activation_mode,
    const std::vector<int32_t>* allowed_constraints,
    int max_probe_rounds,
    int verify_limit,
    int* mismatches) {
  const int limit = verify_limit > 0
                        ? std::min<int>(verify_limit, tasks.size())
                        : static_cast<int>(tasks.size());
  for (int i = 0; i < limit; ++i) {
    const auto cpu_status = CpuProbeStatus(
        layout, bit_dom, domain_sizes, tasks[i], activation_mode,
        allowed_constraints, max_probe_rounds);
    if (statuses[i] != cpu_status) {
      ++(*mismatches);
      if (*mismatches <= 5) {
        std::cerr << "[MetalSAC] Verify mismatch task=" << i
                  << " var=" << tasks[i].var_id
                  << " value=" << tasks[i].value
                  << " cpu=" << ProbeStatusName(cpu_status)
                  << " metal=" << ProbeStatusName(statuses[i]) << "\n";
      }
    }
  }
  return limit;
}

}  // namespace

int main(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);
  const std::string input = absl::GetFlag(FLAGS_input);
  if (input.empty()) {
    std::cerr << "Usage: " << argv[0] << " --input=/path/to/instance.xml\n";
    return 2;
  }
  const std::string metallib = absl::GetFlag(FLAGS_metallib).empty()
                                   ? DefaultMetallibPath(argv[0])
                                   : absl::GetFlag(FLAGS_metallib);
  auto activation_or = ParseActivationMode(absl::GetFlag(FLAGS_activation_mode));
  if (!activation_or.ok()) {
    std::cerr << "[MetalSAC] " << activation_or.status() << "\n";
    return 2;
  }
  auto sac_mode_or = ParseSacMode(absl::GetFlag(FLAGS_sac_mode));
  if (!sac_mode_or.ok()) {
    std::cerr << "[MetalSAC] " << sac_mode_or.status() << "\n";
    return 2;
  }
  auto probe_fusion_or = ParseProbeFusion(absl::GetFlag(FLAGS_probe_fusion));
  if (!probe_fusion_or.ok()) {
    std::cerr << "[MetalSAC] " << probe_fusion_or.status() << "\n";
    return 2;
  }
  auto model_or = LoadNormalizedModel(input);
  if (!model_or.ok()) {
    std::cerr << "[MetalSAC] Parse/normalize failed: "
              << model_or.status() << "\n";
    return 1;
  }
  auto layout_or = cpim::model::BuildDeviceLayoutFromIntermediate(*model_or);
  if (!layout_or.ok()) {
    std::cerr << "[MetalSAC] Device layout build failed: "
              << layout_or.status() << "\n";
    return 1;
  }

  std::vector<RunRecord> records;
  const int total_runs =
      std::max(0, absl::GetFlag(FLAGS_warmup)) +
      std::max(0, absl::GetFlag(FLAGS_runs));
  for (int index = 0; index < total_runs; ++index) {
    const bool measured = index >= absl::GetFlag(FLAGS_warmup);
    const int run = index - absl::GetFlag(FLAGS_warmup);

    std::vector<uint32_t> current_bit_dom;
    std::vector<int32_t> current_domain_sizes;
    auto gac_stats_or =
        RunStableGac(*layout_or, metallib, &current_bit_dom,
                     &current_domain_sizes);
    if (!gac_stats_or.ok()) {
      std::cerr << "[MetalSAC] GAC snapshot run failed: "
                << gac_stats_or.status() << "\n";
      return 1;
    }

    RunRecord record;
    record.run = run;
    record.gac_stats = *gac_stats_or;

    if (*sac_mode_or == cpim::solver::metal::MetalSacMode::kBatchProbe) {
      std::vector<cpim::solver::metal::MetalSacProbeTask> tasks =
          BuildProbeTasks(*layout_or, current_bit_dom,
                          absl::GetFlag(FLAGS_probe_limit));

      cpim::solver::metal::MetalBatchProbeOptions probe_options;
      probe_options.metallib_path = metallib;
      probe_options.activation_mode = *activation_or;
      probe_options.budget.max_probe_rounds =
          absl::GetFlag(FLAGS_max_probe_rounds);
      probe_options.probe_fusion = *probe_fusion_or;
      probe_options.fusion_rounds = absl::GetFlag(FLAGS_fusion_rounds);
      cpim::solver::metal::MetalBatchProbeRunner runner(
          *layout_or, current_bit_dom, current_domain_sizes, tasks,
          probe_options);
      auto probe_stats_or = runner.Run();
      if (!probe_stats_or.ok()) {
        std::cerr << "[MetalSAC] Batch probe run failed: "
                  << probe_stats_or.status() << "\n";
        return 1;
      }
      record.probe_stats = *probe_stats_or;
      if (absl::GetFlag(FLAGS_verify) && measured) {
        record.verify_checked = VerifyProbeStatuses(
            *layout_or, current_bit_dom, current_domain_sizes, tasks,
            runner.statuses(), *activation_or, nullptr,
            absl::GetFlag(FLAGS_max_probe_rounds),
            absl::GetFlag(FLAGS_verify_probe_limit),
            &record.verify_mismatches);
        record.verified = record.verify_mismatches == 0;
        if (!record.verified) {
          std::cerr << "[MetalSAC] CPU/Metal probe status mismatch\n";
          return 1;
        }
      } else {
        record.verified = !absl::GetFlag(FLAGS_verify);
      }
    } else {
      const auto nsacq_start = std::chrono::steady_clock::now();
      std::deque<cpim::solver::metal::MetalSacProbeTask> queue;
      std::vector<char> queued(
          static_cast<size_t>(layout_or->num_vars * layout_or->max_dom_size), 0);
      int next_task_id = 0;
      EnqueueRemainingValues(*layout_or, current_bit_dom, AllVars(*layout_or),
                             &queue, &queued,
                             &record.nsacq_queue_push_count, &next_task_id);

      const int batch_limit =
          absl::GetFlag(FLAGS_probe_limit) > 0
              ? absl::GetFlag(FLAGS_probe_limit)
              : static_cast<int>(queue.size());
      const int max_batches = std::max(0, absl::GetFlag(FLAGS_max_sac_batches));
      const int outer_budget = std::max(0, absl::GetFlag(FLAGS_outer_queue_budget));
      while (!queue.empty() &&
             (max_batches == 0 || record.nsacq_batches < max_batches) &&
             (outer_budget == 0 ||
              record.nsacq_queue_pop_count < outer_budget)) {
        const int remaining_budget =
            outer_budget == 0
                ? batch_limit
                : std::min(batch_limit,
                           outer_budget - record.nsacq_queue_pop_count);
        if (remaining_budget <= 0) break;

        std::vector<cpim::solver::metal::MetalSacProbeTask> tasks;
        tasks.reserve(static_cast<size_t>(remaining_budget));
        std::vector<int> batch_vars;
        std::vector<char> batch_var_seen(static_cast<size_t>(layout_or->num_vars), 0);
        while (!queue.empty() &&
               static_cast<int>(tasks.size()) < remaining_budget) {
          auto task = queue.front();
          queue.pop_front();
          const int key = task.var_id * layout_or->max_dom_size + task.value;
          queued[key] = 0;
          ++record.nsacq_queue_pop_count;
          if (!BitTest(current_bit_dom, task.var_id * layout_or->bit_words,
                       task.value)) {
            continue;
          }
          tasks.push_back(task);
          if (task.var_id >= 0 && task.var_id < layout_or->num_vars &&
              !batch_var_seen[task.var_id]) {
            batch_var_seen[task.var_id] = 1;
            batch_vars.push_back(task.var_id);
          }
        }
        if (tasks.empty()) continue;

        std::vector<int> allowed_vars = batch_vars;
        if (*sac_mode_or == cpim::solver::metal::MetalSacMode::kSacqAdj) {
          allowed_vars = AdjacentVars(*layout_or, batch_vars);
        } else if (*sac_mode_or ==
                   cpim::solver::metal::MetalSacMode::kSacqFull) {
          allowed_vars = AllVars(*layout_or);
        }
        std::vector<int32_t> allowed_constraints =
            ConstraintMaskForVars(*layout_or, allowed_vars, *sac_mode_or);

        cpim::solver::metal::MetalBatchProbeOptions probe_options;
        probe_options.metallib_path = metallib;
        probe_options.activation_mode = *activation_or;
        probe_options.budget.max_probe_rounds =
            absl::GetFlag(FLAGS_max_probe_rounds);
        probe_options.allowed_constraints = allowed_constraints;
        probe_options.collect_world_domains =
            absl::GetFlag(FLAGS_dwo_forensics);
        probe_options.probe_fusion = *probe_fusion_or;
        probe_options.fusion_rounds = absl::GetFlag(FLAGS_fusion_rounds);
        cpim::solver::metal::MetalBatchProbeRunner runner(
            *layout_or, current_bit_dom, current_domain_sizes, tasks,
            probe_options);
        auto probe_stats_or = runner.Run();
        if (!probe_stats_or.ok()) {
          std::cerr << "[MetalSAC] NSACQ probe batch failed: "
                    << probe_stats_or.status() << "\n";
          return 1;
        }
        ++record.nsacq_batches;
        record.probe_stats.probes += probe_stats_or->probes;
        record.probe_stats.ok_count += probe_stats_or->ok_count;
        record.probe_stats.dwo_count += probe_stats_or->dwo_count;
        record.probe_stats.unknown_count += probe_stats_or->unknown_count;
        record.probe_stats.rounds += probe_stats_or->rounds;
        record.probe_stats.dispatch_count += probe_stats_or->dispatch_count;
        record.probe_stats.command_buffer_count +=
            probe_stats_or->command_buffer_count;
        record.probe_stats.active_frontier_total +=
            probe_stats_or->active_frontier_total;
        record.probe_stats.fusion_rounds = probe_stats_or->fusion_rounds;
        record.probe_stats.fused_rounds_encoded +=
            probe_stats_or->fused_rounds_encoded;
        record.probe_stats.fused_rounds_wasted +=
            probe_stats_or->fused_rounds_wasted;
        record.probe_stats.allowed_constraints_count +=
            probe_stats_or->allowed_constraints_count;
        record.probe_stats.budget_exceeded =
            record.probe_stats.budget_exceeded || probe_stats_or->budget_exceeded;
        record.probe_stats.elapsed_ms += probe_stats_or->elapsed_ms;
        record.probe_stats.dispatch_ms += probe_stats_or->dispatch_ms;
        record.probe_stats.dispatch_encode_ms +=
            probe_stats_or->dispatch_encode_ms;
        record.probe_stats.dispatch_wait_ms += probe_stats_or->dispatch_wait_ms;
        record.probe_stats.dispatch_non_kernel_ms +=
            probe_stats_or->dispatch_non_kernel_ms;
        record.probe_stats.kernel_ms += probe_stats_or->kernel_ms;
        record.probe_stats.gpu_timing_available =
            record.probe_stats.gpu_timing_available ||
            probe_stats_or->gpu_timing_available;
        record.probe_stats.device_name = probe_stats_or->device_name;

        std::vector<int> deleted_vars;
        const auto& statuses = runner.statuses();
        for (size_t i = 0; i < tasks.size(); ++i) {
          if (statuses[i] != cpim::solver::metal::MetalSacProbeStatus::kDwo) {
            continue;
          }
          if (absl::GetFlag(FLAGS_verify) && measured) {
            const auto cpu_status = CpuProbeStatus(
                *layout_or, current_bit_dom, current_domain_sizes, tasks[i],
                *activation_or, allowed_constraints.empty()
                                    ? nullptr
                                    : &allowed_constraints,
                absl::GetFlag(FLAGS_max_probe_rounds));
            ++record.verify_checked;
            if (cpu_status != cpim::solver::metal::MetalSacProbeStatus::kDwo) {
              ++record.verify_mismatches;
              ++record.nsacq_rejected_dwo_count;
              if (absl::GetFlag(FLAGS_dwo_forensics)) {
                AnalyzeRejectedDwo(
                    *layout_or, tasks, i, runner.world_bit_dom(),
                    runner.world_domain_sizes(), runner.dwo_debug_words(),
                    current_bit_dom, allowed_constraints, &record);
              }
              if (record.nsacq_rejected_dwo_count <= 5) {
                std::cerr << "[MetalSAC] Rejected unconfirmed DWO task="
                          << i << " var=" << tasks[i].var_id
                          << " value=" << tasks[i].value
                          << " cpu=" << ProbeStatusName(cpu_status)
                          << " metal=dwo\n";
              }
              continue;
            }
          }
          ++record.nsacq_confirmed_dwo_count;
          const int base = tasks[i].var_id * layout_or->bit_words;
          if (BitClearIfPresent(&current_bit_dom, base, tasks[i].value)) {
            --current_domain_sizes[tasks[i].var_id];
            ++record.nsacq_deleted_values;
            deleted_vars.push_back(tasks[i].var_id);
          }
        }
        if (deleted_vars.empty()) continue;

        const std::vector<uint32_t> before_gac = current_bit_dom;
        cpim::model::DeviceModelLayout seeded =
            LayoutWithSnapshot(*layout_or, current_bit_dom, current_domain_sizes);
        auto next_gac_or = RunStableGac(seeded, metallib, &current_bit_dom,
                                        &current_domain_sizes);
        if (!next_gac_or.ok()) {
          std::cerr << "[MetalSAC] NSACQ post-delete GAC failed: "
                    << next_gac_or.status() << "\n";
          return 1;
        }
        ++record.nsacq_gac_runs;
        record.nsacq_gac_deletions += next_gac_or->deletions;
        std::vector<int> changed_vars = ChangedVars(*layout_or, before_gac,
                                                    current_bit_dom);
        changed_vars.insert(changed_vars.end(), deleted_vars.begin(),
                            deleted_vars.end());
        std::vector<int> requeue_vars = changed_vars;
        if (*sac_mode_or == cpim::solver::metal::MetalSacMode::kSacqAdj) {
          requeue_vars = AdjacentVars(*layout_or, changed_vars);
        } else if (*sac_mode_or ==
                   cpim::solver::metal::MetalSacMode::kSacqFull) {
          requeue_vars = AllVars(*layout_or);
        }
        EnqueueRemainingValues(*layout_or, current_bit_dom, requeue_vars, &queue,
                               &queued, &record.nsacq_queue_push_count,
                               &next_task_id);
      }
      record.nsacq_final_queue_size = static_cast<int>(queue.size());
      record.nsacq_final_domain_values = CountDomainValues(current_domain_sizes);
      record.nsacq_queue_budget_exceeded = !queue.empty();
      const auto nsacq_end = std::chrono::steady_clock::now();
      record.nsacq_elapsed_ms =
          std::chrono::duration<double, std::milli>(
              nsacq_end - nsacq_start).count();
      if (record.probe_stats.probes > 0) {
        record.nsacq_raw_dwo_count = record.probe_stats.dwo_count;
        const int dwo_decisions =
            record.nsacq_confirmed_dwo_count + record.nsacq_rejected_dwo_count;
        if (dwo_decisions > 0) {
          record.nsacq_raw_dwo_precision =
              static_cast<double>(record.nsacq_confirmed_dwo_count) /
              static_cast<double>(dwo_decisions);
        }
        record.probe_stats.probes_per_sec =
            static_cast<double>(record.probe_stats.probes) /
            (record.probe_stats.elapsed_ms / 1000.0);
        record.probe_stats.dispatch_per_probe =
            static_cast<double>(record.probe_stats.dispatch_count) /
            record.probe_stats.probes;
        record.probe_stats.command_buffer_per_probe =
            static_cast<double>(record.probe_stats.command_buffer_count) /
            record.probe_stats.probes;
        record.probe_stats.non_kernel_per_probe =
            record.probe_stats.dispatch_non_kernel_ms /
            record.probe_stats.probes;
      }
      record.verified =
          !absl::GetFlag(FLAGS_verify) || record.nsacq_rejected_dwo_count == 0;
    }
    if (measured) {
      records.push_back(record);
      const auto& s = record.probe_stats;
      std::cout << "[MetalSAC] run=" << run
                << " sac_mode=" << SacModeName(*sac_mode_or)
                << " probes=" << s.probes
                << " ok=" << s.ok_count
                << " dwo=" << s.dwo_count
                << " unknown=" << s.unknown_count
                << " rounds=" << s.rounds
                << " dispatch_count=" << s.dispatch_count
                << " command_buffer_count=" << s.command_buffer_count
                << " nsacq_batches=" << record.nsacq_batches
                << " nsacq_deleted_values=" << record.nsacq_deleted_values
                << " nsacq_gac_runs=" << record.nsacq_gac_runs
                << " elapsed_ms=" << s.elapsed_ms
                << " nsacq_elapsed_ms=" << record.nsacq_elapsed_ms
                << " probes_per_sec=" << s.probes_per_sec
                << " dispatch_per_probe=" << s.dispatch_per_probe
                << " command_buffer_per_probe=" << s.command_buffer_per_probe
                << " non_kernel_per_probe=" << s.non_kernel_per_probe
                << " fused_rounds_encoded=" << s.fused_rounds_encoded
                << " fused_rounds_wasted=" << s.fused_rounds_wasted
                << " verified=" << (record.verified ? "true" : "false")
                << "\n";
    }
  }

  WriteCsv(absl::GetFlag(FLAGS_csv), input, *layout_or, *sac_mode_or,
           *activation_or, *probe_fusion_or,
           absl::GetFlag(FLAGS_probe_limit),
           absl::GetFlag(FLAGS_max_probe_rounds),
           absl::GetFlag(FLAGS_fusion_rounds),
           absl::GetFlag(FLAGS_max_sac_batches),
           absl::GetFlag(FLAGS_outer_queue_budget), records);
  return 0;
}
