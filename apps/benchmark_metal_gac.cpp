#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "glog/logging.h"

#include "model/device_layout.h"
#include "model/gac_cpu.h"
#include "model/intermediate_model.h"
#include "model/model_normalizer.h"
#include "model/xcsp_parser.h"
#include "solver/metal/metal_gac_solver.h"

ABSL_FLAG(std::string, input, "", "Path to XCSP2/3 XML instance");
ABSL_FLAG(std::string, metallib, "",
          "Path to metal_gac.metallib; empty means next to this executable");
ABSL_FLAG(int, runs, 10, "Measured Metal GAC runs");
ABSL_FLAG(int, warmup, 2, "Warmup Metal GAC runs");
ABSL_FLAG(int, max_iterations, 10000, "Maximum Metal host-dispatch iterations");
ABSL_FLAG(std::string, csv, "", "Optional per-run CSV output path");
ABSL_FLAG(bool, verify, true, "Verify every measured Metal run against CPU GAC");
ABSL_FLAG(bool, verbose, false, "Print per-iteration Metal GAC statistics");
ABSL_FLAG(std::string, runner_mode, "cold",
          "Metal GAC runner mode: cold or prepared");
ABSL_FLAG(std::string, readonly_storage, "shared",
          "Readonly Metal buffer storage: shared or private");
ABSL_FLAG(std::string, frontier_mode, "flags",
          "Metal frontier dispatch mode: flags, compact, worklist, "
          "cta_worklist, bulk_sync_mask, or auto");
ABSL_FLAG(std::string, kernel_variant, "scalar",
          "Metal kernel variant: scalar, word_parallel, simdgroup, or auto");
ABSL_FLAG(std::string, bitsup_layout, "pair",
          "Metal bitSup layout: pair, directional, or auto");
ABSL_FLAG(std::string, reset_mode, "cpu",
          "Metal mutable-state reset mode: cpu, blit, or auto");
ABSL_FLAG(std::string, policy_mode, "none",
          "Default-off runtime policy: none or bh_cta_allowlist");
ABSL_FLAG(std::string, cta_owner_mode, "modulo",
          "CTA worklist owner mapping: modulo, static_edge_cut, or "
          "vebo_weighted");
ABSL_FLAG(std::string, cta_queue_mode, "local_only",
          "CTA worklist queue mode: local_only, spill_replay, or "
          "bounded_replay");
ABSL_FLAG(std::string, cta_handoff_mode, "push_constraints",
          "CTA worklist handoff mode: push_constraints or dirty_var_pull");
ABSL_FLAG(int, cta_local_round_budget, 8,
          "CTA worklist local round budget before spilling pending work");
ABSL_FLAG(int, cta_replay_round_budget, 8,
          "CTA bounded_replay extra local rounds after the base budget");
ABSL_FLAG(int, cta_dirty_pull_min_degree, 0,
          "Minimum subscription degree for dirty_var_pull; lower-degree "
          "cross-owner handoff falls back to direct push");
ABSL_FLAG(bool, cpu_timing, false,
          "Measure CPU GAC Run() time and include it in benchmark CSV");
ABSL_FLAG(int, cpu_warmup, -1,
          "CPU timing warmup runs; negative means use --warmup");
ABSL_FLAG(int, cpu_runs, -1,
          "CPU timing measured runs; negative means use --runs");

namespace {

struct CpuTimingRecord {
  int run = 0;
  double solve_ms = 0.0;
  cpim::model::GacCpuStats stats;
};

struct BenchmarkRecord {
  int run = 0;
  cpim::solver::metal::MetalGacStats stats;
  std::string policy_mode;
  bool policy_selected = false;
  std::string policy_reason;
  std::string policy_bucket;
  bool verified = false;
  bool cpu_timing_enabled = false;
  double cpu_solve_ms = 0.0;
  int cpu_iterations = 0;
  int cpu_deletions = 0;
  bool cpu_inconsistent = false;
};

struct PolicyDecision {
  std::string mode = "none";
  bool selected = false;
  std::string reason = "policy_disabled";
  std::string bucket;
};

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
      std::cout << "[Benchmark] Manifest input resolved to "
                << first_file->path.string() << "\n";
      model_or = parser->Parse(first_file->path);
    }
  }
  if (!model_or.ok()) {
    return model_or.status();
  }

  cpim::model::ModelNormalizer normalizer;
  auto normalized_or = normalizer.Normalize(*model_or);
  if (!normalized_or.ok()) {
    return normalized_or.status();
  }
  return std::move(*normalized_or);
}

std::string DefaultMetallibPath(const char* argv0) {
  std::filesystem::path exe_path = std::filesystem::absolute(argv0);
  return (exe_path.parent_path() / "metal_gac.metallib").string();
}

absl::StatusOr<cpim::solver::metal::MetalReadonlyStorageMode>
ParseReadonlyStorageMode(const std::string& value) {
  if (value == "shared") {
    return cpim::solver::metal::MetalReadonlyStorageMode::kShared;
  }
  if (value == "private") {
    return cpim::solver::metal::MetalReadonlyStorageMode::kPrivate;
  }
  return absl::InvalidArgumentError(
      "readonly_storage must be 'shared' or 'private'");
}

