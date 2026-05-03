#include <algorithm>
#include <cstdint>
#include <filesystem>
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
ABSL_FLAG(int, max_iterations, 10000, "Maximum Metal host-dispatch iterations");
ABSL_FLAG(int, max_print, 8, "Maximum mismatched variables to print");
ABSL_FLAG(bool, verbose, false, "Print per-iteration Metal GAC statistics");

namespace {

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
      std::cout << "[Compare] Manifest input resolved to "
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

void PrintWords(std::ostream& out, const std::vector<uint32_t>& words, int base,
                int bit_words) {
  for (int word = 0; word < bit_words; ++word) {
    if (word > 0) out << " ";
    out << "0x" << std::hex << words[base + word] << std::dec;
  }
}

bool SameResultShape(const cpim::model::GacCpuRunner& cpu,
                     const cpim::solver::metal::MetalGacSolver& metal) {
  const auto& cpu_domains = cpu.domain_sizes();
  const auto& metal_domains = metal.domain_sizes();
  const auto& cpu_words = cpu.bit_dom();
  const auto& metal_words = metal.bit_dom();

  if (cpu_domains.size() != metal_domains.size() ||
      cpu_words.size() != metal_words.size()) {
    std::cerr << "[Compare] Result shape mismatch: CPU domains="
              << cpu_domains.size() << " Metal domains=" << metal_domains.size()
              << " CPU words=" << cpu_words.size()
              << " Metal words=" << metal_words.size() << "\n";
    return false;
  }
  return true;
}

bool CompareResults(const cpim::model::GacCpuRunner& cpu,
                    const cpim::solver::metal::MetalGacSolver& metal,
                    int max_print) {
  if (!SameResultShape(cpu, metal)) {
    return false;
  }

  bool ok = true;
  int printed = 0;
  const auto& cpu_domains = cpu.domain_sizes();
  const auto& metal_domains = metal.domain_sizes();
  const auto& cpu_words = cpu.bit_dom();
  const auto& metal_words = metal.bit_dom();

  for (int var = 0; var < cpu.num_vars(); ++var) {
    const int base = var * cpu.bit_words();
    bool var_ok = cpu_domains[var] == metal_domains[var];
    for (int word = 0; word < cpu.bit_words(); ++word) {
      if (cpu_words[base + word] != metal_words[base + word]) {
        var_ok = false;
        break;
      }
    }

    if (!var_ok) {
      ok = false;
      if (printed < max_print) {
        std::cerr << "[Compare] var " << var
                  << " domain_size CPU=" << cpu_domains[var]
                  << " Metal=" << metal_domains[var] << "\n";
        std::cerr << "  CPU   ";
        PrintWords(std::cerr, cpu_words, base, cpu.bit_words());
        std::cerr << "\n  Metal ";
        PrintWords(std::cerr, metal_words, base, cpu.bit_words());
        std::cerr << "\n";
        ++printed;
      }
    }
  }

  if (!ok && printed >= max_print) {
    std::cerr << "[Compare] More mismatches omitted by --max_print\n";
  }
  return ok;
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

  std::string metallib = absl::GetFlag(FLAGS_metallib);
  if (metallib.empty()) {
    metallib = DefaultMetallibPath(argv[0]);
  }

  auto model_or = LoadNormalizedModel(input);
  if (!model_or.ok()) {
    std::cerr << "[Compare] Parse/normalize failed: " << model_or.status() << "\n";
    return 1;
  }
  cpim::model::IntermediateModel model = std::move(*model_or);

  auto layout_or = cpim::model::BuildDeviceLayoutFromIntermediate(model);
  if (!layout_or.ok()) {
    std::cerr << "[Compare] Device layout build failed: "
              << layout_or.status() << "\n";
    return 1;
  }

  std::cout << "[Compare] Model: V=" << model.num_variables()
            << " C=" << model.num_constraints()
            << " max_dom=" << layout_or->max_dom_size
            << " bit_words=" << layout_or->bit_words << "\n";

  cpim::model::GacCpuRunner cpu(model);
  const cpim::model::GacCpuStats cpu_stats = cpu.Run();
  std::cout << "[CPU] iterations=" << cpu_stats.iterations
            << " deletions=" << cpu_stats.deletions
            << " inconsistent=" << (cpu_stats.inconsistent ? "true" : "false")
            << "\n";

  cpim::solver::metal::MetalGacOptions options;
  options.metallib_path = metallib;
  options.max_iterations = absl::GetFlag(FLAGS_max_iterations);
  options.verbose = absl::GetFlag(FLAGS_verbose);

  cpim::solver::metal::MetalGacSolver metal(*layout_or, options);
  auto metal_stats_or = metal.Run();
  if (!metal_stats_or.ok()) {
    std::cerr << "[Metal] Run failed: " << metal_stats_or.status() << "\n";
    return 1;
  }
  const auto metal_stats = *metal_stats_or;
  std::cout << "[Metal] device=" << metal_stats.device_name
            << " readonly_storage=" << metal_stats.readonly_storage
            << " frontier_mode=" << metal_stats.frontier_mode
            << " iterations=" << metal_stats.iterations
            << " deletions=" << metal_stats.deletions
            << " dispatch_count=" << metal_stats.dispatch_count
            << " inconsistent=" << (metal_stats.inconsistent ? "true" : "false")
            << " budget_exceeded="
            << (metal_stats.budget_exceeded ? "true" : "false")
            << " elapsed_ms=" << metal_stats.elapsed_ms
            << " setup_ms=" << metal_stats.setup_ms
            << " dispatch_ms=" << metal_stats.dispatch_ms
            << " kernel_ms=" << metal_stats.kernel_ms
            << " gpu_timing_available="
            << (metal_stats.gpu_timing_available ? "true" : "false") << "\n";

  const int max_print = std::max(1, absl::GetFlag(FLAGS_max_print));
  const bool same_inconsistent = cpu_stats.inconsistent == metal_stats.inconsistent;
  if (!same_inconsistent || metal_stats.budget_exceeded) {
    std::cerr << "[Compare] CPU and Metal GAC results differ\n";
    return 1;
  }

  if (cpu_stats.inconsistent && metal_stats.inconsistent) {
    if (!SameResultShape(cpu, metal)) {
      return 1;
    }
    std::cout << "[Compare] Both CPU and Metal report inconsistent; "
              << "final domains are not compared\n";
    return 0;
  }

  if (!CompareResults(cpu, metal, max_print)) {
    std::cerr << "[Compare] CPU and Metal GAC results differ\n";
    return 1;
  }

  std::cout << "[Compare] CPU and Metal GAC results match\n";
  return 0;
}
