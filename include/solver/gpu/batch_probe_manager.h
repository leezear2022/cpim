// Copyright 2025 CPIM Project
// Batch Probe Manager for GPU-accelerated SAC
// Implements Batch-1 approach: time-dimension batching of singleton probes

#ifndef CPIM_BATCH_PROBE_MANAGER_H_
#define CPIM_BATCH_PROBE_MANAGER_H_

#include <vector>
#include <cuda_runtime.h>
#include "GModel.cuh"

namespace cpim {

// ============================================================================
// ProbeTask - 单个探测任务 (12 bytes)
// ============================================================================
struct ProbeTask {
  int var_id;    // 变量 ID
  int value;     // 探测值
  int task_id;   // 任务 ID（调试用）

  __host__ __device__ ProbeTask() : var_id(-1), value(-1), task_id(-1) {}
  __host__ __device__ ProbeTask(int v, int a, int tid = -1)
      : var_id(v), value(a), task_id(tid) {}
};

// ============================================================================
// BatchProbeControl - 批量探测控制块 (~128 bytes)
// ============================================================================
struct BatchProbeControl {
  // ========== 任务管理 ==========
  int num_tasks;              // 总任务数
  int current_task_index;     // 当前任务索引（原子递增）

  // ========== 快照与恢复 ==========
  const u32* domain_snapshot;     // 域快照（单份共享，所有任务恢复时使用）
  const int* dom_size_snapshot;   // 域大小快照 [num_vars]

  // ========== 任务与结果 ==========
  const ProbeTask* tasks;     // 任务数组 [num_tasks]
  bool* results;              // 结果数组 [num_tasks] (true = 一致, false = DWO)

  // ========== GAC 控制（嵌入 PersistentGACControl 字段）==========
  int inconsistent_flag;      // 不一致标志（1 = DWO）
  int scanner_index;          // 前沿扫描索引
  unsigned long long deletions; // 删值计数器
  int iterations;             // 迭代计数器
  int converged_flag;         // 收敛标志（1 = 已收敛）
  int frontier_nonempty;      // 前沿非空标志（1 = 非空）

  // ========== 前沿位图 ==========
  u32* frontier_A;            // 前沿位图 A [bitmap_size_words]
  u32* frontier_B;            // 前沿位图 B [bitmap_size_words]
  int bitmap_size_words;      // 位图大小（word 数）

  // ========== P0-1: Frontier 初始化策略 ==========
  bool snapshot_is_ac;            // 快照是否已 AC（true = 使用邻域激活）
  int activation_strategy;        // 0 = FULL, 1 = NEIGHBOR

  // ========== P0-2: Cheap Precheck 统计 ==========
  unsigned long long precheck_count;      // Precheck 总次数
  unsigned long long short_circuit_count; // 短路次数（跳过 GAC）
  int need_gac_flag;                      // Precheck 结果标志（0=短路，1=需要GAC）

  // ========== 构造函数 ==========
  __host__ __device__ BatchProbeControl()
      : num_tasks(0),
        current_task_index(0),
        domain_snapshot(nullptr),
        dom_size_snapshot(nullptr),
        tasks(nullptr),
        results(nullptr),
        inconsistent_flag(0),
        scanner_index(0),
        deletions(0),
        iterations(0),
        converged_flag(0),
        frontier_nonempty(0),
        frontier_A(nullptr),
        frontier_B(nullptr),
        bitmap_size_words(0),
        snapshot_is_ac(false),        // P0-1
        activation_strategy(-1),      // P0-1: -1 表示未设置
        precheck_count(0),            // P0-2
        short_circuit_count(0),       // P0-2
        need_gac_flag(0) {}           // P0-2
};

// ============================================================================
// BatchProbeManager - Host 端批量探测管理器
// ============================================================================
class BatchProbeManager {
 public:
  // 构造函数
  // @param model: GModel 指针
  // @param max_batch_size: 最大批量大小（默认 256）
  explicit BatchProbeManager(GModel* model, int max_batch_size = 256);

