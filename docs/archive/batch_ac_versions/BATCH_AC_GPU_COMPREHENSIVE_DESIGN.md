# Batch AC-GPU 综合设计文档

> 本文档综合了 SAC-GPU 批量化实现的完整设计思路，涵盖 Batch-1/2/3 三个层次的并行策略，以及 Jetson 平台适配方案。

## 1. 背景与目标

### 1.1 SAC（Singleton Arc Consistency）语义

SAC 的定义是一个"性质"：对于变量 $X_i$ 的值 $a$，将其**单例赋值**（$D(X_i) \leftarrow \{a\}$），然后对整个 CSP 运行 **全局 GAC 传播**：
- 若收敛到非空域：$(X_i, a)$ 是 SAC 一致的
- 若出现空域（DWO）：$(X_i, a)$ 不是 SAC 一致的，需要删除

**关键区分**：
| 算法 | Frontier 初始化 | 语义 |
|------|-----------------|------|
| MAC 增量 GAC | 只激活赋值变量的邻接约束 | 赋值后局部传播 |
| **SAC 单例检查** | 激活**所有约束** | 验证全局一致性 |

### 1.2 Batch AC 的核心思想

传统 SAC 是串行的：
```
for (var, val) in all_values:
    赋值 → AC 传播 → 检查 DWO → 恢复状态
```

Batch AC 的目标是**批量化**这些独立的探测（probe），利用 GPU 并行能力加速。

### 1.3 三层批量化策略

```
┌─────────────────────────────────────────────────────────────────────┐
│  Batch-1: 时间维度批量化（已实现）                                   │
│  • 所有 GPU 线程协作处理一个 probe                                  │
│  • Probe 之间串行执行                                               │
│  • 共享一份域快照                                                   │
│  • 并行度 = 约束数（~1000）                                         │
├─────────────────────────────────────────────────────────────────────┤
│  Batch-2: 空间维度批量化                                            │
│  • B 个 probe 并行执行（每个有独立工作域）                          │
│  • 每个 Block（或多个 Block）处理一个 probe                         │
│  • 并行度 = B × 约束数（~10K-100K）                                 │
├─────────────────────────────────────────────────────────────────────┤
│  Batch-3: 极致并行化                                                │
│  • Batch-3A: 异步调度 + 工作窃取                                    │
│  • Batch-3B: World-SIMD（位切片，一次处理 32 个 world）             │
│  • 并行度 = 128+ 并发 probe                                         │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 2. Jetson Orin 平台约束

### 2.1 硬件特性

```
Jetson Orin NX 16GB:
├── GPU 架构:        Ampere (SM 8.7)
├── SM 数量:         8
├── 每 SM 最大线程:  1536
├── 每 SM 最大 Blocks: 16（理论值）
├── Warp 大小:       32
├── L1/Shared Memory: 128 KB/SM（可配置比例）
├── L2 Cache:        4 MB（全局共享）
├── 内存:            统一内存架构（CPU/GPU 共享）
└── 内存带宽:        ~100 GB/s
```

### 2.2 Cooperative Kernel 限制

```cpp
// Cooperative Launch 可驻留 blocks 计算
int max_blocks_per_sm;
cudaOccupancyMaxActiveBlocksPerMultiprocessor(
    &max_blocks_per_sm, kernel, block_size, shared_mem);