absl::StatusOr<cpim::solver::metal::MetalRunnerMode> ParseRunnerMode(
    const std::string& value) {
  if (value == "cold") {
    return cpim::solver::metal::MetalRunnerMode::kCold;
  }
  if (value == "prepared") {
    return cpim::solver::metal::MetalRunnerMode::kPrepared;
  }
  return absl::InvalidArgumentError("runner_mode must be 'cold' or 'prepared'");
}

absl::StatusOr<cpim::solver::metal::MetalFrontierMode> ParseFrontierMode(
    const std::string& value) {
  if (value == "flags") {
    return cpim::solver::metal::MetalFrontierMode::kFlags;
  }
  if (value == "compact") {
    return cpim::solver::metal::MetalFrontierMode::kCompact;
  }
  if (value == "worklist") {
    return cpim::solver::metal::MetalFrontierMode::kWorklist;
  }
  if (value == "cta_worklist") {
    return cpim::solver::metal::MetalFrontierMode::kCtaWorklist;
  }
  if (value == "bulk_sync_mask") {
    return cpim::solver::metal::MetalFrontierMode::kBulkSyncMask;
  }
  if (value == "auto") {
    return cpim::solver::metal::MetalFrontierMode::kAuto;
  }
  return absl::InvalidArgumentError(
      "frontier_mode must be 'flags', 'compact', 'worklist', "
      "'cta_worklist', 'bulk_sync_mask', or 'auto'");
}

absl::StatusOr<cpim::solver::metal::MetalKernelVariant> ParseKernelVariant(
    const std::string& value) {
  if (value == "scalar") {
    return cpim::solver::metal::MetalKernelVariant::kScalar;
  }
  if (value == "word_parallel") {
    return cpim::solver::metal::MetalKernelVariant::kWordParallel;
  }
  if (value == "simdgroup") {
    return cpim::solver::metal::MetalKernelVariant::kSimdgroup;
  }
  if (value == "auto") {
    return cpim::solver::metal::MetalKernelVariant::kAuto;
  }
  return absl::InvalidArgumentError(
      "kernel_variant must be 'scalar', 'word_parallel', 'simdgroup', or 'auto'");
}

absl::StatusOr<cpim::solver::metal::MetalBitSupLayout> ParseBitSupLayout(
    const std::string& value) {
  if (value == "pair") {
    return cpim::solver::metal::MetalBitSupLayout::kPair;
  }
  if (value == "directional") {
    return cpim::solver::metal::MetalBitSupLayout::kDirectional;
  }
  if (value == "auto") {
    return cpim::solver::metal::MetalBitSupLayout::kAuto;
  }
  return absl::InvalidArgumentError(
      "bitsup_layout must be 'pair', 'directional', or 'auto'");
}

absl::StatusOr<cpim::solver::metal::MetalResetMode> ParseResetMode(
    const std::string& value) {
  if (value == "cpu") {
    return cpim::solver::metal::MetalResetMode::kCpu;
  }
  if (value == "blit") {
    return cpim::solver::metal::MetalResetMode::kBlit;
  }
  if (value == "auto") {
    return cpim::solver::metal::MetalResetMode::kAuto;
  }
  return absl::InvalidArgumentError(
      "reset_mode must be 'cpu', 'blit', or 'auto'");
}

absl::StatusOr<cpim::solver::metal::MetalCtaOwnerMode> ParseCtaOwnerMode(
    const std::string& value) {
  if (value == "modulo") {
    return cpim::solver::metal::MetalCtaOwnerMode::kModulo;
  }
  if (value == "static_edge_cut") {
    return cpim::solver::metal::MetalCtaOwnerMode::kStaticEdgeCut;
  }
  if (value == "vebo_weighted") {
    return cpim::solver::metal::MetalCtaOwnerMode::kVeboWeighted;
  }
  return absl::InvalidArgumentError(
      "cta_owner_mode must be 'modulo', 'static_edge_cut', or "
      "'vebo_weighted'");
}

absl::StatusOr<cpim::solver::metal::MetalCtaQueueMode> ParseCtaQueueMode(
    const std::string& value) {
  if (value == "local_only") {
    return cpim::solver::metal::MetalCtaQueueMode::kLocalOnly;
  }
  if (value == "spill_replay") {
    return cpim::solver::metal::MetalCtaQueueMode::kSpillReplay;
  }
  if (value == "bounded_replay") {
    return cpim::solver::metal::MetalCtaQueueMode::kBoundedReplay;
  }
  return absl::InvalidArgumentError(
      "cta_queue_mode must be 'local_only', 'spill_replay', or "
      "'bounded_replay'");
}

absl::StatusOr<cpim::solver::metal::MetalCtaHandoffMode> ParseCtaHandoffMode(
    const std::string& value) {
  if (value == "push_constraints") {
    return cpim::solver::metal::MetalCtaHandoffMode::kPushConstraints;
  }
  if (value == "dirty_var_pull") {
    return cpim::solver::metal::MetalCtaHandoffMode::kDirtyVarPull;
  }
  return absl::InvalidArgumentError(
      "cta_handoff_mode must be 'push_constraints' or 'dirty_var_pull'");
}

