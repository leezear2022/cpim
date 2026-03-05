// 简单的 GModel MAC 求解器
#include <iostream>
#include <vector>
#include <cstring>
#include <memory>
#include "GModel.cuh"
#include "model/xcsp_parser.h"
#include "model/model_normalizer.h"
#include "model/gmodel_adapter.h"
#include "solver/common/variable_selector.h"

using namespace cpim;
using namespace cpim::model;

// 简单的 MAC 搜索
class SimpleGModelSolver {
 public:
  explicit SimpleGModelSolver(GModel* model, bool use_persistent = false,
                              std::unique_ptr<VariableSelector> selector = nullptr)
      : model_(model), use_persistent_(use_persistent),
        selector_(selector ? std::move(selector)
                           : std::make_unique<MinDomainSelector>()) {}

  bool Solve() {
    std::cout << "\n=== Starting MAC Search ===" << std::endl;

    // 初始化 solution_
    solution_.assign(model_->num_vars, -1);

    // 初始 GAC
    GacStats init_stats = use_persistent_
        ? model_->EnforceGAC_Persistent(false)
        : model_->EnforceGAC(false);
    std::cout << "[Solver] Initial GAC: " << init_stats.deletions << " deletions, "
              << init_stats.iterations << " iterations"
              << (use_persistent_ ? " (Persistent)" : "") << std::endl;

    if (init_stats.inconsistent || HasEmptyDomain(0)) {
      std::cout << "[Solver] Problem is inconsistent!" << std::endl;
      return false;
    }

    // 打印域大小（调试）
    std::cout << "[Solver] Domain sizes after initial GAC:" << std::endl;
    for (int var = 0; var < model_->num_vars; ++var) {
      int size = model_->GetDomainSize(var);
      std::cout << "  var[" << var << "]: " << size << std::endl;
    }

    std::cout << "[Solver] Starting search from level 0..." << std::endl;
    return Search(0);
  }

  const std::vector<int>& GetSolution() const { return solution_; }
  int GetPositives() const { return positives_; }
  int GetNegatives() const { return negatives_; }
  int GetNodes() const { return nodes_; }

 private:
  bool Search(int level) {
    // 选择未赋值变量（简单启发式：最小域）
    int var = SelectVariable(level);
    if (var == -1) {
      // 所有变量已赋值 → 找到解
      std::cout << "\n[Solver] Solution found at level " << level << "!" << std::endl;
      RecordSolution(level);
      return true;
    }

    // 使用无限循环尝试当前变量的所有值
    while (true) {
      // 选择第一个值（最小值优先）
      int value = GetFirstValue(var);
      if (value == -1) {
        return false;  // 域为空，回溯到上一层
      }

      ++nodes_;
      std::cout << "[Try] Level " << level << ": var[" << var
                << "] = " << value << std::endl;

      // Phase 1.2: 创建新层级
      model_->NewLevel();
      ++positives_;  // 统计正向节点

      // Phase 1.2: 赋值
      model_->AssignValue(var, value);
      solution_[var] = value;

      // 传播
      GacStats stats = use_persistent_
          ? model_->EnforceGAC_Persistent(false, var)
          : model_->EnforceGAC(false, var);
      std::cout << "  [GAC] deletions=" << stats.deletions
                << ", inconsistent=" << (stats.inconsistent ? "true" : "false");

      // 检查一致性
      bool consistent = !stats.inconsistent && !HasEmptyDomain(level);

      if (consistent) {
        std::cout << " → continue search" << std::endl;
        // 递归搜索下一层
        if (Search(level + 1)) {
          return true;  // 找到解
        }
        // 回溯失败，继续尝试当前层的下一个值
        std::cout << "[Backtrack] Level " << level << ": var[" << var
                  << "] = " << value << std::endl;
      } else {
        std::cout << " → prune" << std::endl;
      }

      // Backtrack 处理（模拟 CPU 的内层 while 循环）
      ++negatives_;  // 统计回溯节点
      model_->BacktrackTo(level - 1);
      solution_[var] = -1;

      // Phase 1.5: 通知失败事件（WDEG / VSIDS）
      if (auto* wdeg = dynamic_cast<DomOverWDegSelector*>(selector_.get())) {
        if (var < static_cast<int>(model_->var_to_constraints.size())) {
          wdeg->OnFailure(model_->var_to_constraints[var]);
        }
      } else if (auto* vsids = dynamic_cast<VSIDSSelector*>(selector_.get())) {
        if (var < static_cast<int>(model_->var_to_constraints.size())) {
          vsids->OnFailure(model_->var_to_constraints[var]);
        }
      }

      // 移除失败的值
      model_->RemoveValue(var, value);

      // 重新传播
      GacStats remove_stats = use_persistent_
          ? model_->EnforceGAC_Persistent(false, var)
          : model_->EnforceGAC(false, var);

      // 检查是否可以继续尝试同层的其他值
      if (remove_stats.inconsistent) {
        // Phase 1.5: re-propagate 也失败，记录失败（WDEG / VSIDS）
        if (auto* wdeg = dynamic_cast<DomOverWDegSelector*>(selector_.get())) {
          if (var < static_cast<int>(model_->var_to_constraints.size())) {
            wdeg->OnFailure(model_->var_to_constraints[var]);
          }
        } else if (auto* vsids = dynamic_cast<VSIDSSelector*>(selector_.get())) {
          if (var < static_cast<int>(model_->var_to_constraints.size())) {
            vsids->OnFailure(model_->var_to_constraints[var]);
          }
        }
        return false;  // re-propagate 失败，回溯到上一层
      }
      // 如果 re-propagate 成功，继续 while 循环尝试下一个值
    }
  }

