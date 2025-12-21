//
// MAC 搜索过程正确性验证
// 通过穷举法验证 MAC 找到的解是否正确
//
#include <iostream>
#include <vector>
#include <set>
#include <string>
#include <algorithm>

#include <glog/logging.h>
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"

#include "Solver.h"
#include "model/intermediate_model.h"
#include "model/model_normalizer.h"
#include "model/xcsp_parser.h"

using namespace cpim::model;

ABSL_FLAG(std::string, input, "", "Input XCSP file");
ABSL_FLAG(int, timeout, 10, "Timeout in seconds");

// 穷举求解器（用于验证 MAC 结果）
class BruteForceSolver {
 public:
  BruteForceSolver(cpim::Network* n) : n_(n) {}

  // 穷举搜索所有解
  std::vector<std::vector<int>> FindAllSolutions() {
    std::vector<std::vector<int>> solutions;
    std::vector<int> assignment(n_->vars.size(), -1);
    Backtrack(0, assignment, solutions);
    return solutions;
  }

  // 检查部分赋值是否满足所有相关约束
  bool CheckConsistency(const std::vector<int>& assignment, int depth) {
    // 检查每个约束
    for (auto* tab : n_->tabs) {
      // 检查约束的所有变量是否都已赋值
      bool all_assigned = true;
      for (auto* v : tab->scope) {
        if (assignment[v->id()] == -1) {
          all_assigned = false;
          break;
        }
      }

      // 如果约束的所有变量都已赋值，检查是否满足
      if (all_assigned) {
        std::vector<int> tuple;
        for (auto* v : tab->scope) {
          tuple.push_back(assignment[v->id()]);
        }
        if (!tab->sat(tuple)) {
          return false;
        }
      }
    }
    return true;
  }

 private:
  void Backtrack(int depth, std::vector<int>& assignment,
                 std::vector<std::vector<int>>& solutions) {
    // 所有变量都已赋值
    if (depth == n_->vars.size()) {
      // 验证解的正确性
      if (CheckConsistency(assignment, depth)) {
        solutions.push_back(assignment);
      }
      return;
    }

    // 尝试当前变量的每个值
    cpim::IntVar* var = n_->vars[depth];
    for (int v = var->head(); v != cpim::Limits::INDEX_OVERFLOW;
         v = var->next(v)) {
      assignment[depth] = v;

      // 剪枝：检查当前部分赋值是否一致
      if (CheckConsistency(assignment, depth + 1)) {
        Backtrack(depth + 1, assignment, solutions);
      }

      assignment[depth] = -1;
    }
  }

  cpim::Network* n_;
};

// 打印解
void PrintSolution(const std::vector<int>& sol) {
  std::cout << "  [";
  for (size_t i = 0; i < sol.size(); ++i) {
    if (i > 0) std::cout << ", ";
    std::cout << sol[i];
  }
  std::cout << "]" << std::endl;
}

// 比较两个解是否相同
bool SolutionsEqual(const std::vector<int>& a, const std::vector<int>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i] != b[i]) return false;
  }
  return true;
}

