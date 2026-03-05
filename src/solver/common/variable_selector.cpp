#include "solver/common/variable_selector.h"

namespace cpim {

// ============================================================================
// MinDomainSelector 实现
// ============================================================================

int MinDomainSelector::SelectVariable(
    const std::vector<int>& solution,
    const std::vector<int>& domain_sizes) {
  int best_var = -1;
  int min_size = std::numeric_limits<int>::max();

  for (int var = 0; var < static_cast<int>(solution.size()); ++var) {
    if (solution[var] != -1) continue;  // 已赋值

    int size = domain_sizes[var];
    if (size > 0 && size < min_size) {
      min_size = size;
      best_var = var;
    }
  }

  return best_var;
}

// ============================================================================
// DomOverDegSelector 实现
// ============================================================================

int DomOverDegSelector::SelectVariable(
    const std::vector<int>& solution,
    const std::vector<int>& domain_sizes) {
  int best_var = -1;
  double best_score = std::numeric_limits<double>::max();

  for (int var = 0; var < static_cast<int>(solution.size()); ++var) {
    if (solution[var] != -1) continue;  // 已赋值

    int dom_size = domain_sizes[var];
    if (dom_size == 0) continue;  // 域为空，跳过

    // 度数（避免除零）
    int degree = degrees_[var];
    if (degree == 0) degree = 1;

    // 计算得分：域大小 / 度数（越小越好）
    double score = static_cast<double>(dom_size) / degree;

    if (score < best_score) {
      best_score = score;
      best_var = var;
    }
  }

  return best_var;
}

// ============================================================================
// DomOverDDegSelector 实现
// ============================================================================

int DomOverDDegSelector::SelectVariable(
    const std::vector<int>& solution,
    const std::vector<int>& domain_sizes) {
  // 计算所有未赋值变量的动态度数
  std::vector<int> ddeg(num_vars_, 0);

  for (int var = 0; var < num_vars_; ++var) {
    if (solution[var] != -1) continue;  // 已赋值变量跳过

    // 遍历包含 var 的所有约束
    for (int cid : var_to_constraints_[var]) {
      // 检查约束 cid 中是否还有其他未赋值变量
      bool has_other_unassigned = false;
      for (int scope_var : constraint_scopes_[cid]) {
        if (scope_var != var && solution[scope_var] == -1) {
          has_other_unassigned = true;
          break;
        }
      }

      // 如果约束中还有其他未赋值变量，则计入动态度数
      if (has_other_unassigned) {
        ddeg[var]++;
      }
    }
  }

  // 选择 DOM/DDEG 最小的变量
  int best_var = -1;
  double best_score = std::numeric_limits<double>::max();

  for (int var = 0; var < num_vars_; ++var) {
    if (solution[var] != -1) continue;  // 已赋值

    int dom_size = domain_sizes[var];
    if (dom_size == 0) continue;  // 域为空

    // 动态度数（避免除零）
    int degree = ddeg[var];
    if (degree == 0) degree = 1;

    // 计算得分：域大小 / 动态度数（越小越好）
    double score = static_cast<double>(dom_size) / degree;

    if (score < best_score) {
      best_score = score;
      best_var = var;
    }
  }

  return best_var;
}

// ============================================================================
// DomOverWDegSelector 实现
// ============================================================================

int DomOverWDegSelector::SelectVariable(
    const std::vector<int>& solution,
    const std::vector<int>& domain_sizes) {
  int best_var = -1;
  double best_score = std::numeric_limits<double>::max();

  for (int var = 0; var < num_vars_; ++var) {
    if (solution[var] != -1) continue;  // 已赋值

    int dom_size = domain_sizes[var];
    if (dom_size == 0) continue;  // 域为空

    // 计算加权度数：变量相关的所有约束的权重之和
    double weighted_degree = 0.0;
    for (int cid : var_to_constraints_[var]) {
      weighted_degree += constraint_weights_[cid];
    }

    // 避免除零
    if (weighted_degree < 0.001) {
      weighted_degree = 1.0;
    }

    // 计算得分：域大小 / 加权度数（越小越好）
    double score = static_cast<double>(dom_size) / weighted_degree;

    if (score < best_score) {
      best_score = score;
      best_var = var;
    }
  }

  return best_var;
}

void DomOverWDegSelector::OnFailure(const std::vector<int>& failed_constraints) {
  // 增加导致失败的约束的权重
  for (int cid : failed_constraints) {
    if (cid >= 0 && cid < num_constraints_) {
      constraint_weights_[cid] += 1.0;
    }
  }
  total_failures_++;
}

// ============================================================================
// VSIDSSelector 实现
// ============================================================================

int VSIDSSelector::SelectVariable(
    const std::vector<int>& solution,
    const std::vector<int>& domain_sizes) {
  int best_var = -1;
  double best_activity = -1.0;
  int best_dom_size = std::numeric_limits<int>::max();

  for (int var = 0; var < num_vars_; ++var) {
    if (solution[var] != -1) continue;  // 已赋值

    int dom_size = domain_sizes[var];
    if (dom_size == 0) continue;  // 域为空

    // 选择 activity 最高的变量
    // 平局时选择域最小的（tie-breaking）
    if (activity_[var] > best_activity ||
        (activity_[var] == best_activity && dom_size < best_dom_size)) {
      best_activity = activity_[var];
      best_var = var;
      best_dom_size = dom_size;
    }
  }

  return best_var;
}

void VSIDSSelector::OnFailure(const std::vector<int>& failed_constraints) {
  // Bump activity: 增加失败约束涉及的所有变量的 activity
  for (int cid : failed_constraints) {
    if (cid >= 0 && cid < static_cast<int>(constraint_scopes_.size())) {
      for (int var : constraint_scopes_[cid]) {
        if (var >= 0 && var < num_vars_) {
          activity_[var] += activity_increment_;
        }
      }
    }
  }

  total_conflicts_++;

  // 定期衰减所有 activity（每 256 次冲突）
  if (total_conflicts_ % 256 == 0) {
    DecayActivities();
  }

  // Increment 增长（避免早期冲突主导）
  activity_increment_ /= decay_factor_;

  // 防止浮点溢出，重新归一化
  if (activity_increment_ > 1e100) {
    RescaleActivities();
  }
}

void VSIDSSelector::DecayActivities() {
  for (double& act : activity_) {
    act *= decay_factor_;
  }
}

void VSIDSSelector::RescaleActivities() {
  // 重新归一化，避免浮点溢出
  const double rescale_factor = 1e-100;
  for (double& act : activity_) {
    act *= rescale_factor;
  }
  activity_increment_ *= rescale_factor;
}

}  // namespace cpim