  int SelectVariable(int level) {
    // 准备 domain_sizes 数组
    std::vector<int> domain_sizes(model_->num_vars);
    for (int var = 0; var < model_->num_vars; ++var) {
      domain_sizes[var] = model_->GetDomainSize(var);
    }

    // 调试：在第一次选择时打印所有变量的分数（仅前5个）
    if (level == 0 && model_->num_vars > 1) {
      std::cout << "\n[DEBUG] 第一次选择变量时的分数计算：" << std::endl;
      for (int var = 0; var < std::min(5, model_->num_vars); ++var) {
        if (solution_[var] != -1) continue;

        int dom_size = domain_sizes[var];
        int degree = var < static_cast<int>(model_->var_degrees.size())
                     ? model_->var_degrees[var] : 0;

        double score_deg = degree > 0 ? static_cast<double>(dom_size) / degree : 999999.0;

        std::cout << "  var[" << var << "]: dom=" << dom_size
                  << ", deg=" << degree
                  << ", score(dom/deg)=" << score_deg << std::endl;
      }
    }

    // 使用可插拔的 VariableSelector
    int selected = selector_->SelectVariable(solution_, domain_sizes);

    // 调试：打印选中的变量
    if (level == 0 && selected != -1) {
      int dom_size = domain_sizes[selected];
      int degree = selected < static_cast<int>(model_->var_degrees.size())
                   ? model_->var_degrees[selected] : 0;
      std::cout << "  → 选中 var[" << selected << "], dom=" << dom_size
                << ", deg=" << degree << std::endl;
    }

    return selected;
  }

  int GetFirstValue(int var) {
    // Phase 1.2: 单层域架构
    const int base_idx = var * model_->bit_dom_int_size;

    for (int value = 0; value < model_->max_dom_size; ++value) {
      const int word = value / 32;
      const int bit = value % 32;
      if (model_->bitDom[base_idx + word] & (1u << bit)) {
        return value;  // 返回第一个（最小）值
      }
    }

    return -1;  // 域为空
  }

  bool HasEmptyDomain(int level) {
    for (int var = 0; var < model_->num_vars; ++var) {
      if (solution_[var] == -1 && model_->GetDomainSize(var) == 0) {
        return true;
      }
    }
    return false;
  }

  void RecordSolution(int level) {
    for (int var = 0; var < model_->num_vars; ++var) {
      if (solution_[var] == -1) {
        // 从 bitDom 读取单值域
        int value = GetFirstValue(var);
        if (value != -1) {
          solution_[var] = value;
        }
      }
    }
  }

  GModel* model_;
  std::vector<int> solution_;
  int nodes_ = 0;
  int positives_ = 0;  // 正向节点数（赋值尝试）
  int negatives_ = 0;  // 回溯节点数（失败的赋值）
  bool use_persistent_ = false;
  std::unique_ptr<VariableSelector> selector_;  // Phase 1.5: 可插拔启发式
};

