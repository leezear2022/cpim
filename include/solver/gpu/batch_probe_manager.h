// Copyright 2025 CPIM Project
// Batch Probe Manager for GPU-accelerated SAC
// Implements Batch-1 approach: time-dimension batching of singleton probes

#ifndef CPIM_BATCH_PROBE_MANAGER_H_
#define CPIM_BATCH_PROBE_MANAGER_H_

#include <vector>
#include <algorithm>
#include <climits>
#include <cstdint>
#include <string>
#include <cuda_runtime.h>
#include "GModel.cuh"

namespace cpim {

// ============================================================================
// ProbeStatus - 探测结果状态（P0-1：UNKNOWN 语义）
// ============================================================================
enum class ProbeStatus : int8_t {
  kOK = 0,       // 正常收敛，域一致（不删值）
  kDWO = 1,      // Domain Wipe-Out（值可删）
  kUNKNOWN = 2   // 预算超限/未收敛（不删值，保守处理）
};

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

  // ========== P0-1 NEW: UNKNOWN 语义与预算控制 ==========
  int8_t* probe_status;           // 探测状态数组 [num_tasks]（ProbeStatus 枚举值）
  int* probe_iterations;          // 每个 probe 的实际迭代次数 [num_tasks]
  int max_iterations_per_probe;   // per-probe 迭代预算（0 = 无限制）
  int budget_hit_flag;            // 当前 probe 是否触发预算（1 = 触发）

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
        need_gac_flag(0),             // P0-2
        probe_status(nullptr),        // P0-1 NEW
        probe_iterations(nullptr),    // P0-1 NEW
        max_iterations_per_probe(0),  // P0-1 NEW: 0 = 无限制
        budget_hit_flag(0) {}         // P0-1 NEW
};

// ============================================================================
// ProbeStatistics - 探测统计信息（P0-1：统计闭环）
// ============================================================================
struct ProbeStatistics {
  // 基础计数
  int total_probes = 0;           // 总探测数
  int dwo_count = 0;              // DWO 计数（可删值）
  int ok_count = 0;               // OK 计数（一致）
  int unknown_count = 0;          // UNKNOWN 计数（预算超限）
  int budget_hit_count = 0;       // 预算触发次数

  // 迭代统计
  int64_t total_iterations = 0;   // 总迭代次数
  int max_iterations = 0;         // 最大单次迭代
  int min_iterations = INT_MAX;   // 最小单次迭代

  // 用于计算分位数的直方图（可选）
  std::vector<int> iter_histogram;

  // 计算 UNKNOWN 比例
  double unknown_rate() const {
    return total_probes > 0 ? static_cast<double>(unknown_count) / total_probes : 0.0;
  }

  // 计算 DWO 比例
  double dwo_rate() const {
    return total_probes > 0 ? static_cast<double>(dwo_count) / total_probes : 0.0;
  }

  // 计算平均迭代次数
  double avg_iterations() const {
    return total_probes > 0 ? static_cast<double>(total_iterations) / total_probes : 0.0;
  }

  // 重置统计
  void Reset() {
    total_probes = dwo_count = ok_count = unknown_count = budget_hit_count = 0;
    total_iterations = 0;
    max_iterations = 0;
    min_iterations = INT_MAX;
    iter_histogram.clear();
  }

  // 合并统计（用于多批次累计）
  void Merge(const ProbeStatistics& other) {
    total_probes += other.total_probes;
    dwo_count += other.dwo_count;
    ok_count += other.ok_count;
    unknown_count += other.unknown_count;
    budget_hit_count += other.budget_hit_count;
    total_iterations += other.total_iterations;
    max_iterations = std::max(max_iterations, other.max_iterations);
    if (other.min_iterations < min_iterations) {
      min_iterations = other.min_iterations;
    }
  }
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

  // ========== P0-1a：停滞检测字段 ==========
  int stagnation_count = 0;        // 连续零删值轮次
  int last_deletions = 0;          // 上轮删值数（截断为 int 足够）
  int last_frontier_popcount = 0;  // 上轮前沿大小
  int work_cnt = 0;                // 当前轮工作量（约束检查数）

