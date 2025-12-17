#include "GModelSolver.h"

#include <iostream>
#include <stdexcept>

#include "Timer.h"

namespace cpim {

GModelSolver::GModelSolver(GModel* model, bool verbose)
    : model_(model),
      verbose_(verbose),
      find_all_solutions_(false),
      max_solutions_(1) {
  if (!model_) {
    throw std::runtime_error("[GModelSolver] model is null!");
  }
}

void GModelSolver::SetFindAllSolutions(bool find_all) {
  find_all_solutions_ = find_all;
  if (find_all) {
    max_solutions_ = 1000000;  // 默认最大解数量
  }
}

void GModelSolver::SetMaxSolutions(int max_solutions) {
  max_solutions_ = max_solutions;
}

GpuSearchStatistics GModelSolver::Solve(int time_limit) {
  Timer timer;
  GpuSearchStatistics stats;
  solutions_.clear();

  if (verbose_) {
    std::cout << "\n=== GModelSolver 开始求解 ===" << std::endl;
    std::cout << "变量数: " << model_->num_vars << std::endl;
    std::cout << "约束数: " << model_->num_constraints << std::endl;
    std::cout << "最大域大小: " << model_->max_dom_size << std::endl;
    std::cout << "时间限制: " << (time_limit > 0 ? std::to_string(time_limit) + "ms" : "无限制") << std::endl;
  }

  // 初始 GAC 传播
  if (verbose_) {
    std::cout << "\n[Level 0] 执行初始 GAC 传播..." << std::endl;
  }

  Timer gac_timer;
  GacStats init_gac = model_->EnforceGAC(verbose_);
  stats.gac_time += gac_timer.elapsed() / 1000.0;
  stats.gac_iterations += init_gac.iterations;
  stats.gac_deletions += init_gac.deletions;

  if (init_gac.inconsistent) {
    if (verbose_) {
      std::cout << "[Level 0] 初始 GAC 传播失败，问题无解！" << std::endl;
    }
    stats.unsolvable = true;
    stats.solve_time = timer.elapsed() / 1000.0;
    return stats;
  }

  if (verbose_) {
    std::cout << "[Level 0] 初始 GAC 传播成功" << std::endl;
    std::cout << "  迭代次数: " << init_gac.iterations << std::endl;
    std::cout << "  删除值数: " << init_gac.deletions << std::endl;
  }

  // 检查是否已经找到解（初始传播后所有变量都赋值）
  if (model_->IsFullyAssigned(0)) {
    if (verbose_) {
      std::cout << "\n[Level 0] 初始传播后已找到解！" << std::endl;
    }
    ExtractSolution(0);
    stats.num_solutions = 1;
    stats.solve_time = timer.elapsed() / 1000.0;
    return stats;
  }

  // 递归搜索
  const double start_time = timer.elapsed() / 1000.0;
  Search(0, stats, time_limit, start_time);

  stats.solve_time = timer.elapsed() / 1000.0;
  stats.num_solutions = static_cast<int>(solutions_.size());

  if (verbose_) {
    std::cout << "\n=== GModelSolver 求解完成 ===" << std::endl;
    std::cout << "求解时间: " << stats.solve_time << "s" << std::endl;
    std::cout << "GAC 时间: " << stats.gac_time << "s" << std::endl;
    std::cout << "正向节点: " << stats.num_positive << std::endl;
    std::cout << "回溯节点: " << stats.num_negative << std::endl;
    std::cout << "GAC 迭代: " << stats.gac_iterations << std::endl;
    std::cout << "GAC 删除: " << stats.gac_deletions << std::endl;
    std::cout << "找到解数: " << stats.num_solutions << std::endl;
    std::cout << "超时: " << (stats.time_out ? "是" : "否") << std::endl;
    std::cout << "无解: " << (stats.unsolvable ? "是" : "否") << std::endl;
  }

  return stats;
}

bool GModelSolver::Search(int level, GpuSearchStatistics& stats, int time_limit,
                          double start_time) {
  // 超时检查
  if (time_limit > 0) {
    Timer temp_timer;
    const double elapsed = temp_timer.elapsed() / 1000.0 - start_time;
    if (elapsed * 1000 >= time_limit) {
      stats.time_out = true;
      return true;  // 停止搜索
    }
  }

  // 选择最小域变量
  const int var = model_->GetMinDomainVar(level);

  // 所有变量都已赋值 → 找到解
  if (var == -1) {
    if (model_->IsFullyAssigned(level)) {
      if (verbose_) {
        std::cout << "\n[Level " << level << "] ✓ 找到解！" << std::endl;
      }
      ExtractSolution(level);

      // 如果只需要一个解，停止搜索
      if (!find_all_solutions_ || solutions_.size() >= static_cast<size_t>(max_solutions_)) {
        return true;
      }
      return false;  // 继续搜索其他解
    } else {
      // 没有可赋值的变量，但不是完整解 → 失败
      if (verbose_) {
        std::cout << "\n[Level " << level << "] ✗ 无可赋值变量但未完全赋值" << std::endl;
      }
      return false;
    }
  }

  // 遍历该变量的所有可能值
  for (int value = model_->GetFirstValue(var, level); value != -1;
       value = model_->GetNextValue(var, value, level)) {
    if (verbose_) {
      std::cout << "\n[Level " << level << "] 尝试赋值 var[" << var << "] = " << value << std::endl;
    }

    // 创建新层级
    const int new_level = model_->CreateNewLevel();
    stats.num_positive++;

    // 赋值
    model_->AssignValue(var, value, new_level);

    // GAC 传播
    Timer gac_timer;
    GacStats gac_stats = model_->EnforceGAC(false);  // 不打印详细信息
    stats.gac_time += gac_timer.elapsed() / 1000.0;
    stats.gac_iterations += gac_stats.iterations;
    stats.gac_deletions += gac_stats.deletions;

    if (gac_stats.inconsistent) {
      // 传播失败 → 回溯
      if (verbose_) {
        std::cout << "[Level " << new_level << "] ✗ GAC 传播失败，回溯" << std::endl;
      }
      model_->BackToLevel(level);
      stats.num_negative++;
      continue;  // 尝试下一个值
    }

    if (verbose_) {
      std::cout << "[Level " << new_level << "] ✓ GAC 传播成功" << std::endl;
    }

    // 递归搜索
    const bool should_stop = Search(new_level, stats, time_limit, start_time);

    // 回溯到当前层级
    model_->BackToLevel(level);

    if (should_stop) {
      return true;  // 找到解或超时
    }
  }

  // 所有值都尝试过了，没有找到解
  return false;
}

void GModelSolver::ExtractSolution(int level) {
  std::vector<int> solution(model_->num_vars);
  for (int var = 0; var < model_->num_vars; ++var) {
    const int value = model_->GetAssignedValue(var, level);
    if (value == -1) {
      throw std::runtime_error(
          "[GModelSolver::ExtractSolution] Variable " + std::to_string(var) +
          " is not assigned at level " + std::to_string(level));
    }
    solution[var] = value;
  }
  solutions_.push_back(solution);
}

std::vector<int> GModelSolver::GetSolution() const {
  if (solutions_.empty()) {
    return {};
  }
  return solutions_[0];
}

std::vector<std::vector<int>> GModelSolver::GetAllSolutions() const {
  return solutions_;
}

}  // namespace cpim
