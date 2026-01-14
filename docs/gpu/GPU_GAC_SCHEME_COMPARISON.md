# GPU GAC 传播方案综合分析与推荐

**文档版本**: v1.0
**创建日期**: 2025-11-08
**目标**: 为 Jetson Orin 平台选择最优的 GPU 约束传播方案

---

## 目录

1. [当前问题分析](#1-当前问题分析)
2. [方案概览](#2-方案概览)
3. [详细对比分析](#3-详细对比分析)
4. [最终推荐方案](#4-最终推荐方案)
5. [实施路线图](#5-实施路线图)
6. [核心代码骨架](#6-核心代码骨架)

---

## 1. 当前问题分析

### 1.1 当前 GModelSolver 的架构

```
GModelSolver::Search()          [CPU]
  └─→ for each value:
        ├─→ AssignValue()       [CPU]
        ├─→ EnforceGAC()        [GPU kernel + CPU 队列管理]
        │     ├─→ while (!queue.empty()) {
        │     │     ├─→ CPU 拷贝队列到 GPU
        │     │     ├─→ CsCheckMainKernel<<<...>>>()
        │     │     ├─→ cudaDeviceSynchronize()  ← 阻塞！
        │     │     ├─→ CPU 遍历 removal 数组
        │     │     └─→ CPU 构建下一轮队列
        │     │   }
        │     └─→ 返回 GacStats
        └─→ Search(next)        [递归]
```

### 1.2 核心瓶颈

| 瓶颈 | 影响 | 占比 |
|------|------|------|
| **每轮 GAC 迭代都需要 CPU/GPU 同步** | 50-100μs/次 同步延迟 | 60-75% |
| **队列管理完全在 CPU 侧** | 串行遍历 O(n+m) | 包含在上面 |
| **搜索逻辑在 CPU 串行执行** | GPU 大部分时间空闲 | 变量选择、层级管理 |
| **统一内存页迁移** | 5-10μs/次 | 次要 |

**本质问题**：当前是"GPU 作为协处理器"的模式，CPU 频繁干预导致并行效率低下。

---

## 2. 方案概览

### 2.1 五种方案对比表

| 方案 | 核心思想 | 队列/工作集 | 执行模型 | 同步方式 | 复杂度 |
|------|----------|-------------|----------|----------|--------|
| **C** | GPU 并发传播 + atomicAnd | 无锁队列 (Lock-Free) | 持久化 Kernel | 动态提交 | ⭐⭐⭐⭐ |
| **F** | Bitmap Frontier | 位图双缓冲 | Host Loop / Persistent | 轮次同步 | ⭐⭐ |
| **G** | 位图工作集 + 持久化 | 位图 | Persistent Kernel | Grid 同步 | ⭐⭐⭐ |
| **M** | 事件驱动 MPMC 队列 | 环形缓冲 | Persistent Kernel | 原子操作 | ⭐⭐⭐⭐⭐ |
| **S** | 混合调度 + 自适应 | 位图/队列 | CPU/GPU 动态切换 | 自适应 | ⭐⭐⭐ |

### 2.2 各方案一句话总结

- **Scheme C**: 用原子位操作实现并发安全的域裁剪，多线程块无锁协作
- **Scheme F**: 用位图替代队列，双缓冲交替处理，实现最简单
- **Scheme G**: 位图工作集 + 持久化内核，GPU 领域的最佳实践
- **Scheme M**: 完全事件驱动，类似 CPU AC3 的 GPU 移植，最灵活但复杂
- **Scheme S**: 根据工作量动态选择 CPU/GPU，适配 Jetson 嵌入式特性

---

## 3. 详细对比分析

### 3.1 Scheme C - GPU 并发约束传播

**核心代码模式**:
```cuda
// 无锁队列 + 原子域更新
__device__ bool atomicAndMask(unsigned int* bits, const unsigned int* mask) {
    bool changed = false;
    for (int i = 0; i < num_words; ++i) {
        unsigned int old = atomicAnd(&bits[i], mask[i]);
        if (old != (old & mask[i])) changed = true;
    }
    return changed;
}
```

**优势**:
- ✅ 原子 AND 保证多约束并发修改同一变量域的正确性
- ✅ 动态提交新任务，消除轮次间同步屏障
- ✅ 借鉴 VeriSAT 的并发游标和冲突广播

**劣势**:
- ❌ 无锁队列实现复杂（head/tail 原子操作）
- ❌ 队列争用可能成为新瓶颈
- ❌ 需要额外的 `in_queue` 标记避免重复入队

**适用场景**: 中大规模问题，约束密集

---

### 3.2 Scheme F - Bitmap Frontier（双缓冲位图）

**核心代码模式**:
```cuda
// 生产者：标记下一轮约束
__device__ void PropagateVarToNextBitmap(int var, u32* next_bitmap) {
    for (int cid : var_to_constraints[var]) {
        int w = cid / 32, b = cid % 32;
        atomicOr(&next_bitmap[w], 1u << b);
    }
}

// 消费者：从位图取任务
__device__ int FetchNextCidFromBitmap(u32* frontier_cur, int* scanner_index) {
    while (true) {
        int w = atomicAdd(scanner_index, 1);
        if (w >= bitmap_size_words) return -1;
        u32 word = atomicExch(&frontier_cur[w], 0u);
        if (word != 0u) {
            int bit = __ffs(word) - 1;
            return w * 32 + bit;
        }
    }
}
```

**优势**:
- ✅ **实现最简单**，可渐进演进
- ✅ atomicOr 分散在整个位图，几乎无争用
- ✅ 自动去重（重复标记无副作用）
- ✅ cp_scheme_F.md 提供了完整的代码骨架

**劣势**:
- ❌ 稀疏情况下扫描效率低（需要层级位图优化）
- ❌ Host 版本仍需每轮同步（但可演进到 Persistent）

**适用场景**: 所有规模，特别适合 Jetson 的渐进式实现

---

### 3.3 Scheme G - 位图工作集 + 持久化内核

**核心代码模式**:
```cuda
__global__ void PersistentGACKernel(...) {
    while (true) {
        // 1. 从 Queue_Current 取任务
        int cid = ClaimFromBitmap(queue_current);
        if (cid < 0) {
            // Grid 级同步
            cg::this_grid().sync();
            if (IsEmpty(queue_next)) return;  // 达到不动点
            Swap(queue_current, queue_next);
            continue;
        }

        // 2. 执行约束传播
        PropagateConstraint(cid);

        // 3. 标记下一轮约束
        if (changed) MarkNeighborsInBitmap(queue_next);
    }
}
```

**优势**:
- ✅ **GPU 领域最佳实践**（推荐方案）
- ✅ 整个传播在 GPU 内完成，只需一次 CPU/GPU 同步
- ✅ 利用 Cooperative Groups 实现 Grid 级同步
- ✅ 支持层级位图处理稀疏性

**劣势**:
- ❌ 需要 Cooperative Groups（CUDA 9.0+）
- ❌ Grid 同步有一定开销（但远小于返回 Host）
- ❌ 小任务时 GPU 启动开销仍存在

**适用场景**: 中大规模问题，推荐作为主方案

---

### 3.4 Scheme M - 事件驱动 MPMC 队列

**核心代码模式**:
```cuda
__global__ void PropagationWorkerKernel(GlobalState state) {
    while (true) {
        // 1. 从 MPMC 队列取事件
        ConEvent ev;
        if (!dequeue(&ev)) {
            if (global_termination_check()) break;
            continue;
        }

        // 2. 执行约束传播
        PropagateResult res = PropagateConstraint(ev.cid, ev.level);

        // 3. 生成新事件（多生产者）
        if (res.x_changed || res.y_changed) {
            for (int cid2 : var_to_constraints[affected_var]) {
                if (!in_queue[cid2].exchange(true)) {
                    enqueue({cid2, level});
                }
            }
        }
    }
}
```

**优势**:
- ✅ 最接近 CPU AC3 的逻辑，概念清晰
- ✅ 支持事件级别的优先级调度
- ✅ 灵活的多层级支持（ev.level）

**劣势**:
- ❌ **MPMC 队列原子争用严重**（head/tail + in_queue）
- ❌ 实现最复杂，调试困难
- ❌ 终止检测复杂（需要 active_workers 计数器）

**适用场景**: 需要优先级调度的特殊场景，不推荐作为主方案

---

### 3.5 Scheme S - 混合调度 + 自适应

**核心代码模式**:
```cpp
GacStats GModel::EnforceGAC(int assigned_var) {
    int estimated_workload = EstimateWorkload(assigned_var);

    if (estimated_workload < CPU_THRESHOLD) {
        // 小任务：CPU 直接处理（利用统一内存）
        return RunBitmapPropagateOnCPU();
    } else if (estimated_workload < GPU_BATCH_THRESHOLD) {
        // 中等任务：GPU 批量处理
        return RunBitmapGACKernel_HostLoop();
    } else {
        // 大任务：GPU 持久化内核
        return RunPersistentGACKernel();
    }
}
```

**优势**:
- ✅ **最适合 Jetson 嵌入式平台**
- ✅ 小任务避免 GPU 启动开销
- ✅ 统一内存零拷贝切换
- ✅ 全场景性能覆盖

**劣势**:
- ❌ 需要调优阈值（CPU_THRESHOLD 等）
- ❌ 需要实现两套传播逻辑（CPU + GPU）
- ❌ 启发式判断可能不准确

**适用场景**: Jetson 等嵌入式平台，推荐作为补充策略

---

### 3.6 方案评分矩阵

| 维度 | C | F | G | M | S |
|------|---|---|---|---|---|
| **实现复杂度** | 3 | 5 | 4 | 2 | 3 |
| **同步效率** | 4 | 3 | 5 | 4 | 4 |
| **原子争用** | 3 | 5 | 5 | 2 | 4 |
| **小任务性能** | 3 | 3 | 2 | 2 | 5 |
| **大任务性能** | 4 | 4 | 5 | 4 | 4 |
| **Jetson 适配** | 3 | 4 | 4 | 2 | 5 |
| **可维护性** | 3 | 5 | 4 | 2 | 3 |
| **总分（/35）** | **23** | **29** | **29** | **18** | **28** |

---

## 4. 最终推荐方案

### 4.1 推荐：**Scheme F (Bitmap) + Scheme S (Hybrid)**

**理由**：
1. **F 方案提供最佳的数据结构**：位图双缓冲，atomicOr 无争用，自动去重
2. **S 方案提供最佳的执行策略**：CPU/GPU 动态切换，适配 Jetson
3. **渐进式实现**：可以从简单的 Host Loop 开始，逐步演进到 Persistent
4. **cp_scheme_F.md 提供了完整代码骨架**：降低实现风险

### 4.2 架构设计

```
┌─────────────────────────────────────────────────────────────────────┐
│                    GModel::EnforceGAC() 入口                         │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │              工作量估算 (Workload Estimation)                   │  │
│  │              estimated = CountSetBits(frontier) × avg_degree   │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                               │                                     │
│         ┌─────────────────────┼─────────────────────┐               │
│         │                     │                     │               │
│         ▼                     ▼                     ▼               │
│  ┌──────────────┐     ┌──────────────┐     ┌──────────────┐        │
│  │  CPU 快路径   │     │  GPU 批量版   │     │  GPU 持久化  │        │
│  │  (< 4×SM)    │     │ (4×SM ~ 100) │     │  (> 100)     │        │
│  │              │     │              │     │              │        │
│  │ NEON 位操作  │     │ Host Loop +  │     │ Persistent   │        │
│  │ 直接处理     │     │ BitmapGAC    │     │ Kernel +     │        │
│  │ 统一内存     │     │ Kernel       │     │ CG Grid Sync │        │
│  └──────────────┘     └──────────────┘     └──────────────┘        │
│         │                     │                     │               │
│         └─────────────────────┼─────────────────────┘               │
│                               ▼                                     │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │              返回 GacStats (iterations, deletions, ...)        │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### 4.3 关键设计决策

| 决策点 | 选择 | 理由 |
|--------|------|------|
| **队列 vs 位图** | 位图 | 无争用、自动去重、内存紧凑 |
| **单 Kernel vs 多 Kernel** | 持久化单 Kernel | 消除启动开销 |
| **同步方式** | Cooperative Groups | GPU 内 Grid 同步 |
| **CPU 补充** | 是 | 小任务避免 GPU 开销 |
| **内存模型** | 统一内存 | Jetson 零拷贝 |

---

## 5. 实施路线图

### 阶段 1: 基础版 - Host Loop（3-5 天）

**目标**: 验证 Bitmap 数据结构和传播逻辑的正确性

**工作内容**:
1. 在 `GModel` 中增加 `GACControl` 结构和双缓冲位图
2. 实现 `BitmapGACKernel`（单轮传播）
3. 在 Host 端编写 `while` 循环控制 Kernel 启动和位图交换
4. 集成到 `GModelSolver`，验证与 CPU 结果一致

**预期收益**:
- 消除 CPU 队列管理开销（6ms → 2ms）
- **GAC 部分加速 2-3x**

### 阶段 2: 持久化版 - Persistent Kernel（5-7 天）

**目标**: 消除 Kernel 启动开销，提升中大规模性能

**工作内容**:
1. 将 Host 端 `while` 循环移入 GPU Kernel 内部
2. 使用 `cooperative_groups::this_grid().sync()` 实现设备端同步
3. 引入 `GACControl::active_count` 作为终止条件
4. 处理稀疏情况（可选：层级位图）

**预期收益**:
- 消除每轮 Kernel 启动开销（20 × 10μs = 0.2ms）
- **GAC 部分加速 3-5x**

### 阶段 3: 混合自适应版 - Hybrid（3-5 天）

**目标**: 全场景性能覆盖，适配 Jetson 嵌入式特性

**工作内容**:
1. 实现 CPU 版位图传播（利用 NEON 指令）
2. 在 `EnforceGAC` 入口引入启发式判断
3. 调优 `CPU_THRESHOLD`，找到最佳切换点
4. 可选：实现 GPU 端变量选择和层级管理

**预期收益**:
- 小任务（<50 约束）CPU 处理，避免 GPU 开销
- **全场景加速 3-10x**

### 阶段 4: 高级优化（1-2 周，可选）

**目标**: 进一步提升性能

**工作内容**:
1. GPU 端变量选择（Parallel Reduction）
2. GPU 端层级管理（并行拷贝）
3. 层级位图处理稀疏性
4. Portfolio 并行搜索

---

## 6. 核心代码骨架

### 6.1 数据结构（GModel.cuh）

```cpp
struct GACControl {
    int inconsistent_flag;   // 全局不一致标志
    int scanner_index;       // Bitmap 扫描游标
    long long deletions;     // 累计删值数
    int iterations;          // 传播轮数
    int active_count;        // 活跃约束数（用于终止检测）
};

class GModel {
public:
    // ... 现有字段 ...

    // GAC GPU 控制资源
    GACControl* d_gac_control = nullptr;
    u32* d_queue_bitmap_A = nullptr;  // Current Frontier
    u32* d_queue_bitmap_B = nullptr;  // Next Frontier
    int bitmap_size_words = 0;

    // 初始化/释放
    void InitializeGPUResources();
    void FreeGPUResources();
    GModelData GetGModelDataView() const;

    // 新版传播接口（支持增量传播）
    GacStats EnforceGAC(bool verbose = false, int assigned_var = -1);

private:
    // CPU 快路径
    GacStats EnforceGAC_CPU(int assigned_var);
    // GPU 批量版
    GacStats EnforceGAC_GPU_HostLoop(int assigned_var);
    // GPU 持久化版
    GacStats EnforceGAC_GPU_Persistent(int assigned_var);
};
```

### 6.2 核心 Kernel（BitmapGACKernel）

```cuda
__global__ void BitmapGACKernel(
    GModelData model,
    GACControl* control,
    u32* frontier_cur,
    u32* frontier_next,
    int bitmap_size_words,
    int current_level
) {
    extern __shared__ u32 shmem[];

    while (true) {
        // 1. 从位图取任务（Block 间负载均衡）
        __shared__ int cid_shared;
        if (threadIdx.x == 0) {
            cid_shared = FetchNextCidFromBitmap(
                frontier_cur, bitmap_size_words,
                &control->scanner_index);
        }
        __syncthreads();

        int cid = cid_shared;
        if (cid < 0) break;  // 当前轮 Frontier 用完

        // 2. 执行约束传播（Block 内并行）
        PropagateResult r = ExecuteConstraintCheck_BpC(
            cid, model, current_level, shmem);

        // 3. 处理结果
        if (threadIdx.x == 0) {
            if (r.deletions > 0) {
                atomicAdd(&control->deletions, (long long)r.deletions);

                // 标记下一轮约束
                const int2 scope = model.constraint_scopes[cid];
                if (r.x_changed) {
                    PropagateVarToNextBitmap(scope.x, model, frontier_next);
                }
                if (r.y_changed) {
                    PropagateVarToNextBitmap(scope.y, model, frontier_next);
                }
            }
            if (r.inconsistent) {
                atomicExch(&control->inconsistent_flag, 1);
            }
        }
        __syncthreads();

        // 4. 快速逃出（发现冲突）
        if (control->inconsistent_flag) break;
    }
}
```

### 6.3 Host 端控制（EnforceGAC）

```cpp
GacStats GModel::EnforceGAC(bool verbose, int assigned_var) {
    // 工作量估算
    int estimated_workload = EstimateWorkload(assigned_var);

    // 动态选择执行路径
    if (estimated_workload < CPU_THRESHOLD) {
        return EnforceGAC_CPU(assigned_var);
    } else if (estimated_workload < GPU_PERSISTENT_THRESHOLD) {
        return EnforceGAC_GPU_HostLoop(assigned_var);
    } else {
        return EnforceGAC_GPU_Persistent(assigned_var);
    }
}

GacStats GModel::EnforceGAC_GPU_HostLoop(int assigned_var) {
    GacStats stats;
    InitializeGPUResources();

    // 1. 初始化控制块
    GACControl h_ctrl{};
    cudaMemcpy(d_gac_control, &h_ctrl, sizeof(GACControl), cudaMemcpyHostToDevice);

    // 2. 初始化 Frontier
    InitializeFrontierBitmap(assigned_var);

    // 3. 主循环（Host 控制）
    bool done = false;
    while (!done) {
        ++h_ctrl.iterations;
        h_ctrl.scanner_index = 0;
        cudaMemcpy(&d_gac_control->scanner_index, &h_ctrl.scanner_index,
                   sizeof(int), cudaMemcpyHostToDevice);

        // 启动 Kernel
        int blocks = std::min(bitmap_size_words, 128);
        int threads = std::min(max_dom_size, 256);
        size_t shmem = 2 * bit_dom_int_size * sizeof(u32);

        BitmapGACKernel<<<blocks, threads, shmem>>>(
            GetGModelDataView(), d_gac_control,
            d_queue_bitmap_A, d_queue_bitmap_B,
            bitmap_size_words, current_level_);
        cudaDeviceSynchronize();

        // 检查结果
        cudaMemcpy(&h_ctrl, d_gac_control, sizeof(GACControl), cudaMemcpyDeviceToHost);

        if (h_ctrl.inconsistent_flag) {
            stats.inconsistent = true;
            done = true;
        } else if (IsBitmapEmpty(d_queue_bitmap_B)) {
            done = true;  // 达到不动点
        } else {
            std::swap(d_queue_bitmap_A, d_queue_bitmap_B);
            cudaMemset(d_queue_bitmap_B, 0, bitmap_size_words * sizeof(u32));
        }
    }

    stats.deletions = h_ctrl.deletions;
    stats.iterations = h_ctrl.iterations;
    return stats;
}
```

### 6.4 GModelSolver 集成

```cpp
// 修改 GModelSolver::Search 中的 GAC 调用
bool GModelSolver::Search(int level, GpuSearchStatistics& stats, ...) {
    // ...

    // 赋值
    model_->AssignValue(var, value, new_level);

    // 增量 GAC 传播（传入赋值变量，启用增量模式）
    Timer gac_timer;
    GacStats gac_stats = model_->EnforceGAC(false, var);  // ← 关键改动
    stats.gac_time += gac_timer.elapsed() / 1000.0;
    stats.gac_iterations += gac_stats.iterations;
    stats.gac_deletions += gac_stats.deletions;

    // ...
}
```

---

## 7. 预期收益总结

| 阶段 | 工作量 | 预期 GAC 加速 | 预期总体加速 |
|------|--------|---------------|--------------|
| 阶段 1（Host Loop） | 3-5 天 | **2-3x** | 1.5-2x |
| 阶段 2（Persistent） | 5-7 天 | **3-5x** | 2-3x |
| 阶段 3（Hybrid） | 3-5 天 | **5-10x**（含小任务） | 3-5x |
| 阶段 4（高级优化） | 1-2 周 | **10-20x** | 5-10x |

**最终目标**：
- 小问题（queens-4）：GPU ≤ CPU
- 中等问题（composed-25）：GPU ≈ CPU
- 大问题（haystacks-11）：GPU > 2x CPU

---

## 8. 结论

**推荐方案**：**Scheme F (Bitmap Frontier) + Scheme S (Hybrid Scheduling)**

**理由**：
1. 位图数据结构最适合 GPU（无争用、自动去重）
2. 混合调度最适合 Jetson（CPU/GPU 动态切换）
3. 渐进式实现路径清晰（可从简单版本开始）
4. cp_scheme_F.md 提供了完整代码骨架，降低实现风险

**立即可执行的下一步**：
1. 在 `GModel` 中添加 `GACControl` 和双缓冲位图
2. 实现 `BitmapGACKernel`（参考 cp_scheme_F.md）
3. 在 Host 端编写 `while` 循环控制
4. 验证与 CPU AC3bit 结果一致

---

**文档结束**

*如有疑问或建议，请联系开发团队*