  // ========== P0-1b：时间片调度字段 ==========
  int total_constraints_checked = 0;  // 累计处理的约束数
  int quantum_exceeded = 0;           // 1 = 已超过工作量子

  __host__ __device__ WorldWorkspace() = default;
};

// ============================================================================
// P0-1b: GACTimesliceState - GAC 时间片状态（用于暂停/恢复）
// ============================================================================
struct GACTimesliceState {
  int iterations = 0;               // 当前迭代次数
  int scanner_index = 0;            // frontier 扫描位置
  int local_word = 0;               // 当前扫描的 word 索引
  int local_offset = 0;             // 当前 word 内的 bit 偏移
  unsigned long long deletions = 0; // 累计删值数
  int stagnation_count = 0;         // 停滞计数
  int total_constraints_checked = 0; // 累计约束检查数

  __host__ __device__ GACTimesliceState() = default;
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
  bool* results;                  // 结果数组 [num_tasks]（兼容旧接口）

  // ========== P0-1 NEW: 三态结果 ==========
  int8_t* task_status;            // 探测状态 [num_tasks]（ProbeStatus 枚举值）
                                  // kOK=0: 收敛一致, kDWO=1: 可删, kUNKNOWN=2: 预算超限

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

  // ========== P0-1a：停滞检测配置 ==========
  int stagnation_threshold;       // 连续多少轮零删值判定为停滞（默认 3）
  float min_productivity;         // 最低产出率阈值 (deletions/work_cnt)（默认 0.001）
  int enable_stagnation_check;    // 是否启用停滞检测（默认 1）

  // ========== P0-1b：时间片调度配置 ==========
  int quantum_cid;                // 工作量子：每个 probe 最多检查的约束数（0=无限制）
  int enable_quantum_check;       // 是否启用工作量子检查（默认 0）

  // ========== P0-2：NSAC allowed-constraints mask ==========
  const u32* allowed_masks;       // [num_vars * constraint_bitmap_words]（nullptr=禁用）
  int constraint_bitmap_words;    // (num_constraints + 31) / 32
  int focal_var;                  // 当前 probe 的 focal variable（用于选择 mask 行）

  // ========== Precheck 统计 ==========
  unsigned long long* precheck_short_circuit_count;  // precheck 短路次数（原子递增）

  __host__ __device__ Batch2PersistentControl()
      : num_tasks(0),
        task_cursor(nullptr),
        domain_snapshot(nullptr),
        dom_size_snapshot(nullptr),
        tasks(nullptr),
        results(nullptr),
        task_status(nullptr),       // P0-1 NEW
        task_iterations(nullptr),
        task_deletions(nullptr),
        workspaces(nullptr),
        num_blocks(0),
        activation_strategy(1),
        enable_precheck(0),
        max_iterations_per_probe(1000),
        chunk_size(1),
        stagnation_threshold(3),        // P0-1a: 默认 3 轮
        min_productivity(0.001f),       // P0-1a: 默认 0.1% 产出率
        enable_stagnation_check(1),     // P0-1a: 默认启用
        quantum_cid(0),                 // P0-1b: 默认无限制
        enable_quantum_check(0),        // P0-1b: 默认关闭
        allowed_masks(nullptr),         // P0-2: 默认禁用
        constraint_bitmap_words(0),     // P0-2
        focal_var(-1),                  // P0-2: -1 表示未设置
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
  // 执行 Persistent Blocks（增强版：额外返回 UNKNOWN probes）
  // unknown_vars/unknown_values 为 nullptr 表示不收集 UNKNOWN probes。
  int ExecutePersistentBlocks(std::vector<int>& failed_vars,
                              std::vector<int>& failed_values,
                              std::vector<int>* unknown_vars,
                              std::vector<int>* unknown_values);

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

  // P0-1 NEW: 获取最近一次执行的统计（三态）
  const ProbeStatistics& GetLastStatistics() const { return last_statistics_; }

  // P0-1 NEW: 设置预算参数
  void SetMaxIterationsPerProbe(int max_iters) { max_iterations_per_probe_ = max_iters; }
  int GetMaxIterationsPerProbe() const { return max_iterations_per_probe_; }

