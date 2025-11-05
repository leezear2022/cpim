// 简单的 GModel MAC 求解器
#include <iostream>
#include <vector>
#include "GModel.cuh"
#include "model/xcsp_parser.h"
#include "model/model_normalizer.h"
#include "model/gmodel_adapter.h"

using namespace cpim;
using namespace cpim::model;

// 简单的 MAC 搜索
class SimpleGModelSolver {
 public:
  explicit SimpleGModelSolver(GModel* model) : model_(model) {}

  bool Solve() {
    std::cout << "\n=== Starting MAC Search ===" << std::endl;

    // 初始化 solution_
    solution_.assign(model_->num_vars, -1);

    // 初始 GAC
    GacStats init_stats = model_->EnforceGAC(false);
    std::cout << "[Solver] Initial GAC: " << init_stats.deletions << " deletions, "
              << init_stats.iterations << " iterations" << std::endl;

    if (init_stats.inconsistent || HasEmptyDomain(0)) {
      std::cout << "[Solver] Problem is inconsistent!" << std::endl;
      return false;
    }

    // 打印域大小（调试）
    std::cout << "[Solver] Domain sizes after initial GAC:" << std::endl;
    for (int var = 0; var < model_->num_vars; ++var) {
      int size = model_->GetDomainSize(var, 0);
      std::cout << "  var[" << var << "]: " << size << std::endl;
    }

    std::cout << "[Solver] Starting search from level 0..." << std::endl;
    return Search(0);
  }

  const std::vector<int>& GetSolution() const { return solution_; }

 private:
  bool Search(int level) {
    ++nodes_;

    // 选择未赋值变量（简单启发式：最小域）
    int var = SelectVariable(level);
    if (var == -1) {
      // 所有变量已赋值 → 找到解
      std::cout << "\n[Solver] Solution found at level " << level << "!" << std::endl;
      RecordSolution(level);
      return true;
    }

    // 获取域值
    std::vector<int> domain = GetDomain(var, level);
    std::cout << "[Solver] Level " << level << ": var[" << var << "] domain size = "
              << domain.size() << std::endl;

    // 尝试每个值
    for (int value : domain) {
      std::cout << "[Try] Level " << level << ": var[" << var
                << "] = " << value << std::endl;

      // 创建新层级
      int new_level = model_->CreateNewLevel();

      // 赋值
      model_->AssignValue(var, value, new_level);
      solution_[var] = value;

      // 传播
      GacStats stats = model_->EnforceGAC(false);
      std::cout << "  [GAC] deletions=" << stats.deletions
                << ", inconsistent=" << (stats.inconsistent ? "true" : "false");

      // 检查一致性
      if (!stats.inconsistent && !HasEmptyDomain(new_level)) {
        std::cout << " → continue search" << std::endl;
        // 递归搜索
        if (Search(new_level)) {
          return true;  // 找到解
        }
        std::cout << "[Backtrack] Level " << level << ": var[" << var
                  << "] = " << value << " (no solution in subtree)" << std::endl;
      } else {
        std::cout << " → prune (inconsistent or empty domain)" << std::endl;
      }

      // 回溯
      model_->BackToLevel(level);
      solution_[var] = -1;
    }

    return false;  // 当前分支失败
  }

  int SelectVariable(int level) {
    int best_var = -1;
    int min_size = model_->max_dom_size + 1;

    for (int var = 0; var < model_->num_vars; ++var) {
      if (solution_[var] != -1) continue;  // 已赋值

      int size = model_->GetDomainSize(var, level);
      if (size > 0 && size < min_size) {
        min_size = size;
        best_var = var;
      }
    }

    return best_var;
  }

  std::vector<int> GetDomain(int var, int level) {
    std::vector<int> domain;
    const int base_idx = level * model_->bit_doms_int_size +
                        var * model_->bit_dom_int_size;

    for (int value = 0; value < model_->max_dom_size; ++value) {
      const int word = value / 32;
      const int bit = value % 32;
      if (model_->bitDom[base_idx + word] & (1u << bit)) {
        domain.push_back(value);
      }
    }

    return domain;
  }

  bool HasEmptyDomain(int level) {
    for (int var = 0; var < model_->num_vars; ++var) {
      if (solution_[var] == -1 && model_->GetDomainSize(var, level) == 0) {
        return true;
      }
    }
    return false;
  }

  void RecordSolution(int level) {
    for (int var = 0; var < model_->num_vars; ++var) {
      if (solution_[var] == -1) {
        // 从 bitDom 读取单值域
        std::vector<int> domain = GetDomain(var, level);
        if (!domain.empty()) {
          solution_[var] = domain[0];
        }
      }
    }
  }

  GModel* model_;
  std::vector<int> solution_;
  int nodes_ = 0;
};

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <xcsp_file>" << std::endl;
    return 1;
  }

  try {
    // 解析和构建模型
    std::cout << "[Main] Parsing: " << argv[1] << std::endl;
    auto parser = XcspParser::Create(ParserType::kLibXml2);
    if (!parser) {
      std::cerr << "Failed to create parser" << std::endl;
      return 1;
    }

    auto model_or = parser->Parse(argv[1]);
    if (!model_or.ok()) {
      std::cerr << "Failed to parse: " << model_or.status() << std::endl;
      return 1;
    }

    std::cout << "[Main] Normalizing..." << std::endl;
    ModelNormalizer normalizer;
    auto normalized_or = normalizer.Normalize(*model_or);
    if (!normalized_or.ok()) {
      std::cerr << "Failed to normalize: " << normalized_or.status() << std::endl;
      return 1;
    }

    IntermediateModel im_model = std::move(*normalized_or);
    std::cout << "[Main] Variables: " << im_model.num_variables() << std::endl;
    std::cout << "[Main] Constraints: " << im_model.num_constraints() << std::endl;

    // 构建 GModel
    std::cout << "[Main] Building GModel..." << std::endl;
    GModelOptions options;
    options.enable_prefetch = false;
    GModel gmodel = GModelAdapter::Build(im_model, options);

    // 测试完整的 MAC 求解
    std::cout << "\n=== Starting MAC Solver ===" << std::endl;
    SimpleGModelSolver solver(&gmodel);

    bool solved = solver.Solve();

    if (solved) {
      std::cout << "\n=== Solution Found! ===" << std::endl;
      const auto& solution = solver.GetSolution();
      for (int var = 0; var < gmodel.num_vars; ++var) {
        std::cout << "  var[" << var << "] = " << solution[var] << std::endl;
      }
    } else {
      std::cout << "\n=== No Solution Found ===" << std::endl;
    }

    return solved ? 0 : 1;

  } catch (const std::exception& e) {
    std::cerr << "[Main] Error: " << e.what() << std::endl;
    return 1;
  }
}