absl::Status ValidatePolicyMode(const std::string& value) {
  if (value == "none" || value == "bh_cta_allowlist") {
    return absl::OkStatus();
  }
  return absl::InvalidArgumentError(
      "policy_mode must be 'none' or 'bh_cta_allowlist'");
}

bool PathHasComponent(const std::string& input, const std::string& component) {
  for (const auto& part : std::filesystem::path(input)) {
    if (part.string() == component) {
      return true;
    }
  }
  return false;
}

PolicyDecision ApplyRuntimePolicy(
    const std::string& policy_mode,
    const std::string& input,
    const cpim::model::DeviceModelLayout& layout,
    cpim::solver::metal::MetalGacOptions* options) {
  PolicyDecision decision;
  decision.mode = policy_mode;
  if (policy_mode == "none") {
    return decision;
  }

  decision.bucket = "BH-4-4 cons=128-511 dom<17 bitw<2";
  const bool family_ok = PathHasComponent(input, "BH-4-4");
  const bool shape_ok =
      layout.num_constraints >= 128 && layout.num_constraints <= 511 &&
      layout.max_dom_size < 17 && layout.bit_words < 2;
  if (!family_ok) {
    decision.reason = "family_mismatch";
    return decision;
  }
  if (!shape_ok) {
    decision.reason = "shape_mismatch";
    return decision;
  }

  options->readonly_storage =
      cpim::solver::metal::MetalReadonlyStorageMode::kShared;
  options->frontier_mode = cpim::solver::metal::MetalFrontierMode::kCtaWorklist;
  options->kernel_variant =
      cpim::solver::metal::MetalKernelVariant::kWordParallel;
  options->bitsup_layout =
      cpim::solver::metal::MetalBitSupLayout::kDirectional;
  options->reset_mode = cpim::solver::metal::MetalResetMode::kCpu;
  options->cta_owner_mode =
      cpim::solver::metal::MetalCtaOwnerMode::kVeboWeighted;
  options->cta_queue_mode =
      cpim::solver::metal::MetalCtaQueueMode::kBoundedReplay;
  options->cta_handoff_mode =
      cpim::solver::metal::MetalCtaHandoffMode::kDirtyVarPull;
  options->cta_local_round_budget = 16;
  options->cta_replay_round_budget = 8;
  options->cta_dirty_pull_min_degree = 8;

  decision.selected = true;
  decision.reason = "bh_cta_allowlist_match";
  return decision;
}

template <typename MetalRunner>
bool SameResultShape(const cpim::model::GacCpuRunner& cpu,
                     const MetalRunner& metal) {
  return cpu.domain_sizes().size() == metal.domain_sizes().size() &&
         cpu.bit_dom().size() == metal.bit_dom().size();
}

template <typename MetalRunner>
bool ResultsMatch(const cpim::model::GacCpuRunner& cpu,
                  const cpim::model::GacCpuStats& cpu_stats,
                  const MetalRunner& metal,
                  const cpim::solver::metal::MetalGacStats& metal_stats) {
  if (cpu_stats.inconsistent != metal_stats.inconsistent ||
      metal_stats.budget_exceeded || !SameResultShape(cpu, metal)) {
    return false;
  }
  if (cpu_stats.inconsistent && metal_stats.inconsistent) {
    return true;
  }

  const auto& cpu_domains = cpu.domain_sizes();
  const auto& metal_domains = metal.domain_sizes();
  const auto& cpu_words = cpu.bit_dom();
  const auto& metal_words = metal.bit_dom();
  for (size_t i = 0; i < cpu_domains.size(); ++i) {
    if (cpu_domains[i] != metal_domains[i]) {
      return false;
    }
  }
  for (size_t i = 0; i < cpu_words.size(); ++i) {
    if (cpu_words[i] != metal_words[i]) {
      return false;
    }
  }
  return true;
}

double Percentile(std::vector<double> values, double q) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const double scaled = q * static_cast<double>(values.size() - 1);
  const size_t index = static_cast<size_t>(std::ceil(scaled));
  return values[std::min(index, values.size() - 1)];
}

std::vector<CpuTimingRecord> RunCpuTiming(
    const cpim::model::IntermediateModel& model, int warmup, int runs) {
  for (int i = 0; i < warmup; ++i) {
    cpim::model::GacCpuRunner cpu(model);
    (void)cpu.Run();
  }

  std::vector<CpuTimingRecord> records;
  records.reserve(runs);
  for (int run = 0; run < runs; ++run) {
    cpim::model::GacCpuRunner cpu(model);
    const auto start = std::chrono::steady_clock::now();
    const cpim::model::GacCpuStats stats = cpu.Run();
    const auto end = std::chrono::steady_clock::now();

    CpuTimingRecord record;
    record.run = run;
    record.solve_ms =
        std::chrono::duration<double, std::milli>(end - start).count();
    record.stats = stats;
    records.push_back(record);
  }
  return records;
}