  // 析构函数（释放 GPU 内存）
  ~BatchProbeManager();

  // 禁止拷贝
  BatchProbeManager(const BatchProbeManager&) = delete;
  BatchProbeManager& operator=(const BatchProbeManager&) = delete;

  // 添加探测任务
  // @param var_id: 变量 ID
  // @param value: 探测值
  void AddTask(int var_id, int value);

  // 执行批量探测
  // @param failed_vars: 输出失败的变量 ID 列表
  // @param failed_values: 输出失败的值列表
  // @return: 失败任务数
  int ExecuteBatch(std::vector<int>& failed_vars,
                   std::vector<int>& failed_values);

  // 清空任务队列
  void Clear();

  // 获取当前任务数
  int GetTaskCount() const { return task_queue_.size(); }

  // 获取最大批量大小
  int GetMaxBatchSize() const { return max_batch_size_; }

  // 设置 Frontier 初始化策略
  // @param strategy: 0 = FULL_ACTIVATION, 1 = NEIGHBOR_ACTIVATION
  void SetActivationStrategy(int strategy);

 private:
  // ========== Host 端数据 ==========
  GModel* model_;                      // GModel 指针
  int max_batch_size_;                 // 最大批量大小
  std::vector<ProbeTask> task_queue_;  // 任务队列

  // ========== Device 端数据（统一内存）==========
  ProbeTask* d_tasks_;                 // 任务数组 [max_batch_size]
  bool* d_results_;                    // 结果数组 [max_batch_size]
  u32* d_snapshot_;                    // 域快照 [num_vars * bit_dom_int_size]
  int* d_dom_size_snapshot_;           // 域大小快照 [num_vars]
  BatchProbeControl* d_control_;       // 控制块
  u32* d_frontier_A_;                  // 前沿位图 A
  u32* d_frontier_B_;                  // 前沿位图 B

  // ========== 内存管理标志 ==========
  bool memory_allocated_;              // 内存是否已分配

  // ========== 内部方法 ==========

  // 分配 GPU 内存
  void AllocateMemory();

  // 释放 GPU 内存
  void FreeMemory();

  // 保存当前域快照
  void SaveSnapshot();

  // 恢复域快照（修复高优先级bug）
  void RestoreSnapshot();

  // 启动批量探测 kernel
  // @param num_tasks: 任务数
  void LaunchBatchProbeKernel(int num_tasks);

  // 收集结果
  // @param num_tasks: 任务数
  // @param failed_vars: 输出失败的变量 ID 列表
  // @param failed_values: 输出失败的值列表
  // @return: 失败任务数
  int CollectResults(int num_tasks,
                     std::vector<int>& failed_vars,
                     std::vector<int>& failed_values);
};

// ============================================================================
// Batch-2: WorldWorkspace - 每个 probe 的私有工作区（Micro-Batch / Persistent Blocks）
// ============================================================================
struct WorldWorkspace {
  // ========== 私有域 ==========
  u32* bitDom = nullptr;           // [num_vars * bit_dom_int_size]
  int* d_cur_dom_size = nullptr;   // [num_vars]

  // ========== 私有前沿 ==========
  u32* frontier_A = nullptr;       // [bitmap_size_words]
  u32* frontier_B = nullptr;       // [bitmap_size_words]

  // ========== 私有控制状态 ==========
  int inconsistent_flag = 0;       // 1 = DWO
  int scanner_index = 0;           // frontier 扫描索引（单 block 使用）
  unsigned long long deletions = 0;
  int iterations = 0;
  int frontier_nonempty = 0;       // 1 = 非空

  __host__ __device__ WorldWorkspace() = default;
};

// ============================================================================
// Batch-2: Host 端管理器（Stage 1: Micro-Batch）
// ============================================================================
class Batch2ProbeManager {
 public:
  // @param model: GModel 指针（要求调用者保证 snapshot 已 AC）
  // @param max_batch_size: micro-batch 大小（默认 64）
  explicit Batch2ProbeManager(GModel* model, int max_batch_size = 64);
  ~Batch2ProbeManager();

