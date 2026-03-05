#pragma once

#include <string>
#include <vector>
#include <algorithm>
#include <chrono>

#include "GModel.cuh"
#include "solver/gpu/batch_probe_manager.h"

namespace cpim {

// ============================================================================
// NeighborCSR - 共享邻接表（CSR 格式）
// 在 GModelSolver 构造时一次性构建，所有 SAC 函数共享使用
// ============================================================================
struct NeighborCSR {
  std::vector<int> offset;  // [num_vars + 1]
  std::vector<int> data;    // 展平的邻居列表

  // 获取变量 v 的邻居数量
  int GetDegree(int v) const { return offset[v + 1] - offset[v]; }

  // 遍历变量 v 的邻居
  const int* GetNeighborsBegin(int v) const { return data.data() + offset[v]; }
  const int* GetNeighborsEnd(int v) const { return data.data() + offset[v + 1]; }

  // 从 GModel 构建 CSR
  static NeighborCSR Build(GModel* model);
};

// SAC 模式选择
enum class SACMode {
  kSAC1,    // SAC1: 每轮全量检查 (dirty set 变量粒度)
  kSAC3,    // SAC3: 队列驱动 (probe 粒度)
  kAuto     // 自动选择（默认 SAC1）
};

// GPU MSAC 配置（搜索中的条件触发 SAC）
// 注意：与 Solver.h 中的 MSACConfig 不同，这是 GPU 求解器专用的配置
struct GpuMSACConfig {
  bool enabled = false;              // 是否启用
  int max_level = 5;                 // 只在前 N 层执行
  int min_domain_size = 10;          // 域大于此值才执行
  double min_fail_rate = 0.3;        // 上层失败率高于此值才执行
  int max_probes_per_node = 100;     // 每节点最多 probe 数
  bool lightweight = true;           // 轻量级模式（仅检查邻域）
};

// GPU 求解统计信息（扩展版本，包含 GAC 和 SAC 统计）
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

  // SAC 统计
  int sac_deletions = 0;     // SAC 删除的值总数
  int sac_probes = 0;        // SAC 探测次数
  int sac_rounds = 0;        // SAC 轮次
  double sac_time = 0.0;     // SAC 总时间（秒）
  StageSelection sac_stage = StageSelection::kAuto;  // SAC 使用的 Stage

  // P1-1: Batch-3A 接入主路径（统计）
  int sac_batch3a_batches = 0;   // 使用 Batch-3A 的 batch 数
  int sac_batch3a_probes = 0;    // Batch-3A 执行的 probes 数
  int sac_batch3a_unknown = 0;   // Batch-3A 返回的 UNKNOWN probes 数
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

  // ========== SAC 配置 ==========
  // 启用 SAC1 预处理（在搜索前执行一次 SAC）
  void SetSAC1Preprocessing(bool enable) { sac1_preprocessing_ = enable; }

  // 设置 SAC Stage 选择模式
  // kAuto: 使用 AutoStageSelector 自动选择（默认）
  // kStage1: 强制使用 Micro-Batch
  // kStage2: 强制使用 Persistent Blocks
  void SetSACStageMode(StageSelection mode) { sac_stage_mode_ = mode; }

  // 设置 SAC 算法模式
  void SetSACMode(SACMode mode) { sac_mode_ = mode; }
  SACMode GetSACMode() const { return sac_mode_; }

  // SAC 早停配置
  struct SACConfig {
    int max_rounds = 100;           // 最大轮次
    double time_budget_ms = 5000;   // 时间预算（毫秒）
    double min_deletion_rate = 0.01; // 删值率低于此阈值则停止
    bool early_stop_enabled = true;  // 是否启用早停
    int warmup_rounds = 2;          // 预热轮次（不检查删值率）
  };

  void SetSACConfig(const SACConfig& config) { sac_config_ = config; }
  const SACConfig& GetSACConfig() const { return sac_config_; }

  // ========== SAC3: 延后复查队列（P0-1c） ==========
  // UNKNOWN probe 不丢弃：当邻域发生删值变化时再重跑，以回收剪枝机会并抑制长尾。
  struct DeferredRecheckConfig {
    bool enabled = true;        // 总开关（关闭则回退到当前行为：UNKNOWN 直接丢弃）
    int max_retries = 3;        // 单个 (var,value) 最多重检次数
    int max_queue_size = 1000;  // 队列上限（超过则丢弃新入队）
    int max_age_rounds = 200;   // 入队超过该轮数仍未复查则丢弃（0=不限制）
  };