  // P0-1a NEW: 停滞检测配置
  void SetStagnationThreshold(int threshold) { stagnation_threshold_ = threshold; }
  int GetStagnationThreshold() const { return stagnation_threshold_; }
  void SetMinProductivity(float min_prod) { min_productivity_ = min_prod; }
  float GetMinProductivity() const { return min_productivity_; }
  void EnableStagnationCheck(bool enabled) { stagnation_check_enabled_ = enabled; }
  bool IsStagnationCheckEnabled() const { return stagnation_check_enabled_; }

  // P0-1b NEW: 时间片调度配置
  void SetQuantumCid(int quantum) { quantum_cid_ = quantum; }
  int GetQuantumCid() const { return quantum_cid_; }
  void EnableQuantumCheck(bool enabled) { quantum_check_enabled_ = enabled; }
  bool IsQuantumCheckEnabled() const { return quantum_check_enabled_; }

 private:
  void AllocateMemory();
  void FreeMemory();
  void SaveSnapshot();
  void LaunchPersistentBlocksKernel();
  int CollectResults(std::vector<int>& failed_vars,
                     std::vector<int>& failed_values,
                     std::vector<int>* unknown_vars,
                     std::vector<int>* unknown_values);

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

  // P0-1 NEW: 三态状态数组
  int8_t* d_task_status_ = nullptr;         // [max_tasks] ProbeStatus 枚举值

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

  // P0-1 NEW: 三态统计
  ProbeStatistics last_statistics_;
  int max_iterations_per_probe_ = 1000;  // 默认预算

  // P0-1a NEW: 停滞检测配置
  int stagnation_threshold_ = 3;         // 连续多少轮零删值判定为停滞
  float min_productivity_ = 0.001f;      // 最低产出率阈值
  bool stagnation_check_enabled_ = true; // 是否启用停滞检测

  // P0-1b NEW: 时间片调度配置
  int quantum_cid_ = 0;                  // 工作量子（0=无限制）
  bool quantum_check_enabled_ = false;   // 是否启用工作量子检查

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
  void ClearCache() {
    cache_medium_valid_ = false;
    cache_large_valid_ = false;
  }

  // 检查是否有缓存
  bool HasCache() const { return cache_medium_valid_ || cache_large_valid_; }