  Batch2ProbeManager(const Batch2ProbeManager&) = delete;
  Batch2ProbeManager& operator=(const Batch2ProbeManager&) = delete;

  void AddTask(int var_id, int value);

  // 执行 micro-batch（内部按 max_batch_size 分批）
  int ExecuteMicroBatch(std::vector<int>& failed_vars,
                        std::vector<int>& failed_values);

  void Clear();

  int GetTaskCount() const { return task_queue_.size(); }
  int GetMaxBatchSize() const { return max_batch_size_; }

  // 0 = FULL_ACTIVATION, 1 = NEIGHBOR_ACTIVATION
  void SetActivationStrategy(int strategy);

  // Cheap Precheck（默认关闭；在 snapshot 已 AC 场景下多为“断言/防御性检查”）
  void EnablePrecheck(bool enabled);

  // 统计收集：记录每个 probe 的 iterations/deletions（默认关闭）
  void EnableStats(bool enabled);
  const std::vector<int>& GetLastProbeIterations() const {
    return last_probe_iterations_;
  }
  const std::vector<unsigned long long>& GetLastProbeDeletions() const {
    return last_probe_deletions_;
  }
  unsigned long long GetLastPrecheckShortCircuitCount() const {
    return last_precheck_short_circuit_count_;
  }

 private:
  void AllocateMemory();
  void FreeMemory();
  void SaveSnapshot();

  // 启动 micro-batch kernel（处理 task_queue_[offset : offset+batch_size)）
  void LaunchMicroBatchKernel(int batch_size);

  // 收集本批次结果（使用 d_results_[0..batch_size)）
  int CollectBatchResults(int batch_size,
                          int task_offset,
                          std::vector<int>& failed_vars,
                          std::vector<int>& failed_values);

  GModel* model_ = nullptr;
  int max_batch_size_ = 0;
  std::vector<ProbeTask> task_queue_;

  // ========== Device 端数据（统一内存）==========
  ProbeTask* d_tasks_ = nullptr;            // [max_batch_size]
  bool* d_results_ = nullptr;               // [max_batch_size]
  u32* d_snapshot_ = nullptr;               // [num_vars * bit_dom_int_size]
  int* d_dom_size_snapshot_ = nullptr;      // [num_vars]

  // Workspaces（只分配 max_batch_size 份）
  WorldWorkspace* d_workspaces_ = nullptr;  // [max_batch_size]
  u32* d_ws_bitdom_ = nullptr;              // [max_batch_size * num_vars * bit_dom_int_size]
  int* d_ws_dom_size_ = nullptr;            // [max_batch_size * num_vars]
  u32* d_ws_frontier_A_ = nullptr;          // [max_batch_size * bitmap_size_words]
  u32* d_ws_frontier_B_ = nullptr;          // [max_batch_size * bitmap_size_words]

  // ========== 配置 ==========
  int activation_strategy_ = 1;             // 默认 NEIGHBOR_ACTIVATION
  bool precheck_enabled_ = false;
  bool stats_enabled_ = false;

  // ========== 最近一次 ExecuteMicroBatch 的统计 ==========
  std::vector<int> last_probe_iterations_;
  std::vector<unsigned long long> last_probe_deletions_;
  unsigned long long last_precheck_short_circuit_count_ = 0;

  bool memory_allocated_ = false;
};

// ============================================================================
// Batch-2: Stage 2 - Persistent Blocks 控制结构
// ============================================================================
struct Batch2PersistentControl {
  // ========== 全局任务管理 ==========
  int num_tasks;                  // 总任务数
  int* task_cursor;               // 全局任务游标（原子递增）