void AttachCpuTiming(const std::vector<CpuTimingRecord>& cpu_records,
                     BenchmarkRecord* record) {
  if (cpu_records.empty()) {
    return;
  }
  const size_t index =
      std::min<size_t>(static_cast<size_t>(record->run), cpu_records.size() - 1);
  const auto& cpu = cpu_records[index];
  record->cpu_timing_enabled = true;
  record->cpu_solve_ms = cpu.solve_ms;
  record->cpu_iterations = cpu.stats.iterations;
  record->cpu_deletions = cpu.stats.deletions;
  record->cpu_inconsistent = cpu.stats.inconsistent;
}

std::string CsvEscape(const std::string& value) {
  if (value.find_first_of(",\"\n") == std::string::npos) {
    return value;
  }
  std::string out = "\"";
  for (char ch : value) {
    if (ch == '"') {
      out += "\"\"";
    } else {
      out += ch;
    }
  }
  out += "\"";
  return out;
}

absl::Status WriteCsv(const std::string& csv_path,
                      const std::string& input,
                      const cpim::model::DeviceModelLayout& layout,
                      const std::vector<BenchmarkRecord>& records) {
  if (csv_path.empty()) {
    return absl::OkStatus();
  }

  std::filesystem::path path(csv_path);
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream out(csv_path);
  if (!out) {
    return absl::InternalError("Failed to open benchmark CSV for writing");
  }

  out << "input,run,device,num_vars,num_constraints,max_dom_size,bit_words,"
      << "policy_mode,policy_selected,policy_reason,policy_bucket,"
      << "runner_mode,readonly_storage,frontier_mode,kernel_variant,"
      << "bitsup_layout,reset_mode,cta_owner_mode,cta_queue_mode,"
      << "cta_handoff_mode,"
      << "cta_local_round_budget,cta_replay_round_budget,"
      << "cta_dirty_pull_min_degree,"
      << "effective_frontier_mode,"
      << "effective_kernel_variant,effective_bitsup_layout,variant_name,"
      << "iterations,deletions,"
      << "dispatch_count,inconsistent,budget_exceeded,elapsed_ms,solve_ms,"
      << "cpu_timing_enabled,cpu_solve_ms,cpu_iterations,cpu_deletions,"
      << "cpu_inconsistent,metal_cpu_solve_ratio,metal_faster_than_cpu,"
      << "setup_ms,prepare_ms,reset_ms,reset_dispatch_ms,dispatch_ms,kernel_ms,"
      << "dispatch_encode_ms,dispatch_wait_ms,dispatch_non_kernel_ms,"
      << "active_constraints_total,worklist_push_count,worklist_rounds,"
      << "worklist_epoch_resets,cta_local_rounds,cta_queue_push_count,"
      << "cta_cross_push_count,cta_overflow_count,cta_queue_overflow_count,"
      << "cta_budget_spill_count,cta_seed_overflow_count,host_round_count,"
      << "cta_budget_replay_rounds,cta_budget_replay_drain_count,"
      << "cta_budget_replay_spill_count,"
      << "bulk_mask_proposed_deletion_count,bulk_mask_actual_deletion_count,"
      << "bulk_mask_changed_word_count,bulk_mask_frontier_push_count,"
      << "bulk_mask_rounds,"
      << "dirty_var_count,dirty_pull_scan_count,dirty_pull_hit_count,"
      << "cross_push_avoided_count,dirty_pull_fallback_push_count,"
      << "owner_map_build_ms,owner_balance_p95,owner_weight_balance_p95,"
      << "owner_local_push_count,owner_cross_push_count,seed_owner_nonempty_count,"
      << "seed_empty_owner_count,seed_max_owner_load,seed_owner_balance_p95,"
      << "frontier_density_avg,"
      << "gpu_timing_available,verified\n";
  out << std::fixed << std::setprecision(6);
  for (const auto& record : records) {
    const auto& s = record.stats;
    out << CsvEscape(input) << ","
        << record.run << ","
        << CsvEscape(s.device_name) << ","
        << layout.num_vars << ","
        << layout.num_constraints << ","
        << layout.max_dom_size << ","
        << layout.bit_words << ","
        << record.policy_mode << ","
        << (record.policy_selected ? "true" : "false") << ","
        << CsvEscape(record.policy_reason) << ","
        << CsvEscape(record.policy_bucket) << ","
        << s.runner_mode << ","
        << s.readonly_storage << ","
        << s.frontier_mode << ","
        << s.kernel_variant << ","
        << s.bitsup_layout << ","
        << s.reset_mode << ","
        << s.cta_owner_mode << ","
        << s.cta_queue_mode << ","
        << s.cta_handoff_mode << ","
        << s.cta_local_round_budget << ","
        << s.cta_replay_round_budget << ","
        << s.cta_dirty_pull_min_degree << ","
        << s.effective_frontier_mode << ","
        << s.effective_kernel_variant << ","
        << s.effective_bitsup_layout << ","
        << s.variant_name << ","
        << s.iterations << ","
        << s.deletions << ","
        << s.dispatch_count << ","
        << (s.inconsistent ? "true" : "false") << ","
        << (s.budget_exceeded ? "true" : "false") << ","
        << s.elapsed_ms << ","
        << s.solve_ms << ","
        << (record.cpu_timing_enabled ? "true" : "false") << ","
        << record.cpu_solve_ms << ","
        << record.cpu_iterations << ","
        << record.cpu_deletions << ","
        << (record.cpu_inconsistent ? "true" : "false") << ","
        << (record.cpu_solve_ms > 0.0 ? s.solve_ms / record.cpu_solve_ms : 0.0)
        << ","
        << (record.cpu_solve_ms > 0.0 && s.solve_ms < record.cpu_solve_ms
                ? "true"
                : "false")
        << ","
        << s.setup_ms << ","
        << s.prepare_ms << ","
        << s.reset_ms << ","
        << s.reset_dispatch_ms << ","
        << s.dispatch_ms << ","
        << s.kernel_ms << ","
        << s.dispatch_encode_ms << ","
        << s.dispatch_wait_ms << ","
        << s.dispatch_non_kernel_ms << ","
        << s.active_constraints_total << ","
        << s.worklist_push_count << ","
        << s.worklist_rounds << ","
        << s.worklist_epoch_resets << ","
        << s.cta_local_rounds << ","
        << s.cta_queue_push_count << ","
        << s.cta_cross_push_count << ","
        << s.cta_overflow_count << ","
        << s.cta_queue_overflow_count << ","
        << s.cta_budget_spill_count << ","
        << s.cta_seed_overflow_count << ","
        << s.host_round_count << ","
        << s.cta_budget_replay_rounds << ","
        << s.cta_budget_replay_drain_count << ","
        << s.cta_budget_replay_spill_count << ","
        << s.bulk_mask_proposed_deletion_count << ","
        << s.bulk_mask_actual_deletion_count << ","
        << s.bulk_mask_changed_word_count << ","
        << s.bulk_mask_frontier_push_count << ","
        << s.bulk_mask_rounds << ","
        << s.dirty_var_count << ","
        << s.dirty_pull_scan_count << ","
        << s.dirty_pull_hit_count << ","
        << s.cross_push_avoided_count << ","
        << s.dirty_pull_fallback_push_count << ","
        << s.owner_map_build_ms << ","
        << s.owner_balance_p95 << ","
        << s.owner_weight_balance_p95 << ","
        << s.owner_local_push_count << ","
        << s.owner_cross_push_count << ","
        << s.seed_owner_nonempty_count << ","
        << s.seed_empty_owner_count << ","
        << s.seed_max_owner_load << ","
        << s.seed_owner_balance_p95 << ","
        << s.frontier_density_avg << ","
        << (s.gpu_timing_available ? "true" : "false") << ","
        << (record.verified ? "true" : "false") << "\n";
  }
  return absl::OkStatus();
}

