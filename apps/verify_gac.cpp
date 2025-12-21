//
// GAC 正确性验证程序
// 通过穷举法验证 AC3bit 的域缩减是否正确
//
#include <iostream>
#include <vector>
#include <set>
#include <string>

#include <glog/logging.h>
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"

#include "Solver.h"
#include "model/intermediate_model.h"
#include "model/model_normalizer.h"
#include "model/xcsp_parser.h"

using namespace cpim::model;

ABSL_FLAG(std::string, input, "", "Input XCSP file");

// 正确计算 GAC 域（作为参考基准）
// GAC 定义：对于变量 x 的值 a，它是 GAC 一致的当且仅当
// 对于每个包含 x 的约束 C，存在一个满足 C 的支持元组 t，
// 其中 t[x]=a 且 t 中其他变量的值都在各自的当前域中。
class CorrectGAC {
 public:
  CorrectGAC(cpim::Network* n) : n_(n) {}

  // 检查 (var_id, value) 是否 GAC 一致
  // 正确的 GAC 检查：对每个包含该变量的约束，检查是否有支持
  bool IsGACConsistent(int var_id, int value) {
    cpim::IntVar* var = n_->vars[var_id];

    // 检查每个包含 var 的约束
    for (cpim::Tabular* tab : n_->subscription[var]) {
      if (!HasSupport(tab, var_id, value)) {
        return false;
      }
    }
    return true;
  }

  // 检查约束 tab 中是否存在支持 (var_id, value) 的元组
  bool HasSupport(cpim::Tabular* tab, int var_id, int value) {
    // 找到 var 在约束中的位置
    int var_pos = -1;
    for (size_t i = 0; i < tab->scope.size(); ++i) {
      if (tab->scope[i]->id() == var_id) {
        var_pos = i;
        break;
      }
    }
    if (var_pos < 0) return true;  // 变量不在约束中

    // 在约束的元组中查找支持
    for (const auto& tuple : tab->tuples()) {
      // 检查元组是否满足：
      // 1. 元组中 var 的值等于给定的 value
      if (tuple[var_pos] != value) continue;

      // 2. 元组中其他变量的值都在当前域中
      bool tuple_valid = true;
      for (size_t i = 0; i < tab->scope.size(); ++i) {
        if (static_cast<int>(i) == var_pos) continue;
        int other_value = tuple[i];
        if (!tab->scope[i]->have(other_value)) {
          tuple_valid = false;
          break;
        }
      }

      if (tuple_valid) {
        return true;  // 找到支持
      }
    }

    return false;  // 没有支持
  }

  // 计算所有变量的 GAC 域（迭代直到不动点）
  std::vector<std::set<int>> ComputeGACDomains() {
    std::vector<std::set<int>> gac_domains(n_->vars.size());

    // 初始化域
    for (size_t var_id = 0; var_id < n_->vars.size(); ++var_id) {
      cpim::IntVar* var = n_->vars[var_id];
      for (int v = var->head(); v != cpim::Limits::INDEX_OVERFLOW;
           v = var->next(v)) {
        gac_domains[var_id].insert(v);
      }
    }

    // 迭代直到不动点
    bool changed = true;
    int iterations = 0;
    while (changed) {
      changed = false;
      ++iterations;

      for (size_t var_id = 0; var_id < n_->vars.size(); ++var_id) {
        std::set<int> new_domain;
        for (int v : gac_domains[var_id]) {
          if (IsGACConsistentWithDomains(var_id, v, gac_domains)) {
            new_domain.insert(v);
          } else {
            changed = true;
          }
        }
        gac_domains[var_id] = new_domain;
      }
    }

    std::cout << "  GAC fixed point reached after " << iterations
              << " iterations" << std::endl;
    return gac_domains;
  }

  // 使用给定的域检查 GAC 一致性
  bool IsGACConsistentWithDomains(int var_id, int value,
      const std::vector<std::set<int>>& domains) {
    cpim::IntVar* var = n_->vars[var_id];

    for (cpim::Tabular* tab : n_->subscription[var]) {
      if (!HasSupportWithDomains(tab, var_id, value, domains)) {
        return false;
      }
    }
    return true;
  }

  bool HasSupportWithDomains(cpim::Tabular* tab, int var_id, int value,
      const std::vector<std::set<int>>& domains) {
    int var_pos = -1;
    for (size_t i = 0; i < tab->scope.size(); ++i) {
      if (tab->scope[i]->id() == var_id) {
        var_pos = i;
        break;
      }
    }
    if (var_pos < 0) return true;

    for (const auto& tuple : tab->tuples()) {
      if (tuple[var_pos] != value) continue;

      bool tuple_valid = true;
      for (size_t i = 0; i < tab->scope.size(); ++i) {
        if (static_cast<int>(i) == var_pos) continue;
        int other_var_id = tab->scope[i]->id();
        int other_value = tuple[i];
        if (domains[other_var_id].find(other_value) == domains[other_var_id].end()) {
          tuple_valid = false;
          break;
        }
      }

      if (tuple_valid) {
        return true;
      }
    }

    return false;
  }