  // ========== 快照（只读共享）==========
  const u32* domain_snapshot;     // 域快照 [num_vars * bit_dom_int_size]
  const int* dom_size_snapshot;   // 域大小快照 [num_vars]

  // ========== 任务与结果 ==========
  const ProbeTask* tasks;         // 任务数组 [num_tasks]
  bool* results;                  // 结果数组 [num_tasks]

  // ========== Per-task 统计（可选，nullptr 表示不收集）==========
  int* task_iterations;           // 每个 task 的迭代次数 [num_tasks]
  unsigned long long* task_deletions;  // 每个 task 的删值数 [num_tasks]

  // ========== Workspaces（每个 block 对应一个）==========
  WorldWorkspace* workspaces;     // [num_blocks]

  // ========== 配置 ==========
  int num_blocks;                 // 启动的 block 数
  int activation_strategy;        // 0 = FULL, 1 = NEIGHBOR
  int enable_precheck;            // 是否启用 precheck
  int max_iterations_per_probe;   // 每个 probe 的最大迭代次数
  int chunk_size;                 // 每次拉取的任务数（批量拉取优化）

  // ========== Precheck 统计 ==========
  unsigned long long* precheck_short_circuit_count;  // precheck 短路次数（原子递增）

  __host__ __device__ Batch2PersistentControl()
      : num_tasks(0),
        task_cursor(nullptr),
        domain_snapshot(nullptr),
        dom_size_snapshot(nullptr),
        tasks(nullptr),
        results(nullptr),
        task_iterations(nullptr),
        task_deletions(nullptr),
        workspaces(nullptr),
        num_blocks(0),
        activation_strategy(1),
        enable_precheck(0),
        max_iterations_per_probe(1000),
        chunk_size(1),
        precheck_short_circuit_count(nullptr) {}
};

// ============================================================================
// Batch-2: Stage 2 管理器 - Persistent Blocks 实现
// ============================================================================
class Batch2PersistentManager {
 public:
  // @param model: GModel 指针（要求调用者保证 snapshot 已 AC）
  // @param num_blocks: 持久 blocks 数量（-1 表示自动调优，默认 -1）
  explicit Batch2PersistentManager(GModel* model, int num_blocks = -1);
  ~Batch2PersistentManager();

  Batch2PersistentManager(const Batch2PersistentManager&) = delete;
  Batch2PersistentManager& operator=(const Batch2PersistentManager&) = delete;

  void AddTask(int var_id, int value);

  // 执行 Persistent Blocks（一次 kernel 调用处理所有任务）
  int ExecutePersistentBlocks(std::vector<int>& failed_vars,
                              std::vector<int>& failed_values);

  void Clear();

  int GetTaskCount() const { return task_queue_.size(); }
  int GetNumBlocks() const { return num_blocks_; }
  int GetEffectiveNumBlocks() const { return effective_num_blocks_; }

  // 自适应 num_blocks（默认开启，num_blocks=-1 时自动启用）
  // 启发式：num_tasks <= 2*num_sms → num_sms blocks
  //         否则 min(max_resident_blocks, num_tasks)
  void EnableAutoTune(bool enabled) { auto_tune_enabled_ = enabled; }
  bool IsAutoTuneEnabled() const { return auto_tune_enabled_; }

  // 计算最优 num_blocks（静态方法，可独立调用）
  static int ComputeOptimalNumBlocks(int num_tasks, int device_id = 0);

  // 设置批量拉取任务大小（默认 4）
  void SetChunkSize(int chunk) { chunk_size_ = chunk > 0 ? chunk : 4; }
  int GetChunkSize() const { return chunk_size_; }

  // 预分配任务数组容量（避免运行时重新分配）
  void ReserveTaskCapacity(int capacity);
  int GetTaskCapacity() const { return max_tasks_; }

  // 0 = FULL_ACTIVATION, 1 = NEIGHBOR_ACTIVATION
  void SetActivationStrategy(int strategy);

  // Cheap Precheck（默认关闭）
  void EnablePrecheck(bool enabled);

