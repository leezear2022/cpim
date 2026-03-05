#include "GModelSolver.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <unordered_map>

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
  const auto deadline =
      (time_limit > 0)
          ? (std::chrono::steady_clock::now() + std::chrono::milliseconds(time_limit))
          : std::chrono::steady_clock::time_point::max();
  Search(0, stats, time_limit, deadline);

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
                          std::chrono::steady_clock::time_point deadline) {
  // 超时检查
  if (time_limit > 0 && std::chrono::steady_clock::now() >= deadline) {
    stats.time_out = true;
    return true;  // 停止搜索
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
    // 增量 GAC：只激活刚赋值变量的邻接约束，减少搜索阶段传播开销
    GacStats gac_stats = model_->EnforceGAC(false, var);  // 不打印详细信息
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
    const bool should_stop = Search(new_level, stats, time_limit, deadline);

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

  struct BudgetStats {
    int dropped_by_queue_cap = 0;  // max_queue_size 丢弃数
    int requeue_skipped = 0;       // 重入队上限导致的跳过数（按变量）
    int total_requeues = 0;        // 已发生的重入队次数（按变量事件计数）
  };

  ProbeQueue(GModel* model,
             const NeighborCSR& neighbor_csr,
             const GModelSolver::FailurePriorityConfig& config,
             const GModelSolver::SacQueueBudgetConfig& budget_config,
             const std::vector<int>* var_probe_count,
             const std::vector<int>* var_dwo_count)
      : model_(model),
        num_vars_(model->num_vars),
        neighbor_csr_(neighbor_csr),
        config_(config),
        budget_config_(budget_config),
        var_probe_count_(var_probe_count),
        var_dwo_count_(var_dwo_count) {
    // 计算每变量需要的 word 数
    words_per_var_ = (model_->max_dom_size + kBitsPerWord - 1) / kBitsPerWord;
    // 分配位打包数组：num_vars * words_per_var
    in_queue_bits_.resize(num_vars_ * words_per_var_, 0);
    requeue_count_.assign(num_vars_, 0);

    if (config_.enabled) {
      num_buckets_ = std::clamp(config_.num_buckets, 1, 16);
      buckets_.resize(num_buckets_);

      // 预计算 max_degree（用于归一化）
      max_degree_ = 1;
      for (int v = 0; v < num_vars_; ++v) {
        max_degree_ = std::max(max_degree_, neighbor_csr_.GetDegree(v));
      }
    }
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

    if (budget_config_.enabled && budget_config_.max_queue_size > 0 &&
        static_cast<int>(size_) >= budget_config_.max_queue_size) {
      budget_stats_.dropped_by_queue_cap++;
      return false;
    }

    if (config_.enabled) {
      const int bucket = ComputeBucket(var);
      buckets_[bucket].emplace_back(var, val);
    } else {
      queue_.emplace_back(var, val);
    }
    in_queue_bits_[idx] |= mask;
    size_++;
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

      // P1-2: 重入队预算（以“邻域事件”为粒度，避免 requeue 爆炸）
      if (budget_config_.enabled) {
        if (budget_config_.max_total_requeues > 0 &&
            budget_stats_.total_requeues >= budget_config_.max_total_requeues) {
          // 达到总重入队上限：停止继续扩张（sound but incomplete）
          return;
        }
        if (budget_config_.max_requeues_per_var > 0 &&
            requeue_count_[neighbor] >= budget_config_.max_requeues_per_var) {
          budget_stats_.requeue_skipped++;
          continue;
        }
        requeue_count_[neighbor]++;
        budget_stats_.total_requeues++;
      }

      for (int val = model_->GetFirstValue(neighbor); val != -1;
           val = model_->GetNextValue(neighbor, val)) {
        Enqueue(neighbor, val);
      }
    }
  }

  // 批量出队（返回实际出队数量）
  // bucket_out：若非空，则为每个出队 task 记录其 bucket（仅对 regular queue 生效；deferred task 在上层标记为 -1）
  int DequeueBatch(int max_count,
                   std::vector<ProbeTask>& tasks,
                   std::vector<int8_t>* bucket_out = nullptr) {
    tasks.clear();
    if (bucket_out != nullptr) bucket_out->clear();
    int count = 0;
    while (!Empty() && count < max_count) {
      int var = -1;
      int val = -1;
      int bucket = 0;
      if (config_.enabled) {
        bucket = PopHighestBucket(&var, &val);
        if (var < 0 || val < 0) break;
      } else {
        auto [v, a] = queue_.front();
        queue_.pop_front();
        var = v;
        val = a;
      }
      size_--;

      // 位打包清除标记
      const int idx = var * words_per_var_ + val / kBitsPerWord;
      const uint32_t mask = 1u << (val % kBitsPerWord);
      in_queue_bits_[idx] &= ~mask;

      // 跳过已赋值的变量或已删除的值
      if (model_->IsAssigned(var)) continue;
      if (!HasValue(var, val)) continue;

      tasks.emplace_back(var, val, static_cast<int>(tasks.size()));
      if (bucket_out != nullptr) {
        bucket_out->push_back(static_cast<int8_t>(bucket));
      }
      count++;
    }
    return count;
  }

  // 队列是否为空
  bool Empty() const { return size_ == 0; }

  // 队列大小
  size_t Size() const { return size_; }

  const BudgetStats& GetBudgetStats() const { return budget_stats_; }

  // 清空队列
  void Clear() {
    queue_.clear();
    for (auto& q : buckets_) q.clear();
    std::fill(in_queue_bits_.begin(), in_queue_bits_.end(), 0);
    std::fill(requeue_count_.begin(), requeue_count_.end(), 0);
    size_ = 0;
  }

 private:
  int ComputeBucket(int var) const {
    if (!config_.enabled || num_buckets_ <= 1) return 0;

    const int dom_size = model_->GetDomainSize(var);
    const int degree = neighbor_csr_.GetDegree(var);

    float dom_term = 0.0f;
    if (model_->max_dom_size > 0) {
      dom_term = static_cast<float>(model_->max_dom_size - dom_size) /
                 static_cast<float>(model_->max_dom_size);
    }

    const float deg_term = static_cast<float>(degree) /
                           static_cast<float>(std::max(1, max_degree_));

    float hist_term = 0.0f;
    if (var_probe_count_ != nullptr && var_dwo_count_ != nullptr &&
        var >= 0 && var < static_cast<int>(var_probe_count_->size()) &&
        var < static_cast<int>(var_dwo_count_->size())) {
      const int probes = (*var_probe_count_)[var];
      const int dwo = (*var_dwo_count_)[var];
      if (probes >= std::max(1, config_.min_hist_probes)) {
        hist_term = static_cast<float>(dwo) / static_cast<float>(probes);
      }
    }

    const float denom = config_.w_dom + config_.w_deg + config_.w_hist;
    float score = 0.0f;
    if (denom > 0.0f) {
      score = (config_.w_dom * dom_term +
               config_.w_deg * deg_term +
               config_.w_hist * hist_term) / denom;
    }
    score = std::clamp(score, 0.0f, 0.999999f);

    int bucket = static_cast<int>(score * static_cast<float>(num_buckets_));
    bucket = std::clamp(bucket, 0, num_buckets_ - 1);
    return bucket;
  }

  int PopHighestBucket(int* var, int* val) {
    for (int b = num_buckets_ - 1; b >= 0; --b) {
      auto& q = buckets_[b];
      if (!q.empty()) {
        auto [v, a] = q.front();
        q.pop_front();
        *var = v;
        *val = a;
        return b;
      }
    }
    // 防御性：理论上不会发生（Empty() 已检查）
    *var = -1;
    *val = -1;
    return 0;
  }

  GModel* model_;
  int num_vars_;
  int words_per_var_;                        // = (max_dom_size + 31) / 32
  std::deque<std::pair<int, int>> queue_;    // FIFO 队列
  std::vector<std::deque<std::pair<int, int>>> buckets_;  // bucketed queues
  std::vector<uint32_t> in_queue_bits_;      // 位打包：[num_vars * words_per_var]
  const NeighborCSR& neighbor_csr_;          // 共享 CSR 邻接表引用

  size_t size_ = 0;
  GModelSolver::FailurePriorityConfig config_;
  GModelSolver::SacQueueBudgetConfig budget_config_;  // P1-2: 外层队列预算（拷贝）
  std::vector<int> requeue_count_;                    // [num_vars] 重入队次数（按变量）
  BudgetStats budget_stats_;
  const std::vector<int>* var_probe_count_ = nullptr;
  const std::vector<int>* var_dwo_count_ = nullptr;
  int num_buckets_ = 1;
  int max_degree_ = 1;
};