  void SetDeferredRecheckConfig(const DeferredRecheckConfig& config) {
    deferred_recheck_config_ = config;
  }
  const DeferredRecheckConfig& GetDeferredRecheckConfig() const {
    return deferred_recheck_config_;
  }

  // ========== SAC3: 失败概率优先调度（P0-1d） ==========
  // 目标：把"更可能 DWO"的 probe 更早跑完，让删值尽早发生，从而降低后续传播成本并抑制长尾。
  // 注意：这只改变 probe 的调度顺序，不改变 soundness 语义（只对 kDWO 删值）。
  struct FailurePriorityConfig {
    bool enabled = false;       // 总开关（关闭则回退 FIFO）
    int num_buckets = 8;        // 分桶数量（建议 4/8/16）
    float w_dom = 1.0f;         // 域收缩项权重（dom 越小越优先）
    float w_deg = 1.0f;         // 度数项权重（degree 越大越优先）
    float w_hist = 1.0f;        // 历史 DWO 率权重（同 var 的 probe 命中率）
    int min_hist_probes = 16;   // 历史统计生效的最小样本数
  };

  void SetFailurePriorityConfig(const FailurePriorityConfig& config) {
    failure_priority_config_ = config;
  }
  const FailurePriorityConfig& GetFailurePriorityConfig() const {
    return failure_priority_config_;
  }

  // ========== P0-2: NSAC allowed-constraints mask ==========
  // singleton test 的传播严格限制在 Xi + N(Xi) 诱导子图（真正的 NSAC）
  // 启用后会预计算每个 focal variable 的 allowed constraints 位图，
  // GPU 端 frontier 扩张时做 bit AND 过滤，严格限制传播范围。
  struct NSACMaskConfig {
    bool enabled = true;  // 总开关（关闭则回退到全图传播）
  };

  void SetNSACMaskConfig(const NSACMaskConfig& config) {
    nsac_mask_config_ = config;
  }
  const NSACMaskConfig& GetNSACMaskConfig() const {
    return nsac_mask_config_;
  }

  // ========== P1-1: Batch-3A 接入 SAC3 主路径 ==========
  // 作为可选加速器：在满足条件时用约束聚合（Batch-3A）替换 Stage2。
  // 最小风险 gating：当 NSAC mask 开启时，Batch-3A 自动禁用并回退到 Stage2。
  struct Batch3AConfig {
    bool enabled = false;   // 总开关（默认关闭）
    int min_worlds = 8;     // 小于此值不启用（聚合收益不足）
    int max_worlds = 32;    // 单次并发 world 上限（Batch-3A 限制）
  };

  void SetBatch3AConfig(const Batch3AConfig& config) {
    batch3a_config_ = config;
  }
  const Batch3AConfig& GetBatch3AConfig() const {
    return batch3a_config_;
  }

  // ========== P1-2: 外层队列预算（queue-level budget） ==========
  // 目标：让 SAC3/MSAC 在难例上可控结束（sound but incomplete），
  // 防止 probe 队列/重入队爆炸导致长尾失控。
  //
  // 语义：预算触发只会减少“继续 probe 的数量/范围”，不会导致误删（仍只对 kDWO 删值）。
  struct SacQueueBudgetConfig {
    bool enabled = false;          // 总开关（默认关闭；开启时才生效）
    int max_total_probes = 0;      // 总 probe 数上限（0=不限制；达到则早停）
    int max_queue_size = 0;        // regular queue 上限（0=不限制；超过则丢弃新入队）
    int max_total_requeues = 0;    // 总重入队次数上限（0=不限制；超过则停止扩张邻域）
    int max_requeues_per_var = 0;  // 单变量最大重入队次数（0=不限制）
  };

  void SetSacQueueBudgetConfig(const SacQueueBudgetConfig& config) {
    sac_queue_budget_config_ = config;
  }
  const SacQueueBudgetConfig& GetSacQueueBudgetConfig() const {
    return sac_queue_budget_config_;
  }

