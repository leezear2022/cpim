#include <iostream>
#include <string>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "glog/logging.h"

#include "GModel.cuh"
#include "model/gmodel_adapter.h"
#include "model/gmodel_validator.h"
#include "model/model_normalizer.h"
#include "model/xcsp_parser.h"

ABSL_FLAG(std::string, input, "", "Path to XCSP2/3 XML instance");
ABSL_FLAG(int, max_print, 8, "Maximum number of entries to print per array");
ABSL_FLAG(bool, run_gac, false,
          "Run baseline GPU CsCheckMain propagation after building GModel");

int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);
  absl::ParseCommandLine(argc, argv);

  const std::string input = absl::GetFlag(FLAGS_input);
  const int max_print = std::max(1, absl::GetFlag(FLAGS_max_print));

  if (input.empty()) {
    std::cerr << "Usage: " << argv[0] << " --input=/path/to/instance.xml"
              << std::endl;
    return 1;
  }

  // ========================================================================
  // 1. 解析 XML
  // ========================================================================
  std::cout << "[Main] Parsing XML: " << input << std::endl;

  auto parser =
      cpim::model::XcspParser::Create(cpim::model::ParserType::kLibXml2);
  if (!parser) {
    std::cerr << "Failed to create XCSP parser" << std::endl;
    return 1;
  }

  auto model_or = parser->Parse(input);
  if (!model_or.ok()) {
    std::cerr << "Failed to parse XML: " << model_or.status() << std::endl;
    return 1;
  }

  std::cout << "[Main] XML parsed successfully" << std::endl;

  // ========================================================================
  // 2. 归一化模型
  // ========================================================================
  std::cout << "[Main] Normalizing model..." << std::endl;

  cpim::model::ModelNormalizer normalizer;
  auto normalized_or = normalizer.Normalize(*model_or);
  if (!normalized_or.ok()) {
    std::cerr << "Failed to normalize model: " << normalized_or.status()
              << std::endl;
    return 1;
  }

  cpim::model::IntermediateModel normalized = std::move(*normalized_or);
  std::cout << "[Main] Model normalized successfully" << std::endl;
  std::cout << "  Variables: " << normalized.num_variables() << std::endl;
  std::cout << "  Constraints: " << normalized.num_constraints() << std::endl;

  // ========================================================================
  // 3. 构建 GModel (使用新的 GModelAdapter)
  // ========================================================================
  std::cout << "\n[Main] Building GModel using GModelAdapter..." << std::endl;

  try {
    // 配置 GModel 构建选项
    cpim::model::GModelOptions options;
    options.device_id = 0;
    options.enable_prefetch = false;  // Jetson 不需要
    options.skip_non_binary = true;   // 跳过非二元约束

    // 使用 GModelAdapter 构建
    cpim::GModel gmodel =
        cpim::model::GModelAdapter::Build(normalized, options);

    std::cout << "[Main] GModel built successfully!" << std::endl;

    // ========================================================================
    // 4. 验证 GModel
    // ========================================================================
    std::cout << "\n[Main] Validating GModel..." << std::endl;

    auto validation = cpim::model::GModelValidator::ValidateBasic(gmodel);
    if (!validation.success) {
      std::cerr << "Basic validation failed:\n"
                << validation.ToString() << std::endl;
      return 1;
    }
    std::cout << "[Main] Basic validation passed" << std::endl;

    // ========================================================================
    // 5. 打印 GModel（CPU 端）
    // ========================================================================
    gmodel.Print(max_print);

    // ========================================================================
    // 6. GPU 验证
    // ========================================================================
    std::cout << "\n[Main] Running GPU validation..." << std::endl;
    auto gpu_validation =
        cpim::model::GModelValidator::ValidateGPUMemory(gmodel);

    if (!gpu_validation.success) {
      std::cerr << "GPU validation failed:\n"
                << gpu_validation.ToString() << std::endl;
      return 1;
    }
    std::cout << "[Main] GPU validation passed" << std::endl;

    if (absl::GetFlag(FLAGS_run_gac)) {
      std::cout << "\n[Main] Running baseline GAC propagation..." << std::endl;
      auto stats = gmodel.EnforceGAC(/*verbose=*/true);
      if (stats.inconsistent) {
        std::cout << "[Main] Propagation detected inconsistency" << std::endl;
      } else {
        std::cout << "[Main] Propagation finished without inconsistency"
                  << std::endl;
      }
    }

  } catch (const std::exception& e) {
    std::cerr << "Failed to build or validate GModel: " << e.what()
              << std::endl;
    return 1;
  }

  std::cout << "\n[Main] All done!" << std::endl;
  return 0;
}