int max_grid_size = max_blocks_per_sm * num_sms;  // 通常 16-32
```

**关键约束**：Batch-2 的并发 world 数 B 受此限制：
```
B × blocks_per_world ≤ max_grid_size (~32)
→ B=16, blocks_per_world=2  或
→ B=32, blocks_per_world=1
```

### 2.3 统一内存优势

Jetson 的 CPU/GPU 共享物理内存，可实现：
- 零拷贝数据访问
- 自动页面迁移
- 简化内存管理

---

## 3. Batch-1：时间维度批量化（已实现）

### 3.1 架构概述

```
┌─────────────────────────────────────────────────────────────────────┐
│  PersistentBatchProbeKernel (Cooperative Groups)                    │
│                                                                      │
│  while (有未处理的任务) {                                            │
│    [1] 原子获取任务 (tid==0 执行 atomicAdd)                          │
│    [2] 并行恢复快照 (所有线程协作拷贝)                               │
│    [3] 单例赋值 (tid==0 执行)                                        │
│    [4] 初始化 Frontier (全约束激活 - SAC 语义)                       │
│    [5] RunGACToFixpoint (复用现有 GAC 传播逻辑)                      │
│    [6] 记录结果 (tid==0 执行)                                        │
│  }                                                                   │
└─────────────────────────────────────────────────────────────────────┘
```

### 3.2 快照共享机制

所有 Probe 共享**同一份只读快照**：

```
                    ┌─────────────────────────────┐
                    │  domain_snapshot (只读)     │
                    │  dom_size_snapshot (只读)   │
                    └──────────────┬──────────────┘
                                   │
           ┌───────────────────────┼───────────────────────┐
           ▼                       ▼                       ▼
    ┌─────────────┐         ┌─────────────┐         ┌─────────────┐
    │  Probe 0    │         │  Probe 1    │   ...   │  Probe N-1  │
    │  恢复→赋值  │         │  恢复→赋值  │         │  恢复→赋值  │
    │  →传播→检查 │         │  →传播→检查 │         │  →传播→检查 │
    └─────────────┘         └─────────────┘         └─────────────┘
```

**内存效率**：只需 1 份快照（~KB 级），而非 N 份（~MB 级）

### 3.3 已实现的关键修复

1. **ExecuteBatch 状态恢复**：批量执行前后 GModel 状态完全一致
2. **Probe 间状态隔离**：每个 probe 开始前恢复 bitDom + d_cur_dom_size
3. **SAC 全局 GAC 语义**：InitializeFrontierForVariable 激活所有约束

### 3.4 性能瓶颈

- Probe 串行执行，GPU 大量空闲
- 典型利用率 ~10%
- 适合作为正确性基准，不适合生产使用

---

## 4. Batch-2：空间维度批量化

### 4.1 核心思想

B 个 Probe 并行执行，每个有**独立的工作域**：

```
┌──────────────────────────────────────────────────────────────────┐
│  Grid                                                             │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐       ┌──────────┐      │
│  │ World 0  │ │ World 1  │ │ World 2  │  ...  │ World B-1│      │
│  │ Probe 0  │ │ Probe 1  │ │ Probe 2  │       │ Probe B-1│      │
│  │          │ │          │ │          │       │          │      │
│  │ 私有域   │ │ 私有域   │ │ 私有域   │       │ 私有域   │      │
│  │ 私有frontier│ │ 私有frontier│ │ ...    │       │ ...      │      │
│  └──────────┘ └──────────┘ └──────────┘       └──────────┘      │
│       ↑             ↑             ↑                 ↑           │
│       └─────────────┴─────────────┴─────────────────┘           │
│                           │                                      │
│                    共享只读快照                                   │
└──────────────────────────────────────────────────────────────────┘
```

### 4.2 方案选择

#### 方案 A：完整域复制（简单直接）

```cpp
// 每个 world 完整拷贝一份域
u32* bitDom_batch[B];           // B 份完整 bitDom
int* dom_size_batch[B];         // B 份完整 dom_size
u32* frontier_A_batch[B];       // B 份 frontier
u32* frontier_B_batch[B];

// 内存需求（Queens-100, B=16）:
// 16 × (100 × 4 × 4 + 100 × 4 + 2 × 160) ≈ 32 KB
```

**优点**：实现简单，无额外计算开销
**缺点**：内存随 B 线性增长

#### 方案 B：Copy-on-Write Delta 存储（内存优化）

```cpp
// 共享只读快照 + 稀疏差异存储
struct DeltaBlock {
    static constexpr int MAX_MODIFIED_VARS = 64;

    int probe_id;                                    // 关联的 probe
    int num_modified;                                // 修改的变量数
    int var_ids[MAX_MODIFIED_VARS];                  // 被修改的变量 ID
    u32 domains[MAX_MODIFIED_VARS * MAX_DOM_WORDS];  // 修改后的域值
    int dom_sizes[MAX_MODIFIED_VARS];                // 修改后的域大小

    // Frontier（每个 probe 独立）
    u32 frontier_A[MAX_BITMAP_WORDS];
    u32 frontier_B[MAX_BITMAP_WORDS];
};