// ============================================================================
// DeferredProbeQueue - UNKNOWN probes 的延后复查队列（P0-1c）
// 语义：UNKNOWN 不删值；仅当“邻域发生删值变化”后才重跑该 probe。
// ============================================================================
class DeferredProbeQueue {
 public:
  static constexpr int kBitsPerWord = 32;

  struct DeferredProbe {
    int var_id = -1;
    int value = -1;
    int nb_epoch_snapshot = 0;  // V1：邻域 epoch 快照（低开销、允许少量 false positive）
    int retry = 0;              // 已重检次数
    int enqueue_round = 0;      // 入队轮次（用于超期清理）
  };

  struct Stats {
    int deferred_in = 0;
    int deferred_out = 0;
    int deferred_hit = 0;       // 重检后 DWO 数
    int deferred_stale = 0;     // 超期/失效丢弃数
    int deferred_overflow = 0;  // 队列溢出丢弃数

    void Reset() {
      deferred_in = 0;
      deferred_out = 0;
      deferred_hit = 0;
      deferred_stale = 0;
      deferred_overflow = 0;
    }
  };

  DeferredProbeQueue(GModel* model,
                     const GModelSolver::DeferredRecheckConfig& config)
      : model_(model), config_(config) {
    num_vars_ = model_->num_vars;
    max_dom_size_ = model_->max_dom_size;
    words_per_var_ = (max_dom_size_ + kBitsPerWord - 1) / kBitsPerWord;
    in_queue_bits_.resize(static_cast<size_t>(num_vars_) * words_per_var_, 0);
  }