  // 获取缓存的决策结果（如果有）
  const StageSelectionResult* GetCachedResult() const {
    if (cache_large_valid_) return &cached_large_result_;
    if (cache_medium_valid_) return &cached_medium_result_;
    return nullptr;
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
  bool cache_medium_valid_ = false;            // 中等任务量缓存是否有效
  bool cache_large_valid_ = false;             // 大任务量缓存是否有效
  StageSelectionResult cached_medium_result_;  // 中等任务量的缓存结果
  StageSelectionResult cached_large_result_;   // 大任务量的缓存结果

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
// Batch-3A: P2-1 约束检查线程映射（用于消融/实验）
// ============================================================================
// 注意：Batch-3A 当前 world_mask 为 u32（最多 32 个 world）。
// - kWarpPerWorld: 现有实现（每个 warp 处理 1 个 world）
// - kSubwarpPerWorld: Route-A（一个 warp 内多个 subwarp 并行处理多个 world）
enum Batch3ACheckMapping : int {
  kWarpPerWorld = 0,
  kSubwarpPerWorld = 1,
  // P2-2: warp-per-word + lane-per-world，配合 shared dom packing（Route-B 原型）
  kWarpPerWordLaneWorld = 2,
};

// ============================================================================
// Batch-3A: 约束聚合控制结构
// ============================================================================
struct Batch3AControl {
  // ========== 约束任务管理 ==========
  int num_constraint_tasks;             // 约束任务总数
  int* constraint_task_cursor;          // 原子游标（动态获取任务）
  Batch3ATask* constraint_tasks;        // 紧凑任务数组 [num_constraint_tasks]

  // ========== Dynamic Submission：队列驱动（每个 block 一套）==========
  // 说明：
  // - 旧版 Batch-3A 每轮由 thread0 扫描全体 cid 构建 local_task_cids（成本与 num_cons 成正比）。
  // - 新版改为 worklist：约束检查结束时把“受影响的邻接约束” enqueue 到下一轮队列，
  //   从而显式利用稀疏性（成本与活跃 cid 数量成正比）。
  //
  // 数据布局：
  // - mask_A/B: [max_blocks * num_cons]，每个元素是该 block 的局部 world_mask（0..G-1 位）
  // - queue_A/B: [max_blocks * queue_capacity]，存放 cid
  // - tail_A/B/overflow: [max_blocks]
  int queue_capacity;                  // 每个 block 的队列容量（建议 >= num_cons；溢出则 UNKNOWN）
  u32* block_frontier_mask_A;          // [max_blocks * num_cons]
  u32* block_frontier_mask_B;          // [max_blocks * num_cons]
  int* block_cid_queue_A;              // [max_blocks * queue_capacity]
  int* block_cid_queue_B;              // [max_blocks * queue_capacity]
  int* block_queue_tail_A;             // [max_blocks]
  int* block_queue_tail_B;             // [max_blocks]
  int* block_overflow;                 // [max_blocks]（1=队列溢出）

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
  int check_mapping;                    // P2-1: Batch3ACheckMapping（默认 kWarpPerWorld）
  int subwarp_size;                     // P2-1: 4/8/16（仅 mapping=subwarp 生效）
  int requested_worlds_per_block;       // P2-2: 覆盖 G（0=auto，范围 1..32）
  int shmem_padding;                   // P2-2b: shared packing stride padding（0=off, 1=on）

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
        queue_capacity(0),
        block_frontier_mask_A(nullptr),
        block_frontier_mask_B(nullptr),
        block_cid_queue_A(nullptr),
        block_cid_queue_B(nullptr),
        block_queue_tail_A(nullptr),
        block_queue_tail_B(nullptr),
        block_overflow(nullptr),
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
        check_mapping(static_cast<int>(kWarpPerWorld)),
        subwarp_size(8),
        requested_worlds_per_block(0),
        shmem_padding(0),
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
  // @param unknown_vars: 输出 UNKNOWN 的变量 ID 列表（可为 nullptr）
  // @param unknown_values: 输出 UNKNOWN 的值列表（可为 nullptr）
  // @return: 失败任务数
  int Execute(std::vector<int>& failed_vars,
              std::vector<int>& failed_values,
              std::vector<int>* unknown_vars,
              std::vector<int>* unknown_values);

  // 兼容旧接口：不收集 UNKNOWN probes
  int Execute(std::vector<int>& failed_vars,
              std::vector<int>& failed_values) {
    return Execute(failed_vars, failed_values, nullptr, nullptr);
  }

  // ========== 配置 ==========
  void SetActivationStrategy(int strategy) { activation_strategy_ = strategy; }
  void SetMaxIterations(int max_iter) { max_iterations_ = max_iter; }
  void EnableStats(bool enabled) { stats_enabled_ = enabled; }
  void SetCheckMapping(Batch3ACheckMapping mapping) { check_mapping_ = mapping; }
  void SetSubwarpSize(int subwarp_size) { subwarp_size_ = subwarp_size; }
  // 覆盖每个 block 处理的 world 数（G），0=auto（由 wrapper 决定）。
  void SetWorldsPerBlock(int worlds_per_block) {
    requested_worlds_per_block_ = std::clamp(worlds_per_block, 0, 32);
  }
  // P2-2b: shared packing stride padding（默认关闭，便于消融）。
  void SetShmemPadding(bool enabled) { shmem_padding_ = enabled; }

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
  static constexpr int kMaxBatch3ABlocks = 16;
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
                     std::vector<int>& failed_values,
                     std::vector<int>* unknown_vars,
                     std::vector<int>* unknown_values);

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

  // Dynamic Submission（每个 block 一套 A/B 队列与 mask）
  int queue_capacity_ = 0;                      // 每个 block 队列容量（通常取 num_cons）
  u32* d_block_frontier_mask_A_ = nullptr;      // [kMaxBatch3ABlocks * num_cons]
  u32* d_block_frontier_mask_B_ = nullptr;      // [kMaxBatch3ABlocks * num_cons]
  int* d_block_cid_queue_A_ = nullptr;          // [kMaxBatch3ABlocks * queue_capacity_]
  int* d_block_cid_queue_B_ = nullptr;          // [kMaxBatch3ABlocks * queue_capacity_]
  int* d_block_queue_tail_A_ = nullptr;         // [kMaxBatch3ABlocks]
  int* d_block_queue_tail_B_ = nullptr;         // [kMaxBatch3ABlocks]
  int* d_block_overflow_ = nullptr;             // [kMaxBatch3ABlocks]

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
  Batch3ACheckMapping check_mapping_ = kWarpPerWorld;
  int subwarp_size_ = 8;
  int requested_worlds_per_block_ = 0;
  bool shmem_padding_ = false;

  // ========== 运行时统计 ==========
  unsigned long long total_constraint_checks_ = 0;
  unsigned long long total_deletions_ = 0;

  bool memory_allocated_ = false;
};

// ============================================================================
// FQ-PT Baseline: Task = (world_id, cid) 的摊平队列持久线程基线
// ============================================================================

// FQ-PT 任务（8 bytes）
struct FQPTTask {
  int world_id;  // 子问题/世界 ID
  int cid;       // 约束 ID