// 读取域：先查 delta，未命中则读 base
__device__ u32 ReadDomain(int block_idx, int var_id, int word_idx) {
    DeltaBlock& block = delta_blocks[block_idx];
    for (int i = 0; i < block.num_modified; ++i) {
        if (block.var_ids[i] == var_id) {
            return block.domains[i * MAX_DOM_WORDS + word_idx];  // Delta 命中
        }
    }
    return base_snapshot[var_id * bit_dom_int_size + word_idx];  // 读 base
}

// 写入域：首次写入时从 base 拷贝（Copy-on-Write）
__device__ void WriteDomain(int block_idx, int var_id, int word_idx, u32 value) {
    DeltaBlock& block = delta_blocks[block_idx];
    int slot = FindOrAllocateSlot(block, var_id);  // 查找或分配 slot

    if (IsNewSlot(slot)) {
        // 首次写入：从 base 拷贝完整域
        CopyFromBase(block, slot, var_id);
    }

    block.domains[slot * MAX_DOM_WORDS + word_idx] = value;
}
```

**优点**：内存效率高（只存差异），适合稀疏修改
**缺点**：读写有额外开销（查找）

#### 推荐策略

1. **先用方案 A** 验证正确性（简单）
2. **内存不足时**切换方案 B（优化）
3. 提供统一接口，运行时自动选择

### 4.3 Kernel 结构

```cuda
__global__ void Batch2ParallelProbeKernel(
    GModelData base_model,
    WorldWorkspace* workspaces,  // [B] 每个 world 的工作空间
    ProbeTask* tasks,
    int num_tasks,
    bool* results) {

    // 计算当前 world ID
    const int blocks_per_world = gridDim.x / num_worlds;
    const int world_id = blockIdx.x / blocks_per_world;
    const int local_block_id = blockIdx.x % blocks_per_world;

    if (world_id >= num_tasks) return;

    WorldWorkspace& ws = workspaces[world_id];
    ProbeTask& task = tasks[world_id];

    // [1] 从快照恢复到私有工作域（并行拷贝）
    RestoreFromSnapshot(ws, base_model.snapshot, local_block_id);
    __syncthreads();

    // [2] 单例赋值
    if (threadIdx.x == 0 && local_block_id == 0) {
        SingletonAssign(ws, task.var_id, task.value);
    }

    // [3] GAC 传播（world 内的 blocks 协作）
    bool inconsistent = RunGACInWorld(ws, base_model, local_block_id);

    // [4] 记录结果
    if (threadIdx.x == 0 && local_block_id == 0) {
        results[world_id] = !inconsistent;
    }
}
```

### 4.4 预期性能

| 指标 | Batch-1 | Batch-2 (B=16) |
|------|---------|----------------|
| 并发 probe | 1 | 16 |
| GPU 利用率 | ~10% | ~60% |
| 预计加速比 | 1× | **10-15×** |

---

## 5. Batch-3A：异步调度 + 工作窃取

### 5.1 核心问题

Batch-2 使用 `grid.sync()` 进行 lockstep 同步，导致：
- 收敛快的 world 空转等待慢的 world
- 负载不均衡严重

### 5.2 解决方案：任务驱动的异步推进

```
┌─────────────────────────────────────────────────────────────────────┐
│  不再 lockstep 同步，改为"任务驱动"                                 │
│                                                                      │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │  Global Task Queue (Lock-free)                                │   │
│  │  [(world_id, constraint_id), (world_id, constraint_id), ...]  │   │
│  └───────────────────────────┬──────────────────────────────────┘   │
│                              │                                       │
│         ┌────────────────────┼────────────────────┐                 │
│         ▼                    ▼                    ▼                 │
│  ┌─────────────┐      ┌─────────────┐      ┌─────────────┐         │
│  │ Persistent  │      │ Persistent  │      │ Persistent  │         │
│  │ Block 0     │      │ Block 1     │      │ Block K     │         │
│  │             │      │             │      │             │         │
│  │ 从队列抢任务 │      │ 从队列抢任务 │      │ 从队列抢任务 │         │
│  │ 执行约束检查 │      │ 执行约束检查 │      │ 执行约束检查 │         │
│  │ 产生新任务   │      │ 产生新任务   │      │ 产生新任务   │         │
│  └─────────────┘      └─────────────┘      └─────────────┘         │
│                                                                      │
└─────────────────────────────────────────────────────────────────────┘
```

### 5.3 持久线程块 + Warp 级任务

```cuda
__global__ void PersistentWarpPoolKernel(
    GModelData base_model,
    ProbeMemoryPool* pool,
    GlobalTaskQueue* queue) {

    const int block_id = blockIdx.x;
    const int warp_id = threadIdx.x / 32;
    const int lane_id = threadIdx.x % 32;

    // 每个 Warp 独立处理任务
    while (!queue->AllCompleted()) {
        // [1] Warp leader 从队列获取任务
        int task_type, task_id;
        if (lane_id == 0) {
            queue->Pop(&task_type, &task_id);
        }
        task_type = __shfl_sync(0xFFFFFFFF, task_type, 0);
        task_id = __shfl_sync(0xFFFFFFFF, task_id, 0);

        if (task_type == TASK_NONE) continue;

        // [2] 根据任务类型执行
        switch (task_type) {
            case TASK_NEW_PROBE:
                // 分配 DeltaBlock，执行单例赋值
                InitializeProbe(pool, task_id, lane_id);
                break;

            case TASK_CHECK_CONSTRAINT:
                // 执行约束检查，可能产生新任务
                CheckConstraintWarp(pool, task_id, lane_id, queue);
                break;

            case TASK_PROBE_COMPLETE:
                // 回收 DeltaBlock，记录结果
                FinalizeProbe(pool, task_id, lane_id);
                break;
        }
    }
}
```

### 5.4 约束亲和性 + 工作窃取

为了优化 bitSup 缓存命中率：

```
┌─────────────────────────────────────────────────────────────────────┐
│  软绑定：每个 Block 有"首选约束范围"                                │
│                                                                      │
│  Block 0: 首选 C[0-99]      Block 1: 首选 C[100-199]    ...         │
│                                                                      │
│  处理逻辑：                                                          │
│  1. 优先处理首选范围内的任务（bitSup 缓存热）                       │
│  2. 首选范围空了 → 从其他范围窃取任务                               │
│  3. Shared Memory 缓存当前约束的 bitSup                             │
└─────────────────────────────────────────────────────────────────────┘
```

```cuda
__device__ void ProcessWithAffinity(int block_id, int num_constraints) {
    // 计算亲和范围
    const int constraints_per_block = (num_constraints + gridDim.x - 1) / gridDim.x;
    const int my_start = block_id * constraints_per_block;
    const int my_end = min(my_start + constraints_per_block, num_constraints);

    // Shared memory 缓存
    extern __shared__ u32 shared_bitSup[];
    __shared__ int cached_cid;

    // [1] 优先从亲和范围找任务
    int cid = FindTaskInRange(my_start, my_end);

    // [2] 没有则窃取
    if (cid < 0) {
        cid = StealFromGlobal();
    }

    // [3] 缓存 bitSup（如果需要）
    if (cid != cached_cid) {
        LoadBitSupToShared(cid, shared_bitSup);
        cached_cid = cid;
    }
    __syncthreads();

    // [4] 使用缓存的 bitSup 处理任务
    ProcessConstraint(cid, shared_bitSup);
}
```

### 5.5 同一约束被多个 Probe 执行

**场景**：约束 C[5] 连接变量 X0, X1
- Probe 0: X0=a（激活 C[5]）
- Probe 1: X1=b（也激活 C[5]）

**解决方案**：约束级任务聚合

```
┌─────────────────────────────────────────────────────────────────────┐
│  约束级任务队列（每个约束一个队列）                                 │
│                                                                      │
│  Queue[C=0]: [(w0), (w3)]     ← 需要检查 C=0 的 world 列表         │
│  Queue[C=5]: [(w0), (w1)]     ← 需要检查 C=5 的 world 列表         │
│  Queue[C=99]: [(w2)]                                                │
│                                                                      │
│  处理流程：                                                          │
│  1. Block 从 Queue[C=5] 取出所有待处理 world: [w0, w1]             │
│  2. 加载 bitSup[C=5] 到 shared memory（一次）                       │
│  3. 顺序处理 w0, w1（复用 bitSup）                                  │
│                                                                      │
│  优点：bitSup 只加载一次，服务多个 probe                            │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 6. Batch-3B：World-SIMD（极致优化）