  bool Enabled() const { return config_.enabled; }
  bool Empty() const { return deferred_.empty(); }

  const Stats& GetStats() const { return stats_; }
  void NotifyDeferredHit() { stats_.deferred_hit++; }

  bool Enqueue(int var_id,
               int value,
               int nb_epoch_snapshot,
               int retry,
               int enqueue_round) {
    if (!config_.enabled) return false;
    if (var_id < 0 || var_id >= num_vars_) return false;
    if (value < 0 || value >= max_dom_size_) return false;

    if (model_->IsAssigned(var_id)) return false;
    if (!HasValue(var_id, value)) return false;

    if (config_.max_queue_size > 0 &&
        deferred_.size() >= static_cast<size_t>(config_.max_queue_size)) {
      stats_.deferred_overflow++;
      return false;
    }
    if (config_.max_retries > 0 && retry >= config_.max_retries) {
      stats_.deferred_stale++;
      return false;
    }

    const int idx = var_id * words_per_var_ + value / kBitsPerWord;
    const uint32_t mask = 1u << (value % kBitsPerWord);
    if (in_queue_bits_[idx] & mask) return false;

    deferred_.push_back(
        DeferredProbe{var_id, value, nb_epoch_snapshot, retry, enqueue_round});
    in_queue_bits_[idx] |= mask;
    stats_.deferred_in++;
    return true;
  }

  void CollectReadyTasks(const std::vector<int>& nb_epoch,
                         int current_round,
                         int max_count,
                         std::vector<ProbeTask>& out_tasks,
                         std::vector<uint8_t>& out_from_deferred,
                         std::vector<int>& out_prev_retry) {
    if (!config_.enabled) return;
    if (max_count <= 0) return;

    std::vector<DeferredProbe> kept;
    kept.reserve(deferred_.size());

    for (const auto& dp : deferred_) {
      if (config_.max_age_rounds > 0 &&
          current_round - dp.enqueue_round > config_.max_age_rounds) {
        ClearInQueueBit(dp.var_id, dp.value);
        stats_.deferred_stale++;
        continue;
      }

      if (model_->IsAssigned(dp.var_id) || !HasValue(dp.var_id, dp.value)) {
        ClearInQueueBit(dp.var_id, dp.value);
        stats_.deferred_stale++;
        continue;
      }

      const bool ready =
          dp.var_id >= 0 && dp.var_id < static_cast<int>(nb_epoch.size()) &&
          nb_epoch[dp.var_id] > dp.nb_epoch_snapshot;
      if (ready && static_cast<int>(out_tasks.size()) < max_count) {
        out_tasks.emplace_back(dp.var_id, dp.value,
                               static_cast<int>(out_tasks.size()));
        out_from_deferred.push_back(1);
        out_prev_retry.push_back(dp.retry);
        ClearInQueueBit(dp.var_id, dp.value);
        stats_.deferred_out++;
        continue;
      }

      kept.push_back(dp);
    }

    deferred_.swap(kept);
  }

 private:
  bool HasValue(int var, int val) const {
    const int word_idx = val / kBitsPerWord;
    const int bit_idx = val % kBitsPerWord;
    const int base_idx = model_->GetBitDomIndex(var, 0);
    const u32* bitDom = model_->GetBitDom();
    return (bitDom[base_idx + word_idx] & (1u << bit_idx)) != 0;
  }

  void ClearInQueueBit(int var_id, int value) {
    const int idx = var_id * words_per_var_ + value / kBitsPerWord;
    const uint32_t mask = 1u << (value % kBitsPerWord);
    in_queue_bits_[idx] &= ~mask;
  }