  // 统计收集
  void EnableStats(bool enabled);
  const std::vector<int>& GetLastProbeIterations() const {
    return last_probe_iterations_;
  }
  const std::vector<unsigned long long>& GetLastProbeDeletions() const {
    return last_probe_deletions_;
  }
  unsigned long long GetLastPrecheckShortCircuitCount() const {
    return last_precheck_short_circuit_count_;
  }

 private:
  void AllocateMemory();
  void FreeMemory();
  void SaveSnapshot();
  void LaunchPersistentBlocksKernel();
  int CollectResults(std::vector<int>& failed_vars,
                     std::vector<int>& failed_values);

  GModel* model_ = nullptr;
  int num_blocks_ = 0;
  std::vector<ProbeTask> task_queue_;

  // ========== Device 端数据（统一内存）==========
  ProbeTask* d_tasks_ = nullptr;            // [max_tasks]
  bool* d_results_ = nullptr;               // [max_tasks]
  u32* d_snapshot_ = nullptr;               // [num_vars * bit_dom_int_size]
  int* d_dom_size_snapshot_ = nullptr;      // [num_vars]
  int* d_task_cursor_ = nullptr;            // 全局任务游标（原子操作）

  // 控制结构
  Batch2PersistentControl* d_control_ = nullptr;

  // Workspaces（每个 block 一份）
  WorldWorkspace* d_workspaces_ = nullptr;  // [num_blocks]
  u32* d_ws_bitdom_ = nullptr;              // [num_blocks * num_vars * bit_dom_int_size]
  int* d_ws_dom_size_ = nullptr;            // [num_blocks * num_vars]
  u32* d_ws_frontier_A_ = nullptr;          // [num_blocks * bitmap_size_words]
  u32* d_ws_frontier_B_ = nullptr;          // [num_blocks * bitmap_size_words]

  // Per-task 统计数组
  int* d_task_iterations_ = nullptr;        // [max_tasks]
  unsigned long long* d_task_deletions_ = nullptr;  // [max_tasks]

  // Precheck 统计（全局计数器）
  unsigned long long* d_precheck_short_circuit_count_ = nullptr;  // 原子递增

  // ========== 配置 ==========
  int activation_strategy_ = 1;             // 默认 NEIGHBOR_ACTIVATION
  bool precheck_enabled_ = false;
  bool stats_enabled_ = false;
  int max_tasks_ = 0;                       // 当前分配的最大任务数
  bool auto_tune_enabled_ = false;          // 自适应 num_blocks
  int effective_num_blocks_ = 0;            // 实际使用的 blocks 数
  int chunk_size_ = 1;                      // 批量拉取任务大小（1 = 无批量）

  // ========== 统计 ==========
  std::vector<int> last_probe_iterations_;
  std::vector<unsigned long long> last_probe_deletions_;
  unsigned long long last_precheck_short_circuit_count_ = 0;

  bool memory_allocated_ = false;
};

// ============================================================================
// AutoStageSelector - 自动选择 Stage 1 或 Stage 2
// ============================================================================
enum class StageSelection {
  kStage1,  // Micro-Batch
  kStage2,  // Persistent Blocks
  kAuto,    // 需要采样决定
};

struct StageSelectionResult {
  StageSelection stage;
  int recommended_blocks;      // Stage 2 推荐 blocks 数
  int recommended_batch_size;  // Stage 1 推荐 batch_size
  std::string reason;          // 决策原因

  // 采样统计（如果执行了采样）
  bool sampled;
  double sample_fail_rate;
  double sample_avg_iterations;
  double sample_avg_deletions;

  // 实测对比结果（如果执行了实测）
  bool timed;                  // 是否进行了实测对比
  double stage1_time_ms;       // Stage 1 采样执行时间 (ms)
  double stage2_time_ms;       // Stage 2 采样执行时间 (ms)
  double speedup_ratio;        // Stage2 / Stage1 加速比
};

class AutoStageSelector {
 public:
  // @param model: GModel 指针
  // @param sample_batch_size: 采样批次大小（默认 16）
  explicit AutoStageSelector(GModel* model, int sample_batch_size = 16);
  ~AutoStageSelector() = default;