### 6.1 核心创新：域表示转置

传统表示（per-world）：
```
world 0: dom[var=0] = {0,1,2,3}  → bitset: 0b1111
world 1: dom[var=0] = {0,2}     → bitset: 0b0101
world 2: dom[var=0] = {1,3}     → bitset: 0b1010
...
```

**World-SIMD 转置表示**：
```
dom_mask[var=0][value=0] = 0b011  ← world 0,1 有值 0
dom_mask[var=0][value=1] = 0b101  ← world 0,2 有值 1
dom_mask[var=0][value=2] = 0b011  ← world 0,1 有值 2
dom_mask[var=0][value=3] = 0b101  ← world 0,2 有值 3
```

### 6.2 约束检查向量化

```cuda
// 传统：循环处理每个 world
for (int w = 0; w < B; ++w) {
    check_constraint(world[w], cid);  // B 次约束检查
}

// World-SIMD：一次位运算处理 32 个 world
__device__ void CheckConstraintSIMD(
    int cid,
    u32 world_mask,           // 哪些 world 需要处理
    u32 dom_mask[][MAX_VAL],  // 转置的域表示
    u32* changed_mask) {      // 输出：哪些 world 有变化

    int2 scope = constraint_scopes[cid];
    int var_x = scope.x, var_y = scope.y;

    u32 support_x = 0, support_y = 0;

    // 遍历值对，计算支持（一次服务 32 个 world）
    for (int vx = 0; vx < dom_size; ++vx) {
        u32 worlds_have_vx = dom_mask[var_x][vx] & world_mask;
        if (worlds_have_vx == 0) continue;

        for (int vy = 0; vy < dom_size; ++vy) {
            if (!IsSupported(cid, vx, vy)) continue;

            u32 worlds_have_vy = dom_mask[var_y][vy] & world_mask;
            u32 worlds_both = worlds_have_vx & worlds_have_vy;

            support_x |= worlds_both;  // 这些 world 中 vx 有支持
            support_y |= worlds_both;  // 这些 world 中 vy 有支持
        }
    }

    // 更新域（批量）
    for (int v = 0; v < dom_size; ++v) {
        u32 old_x = dom_mask[var_x][v];
        u32 new_x = old_x & support_x;
        dom_mask[var_x][v] = new_x;
        *changed_mask |= (old_x ^ new_x);
    }
    // var_y 同理...
}
```

