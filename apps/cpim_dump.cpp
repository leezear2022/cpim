#include <iostream>
#include <string>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "glog/logging.h"

#include <thrust/host_vector.h>

#include "CPIMBase.h"
#include "model/cmodel_adapter.h"
#include "model/model_normalizer.h"
#include "model/xcsp_parser.h"

#include "cuSAC.cuh"

ABSL_FLAG(std::string, input, "", "Path to XCSP2/3 XML instance");
ABSL_FLAG(int, max_print, 8, "Maximum number of entries to print per array");

namespace {

template <typename Container>
void PrintVector(const Container& values, int limit,
                 const std::string& label) {
  const int size = static_cast<int>(values.size());
  std::cout << label << " (size=" << values.size() << "): ";
  for (int i = 0; i < size && i < limit; ++i) {
    if (i) std::cout << ", ";
    std::cout << values[i];
  }
  if (size > limit) {
    std::cout << ", ...";
  }
  std::cout << '\n';
}

void PrintUint3Vector(const thrust::host_vector<uint3>& vec, int limit,
                      const std::string& label) {
  std::cout << label << " (size=" << vec.size() << "): ";
  for (int i = 0; i < static_cast<int>(vec.size()) && i < limit; ++i) {
    if (i) std::cout << "; ";
    std::cout << "(" << vec[i].x << ", " << vec[i].y << ", " << vec[i].z
              << ")";
  }
  if (static_cast<int>(vec.size()) > limit) {
    std::cout << "; ...";
  }
  std::cout << '\n';
}

void PrintBitDom(const cpim::CModel& model, int limit) {
  std::cout << "bitDom level 0:" << '\n';
  for (int var = 0; var < model.kNumVars && var < limit; ++var) {
    std::cout << "  var " << var << ": ";
    for (int word = 0; word < model.kBitDomIntSize; ++word) {
      const cpim::u32 value =
          model.h_bitDom[var * model.kBitDomIntSize + word];
      std::cout << std::hex << value << std::dec;
      if (word + 1 < model.kBitDomIntSize) std::cout << " ";
    }
    std::cout << '\n';
  }
}

}  // namespace

int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);
  absl::ParseCommandLine(argc, argv);

  const std::string input = absl::GetFlag(FLAGS_input);
  const int max_print = std::max(1, absl::GetFlag(FLAGS_max_print));

  if (input.empty()) {
    std::cerr << "Usage: " << argv[0] << " --input=/path/to/instance.xml" << '\n';
    return 1;
  }

  auto parser = cpim::model::XcspParser::Create(cpim::model::ParserType::kLibXml2);
  if (!parser) {
    std::cerr << "Failed to create XCSP parser" << '\n';
    return 1;
  }

  auto model_or = parser->Parse(input);
  if (!model_or.ok()) {
    std::cerr << "Failed to parse XML: " << model_or.status() << '\n';
    return 1;
  }

  cpim::model::ModelNormalizer normalizer;
  auto normalized_or = normalizer.Normalize(*model_or);
  if (!normalized_or.ok()) {
    std::cerr << "Failed to normalize model: " << normalized_or.status() << '\n';
    return 1;
  }

  cpim::model::IntermediateModel normalized = std::move(*normalized_or);

  try {
    auto adapter = cpim::model::CModelAdapter::FromIntermediate(normalized);
    cpim::CModel gpu_model(std::move(adapter));

    std::cout << "=== CModel Summary ===" << '\n';
    std::cout << "Variables: " << gpu_model.kNumVars
              << ", Constraints: " << gpu_model.kNumTabs
              << ", MaxDomSize: " << gpu_model.kMaxDomSize << '\n';

    PrintVector(gpu_model.h_dom_size, max_print, "Domain sizes");
    PrintVector(gpu_model.h_Deg, max_print, "Variable degrees");

    PrintUint3Vector(gpu_model.h_subscription, max_print,
                     "Subscription entries");

    thrust::host_vector<uint3> host_constraints = gpu_model.d_MCon;
    PrintUint3Vector(host_constraints, max_print, "Constraint scope list");

    PrintBitDom(gpu_model, max_print);

    std::cout << "=== End of dump ===" << '\n';
  } catch (const std::exception& e) {
    std::cerr << "Failed to build CModel: " << e.what() << '\n';
    return 1;
  }

  return 0;
}
