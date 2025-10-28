#include <iostream>
#include <string>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "glog/logging.h"

#include "model/gac_cpu.h"
#include "model/model_normalizer.h"
#include "model/xcsp_parser.h"

ABSL_FLAG(std::string, input, "", "Path to XCSP2/3 XML instance");
ABSL_FLAG(int, max_print, 8, "Maximum number of variable domains to print");

int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);
  absl::ParseCommandLine(argc, argv);

  const std::string input = absl::GetFlag(FLAGS_input);
  const int max_print = std::max(1, absl::GetFlag(FLAGS_max_print));
  if (input.empty()) {
    std::cerr << "Usage: " << argv[0] << " --input=/path/to/instance.xml\n";
    return 1;
  }

  std::cout << "[GAC-CPU] Parsing XML: " << input << std::endl;
  auto parser = cpim::model::XcspParser::Create(cpim::model::ParserType::kLibXml2);
  if (!parser) {
    std::cerr << "Failed to create parser" << std::endl;
    return 1;
  }
  auto model_or = parser->Parse(input);
  if (!model_or.ok()) {
    std::cerr << "Parse failed: " << model_or.status() << std::endl;
    return 1;
  }

  cpim::model::ModelNormalizer normalizer;
  auto normalized_or = normalizer.Normalize(*model_or);
  if (!normalized_or.ok()) {
    std::cerr << "Normalize failed: " << normalized_or.status() << std::endl;
    return 1;
  }
  cpim::model::IntermediateModel im = std::move(*normalized_or);
  std::cout << "[GAC-CPU] Model normalized: V=" << im.num_variables()
            << " C=" << im.num_constraints() << std::endl;

  cpim::model::GacCpuRunner runner(im);
  runner.Print(max_print);
  auto stats = runner.Run();
  std::cout << "\n[GAC-CPU] Done. iterations=" << stats.iterations
            << " deletions=" << stats.deletions
            << " inconsistent=" << (stats.inconsistent ? "true" : "false")
            << std::endl;
  runner.Print(max_print);

  return 0;
}

