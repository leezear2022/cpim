# GPU Batch AC 实施文档（Batch-1 方案）

## 文档信息

- **版本**: v1.0
- **创建时间**: 2025-12-25
- **作者**: Phase 1.6 GPU Batch AC Implementation
- **状态**: 设计完成，待实施
- **理论基础**: [Batch_AC.md](../planning/Batch_AC.md)
- **预计工期**: 2-4周

---

## 目录

1. [项目概述](#项目概述)
2. [技术方案](#技术方案)
3. [架构设计](#架构设计)
4. [数据结构详细设计](#数据结构详细设计)
5. [Kernel实现详解](#kernel实现详解)
6. [API设计](#api设计)
7. [实施步骤](#实施步骤)
8. [测试计划](#测试计划)
9. [性能分析](#性能分析)
10. [常见问题](#常见问题)

---

## 项目概述

### 1.1 目标

实现 GPU 加速的批量弧一致性（Batch AC）传播，用于优化 SAC（Singleton Arc Consistency）性能。通过在 GPU 上批量并行处理多个单例世界检查（singleton probe），显著降低 kernel 启动开销，实现 10-20x 加速。

### 1.2 核心思想

**从理论到实践**（参考 [Batch_AC.md](../planning/Batch_AC.md)）：

- **理论层**: SAC-checking pass = 对一批单例世界各自运行 AC 到不动点
- **算子层**: 单世界 AC = Boolean bitGEMV，批量世界 = Boolean bitGEMM
- **实现层**: 持久化 kernel 连续处理 N 个 probe 任务，摊薄启动开销

**Batch-1 vs Batch-2**：

| 方案 | Batch-1（本文档） | Batch-2（未来） |
|------|------------------|----------------|
| 核心思想 | 时间维batch：串行处理多任务 | 空间维batch：并行多世界 |
| 算子特性 | bitGEMV序列 | 真正的bitGEMM |
| 内存需求 | 单域 + 快照（~4-10KB） | 多世界域（10-100MB） |
| 实现复杂度 | 低（复用>95%） | 高（重写kernel） |
| 开发周期 | 1-2周 | 3-4月 |
| 预期加速 | 10-20x | 50-100x |

**选择Batch-1的理由**：
1. ✅ 快速落地（1-2周 vs 3-4月）
2. ✅ 低风险（100%复用现有kernel代码）
3. ✅ Jetson友好（内存压力极小）
4. ✅ 显著收益（10-20x已足够满足大部分需求）

### 1.3 关键指标

**Queens-12（144个probe任务）**：
- Kernel启动：144次 → 2次
- 执行时间：100ms（CPU） → 5-7ms（GPU Batch-1）
- 加速比：14-20x

**Langford-3-9（~270个probe任务）**：
- Kernel启动：270次 → 1-2次
- 执行时间：1s（CPU） → 30-40ms（GPU Batch-1）
- 加速比：25-33x

---

## 技术方案

### 2.1 整体架构流程

```
┌─────────────────────────────────────────────────────────────┐
│ CPU Side (SAC Wrapper)                                      │
│                                                              │
│  [1] 收集 Probe 任务                                         │
│      BatchProbeManager batch_mgr(gmodel, 256);              │
│      for each (var, val):                                   │
│          batch_mgr.AddTask(var, val);                       │
│                                                              │
│  [2] 保存域快照                                             │
│      batch_mgr.SaveSnapshot();  // bitDom → d_snapshot      │
│                                                              │
│  [3] 执行批量 Probe                                          │
│      vector<int> failed_vars, failed_vals;                  │
│      batch_mgr.ExecuteBatch(failed_vars, failed_vals);      │
│          └─> LaunchBatchProbeKernel(tasks, results)         │
│                                                              │
│  [4] 应用删值并重新传播                                      │
│      for (i : failed):                                      │
│          vars[failed_vars[i]]->RemoveValue(failed_vals[i]); │
│      kernel->enforce();                                     │
└────────────────┬────────────────────────────────────────────┘
                 │ cudaLaunchCooperativeKernel
                 ▼
┌─────────────────────────────────────────────────────────────┐
│ GPU Side (PersistentBatchProbeKernel)                       │
│                                                              │
│  cg::grid_group grid = cg::this_grid();                     │
│                                                              │
│  while (true) {                                             │
│    // [A] 获取任务（grid级原子）                             │
│    task_idx = atomicAdd(&control->current_task_index, 1);   │
│    if (task_idx >= num_tasks) break;                        │
│    ProbeTask task = tasks[task_idx];                        │
│                                                              │
│    // [B] 并行恢复快照                                       │
│    for (i = tid; i < total_words; i += total_threads):      │
│        bitDom[i] = domain_snapshot[i];                      │
│    grid.sync();                                             │
│                                                              │
│    // [C] 单例赋值 + 初始化 Frontier                         │
│    if (leader_thread):                                      │
│        bitDom[var] = {value};  // 清空并设置单值             │
│        InitializeFrontierForVariable(var);                  │
│        ResetGACControl();                                   │
│    grid.sync();                                             │
│                                                              │
│    // [D] GAC 到不动点（100%复用现有逻辑）                    │
│    RunGACToFixpoint(model, control, shmem, max_iters);      │
│    //   └─> while (frontier_nonempty):                      │
│    //           ExecuteConstraintCheck_BpC(...)  ← 复用      │
│    //           FetchNextCidFromBitmap(...)      ← 复用      │
│    //           PropagateVarToNextBitmap(...)    ← 复用      │
│    //           Swap frontiers + check convergence          │
│                                                              │
│    // [E] 记录结果                                           │
│    if (leader_thread):                                      │
│        results[task_idx] = !inconsistent_flag;              │
│    grid.sync();                                             │
│  }                                                           │
└─────────────────────────────────────────────────────────────┘
```

### 2.2 关键技术点

#### 2.2.1 快照恢复（Snapshot Restore）

**为什么不用Trail？**
- Trail 是单线程设计，不支持并行多世界
- Probe 深度=1，快照比 Trail 更快
- 避免 per-world Trail 的复杂性

**快照策略**：
```cpp
// 保存（CPU端，一次性）
SaveSnapshot() {
  int total_words = num_vars * bit_dom_int_size;
  memcpy(d_snapshot, bitDom, total_words * sizeof(u32));
}

// 恢复（GPU端，每个probe前，所有线程并行）
RestoreSnapshot<<<grid, block>>>() {
  int tid = blockIdx.x * blockDim.x + threadIdx.x;
  int total_threads = gridDim.x * blockDim.x;
  for (int i = tid; i < total_words; i += total_threads) {
    bitDom[i] = domain_snapshot[i];
  }
}
```

**性能**：
- Queens-12: 48 bytes，恢复 <1μs
- Langford-3-9: 108 bytes，恢复 <2μs

#### 2.2.2 Cooperative Groups 全局同步

**为什么需要**：
- 确保所有block完成快照恢复后才开始GAC
- 确保GAC完成后才进入下一个probe

**使用方式**：
```cuda
cg::grid_group grid = cg::this_grid();

// 恢复快照
RestoreSnapshot(...);
grid.sync();  // ← 等待所有block完成

// GAC传播
RunGACToFixpoint(...);
grid.sync();  // ← 等待所有block完成
```

**要求**：
- 必须使用 `cudaLaunchCooperativeKernel()`
- 设备需支持 `cudaDevAttrCooperativeLaunch`
- Jetson Orin 支持✓

#### 2.2.3 代码复用策略

**100% 复用现有函数**：

| 函数 | 位置 | 用途 | 复用方式 |
|------|------|------|---------|
| ExecuteConstraintCheck_BpC | GModel.cu:688-805 | 单约束支持检查 | 直接调用 |
| FetchNextCidFromBitmap | GModel.cu:659-683 | Bitmap任务分发 | 直接调用 |
| PropagateVarToNextBitmap | GModel.cu:640-655 | 激活邻接约束 | 直接调用 |

**新增辅助函数**（封装现有逻辑）：

| 函数 | 位置 | 用途 | 实现方式 |
|------|------|------|---------|
| RunGACToFixpoint | GModel.cu（新增） | GAC迭代到不动点 | 封装PersistentGACKernel的主循环 |
| InitializeFrontierForVariable | GModel.cu（新增） | 初始化单变量frontier | 查询d_subscription激活邻接约束 |

---

## 数据结构详细设计

### 3.1 ProbeTask（探测任务）

**定义**（文件：`include/solver/gpu/batch_probe_manager.h`）：

```cpp
struct ProbeTask {
  int var_id;       // 变量 ID（[0, num_vars)）
  int value;        // 值（[0, max_dom_size)，统一值空间）
  int task_id;      // 任务序号（调试用，可选）

  // 默认构造
  __host__ __device__
  ProbeTask() : var_id(-1), value(-1), task_id(-1) {}

  // 带参数构造
  __host__ __device__
  ProbeTask(int v, int a, int id = -1)
      : var_id(v), value(a), task_id(id) {}
};
```

**内存布局**：
```
| var_id (4B) | value (4B) | task_id (4B) |  → 总计 12 bytes
```

**使用示例**：
```cpp
// CPU端收集任务
vector<ProbeTask> tasks;
for (int var = 0; var < num_vars; ++var) {
  if (assigned(var)) continue;
  for (int val = first_value(var); val != -1; val = next_value(var, val)) {
    tasks.emplace_back(var, val, tasks.size());
  }
}

// GPU端访问
__global__ void ProcessTasks(const ProbeTask* tasks, int num_tasks) {
  int tid = ...;
  if (tid >= num_tasks) return;
  ProbeTask task = tasks[tid];
  printf("Processing var=%d, val=%d\n", task.var_id, task.value);
}
```

### 3.2 BatchProbeControl（批量控制块）

**定义**（文件：`include/solver/gpu/batch_probe_manager.h`）：

```cpp
struct BatchProbeControl {
  // ===== 任务管理 =====
  int num_tasks;                    // 当前批次任务总数（只读）
  int current_task_index;           // 原子递增的任务索引（读写，原子操作）

  // ===== 快照与恢复 =====
  const u32* domain_snapshot;       // 域快照（共享，只读）

  // ===== 任务与结果 =====
  const ProbeTask* tasks;           // 任务数组 [num_tasks]（只读）
  bool* results;                    // 结果数组 [num_tasks]（写入）

  // ===== GAC 控制（嵌入 PersistentGACControl 字段） =====
  int inconsistent_flag;            // 当前 probe 不一致标志（原子读写）
  int scanner_index;                // Frontier bitmap 扫描游标（原子递增）
  unsigned long long deletions;     // 当前 probe 删值数（原子累加）
  int iterations;                   // 当前 probe GAC 迭代数（递增）
  int converged_flag;               // 当前 probe 收敛标志（原子设置）
  int frontier_nonempty;            // Frontier 非空标志（原子OR）

  // ===== Frontier Bitmaps（双缓冲） =====
  u32* frontier_A;                  // Current Frontier
  u32* frontier_B;                  // Next Frontier
  int bitmap_size_words;            // Bitmap 大小（words）

  // 构造函数（Host端初始化）
  __host__
  BatchProbeControl()
      : num_tasks(0),
        current_task_index(0),
        domain_snapshot(nullptr),
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
        bitmap_size_words(0) {}
};
```

**内存占用**：
```
sizeof(BatchProbeControl) ≈ 128 bytes（固定大小）
```

**初始化示例**（每个probe前重置GAC控制字段）：
```cuda
__device__ void ResetGACControl(BatchProbeControl* control) {
  if (threadIdx.x == 0 && blockIdx.x == 0) {
    control->inconsistent_flag = 0;
    control->scanner_index = 0;
    control->deletions = 0;
    control->iterations = 0;
    control->converged_flag = 0;
    control->frontier_nonempty = 0;
  }
}
```

### 3.3 BatchProbeManager（Host端管理器）

**定义**（文件：`include/solver/gpu/batch_probe_manager.h`）：

```cpp
namespace cpim {

class BatchProbeManager {
 public:
  // 构造函数
  // @param model: GModel 指针（必须已初始化）
  // @param max_batch_size: 最大批次大小（默认256，可调）
  explicit BatchProbeManager(GModel* model, int max_batch_size = 256);

  // 析构函数（释放 GPU 内存）
  ~BatchProbeManager();

  // 禁止拷贝
  BatchProbeManager(const BatchProbeManager&) = delete;
  BatchProbeManager& operator=(const BatchProbeManager&) = delete;

  // ===== 任务管理 =====

  // 添加 probe 任务到队列
  // @param var_id: 变量 ID
  // @param value: 值（统一值空间）
  void AddTask(int var_id, int value);

  // 执行批量 probe（核心接口）
  // @param failed_vars: 输出 - 失败任务的变量 ID
  // @param failed_values: 输出 - 失败任务的值
  // @return 失败任务数
  int ExecuteBatch(std::vector<int>& failed_vars,
                   std::vector<int>& failed_values);

  // 清空任务队列
  void Clear();

  // ===== 统计信息 =====

  int GetTotalProbes() const { return total_probes_; }
  int GetBatchCount() const { return batch_count_; }
  int GetMaxBatchSize() const { return max_batch_size_; }

 private:
  // ===== 成员变量 =====
  GModel* model_;                       // GModel 指针（外部拥有）
  int max_batch_size_;                  // 最大批次大小
  std::vector<ProbeTask> task_queue_;   // CPU 端任务队列

  // GPU 内存（统一内存，cudaMallocManaged）
  ProbeTask* d_tasks_;                  // GPU 任务数组
  bool* d_results_;                     // GPU 结果数组
  u32* d_snapshot_;                     // GPU 域快照
  BatchProbeControl* d_control_;        // GPU 控制块
  u32* d_frontier_A_;                   // GPU Frontier A
  u32* d_frontier_B_;                   // GPU Frontier B

  // 统计信息
  int total_probes_;                    // 累计 probe 数
  int batch_count_;                     // 累计批次数

  // ===== 内部方法 =====

  // GPU 内存分配
  void AllocateGPUMemory();

  // GPU 内存释放
  void FreeGPUMemory();

  // 保存域快照
  void SaveSnapshot();

  // 启动批量 probe kernel
  void LaunchBatchProbeKernel(int num_tasks);
};

}  // namespace cpim
```

**使用示例**：
```cpp
// 1. 创建管理器
BatchProbeManager batch_mgr(gmodel, 256);

// 2. 收集任务
for (auto* var : vars) {
  if (var->assigned()) continue;
  for (int val = var->first(); val != -1; val = var->next(val)) {
    batch_mgr.AddTask(var->id(), val);
  }
}

// 3. 执行批量 probe
std::vector<int> failed_vars, failed_vals;
int num_failed = batch_mgr.ExecuteBatch(failed_vars, failed_vals);

// 4. 应用删值
for (size_t i = 0; i < failed_vars.size(); ++i) {
  vars[failed_vars[i]]->RemoveValue(failed_vals[i]);
}

// 5. 重新传播
ac_kernel->enforce();
```

---

## Kernel实现详解

### 4.1 PersistentBatchProbeKernel（主入口）

**签名**（文件：`src/solver/gpu/GModel.cu`）：

```cuda
__global__
void PersistentBatchProbeKernel(
    GModelData model,                    // GModel 数据视图（只读）
    BatchProbeControl* control,          // 控制块（读写）
    int max_iterations_per_probe)        // 每个 probe 最大 GAC 迭代数
```

**完整实现**：

```cuda
__global__
void PersistentBatchProbeKernel(
    GModelData model,
    BatchProbeControl* control,
    int max_iterations_per_probe) {

  // 初始化 Cooperative Groups
  cg::grid_group grid = cg::this_grid();

  // 共享内存（用于 ExecuteConstraintCheck_BpC）
  extern __shared__ u32 shmem[];

  // ===== 主循环：连续处理多个 probe =====
  while (true) {
    // ----- [1] 获取下一个任务（grid 级原子） -----
    __shared__ int task_idx;
    __shared__ ProbeTask task;

    if (blockIdx.x == 0 && threadIdx.x == 0) {
      task_idx = atomicAdd(&control->current_task_index, 1);
      if (task_idx < control->num_tasks) {
        task = control->tasks[task_idx];
      }
    }
    grid.sync();

    // 检查是否所有任务处理完毕
    if (task_idx >= control->num_tasks) {
      break;  // 退出主循环
    }

    // ----- [2] 并行恢复快照 -----
    int tid = blockIdx.x * blockDim.x + threadIdx.x;
    int total_threads = gridDim.x * blockDim.x;
    int total_words = model.bit_doms_int_size;

    for (int idx = tid; idx < total_words; idx += total_threads) {
      model.bitDom[idx] = control->domain_snapshot[idx];
    }
    grid.sync();

    // ----- [3] 单例赋值 + 初始化 Frontier -----
    if (blockIdx.x == 0 && threadIdx.x == 0) {
      // 3.1 清空变量域
      int base = task.var_id * model.bit_dom_int_size;
      for (int w = 0; w < model.bit_dom_int_size; ++w) {
        model.bitDom[base + w] = 0u;
      }

      // 3.2 设置单例值
      int word = task.value / 32;
      int bit = task.value % 32;
      model.bitDom[base + word] = (1u << bit);
      model.d_cur_dom_size[task.var_id] = 1;

      // 3.3 初始化 frontier（只激活 var_id 的邻接约束）
      InitializeFrontierForVariable(model, control, task.var_id);

      // 3.4 重置 GAC 控制块
      control->inconsistent_flag = 0;
      control->scanner_index = 0;
      control->deletions = 0;
      control->iterations = 0;
      control->converged_flag = 0;
      control->frontier_nonempty = 0;
    }
    grid.sync();

    // ----- [4] GAC 传播到不动点（100% 复用现有逻辑） -----
    RunGACToFixpoint(model, control, shmem, max_iterations_per_probe);

    // ----- [5] 记录结果 -----
    if (blockIdx.x == 0 && threadIdx.x == 0) {
      // true = 一致（可行），false = 不一致（需删除）
      control->results[task_idx] = (control->inconsistent_flag == 0);
    }

    grid.sync();
  }
}
```

**关键点**：
1. **grid.sync()**: 5次全局同步（任务获取、快照恢复、赋值、GAC、结果记录）
2. **leader thread**: 单线程负责赋值和控制块重置（避免原子操作开销）
3. **并行恢复**: 所有线程协作拷贝快照（Queens-12仅48B，<1μs）

### 4.2 RunGACToFixpoint（GAC迭代封装）

**签名**：

```cuda
__device__
void RunGACToFixpoint(
    const GModelData& model,
    BatchProbeControl* control,
    u32* shmem,
    int max_iterations)
```

**实现**（封装 PersistentGACKernel 的主循环逻辑）：

```cuda
__device__
void RunGACToFixpoint(
    const GModelData& model,
    BatchProbeControl* control,
    u32* shmem,
    int max_iterations) {

  cg::grid_group grid = cg::this_grid();
  int local_word = 0;
  int local_offset = 0;

  // 主迭代循环
  for (int iter = 0; iter < max_iterations; ++iter) {
    // ===== [1] 重置扫描索引 =====
    if (blockIdx.x == 0 && threadIdx.x == 0) {
      control->scanner_index = 0;
      control->iterations++;
    }
    grid.sync();

    // ===== [2] 处理当前 frontier =====
    while (true) {
      __shared__ int cid_shared;

      // 获取下一个约束 ID
      if (threadIdx.x == 0) {
        int cid = FetchNextCidFromBitmap(  // ← 复用现有函数
            control->frontier_A,
            control->bitmap_size_words,
            &control->scanner_index,
            local_word,
            local_offset);
        cid_shared = cid;
      }
      __syncthreads();

      int cid = cid_shared;
      if (cid < 0 || cid >= model.num_constraints) {
        break;  // 当前 block 没有更多任务
      }

      // 执行约束传播
      PropagateResult r = ExecuteConstraintCheck_BpC(  // ← 复用现有函数
          cid, model, shmem);

      // 记录删值和激活邻接约束
      if (threadIdx.x == 0) {
        if (r.deletions > 0) {
          atomicAdd(&control->deletions, (unsigned long long)r.deletions);
          const int2 scope = model.constraint_scopes[cid];

          if (r.x_changed) {
            PropagateVarToNextBitmap(scope.x, model, control->frontier_B);  // ← 复用
          }
          if (r.y_changed) {
            PropagateVarToNextBitmap(scope.y, model, control->frontier_B);  // ← 复用
          }
        }

        if (r.inconsistent) {
          atomicExch(&control->inconsistent_flag, 1);
        }
      }
      __syncthreads();

      // DWO 提前退出
      if (control->inconsistent_flag) {
        break;
      }
    }

    // ===== [3] 全局同步 =====
    grid.sync();

    if (control->inconsistent_flag) {
      break;  // 检测到不一致，退出迭代
    }

    // ===== [4] 收敛检测（检查 frontier_B 是否为空） =====
    __shared__ int block_nonempty;
    if (threadIdx.x == 0) {
      block_nonempty = 0;
    }
    __syncthreads();

    // 每个 block 检查部分 words
    int words_per_block = (control->bitmap_size_words + gridDim.x - 1) / gridDim.x;
    int start = blockIdx.x * words_per_block;
    int end = min(start + words_per_block, control->bitmap_size_words);

    for (int w = start + (int)threadIdx.x; w < end; w += (int)blockDim.x) {
      if (control->frontier_B[w] != 0u) {
        atomicExch(&block_nonempty, 1);
        break;
      }
    }
    __syncthreads();

    if (threadIdx.x == 0) {
      atomicOr(&control->frontier_nonempty, block_nonempty);
    }

    grid.sync();

    // 检查是否收敛
    if (control->frontier_nonempty == 0) {
      if (blockIdx.x == 0 && threadIdx.x == 0) {
        control->converged_flag = 1;
      }
      break;  // 达到不动点，退出迭代
    }

    // ===== [5] Swap 双缓冲 =====
    if (blockIdx.x == 0 && threadIdx.x == 0) {
      u32* temp = control->frontier_A;
      control->frontier_A = control->frontier_B;
      control->frontier_B = temp;
      control->frontier_nonempty = 0;
    }

    grid.sync();

    // ===== [6] 清空新的 next frontier =====
    tid = blockIdx.x * blockDim.x + threadIdx.x;
    total_threads = gridDim.x * blockDim.x;
    for (int w = tid; w < control->bitmap_size_words; w += total_threads) {
      control->frontier_B[w] = 0u;
    }

    grid.sync();
  }
}
```

**代码复用率**: >95%（完全照搬 PersistentGACKernel 的逻辑）

### 4.3 InitializeFrontierForVariable（初始化Frontier）

**签名**：

```cuda
__device__
void InitializeFrontierForVariable(
    const GModelData& model,
    BatchProbeControl* control,
    int var_id)
```

**实现**：

```cuda
__device__
void InitializeFrontierForVariable(
    const GModelData& model,
    BatchProbeControl* control,
    int var_id) {

  // 注意：此函数必须由单个线程调用（leader thread）

  // 清空 frontier_A
  for (int w = 0; w < control->bitmap_size_words; ++w) {
    control->frontier_A[w] = 0u;
  }

  // 查询变量的邻接约束（CSR 格式）
  int start = model.d_subscription_offset[var_id];
  int end = model.d_subscription_offset[var_id + 1];

  for (int i = start; i < end; ++i) {
    uint3 entry = model.d_subscription[i];
    int cid = entry.z;  // 约束 ID 在 z 分量

    // 激活该约束
    int w = cid / 32;
    int b = cid % 32;
    if (w < control->bitmap_size_words) {
      control->frontier_A[w] |= (1u << b);
    }
  }
}
```

**说明**：
- 只激活 var_id 邻接的约束（增量传播）
- 时间复杂度：O(degree(var))，通常 <10 个约束

---

## API设计

### 5.1 BatchProbeManager 公共接口

#### 5.1.1 构造与析构

```cpp
// 构造函数
BatchProbeManager::BatchProbeManager(GModel* model, int max_batch_size)
    : model_(model),
      max_batch_size_(max_batch_size),
      d_tasks_(nullptr),
      d_results_(nullptr),
      d_snapshot_(nullptr),
      d_control_(nullptr),
      d_frontier_A_(nullptr),
      d_frontier_B_(nullptr),
      total_probes_(0),
      batch_count_(0) {

  if (!model_) {
    throw std::runtime_error("[BatchProbeManager] GModel is null!");
  }

  AllocateGPUMemory();
}

// 析构函数
BatchProbeManager::~BatchProbeManager() {
  FreeGPUMemory();
}
```

#### 5.1.2 AddTask（添加任务）

```cpp
void BatchProbeManager::AddTask(int var_id, int value) {
  if (var_id < 0 || var_id >= model_->num_vars) {
    throw std::runtime_error(
        "[BatchProbeManager::AddTask] Invalid var_id: " + std::to_string(var_id));
  }

  if (value < 0 || value >= model_->max_dom_size) {
    throw std::runtime_error(
        "[BatchProbeManager::AddTask] Invalid value: " + std::to_string(value));
  }

  task_queue_.emplace_back(var_id, value, static_cast<int>(task_queue_.size()));
}
```

#### 5.1.3 ExecuteBatch（执行批量probe）

```cpp
int BatchProbeManager::ExecuteBatch(
    std::vector<int>& failed_vars,
    std::vector<int>& failed_values) {

  failed_vars.clear();
  failed_values.clear();

  if (task_queue_.empty()) {
    return 0;
  }

  const int num_tasks = static_cast<int>(task_queue_.size());

  // [1] 保存域快照
  SaveSnapshot();

  // [2] 拷贝任务到 GPU
  std::memcpy(d_tasks_, task_queue_.data(), num_tasks * sizeof(ProbeTask));

  // [3] 启动 batch probe kernel
  LaunchBatchProbeKernel(num_tasks);

  // [4] 收集失败结果
  for (int i = 0; i < num_tasks; ++i) {
    if (!d_results_[i]) {  // false = 失败（不一致）
      failed_vars.push_back(task_queue_[i].var_id);
      failed_values.push_back(task_queue_[i].value);
    }
  }

  // [5] 更新统计
  total_probes_ += num_tasks;
  batch_count_++;

  // [6] 清空任务队列
  task_queue_.clear();

  return static_cast<int>(failed_vars.size());
}
```

#### 5.1.4 Clear（清空任务队列）

```cpp
void BatchProbeManager::Clear() {
  task_queue_.clear();
}
```

### 5.2 内部方法

#### 5.2.1 AllocateGPUMemory（分配GPU内存）

```cpp
void BatchProbeManager::AllocateGPUMemory() {
  cudaError_t err;

  // [1] 任务数组
  err = cudaMallocManaged(&d_tasks_, max_batch_size_ * sizeof(ProbeTask));
  if (err != cudaSuccess) {
    throw std::runtime_error(
        "[BatchProbeManager] Failed to allocate d_tasks_: " +
        std::string(cudaGetErrorString(err)));
  }

  // [2] 结果数组
  err = cudaMallocManaged(&d_results_, max_batch_size_ * sizeof(bool));
  if (err != cudaSuccess) {
    cudaFree(d_tasks_);
    throw std::runtime_error(
        "[BatchProbeManager] Failed to allocate d_results_: " +
        std::string(cudaGetErrorString(err)));
  }

  // [3] 域快照
  const int snapshot_size = model_->bit_doms_int_size * sizeof(u32);
  err = cudaMallocManaged(&d_snapshot_, snapshot_size);
  if (err != cudaSuccess) {
    cudaFree(d_tasks_);
    cudaFree(d_results_);
    throw std::runtime_error(
        "[BatchProbeManager] Failed to allocate d_snapshot_: " +
        std::string(cudaGetErrorString(err)));
  }

  // [4] 控制块
  err = cudaMallocManaged(&d_control_, sizeof(BatchProbeControl));
  if (err != cudaSuccess) {
    cudaFree(d_tasks_);
    cudaFree(d_results_);
    cudaFree(d_snapshot_);
    throw std::runtime_error(
        "[BatchProbeManager] Failed to allocate d_control_: " +
        std::string(cudaGetErrorString(err)));
  }

  // [5] Frontier bitmaps
  const int bitmap_size_words = (model_->num_constraints + 31) / 32;
  const int bitmap_bytes = bitmap_size_words * sizeof(u32);

  err = cudaMallocManaged(&d_frontier_A_, bitmap_bytes);
  if (err != cudaSuccess) {
    cudaFree(d_tasks_);
    cudaFree(d_results_);
    cudaFree(d_snapshot_);
    cudaFree(d_control_);
    throw std::runtime_error(
        "[BatchProbeManager] Failed to allocate d_frontier_A_: " +
        std::string(cudaGetErrorString(err)));
  }

  err = cudaMallocManaged(&d_frontier_B_, bitmap_bytes);
  if (err != cudaSuccess) {
    cudaFree(d_tasks_);
    cudaFree(d_results_);
    cudaFree(d_snapshot_);
    cudaFree(d_control_);
    cudaFree(d_frontier_A_);
    throw std::runtime_error(
        "[BatchProbeManager] Failed to allocate d_frontier_B_: " +
        std::string(cudaGetErrorString(err)));
  }

  // [6] 初始化控制块
  new (d_control_) BatchProbeControl();
  d_control_->frontier_A = d_frontier_A_;
  d_control_->frontier_B = d_frontier_B_;
  d_control_->bitmap_size_words = bitmap_size_words;
}
```

#### 5.2.2 FreeGPUMemory（释放GPU内存）

```cpp
void BatchProbeManager::FreeGPUMemory() {
  if (d_tasks_) {
    cudaFree(d_tasks_);
    d_tasks_ = nullptr;
  }
  if (d_results_) {
    cudaFree(d_results_);
    d_results_ = nullptr;
  }
  if (d_snapshot_) {
    cudaFree(d_snapshot_);
    d_snapshot_ = nullptr;
  }
  if (d_control_) {
    cudaFree(d_control_);
    d_control_ = nullptr;
  }
  if (d_frontier_A_) {
    cudaFree(d_frontier_A_);
    d_frontier_A_ = nullptr;
  }
  if (d_frontier_B_) {
    cudaFree(d_frontier_B_);
    d_frontier_B_ = nullptr;
  }
}
```

#### 5.2.3 SaveSnapshot（保存域快照）

```cpp
void BatchProbeManager::SaveSnapshot() {
  const int snapshot_size = model_->bit_doms_int_size * sizeof(u32);
  std::memcpy(d_snapshot_, model_->bitDom, snapshot_size);
}
```

#### 5.2.4 LaunchBatchProbeKernel（启动kernel）

```cpp
void BatchProbeManager::LaunchBatchProbeKernel(int num_tasks) {
  // [1] 检查 Cooperative Launch 支持
  int deviceId = 0;
  int supportsCoopLaunch = 0;
  cudaDeviceGetAttribute(&supportsCoopLaunch,
                         cudaDevAttrCooperativeLaunch, deviceId);

  if (!supportsCoopLaunch) {
    throw std::runtime_error(
        "[BatchProbeManager] Device does not support Cooperative Launch!");
  }

  // [2] 初始化控制块
  d_control_->num_tasks = num_tasks;
  d_control_->current_task_index = 0;
  d_control_->domain_snapshot = d_snapshot_;
  d_control_->tasks = d_tasks_;
  d_control_->results = d_results_;

  // [3] 计算 kernel 配置
  int threadsPerBlock = std::min(model_->max_dom_size, 256);
  size_t sharedMemBytes = 2 * model_->bit_dom_int_size * sizeof(u32);

  // 查询最大可驻留 blocks
  int maxBlocksPerSM = 0;
  cudaOccupancyMaxActiveBlocksPerMultiprocessor(
      &maxBlocksPerSM,
      PersistentBatchProbeKernel,
      threadsPerBlock,
      sharedMemBytes);

  cudaDeviceProp prop;
  cudaGetDeviceProperties(&prop, deviceId);

  int maxGridSize = maxBlocksPerSM * prop.multiProcessorCount;
  int desiredBlocks = std::min((model_->num_constraints + 31) / 32, 128);
  int actualGridSize = std::min(desiredBlocks, maxGridSize);

  // [4] 准备 kernel 参数
  GModelData md = model_->GetGModelDataView();
  int maxIterations = model_->num_vars * 10;  // 安全上限

  void* args[] = {
      &md,
      &d_control_,
      &maxIterations
  };

  // [5] 启动 Cooperative Kernel
  cudaError_t err = cudaLaunchCooperativeKernel(
      (void*)PersistentBatchProbeKernel,
      dim3(actualGridSize),
      dim3(threadsPerBlock),
      args,
      sharedMemBytes);

  if (err != cudaSuccess) {
    throw std::runtime_error(
        "[BatchProbeManager] Cooperative launch failed: " +
        std::string(cudaGetErrorString(err)));
  }

  // [6] 等待完成
  err = cudaDeviceSynchronize();
  if (err != cudaSuccess) {
    throw std::runtime_error(
        "[BatchProbeManager] Kernel execution failed: " +
        std::string(cudaGetErrorString(err)));
  }
}
```

---

## 实施步骤

### Week 1: 核心实现

#### Day 1-2: 创建数据结构和头文件

**任务**：
1. 创建 `include/solver/gpu/batch_probe_manager.h`
   - ProbeTask, BatchProbeControl 结构定义
   - BatchProbeManager 类声明
2. 修改 `include/GModel.cuh`
   - 添加前置声明

**验收**：
- 头文件编译通过
- 无循环依赖

#### Day 3-4: 实现 BatchProbeManager

**任务**：
1. 创建 `src/solver/gpu/batch_probe_manager.cu`
   - 实现构造/析构
   - 实现 AllocateGPUMemory/FreeGPUMemory
   - 实现 AddTask/Clear
   - 实现 SaveSnapshot
   - 实现 ExecuteBatch（暂时空实现）

**验收**：
- 内存分配/释放无泄漏
- 任务队列管理正常

#### Day 5-7: 实现 GPU Kernel

**任务**：
1. 修改 `src/solver/gpu/GModel.cu`
   - 实现 InitializeFrontierForVariable
   - 实现 RunGACToFixpoint（复用 PersistentGACKernel 逻辑）
   - 实现 PersistentBatchProbeKernel
2. 实现 BatchProbeManager::LaunchBatchProbeKernel

**验收**：
- Kernel 编译通过
- 无 CUDA 语法错误

#### Day 8: 修改 CMakeLists.txt 并编译

**任务**：
1. 修改 `CMakeLists.txt`
   - 添加 batch_probe_manager.cu 到 CUDA_SOURCES
2. 编译验证

**验收**：
- ✅ `cmake .. && make -j4` 编译通过
- ✅ 无链接错误

---

### Week 2: 测试与集成

#### Day 9-10: 创建测试程序

**任务**：
1. 创建 `apps/test_batch_probe.cpp`
   - 加载 Queens-4
   - 收集 probe 任务
   - 调用 BatchProbeManager
   - 打印结果

**验收**：
- 程序运行无崩溃
- Queens-4 正确性（节点数对比）

#### Day 11-12: Python 测试脚本

**任务**：
1. 创建 `tests/python/test_batch_probe.py`
   - 对比 CPU SAC vs GPU Batch-1
   - 测试 TIER 0 实例
   - 性能统计

**验收**：
- ✅ Queens-4 节点数匹配（P=4, N=0）
- ✅ Queens-12 加速 >10x

#### Day 13-14: 创建 MSAC-GPU 类

**任务**：
1. 创建 `src/solver/gpu/MSAC_GPU.cpp`
   - 继承 AC 接口
   - 实现 enforce() 方法（集成 BatchProbeManager）
2. 修改 `apps/cpim_test_parser.cpp`
   - 添加 `--ac_algorithm=msac_gpu` 选项

**验收**：
- ✅ TIER 0 全部通过（12/12）

---

### Week 3-4: 优化与扩展测试

#### Week 3: 性能优化

**任务**：
1. 自适应 batch size
   - 小问题使用小 batch（减少开销）
   - 大问题使用大 batch（充分利用 GPU）
2. Grid/Block 参数调优
   - 实验不同配置组合
   - 使用 nvprof 分析瓶颈

**验收**：
- Queens-12 加速 >15x
- Langford-3-9 加速 >15x

#### Week 4: 大规模测试

**任务**：
1. TIER 1 测试（39个实例）
2. 问题诊断和修复
3. 文档完善

**验收**：
- ✅ TIER 1 通过率 >90%
- ✅ 文档完整（实施文档、API文档）

---

## 测试计划

### 8.1 单元测试

#### 测试 1: BatchProbeManager 基础功能

**测试内容**：
- 构造/析构
- AddTask
- ExecuteBatch（空任务）
- Clear

**预期结果**：
- 无内存泄漏
- 无崩溃

#### 测试 2: 快照保存与恢复

**测试内容**：
- SaveSnapshot
- 启动 kernel 验证恢复正确性

**预期结果**：
- 快照前后域一致

#### 测试 3: 单 Probe 正确性

**测试内容**：
- Queens-4，单个 probe 任务
- 对比 CPU SAC 结果

**预期结果**：
- 结果一致（true/false）

### 8.2 集成测试

#### 测试 4: TIER 0（12个实例）

**测试实例**：
- test.xml
- queens-4_ext.xml
- queens-12_ext.xml
- langford-2-4-ext.xml
- langford-3-9-ext.xml
- driverlogw-01c-sat_ext.xml
- graphw-05_ext.xml
- rand-2-40-8-753-100-0_ext.xml
- rand-2-40-8-753-100-5_ext.xml
- rand-2-40-25-180-500-0_ext.xml
- rand-2-40-80-103-800-0_ext.xml
- BH-4-4-e-0_ext.xml

**验收标准**：
- ✅ 节点数匹配（Positives/Negatives）
- ✅ 加速比 >10x（Queens-12）

#### 测试 5: TIER 1（39个实例）

**测试范围**：
- Langford 系列（27个）
- 小型随机问题（12个）

**验收标准**：
- ✅ 通过率 >90%
- ✅ 平均加速比 >10x

### 8.3 性能测试

#### 测试 6: Kernel 启动次数

**测试方法**：
- 使用 nvprof 或 CUDA API 统计

**预期结果**：
- Queens-12: 144次 → 2次
- Langford-3-9: ~270次 → 1-2次

#### 测试 7: 端到端性能

**测试指标**：
- 总时间（包括 CPU 开销）
- GPU kernel 时间
- 内存拷贝时间

**预期结果**：
- Queens-12: <10ms
- Langford-3-9: <50ms

---

## 性能分析

### 9.1 内存开销分析

#### Queens-12

| 组件 | 大小 | 公式 |
|------|------|------|
| d_snapshot_ | 48 B | 12 vars × 1 word × 4 bytes |
| d_tasks_ (256) | 3 KB | 256 × 12 bytes |
| d_results_ (256) | 256 B | 256 × 1 byte |
| d_control_ | 128 B | sizeof(BatchProbeControl) |
| d_frontier_A_ | 16 B | (120 cons + 31) / 32 × 4 |
| d_frontier_B_ | 16 B | 同上 |
| **总计** | **~3.5 KB** | |

#### Langford-3-9

| 组件 | 大小 | 公式 |
|------|------|------|
| d_snapshot_ | 72 B | 27 vars × 1 word × 4 bytes |
| d_tasks_ (512) | 6 KB | 512 × 12 bytes |
| d_results_ (512) | 512 B | 512 × 1 byte |
| d_control_ | 128 B | 固定 |
| d_frontier_A_ | ~64 B | (~500 cons + 31) / 32 × 4 |
| d_frontier_B_ | ~64 B | 同上 |
| **总计** | **~7 KB** | |

**结论**：Jetson 上内存压力极小（<10KB）

### 9.2 时间开销分解

#### Queens-12（144 probe）

| 阶段 | CPU SAC | GPU Sequential | GPU Batch-1 |
|------|---------|---------------|-------------|
| Kernel 启动 | N/A | 144 × 50μs = 7.2ms | 2 × 50μs = 100μs |
| GAC 传播 | 100ms | 144 × 50μs = 7.2ms | 5ms |
| 其他开销 | ~50ms | ~2ms | ~1ms |
| **总计** | **150ms** | **16.4ms** | **6.1ms** |
| **加速比** | **1x** | **9x** | **24x** |

**说明**：GPU Batch-1 的加速来自两方面：
1. Kernel 启动减少：7.2ms → 0.1ms（节省 7.1ms）
2. GPU 并行传播：比 CPU 快 10x

### 9.3 瓶颈分析

**当前瓶颈**（按影响排序）：

1. **快照恢复**（并行拷贝，已优化）
   - Queens-12: 48 B，<1μs
   - Langford-3-9: 72 B，<2μs
   - **不是瓶颈**

2. **GAC 传播**（复用现有 kernel）
   - 受限于约束密度和域大小
   - **已接近最优**（现有 kernel 高度优化）

3. **Cooperative Launch 开销**
   - 单次启动 ~50μs
   - **可接受**（相比串行启动节省 >100x）

4. **内存访问**（统一内存）
   - Jetson 零拷贝架构
   - **不是瓶颈**

**进一步优化方向**（Batch-2）：
- 真正的 bitGEMM（支持矩阵在多世界间复用）
- 预期加速 50-100x（理论上限）

---

## 常见问题

### Q1: 为什么选择 Batch-1 而不是 Batch-2？

**A**: Batch-1 的快速落地和低风险：
- **开发周期**：1-2周 vs 3-4月
- **实现复杂度**：复用>95% vs 重写 kernel
- **内存需求**：<10KB vs 10-100MB
- **收益**：10-20x 已足够满足大部分需求
- **风险**：低（Cooperative Launch 有 fallback）

Batch-2 作为长期优化方向，待 Batch-1 成功验证后再考虑。

### Q2: Cooperative Launch 不支持怎么办？

**A**: 有两种 fallback 方案：
1. **降级到 sequential probe**：每个 probe 启动一个普通 kernel
2. **使用 BitmapGACKernel**：放弃 Cooperative Groups，改用 CPU 循环

实际上 Jetson Orin 支持 Cooperative Launch，不会触发 fallback。

### Q3: 内存不足怎么办？

**A**: 自适应 batch size：
```cpp
int adaptive_batch_size = std::min(max_batch_size, available_memory / task_size);
```

实际上 Batch-1 内存需求极小（<10KB），Jetson 上不会不足。

### Q4: 为什么不使用 Trail？

**A**: 三个原因：
1. **Trail 是单线程设计**：不支持并行多世界
2. **Probe 深度=1**：快照比 Trail 更快
3. **简化实现**：避免 per-world Trail 的复杂性

Batch-2 如果需要多世界，可考虑：
- per-world Trail
- 批量快照管理

### Q5: 如何验证正确性？

**A**: 三种验证方式：
1. **节点数对比**：GPU vs CPU SAC（Positives/Negatives 必须完全一致）
2. **单 probe 验证**：逐个 probe 对比结果（true/false）
3. **解验证**：最终解必须满足所有约束

### Q6: 性能不达预期怎么办？

**A**: 诊断步骤：
1. **nvprof 分析**：找出瓶颈（kernel 时间、内存拷贝等）
2. **调优参数**：Grid/Block 大小、batch size
3. **对比基准**：确认是 GPU 问题还是问题本身难度高
4. **考虑 Batch-2**：如果 Batch-1 已优化到极限但仍不满足需求

### Q7: 如何集成到现有求解器？

**A**: 两种方式：

**方式 1：替换 AC3bit**（推荐）
```cpp
// 修改 MAC 求解器
std::unique_ptr<AC> ac_kernel;
if (use_gpu) {
  ac_kernel = std::make_unique<MSAC_GPU>(model, gmodel);
} else {
  ac_kernel = std::make_unique<AC3bit>(model);
}
```

**方式 2：仅在 SAC 阶段使用**
```cpp
// 在 MSAC3bit 中调用
if (use_gpu && batch_mgr_) {
  batch_mgr_->ExecuteBatch(failed_vars, failed_vals);
} else {
  // 现有 CPU SAC 逻辑
}
```

### Q8: Batch-2 什么时候实施？

**A**: 满足以下条件时考虑：
1. **Batch-1 成功验证**（TIER 0/1 通过）
2. **用户需求**（10-20x 不满足，需要更高加速）
3. **内存预算**（Jetson 上有 100MB+ 可用）
4. **时间预算**（可投入 3-4 月开发周期）

实际上，Batch-1 的 10-20x 加速已能满足绝大多数场景。

---

## 附录

### A. 关键文件清单

#### 新增文件（6个）

1. **include/solver/gpu/batch_probe_manager.h** - BatchProbeManager 声明
2. **src/solver/gpu/batch_probe_manager.cu** - BatchProbeManager 实现
3. **src/solver/gpu/MSAC_GPU.cpp** - GPU SAC 算法实现
4. **apps/test_batch_probe.cpp** - 测试程序
5. **tests/python/test_batch_probe.py** - Python 测试脚本
6. **docs/implementation/BATCH_AC_GPU_IMPLEMENTATION.md** - 本文档

#### 修改文件（3个）

1. **src/solver/gpu/GModel.cu**
   - 添加：PersistentBatchProbeKernel（~150行，第1050行后）
   - 添加：RunGACToFixpoint（~80行）
   - 添加：InitializeFrontierForVariable（~30行）

2. **include/GModel.cuh**
   - 添加：前置声明（BatchProbeControl, ProbeTask）
   - 位置：第50行附近（结构体定义区）

3. **CMakeLists.txt**
   - 添加：batch_probe_manager.cu 到 CUDA_SOURCES
   - 添加：test_batch_probe 到 apps

### B. 参考文档

- [Batch_AC.md](../planning/Batch_AC.md) - 理论基础
- [SAC_OPTIMIZATION_DESIGN.md](../planning/SAC_OPTIMIZATION_DESIGN.md) - CPU SAC 优化
- [UNIFIED_TRAIL_MEMO.md](../bugfixes/UNIFIED_TRAIL_MEMO.md) - Trail 机制
- [GPU_BINARY_BACKTRACK_FIX.md](../bugfixes/GPU_BINARY_BACKTRACK_FIX.md) - GPU 搜索修复

### C. 版本历史

| 版本 | 日期 | 变更 |
|------|------|------|
| v0.1 | 2025-12-20 | 初稿（仅架构设计） |
| v0.5 | 2025-12-23 | 添加 Kernel 实现细节 |
| v1.0 | 2025-12-25 | 完整实施文档（本版本） |

---

**文档状态**: 设计完成，待实施
**下一步**: Week 1 核心实现（创建头文件和数据结构）
**负责人**: Phase 1.6 GPU Batch AC Implementation
**审核人**: 待定
**批准日期**: 待定