### 6.3 优势分析

| 维度 | 传统 Batch-2 | World-SIMD |
|------|--------------|------------|
| bitSup 加载 | B 次 | **1 次** |
| 约束检查 | B 次独立检查 | **1 次向量化检查** |
| 内存访问 | 分散 | 合并 |
| 适合场景 | 通用 | 约束密集型问题 |

### 6.4 实现复杂度

需要重构的组件：
1. 域表示：`bitDom[var][word]` → `dom_mask[var][value]`
2. 约束检查：逐 world 检查 → 向量化检查
3. Frontier：per-world bitmap → `world_mask` per constraint

---

## 7. GPU 内存池设计

### 7.1 分层内存架构

```
┌─────────────────────────────────────────────────────────────────────┐
│  Layer 0: 共享只读快照（所有 probe 共享）                           │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │  base_snapshot: bitDom[num_vars × bit_dom_int_size]         │    │
│  │  base_dom_sizes: d_cur_dom_size[num_vars]                   │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                              │                                       │
│                              ▼ Copy-on-Write                        │
│  Layer 1: 差异存储（每个活跃 probe 一份 DeltaBlock）                │
│  ┌──────────────┐  ┌──────────────┐       ┌──────────────┐         │
│  │ DeltaBlock 0 │  │ DeltaBlock 1 │  ...  │ DeltaBlock K │         │
│  │ probe_id: 3  │  │ probe_id: 7  │       │ probe_id: 15 │         │
│  │ modified: 5  │  │ modified: 3  │       │ modified: 8  │         │
│  └──────────────┘  └──────────────┘       └──────────────┘         │
│                                                                      │
│  Layer 2: 内存池管理器（Lock-free 分配/释放）                       │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │  free_stack: [2, 4, 5, 6, ...]  ← 可用 DeltaBlock 索引      │    │
│  │  free_top: atomic<int>                                       │    │
│  └─────────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────────┘
```