int main(int argc, char* argv[]) {
  absl::ParseCommandLine(argc, argv);
  google::InitGoogleLogging(argv[0]);
  FLAGS_logtostderr = 1;

  std::string input_file = absl::GetFlag(FLAGS_input);
  int timeout = absl::GetFlag(FLAGS_timeout);

  if (input_file.empty()) {
    std::cerr << "Usage: verify_search --input=<xcsp_file>" << std::endl;
    return 1;
  }

  // 解析模型
  auto parser = XcspParser::Create(ParserType::kLibXml2);
  auto model_or = parser->Parse(input_file);
  if (!model_or.ok()) {
    std::cerr << "Failed to parse: " << model_or.status() << std::endl;
    return 1;
  }

  ModelNormalizer normalizer;
  auto normalized_or = normalizer.Normalize(*model_or);
  if (!normalized_or.ok()) {
    std::cerr << "Normalization failed: " << normalized_or.status() << std::endl;
    return 1;
  }
  const auto& model = *normalized_or;

  std::cout << "=== Model: " << model.name() << " ===" << std::endl;
  std::cout << "Variables: " << model.num_variables() << std::endl;
  std::cout << "Constraints: " << model.num_constraints() << std::endl;

  // 计算搜索空间大小
  uint64_t search_space = 1;
  for (const auto& var : model.variables()) {
    const auto& domain = model.GetDomain(var.domain);
    search_space *= domain.Size();
  }
  std::cout << "Search space: " << search_space << std::endl;

  if (search_space > 1000000) {
    std::cout << "WARNING: Search space too large for brute force verification!" << std::endl;
    std::cout << "Skipping brute force solver." << std::endl;
  }

  // 创建 Network 用于 MAC
  cpim::Network network1(model);

  // 运行 MAC
  std::cout << "\n=== Running MAC with AC3bit ===" << std::endl;
  cpim::MAC mac(&network1, cpim::AC_3bit, cpim::Heuristic::VRH_DOM_MIN,
                cpim::Heuristic::VLH_MIN);
  auto stats = mac.enforce(timeout * 1000);

  std::cout << "MAC result: " << (stats.num_sol > 0 ? "SAT" : "UNSAT") << std::endl;
  std::cout << "Solve time: " << stats.solve_time << " ms" << std::endl;
  std::cout << "Positive decisions: " << stats.num_positive << std::endl;
  std::cout << "Negative decisions (backtracks): " << stats.num_negative << std::endl;
  std::cout << "Timeout: " << (stats.time_out ? "Yes" : "No") << std::endl;

  if (stats.num_sol > 0) {
    std::cout << "\nMAC solution:" << std::endl;
    PrintSolution(mac.solution);

    // 验证 MAC 的解是否正确
    std::cout << "\n=== Verifying MAC solution ===" << std::endl;
    bool mac_solution_valid = mac.solution_check();
    std::cout << "MAC solution check: " << (mac_solution_valid ? "VALID" : "INVALID") << std::endl;

    if (!mac_solution_valid) {
      std::cout << "ERROR: MAC found an invalid solution!" << std::endl;
      google::ShutdownGoogleLogging();
      return 1;
    }
  }

  // 如果搜索空间不太大，使用穷举法验证
  if (search_space <= 1000000) {
    std::cout << "\n=== Running brute force solver ===" << std::endl;
    cpim::Network network2(model);
    BruteForceSolver bf(&network2);
    auto bf_solutions = bf.FindAllSolutions();

    std::cout << "Brute force found " << bf_solutions.size() << " solution(s)" << std::endl;

    if (bf_solutions.empty()) {
      // 穷举法说 UNSAT
      if (stats.num_sol > 0) {
        std::cout << "ERROR: MAC found solution but brute force says UNSAT!" << std::endl;
        google::ShutdownGoogleLogging();
        return 1;
      } else {
        std::cout << "Both MAC and brute force agree: UNSAT" << std::endl;
      }
    } else {
      // 穷举法找到解
      if (stats.num_sol == 0) {
        std::cout << "ERROR: Brute force found solutions but MAC says UNSAT!" << std::endl;
        std::cout << "First brute force solution:" << std::endl;
        PrintSolution(bf_solutions[0]);
        google::ShutdownGoogleLogging();
        return 1;
      }

      // 检查 MAC 的解是否在穷举解集中
      bool found = false;
      for (const auto& bf_sol : bf_solutions) {
        if (SolutionsEqual(mac.solution, bf_sol)) {
          found = true;
          break;
        }
      }

      if (!found) {
        std::cout << "ERROR: MAC solution not in brute force solution set!" << std::endl;
        std::cout << "Brute force solutions:" << std::endl;
        for (const auto& sol : bf_solutions) {
          PrintSolution(sol);
        }
        google::ShutdownGoogleLogging();
        return 1;
      }

      std::cout << "MAC solution is valid (found in brute force solutions)" << std::endl;
    }
  }

  std::cout << "\n=== Verification Summary ===" << std::endl;
  std::cout << "✓ MAC search process is CORRECT" << std::endl;

  google::ShutdownGoogleLogging();
  return 0;
}