  __host__ __device__ FQPTTask() : world_id(-1), cid(-1) {}
  __host__ __device__ FQPTTask(int w, int c) : world_id(w), cid(c) {}
};

// MPMC ring 的槽位（seq + payload）
struct FQPTRingSlot {
  unsigned long long seq;  // 序号（避免 MPMC holes）
  FQPTTask task;

  __host__ __device__ FQPTRingSlot() : seq(0), task() {}
};

// FQ-PT 控制结构（Host 填充，Kernel 读写）
struct FQPTControl {
  // ========== 世界与工作区 ==========
  int num_worlds;                       // world 数量（= probe 数）
  const ProbeTask* world_probes;        // [num_worlds]
  WorldWorkspace* workspaces;           // [num_worlds]
  const u32* domain_snapshot;           // 只读快照 [num_vars * bit_dom_int_size]
  const int* dom_size_snapshot;         // 只读快照 [num_vars]
  bool* world_results;                  // [num_worlds]（true=OK/UNKNOWN, false=DWO）
  int* world_status;                    // [num_worlds]（ProbeStatus 枚举值）
  int* world_locks;                     // [num_worlds]（0=free, 1=held）

  // ========== 全局 MPMC ring ==========
  FQPTRingSlot* queue_slots;            // [queue_capacity]
  unsigned long long* enqueue_pos;      // 生产者位置（CAS）
  unsigned long long* dequeue_pos;      // 消费者位置（CAS）
  int queue_capacity;                   // 2 的幂
  int queue_mask;                       // queue_capacity - 1

  // ========== 安全退出与统计 ==========
  unsigned long long* pending_tasks;     // 未完成任务数（安全退出判据）
  unsigned long long* processed_tasks;   // 已完成任务数
  unsigned long long* overflow_count;    // 队列溢出计数
  unsigned long long* unknown_count;     // UNKNOWN world 计数

  // ========== CTA-local 策略 ==========
  int cta_pop_batch;                     // 批量 pop 大小 K
  int local_buffer_capacity;             // CTA 本地缓冲容量 L
  int lock_retry_limit;                  // world_lock 拿锁失败重试次数
  int lock_backoff;                      // 拿锁失败时 backoff 迭代数
  int enable_cid_grouping;               // 1=启用按 cid 分桶
  int enable_parallel_group_check;       // 1=启用分桶后并行检查
  int group_warps_per_cta;               // 分组检查最多使用的 warp 数
  int group_degrade_threshold;           // max_bucket_size<=threshold 时退化到逐任务
  int enable_world_owner;                // 1=启用 Owner-World + Frontier 路径
  int enable_world_stealing;             // 1=启用 world_cursor 动态领取（OW2）
  unsigned int* world_cursor;            // OW2: world 动态领取游标
  int enable_ow1_frontier_scatter;       // 1=启用 OW1 邻接写回 warp 协作
  int ow1_min_degree;                    // OW1 触发最小 degree（低于阈值走 OW0）
  int ow1_scatter_mode;                  // 0=OW0 fallback, 1=OW1, 2=OW1_v2(match_any)
  int ow1_force_scatter;                 // 1=忽略 min_degree，强制 scatter
  int enable_cid_microbatch_profile;     // 1=启用 OW3a 命中率统计采样
  int microbatch_profile_interval;       // OW3a 采样间隔（轮）