  AutoStageSelector(const AutoStageSelector&) = delete;
  AutoStageSelector& operator=(const AutoStageSelector&) = delete;

  // 基于任务数快速决策（不采样）
  StageSelectionResult DecideByTaskCount(int num_tasks) const;

  // 执行采样并决策（基于统计）
  // @param tasks: 所有待执行的任务
  // @return: 选择结果
  StageSelectionResult DecideWithSampling(const std::vector<ProbeTask>& tasks);

  // 执行实测对比并决策（推荐：更准确但开销略大）
  // 对同一批采样任务分别运行 Stage 1 和 Stage 2，用实际耗时选择
  // @param tasks: 所有待执行的任务
  // @param num_blocks: Stage 2 使用的 blocks 数（-1=自动）
  // @return: 选择结果
  StageSelectionResult DecideWithTimedComparison(
      const std::vector<ProbeTask>& tasks, int num_blocks = -1);

  // ========== 生产用法：带缓存的决策 ==========
  // 适用于 SAC/MSAC 集成：每个 instance 只决策一次，后续使用缓存结果
  //
  // @param tasks: 当前待执行的任务（用于首次采样）
  // @param num_blocks: Stage 2 使用的 blocks 数（-1=自动）
  // @return: 选择结果（首次调用执行实测，后续返回缓存）
  //
  // 缓存策略：
  // - 首次调用：运行 DecideWithTimedComparison 并缓存结果
  // - 后续调用：直接返回缓存结果，跳过采样
  // - 调用 ClearCache() 可重置缓存
  //
  // 使用示例:
  //   AutoStageSelector selector(gmodel);
  //   // 第一轮 SAC：执行采样
  //   auto result = selector.DecideCached(tasks1);
  //   // 第二轮 SAC：直接返回缓存
  //   auto result2 = selector.DecideCached(tasks2);  // 无开销
  StageSelectionResult DecideCached(const std::vector<ProbeTask>& tasks,
                                    int num_blocks = -1);

  // 清除缓存，下次调用 DecideCached 将重新采样
  void ClearCache() { cache_valid_ = false; }

  // 检查是否有缓存
  bool HasCache() const { return cache_valid_; }

  // 获取缓存的决策结果（如果有）
  const StageSelectionResult* GetCachedResult() const {
    return cache_valid_ ? &cached_result_ : nullptr;
  }

  // 设置阈值参数
  void SetSmallTaskThreshold(int threshold) { small_task_threshold_ = threshold; }
  void SetHighFailRateThreshold(double rate) { high_fail_rate_threshold_ = rate; }
  void SetLowIterationsThreshold(double iter) { low_iterations_threshold_ = iter; }

 private:
  GModel* model_ = nullptr;
  int sample_batch_size_ = 16;

  // 决策阈值
  int small_task_threshold_ = 32;      // 小于此值倾向 Stage 1
  double high_fail_rate_threshold_ = 0.3;  // 失败率高于此值倾向 Stage 2
  double low_iterations_threshold_ = 3.0;  // 迭代数低于此值倾向 Stage 2

  // ========== 缓存 ==========
  bool cache_valid_ = false;           // 缓存是否有效
  StageSelectionResult cached_result_; // 缓存的决策结果

  // 获取设备 SM 数量
  int GetNumSMs() const;
};

// ============================================================================
// Batch-3A: 约束聚合任务 (8 bytes)
// ============================================================================
struct Batch3ATask {
  int cid;            // 约束 ID
  u32 world_mask;     // 哪些 world 需要检查该约束（位掩码，最多 32 个 world）