// 启发式类型枚举
enum class Heuristic {
  MIN_DOMAIN,
  DOM_DEG,
  DOM_DDEG,
  DOM_WDEG,
  VSIDS
};

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <xcsp_file> [--persistent] [--heuristic=min_domain|dom_deg|dom_ddeg|dom_wdeg|vsids]" << std::endl;
    return 1;
  }

  // 解析命令行选项
  bool use_persistent = false;
  Heuristic heuristic = Heuristic::MIN_DOMAIN;
  const char* xcsp_file = argv[1];

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--persistent") == 0) {
      use_persistent = true;
    } else if (std::strncmp(argv[i], "--heuristic=", 12) == 0) {
      std::string h = argv[i] + 12;
      if (h == "dom_deg") {
        heuristic = Heuristic::DOM_DEG;
      } else if (h == "dom_ddeg") {
        heuristic = Heuristic::DOM_DDEG;
      } else if (h == "dom_wdeg") {
        heuristic = Heuristic::DOM_WDEG;
      } else if (h == "vsids") {
        heuristic = Heuristic::VSIDS;
      } else if (h == "min_domain") {
        heuristic = Heuristic::MIN_DOMAIN;
      } else {
        std::cerr << "Unknown heuristic: " << h << std::endl;
        return 1;
      }
    } else if (argv[i][0] != '-') {
      xcsp_file = argv[i];
    }
  }

  try {
    // 解析和构建模型
    std::cout << "[Main] Parsing: " << xcsp_file << std::endl;
    if (use_persistent) {
      std::cout << "[Main] Using Persistent Kernel (Cooperative Groups)" << std::endl;
    }
    auto parser = XcspParser::Create(ParserType::kLibXml2);
    if (!parser) {
      std::cerr << "Failed to create parser" << std::endl;
      return 1;
    }

    auto model_or = parser->Parse(xcsp_file);
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

    // 打印度数统计信息
    std::cout << "\n=== 变量度数分析 ===" << std::endl;
    int min_deg = 999999, max_deg = 0;
    double avg_deg = 0;
    for (int d : gmodel.var_degrees) {
      min_deg = std::min(min_deg, d);
      max_deg = std::max(max_deg, d);
      avg_deg += d;
    }
    avg_deg /= gmodel.num_vars;
    std::cout << "  度数范围: [" << min_deg << ", " << max_deg << "]" << std::endl;
    std::cout << "  平均度数: " << avg_deg << std::endl;
    std::cout << "  度数差异: " << (max_deg - min_deg) << std::endl;

    // 打印前几个变量的度数
    std::cout << "  前10个变量度数: ";
    for (int i = 0; i < std::min(10, gmodel.num_vars); ++i) {
      std::cout << gmodel.var_degrees[i] << " ";
    }
    std::cout << std::endl;

    // 调试：验证 var_to_constraints 是否正确
    std::cout << "\n=== 验证 var_to_constraints ===" << std::endl;
    for (int var = 0; var < std::min(3, gmodel.num_vars); ++var) {
      std::cout << "  var[" << var << "]: degree=" << gmodel.var_degrees[var]
                << ", constraints=[";
      for (int cid : gmodel.var_to_constraints[var]) {
        std::cout << cid << " ";
      }
      std::cout << "]" << std::endl;

      // 验证度数是否匹配约束数量
      if (static_cast<int>(gmodel.var_to_constraints[var].size()) != gmodel.var_degrees[var]) {
        std::cout << "    ⚠️ 警告：度数不匹配！var_to_constraints.size()="
                  << gmodel.var_to_constraints[var].size()
                  << " 但 var_degrees=" << gmodel.var_degrees[var] << std::endl;
      }
    }

    // 创建对应的 VariableSelector
    std::cout << "\n=== Starting MAC Solver ===" << std::endl;
    std::unique_ptr<VariableSelector> selector;

    switch (heuristic) {
      case Heuristic::MIN_DOMAIN:
        std::cout << "[Main] Using heuristic: MIN_DOMAIN" << std::endl;
        selector = std::make_unique<MinDomainSelector>();
        break;

      case Heuristic::DOM_DEG:
        std::cout << "[Main] Using heuristic: DOM/DEG" << std::endl;
        selector = std::make_unique<DomOverDegSelector>(gmodel.var_degrees);
        break;

      case Heuristic::DOM_DDEG:
        std::cout << "[Main] Using heuristic: DOM/DDEG" << std::endl;
        selector = std::make_unique<DomOverDDegSelector>(
            gmodel.num_vars, gmodel.var_to_constraints, gmodel.constraint_scopes_cpu);
        break;

      case Heuristic::DOM_WDEG:
        std::cout << "[Main] Using heuristic: DOM/WDEG (Weighted Degree)" << std::endl;
        selector = std::make_unique<DomOverWDegSelector>(
            gmodel.num_vars, gmodel.num_constraints, gmodel.var_to_constraints);
        break;

      case Heuristic::VSIDS:
        std::cout << "[Main] Using heuristic: VSIDS (Variable State Independent Decaying Sum)" << std::endl;
        selector = std::make_unique<VSIDSSelector>(
            gmodel.num_vars, gmodel.constraint_scopes_cpu);
        break;
    }

    SimpleGModelSolver solver(&gmodel, use_persistent, std::move(selector));

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

    // 输出统计信息（与 CPU 求解器格式一致）
    std::cout << "\n=== MAC Statistics ===" << std::endl;
    std::cout << "Positives: " << solver.GetPositives() << std::endl;
    std::cout << "Negatives: " << solver.GetNegatives() << std::endl;
    std::cout << "Nodes: " << solver.GetNodes() << std::endl;

    return solved ? 0 : 1;

  } catch (const std::exception& e) {
    std::cerr << "[Main] Error: " << e.what() << std::endl;
    return 1;
  }
}
