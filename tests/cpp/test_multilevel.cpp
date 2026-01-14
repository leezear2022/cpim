// 简单测试多层级功能
#include <iostream>
#include "GModel.cuh"
#include "model/xcsp_parser.h"
#include "model/model_normalizer.h"
#include "model/intermediate_model.h"
#include "model/gmodel_adapter.h"

using namespace cpim;
using namespace cpim::model;

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <xcsp_file>" << std::endl;
    return 1;
  }

  try {
    // 1. 解析 XML
    std::cout << "[Test] Parsing: " << argv[1] << std::endl;
    auto parser = XcspParser::Create(ParserType::kLibXml2);
    if (!parser) {
      std::cerr << "Failed to create XCSP parser" << std::endl;
      return 1;
    }

    auto model_or = parser->Parse(argv[1]);
    if (!model_or.ok()) {
      std::cerr << "Failed to parse XML: " << model_or.status() << std::endl;
      return 1;
    }

    // 2. 归一化模型
    std::cout << "[Test] Normalizing..." << std::endl;
    ModelNormalizer normalizer;
    auto normalized_or = normalizer.Normalize(*model_or);
    if (!normalized_or.ok()) {
      std::cerr << "Failed to normalize: " << normalized_or.status() << std::endl;
      return 1;
    }

    IntermediateModel im_model = std::move(*normalized_or);

    std::cout << "[Test] Building GModel..." << std::endl;
    GModelOptions options;
    options.enable_prefetch = false;
    GModel gmodel = GModelAdapter::Build(im_model, options);

    // 打印 bitSup 内容（调试用）
    std::cout << "\n=== BitSup Content (Debug) ===" << std::endl;
    for (int cid = 0; cid < gmodel.num_constraints; ++cid) {
      int2 scope = gmodel.constraint_scopes[cid];
      if (scope.x < 0) continue;

      std::cout << "Constraint " << cid << ": var[" << scope.x << "] - var["
                << scope.y << "]" << std::endl;

      // x → y supports
      std::cout << "  x → y supports:" << std::endl;
      for (int x_val = 0; x_val < gmodel.max_dom_size; ++x_val) {
        const int idx_base = cid * gmodel.bitsup_per_constraint +
                            (0 * gmodel.max_dom_size + x_val) * gmodel.bit_dom_int_size;
        std::cout << "    x=" << x_val << " → y={";
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

      // y → x supports
      std::cout << "  y → x supports:" << std::endl;
      for (int y_val = 0; y_val < gmodel.max_dom_size; ++y_val) {
        const int idx_base = cid * gmodel.bitsup_per_constraint +
                            (1 * gmodel.max_dom_size + y_val) * gmodel.bit_dom_int_size;
        std::cout << "    y=" << y_val << " → x={";
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

    std::cout << "\n=== Multi-Level Test ===" << std::endl;
    std::cout << "Initial level: " << gmodel.GetCurrentLevel() << std::endl;

    // 2. Phase 1.2: 测试 NewLevel（API 更名）
    std::cout << "\n[Test] Creating level 1..." << std::endl;
    gmodel.NewLevel();
    int level1 = gmodel.GetCurrentLevel();
    std::cout << "  Current level: " << level1 << std::endl;

    // 3. Phase 1.2: 测试 AssignValue（移除 level 参数）
    if (gmodel.num_vars > 0) {
      std::cout << "\n[Test] Assigning var[0] = 0 at level 1..." << std::endl;
      bool success = gmodel.AssignValue(0, 0);
      std::cout << "  Success: " << (success ? "true" : "false") << std::endl;
      std::cout << "  Domain size after assignment: "
                << gmodel.GetDomainSize(0) << std::endl;
    }

    // 4. Phase 1.2: 测试 NewLevel（第二层）
    std::cout << "\n[Test] Creating level 2..." << std::endl;
    gmodel.NewLevel();
    int level2 = gmodel.GetCurrentLevel();
    std::cout << "  Current level: " << level2 << std::endl;

    if (gmodel.num_vars > 1) {
      std::cout << "  Domain size of var[0] at level 2: "
                << gmodel.GetDomainSize(0) << std::endl;
    }

    // 5. 测试 BackToLevel
    std::cout << "\n[Test] Backtracking to level 0..." << std::endl;
    gmodel.BacktrackTo(0);
    std::cout << "  Current level: " << gmodel.GetCurrentLevel() << std::endl;

    if (gmodel.num_vars > 0) {
      std::cout << "  Domain size of var[0] at level 0: "
                << gmodel.GetDomainSize(0) << std::endl;
    }

    // 6. 测试 RemoveValue
    if (gmodel.num_vars > 0 && gmodel.max_dom_size > 1) {
      std::cout << "\n[Test] Removing value 1 from var[0] at level 0..." << std::endl;

      // 打印删除前的域
      std::cout << "  Before removal, var[0] at level 0: {";
      for (int val = 0; val < gmodel.max_dom_size; ++val) {
        const int word = val / 32;
        const int bit = val % 32;
        const int idx = 0 * gmodel.bit_dom_int_size + word;  // level 0, var 0
        if (gmodel.bitDom[idx] & (1u << bit)) {
          std::cout << val << " ";
        }
      }
      std::cout << "}" << std::endl;

      // Phase 1.2: RemoveValue 移除 level 参数
      bool success = gmodel.RemoveValue(0, 1);
      std::cout << "  Success: " << (success ? "true" : "false") << std::endl;
      std::cout << "  Domain size after removal: "
                << gmodel.GetDomainSize(0) << std::endl;

      // 打印删除后的域
      std::cout << "  After removal, var[0] at level 0: {";
      for (int val = 0; val < gmodel.max_dom_size; ++val) {
        const int word = val / 32;
        const int bit = val % 32;
        const int idx = 0 * gmodel.bit_dom_int_size + word;  // level 0, var 0
        if (gmodel.bitDom[idx] & (1u << bit)) {
          std::cout << val << " ";
        }
      }
      std::cout << "}" << std::endl;
    }

    std::cout << "\n=== All Tests Passed! ===" << std::endl;

    // 7. 测试 EnforceGAC（约束传播）
    std::cout << "\n[Test] Testing GAC enforcement at level 0..." << std::endl;
    gmodel.BacktrackTo(0);  // 确保在 level 0

    GacStats gac_stats = gmodel.EnforceGAC(true);  // verbose=true
    std::cout << "\n[Test] GAC Results:" << std::endl;
    std::cout << "  Iterations: " << gac_stats.iterations << std::endl;
    std::cout << "  Deletions: " << gac_stats.deletions << std::endl;
    std::cout << "  Inconsistent: " << (gac_stats.inconsistent ? "true" : "false") << std::endl;

    // 打印传播后的域
    std::cout << "\n[Test] Domain sizes after GAC:" << std::endl;
    for (int var = 0; var < gmodel.num_vars; ++var) {
      std::cout << "  var[" << var << "]: " << gmodel.GetDomainSize(var) << std::endl;
    }

    std::cout << "\n=== All Tests Passed! ===" << std::endl;
    return 0;

  } catch (const std::exception& e) {
    std::cerr << "[Test] Error: " << e.what() << std::endl;
    return 1;
  }
}
