// 调试 bitSup 内容
#include <iostream>
#include <bitset>
#include "GModel.cuh"
#include "model/xcsp_parser.h"
#include "model/model_normalizer.h"
#include "model/gmodel_adapter.h"

using namespace cpim;
using namespace cpim::model;

void PrintBitSup(const GModel& gmodel) {
  std::cout << "\n=== BitSup Content ===" << std::endl;

  for (int cid = 0; cid < gmodel.num_constraints; ++cid) {
    int2 scope = gmodel.constraint_scopes[cid];
    if (scope.x < 0) continue;

    std::cout << "\nConstraint " << cid << ": var[" << scope.x << "] - var["
              << scope.y << "]" << std::endl;

    // 打印 x → y 的支持
    std::cout << "  x → y supports:" << std::endl;
    for (int x_val = 0; x_val < gmodel.max_dom_size; ++x_val) {
      std::cout << "    x=" << x_val << " → y={";

      const int idx_base = cid * gmodel.bitsup_per_constraint +
                          (0 * gmodel.max_dom_size + x_val) * gmodel.bit_dom_int_size;

      bool first = true;
      for (int y_val = 0; y_val < gmodel.max_dom_size; ++y_val) {
        const int word = y_val / 32;
        const int bit = y_val % 32;
        if (gmodel.bitSupData[idx_base + word].x & (1u << bit)) {
          if (!first) std::cout << ",";
          std::cout << y_val;
          first = false;
        }
      }
      std::cout << "}" << std::endl;
    }

    // 打印 y → x 的支持
    std::cout << "  y → x supports:" << std::endl;
    for (int y_val = 0; y_val < gmodel.max_dom_size; ++y_val) {
      std::cout << "    y=" << y_val << " → x={";

      const int idx_base = cid * gmodel.bitsup_per_constraint +
                          (1 * gmodel.max_dom_size + y_val) * gmodel.bit_dom_int_size;

      bool first = true;
      for (int x_val = 0; x_val < gmodel.max_dom_size; ++x_val) {
        const int word = x_val / 32;
        const int bit = x_val % 32;
        if (gmodel.bitSupData[idx_base + word].y & (1u << bit)) {
          if (!first) std::cout << ",";
          std::cout << x_val;
          first = false;
        }
      }
      std::cout << "}" << std::endl;
    }
  }
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <xcsp_file>" << std::endl;
    return 1;
  }

  try {
    // 解析模型
    auto parser = XcspParser::Create(ParserType::kLibXml2);
    if (!parser) return 1;

    auto model_or = parser->Parse(argv[1]);
    if (!model_or.ok()) {
      std::cerr << "Parse failed: " << model_or.status() << std::endl;
      return 1;
    }

    ModelNormalizer normalizer;
    auto normalized_or = normalizer.Normalize(*model_or);
    if (!normalized_or.ok()) {
      std::cerr << "Normalize failed: " << normalized_or.status() << std::endl;
      return 1;
    }

    IntermediateModel im_model = std::move(*normalized_or);

    // 构建 GModel
    GModelOptions options;
    options.enable_prefetch = false;
    GModel gmodel = GModelAdapter::Build(im_model, options);

    // 打印 bitSup
    PrintBitSup(gmodel);

    return 0;

  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }
}