  GModel* model_;
  GModelSolver::DeferredRecheckConfig config_;
  int num_vars_ = 0;
  int max_dom_size_ = 0;
  int words_per_var_ = 0;
  std::vector<DeferredProbe> deferred_;
  std::vector<uint32_t> in_queue_bits_;
  Stats stats_;
};

// ============================================================================
// EnforceSAC1 - GPU SAC1 预处理（使用 Batch Probe 基础设施）
// ============================================================================
int GModelSolver::EnforceSAC1(GpuSearchStatistics& stats) {
  Timer sac_timer;
  int total_deletions = 0;
  int round = 0;
  unsigned long long total_precheck_short_circuits = 0;  // precheck 短路总数

  // P0-3: reset probe-level stats cache (used by preprocess benchmark)
  last_sac_probe_iterations_.clear();
  last_sac_total_iterations_ = 0;
  last_sac_max_iterations_ = 0;
  last_sac_unknown_probes_ = 0;
  last_sac_early_stopped_ = false;

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
  stage2_manager.EnableStats(collect_sac_probe_stats_);

  auto AppendProbeIterations = [&](const std::vector<int>& iters) {
    if (!collect_sac_probe_stats_ || iters.empty()) return;
    last_sac_probe_iterations_.insert(last_sac_probe_iterations_.end(),
                                      iters.begin(), iters.end());
    for (int v : iters) {
      last_sac_total_iterations_ += v;
      if (v > last_sac_max_iterations_) last_sac_max_iterations_ = v;
    }
  };

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
      AppendProbeIterations(stage1_manager.GetLastProbeIterations());
      stage1_manager.Clear();
    } else {
      // 使用 Stage 2 (Persistent Blocks)
      for (const auto& t : tasks) {
        stage2_manager.AddTask(t.var_id, t.value);
      }
      stage2_manager.ExecutePersistentBlocks(failed_vars, failed_values);
      precheck_short_circuits = stage2_manager.GetLastPrecheckShortCircuitCount();
      AppendProbeIterations(stage2_manager.GetLastProbeIterations());
      last_sac_unknown_probes_ += stage2_manager.GetLastStatistics().unknown_count;
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
  last_sac_early_stopped_ = early_stopped;

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

  // P0-3: reset probe-level stats cache (used by preprocess benchmark)
  last_sac_probe_iterations_.clear();
  last_sac_total_iterations_ = 0;
  last_sac_max_iterations_ = 0;
  last_sac_unknown_probes_ = 0;
  last_sac_early_stopped_ = false;

  if (verbose_) {
    std::cout << "\n=== SAC3 预处理开始（无删值快路径优化）===" << std::endl;
  }

  // 使用构造时构建的共享邻接表（neighbor_csr_ 成员）

  // P0-2: NSAC allowed-constraints mask 初始化
  if (nsac_mask_config_.enabled) {
    if (!model_->IsAllowedMasksBuilt()) {
      Timer mask_timer;
      model_->BuildAllowedMasks();
      if (verbose_) {
        std::cout << "[SAC3] NSAC mask built in " << mask_timer.elapsed()
                  << " ms (bitmap_words=" << model_->GetGModelDataView().constraint_bitmap_words
                  << ")" << std::endl;
      }
    }
    model_->SetNSACMaskEnabled(true);
  } else {
    model_->SetNSACMaskEnabled(false);
  }

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
  stage2_manager.EnableStats(collect_sac_probe_stats_);

  auto AppendProbeIterations = [&](const std::vector<int>& iters) {
    if (!collect_sac_probe_stats_ || iters.empty()) return;
    last_sac_probe_iterations_.insert(last_sac_probe_iterations_.end(),
                                      iters.begin(), iters.end());
    for (int v : iters) {
      last_sac_total_iterations_ += v;
      if (v > last_sac_max_iterations_) last_sac_max_iterations_ = v;
    }
  };

  // P1-1: Batch-3A 约束聚合作为可选加速器（默认关闭）
  // 最小风险 gating：NSAC mask 开启时禁用 Batch-3A，强制回退 Stage2（保证“真 NSAC”语义一致）。
  const bool batch3a_requested = batch3a_config_.enabled;
  const bool nsac_enabled = model_->IsNSACMaskEnabled();
  std::unique_ptr<Batch3AManager> batch3a_manager;
  if (batch3a_requested) {
    if (nsac_enabled) {
      if (verbose_) {
        std::cout << "[SAC3] NSAC mask enabled, Batch-3A disabled (fallback Stage2)"
                  << std::endl;
      }
    } else {
      const int max_worlds = std::clamp(batch3a_config_.max_worlds, 1, 32);
      batch3a_manager = std::make_unique<Batch3AManager>(model_, -1, max_worlds);
      batch3a_manager->SetActivationStrategy(1);  // NEIGHBOR_ACTIVATION
      batch3a_manager->SetMaxIterations(stage2_manager.GetMaxIterationsPerProbe());
    }
  }

  // P0-1c：UNKNOWN probes 延后复查队列（邻域 epoch）
  DeferredProbeQueue deferred_queue(model_, deferred_recheck_config_);
  std::vector<int> nb_epoch(model_->num_vars, 0);
  auto BumpNeighborhoodEpoch = [&](int changed_var) {
    if (changed_var < 0 || changed_var >= model_->num_vars) return;
    nb_epoch[changed_var]++;
    for (const int* p = neighbor_csr_.GetNeighborsBegin(changed_var);
         p != neighbor_csr_.GetNeighborsEnd(changed_var); ++p) {
      nb_epoch[*p]++;
    }
  };

  // 预分配任务容量
  const int max_batch_size = 1024;
  stage2_manager.ReserveTaskCapacity(max_batch_size);

  bool early_stopped = false;
  std::vector<ProbeTask> tasks;
  tasks.reserve(max_batch_size);
  std::vector<uint8_t> task_from_deferred;
  task_from_deferred.reserve(max_batch_size);
  std::vector<int> task_prev_retry;
  task_prev_retry.reserve(max_batch_size);
  std::vector<int8_t> task_bucket;
  task_bucket.reserve(max_batch_size);
  std::vector<int> failed_vars, failed_values;
  std::vector<int> unknown_vars, unknown_values;

  // P0-1d：失败概率优先（按 var 统计历史 DWO 命中率，用于 probe 入队分桶）
  std::vector<int> var_probe_count(model_->num_vars, 0);
  std::vector<int> var_dwo_count(model_->num_vars, 0);
  const bool failure_priority_enabled =
      failure_priority_config_.enabled && failure_priority_config_.num_buckets > 1;
  std::vector<int> bucket_total;
  std::vector<int> bucket_dwo;
  if (failure_priority_enabled) {
    const int n = std::clamp(failure_priority_config_.num_buckets, 1, 16);
    bucket_total.assign(n, 0);
    bucket_dwo.assign(n, 0);
  }

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
  ProbeQueue::BudgetStats probe_queue_budget_stats;
  bool has_probe_queue_budget_stats = false;
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
  auto CollectLinearBatch = [&](int max_count) -> bool {
    tasks.clear();
    while (tasks.size() < static_cast<size_t>(max_count)) {
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

  // 辅助 lambda：执行一批 probe 并返回是否有删值（同时收集 UNKNOWN probes）
  auto ExecuteBatch = [&](std::vector<int>& out_failed_vars,
                          std::vector<int>& out_failed_values,
                          std::vector<int>& out_unknown_vars,
                          std::vector<int>& out_unknown_values) -> bool {
    out_failed_vars.clear();
    out_failed_values.clear();
    out_unknown_vars.clear();
    out_unknown_values.clear();
    unsigned long long precheck_short_circuits = 0;

    bool has_deferred = false;
    for (uint8_t f : task_from_deferred) {
      if (f) {
        has_deferred = true;
        break;
      }
    }

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
    // 若本批次包含“已知长尾”的 deferred probes，则强制走 Stage2
    if (has_deferred && sac_stage_mode_ == StageSelection::kAuto) {
      stage = StageSelection::kStage2;
      stats.sac_stage = stage;
    }

    // P1-1: Batch-3A 可选加速器（仅在非 NSAC 且 batch 规模合适时启用）
    const int num_tasks = static_cast<int>(tasks.size());
    const bool can_use_batch3a =
        (batch3a_manager != nullptr) &&
        !has_deferred &&
        (num_tasks >= batch3a_config_.min_worlds) &&
        (num_tasks <= batch3a_manager->GetMaxWorlds()) &&
        batch3a_manager->IsSuitableForBatch3A();

    if (can_use_batch3a) {
      for (const auto& t : tasks) {
        batch3a_manager->AddTask(t.var_id, t.value);
      }
      batch3a_manager->Execute(out_failed_vars, out_failed_values,
                               &out_unknown_vars, &out_unknown_values);
      precheck_short_circuits = 0;
      last_sac_unknown_probes_ += static_cast<int>(out_unknown_vars.size());

      // 统计：标记本批次使用了 Batch-3A（Stage 字段仍保留 Stage2 语义，便于兼容现有打印）
      stats.sac_stage = StageSelection::kStage2;
      stats.sac_batch3a_batches += 1;
      stats.sac_batch3a_probes += num_tasks;
      stats.sac_batch3a_unknown += static_cast<int>(out_unknown_vars.size());

      stats.sac_probes += num_tasks;
      return !out_failed_vars.empty();
    }

    if (stage == StageSelection::kStage1) {
      for (const auto& t : tasks) {
        stage1_manager.AddTask(t.var_id, t.value);
      }
      stage1_manager.ExecuteMicroBatch(out_failed_vars, out_failed_values);
      precheck_short_circuits = stage1_manager.GetLastPrecheckShortCircuitCount();
      AppendProbeIterations(stage1_manager.GetLastProbeIterations());
      stage1_manager.Clear();
    } else {
      for (const auto& t : tasks) {
        stage2_manager.AddTask(t.var_id, t.value);
      }
      stage2_manager.ExecutePersistentBlocks(out_failed_vars, out_failed_values,
                                             &out_unknown_vars, &out_unknown_values);
      precheck_short_circuits = stage2_manager.GetLastPrecheckShortCircuitCount();
      AppendProbeIterations(stage2_manager.GetLastProbeIterations());
      last_sac_unknown_probes_ += static_cast<int>(out_unknown_vars.size());
      stage2_manager.Clear();
    }
    total_precheck_short_circuits += precheck_short_circuits;
    stats.sac_probes += static_cast<int>(tasks.size());

    return !out_failed_vars.empty();
  };

  // P0-1c: UNKNOWN → deferred queue（先入队，再由邻域 epoch 触发复查）
  auto EnqueueUnknownToDeferred = [&](int enqueue_round) {
    if (!deferred_queue.Enabled() || unknown_vars.empty()) return;

    std::unordered_map<uint64_t, int> task_index;
    task_index.reserve(tasks.size() * 2);
    for (int i = 0; i < static_cast<int>(tasks.size()); ++i) {
      uint64_t key =
          (static_cast<uint64_t>(tasks[i].var_id) << 32) |
          static_cast<uint32_t>(tasks[i].value);
      task_index[key] = i;
    }

    for (size_t i = 0; i < unknown_vars.size(); ++i) {
      const int var = unknown_vars[i];
      const int val = unknown_values[i];
      if (var < 0 || var >= model_->num_vars) continue;

      uint64_t key = (static_cast<uint64_t>(var) << 32) |
                     static_cast<uint32_t>(val);
      const auto it = task_index.find(key);
      const int idx = (it == task_index.end()) ? -1 : it->second;

      const bool from_deferred =
          (idx >= 0) ? (task_from_deferred[idx] != 0) : false;
      const int prev_retry = (idx >= 0) ? task_prev_retry[idx] : 0;
      const int retry = from_deferred ? (prev_retry + 1) : 0;

      deferred_queue.Enqueue(var, val, nb_epoch[var], retry, enqueue_round);
    }
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

    // 删值触发邻域 epoch 递增（驱动 deferred probes 复查）
    for (int v : failed_unique_vars) {
      BumpNeighborhoodEpoch(v);
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
      probe_queue = new ProbeQueue(model_, neighbor_csr_, failure_priority_config_,
                                   sac_queue_budget_config_,
                                   &var_probe_count, &var_dwo_count);
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
        BumpNeighborhoodEpoch(v);
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
      if (sac_config_.max_rounds > 0 && batch_count > sac_config_.max_rounds) {
        if (verbose_) {
          std::cout << "[SAC3] 达到最大批次数 (" << sac_config_.max_rounds
                    << ")，停止" << std::endl;
        }
        early_stopped = true;
        batch_count = sac_config_.max_rounds;
        break;
      }
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

    // P1-2: 外层预算（总 probe 数上限）
    int linear_max_batch = max_batch_size;
    if (sac_queue_budget_config_.enabled &&
        sac_queue_budget_config_.max_total_probes > 0) {
      const int remaining =
          sac_queue_budget_config_.max_total_probes - stats.sac_probes;
      if (remaining <= 0) {
        if (verbose_) {
          std::cout << "[SAC3] 达到总 probe 上限 ("
                    << sac_queue_budget_config_.max_total_probes
                    << ")，停止" << std::endl;
        }
        early_stopped = true;
        break;
      }
      linear_max_batch = std::min(linear_max_batch, remaining);
    }

    // 收集任务
    if (!CollectLinearBatch(linear_max_batch)) break;

    // 本批次不是 deferred probes
    task_from_deferred.assign(tasks.size(), 0);
    task_prev_retry.assign(tasks.size(), 0);

    if (verbose_ && batch_count % 10 == 1) {
      std::cout << "[SAC3 Linear Batch " << batch_count << "] "
                << tasks.size() << " probes" << std::endl;
    }

    // 执行 batch probe
    bool has_deletions = ExecuteBatch(failed_vars, failed_values,
                                      unknown_vars, unknown_values);
    EnqueueUnknownToDeferred(batch_count);

    // P0-1d：更新历史统计（用于后续分桶/调度）
    for (const auto& t : tasks) {
      if (t.var_id >= 0 && t.var_id < model_->num_vars) {
        var_probe_count[t.var_id]++;
      }
    }
    for (int v : failed_vars) {
      if (v >= 0 && v < model_->num_vars) {
        var_dwo_count[v]++;
      }
    }

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

    while (true) {
      // 早停检查
      if (sac_config_.early_stop_enabled) {
        double elapsed_ms = sac_timer.elapsed();
        if (sac_config_.max_rounds > 0 && batch_count >= sac_config_.max_rounds) {
          if (verbose_) {
            std::cout << "[SAC3] 达到最大批次数 (" << sac_config_.max_rounds
                      << ")，停止" << std::endl;
          }
          early_stopped = true;
          break;
        }
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

      // P1-2: 外层预算（总 probe 数上限）
      int queue_max_batch = max_batch_size;
      if (sac_queue_budget_config_.enabled &&
          sac_queue_budget_config_.max_total_probes > 0) {
        const int remaining =
            sac_queue_budget_config_.max_total_probes - stats.sac_probes;
        if (remaining <= 0) {
          if (verbose_) {
            std::cout << "[SAC3] 达到总 probe 上限 ("
                      << sac_queue_budget_config_.max_total_probes
                      << ")，停止" << std::endl;
          }
          early_stopped = true;
          break;
        }
        queue_max_batch = std::min(queue_max_batch, remaining);
      }

      tasks.clear();
      task_from_deferred.clear();
      task_prev_retry.clear();
      task_bucket.clear();

      // [1] 优先调度 deferred ready tasks
      if (deferred_queue.Enabled()) {
        deferred_queue.CollectReadyTasks(nb_epoch, batch_count, queue_max_batch,
                                         tasks, task_from_deferred,
                                         task_prev_retry);
      }
      task_bucket.assign(tasks.size(), static_cast<int8_t>(-1));

      // [2] regular probe queue 填充 batch（保持 GPU 吞吐）
      if (static_cast<int>(tasks.size()) < queue_max_batch && !probe_queue->Empty()) {
        std::vector<ProbeTask> extra;
        std::vector<int8_t> extra_bucket;
        probe_queue->DequeueBatch(
            queue_max_batch - static_cast<int>(tasks.size()),
            extra,
            &extra_bucket);
        for (int i = 0; i < static_cast<int>(extra.size()); ++i) {
          tasks.push_back(extra[i]);
          task_from_deferred.push_back(0);
          task_prev_retry.push_back(0);
          task_bucket.push_back(extra_bucket.empty() ? 0 : extra_bucket[i]);
        }
      }

      if (tasks.empty()) {
        if (probe_queue->Empty()) break;
        continue;
      }

      batch_count++;

      if (verbose_ && batch_count % 10 == 1) {
        std::cout << "[SAC3 Queue Batch " << batch_count << "] "
                  << tasks.size() << " probes, queue remaining: "
                  << probe_queue->Size() << std::endl;
      }

      // 执行 batch probe
      bool has_deletions = ExecuteBatch(failed_vars, failed_values,
                                        unknown_vars, unknown_values);
      EnqueueUnknownToDeferred(batch_count);

      // P0-1d：更新历史统计（用于后续分桶/调度）
      for (const auto& t : tasks) {
        if (t.var_id >= 0 && t.var_id < model_->num_vars) {
          var_probe_count[t.var_id]++;
        }
      }
      for (int v : failed_vars) {
        if (v >= 0 && v < model_->num_vars) {
          var_dwo_count[v]++;
        }
      }
      if (failure_priority_enabled) {
        for (int8_t b : task_bucket) {
          if (b >= 0 && b < static_cast<int8_t>(bucket_total.size())) {
            bucket_total[b]++;
          }
        }
        if (!failed_vars.empty()) {
          std::unordered_map<uint64_t, int8_t> bucket_by_task;
          bucket_by_task.reserve(tasks.size() * 2);
          for (int i = 0; i < static_cast<int>(tasks.size()); ++i) {
            const int8_t b = task_bucket[i];
            if (b < 0) continue;
            uint64_t key = (static_cast<uint64_t>(tasks[i].var_id) << 32) |
                           static_cast<uint32_t>(tasks[i].value);
            bucket_by_task[key] = b;
          }
          for (size_t i = 0; i < failed_vars.size(); ++i) {
            uint64_t key = (static_cast<uint64_t>(failed_vars[i]) << 32) |
                           static_cast<uint32_t>(failed_values[i]);
            const auto it = bucket_by_task.find(key);
            if (it == bucket_by_task.end()) continue;
            const int8_t b = it->second;
            if (b >= 0 && b < static_cast<int8_t>(bucket_dwo.size())) {
              bucket_dwo[b]++;
            }
          }
        }
      }

      if (has_deletions) {
        // 统计 deferred hit（重检后 DWO）
        if (deferred_queue.Enabled()) {
          std::unordered_map<uint64_t, int> task_index;
          task_index.reserve(tasks.size() * 2);
          for (int i = 0; i < static_cast<int>(tasks.size()); ++i) {
            uint64_t key =
                (static_cast<uint64_t>(tasks[i].var_id) << 32) |
                static_cast<uint32_t>(tasks[i].value);
            task_index[key] = i;
          }
          for (size_t i = 0; i < failed_vars.size(); ++i) {
            const int var = failed_vars[i];
            const int val = failed_values[i];
            uint64_t key = (static_cast<uint64_t>(var) << 32) |
                           static_cast<uint32_t>(val);
            const auto it = task_index.find(key);
            const int idx = (it == task_index.end()) ? -1 : it->second;
            if (idx >= 0 && task_from_deferred[idx] != 0) {
              deferred_queue.NotifyDeferredHit();
            }
          }
        }

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

        // 删值触发邻域 epoch 递增（驱动 deferred probes 复查）
        for (int v : failed_unique_vars) {
          BumpNeighborhoodEpoch(v);
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
            BumpNeighborhoodEpoch(v);
            probe_queue->EnqueueNeighborhood(v);
            gac_modified_flags[v] = 0;
          }
        }
      }
    }
  }

  // 清理 ProbeQueue（如果已创建）
  if (probe_queue != nullptr) {
    probe_queue_budget_stats = probe_queue->GetBudgetStats();
    has_probe_queue_budget_stats = true;
    delete probe_queue;
    probe_queue = nullptr;
  }

  stats.sac_deletions = total_deletions;
  stats.sac_rounds = batch_count;
  stats.sac_time = sac_timer.elapsed() / 1000.0;
  last_sac_early_stopped_ = early_stopped;

  if (verbose_) {
    std::cout << "=== SAC3 预处理完成" << (early_stopped ? "（早停）" : "")
              << " ===" << std::endl;
    std::cout << "  批次数: " << batch_count << std::endl;
    std::cout << "  删除: " << total_deletions << std::endl;
    std::cout << "  模式: " << (use_queue_mode ? "队列模式" : "快路径（无队列）")
              << std::endl;
    std::cout << "  Precheck 短路: " << total_precheck_short_circuits << std::endl;
    if (stats.sac_batch3a_batches > 0) {
      std::cout << "  Batch3A: batches=" << stats.sac_batch3a_batches
                << ", probes=" << stats.sac_batch3a_probes
                << ", unknown=" << stats.sac_batch3a_unknown << std::endl;
    }
    std::cout << "  时间: " << stats.sac_time << "s" << std::endl;
    if (deferred_queue.Enabled()) {
      const auto& dqs = deferred_queue.GetStats();
      std::cout << "  Deferred: in=" << dqs.deferred_in
                << ", out=" << dqs.deferred_out
                << ", hit=" << dqs.deferred_hit
                << ", stale=" << dqs.deferred_stale
                << ", overflow=" << dqs.deferred_overflow
                << std::endl;
    }
    if (failure_priority_enabled) {
      std::cout << "  FailurePriority buckets (high->low): ";
      for (int b = static_cast<int>(bucket_total.size()) - 1; b >= 0; --b) {
        const int total = bucket_total[b];
        const int dwo = bucket_dwo[b];
        const double hit = (total > 0) ? (100.0 * dwo / total) : 0.0;
        std::cout << "[" << b << ": " << dwo << "/" << total
                  << " (" << hit << "%)]";
        if (b != 0) std::cout << " ";
      }
      std::cout << std::endl;
    }
    if (sac_queue_budget_config_.enabled && has_probe_queue_budget_stats) {
      std::cout << "  QueueBudget: dropped_by_queue_cap="
                << probe_queue_budget_stats.dropped_by_queue_cap
                << ", requeue_skipped=" << probe_queue_budget_stats.requeue_skipped
                << ", total_requeues=" << probe_queue_budget_stats.total_requeues
                << std::endl;
    }
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