void PrintSummary(const std::vector<BenchmarkRecord>& records) {
  std::vector<double> solve;
  std::vector<double> elapsed;
  std::vector<double> dispatch;
  std::vector<double> kernel;
  std::vector<double> cpu_solve;
  std::vector<double> metal_cpu_ratio;
  solve.reserve(records.size());
  elapsed.reserve(records.size());
  dispatch.reserve(records.size());
  kernel.reserve(records.size());
  cpu_solve.reserve(records.size());
  metal_cpu_ratio.reserve(records.size());
  int metal_faster = 0;
  for (const auto& record : records) {
    solve.push_back(record.stats.solve_ms);
    elapsed.push_back(record.stats.elapsed_ms);
    dispatch.push_back(record.stats.dispatch_ms);
    kernel.push_back(record.stats.kernel_ms);
    if (record.cpu_timing_enabled && record.cpu_solve_ms > 0.0) {
      cpu_solve.push_back(record.cpu_solve_ms);
      metal_cpu_ratio.push_back(record.stats.solve_ms / record.cpu_solve_ms);
      if (record.stats.solve_ms < record.cpu_solve_ms) {
        ++metal_faster;
      }
    }
  }

  const auto& last = records.back().stats;
  const auto& last_record = records.back();
  std::cout << std::fixed << std::setprecision(3);
  std::cout << "[Benchmark] device=" << last.device_name
            << " policy_mode=" << last_record.policy_mode
            << " policy_selected="
            << (last_record.policy_selected ? "true" : "false")
            << " policy_reason=" << last_record.policy_reason
            << " policy_bucket=" << last_record.policy_bucket
            << " runner_mode=" << last.runner_mode
            << " readonly_storage=" << last.readonly_storage
            << " frontier_mode=" << last.frontier_mode
            << " kernel_variant=" << last.kernel_variant
            << " bitsup_layout=" << last.bitsup_layout
            << " reset_mode=" << last.reset_mode
            << " cta_owner_mode=" << last.cta_owner_mode
            << " cta_queue_mode=" << last.cta_queue_mode
            << " cta_handoff_mode=" << last.cta_handoff_mode
            << " cta_local_round_budget=" << last.cta_local_round_budget
            << " cta_replay_round_budget=" << last.cta_replay_round_budget
            << " cta_dirty_pull_min_degree="
            << last.cta_dirty_pull_min_degree
            << " effective_frontier_mode=" << last.effective_frontier_mode
            << " effective_kernel_variant=" << last.effective_kernel_variant
            << " effective_bitsup_layout=" << last.effective_bitsup_layout
            << " variant_name=" << last.variant_name
            << " runs=" << records.size()
            << " iterations=" << last.iterations
            << " deletions=" << last.deletions
            << " dispatch_count=" << last.dispatch_count
            << " worklist_push_count=" << last.worklist_push_count
            << " worklist_rounds=" << last.worklist_rounds
            << " worklist_epoch_resets=" << last.worklist_epoch_resets
            << " cta_local_rounds=" << last.cta_local_rounds
            << " cta_queue_push_count=" << last.cta_queue_push_count
            << " cta_cross_push_count=" << last.cta_cross_push_count
            << " cta_overflow_count=" << last.cta_overflow_count
            << " cta_queue_overflow_count=" << last.cta_queue_overflow_count
            << " cta_budget_spill_count=" << last.cta_budget_spill_count
            << " cta_seed_overflow_count=" << last.cta_seed_overflow_count
            << " cta_budget_replay_rounds=" << last.cta_budget_replay_rounds
            << " cta_budget_replay_drain_count="
            << last.cta_budget_replay_drain_count
            << " cta_budget_replay_spill_count="
            << last.cta_budget_replay_spill_count
            << " host_round_count=" << last.host_round_count
            << " bulk_mask_proposed_deletion_count="
            << last.bulk_mask_proposed_deletion_count
            << " bulk_mask_actual_deletion_count="
            << last.bulk_mask_actual_deletion_count
            << " bulk_mask_changed_word_count="
            << last.bulk_mask_changed_word_count
            << " bulk_mask_frontier_push_count="
            << last.bulk_mask_frontier_push_count
            << " bulk_mask_rounds=" << last.bulk_mask_rounds
            << " dirty_var_count=" << last.dirty_var_count
            << " dirty_pull_scan_count=" << last.dirty_pull_scan_count
            << " dirty_pull_hit_count=" << last.dirty_pull_hit_count
            << " cross_push_avoided_count=" << last.cross_push_avoided_count
            << " dirty_pull_fallback_push_count="
            << last.dirty_pull_fallback_push_count
            << " owner_map_build_ms=" << last.owner_map_build_ms
            << " owner_balance_p95=" << last.owner_balance_p95
            << " owner_weight_balance_p95=" << last.owner_weight_balance_p95
            << " owner_local_push_count=" << last.owner_local_push_count
            << " owner_cross_push_count=" << last.owner_cross_push_count
            << " seed_owner_nonempty_count=" << last.seed_owner_nonempty_count
            << " seed_empty_owner_count=" << last.seed_empty_owner_count
            << " seed_max_owner_load=" << last.seed_max_owner_load
            << " seed_owner_balance_p95=" << last.seed_owner_balance_p95
            << " frontier_density_avg=" << last.frontier_density_avg
            << " dispatch_encode_ms=" << last.dispatch_encode_ms
            << " dispatch_wait_ms=" << last.dispatch_wait_ms
            << " dispatch_non_kernel_ms=" << last.dispatch_non_kernel_ms
            << " gpu_timing_available="
            << (last.gpu_timing_available ? "true" : "false") << "\n";
  std::cout << "[Benchmark] solve_ms p50=" << Percentile(solve, 0.50)
            << " p95=" << Percentile(solve, 0.95)
            << " p99=" << Percentile(solve, 0.99) << "\n";
  std::cout << "[Benchmark] elapsed_ms p50=" << Percentile(elapsed, 0.50)
            << " p95=" << Percentile(elapsed, 0.95)
            << " p99=" << Percentile(elapsed, 0.99) << "\n";
  std::cout << "[Benchmark] dispatch_ms p50=" << Percentile(dispatch, 0.50)
            << " p95=" << Percentile(dispatch, 0.95)
            << " p99=" << Percentile(dispatch, 0.99) << "\n";
  std::cout << "[Benchmark] kernel_ms p50=" << Percentile(kernel, 0.50)
            << " p95=" << Percentile(kernel, 0.95)
            << " p99=" << Percentile(kernel, 0.99) << "\n";
  if (!cpu_solve.empty()) {
    std::cout << "[Benchmark] cpu_solve_ms p50=" << Percentile(cpu_solve, 0.50)
              << " p95=" << Percentile(cpu_solve, 0.95)
              << " p99=" << Percentile(cpu_solve, 0.99) << "\n";
    std::cout << "[Benchmark] metal_cpu_solve_ratio p50="
              << Percentile(metal_cpu_ratio, 0.50)
              << " p95=" << Percentile(metal_cpu_ratio, 0.95)
              << " p99=" << Percentile(metal_cpu_ratio, 0.99)
              << " metal_faster=" << metal_faster << "/"
              << metal_cpu_ratio.size() << "\n";
  }
}

}  // namespace

