#include <algorithm>
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
          "Metal frontier dispatch mode: flags, compact, worklist, or auto");
ABSL_FLAG(std::string, kernel_variant, "scalar",
          "Metal kernel variant: scalar, word_parallel, simdgroup, or auto");
ABSL_FLAG(std::string, bitsup_layout, "pair",
          "Metal bitSup layout: pair, directional, or auto");
ABSL_FLAG(std::string, reset_mode, "cpu",
          "Metal mutable-state reset mode: cpu, blit, or auto");

namespace {

struct BenchmarkRecord {
  int run = 0;
  cpim::solver::metal::MetalGacStats stats;
  bool verified = false;
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
  if (value == "auto") {
    return cpim::solver::metal::MetalFrontierMode::kAuto;
  }
  return absl::InvalidArgumentError(
      "frontier_mode must be 'flags', 'compact', 'worklist', or 'auto'");
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
      << "runner_mode,readonly_storage,frontier_mode,kernel_variant,"
      << "bitsup_layout,reset_mode,effective_frontier_mode,"
      << "effective_kernel_variant,effective_bitsup_layout,variant_name,"
      << "iterations,deletions,"
      << "dispatch_count,inconsistent,budget_exceeded,elapsed_ms,solve_ms,"
      << "setup_ms,prepare_ms,reset_ms,reset_dispatch_ms,dispatch_ms,kernel_ms,"
      << "active_constraints_total,worklist_push_count,worklist_rounds,"
      << "worklist_epoch_resets,frontier_density_avg,"
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
        << s.runner_mode << ","
        << s.readonly_storage << ","
        << s.frontier_mode << ","
        << s.kernel_variant << ","
        << s.bitsup_layout << ","
        << s.reset_mode << ","
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
        << s.setup_ms << ","
        << s.prepare_ms << ","
        << s.reset_ms << ","
        << s.reset_dispatch_ms << ","
        << s.dispatch_ms << ","
        << s.kernel_ms << ","
        << s.active_constraints_total << ","
        << s.worklist_push_count << ","
        << s.worklist_rounds << ","
        << s.worklist_epoch_resets << ","
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
  solve.reserve(records.size());
  elapsed.reserve(records.size());
  dispatch.reserve(records.size());
  kernel.reserve(records.size());
  for (const auto& record : records) {
    solve.push_back(record.stats.solve_ms);
    elapsed.push_back(record.stats.elapsed_ms);
    dispatch.push_back(record.stats.dispatch_ms);
    kernel.push_back(record.stats.kernel_ms);
  }

  const auto& last = records.back().stats;
  std::cout << std::fixed << std::setprecision(3);
  std::cout << "[Benchmark] device=" << last.device_name
            << " runner_mode=" << last.runner_mode
            << " readonly_storage=" << last.readonly_storage
            << " frontier_mode=" << last.frontier_mode
            << " kernel_variant=" << last.kernel_variant
            << " bitsup_layout=" << last.bitsup_layout
            << " reset_mode=" << last.reset_mode
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
            << " frontier_density_avg=" << last.frontier_density_avg
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
  options.runner_mode = *runner_mode_or;
  options.readonly_storage = *readonly_storage_or;
  options.frontier_mode = *frontier_mode_or;
  options.kernel_variant = *kernel_variant_or;
  options.bitsup_layout = *bitsup_layout_or;
  options.reset_mode = *reset_mode_or;

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