 private:
  cpim::Network* n_;
};

// 打印域
void PrintDomain(cpim::IntVar* var, int level) {
  std::cout << "  V" << var->id() << ": {";
  bool first = true;
  for (int v = var->head(); v != cpim::Limits::INDEX_OVERFLOW; v = var->next(v)) {
    if (!first) std::cout << ", ";
    std::cout << v;
    first = false;
  }
  std::cout << "} (size=" << var->size() << ")" << std::endl;
}

int main(int argc, char* argv[]) {
  absl::ParseCommandLine(argc, argv);
  google::InitGoogleLogging(argv[0]);
  FLAGS_logtostderr = 1;

  std::string input_file = absl::GetFlag(FLAGS_input);
  if (input_file.empty()) {
    std::cerr << "Usage: verify_gac --input=<xcsp_file>" << std::endl;
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

  // 创建 Network
  cpim::Network network(model);

  std::cout << "\n=== Initial Domains ===" << std::endl;
  for (auto* var : network.vars) {
    PrintDomain(var, 0);
  }

  // 运行 AC3bit
  std::cout << "\n=== Running AC3bit ===" << std::endl;
  cpim::AC3bit ac(&network);
  auto result = ac.enforce(network.vars, 0);
  std::cout << "AC3bit result: " << (result.state ? "consistent" : "inconsistent") << std::endl;
  std::cout << "Deletions: " << result.num_delete << std::endl;

  std::cout << "\n=== Domains after AC3bit ===" << std::endl;
  std::vector<std::set<int>> ac3bit_domains(network.vars.size());
  for (auto* var : network.vars) {
    PrintDomain(var, 0);
    for (int v = var->head(); v != cpim::Limits::INDEX_OVERFLOW; v = var->next(v)) {
      ac3bit_domains[var->id()].insert(v);
    }
  }

  // 使用正确的 GAC 算法计算 GAC 域
  std::cout << "\n=== Computing Correct GAC (iterative fixpoint) ===" << std::endl;

  // 重新创建 Network（恢复原始域）
  cpim::Network network2(model);
  CorrectGAC correct_gac(&network2);
  auto bf_domains = correct_gac.ComputeGACDomains();

  std::cout << "\n=== Correct GAC Domains ===" << std::endl;
  for (size_t i = 0; i < network2.vars.size(); ++i) {
    std::cout << "  V" << i << ": {";
    bool first = true;
    for (int v : bf_domains[i]) {
      if (!first) std::cout << ", ";
      std::cout << v;
      first = false;
    }
    std::cout << "} (size=" << bf_domains[i].size() << ")" << std::endl;
  }

  // 检查正确 GAC 是否检测到 UNSAT（所有域为空或任一域为空）
  bool correct_gac_unsat = false;
  for (const auto& dom : bf_domains) {
    if (dom.empty()) {
      correct_gac_unsat = true;
      break;
    }
  }

  // 比较两个结果
  std::cout << "\n=== Comparison ===" << std::endl;

  // 特殊处理：UNSAT 情况
  if (correct_gac_unsat) {
    std::cout << "Correct GAC detected UNSAT (some domain became empty)" << std::endl;
    if (!result.state) {
      std::cout << "AC3bit also detected inconsistency - CORRECT!" << std::endl;
      std::cout << "Note: AC3bit stops early when detecting inconsistency," << std::endl;
      std::cout << "      so intermediate domains may differ from fully propagated GAC." << std::endl;
      google::ShutdownGoogleLogging();
      return 0;  // 成功：两者都检测到 UNSAT
    } else {
      std::cout << "ERROR: AC3bit reported consistent but problem is UNSAT!" << std::endl;
      google::ShutdownGoogleLogging();
      return 1;
    }
  }

  // SAT 情况：比较域
  bool all_match = true;
  for (size_t i = 0; i < network.vars.size(); ++i) {
    if (ac3bit_domains[i] != bf_domains[i]) {
      all_match = false;
      std::cout << "MISMATCH at V" << i << ":" << std::endl;

      // AC3bit 多剪的值
      for (int v : bf_domains[i]) {
        if (ac3bit_domains[i].find(v) == ac3bit_domains[i].end()) {
          std::cout << "  AC3bit OVER-PRUNED: " << v << " (should be in domain)" << std::endl;
        }
      }

      // AC3bit 少剪的值
      for (int v : ac3bit_domains[i]) {
        if (bf_domains[i].find(v) == bf_domains[i].end()) {
          std::cout << "  AC3bit UNDER-PRUNED: " << v << " (should NOT be in domain)" << std::endl;
        }
      }
    }
  }

  if (all_match) {
    std::cout << "All domains match! AC3bit is CORRECT." << std::endl;
  } else {
    std::cout << "Domains DO NOT match! AC3bit has BUG." << std::endl;
  }

  google::ShutdownGoogleLogging();
  return all_match ? 0 : 1;
}