  __host__ __device__ Batch3ATask() : cid(-1), world_mask(0) {}
  __host__ __device__ Batch3ATask(int c, u32 mask) : cid(c), world_mask(mask) {}
};

// ============================================================================
// Batch-3A: 约束聚合控制结构
// ============================================================================
struct Batch3AControl {
  // ========== 约束任务管理 ==========
  int num_constraint_tasks;             // 约束任务总数
  int* constraint_task_cursor;          // 原子游标（动态获取任务）
  Batch3ATask* constraint_tasks;        // 紧凑任务数组 [num_constraint_tasks]

  // ========== World 管理 ==========
  int num_worlds;                       // 当前 world 数量（≤ 32）
  ProbeTask* world_probes;              // 每个 world 的 probe 信息 [num_worlds]
  WorldWorkspace* workspaces;           // 每个 world 的私有工作区 [num_worlds]

  // ========== 快照（只读共享）==========
  const u32* domain_snapshot;           // 域快照 [num_vars * bit_dom_int_size]
  const int* dom_size_snapshot;         // 域大小快照 [num_vars]

  // ========== 结果 ==========
  bool* results;                        // 每个 world 的结果 [num_worlds]

  // ========== 全局迭代控制 ==========
  int* global_iteration;                // 当前全局迭代次数
  int* all_converged_flag;              // 所有 world 已收敛
  int* any_world_active;                // 至少有一个 world 需要继续迭代
  u32* active_world_mask;               // 活跃 world 掩码（未收敛且未 DWO）

  // ========== 配置 ==========
  int num_blocks;                       // 持久 blocks 数量
  int max_iterations;                   // 最大迭代次数
  int activation_strategy;              // 0 = FULL, 1 = NEIGHBOR

  // ========== Block 分片信息（Phase 4）==========
  int worlds_per_block;                 // G: 每个 block 处理多少 world
  int block_world_start[16];            // 每个 block 的 world 起始索引 [max 16 blocks]
  int block_world_count[16];            // 每个 block 处理的 world 数量 [max 16 blocks]

  // ========== 统计（可选）==========
  unsigned long long* total_constraint_checks;  // 约束检查次数
  unsigned long long* total_deletions;          // 总删值数

  __host__ __device__ Batch3AControl()
      : num_constraint_tasks(0),
        constraint_task_cursor(nullptr),
        constraint_tasks(nullptr),
        num_worlds(0),
        world_probes(nullptr),
        workspaces(nullptr),
        domain_snapshot(nullptr),
        dom_size_snapshot(nullptr),
        results(nullptr),
        global_iteration(nullptr),
        all_converged_flag(nullptr),
        any_world_active(nullptr),
        active_world_mask(nullptr),
        num_blocks(0),
        max_iterations(1000),
        activation_strategy(1),
        worlds_per_block(4),
        total_constraint_checks(nullptr),
        total_deletions(nullptr) {
    for (int i = 0; i < 16; ++i) {
      block_world_start[i] = 0;
      block_world_count[i] = 0;
    }
  }
};

// ============================================================================
// Batch-3A: 约束聚合管理器 - Persistent Blocks + bitSup 复用
// ============================================================================
class Batch3AManager {
 public:
  // @param model: GModel 指针（要求调用者保证 snapshot 已 AC）
  // @param num_blocks: 持久 blocks 数量（-1 表示自动调优）
  // @param max_worlds: 最大 world 数量（默认 32）
  explicit Batch3AManager(GModel* model, int num_blocks = -1, int max_worlds = 32);
  ~Batch3AManager();

  Batch3AManager(const Batch3AManager&) = delete;
  Batch3AManager& operator=(const Batch3AManager&) = delete;

  // ========== 任务管理 ==========
  void AddTask(int var_id, int value);
  void Clear();
  int GetTaskCount() const { return task_queue_.size(); }
  int GetMaxWorlds() const { return max_worlds_; }