  // MSAC 配置（搜索中的条件触发 SAC）
  void SetMSACConfig(const GpuMSACConfig& config) { msac_config_ = config; }
  const GpuMSACConfig& GetMSACConfig() const { return msac_config_; }

  // 执行 SAC1 预处理
  // 返回删除的值的数量；如果检测到不一致返回 -1
  int EnforceSAC1(GpuSearchStatistics& stats);

  // 执行 SAC3 预处理（队列驱动，probe 粒度）
  // 返回删除的值的数量；如果检测到不一致返回 -1
  int EnforceSAC3(GpuSearchStatistics& stats);

  // ========== P0-3: 统一观测入口（最小可用）==========
  // 用于 preprocess/benchmark 口径的统计闭环：把每个 probe 的 iterations 汇总起来，
  // 便于输出 avg/p95/max iterations、unknown probes 等指标。
  //
  // 注意：
  // - 默认关闭（避免在求解/搜索阶段引入额外 host 侧收集开销）。
  // - 开启后，会在 SAC1/SAC3 内部启用 Stage2 的 stats 收集，并把每批次的 probe_iterations
  //   追加到 last_sac_probe_iterations_。
  void EnableSacProbeStats(bool enabled) { collect_sac_probe_stats_ = enabled; }
  bool IsSacProbeStatsEnabled() const { return collect_sac_probe_stats_; }

  const std::vector<int>& GetLastSacProbeIterations() const {
    return last_sac_probe_iterations_;
  }
  int64_t GetLastSacTotalIterations() const { return last_sac_total_iterations_; }
  int GetLastSacMaxIterations() const { return last_sac_max_iterations_; }
  int GetLastSacUnknownProbes() const { return last_sac_unknown_probes_; }
  bool WasLastSacEarlyStopped() const { return last_sac_early_stopped_; }

 private:
  GModel* model_;                         // GModel 指针（不拥有）
  bool verbose_;                          // 是否打印详细信息
  bool find_all_solutions_;               // 是否查找所有解
  int max_solutions_;                     // 最大解的数量
  std::vector<std::vector<int>> solutions_;  // 找到的所有解

  // 共享邻接表（构造时一次性构建）
  NeighborCSR neighbor_csr_;

  // SAC 配置
  bool sac1_preprocessing_ = false;       // 是否启用 SAC1 预处理
  StageSelection sac_stage_mode_ = StageSelection::kAuto;  // SAC Stage 模式
  SACConfig sac_config_;                  // SAC 早停配置
  SACMode sac_mode_ = SACMode::kSAC1;     // SAC 算法模式

  // MSAC 配置
  GpuMSACConfig msac_config_;             // MSAC 配置
  int last_level_failures_ = 0;           // 上层失败次数（用于计算失败率）
  int last_level_positives_ = 0;          // 上层正向节点数

  // SAC3 延后复查队列配置（P0-1c）
  DeferredRecheckConfig deferred_recheck_config_;
  FailurePriorityConfig failure_priority_config_;
  NSACMaskConfig nsac_mask_config_;  // P0-2: NSAC allowed-constraints mask
  Batch3AConfig batch3a_config_;     // P1-1: Batch-3A 可选加速器
  SacQueueBudgetConfig sac_queue_budget_config_;  // P1-2: 外层队列预算

  // ========== P0-3: SAC preprocess 统计缓存 ==========
  bool collect_sac_probe_stats_ = false;
  std::vector<int> last_sac_probe_iterations_;
  int64_t last_sac_total_iterations_ = 0;
  int last_sac_max_iterations_ = 0;
  int last_sac_unknown_probes_ = 0;
  bool last_sac_early_stopped_ = false;

  // 递归搜索（DFS + MAC）
  // 返回 true 表示找到解或达到最大解数量
  bool Search(int level, GpuSearchStatistics& stats, int time_limit,
              std::chrono::steady_clock::time_point deadline);

  // MSAC 辅助函数
  // 判断是否应该执行 MSAC
  bool ShouldEnforceMSAC(int level, int var, const GpuSearchStatistics& stats);

  // 执行轻量级 MSAC（只检查邻域）
  // 返回删除的值的数量；如果检测到不一致返回 -1
  int EnforceLightweightMSAC(int var, GpuSearchStatistics& stats);

  // 从当前状态提取解
  void ExtractSolution(int level);
};

}  // namespace cpim