int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);
  absl::ParseCommandLine(argc, argv);

  const std::string input = absl::GetFlag(FLAGS_input);
  if (input.empty()) {
    std::cerr << "Usage: " << argv[0] << " --input=/path/to/instance.xml\n";
    return 1;
  }

  const int runs = std::max(1, absl::GetFlag(FLAGS_runs));
  const int warmup = std::max(0, absl::GetFlag(FLAGS_warmup));
  const bool cpu_timing = absl::GetFlag(FLAGS_cpu_timing);
  const int cpu_warmup =
      std::max(0, absl::GetFlag(FLAGS_cpu_warmup) < 0
                      ? warmup
                      : absl::GetFlag(FLAGS_cpu_warmup));
  const int cpu_runs =
      std::max(1, absl::GetFlag(FLAGS_cpu_runs) < 0
                      ? runs
                      : absl::GetFlag(FLAGS_cpu_runs));
  std::string metallib = absl::GetFlag(FLAGS_metallib);
  if (metallib.empty()) {
    metallib = DefaultMetallibPath(argv[0]);
  }

  auto model_or = LoadNormalizedModel(input);
  if (!model_or.ok()) {
    std::cerr << "[Benchmark] Parse/normalize failed: " << model_or.status() << "\n";
    return 1;
  }
  cpim::model::IntermediateModel model = std::move(*model_or);

  auto layout_or = cpim::model::BuildDeviceLayoutFromIntermediate(model);
  if (!layout_or.ok()) {
    std::cerr << "[Benchmark] Device layout build failed: "
              << layout_or.status() << "\n";
    return 1;
  }

  cpim::model::GacCpuRunner cpu(model);
  const cpim::model::GacCpuStats cpu_stats = cpu.Run();
  std::cout << "[Benchmark] Model: V=" << model.num_variables()
            << " C=" << model.num_constraints()
            << " max_dom=" << layout_or->max_dom_size
            << " bit_words=" << layout_or->bit_words << "\n";
  std::cout << "[CPU] iterations=" << cpu_stats.iterations
            << " deletions=" << cpu_stats.deletions
            << " inconsistent=" << (cpu_stats.inconsistent ? "true" : "false")
            << "\n";

  std::vector<CpuTimingRecord> cpu_timing_records;
  if (cpu_timing) {
    cpu_timing_records = RunCpuTiming(model, cpu_warmup, cpu_runs);
  }

  cpim::solver::metal::MetalGacOptions options;
  options.metallib_path = metallib;
  options.max_iterations = absl::GetFlag(FLAGS_max_iterations);
  options.verbose = absl::GetFlag(FLAGS_verbose);
  auto runner_mode_or = ParseRunnerMode(absl::GetFlag(FLAGS_runner_mode));
  if (!runner_mode_or.ok()) {
    std::cerr << "[Benchmark] " << runner_mode_or.status() << "\n";
    return 1;
  }
  auto readonly_storage_or = ParseReadonlyStorageMode(
      absl::GetFlag(FLAGS_readonly_storage));
  if (!readonly_storage_or.ok()) {
    std::cerr << "[Benchmark] " << readonly_storage_or.status() << "\n";
    return 1;
  }
  auto frontier_mode_or = ParseFrontierMode(absl::GetFlag(FLAGS_frontier_mode));
  if (!frontier_mode_or.ok()) {
    std::cerr << "[Benchmark] " << frontier_mode_or.status() << "\n";
    return 1;
  }
  auto kernel_variant_or = ParseKernelVariant(absl::GetFlag(FLAGS_kernel_variant));
  if (!kernel_variant_or.ok()) {
    std::cerr << "[Benchmark] " << kernel_variant_or.status() << "\n";
    return 1;
  }
  auto bitsup_layout_or = ParseBitSupLayout(absl::GetFlag(FLAGS_bitsup_layout));
  if (!bitsup_layout_or.ok()) {
    std::cerr << "[Benchmark] " << bitsup_layout_or.status() << "\n";
    return 1;
  }
  auto reset_mode_or = ParseResetMode(absl::GetFlag(FLAGS_reset_mode));
  if (!reset_mode_or.ok()) {
    std::cerr << "[Benchmark] " << reset_mode_or.status() << "\n";
    return 1;
  }
  auto cta_owner_mode_or =
      ParseCtaOwnerMode(absl::GetFlag(FLAGS_cta_owner_mode));
  if (!cta_owner_mode_or.ok()) {
    std::cerr << "[Benchmark] " << cta_owner_mode_or.status() << "\n";
    return 1;
  }
  auto cta_queue_mode_or =
      ParseCtaQueueMode(absl::GetFlag(FLAGS_cta_queue_mode));
  if (!cta_queue_mode_or.ok()) {
    std::cerr << "[Benchmark] " << cta_queue_mode_or.status() << "\n";
    return 1;
  }
  auto cta_handoff_mode_or =
      ParseCtaHandoffMode(absl::GetFlag(FLAGS_cta_handoff_mode));
  if (!cta_handoff_mode_or.ok()) {
    std::cerr << "[Benchmark] " << cta_handoff_mode_or.status() << "\n";
    return 1;
  }
  const int cta_local_round_budget =
      absl::GetFlag(FLAGS_cta_local_round_budget);
  const int cta_replay_round_budget =
      absl::GetFlag(FLAGS_cta_replay_round_budget);
  const int cta_dirty_pull_min_degree =
      absl::GetFlag(FLAGS_cta_dirty_pull_min_degree);
  if (cta_local_round_budget < 1 || cta_local_round_budget > 256) {
    std::cerr << "[Benchmark] cta_local_round_budget must be in [1, 256]\n";
    return 1;
  }
  if (cta_replay_round_budget < 0 || cta_replay_round_budget > 256) {
    std::cerr << "[Benchmark] cta_replay_round_budget must be in [0, 256]\n";
    return 1;
  }
  if (cta_dirty_pull_min_degree < 0) {
    std::cerr << "[Benchmark] cta_dirty_pull_min_degree must be >= 0\n";
    return 1;
  }
  const std::string policy_mode = absl::GetFlag(FLAGS_policy_mode);
  absl::Status policy_status = ValidatePolicyMode(policy_mode);
  if (!policy_status.ok()) {
    std::cerr << "[Benchmark] " << policy_status << "\n";
    return 1;
  }
  options.runner_mode = *runner_mode_or;
  options.readonly_storage = *readonly_storage_or;
  options.frontier_mode = *frontier_mode_or;
  options.kernel_variant = *kernel_variant_or;
  options.bitsup_layout = *bitsup_layout_or;
  options.reset_mode = *reset_mode_or;
  options.cta_owner_mode = *cta_owner_mode_or;
  options.cta_queue_mode = *cta_queue_mode_or;
  options.cta_handoff_mode = *cta_handoff_mode_or;
  options.cta_local_round_budget = cta_local_round_budget;
  options.cta_replay_round_budget = cta_replay_round_budget;
  options.cta_dirty_pull_min_degree = cta_dirty_pull_min_degree;

  const PolicyDecision policy_decision =
      ApplyRuntimePolicy(policy_mode, input, *layout_or, &options);
  std::cout << "[Benchmark] policy_mode=" << policy_decision.mode
            << " selected="
            << (policy_decision.selected ? "true" : "false")
            << " reason=" << policy_decision.reason
            << " bucket=" << policy_decision.bucket << "\n";

  std::vector<BenchmarkRecord> records;
  records.reserve(runs);
  const bool verify = absl::GetFlag(FLAGS_verify);

  if (options.runner_mode == cpim::solver::metal::MetalRunnerMode::kPrepared) {
    cpim::solver::metal::MetalPreparedGacRunner metal(*layout_or, options);
    absl::Status prepare_status = metal.Prepare();
    if (!prepare_status.ok()) {
      std::cerr << "[Benchmark] Prepare failed: " << prepare_status << "\n";
      return 1;
    }
    for (int i = 0; i < warmup; ++i) {
      auto stats_or = metal.Run();
      if (!stats_or.ok()) {
        std::cerr << "[Benchmark] Warmup failed: " << stats_or.status() << "\n";
        return 1;
      }
    }

    for (int run = 0; run < runs; ++run) {
      auto stats_or = metal.Run();
      if (!stats_or.ok()) {
        std::cerr << "[Benchmark] Metal run failed: " << stats_or.status()
                  << "\n";
        return 1;
      }

      BenchmarkRecord record;
      record.run = run;
      record.stats = *stats_or;
      record.policy_mode = policy_decision.mode;
      record.policy_selected = policy_decision.selected;
      record.policy_reason = policy_decision.reason;
      record.policy_bucket = policy_decision.bucket;
      AttachCpuTiming(cpu_timing_records, &record);
      record.verified = !verify ||
                        ResultsMatch(cpu, cpu_stats, metal, record.stats);
      if (!record.verified) {
        std::cerr << "[Benchmark] CPU and Metal results differ on run "
                  << run << "\n";
        return 1;
      }
      records.push_back(std::move(record));
    }
  } else {
    for (int i = 0; i < warmup; ++i) {
      cpim::solver::metal::MetalGacSolver metal(*layout_or, options);
      auto stats_or = metal.Run();
      if (!stats_or.ok()) {
        std::cerr << "[Benchmark] Warmup failed: " << stats_or.status() << "\n";
        return 1;
      }
    }

    for (int run = 0; run < runs; ++run) {
      cpim::solver::metal::MetalGacSolver metal(*layout_or, options);
      auto stats_or = metal.Run();
      if (!stats_or.ok()) {
        std::cerr << "[Benchmark] Metal run failed: " << stats_or.status()
                  << "\n";
        return 1;
      }

      BenchmarkRecord record;
      record.run = run;
      record.stats = *stats_or;
      record.policy_mode = policy_decision.mode;
      record.policy_selected = policy_decision.selected;
      record.policy_reason = policy_decision.reason;
      record.policy_bucket = policy_decision.bucket;
      AttachCpuTiming(cpu_timing_records, &record);
      record.verified = !verify ||
                        ResultsMatch(cpu, cpu_stats, metal, record.stats);
      if (!record.verified) {
        std::cerr << "[Benchmark] CPU and Metal results differ on run "
                  << run << "\n";
        return 1;
      }
      records.push_back(std::move(record));
    }
  }

  PrintSummary(records);
  absl::Status csv_status = WriteCsv(
      absl::GetFlag(FLAGS_csv), input, *layout_or, records);
  if (!csv_status.ok()) {
    std::cerr << "[Benchmark] CSV write failed: " << csv_status << "\n";
    return 1;
  }
  if (!absl::GetFlag(FLAGS_csv).empty()) {
    std::cout << "[Benchmark] CSV written to " << absl::GetFlag(FLAGS_csv) << "\n";
  }
  return 0;
}