  // ========== 执行 ==========
  // 执行约束聚合批量探测
  // @param failed_vars: 输出失败的变量 ID 列表
  // @param failed_values: 输出失败的值列表
  // @return: 失败任务数
  int Execute(std::vector<int>& failed_vars,
              std::vector<int>& failed_values);

  // ========== 配置 ==========
  void SetActivationStrategy(int strategy) { activation_strategy_ = strategy; }
  void SetMaxIterations(int max_iter) { max_iterations_ = max_iter; }
  void EnableStats(bool enabled) { stats_enabled_ = enabled; }

  // ========== 统计 ==========
  unsigned long long GetTotalConstraintChecks() const {
    return total_constraint_checks_;
  }
  unsigned long long GetTotalDeletions() const { return total_deletions_; }

  // ========== 域大小检查（用于 fallback 决策）==========
  // 检查是否适合使用 Batch-3A（bitSup 能放入 shared memory）
  // @return: true 表示适合使用 Batch-3A
  bool IsSuitableForBatch3A() const;

  // 获取每个约束的 bitSup 大小（字节）
  int GetBitSupSizePerConstraint() const;

 private:
  void AllocateMemory();
  void FreeMemory();
  void SaveSnapshot();

  // 从 probe 任务构建约束聚合任务
  // @param num_worlds: 当前 batch 的 world 数量
  void BuildConstraintTasks(int num_worlds);

  // 初始化所有 world（恢复快照、执行单例赋值、初始化 frontier）
  void InitializeWorlds(int num_worlds);

  // 启动约束聚合 kernel
  void LaunchBatch3AKernel(int num_worlds);

  // 收集结果
  int CollectResults(int num_worlds,
                     std::vector<int>& failed_vars,
                     std::vector<int>& failed_values);

  // ========== Host 端数据 ==========
  GModel* model_ = nullptr;
  int num_blocks_ = 0;
  int max_worlds_ = 32;
  std::vector<ProbeTask> task_queue_;

  // ========== Device 端数据（统一内存）==========
  // 任务与结果
  ProbeTask* d_tasks_ = nullptr;                // [max_worlds]
  bool* d_results_ = nullptr;                   // [max_worlds]
  Batch3ATask* d_constraint_tasks_ = nullptr;   // [num_constraints]（最大可能）
  int* d_constraint_task_cursor_ = nullptr;     // 全局游标

  // 快照
  u32* d_snapshot_ = nullptr;                   // [num_vars * bit_dom_int_size]
  int* d_dom_size_snapshot_ = nullptr;          // [num_vars]

  // Workspaces（每个 world 一份）
  WorldWorkspace* d_workspaces_ = nullptr;      // [max_worlds]
  u32* d_ws_bitdom_ = nullptr;                  // [max_worlds * num_vars * bit_dom_int_size]
  int* d_ws_dom_size_ = nullptr;                // [max_worlds * num_vars]
  u32* d_ws_frontier_A_ = nullptr;              // [max_worlds * bitmap_size_words]
  u32* d_ws_frontier_B_ = nullptr;              // [max_worlds * bitmap_size_words]

  // 控制结构
  Batch3AControl* d_control_ = nullptr;

  // 全局迭代控制
  int* d_global_iteration_ = nullptr;
  int* d_all_converged_flag_ = nullptr;
  int* d_any_world_active_ = nullptr;
  u32* d_active_world_mask_ = nullptr;

  // 统计
  unsigned long long* d_total_constraint_checks_ = nullptr;
  unsigned long long* d_total_deletions_ = nullptr;

  // ========== 配置 ==========
  int activation_strategy_ = 1;                 // 默认 NEIGHBOR_ACTIVATION
  int max_iterations_ = 1000;
  bool stats_enabled_ = false;

  // ========== 运行时统计 ==========
  unsigned long long total_constraint_checks_ = 0;
  unsigned long long total_deletions_ = 0;

  bool memory_allocated_ = false;
};

}  // namespace cpim

#endif  // CPIM_BATCH_PROBE_MANAGER_H_