  // ========== 可选统计 ==========
  unsigned long long* total_constraint_checks;  // 约束检查次数
  unsigned long long* total_deletions;          // 总删值数
  unsigned long long* stale_drop_count;         // 陈旧任务丢弃数
  unsigned long long* lock_fail_count;          // 拿锁失败次数（task 维度）
  unsigned long long* lock_retry_count;         // 拿锁重试总次数（attempt 维度）
  unsigned long long* bucket_count;             // 实际触发分桶次数
  unsigned long long* bucket_task_sum;          // 分桶任务总数
  unsigned long long* bucket_active_warp_sum;   // 分桶活跃 warp 总数
  unsigned long long* frontier_pop_count;       // Owner-World: frontier pop 次数
  unsigned long long* frontier_scan_steps;      // Owner-World: L1 扫描步数
  unsigned long long* ow1_scatter_calls;        // OW1: scatter 分支触发次数
  unsigned long long* ow1_fallback_calls;       // OW1: fallback 分支触发次数
  unsigned long long* ow1_word_leader_writes;   // OW1: leader 写回 frontier word 次数
  unsigned long long* microbatch_rounds;        // OW3a: 采样总轮数
  unsigned long long* microbatch_sel_ge2_rounds;  // OW3a: sel_count>=2 的轮数
  unsigned long long* microbatch_sel_sum;       // OW3a: sel_count 累计和

  __host__ __device__ FQPTControl()
      : num_worlds(0),
        world_probes(nullptr),
        workspaces(nullptr),
        domain_snapshot(nullptr),
        dom_size_snapshot(nullptr),
        world_results(nullptr),
        world_status(nullptr),
        world_locks(nullptr),
        queue_slots(nullptr),
        enqueue_pos(nullptr),
        dequeue_pos(nullptr),
        queue_capacity(0),
        queue_mask(0),
        pending_tasks(nullptr),
        processed_tasks(nullptr),
        overflow_count(nullptr),
        unknown_count(nullptr),
        cta_pop_batch(4),
        local_buffer_capacity(64),
        lock_retry_limit(8),
        lock_backoff(32),
        enable_cid_grouping(0),
        enable_parallel_group_check(0),
        group_warps_per_cta(4),
        group_degrade_threshold(1),
        enable_world_owner(0),
        enable_world_stealing(0),
        world_cursor(nullptr),
        enable_ow1_frontier_scatter(0),
        ow1_min_degree(32),
        ow1_scatter_mode(1),
        ow1_force_scatter(0),
        enable_cid_microbatch_profile(0),
        microbatch_profile_interval(64),
        total_constraint_checks(nullptr),
        total_deletions(nullptr),
        stale_drop_count(nullptr),
        lock_fail_count(nullptr),
        lock_retry_count(nullptr),
        bucket_count(nullptr),
        bucket_task_sum(nullptr),
        bucket_active_warp_sum(nullptr),
        frontier_pop_count(nullptr),
        frontier_scan_steps(nullptr),
        ow1_scatter_calls(nullptr),
        ow1_fallback_calls(nullptr),
        ow1_word_leader_writes(nullptr),
        microbatch_rounds(nullptr),
        microbatch_sel_ge2_rounds(nullptr),
        microbatch_sel_sum(nullptr) {}
};

// FQ-PT 运行统计（Host 侧）
struct FQPTStatistics {
  int total_worlds = 0;
  int dwo_worlds = 0;
  int ok_worlds = 0;
  int unknown_worlds = 0;

