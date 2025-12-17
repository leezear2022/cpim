#pragma once

#include <string>
#include <vector>

#include "GModel.cuh"

namespace cpim {

// GPU 求解统计信息（扩展版本，包含 GAC 统计）
struct GpuSearchStatistics {
  int num_positive = 0;      // 正向赋值次数
  int num_negative = 0;      // 回溯次数
  int num_solutions = 0;     // 找到的解的数量
  int gac_iterations = 0;    // GAC 传播迭代次数
  int gac_deletions = 0;     // GAC 删除的值总数
  bool time_out = false;     // 是否超时
  bool unsolvable = false;   // 是否证明无解
  double solve_time = 0.0;   // 求解时间（秒）
  double gac_time = 0.0;     // GAC 总时间（秒）
};

// ============================================================================
// GModelSolver - 基于 GModel 的 GPU 约束求解器
//
// 使用 MAC (Maintaining Arc Consistency) 搜索策略：
// 1. 选择最小域变量（MRV 启发式）
// 2. 选择最小值（按顺序）
// 3. 赋值后执行 GPU GAC 传播
// 4. 失败时回溯并删除该值
// ============================================================================
class GModelSolver {
 public:
  // 构造函数
  explicit GModelSolver(GModel* model, bool verbose = false);

  // 求解问题
  // time_limit: 时间限制（毫秒），0 表示无限制
  // 返回求解统计信息
  GpuSearchStatistics Solve(int time_limit = 0);

  // 获取找到的解（如果有）
  // 返回变量赋值数组，索引为变量 ID，值为赋值值
  // 如果没有找到解，返回空数组
  std::vector<int> GetSolution() const;

  // 获取找到的所有解（如果 FindAllSolutions 模式）
  std::vector<std::vector<int>> GetAllSolutions() const;

  // 设置是否查找所有解（默认只查找第一个解）
  void SetFindAllSolutions(bool find_all);

  // 设置最大解的数量（默认 1）
  void SetMaxSolutions(int max_solutions);

 private:
  GModel* model_;                         // GModel 指针（不拥有）
  bool verbose_;                          // 是否打印详细信息
  bool find_all_solutions_;               // 是否查找所有解
  int max_solutions_;                     // 最大解的数量
  std::vector<std::vector<int>> solutions_;  // 找到的所有解

  // 递归搜索（DFS + MAC）
  // 返回 true 表示找到解或达到最大解数量
  bool Search(int level, GpuSearchStatistics& stats, int time_limit,
              double start_time);

  // 从当前状态提取解
  void ExtractSolution(int level);
};

}  // namespace cpim