### 7.2 ProbeMemoryPool 接口

```cpp
class ProbeMemoryPool {
public:
    static constexpr int MAX_CONCURRENT_PROBES = 128;
    static constexpr int MAX_MODIFIED_VARS = 64;

    // 分配 DeltaBlock 给 probe
    __device__ int Allocate(int probe_id);

    // 释放 DeltaBlock
    __device__ void Free(int block_idx);

    // Copy-on-Write 读取
    __device__ u32 ReadDomainWord(int block_idx, int var_id, int word_idx);

    // Copy-on-Write 写入
    __device__ void WriteDomainWord(int block_idx, int var_id, int word_idx, u32 value);

private:
    // 共享快照
    const u32* base_snapshot_;
    const int* base_dom_sizes_;
    int bit_dom_int_size_;

    // DeltaBlock 池
    DeltaBlock blocks_[MAX_CONCURRENT_PROBES];

    // Lock-free 空闲栈
    int free_stack_[MAX_CONCURRENT_PROBES];
    int free_top_;  // atomic
};
```

### 7.3 SoA 内存布局优化

为最大化 L2 缓存命中率，采用 Structure of Arrays：

```cpp
struct DeltaPoolSoA {
    // 热数据（频繁访问）- 放在一起
    int probe_ids[POOL_SIZE];
    int num_modified[POOL_SIZE];

    // 温数据（GAC 迭代访问）
    int var_ids[POOL_SIZE][MAX_MODIFIED];
    u32 domains[POOL_SIZE][MAX_MODIFIED][MAX_WORDS];

    // 冷数据（偶尔访问）
    u32 frontier_A[POOL_SIZE][MAX_BITMAP_WORDS];
    u32 frontier_B[POOL_SIZE][MAX_BITMAP_WORDS];
};
```

---

## 8. 统一接口设计

### 8.1 自动模式选择

```cpp
enum class BatchMode {
    Batch1,      // 时间维度（兜底）
    Batch2,      // 空间维度
    Batch3A,     // 异步调度
    Batch3B,     // World-SIMD
    Auto         // 自动选择
};

class BatchACEngine {
public:
    // 统一入口
    int RunSACPass(
        GModel* model,
        std::vector<ProbeTask>& tasks,
        std::vector<int>& failed_vars,
        std::vector<int>& failed_values,
        BatchMode mode = BatchMode::Auto);

private:
    // 自动选择最优模式
    BatchMode SelectOptimalMode(int num_tasks, int num_vars, int num_constraints);

    // 各模式实现
    int RunBatch1(GModel* model, ...);
    int RunBatch2(GModel* model, int micro_batch_size, ...);
    int RunBatch3A(GModel* model, ...);
    int RunBatch3B(GModel* model, ...);
};
```

### 8.2 模式选择策略

```cpp
BatchMode BatchACEngine::SelectOptimalMode(int num_tasks, int num_vars, int num_constraints) {
    // 查询设备能力
    int max_concurrent = QueryMaxConcurrentBlocks();
    size_t available_memory = QueryAvailableMemory();

    // 计算各模式的内存需求
    size_t batch2_memory = num_vars * bit_dom_int_size * sizeof(u32) * max_concurrent;
    size_t batch3b_memory = num_vars * max_dom_size * sizeof(u32);  // World-SIMD

    // 选择策略
    if (num_tasks < 16) {
        return BatchMode::Batch1;  // 任务太少，开销不值得
    }

    if (batch3b_memory < available_memory && num_constraints > 1000) {
        return BatchMode::Batch3B;  // 约束密集，World-SIMD 最优
    }

    if (batch2_memory < available_memory) {
        return BatchMode::Batch3A;  // 异步调度
    }

    return BatchMode::Batch2;  // 空间并行，分批处理
}
```

---

## 9. 实现路线图