  unsigned long long processed_tasks = 0;
  unsigned long long overflow_count = 0;
  unsigned long long constraint_checks = 0;
  unsigned long long deletions = 0;
  unsigned long long stale_drop_count = 0;
  unsigned long long lock_fail_count = 0;
  unsigned long long lock_retry_count = 0;
  double avg_bucket_size = 0.0;
  double avg_bucket_utilization = 0.0;
  unsigned long long frontier_pop_count = 0;
  unsigned long long frontier_scan_steps = 0;
  double avg_frontier_scan_steps = 0.0;
  unsigned long long ow1_scatter_calls = 0;
  unsigned long long ow1_fallback_calls = 0;
  unsigned long long ow1_word_leader_writes = 0;
  unsigned long long microbatch_rounds = 0;
  unsigned long long microbatch_sel_ge2_rounds = 0;
  unsigned long long microbatch_sel_sum = 0;
  double avg_sel_count = 0.0;
};

// FQ-PT Baseline Host 管理器
class FQPTBaselineManager {
 public:
  explicit FQPTBaselineManager(GModel* model, int num_blocks = -1);
  ~FQPTBaselineManager();

  FQPTBaselineManager(const FQPTBaselineManager&) = delete;
  FQPTBaselineManager& operator=(const FQPTBaselineManager&) = delete;

  void AddTask(int var_id, int value);
  void Clear();
  int GetTaskCount() const { return static_cast<int>(task_queue_.size()); }

  int Execute(std::vector<int>& failed_vars,
              std::vector<int>& failed_values,
              std::vector<int>* unknown_vars,
              std::vector<int>* unknown_values);
  int Execute(std::vector<int>& failed_vars,
              std::vector<int>& failed_values) {
    return Execute(failed_vars, failed_values, nullptr, nullptr);
  }

  void EnableStats(bool enabled) { stats_enabled_ = enabled; }

  void SetQueueCapacity(int capacity_pow2);
  void SetCtaPopBatch(int k) { cta_pop_batch_ = std::max(1, k); }
  void SetLocalBufferCapacity(int l) { local_buffer_capacity_ = std::max(8, l); }
  void SetLockRetryLimit(int n) { lock_retry_limit_ = std::max(1, n); }
  void SetLockBackoff(int n) { lock_backoff_ = std::max(0, n); }
  void SetEnableCidGrouping(bool enabled) {
    enable_cid_grouping_ = enabled;
  }
  void SetEnableParallelGroupCheck(bool enabled) {
    enable_parallel_group_check_ = enabled;
  }
  void SetGroupWarpsPerCta(int warps) { group_warps_per_cta_ = std::max(1, warps); }
  void SetGroupDegradeThreshold(int threshold) {
    group_degrade_threshold_ = std::max(1, threshold);
  }
  void SetEnableWorldOwner(bool enabled);
  void SetEnableWorldStealing(bool enabled);
  void SetEnableOW1FrontierScatter(bool enabled) {
    enable_ow1_frontier_scatter_ = enabled;
  }
  void SetOW1MinDegree(int degree) { ow1_min_degree_ = std::max(1, degree); }
  void SetOW1ScatterMode(int mode) { ow1_scatter_mode_ = std::clamp(mode, 0, 2); }
  void SetOW1ForceScatter(bool enabled) { ow1_force_scatter_ = enabled; }
  void SetEnableCidMicrobatchProfile(bool enabled) {
    enable_cid_microbatch_profile_ = enabled;
  }
  void SetMicrobatchProfileInterval(int interval) {
    microbatch_profile_interval_ = std::max(1, interval);
  }

  int GetNumBlocks() const { return num_blocks_; }
  const FQPTStatistics& GetLastStatistics() const { return last_stats_; }

  static int ComputeRecommendedNumBlocks(int device_id = 0);
  static int RoundUpPow2(int v);

 private:
  void AllocateMemory();
  void FreeMemory();
  void EnsureTaskCapacity(int required_tasks);
  void SaveSnapshot();
  void InitializeWorldsFromSnapshot(int num_worlds);
  void InitializeGlobalQueueWithSeedTasks(int num_worlds);
  void LaunchKernel(int num_worlds);
  int CollectResults(int num_worlds,
                     std::vector<int>& failed_vars,
                     std::vector<int>& failed_values,
                     std::vector<int>* unknown_vars,
                     std::vector<int>* unknown_values);

  GModel* model_ = nullptr;
  int num_blocks_ = 0;
  int max_tasks_ = 0;
  std::vector<ProbeTask> task_queue_;

