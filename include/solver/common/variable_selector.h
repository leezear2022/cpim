// 变量选择启发式接口
// 支持可插拔的变量选择策略（MIN_DOMAIN, DOM/DEG, DOM/DDEG 等）

#ifndef CPIM_SOLVER_COMMON_VARIABLE_SELECTOR_H_
#define CPIM_SOLVER_COMMON_VARIABLE_SELECTOR_H_

#include <vector>
#include <memory>
#include <limits>

namespace cpim {

// 变量选择器接口
// 子类实现不同的启发式策略
class VariableSelector {
 public:
  virtual ~VariableSelector() = default;

  // 选择下一个未赋值变量
  // 参数：
  //   solution: 当前部分赋值 (solution[var] == -1 表示未赋值)
  //   domain_sizes: 各变量的域大小
  // 返回：选中的变量索引，-1 表示所有变量已赋值
  virtual int SelectVariable(
      const std::vector<int>& solution,
      const std::vector<int>& domain_sizes) = 0;

  // 通知赋值事件（用于动态启发式）
  virtual void OnAssignment(int var, int value) {}

  // 通知回溯事件（用于动态启发式）
  virtual void OnBacktrack(int var) {}
};

// 最小域优先 (MinDomain)
// 选择域最小的未赋值变量
class MinDomainSelector : public VariableSelector {
 public:
  MinDomainSelector() = default;

  int SelectVariable(
      const std::vector<int>& solution,
      const std::vector<int>& domain_sizes) override;
};

// DOM/DEG 启发式
// 选择 (域大小 / 度数) 最小的变量
// 度数 = 包含该变量的约束数量（静态）
class DomOverDegSelector : public VariableSelector {
 public:
  // degrees[var] = 包含 var 的约束数量
  explicit DomOverDegSelector(const std::vector<int>& degrees)
      : degrees_(degrees) {}

  int SelectVariable(
      const std::vector<int>& solution,
      const std::vector<int>& domain_sizes) override;

 private:
  std::vector<int> degrees_;
};

// DOM/DDEG 启发式
// 选择 (域大小 / 动态度数) 最小的变量
// 动态度数 = 包含该变量且至少有一个其他未赋值变量的约束数量
class DomOverDDegSelector : public VariableSelector {
 public:
  // 参数：
  //   num_vars: 变量数量
  //   var_to_constraints: var_to_constraints[var] = 包含 var 的约束ID列表
  //   constraint_scopes: constraint_scopes[cid] = 约束 cid 的变量列表
  DomOverDDegSelector(
      int num_vars,
      const std::vector<std::vector<int>>& var_to_constraints,
      const std::vector<std::vector<int>>& constraint_scopes)
      : num_vars_(num_vars),
        var_to_constraints_(var_to_constraints),
        constraint_scopes_(constraint_scopes) {}

  int SelectVariable(
      const std::vector<int>& solution,
      const std::vector<int>& domain_sizes) override;

 private:
  int num_vars_;
  std::vector<std::vector<int>> var_to_constraints_;  // var → [约束ID]
  std::vector<std::vector<int>> constraint_scopes_;   // 约束ID → [scope变量]
};

// DOM/WDEG 启发式（加权度数 - 最强启发式之一）
// 选择 (域大小 / 加权度数) 最小的变量
// 加权度数 = 包含该变量的所有约束的权重之和
// 约束权重在导致失败时增加（学习机制）
//
// 参考: Boussemart et al. (2004) "Boosting Systematic Search by Weighting Constraints"
class DomOverWDegSelector : public VariableSelector {
 public:
  DomOverWDegSelector(
      int num_vars,
      int num_constraints,
      const std::vector<std::vector<int>>& var_to_constraints)
      : num_vars_(num_vars),
        num_constraints_(num_constraints),
        var_to_constraints_(var_to_constraints),
        constraint_weights_(num_constraints, 1.0),  // 初始权重为 1
        total_failures_(0) {}

  int SelectVariable(
      const std::vector<int>& solution,
      const std::vector<int>& domain_sizes) override;

  // 通知失败事件（约束权重增加）
  // failed_constraints: 导致失败的约束ID列表
  void OnFailure(const std::vector<int>& failed_constraints);

 private:
  int num_vars_;
  int num_constraints_;
  std::vector<std::vector<int>> var_to_constraints_;  // var → [约束ID]
  std::vector<double> constraint_weights_;            // 约束权重
  int total_failures_;                                // 总失败次数（用于统计）
};

// VSIDS 启发式（Variable State Independent Decaying Sum）
// 基于变量 activity 的启发式，源自 SAT 求解器（Chaff, MiniSat）
// 选择 activity 最高的变量（冲突时增加，定期衰减）
//
// 核心机制：
// 1. 每个变量维护一个 activity 分数
// 2. 冲突发生时，涉及的变量 activity 增加
// 3. 定期对所有 activity 进行衰减（乘以 decay_factor）
// 4. 选择 activity 最高的未赋值变量
//
// 参考: Moskewicz et al. (2001) "Chaff: Engineering an Efficient SAT Solver"
class VSIDSSelector : public VariableSelector {
 public:
  VSIDSSelector(
      int num_vars,
      const std::vector<std::vector<int>>& constraint_scopes,
      double decay_factor = 0.95)
      : num_vars_(num_vars),
        constraint_scopes_(constraint_scopes),
        activity_(num_vars, 0.0),
        activity_increment_(1.0),
        decay_factor_(decay_factor),
        total_conflicts_(0) {}

  int SelectVariable(
      const std::vector<int>& solution,
      const std::vector<int>& domain_sizes) override;

  // 通知失败事件（增加涉及变量的 activity）
  void OnFailure(const std::vector<int>& failed_constraints);

 private:
  void DecayActivities();
  void RescaleActivities();

  int num_vars_;
  std::vector<std::vector<int>> constraint_scopes_;  // 约束 → 变量列表
  std::vector<double> activity_;                     // 变量 activity 分数
  double activity_increment_;                        // 当前 activity 增量
  double decay_factor_;                              // 衰减因子（默认 0.95）
  int total_conflicts_;                              // 总冲突次数
};

}  // namespace cpim

#endif  // CPIM_SOLVER_COMMON_VARIABLE_SELECTOR_H_