```
┌─────────────────────────────────────────────────────────────────────┐
│  Phase 1: Batch-1 ✅ 已完成                                         │
│  ├── 持久 Cooperative Kernel                                        │
│  ├── 快照保存/恢复机制                                              │
│  ├── SAC 全局 GAC 语义                                              │
│  └── 状态一致性验证                                                 │
├─────────────────────────────────────────────────────────────────────┤
│  Phase 2: Batch-2 基础版（1-2 周）                                  │
│  ├── WorldWorkspace 数据结构                                        │
│  ├── 完整域复制方案（方案 A）                                       │
│  ├── Batch2ParallelProbeKernel                                      │
│  └── 正确性验证（对比 Batch-1）                                     │
├─────────────────────────────────────────────────────────────────────┤
│  Phase 3: Batch-2 优化版（1 周）                                    │
│  ├── Copy-on-Write Delta 存储（方案 B）                             │
│  ├── ProbeMemoryPool 实现                                           │
│  └── 内存效率对比                                                   │
├─────────────────────────────────────────────────────────────────────┤
│  Phase 4: Batch-3A 异步调度（2 周）                                 │
│  ├── GlobalTaskQueue (Lock-free)                                    │
│  ├── 持久 Warp 池                                                   │
│  ├── 约束亲和性 + 工作窃取                                          │
│  └── Shared Memory bitSup 缓存                                      │
├─────────────────────────────────────────────────────────────────────┤
│  Phase 5: Batch-3B World-SIMD（研究型，2-3 周）                     │
│  ├── 域表示转置：dom_mask[var][value]                               │
│  ├── 向量化约束检查                                                 │
│  └── 性能极限测试                                                   │
├─────────────────────────────────────────────────────────────────────┤
│  Phase 6: 统一接口 + 自动调优（1 周）                               │
│  ├── BatchACEngine 统一入口                                         │
│  ├── 自动模式选择                                                   │
│  └── 基准测试套件                                                   │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 10. 性能预估

| 模式 | 并发 Probe | GPU 利用率 | 预计加速比 | 适用场景 |
|------|-----------|------------|------------|----------|
| Batch-1 | 1 | ~10% | 1× | 正确性基准 |
| Batch-2 | 16-32 | ~60% | 10-15× | 通用 |
| Batch-3A | 64-128 | ~85% | 30-50× | 负载不均衡问题 |
| Batch-3B | 32 (SIMD) | ~95% | 50-100× | 约束密集问题 |

---

## 11. 关键设计决策总结

1. **SAC 语义**：必须使用全局 GAC（激活所有约束），不能用 MAC 的增量传播

2. **快照共享**：Batch-1 的关键优化，只需 1 份快照服务所有串行 probe

3. **空间并行**：Batch-2/3 的核心，每个 probe 需要独立工作域

4. **内存优化**：Copy-on-Write Delta 存储，稀疏记录修改

5. **持久线程块**：避免 kernel launch 开销，持续处理任务队列

6. **约束亲和性**：软绑定 Block ↔ 约束范围，优化 bitSup 缓存

7. **工作窃取**：消除负载不均衡，提高 GPU 利用率

8. **World-SIMD**：终极优化，一次约束检查服务 32 个 world

---

## 附录 A：已实现代码位置

| 组件 | 文件 | 行号 |
|------|------|------|
| BatchProbeManager | `include/solver/gpu/batch_probe_manager.h` | 全文件 |
| BatchProbeControl | `include/solver/gpu/batch_probe_manager.h` | 30-73 |
| PersistentBatchProbeKernel | `src/solver/gpu/GModel.cu` | 1183-1271 |
| InitializeFrontierForVariable | `src/solver/gpu/GModel.cu` | 1011-1056 |
| RunGACToFixpoint | `src/solver/gpu/GModel.cu` | 1058-1176 |
| SaveSnapshot/RestoreSnapshot | `src/solver/gpu/batch_probe_manager.cu` | 188-240 |
| ExecuteBatch | `src/solver/gpu/batch_probe_manager.cu` | 320-356 |

---

## 附录 B：参考文档

- [BATCH_AC_GPU_DESIGN.md](BATCH_AC_GPU_DESIGN.md) - 初版设计
- [BATCH_AC_GPU_BATCH2_BATCH3_DESIGN.md](BATCH_AC_GPU_BATCH2_BATCH3_DESIGN.md) - Batch-2/3 详细设计
- [Batch_AC.md](Batch_AC.md) - SAC 理论基础
- [UNIFIED_TRAIL_MEMO.md](../bugfixes/UNIFIED_TRAIL_MEMO.md) - Trail 回溯系统

---

*文档版本：v1.0*
*最后更新：2025-01-XX*
*作者：CPIM 团队*