  // ========== 运行时配置 ==========
  int queue_capacity_ = 0;         // 必须是 2 的幂
  int cta_pop_batch_ = 4;
  int local_buffer_capacity_ = 64;
  int lock_retry_limit_ = 8;
  int lock_backoff_ = 32;
  bool enable_cid_grouping_ = false;
  bool enable_parallel_group_check_ = false;
  int group_warps_per_cta_ = 4;
  int group_degrade_threshold_ = 1;
  bool enable_world_owner_ = false;
  bool enable_world_stealing_ = false;
  bool enable_ow1_frontier_scatter_ = false;
  int ow1_min_degree_ = 32;
  int ow1_scatter_mode_ = 1;
  bool ow1_force_scatter_ = false;
  bool enable_cid_microbatch_profile_ = false;
  int microbatch_profile_interval_ = 64;
  bool stats_enabled_ = true;

  // ========== Device 端内存（统一内存）==========
  ProbeTask* d_tasks_ = nullptr;               // [max_tasks_]
  bool* d_world_results_ = nullptr;            // [max_tasks_]
  int* d_world_status_ = nullptr;              // [max_tasks_]
  u32* d_snapshot_ = nullptr;                  // [num_vars * bit_dom_int_size]
  int* d_dom_size_snapshot_ = nullptr;         // [num_vars]

  WorldWorkspace* d_workspaces_ = nullptr;     // [max_tasks_]
  u32* d_ws_bitdom_ = nullptr;                 // [max_tasks_ * num_vars * bit_dom_int_size]
  int* d_ws_dom_size_ = nullptr;               // [max_tasks_ * num_vars]
  u32* d_ws_frontier_A_ = nullptr;             // [max_tasks_ * bitmap_size_words]
  u32* d_ws_frontier_B_ = nullptr;             // [max_tasks_ * bitmap_size_words]
  int* d_world_locks_ = nullptr;               // [max_tasks_]

  FQPTRingSlot* d_queue_slots_ = nullptr;      // [queue_capacity_]
  unsigned long long* d_enqueue_pos_ = nullptr;
  unsigned long long* d_dequeue_pos_ = nullptr;
  unsigned long long* d_pending_tasks_ = nullptr;
  unsigned long long* d_processed_tasks_ = nullptr;
  unsigned long long* d_overflow_count_ = nullptr;
  unsigned long long* d_unknown_count_ = nullptr;

  unsigned long long* d_total_constraint_checks_ = nullptr;
  unsigned long long* d_total_deletions_ = nullptr;
  unsigned long long* d_stale_drop_count_ = nullptr;
  unsigned long long* d_lock_fail_count_ = nullptr;
  unsigned long long* d_lock_retry_count_ = nullptr;
  unsigned long long* d_bucket_count_ = nullptr;
  unsigned long long* d_bucket_task_sum_ = nullptr;
  unsigned long long* d_bucket_active_warp_sum_ = nullptr;
  unsigned long long* d_frontier_pop_count_ = nullptr;
  unsigned long long* d_frontier_scan_steps_ = nullptr;
  unsigned long long* d_ow1_scatter_calls_ = nullptr;
  unsigned long long* d_ow1_fallback_calls_ = nullptr;
  unsigned long long* d_ow1_word_leader_writes_ = nullptr;
  unsigned long long* d_microbatch_rounds_ = nullptr;
  unsigned long long* d_microbatch_sel_ge2_rounds_ = nullptr;
  unsigned long long* d_microbatch_sel_sum_ = nullptr;
  unsigned int* d_world_cursor_ = nullptr;

  FQPTControl* d_control_ = nullptr;
  bool memory_allocated_ = false;

  FQPTStatistics last_stats_;
};

// FQ-PT Kernel Wrapper（实现在 GModel.cu）
void LaunchFQPTBaselineKernelWrapper(
    GModelData model_data,
    FQPTControl* control,
    int num_blocks);
void LaunchFQPTOwnerFrontierKernelWrapper(
    GModelData model_data,
    FQPTControl* control,
    int num_blocks);

}  // namespace cpim

#endif  // CPIM_BATCH_PROBE_MANAGER_H_
