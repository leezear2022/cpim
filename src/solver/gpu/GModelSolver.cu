#include "GModelSolver.h"

#include <algorithm>
#include <deque>
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
  // 构建共享邻接表（CSR 格式），所有 SAC 函数共用
  neighbor_csr_ = NeighborCSR::Build(model_);
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

  // SAC 预处理（如果启用）
  if (sac1_preprocessing_) {
    int sac_result = 0;
    SACMode effective_mode = sac_mode_;
    if (effective_mode == SACMode::kAuto) {
      effective_mode = SACMode::kSAC1;  // 默认使用 SAC1
    }

    if (effective_mode == SACMode::kSAC3) {
      if (verbose_) {
        std::cout << "\n[Level 0] 执行 SAC3 预处理..." << std::endl;
      }
      sac_result = EnforceSAC3(stats);
    } else {
      if (verbose_) {
        std::cout << "\n[Level 0] 执行 SAC1 预处理..." << std::endl;
      }
      sac_result = EnforceSAC1(stats);
    }

    if (sac_result == -1) {
      if (verbose_) {
        std::cout << "[Level 0] SAC 检测到不一致，问题无解！" << std::endl;
      }
      stats.unsolvable = true;
      stats.solve_time = timer.elapsed() / 1000.0;
      return stats;
    }
    if (verbose_) {
      std::cout << "[Level 0] SAC 预处理完成，删除 " << sac_result << " 个值" << std::endl;
    }
  }

  // Phase 1.2: 移除 level 参数
  // 检查是否已经找到解（初始传播后所有变量都赋值）
  if (model_->IsFullyAssigned()) {
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

  // Phase 1.2: 移除 level 参数
  // 选择最小域变量
  const int var = model_->GetMinDomainVar();

  // 所有变量都已赋值 → 找到解
  if (var == -1) {
    if (model_->IsFullyAssigned()) {
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

  // Phase 1.2: 移除 level 参数，API 更名
  // 遍历该变量的所有可能值
  for (int value = model_->GetFirstValue(var); value != -1;
       value = model_->GetNextValue(var, value)) {
    if (verbose_) {
      std::cout << "\n[Level " << level << "] 尝试赋值 var[" << var << "] = " << value << std::endl;
    }

    // 创建新层级
    model_->NewLevel();
    const int new_level = model_->GetCurrentLevel();
    stats.num_positive++;

    // 赋值
    model_->AssignValue(var, value);

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
      // Phase 1.2: API 更名，回溯到进入本层之前
      model_->BacktrackTo(new_level - 1);
      stats.num_negative++;
      last_level_failures_++;  // 记录失败次数（用于 MSAC 触发判断）
      continue;  // 尝试下一个值
    }

    if (verbose_) {
      std::cout << "[Level " << new_level << "] ✓ GAC 传播成功" << std::endl;
    }

    // MSAC 集成：在 GAC 成功后、递归搜索前执行
    if (ShouldEnforceMSAC(new_level, var, stats)) {
      int msac_result = EnforceLightweightMSAC(var, stats);
      if (msac_result == -1) {
        // MSAC 检测到不一致，回溯
        if (verbose_) {
          std::cout << "[Level " << new_level << "] ✗ MSAC 检测到不一致，回溯" << std::endl;
        }
        model_->BacktrackTo(new_level - 1);
        stats.num_negative++;
        last_level_failures_++;
        continue;
      } else if (verbose_ && msac_result > 0) {
        std::cout << "[Level " << new_level << "] MSAC 删除了 " << msac_result << " 个值" << std::endl;
      }
    }

    // 更新上层统计（用于下层的 MSAC 触发判断）
    int prev_failures = last_level_failures_;
    int prev_positives = last_level_positives_;
    last_level_failures_ = 0;
    last_level_positives_ = 0;

    // 递归搜索
    const bool should_stop = Search(new_level, stats, time_limit, start_time);

    // 恢复上层统计
    last_level_failures_ = prev_failures;
    last_level_positives_ = prev_positives + 1;

    // Phase 1.2: API 更名，回溯到进入本层之前
    // 回溯到当前层级
    model_->BacktrackTo(new_level - 1);

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
    // Phase 1.2: 移除 level 参数
    const int value = model_->GetAssignedValue(var);
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

// ============================================================================
// NeighborCSR::Build - 构建 CSR 格式邻接表（实现在头文件声明的静态方法）
// ============================================================================
NeighborCSR NeighborCSR::Build(GModel* model) {
  NeighborCSR csr;
  const int num_vars = model->num_vars;
  const auto& scopes = model->constraint_scopes_cpu;

  // 临时邻接表
  std::vector<std::vector<int>> temp_neighbors(num_vars);
  for (size_t cid = 0; cid < scopes.size(); ++cid) {
    const auto& vars_in_constraint = scopes[cid];
    for (int v1 : vars_in_constraint) {
      for (int v2 : vars_in_constraint) {
        if (v1 != v2) {
          temp_neighbors[v1].push_back(v2);
        }
      }
    }
  }

  // 去重并排序
  for (auto& ns : temp_neighbors) {
    std::sort(ns.begin(), ns.end());
    ns.erase(std::unique(ns.begin(), ns.end()), ns.end());
  }

  // 构建 CSR offset 数组
  csr.offset.resize(num_vars + 1);
  csr.offset[0] = 0;
  for (int v = 0; v < num_vars; ++v) {
    csr.offset[v + 1] = csr.offset[v] + static_cast<int>(temp_neighbors[v].size());
  }

  // 填充 data 数组
  csr.data.resize(csr.offset[num_vars]);
  for (int v = 0; v < num_vars; ++v) {
    std::copy(temp_neighbors[v].begin(), temp_neighbors[v].end(),
              csr.data.begin() + csr.offset[v]);
  }

  return csr;
}

// ============================================================================
// SACDirtySet - 增量任务收集辅助类（使用共享 CSR 邻接表）
// ============================================================================
class SACDirtySet {
 public:
  SACDirtySet(GModel* model, const NeighborCSR& neighbor_csr)
      : model_(model), num_vars_(model->num_vars), neighbor_csr_(neighbor_csr) {
    dirty_.resize(num_vars_, false);
  }

  // 初始化所有变量为 dirty
  void InitAll() {
    std::fill(dirty_.begin(), dirty_.end(), true);
  }

  // 清除所有 dirty 标记
  void Clear() {
    std::fill(dirty_.begin(), dirty_.end(), false);
  }

  // 标记变量及其邻域为 dirty
  void MarkDirty(int var) {
    if (var < 0 || var >= num_vars_) return;
    dirty_[var] = true;
    // 使用 CSR 遍历邻居
    for (const int* p = neighbor_csr_.GetNeighborsBegin(var);
         p != neighbor_csr_.GetNeighborsEnd(var); ++p) {
      dirty_[*p] = true;
    }
  }

  // 检查变量是否需要 probe
  bool NeedsProbe(int var) const {
    return var >= 0 && var < num_vars_ && dirty_[var];
  }

  // 收集 dirty 变量的任务
  std::vector<ProbeTask> CollectTasks() const {
    std::vector<ProbeTask> tasks;
    for (int var = 0; var < num_vars_; ++var) {
      if (!dirty_[var]) continue;
      if (model_->IsAssigned(var)) continue;
      for (int val = model_->GetFirstValue(var); val != -1;
           val = model_->GetNextValue(var, val)) {
        tasks.emplace_back(var, val, static_cast<int>(tasks.size()));
      }
    }
    return tasks;
  }

  // 获取 dirty 变量数量
  int DirtyCount() const {
    int count = 0;
    for (bool d : dirty_) {
      if (d) count++;
    }
    return count;
  }

 private:
  GModel* model_;
  int num_vars_;
  std::vector<bool> dirty_;
  const NeighborCSR& neighbor_csr_;  // 共享 CSR 邻接表引用
};

// ============================================================================
// ProbeQueue - SAC3 队列管理类（probe 粒度）
// 优化：使用共享 CSR 邻接表 + 位打包 in_queue
// ============================================================================
class ProbeQueue {
 public:
  static constexpr int kBitsPerWord = 32;

  ProbeQueue(GModel* model, const NeighborCSR& neighbor_csr)
      : model_(model), num_vars_(model->num_vars), neighbor_csr_(neighbor_csr) {
    // 计算每变量需要的 word 数
    words_per_var_ = (model_->max_dom_size + kBitsPerWord - 1) / kBitsPerWord;
    // 分配位打包数组：num_vars * words_per_var
    in_queue_bits_.resize(num_vars_ * words_per_var_, 0);
  }

  // 检查值是否在域中（直接位域检查）
  bool HasValue(int var, int val) const {
    if (var < 0 || var >= num_vars_ || val < 0 || val >= model_->max_dom_size) {
      return false;
    }
    const int word_idx = val / kBitsPerWord;
    const int bit_idx = val % kBitsPerWord;
    const int base_idx = model_->GetBitDomIndex(var, 0);
    const u32* bitDom = model_->GetBitDom();
    return (bitDom[base_idx + word_idx] & (1u << bit_idx)) != 0;
  }

  // 入队所有 (var, val) 对（初始化时使用）
  void EnqueueAll() {
    for (int var = 0; var < num_vars_; ++var) {
      if (model_->IsAssigned(var)) continue;
      for (int val = model_->GetFirstValue(var); val != -1;
           val = model_->GetNextValue(var, val)) {
        Enqueue(var, val);
      }
    }
  }

  // 单值入队（带去重，使用位打包）
  bool Enqueue(int var, int val) {
    if (var < 0 || var >= num_vars_) return false;
    if (val < 0 || val >= model_->max_dom_size) return false;

    // 位打包检查和设置
    const int idx = var * words_per_var_ + val / kBitsPerWord;
    const uint32_t mask = 1u << (val % kBitsPerWord);
    if (in_queue_bits_[idx] & mask) return false;  // 已在队列中

    queue_.emplace_back(var, val);
    in_queue_bits_[idx] |= mask;
    return true;
  }

  // 入队邻域变量的所有值（删值后调用，使用 CSR 遍历）
  void EnqueueNeighborhood(int var) {
    if (var < 0 || var >= num_vars_) return;
    // 使用 CSR 遍历邻居
    for (const int* p = neighbor_csr_.GetNeighborsBegin(var);
         p != neighbor_csr_.GetNeighborsEnd(var); ++p) {
      const int neighbor = *p;
      if (model_->IsAssigned(neighbor)) continue;
      for (int val = model_->GetFirstValue(neighbor); val != -1;
           val = model_->GetNextValue(neighbor, val)) {
        Enqueue(neighbor, val);
      }
    }
  }

  // 批量出队（返回实际出队数量）
  int DequeueBatch(int max_count, std::vector<ProbeTask>& tasks) {
    tasks.clear();
    int count = 0;
    while (!queue_.empty() && count < max_count) {
      auto [var, val] = queue_.front();
      queue_.pop_front();

      // 位打包清除标记
      const int idx = var * words_per_var_ + val / kBitsPerWord;
      const uint32_t mask = 1u << (val % kBitsPerWord);
      in_queue_bits_[idx] &= ~mask;

      // 跳过已赋值的变量或已删除的值
      if (model_->IsAssigned(var)) continue;
      if (!HasValue(var, val)) continue;

      tasks.emplace_back(var, val, static_cast<int>(tasks.size()));
      count++;
    }
    return count;
  }

  // 队列是否为空
  bool Empty() const { return queue_.empty(); }

  // 队列大小
  size_t Size() const { return queue_.size(); }

  // 清空队列
  void Clear() {
    queue_.clear();
    std::fill(in_queue_bits_.begin(), in_queue_bits_.end(), 0);
  }

 private:
  GModel* model_;
  int num_vars_;
  int words_per_var_;                        // = (max_dom_size + 31) / 32
  std::deque<std::pair<int, int>> queue_;    // FIFO 队列
  std::vector<uint32_t> in_queue_bits_;      // 位打包：[num_vars * words_per_var]
  const NeighborCSR& neighbor_csr_;          // 共享 CSR 邻接表引用
};

// ============================================================================
// EnforceSAC1 - GPU SAC1 预处理（使用 Batch Probe 基础设施）
// ============================================================================
int GModelSolver::EnforceSAC1(GpuSearchStatistics& stats) {
  Timer sac_timer;
  int total_deletions = 0;
  int round = 0;
  unsigned long long total_precheck_short_circuits = 0;  // precheck 短路总数

  if (verbose_) {
    std::cout << "\n=== SAC1 预处理开始 ===" << std::endl;
  }

  // 使用构造时构建的共享邻接表（neighbor_csr_ 成员）

  // 创建 AutoStageSelector（用于首轮决策，后续使用缓存）
  AutoStageSelector stage_selector(model_, 32);

  // 创建 probe managers
  Batch2ProbeManager stage1_manager(model_, 64);
  stage1_manager.SetActivationStrategy(1);  // NEIGHBOR_ACTIVATION
  stage1_manager.EnableStats(true);
  stage1_manager.EnablePrecheck(true);  // 启用 cheap precheck

  Batch2PersistentManager stage2_manager(model_, -1);  // auto num_blocks
  stage2_manager.SetChunkSize(1);
  stage2_manager.EnablePrecheck(true);  // 启用 cheap precheck

  // 预分配任务容量（减少运行时重新分配）
  const int estimated_tasks = model_->num_vars * model_->max_dom_size;
  const int initial_capacity = std::max(2048, estimated_tasks);
  stage2_manager.ReserveTaskCapacity(initial_capacity);

  // 创建 Dirty Set（增量任务收集，使用共享 CSR）
  SACDirtySet dirty_set(model_, neighbor_csr_);
  dirty_set.InitAll();  // 首轮：所有变量都需要检查

  bool changed = true;
  int last_round_deletions = 0;  // 上一轮删除数（用于计算删值率）
  int last_round_probes = 0;     // 上一轮 probe 数
  bool early_stopped = false;    // 是否因早停而退出

  while (changed) {
    changed = false;
    round++;

    // ========== 早停检查 ==========
    if (sac_config_.early_stop_enabled) {
      // 检查时间预算
      double elapsed_ms = sac_timer.elapsed();
      if (elapsed_ms >= sac_config_.time_budget_ms) {
        if (verbose_) {
          std::cout << "[SAC1] 时间预算耗尽 (" << elapsed_ms
                    << "ms >= " << sac_config_.time_budget_ms << "ms)，停止"
                    << std::endl;
        }
        early_stopped = true;
        break;
      }

      // 检查最大轮次
      if (round > sac_config_.max_rounds) {
        if (verbose_) {
          std::cout << "[SAC1] 达到最大轮次 (" << sac_config_.max_rounds
                    << ")，停止" << std::endl;
        }
        early_stopped = true;
        break;
      }

      // 检查删值率（预热后）
      if (round > sac_config_.warmup_rounds && last_round_probes > 0) {
        double deletion_rate =
            static_cast<double>(last_round_deletions) / last_round_probes;
        if (deletion_rate < sac_config_.min_deletion_rate) {
          if (verbose_) {
            std::cout << "[SAC1] 删值率过低 (" << deletion_rate
                      << " < " << sac_config_.min_deletion_rate << ")，停止"
                      << std::endl;
          }
          early_stopped = true;
          break;
        }
      }
    }

    // 使用 Dirty Set 收集任务（只收集 dirty 变量的 probes）
    std::vector<ProbeTask> tasks = dirty_set.CollectTasks();

    if (tasks.empty()) break;

    // 决定使用哪个 Stage
    StageSelection stage;
    if (sac_stage_mode_ == StageSelection::kAuto) {
      auto decision = stage_selector.DecideCached(tasks);
      stage = decision.stage;
      stats.sac_stage = stage;
    } else {
      stage = sac_stage_mode_;
      stats.sac_stage = stage;
    }

    if (verbose_) {
      std::cout << "[SAC1 Round " << round << "] "
                << tasks.size() << " probes (dirty vars: " << dirty_set.DirtyCount()
                << "/" << model_->num_vars << "), using "
                << (stage == StageSelection::kStage1 ? "Stage 1" : "Stage 2")
                << std::endl;
    }

    // 执行 batch probe
    std::vector<int> failed_vars, failed_values;

    unsigned long long precheck_short_circuits = 0;
    if (stage == StageSelection::kStage1) {
      // 使用 Stage 1 (Micro-Batch)
      for (const auto& t : tasks) {
        stage1_manager.AddTask(t.var_id, t.value);
      }
      stage1_manager.ExecuteMicroBatch(failed_vars, failed_values);
      precheck_short_circuits = stage1_manager.GetLastPrecheckShortCircuitCount();
      stage1_manager.Clear();
    } else {
      // 使用 Stage 2 (Persistent Blocks)
      for (const auto& t : tasks) {
        stage2_manager.AddTask(t.var_id, t.value);
      }
      stage2_manager.ExecutePersistentBlocks(failed_vars, failed_values);
      precheck_short_circuits = stage2_manager.GetLastPrecheckShortCircuitCount();
      stage2_manager.Clear();
    }
    total_precheck_short_circuits += precheck_short_circuits;

    stats.sac_probes += static_cast<int>(tasks.size());

    // 记录本轮统计（用于早停判断）
    last_round_probes = static_cast<int>(tasks.size());
    last_round_deletions = static_cast<int>(failed_vars.size());

    // 处理失败的 probes（删除不一致的值）
    if (!failed_vars.empty()) {
      changed = true;

      // 清除 dirty set，准备标记受影响的变量
      dirty_set.Clear();

	      for (size_t i = 0; i < failed_vars.size(); ++i) {
	        const int var = failed_vars[i];
	        const int val = failed_values[i];

        // 从域中删除该值
        model_->RemoveValue(var, val);
        total_deletions++;

        // 标记变量及其邻域为 dirty（下轮需要重新检查）
        dirty_set.MarkDirty(var);

        if (verbose_) {
          std::cout << "  删除: var[" << var << "]=" << val << std::endl;
        }

        // 检查域是否为空（不一致）
        if (model_->GetDomainSize(var) == 0) {
          if (verbose_) {
            std::cout << "[SAC1] 检测到不一致！var[" << var << "] 域为空" << std::endl;
          }
          stats.sac_deletions = total_deletions;
          stats.sac_rounds = round;
          stats.sac_time = sac_timer.elapsed() / 1000.0;
          return -1;  // 不一致
        }
      }

      // 删除后需要重新执行 GAC
      GacStats gac_stats = model_->EnforceGAC(false);
      stats.gac_iterations += gac_stats.iterations;
      stats.gac_deletions += gac_stats.deletions;

      if (gac_stats.inconsistent) {
        if (verbose_) {
          std::cout << "[SAC1] GAC 传播后检测到不一致！" << std::endl;
        }
        stats.sac_deletions = total_deletions;
        stats.sac_rounds = round;
        stats.sac_time = sac_timer.elapsed() / 1000.0;
        return -1;  // 不一致
      }

      // GAC 删除的值是 SAC 删除的传播结果
      // 受影响的变量应该已被 MarkDirty() 覆盖（它标记了删除变量的邻域）
      // 这是 SAC3 风格的增量更新：只重新检查受影响的变量
      // NOTE: 如果出现正确性问题，可以回退到 dirty_set.InitAll()
    } else {
      // 没有删除，下轮不需要再检查任何变量
      dirty_set.Clear();
    }
  }

  stats.sac_deletions = total_deletions;
  stats.sac_rounds = round;
  stats.sac_time = sac_timer.elapsed() / 1000.0;

  if (verbose_) {
    std::cout << "=== SAC1 预处理完成" << (early_stopped ? "（早停）" : "") << " ===" << std::endl;
    std::cout << "  轮次: " << round << std::endl;
    std::cout << "  删除: " << total_deletions << std::endl;
    std::cout << "  Precheck 短路: " << total_precheck_short_circuits << std::endl;
    std::cout << "  时间: " << stats.sac_time << "s" << std::endl;
    std::cout << "  Stage: " << (stats.sac_stage == StageSelection::kStage1 ? "Stage 1" : "Stage 2") << std::endl;
  }

  return total_deletions;
}

// ============================================================================
// EnforceSAC3 - GPU SAC3 预处理（队列驱动，probe 粒度）
// 优化：无删值快路径 - 首轮线性扫描，仅在发生删值时切换到队列模式
// ============================================================================
int GModelSolver::EnforceSAC3(GpuSearchStatistics& stats) {
  Timer sac_timer;
  int total_deletions = 0;
  int batch_count = 0;
  unsigned long long total_precheck_short_circuits = 0;

  if (verbose_) {
    std::cout << "\n=== SAC3 预处理开始（无删值快路径优化）===" << std::endl;
  }

  // 使用构造时构建的共享邻接表（neighbor_csr_ 成员）

  // 创建 AutoStageSelector
  AutoStageSelector stage_selector(model_, 32);

  // 创建 probe managers（复用整个 SAC3 过程）
  Batch2ProbeManager stage1_manager(model_, 64);
  stage1_manager.SetActivationStrategy(1);  // NEIGHBOR_ACTIVATION
  stage1_manager.EnableStats(true);
  stage1_manager.EnablePrecheck(true);

  Batch2PersistentManager stage2_manager(model_, -1);
  stage2_manager.SetChunkSize(1);
  stage2_manager.EnablePrecheck(true);

  // 预分配任务容量
  const int max_batch_size = 1024;
  stage2_manager.ReserveTaskCapacity(max_batch_size);

  bool early_stopped = false;
  std::vector<ProbeTask> tasks;
  tasks.reserve(max_batch_size);
  std::vector<int> failed_vars, failed_values;

  UnifiedTrail* trail = model_->trail_;
  std::vector<char> gac_modified_flags;
  std::vector<int> gac_modified_vars;
  if (trail != nullptr) {
    gac_modified_flags.assign(model_->num_vars, 0);
    gac_modified_vars.reserve(64);
  }
  std::vector<char> failed_var_flags(model_->num_vars, 0);
  std::vector<int> failed_unique_vars;
  failed_unique_vars.reserve(64);

  // =========================================================================
  // 无删值快路径：延迟创建 ProbeQueue
  // Phase 1: 线性扫描模式（无队列/去重开销）
  // Phase 2: 仅当删值发生时切换到队列模式
  // =========================================================================
  ProbeQueue* probe_queue = nullptr;  // 延迟创建
  bool use_queue_mode = false;

  // 线性扫描状态
  int linear_var = 0;
  int linear_val = 0;

  // 内联检查值是否在域中（避免调用不存在的 GModel::HasValue）
  static constexpr int kBitsPerWord = 32;
  auto CheckHasValue = [&](int var, int val) -> bool {
    if (var < 0 || var >= model_->num_vars || val < 0 || val >= model_->max_dom_size) {
      return false;
    }
    const int word_idx = val / kBitsPerWord;
    const int bit_idx = val % kBitsPerWord;
    const int base_idx = model_->GetBitDomIndex(var, 0);
    const u32* bitDom = model_->GetBitDom();
    return (bitDom[base_idx + word_idx] & (1u << bit_idx)) != 0;
  };

  // 辅助 lambda：从线性扫描收集下一批任务
  auto CollectLinearBatch = [&]() -> bool {
    tasks.clear();
    while (tasks.size() < static_cast<size_t>(max_batch_size)) {
      // 找到下一个有效的 (var, val) 对
      while (linear_var < model_->num_vars) {
        // 跳过已赋值变量（单例变量无需 SAC probe）
        if (model_->IsAssigned(linear_var)) {
          linear_val = 0;
          linear_var++;
          continue;
        }
        if (linear_val < model_->max_dom_size &&
            CheckHasValue(linear_var, linear_val)) {
          tasks.push_back({linear_var, linear_val});
          linear_val++;
          break;  // 找到一个，继续外层循环
        }
        linear_val++;
        if (linear_val >= model_->max_dom_size) {
          linear_val = 0;
          linear_var++;
        }
      }
      if (linear_var >= model_->num_vars) {
        break;  // 扫描完成
      }
    }
    return !tasks.empty();
  };

  // 辅助 lambda：执行一批 probe 并返回是否有删值
  auto ExecuteBatch = [&](std::vector<int>& out_failed_vars,
                          std::vector<int>& out_failed_values) -> bool {
    out_failed_vars.clear();
    out_failed_values.clear();
    unsigned long long precheck_short_circuits = 0;

    // 决定使用哪个 Stage
    StageSelection stage;
    if (sac_stage_mode_ == StageSelection::kAuto) {
      auto decision = stage_selector.DecideCached(tasks);
      stage = decision.stage;
      stats.sac_stage = stage;
    } else {
      stage = sac_stage_mode_;
      stats.sac_stage = stage;
    }

    if (stage == StageSelection::kStage1) {
      for (const auto& t : tasks) {
        stage1_manager.AddTask(t.var_id, t.value);
      }
      stage1_manager.ExecuteMicroBatch(out_failed_vars, out_failed_values);
      precheck_short_circuits = stage1_manager.GetLastPrecheckShortCircuitCount();
      stage1_manager.Clear();
    } else {
      for (const auto& t : tasks) {
        stage2_manager.AddTask(t.var_id, t.value);
      }
      stage2_manager.ExecutePersistentBlocks(out_failed_vars, out_failed_values);
      precheck_short_circuits = stage2_manager.GetLastPrecheckShortCircuitCount();
      stage2_manager.Clear();
    }
    total_precheck_short_circuits += precheck_short_circuits;
    stats.sac_probes += static_cast<int>(tasks.size());

    return !out_failed_vars.empty();
  };

  // 辅助 lambda：处理删值并切换到队列模式
  auto HandleDeletionsAndSwitchToQueue = [&]() -> int {
    // 处理删值
    failed_unique_vars.clear();
    for (size_t i = 0; i < failed_vars.size(); ++i) {
      const int var = failed_vars[i];
      const int val = failed_values[i];

      model_->RemoveValue(var, val);
      total_deletions++;

      if (verbose_) {
        std::cout << "  删除: var[" << var << "]=" << val << std::endl;
      }

      if (model_->GetDomainSize(var) == 0) {
        if (verbose_) {
          std::cout << "[SAC3] 检测到不一致！var[" << var << "] 域为空" << std::endl;
        }
        return -1;  // 不一致
      }

      if (!failed_var_flags[var]) {
        failed_var_flags[var] = 1;
        failed_unique_vars.push_back(var);
      }
    }

    // 执行 GAC
    const int trail_size_before_gac = trail != nullptr ? trail->Size() : 0;
    GacStats gac_stats = model_->EnforceGAC(false);
    stats.gac_iterations += gac_stats.iterations;
    stats.gac_deletions += gac_stats.deletions;

    if (gac_stats.inconsistent) {
      if (verbose_) {
        std::cout << "[SAC3] GAC 传播后检测到不一致！" << std::endl;
      }
      return -1;
    }

    // 创建 ProbeQueue（延迟初始化，使用构造时的共享 CSR 成员）
    if (probe_queue == nullptr) {
      probe_queue = new ProbeQueue(model_, neighbor_csr_);
      if (verbose_) {
        std::cout << "[SAC3] 检测到删值，切换到队列模式" << std::endl;
      }
    }

    // 将删值变量的邻域入队
    for (int v : failed_unique_vars) {
      probe_queue->EnqueueNeighborhood(v);
      failed_var_flags[v] = 0;
    }

    // 追踪 GAC 级联删值
    if (gac_stats.deletions > 0 && trail != nullptr) {
      const int trail_size_after_gac = trail->Size();
      gac_modified_vars.clear();
      for (int i = trail_size_before_gac; i < trail_size_after_gac; ++i) {
        const int v = trail->GetEntry(i).var_id;
        if (v < 0 || v >= model_->num_vars) continue;
        if (gac_modified_flags[v]) continue;
        gac_modified_flags[v] = 1;
        gac_modified_vars.push_back(v);
      }
      for (int v : gac_modified_vars) {
        probe_queue->EnqueueNeighborhood(v);
        gac_modified_flags[v] = 0;
      }
    }

    // 将线性扫描剩余的 (var, val) 也入队
    // 注意：这些是尚未检查的值，需要在队列模式中继续处理
    for (int v = linear_var; v < model_->num_vars; ++v) {
      // 跳过已赋值变量（单例变量无需 SAC probe）
      if (model_->IsAssigned(v)) continue;
      int start_val = (v == linear_var) ? linear_val : 0;
      for (int val = start_val; val < model_->max_dom_size; ++val) {
        if (CheckHasValue(v, val)) {
          probe_queue->Enqueue(v, val);
        }
      }
    }

    use_queue_mode = true;
    return 0;  // 成功
  };

  if (verbose_) {
    int total_values = 0;
    for (int v = 0; v < model_->num_vars; ++v) {
      total_values += model_->GetDomainSize(v);
    }
    std::cout << "[SAC3] Phase 1: 线性扫描模式（无删值快路径），总值数: "
              << total_values << std::endl;
  }

  // =========================================================================
  // Phase 1: 线性扫描（无队列开销）
  // =========================================================================
  while (!use_queue_mode && linear_var < model_->num_vars) {
    batch_count++;

    // 早停检查
    if (sac_config_.early_stop_enabled) {
      double elapsed_ms = sac_timer.elapsed();
      if (elapsed_ms >= sac_config_.time_budget_ms) {
        if (verbose_) {
          std::cout << "[SAC3] 时间预算耗尽 (" << elapsed_ms
                    << "ms >= " << sac_config_.time_budget_ms << "ms)，停止"
                    << std::endl;
        }
        early_stopped = true;
        break;
      }
    }

    // 收集任务
    if (!CollectLinearBatch()) break;

    if (verbose_ && batch_count % 10 == 1) {
      std::cout << "[SAC3 Linear Batch " << batch_count << "] "
                << tasks.size() << " probes" << std::endl;
    }

    // 执行 batch probe
    bool has_deletions = ExecuteBatch(failed_vars, failed_values);

    if (has_deletions) {
      // 发生删值，切换到队列模式
      int result = HandleDeletionsAndSwitchToQueue();
      if (result < 0) {
        stats.sac_deletions = total_deletions;
        stats.sac_rounds = batch_count;
        stats.sac_time = sac_timer.elapsed() / 1000.0;
        return -1;
      }
      break;  // 退出线性扫描，进入队列模式
    }
  }

  // =========================================================================
  // Phase 2: 队列模式（仅当有删值时才会进入）
  // =========================================================================
  if (use_queue_mode && probe_queue) {
    if (verbose_) {
      std::cout << "[SAC3] Phase 2: 队列模式，队列大小: "
                << probe_queue->Size() << std::endl;
    }

    while (!probe_queue->Empty()) {
      batch_count++;

      // 早停检查
      if (sac_config_.early_stop_enabled) {
        double elapsed_ms = sac_timer.elapsed();
        if (elapsed_ms >= sac_config_.time_budget_ms) {
          if (verbose_) {
            std::cout << "[SAC3] 时间预算耗尽 (" << elapsed_ms
                      << "ms >= " << sac_config_.time_budget_ms << "ms)，停止"
                      << std::endl;
          }
          early_stopped = true;
          break;
        }
      }

      // 批量出队
      probe_queue->DequeueBatch(max_batch_size, tasks);
      if (tasks.empty()) continue;

      if (verbose_ && batch_count % 10 == 1) {
        std::cout << "[SAC3 Queue Batch " << batch_count << "] "
                  << tasks.size() << " probes, queue remaining: "
                  << probe_queue->Size() << std::endl;
      }

      // 执行 batch probe
      bool has_deletions = ExecuteBatch(failed_vars, failed_values);

      if (has_deletions) {
        // 处理删值
        failed_unique_vars.clear();
        for (size_t i = 0; i < failed_vars.size(); ++i) {
          const int var = failed_vars[i];
          const int val = failed_values[i];

          model_->RemoveValue(var, val);
          total_deletions++;

          if (verbose_) {
            std::cout << "  删除: var[" << var << "]=" << val << std::endl;
          }

          if (model_->GetDomainSize(var) == 0) {
            if (verbose_) {
              std::cout << "[SAC3] 检测到不一致！var[" << var << "] 域为空"
                        << std::endl;
            }
            delete probe_queue;  // 清理
            stats.sac_deletions = total_deletions;
            stats.sac_rounds = batch_count;
            stats.sac_time = sac_timer.elapsed() / 1000.0;
            return -1;
          }

          if (!failed_var_flags[var]) {
            failed_var_flags[var] = 1;
            failed_unique_vars.push_back(var);
          }
        }

        // 入队邻域
        for (int v : failed_unique_vars) {
          probe_queue->EnqueueNeighborhood(v);
          failed_var_flags[v] = 0;
        }

        // 执行 GAC
        const int trail_size_before_gac = trail != nullptr ? trail->Size() : 0;
        GacStats gac_stats = model_->EnforceGAC(false);
        stats.gac_iterations += gac_stats.iterations;
        stats.gac_deletions += gac_stats.deletions;

        if (gac_stats.inconsistent) {
          if (verbose_) {
            std::cout << "[SAC3] GAC 传播后检测到不一致！" << std::endl;
          }
          delete probe_queue;  // 清理
          stats.sac_deletions = total_deletions;
          stats.sac_rounds = batch_count;
          stats.sac_time = sac_timer.elapsed() / 1000.0;
          return -1;
        }

        // 追踪 GAC 级联删值
        if (gac_stats.deletions > 0 && trail != nullptr) {
          const int trail_size_after_gac = trail->Size();
          gac_modified_vars.clear();
          for (int i = trail_size_before_gac; i < trail_size_after_gac; ++i) {
            const int v = trail->GetEntry(i).var_id;
            if (v < 0 || v >= model_->num_vars) continue;
            if (gac_modified_flags[v]) continue;
            gac_modified_flags[v] = 1;
            gac_modified_vars.push_back(v);
          }
          for (int v : gac_modified_vars) {
            probe_queue->EnqueueNeighborhood(v);
            gac_modified_flags[v] = 0;
          }
        }
      }
    }
  }

  // 清理 ProbeQueue（如果已创建）
  if (probe_queue != nullptr) {
    delete probe_queue;
    probe_queue = nullptr;
  }

  stats.sac_deletions = total_deletions;
  stats.sac_rounds = batch_count;
  stats.sac_time = sac_timer.elapsed() / 1000.0;

  if (verbose_) {
    std::cout << "=== SAC3 预处理完成" << (early_stopped ? "（早停）" : "")
              << " ===" << std::endl;
    std::cout << "  批次数: " << batch_count << std::endl;
    std::cout << "  删除: " << total_deletions << std::endl;
    std::cout << "  模式: " << (use_queue_mode ? "队列模式" : "快路径（无队列）")
              << std::endl;
    std::cout << "  Precheck 短路: " << total_precheck_short_circuits << std::endl;
    std::cout << "  时间: " << stats.sac_time << "s" << std::endl;
  }

  return total_deletions;
}

// ============================================================================
// MSAC 辅助函数
// ============================================================================

bool GModelSolver::ShouldEnforceMSAC(int level, int var,
                                      const GpuSearchStatistics& stats) {
  if (!msac_config_.enabled) return false;

  // 条件 1: 只在前 N 层执行
  if (level > msac_config_.max_level) return false;

  // 条件 2: 当前变量域大小检查（如果太小，SAC 收益不大）
  // 注意：这里检查的是整体未赋值变量的平均域大小
  int total_domain = 0;
  int unassigned_count = 0;
  for (int v = 0; v < model_->num_vars; ++v) {
    if (!model_->IsAssigned(v)) {
      total_domain += model_->GetDomainSize(v);
      unassigned_count++;
    }
  }
  if (unassigned_count > 0) {
    double avg_domain = static_cast<double>(total_domain) / unassigned_count;
    if (avg_domain < msac_config_.min_domain_size) return false;
  }

  // 条件 3: 上层失败率检查
  if (last_level_positives_ > 0) {
    double fail_rate = static_cast<double>(last_level_failures_) / last_level_positives_;
    if (fail_rate < msac_config_.min_fail_rate) return false;
  }

  return true;
}

int GModelSolver::EnforceLightweightMSAC(int assigned_var,
                                          GpuSearchStatistics& stats) {
  // 轻量级 MSAC：只检查刚赋值变量的邻域
  Timer msac_timer;

  // 使用构造时构建的共享邻接表（neighbor_csr_ 成员）

  if (assigned_var < 0 || assigned_var >= model_->num_vars) {
    return 0;
  }

  // 收集邻域变量的 probe 任务（使用 CSR 遍历邻居）
  std::vector<ProbeTask> tasks;
  int task_count = 0;
  for (const int* p = neighbor_csr_.GetNeighborsBegin(assigned_var);
       p != neighbor_csr_.GetNeighborsEnd(assigned_var); ++p) {
    const int neighbor = *p;
    if (model_->IsAssigned(neighbor)) continue;
    for (int val = model_->GetFirstValue(neighbor); val != -1;
         val = model_->GetNextValue(neighbor, val)) {
      if (task_count >= msac_config_.max_probes_per_node) break;
      tasks.emplace_back(neighbor, val, task_count);
      task_count++;
    }
    if (task_count >= msac_config_.max_probes_per_node) break;
  }

  if (tasks.empty()) return 0;

  // 使用 Persistent Manager 执行 probes
  Batch2PersistentManager manager(model_, -1);
  manager.SetChunkSize(1);
  manager.EnablePrecheck(true);

  for (const auto& t : tasks) {
    manager.AddTask(t.var_id, t.value);
  }

  std::vector<int> failed_vars, failed_values;
  manager.ExecutePersistentBlocks(failed_vars, failed_values);

  stats.sac_probes += static_cast<int>(tasks.size());

  int deletions = 0;
  for (size_t i = 0; i < failed_vars.size(); ++i) {
    const int var = failed_vars[i];
    const int val = failed_values[i];

    model_->RemoveValue(var, val);
    deletions++;

    if (model_->GetDomainSize(var) == 0) {
      stats.sac_deletions += deletions;
      return -1;  // 不一致
    }
  }

  if (deletions > 0) {
    GacStats gac_stats = model_->EnforceGAC(false);
    stats.gac_iterations += gac_stats.iterations;
    stats.gac_deletions += gac_stats.deletions;

    if (gac_stats.inconsistent) {
      stats.sac_deletions += deletions;
      return -1;
    }
  }

  stats.sac_deletions += deletions;
  stats.sac_time += msac_timer.elapsed() / 1000.0;

  return deletions;
}

}  // namespace cpim
