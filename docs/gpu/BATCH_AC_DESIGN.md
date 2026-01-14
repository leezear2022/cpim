# Batch AC-GPU 综合设计文档 V2

> 本文档综合了 SAC-GPU 批量化实现的完整设计思路，涵盖 Batch-1/2/3 三个层次的并行策略，以及 Jetson 平台适配方案。
>
> **V2 更新**：修正 SAC 语义表述、同步机制、性能优化策略。

---

## 1. 背景与目标

### 1.1 SAC（Singleton Arc Consistency）语义

SAC 的定义是一个"性质"：对于变量 $X_i$ 的值 $a$，将其**单例赋值**（$D(X_i) \leftarrow \{a\}$），然后对整个 CSP 运行 **AC 传播到不动点**：
- 若收敛到非空域：$(X_i, a)$ 是 SAC 一致的
- 若出现空域（DWO）：$(X_i, a)$ 不是 SAC 一致的，需要删除

### 1.2 Frontier 初始化的正确性澄清

**关键澄清**：SAC 要求"跑到全局 AC 不动点"，但 **frontier 初始化方式有两种等价选择**：

| 方式 | 初始化 | 前提条件 | 正确性 | 性能 |
|------|--------|----------|--------|------|
| 全约束激活 | `frontier = ALL_CONSTRAINTS` | 无（安全） | ✓ 正确 | 较慢 |
| **邻域激活** | `frontier = 邻接约束(X_i)` | **snapshot 已 AC** | ✓ 正确（等价） | **更快** |

**为什么邻域激活等价正确**：
- **前提**：base 状态已经是 AC（弧一致）← **关键！**
- 单例赋值 $X_i = a$ 本质是一次"域收缩"
- AC 算法的增量性质：域收缩只需从受影响约束开始传播
- 从 $X_i$ 的邻接约束入队，会自然扩散到所有需要检查的约束
- 最终收敛到同一个 AC 不动点

**当前代码现状与配置策略**：

```cpp
// 当前实现（GModel.cu:1039）：全约束激活（安全 baseline）
// 未来优化方向：可配置策略

enum class FrontierInitStrategy {
    FULL_ACTIVATION,     // 全约束激活（当前默认）
    NEIGHBOR_ACTIVATION  // 邻域激活（优化目标）
};

// 推荐配置逻辑
FrontierInitStrategy SelectStrategy(const GModel& model, bool snapshot_is_ac) {
    if (!snapshot_is_ac) {
        return FULL_ACTIVATION;  // snapshot 未 AC，必须全激活
    }

    #ifdef DEBUG_MODE
        return FULL_ACTIVATION;  // Debug 模式：验证正确性
    #else
        return NEIGHBOR_ACTIVATION;  // Release 模式：性能优先
    #endif
}

// 实现示例
void InitializeFrontierForProbe(
    int var_id, u32* frontier, const GModelData& model,
    FrontierInitStrategy strategy) {

    int bitmap_size_words = (model.num_constraints + 31) / 32;

    if (strategy == FULL_ACTIVATION) {
        // 全约束激活
        for (int w = 0; w < bitmap_size_words; ++w) {
            frontier[w] = 0xFFFFFFFFu;
        }
        // 处理最后 word 的越界位
        int last_bit = model.num_constraints % 32;
        if (last_bit != 0) {
            frontier[bitmap_size_words - 1] &= ((1u << last_bit) - 1);
        }
    } else {
        // 邻域激活
        memset(frontier, 0, bitmap_size_words * sizeof(u32));
        int start = model.d_subscription_offset[var_id];
        int end = model.d_subscription_offset[var_id + 1];
        for (int i = start; i < end; ++i) {
            int cid = model.d_subscription[i].z;
            frontier[cid / 32] |= (1u << (cid % 32));
        }
    }
}
```

**代码对齐说明**：
- 当前 Batch-1 实现使用 **FULL_ACTIVATION**（GModel.cu:1039-1054）
- 优化 Phase 2 将添加 **NEIGHBOR_ACTIVATION** 选项
- 默认策略：snapshot 已 AC 时用邻域激活，否则回退全激活

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
│  • 每个 Block 处理一个 probe（blocks_per_world=1）                  │
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

### 2.2 Batch-2 的并发度与 Launch 模式选择

**关键决策**：Batch-2 使用 `blocks_per_world = 1`，**不使用 cooperative launch**。

#### 方案对比

| 方案 | blocks_per_world | Launch 模式 | 同步机制 | 并发度限制 |
|------|------------------|-------------|----------|------------|
| **推荐** | 1 | 普通 launch | `__syncthreads()` | **无硬限制** |
| 复杂 | >1 | Cooperative | `grid.sync()` | 受可驻留 blocks 限制 (~32) |

#### 推荐方案详解

```cpp
// Batch-2 推荐配置：blocks_per_world=1 + 普通 launch
const int blocks_per_world = 1;  // 每个 world 一个 block
const int block_size = 256;       // 每个 block 256 线程

// 并发度计算（建议值，非硬限制）
int max_active_blocks_per_sm;
cudaOccupancyMaxActiveBlocksPerMultiprocessor(
    &max_active_blocks_per_sm,
    Batch2ProbeKernel,
    block_size,
    shared_mem_size);

// 推荐并发 world 数（优化 SM 利用率）
int recommended_concurrent_worlds = max_active_blocks_per_sm * num_sms;  // ~16-32

// 实际 grid 配置（可超过推荐值，由硬件调度）
int num_worlds = min(num_probes, 256);  // 示例：最多启动 256 个 blocks
dim3 grid(num_worlds, 1, 1);
dim3 block(block_size, 1, 1);

// 普通 launch（非 cooperative）
Batch2ProbeKernel<<<grid, block, shared_mem_size>>>(args...);
```

**关键优势**：
- ✅ 无 cooperative 可驻留 blocks 硬限制（grid 可以远大于 32）
- ✅ `__syncthreads()` 足够（block 内同步），无需 `grid.sync()`
- ✅ 硬件自动调度 blocks 到 SM（无需手动管理驻留）
- ✅ 简单高效，适合 Jetson

#### Cooperative 方案的代价（不推荐）

如果 `blocks_per_world > 1`，必须用 cooperative launch：

```cpp
// 不推荐：需要全 grid 驻留
int max_grid_size = max_active_blocks_per_sm * num_sms;  // ~16-32
int num_worlds = max_grid_size / blocks_per_world;  // 严格受限

void* args[] = {&arg1, &arg2, ...};
cudaLaunchCooperativeKernel(
    (void*)Batch2ProbeKernel,
    dim3(max_grid_size, 1, 1),  // 必须全驻留
    dim3(block_size, 1, 1),
    args,
    shared_mem_size);
```

**代价**：
- ❌ 并发 world 数严格受限（通常 ≤32）
- ❌ 全 grid 锁步同步（负载不均衡时空转）
- ❌ 实现复杂度高

### 2.3 术语澄清：并发度 vs 硬限制

| 术语 | 含义 | Batch-2 中的角色 |
|------|------|------------------|
| `max_active_blocks_per_sm` | 每 SM 可同时驻留的最大 blocks | **建议值**（优化并发度） |
| `recommended_concurrent_worlds` | 推荐的并发 world 数 | **性能建议**（非硬限制） |
| `max_grid_size` (cooperative) | Cooperative 模式的驻留上限 | Batch-2 **不适用** |
| `num_worlds` (实际) | 实际启动的 blocks 数 | 可超过推荐值，硬件调度 |

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
│    [4] 初始化 Frontier (全约束激活 - 当前实现, GModel.cu:1039)       │
│        **注**: Phase2 将引入邻域激活优化                             │
│    [5] RunGACToFixpoint (复用现有 GAC 传播逻辑)                      │
│    [6] 记录结果 (tid==0 执行)                                        │
│  }                                                                   │
└─────────────────────────────────────────────────────────────────────┘
```

**代码现状说明**（对齐 GModel.cu）：
- **当前实现**：步骤[4] 使用 `FULL_ACTIVATION`（GModel.cu:1039-1054）
  - 激活所有约束：`frontier_bitmap[w] = 0xFFFFFFFFu`
  - 处理最后 word 的越界位掩码
  - 作为安全的正确性 baseline
- **Phase2 优化目标**：可选的 `NEIGHBOR_ACTIVATION`（§1.2 详述）
  - 仅激活 var_id 的邻接约束
  - 前提：snapshot 已 AC
  - 预期减少 30-50% frontier 初始化开销

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

### 3.3 性能瓶颈

- Probe 串行执行，GPU 大量空闲
- 典型利用率 ~10%
- 适合作为正确性基准

---

## 4. Batch-2：空间维度批量化

### 4.1 核心思想

B 个 Probe 并行执行，每个有**独立的工作域**。

**强制约束**：`blocks_per_world = 1`（避免跨 block 同步问题）

```
┌──────────────────────────────────────────────────────────────────┐
│  Grid (B blocks, 每个 block 处理一个 world)                      │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐       ┌──────────┐      │
│  │ Block 0  │ │ Block 1  │ │ Block 2  │  ...  │ Block B-1│      │
│  │ World 0  │ │ World 1  │ │ World 2  │       │ World B-1│      │
│  │ Probe 0  │ │ Probe 1  │ │ Probe 2  │       │ Probe B-1│      │
│  │          │ │          │ │          │       │          │      │
│  │ 私有域   │ │ 私有域   │ │ 私有域   │       │ 私有域   │      │
│  │__syncthreads()│ │__syncthreads()│ │ ...  │       │ ...      │      │
│  └──────────┘ └──────────┘ └──────────┘       └──────────┘      │
│       ↑             ↑             ↑                 ↑           │
│       └─────────────┴─────────────┴─────────────────┘           │
│                           │                                      │
│                    共享只读快照                                   │
└──────────────────────────────────────────────────────────────────┘
```

### 4.2 方案选择

#### 方案 A：完整域复制（简单直接，推荐先实现）

```cpp
// 每个 world 完整拷贝一份域
struct WorldWorkspace {
    u32 bitDom[MAX_VARS * MAX_DOM_WORDS];
    int dom_size[MAX_VARS];
    u32 frontier_A[MAX_BITMAP_WORDS];
    u32 frontier_B[MAX_BITMAP_WORDS];
    int inconsistent_flag;
    // ... 其他 GAC 控制状态
};

// 内存需求计算（精确，对齐实际约束数）
// bitmap_size_words = (num_constraints + 31) / 32
int bitmap_size_words = (num_constraints + 31) / 32;

size_t workspace_size =
    num_vars * bit_dom_int_size * sizeof(u32) +     // bitDom
    num_vars * sizeof(int) +                         // dom_size
    2 * bitmap_size_words * sizeof(u32) +            // frontier A/B
    sizeof(GACControlState);                         // 控制状态（~64B）

size_t total_memory = B * workspace_size;

// 示例 1：Queens-12, B=16
// num_vars=12, bit_dom_int_size=1, num_constraints=66
// bitmap_size_words = (66+31)/32 = 3
// workspace = 12*1*4 + 12*4 + 2*3*4 + 64 = 48 + 48 + 24 + 64 = 184 B
// total = 16 * 184 = 2.9 KB

// 示例 2：Queens-100, B=16
// num_vars=100, bit_dom_int_size=4, num_constraints=4950
// bitmap_size_words = (4950+31)/32 = 156
// workspace = 100*4*4 + 100*4 + 2*156*4 + 64 = 1600 + 400 + 1248 + 64 = 3312 B
// total = 16 * 3312 = 51.8 KB
```

#### 方案 B：Copy-on-Write Delta 存储（内存优化）

```cpp
struct DeltaBlock {
    static constexpr int MAX_MODIFIED_VARS = 64;
    static constexpr int HASH_SIZE = 128;  // 必须是 2 的幂

    int probe_id;
    int num_modified;

    // O(1) 查找：var_id → slot 的 hash 表
    int hash_table[HASH_SIZE];  // -1 = 空

    // 实际数据
    int var_ids[MAX_MODIFIED_VARS];
    u32 domains[MAX_MODIFIED_VARS * MAX_DOM_WORDS];
    int dom_sizes[MAX_MODIFIED_VARS];

    // Frontier
    u32 frontier_A[MAX_BITMAP_WORDS];
    u32 frontier_B[MAX_BITMAP_WORDS];
};

// O(1) 查找实现（开放寻址）
__device__ int FindSlot(DeltaBlock& block, int var_id) {
    int h = var_id & (HASH_SIZE - 1);
    for (int i = 0; i < HASH_SIZE; ++i) {
        int idx = (h + i) & (HASH_SIZE - 1);
        int slot = block.hash_table[idx];
        if (slot == -1) return -1;  // 未找到
        if (block.var_ids[slot] == var_id) return slot;
    }
    return -1;
}

// O(1) 分配新 slot
__device__ int AllocateSlot(DeltaBlock& block, int var_id) {
    int slot = atomicAdd(&block.num_modified, 1);
    block.var_ids[slot] = var_id;

    // 插入 hash 表
    int h = var_id & (HASH_SIZE - 1);
    for (int i = 0; i < HASH_SIZE; ++i) {
        int idx = (h + i) & (HASH_SIZE - 1);
        int old = atomicCAS(&block.hash_table[idx], -1, slot);
        if (old == -1) break;  // 成功插入
    }
    return slot;
}
```

### 4.3 Kernel 结构（blocks_per_world=1）

```cuda
__global__ void Batch2ProbeKernel(
    GModelData base_model,
    WorldWorkspace* workspaces,  // [B]
    ProbeTask* tasks,
    int num_tasks,
    bool* results) {

    const int world_id = blockIdx.x;
    if (world_id >= num_tasks) return;

    WorldWorkspace& ws = workspaces[world_id];
    ProbeTask& task = tasks[world_id];

    // [1] 从快照恢复到私有工作域（block 内并行）
    for (int i = threadIdx.x; i < total_dom_words; i += blockDim.x) {
        ws.bitDom[i] = base_model.snapshot[i];
    }
    for (int v = threadIdx.x; v < num_vars; v += blockDim.x) {
        ws.dom_size[v] = base_model.dom_size_snapshot[v];
    }
    __syncthreads();  // block 内同步，OK

    // [2] 单例赋值
    if (threadIdx.x == 0) {
        SingletonAssign(ws, task.var_id, task.value);
    }
    __syncthreads();

    // [3] 初始化 Frontier（邻域激活）
    InitializeFrontierForProbe(task.var_id, ws.frontier_A, base_model);
    __syncthreads();

    // [4] GAC 传播（block 内协作）
    bool inconsistent = RunGACInBlock(ws, base_model);

    // [5] 记录结果
    if (threadIdx.x == 0) {
        results[world_id] = !inconsistent;
    }
}
```

---

## 5. 关键性能优化

### 5.1 Cheap Precheck（安全“早失败”）⭐ 高优先级

在完整 GAC 之前，先做快速预检查：检查赋值变量 `(X=a)` 在**每条邻接约束**上是否仍有直接支持。

**重要修正（收益口径）**：
- 若 base snapshot **已经是 AC**（SAC/MAC 正常前提），则任意保留在域内的 `(X=a)` 必然在每条邻接约束上都有支持；
  因此“早失败”命中率**理论上接近 0%**，更多是**断言/防御性检查**（例如：检测 snapshot 其实未 AC、输入越界、数据损坏等）。
- 它不能替代完整 GAC：即使邻接约束上都有直接支持，也可能在后续传播链上触发 DWO。
- 若需要可观测的“短路率/吞吐提升”，要么做“早成功”（证明不会产生任何删值），要么做更强但更贵的局部传播；这属于后续优化议题。

#### bitSup 方向编码（关键细节）

**bitSupData 结构**（参考 GModel.cu:734）：
```
bitSupData[cid][value_pair_word] : uint2
  .x : var_x 的值被 var_y 支持的位集
  .y : var_y 的值被 var_x 支持的位集
```

**方向选择规则**：
- 若检查变量是 `scope.x`，使用 `uint2.x`（x 的支持）
- 若检查变量是 `scope.y`，使用 `uint2.y`（y 的支持）

#### 实现

```cuda
__device__ bool CheapPrecheck(
    int var_id, int value,
    WorldWorkspace& ws,
    const GModelData& model) {

    // 只检查赋值变量的邻接约束
    int start = model.d_subscription_offset[var_id];
    int end = model.d_subscription_offset[var_id + 1];

    for (int i = start; i < end; ++i) {
        int cid = model.d_subscription[i].z;
        int2 scope = model.constraint_scopes[cid];

        // 判断 var_id 是 x 还是 y，以及对应的另一个变量
        bool is_x = (scope.x == var_id);
        int other_var = is_x ? scope.y : scope.x;

        // 检查 value 在该约束上是否有支持
        bool has_support = CheckValueSupportBitSup(
            cid, var_id, value, other_var, is_x, ws, model);

        if (!has_support) {
            return true;  // 立即 DWO，跳过完整 GAC
        }
    }
    return false;  // 通过预检查，需要完整 GAC
}

// 检查单个值的支持（使用 bitSupData，对齐 ExecuteConstraintCheck_BpC）
__device__ bool CheckValueSupportBitSup(
    int cid, int var_id, int value, int other_var,
    bool is_x,  // var_id 是否是 scope.x
    const WorldWorkspace& ws,
    const GModelData& model) {

    int word_idx = value / 32;
    int bit_idx = value % 32;

    // 计算 bitSup 索引（参考 GModel.cu:734-751）
    // bitsup_per_constraint = max_arity * max_dom_size * bit_dom_int_size
    const int bitsup_per_constraint = 2 * model.max_dom_size * model.bit_dom_int_size;

    // var_id 在约束 scope 中的位置（0=第一个变量，1=第二个变量）
    int var_pos = is_x ? 0 : 1;

    // bitSup 基址：sup_idx_base = cid * bitsup_per_constraint + var_pos * max_dom_size * bit_dom_int_size
    const int sup_idx_base = cid * bitsup_per_constraint
                           + var_pos * model.max_dom_size * model.bit_dom_int_size;

    // 遍历当前值的支持向量（对应 other_var 的每个 word）
    for (int w = 0; w < model.bit_dom_int_size; ++w) {
        u32 other_dom_word = ws.bitDom[other_var * model.bit_dom_int_size + w];
        if (other_dom_word == 0) continue;

        // 获取 bitSup：sup_idx = sup_idx_base + value * bit_dom_int_size + w
        // 注意：bitSupData 是 uint2 数组，.x 和 .y 都存储支持位集（冗余存储）
        int sup_idx = sup_idx_base + value * model.bit_dom_int_size + w;
        uint2 sup = model.bitSupData[sup_idx];

        // 选择对应方向的支持位集（通常 .x 和 .y 相同）
        u32 support_word = is_x ? sup.x : sup.y;

        // Boolean AND 检查：other_dom_word 中是否有值在 support_word 中
        if ((other_dom_word & support_word) != 0) {
            return true;  // 找到支持
        }
    }

    return false;  // 无支持
}

// 在 probe 处理流程中（若 precheck 判定无支持，则必然 DWO）
if (CheapPrecheck(task.var_id, task.value, ws, model)) {
    if (threadIdx.x == 0) {
        results[world_id] = false;  // DWO
    }
    return;  // 跳过完整 GAC
}
// 继续完整 GAC...
```

**可观测结果（当前代码/测试）**：
- 在 `TIER0/TIER1`（AC snapshot 前提）上，Batch-1 的 `short_circuit_count` 实测为 0（短路率 0%）。

**注意事项**：
- bitSup 方向必须正确，否则会误判
- 参考现有实现 `ExecuteConstraintCheck_BpC`（GModel.cu:734）的索引方式

### 5.2 DWO 检测移出热路径 ⭐ 高优先级

**当前问题**：`ExecuteConstraintCheck` 每个约束都重算 `d_cur_dom_size` 并判空，开销大。

**优化方案**：
```cuda
// 约束检查只做删位 + 标记 changed
__device__ PropagateResult ExecuteConstraintCheck_Optimized(
    int cid,
    WorldWorkspace& ws,
    const GModelData& model,
    u32* changed_vars_bitmap) {  // 输出：被修改的变量位图

    // ... 计算新域 new_dom_x, new_dom_y ...

    PropagateResult r = {0, false, false, false};

    if (new_dom_x != old_dom_x) {
        ws.bitDom[var_x_base + w] = new_dom_x;
        r.x_changed = true;
        // 标记 var_x 被修改（不立即判空）
        atomicOr(&changed_vars_bitmap[var_x / 32], 1u << (var_x % 32));
    }
    // var_y 同理...

    return r;  // 不返回 inconsistent，延迟检查
}

// 在每轮迭代的 __syncthreads() 边界集中检查
__device__ bool CheckDWOAndUpdateSizes(
    WorldWorkspace& ws,
    u32* changed_vars_bitmap,
    int num_vars) {

    __shared__ bool any_dwo;
    if (threadIdx.x == 0) any_dwo = false;
    __syncthreads();

    // 并行检查被修改的变量
    for (int v = threadIdx.x; v < num_vars; v += blockDim.x) {
        if (changed_vars_bitmap[v / 32] & (1u << (v % 32))) {
            // 重算域大小
            int new_size = 0;
            for (int w = 0; w < bit_dom_int_size; ++w) {
                new_size += __popc(ws.bitDom[v * bit_dom_int_size + w]);
            }
            ws.dom_size[v] = new_size;

            if (new_size == 0) {
                any_dwo = true;  // 发现 DWO
            }
        }
    }
    __syncthreads();

    // 清空 changed 位图
    for (int w = threadIdx.x; w < (num_vars + 31) / 32; w += blockDim.x) {
        changed_vars_bitmap[w] = 0;
    }
    __syncthreads();

    return any_dwo;
}
```

**预期收益**：减少热路径计算，更一致的并发行为。

### 5.3 Warp-per-Word 约束检查 ⭐ 中优先级

针对 Ampere/Orin 优化约束检查：

```cuda
// 每个 warp 处理一个 domain word（32 个值）
__device__ void CheckConstraintWarpPerWord(
    int cid,
    WorldWorkspace& ws,
    const GModelData& model) {

    int2 scope = model.constraint_scopes[cid];
    int var_x = scope.x, var_y = scope.y;

    // 每个 warp 处理 var_x 的一个 word
    int warp_id = threadIdx.x / 32;
    int lane_id = threadIdx.x % 32;
    int num_warps = blockDim.x / 32;

    for (int word = warp_id; word < bit_dom_int_size; word += num_warps) {
        u32 dom_word = ws.bitDom[var_x * bit_dom_int_size + word];

        // lane i 处理 value = word*32 + i
        int value = word * 32 + lane_id;
        bool keep = false;

        if (dom_word & (1u << lane_id)) {
            // 检查该值是否有支持
            keep = HasSupport(cid, var_x, value, var_y, ws, model);
        }

        // warp 内收集结果
        u32 keep_mask = __ballot_sync(0xFFFFFFFF, keep);

        // lane 0 更新该 word
        if (lane_id == 0) {
            u32 new_word = dom_word & keep_mask;
            if (new_word != dom_word) {
                ws.bitDom[var_x * bit_dom_int_size + word] = new_word;
                // 标记变化...
            }
        }
    }
    __syncwarp();
}
```

**预期收益**：减少 shared memory 原子操作，更好利用 warp 级原语。

### 5.4 只读数据优化 ⭐ 中优先级

**目标**：减少 UMA 一致性开销，提高 L2/纹理/只读缓存命中率。

**适用数据**：`bitSupData`, `subscription`, `constraint_scopes`

#### 方案 A：Managed Memory + MemAdvise（当前使用）

```cpp
// 优化 managed memory 的只读数据（Jetson UMA 适配）
void OptimizeManagedReadOnly(void* ptr, size_t size, int device_id) {
    // 设置只读提示（减少一致性开销）
    cudaMemAdvise(ptr, size, cudaMemAdviseSetReadMostly, device_id);

    // 预取到 GPU（需检查设备是否支持）
    // 参考 gmodel_adapter.cu:441-449 的 Jetson 检测逻辑
    cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, device_id);

    if (prop.concurrentManagedAccess == 0) {
        // Jetson UMA：CPU/GPU 共享物理内存，跳过 prefetch
        // LOG(INFO) << "Device uses integrated UMA, no prefetch needed";
        return;
    }

    // 独立显存设备：prefetch 有效
    cudaMemPrefetchAsync(ptr, size, device_id);
}

// 应用到现有数据
OptimizeManagedReadOnly(bitSupData, bitSup_size, device_id);
OptimizeManagedReadOnly(d_subscription, subscription_size, device_id);
OptimizeManagedReadOnly(constraint_scopes, scopes_size, device_id);
```

**优点**：
- ✅ 简单，不改变数据布局
- ✅ 保留 CPU 端访问能力（调试友好）
- ✅ Jetson UMA 架构友好（自动检测 concurrentManagedAccess）
- ✅ 独立显存设备也有效（条件 prefetch）

#### 方案 B：Device-Only + 纹理/只读缓存（高级）

```cpp
// 迁移到 device-only 内存 + 硬件缓存
void MigrateToDeviceOnly(GModel& model) {
    // 1. 分配 device-only 内存
    u32* d_bitSupData_device;
    cudaMalloc(&d_bitSupData_device, bitSup_size);

    // 2. 拷贝数据（一次性，从 host 或 managed）
    cudaMemcpy(d_bitSupData_device, h_bitSupData,
               bitSup_size, cudaMemcpyHostToDevice);

    // 3a. 选项 1：创建纹理对象（已有，继续使用）
    cudaResourceDesc resDesc = {};
    resDesc.resType = cudaResourceTypeLinear;
    resDesc.res.linear.devPtr = d_bitSupData_device;
    resDesc.res.linear.sizeInBytes = bitSup_size;
    // ... 创建纹理对象

    // 3b. 选项 2：使用 __ldg() 内建函数（只读缓存）
    // 在 kernel 中：u32 val = __ldg(&d_bitSupData[idx]);
}

// 在 kernel 中使用 __ldg()
__device__ uint2 ReadBitSupReadOnly(
    const u32* bitSupData, int cid, int word_x, int word_y,
    int bit_dom_int_size) {

    int base = cid * bit_dom_int_size * bit_dom_int_size * 2;
    int idx = base + (word_x * bit_dom_int_size + word_y) * 2;

    // 使用只读缓存（L1 或 texture cache）
    u32 x_support = __ldg(&bitSupData[idx]);
    u32 y_support = __ldg(&bitSupData[idx + 1]);

    return make_uint2(x_support, y_support);
}
```

**优点**：
- ✅ 最大化缓存效率（纹理缓存 + 只读缓存）
- ✅ 减少 managed 一致性协议开销

**缺点**：
- ❌ CPU 端无法直接访问（需要额外拷贝）
- ❌ 实现复杂度较高

#### 推荐策略

| 阶段 | 方案 | 说明 |
|------|------|------|
| **当前/短期** | 方案 A（Managed + MemAdvise） | 简单有效，Jetson UMA 友好 |
| **优化阶段** | 方案 B（Device-Only + __ldg）| 最大化性能，适合生产环境 |

**预期收益**：
- 方案 A：减少 10-20% 一致性开销
- 方案 B：提高 20-30% 缓存命中率

---

## 6. Batch-3A：异步调度 + 工作窃取

### 6.1 核心问题

Batch-2 使用 block 级并行，但存在：
- 不同 probe 收敛速度不同
- 需要分批处理（每批 B 个）

### 6.2 解决方案：任务驱动的异步推进

```
┌─────────────────────────────────────────────────────────────────────┐
│  约束聚合任务队列（同一约束的多个 world 聚合）                      │
│                                                                      │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │  Task Queue                                                   │   │
│  │  [(cid=5, world_mask=0b0011), (cid=12, world_mask=0b0101), ...]│   │
│  │                                                               │   │
│  │  优势：同一 cid 的 bitSup 只加载一次，服务多个 world          │   │
│  └───────────────────────────┬──────────────────────────────────┘   │
│                              │                                       │
│         ┌────────────────────┼────────────────────┐                 │
│         ▼                    ▼                    ▼                 │
│  ┌─────────────┐      ┌─────────────┐      ┌─────────────┐         │
│  │ Persistent  │      │ Persistent  │      │ Persistent  │         │
│  │ Block 0     │      │ Block 1     │      │ Block K     │         │
│  │ Affinity:   │      │ Affinity:   │      │ Affinity:   │         │
│  │ C[0-99]     │      │ C[100-199]  │      │ C[K*100-]   │         │
│  └─────────────┘      └─────────────┘      └─────────────┘         │
│                                                                      │
└─────────────────────────────────────────────────────────────────────┘
```

### 6.3 约束聚合队列

```cpp
// 任务结构：(constraint_id, world_mask)
struct AggregatedTask {
    int cid;           // 约束 ID
    u32 world_mask;    // 哪些 world 需要检查该约束
};

// 两级位图优化：避免 O(C) 扫描
struct TaskQueue {
    u32* constraint_masks;        // [num_constraints] per-constraint world masks
    u32* active_chunks;           // [num_chunks] 每个 chunk 是否有活跃约束
                                   // num_chunks = (num_constraints + 31) / 32
};

// 队列操作（优化版：O(C/32) → 接近 O(1)）
__device__ AggregatedTask PopTask(TaskQueue* queue, int affinity_start, int affinity_end, int num_constraints) {
    int num_chunks = (num_constraints + 31) / 32;

    // 1. 优先从亲和范围找任务（两级查找）
    int chunk_start = affinity_start / 32;
    int chunk_end = (affinity_end + 31) / 32;

    for (int chunk = chunk_start; chunk < chunk_end; ++chunk) {
        u32 active_mask = queue->active_chunks[chunk];
        if (active_mask == 0) continue;

        // 在该 chunk 内查找
        int base = chunk * 32;
        int end = min(base + 32, num_constraints);
        for (int c = base; c < end; ++c) {
            if (c < affinity_start || c >= affinity_end) continue;

            u32 mask = atomicExch(&queue->constraint_masks[c], 0);
            if (mask != 0) {
                // 如果该约束清空，更新 chunk 位图
                if (atomicLoad(&queue->constraint_masks[c]) == 0) {
                    atomicAnd(&queue->active_chunks[chunk], ~(1u << (c % 32)));
                }
                return {c, mask};
            }
        }
    }

    // 2. 工作窃取：从全局找任务（两级查找）
    for (int chunk = 0; chunk < num_chunks; ++chunk) {
        u32 active_mask = atomicExch(&queue->active_chunks[chunk], 0);
        if (active_mask == 0) continue;

        // 在该 chunk 内查找
        int base = chunk * 32;
        for (int bit = 0; bit < 32; ++bit) {
            if (!(active_mask & (1u << bit))) continue;

            int c = base + bit;
            if (c >= num_constraints) break;

            u32 mask = atomicExch(&queue->constraint_masks[c], 0);
            if (mask != 0) {
                // 恢复 chunk 位图中其他位
                u32 remaining_mask = active_mask & ~(1u << bit);
                if (remaining_mask != 0) {
                    atomicOr(&queue->active_chunks[chunk], remaining_mask);
                }
                return {c, mask};
            }
        }
    }

    return {-1, 0};  // 无任务
}

// 插入任务时更新两级位图
__device__ void PushTask(TaskQueue* queue, int cid, u32 world_mask) {
    atomicOr(&queue->constraint_masks[cid], world_mask);
    int chunk = cid / 32;
    int bit = cid % 32;
    atomicOr(&queue->active_chunks[chunk], 1u << bit);
}

**复杂度分析**：
- **旧版**：O(C) 线性扫描（C=1000+ 时开销显著）
- **新版**：O(C/32) chunk 扫描 + O(32) bit 扫描 ≈ O(1) 对于合理 C

**world_mask 扩展**（超过 32 worlds）：
- 方案 A：改用 `u64 world_mask` 支持 64 worlds
- 方案 B：多个 u32 数组 `world_masks[MAX_WORLDS/32]`
- 方案 C：链表结构（复杂，不推荐）
```

### 6.4 处理流程

```cuda
__device__ void ProcessAggregatedTask(
    AggregatedTask task,
    WorldPool* worlds,
    const GModelData& model,
    u32* shared_bitSup) {

    // 1. 加载 bitSup 到 shared memory（一次）
    LoadBitSupToShared(task.cid, model, shared_bitSup);
    __syncthreads();

    // 2. 对每个需要处理的 world 执行约束检查
    for (int w = 0; w < 32; ++w) {
        if (task.world_mask & (1u << w)) {
            CheckConstraintForWorld(task.cid, w, worlds, shared_bitSup);
        }
    }
}
```

**优势**：bitSup 只加载一次，服务多个 world，特别适合 Jetson 的小 L1/L2。

---

## 7. Batch-3B：World-SIMD（极致优化）

### 7.1 核心创新：域表示转置

```
传统表示（per-world）:
  world[w].dom[var][word] : u32

World-SIMD 转置表示:
  dom_mask[var][value] : u32  ← 第 w 位表示 world w 是否有该值
```

### 7.2 约束检查向量化（基于 bitSup）

```cuda
// 使用 bitSup 位集的向量化检查
__device__ void CheckConstraintSIMD(
    int cid,
    u32 world_mask,              // 哪些 world 需要处理
    u32 dom_mask[][MAX_VAL],     // 转置的域表示
    const GModelData& model) {

    int2 scope = model.constraint_scopes[cid];
    int var_x = scope.x, var_y = scope.y;

    // 逐 word 处理（利用 bitSup 的位集结构）
    for (int wx = 0; wx < bit_dom_int_size; ++wx) {
        for (int wy = 0; wy < bit_dom_int_size; ++wy) {
            // 获取 bitSup 行（一个 word 的支持信息）
            uint2 sup = tex3D(bitSupTex, wy, wx, cid);

            // sup.x: var_x 的 word wx 中哪些值被 var_y 的 word wy 支持
            // sup.y: var_y 的 word wy 中哪些值被 var_x 的 word wx 支持

            // 向量化计算：哪些 world 中这些值有支持
            // ...
        }
    }
}
```

### 7.3 实现复杂度

需要重构的组件：
1. 域表示：`bitDom[var][word]` → `dom_mask[var][value]`
2. 约束检查：需要适配 bitSup 的位集结构
3. Frontier：per-world bitmap → `world_mask` per constraint

**建议**：作为研究方向，在 Batch-2/3A 稳定后再考虑。

---

## 8. GPU 内存池设计

### 8.1 分层内存架构

```
┌─────────────────────────────────────────────────────────────────────┐
│  Layer 0: 共享只读快照（所有 probe 共享）                             │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │  base_snapshot: bitDom[num_vars × bit_dom_int_size]         │    │
│  │  base_dom_sizes: d_cur_dom_size[num_vars]                   │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                              │                                      │
│                              ▼ Copy-on-Write (方案 B)               │
│  Layer 1: 差异存储（每个活跃 probe 一份 DeltaBlock）                  │
│  ┌──────────────┐  ┌──────────────┐       ┌──────────────┐          │
│  │ DeltaBlock 0 │  │ DeltaBlock 1 │  ...  │ DeltaBlock K │          │
│  │ O(1) hash    │  │ O(1) hash    │       │ O(1) hash    │          │
│  └──────────────┘  └──────────────┘       └──────────────┘          │
│                                                                     │
│  Layer 2: Lock-free 内存池管理器                                     │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │  free_stack: atomic<int>                                    │    │
│  └─────────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────────┘
```

### 8.2 SoA 内存布局（L2 缓存优化）

```cpp
struct DeltaPoolSoA {
    // 热数据（频繁访问）- 连续存放
    int probe_ids[POOL_SIZE];
    int num_modified[POOL_SIZE];

    // 温数据（GAC 迭代访问）
    int var_ids[POOL_SIZE][MAX_MODIFIED];
    u32 domains[POOL_SIZE][MAX_MODIFIED][MAX_WORDS];

    // 冷数据
    u32 frontier_A[POOL_SIZE][MAX_BITMAP_WORDS];
    u32 frontier_B[POOL_SIZE][MAX_BITMAP_WORDS];
};
```

---

## 9. 统一接口设计

### 9.1 自动模式选择

```cpp
enum class BatchMode { Batch1, Batch2, Batch3A, Batch3B, Auto };

class BatchACEngine {
public:
    int RunSACPass(
        GModel* model,
        std::vector<ProbeTask>& tasks,
        std::vector<int>& failed_vars,
        std::vector<int>& failed_values,
        BatchMode mode = BatchMode::Auto);

private:
    BatchMode SelectOptimalMode(int num_tasks, const GModel& model);
};
```

### 9.2 模式选择策略

```cpp
BatchMode BatchACEngine::SelectOptimalMode(int num_tasks, const GModel& model) {
    // 1. 查询设备能力
    cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, 0);  // 假设设备 0
    int num_sms = prop.multiProcessorCount;

    // 计算 shared memory 需求（依赖实现）
    size_t shared_mem_size = 2 * model.bit_dom_int_size * sizeof(u32);  // 2个域的 shared memory

    int max_blocks_per_sm;
    cudaOccupancyMaxActiveBlocksPerMultiprocessor(
        &max_blocks_per_sm, Batch2ProbeKernel, 256, shared_mem_size);

    int max_concurrent_worlds = max_blocks_per_sm * num_sms;  // ~16-32 for Orin

    // 2. 计算 Batch-2 内存需求
    int bitmap_size_words = (model.num_constraints + 31) / 32;
    size_t workspace_size =
        model.num_vars * model.bit_dom_int_size * sizeof(u32) +
        model.num_vars * sizeof(int) +
        2 * bitmap_size_words * sizeof(u32) +
        sizeof(GACControlState);  // ~64B

    size_t batch2_memory = max_concurrent_worlds * workspace_size;

    // 3. 查询可用内存
    size_t free_memory, total_memory;
    cudaMemGetInfo(&free_memory, &total_memory);

    // 4. 选择策略
    if (num_tasks < 8) {
        return BatchMode::Batch1;  // 任务太少，开销不值得
    }

    if (batch2_memory > free_memory * 0.8) {
        // 内存不足，用 Batch-1 分批处理
        return BatchMode::Batch1;
    }

    if (num_tasks > max_concurrent_worlds * 4) {
        // 任务很多，用 Batch-3A 异步调度
        return BatchMode::Batch3A;
    }

    // 默认 Batch-2
    return BatchMode::Batch2;
}
```

---

## 10. 实现路线图（按落地优先级）

### 推荐落地顺序（基于 Orin 平台特性）

```
┌─────────────────────────────────────────────────────────────────────┐
│  Phase 1: Batch-1 ✅ 已完成                                         │
│  ├── 持久 Cooperative Kernel                                        │
│  ├── 快照保存/恢复机制                                              │
│  ├── 全约束激活（安全 baseline）                                    │
│  └── 验收：TIER0/TIER1 正确性 100%                                  │
├─────────────────────────────────────────────────────────────────────┤
│  Phase 2: Batch-1 性能优化（1 周）⭐ 最优先                         │
│  ├── [P0] Frontier 初始化策略化（GModel.cu:1011）                   │
│  │   ├── 添加 FrontierInitStrategy 枚举                             │
│  │   ├── 实现邻域激活分支（snapshot 已 AC 时默认）                 │
│  │   └── 保留全激活作为 debug/fallback                              │
│  ├── [P0] Cheap Precheck（两级短路）                                │
│  │   ├── 实现 CheckValueSupportBitSup（注意 bitSup 方向！）        │
│  │   ├── 在 RunGACToFixpoint 前调用                                 │
│  │   └── 统计短路率                                                 │
│  ├── [P1] DWO 检测移出热路径                                        │
│  │   ├── 约束检查只删位 + 标记 changed_vars_bitmap                  │
│  │   └── __syncthreads() 边界集中检查空域 + 更新 dom_size          │
│  └── 验收：吞吐提升 2-3×（以实测为准；frontier 优化确定有收益）      │
├─────────────────────────────────────────────────────────────────────┤
│  Phase 3: Batch-2 基础版（1-2 周）                                  │
│  ├── WorldWorkspace 数据结构（完整域复制）                          │
│  ├── Batch2ProbeKernel 实现                                         │
│  │   ├── blocks_per_world=1（强制，避免同步问题）                   │
│  │   ├── 普通 launch（非 cooperative）                              │
│  │   └── 并发度 = min(num_probes, 256)                              │
│  ├── 复用 Phase 2 的优化（邻域初始化 + precheck）                   │
│  └── 验收：TIER0 正确性 100%，吞吐比优化后 Batch-1 提升 5-10×      │
├─────────────────────────────────────────────────────────────────────┤
│  Phase 4: Batch-2 优化版（1 周）                                    │
│  ├── Copy-on-Write Delta 存储                                       │
│  │   ├── DeltaBlock 结构 (O(1) hash 查找)                           │
│  │   ├── ProbeMemoryPool 管理器                                     │
│  │   └── SoA 内存布局（L2 缓存优化）                                │
│  ├── Warp-per-Word 约束检查（可选）                                 │
│  ├── 只读数据优化（Managed + MemAdvise，简单）                      │
│  └── 验收：内存占用减少 50%+，吞吐进一步提升 20-30%                 │
├─────────────────────────────────────────────────────────────────────┤
│  Phase 5: Batch-3A 异步调度（2 周，可选）                           │
│  ├── 约束聚合任务队列 (cid, world_mask)                             │
│  ├── 持久线程块 + 约束亲和性                                        │
│  ├── Shared Memory bitSup 缓存                                      │
│  └── 验收：负载不均衡问题吞吐提升 2× 以上                           │
├─────────────────────────────────────────────────────────────────────┤
│  Phase 6: Batch-3B World-SIMD（研究型，长期）                       │
│  ├── 域表示转置                                                     │
│  ├── 向量化约束检查（基于 bitSup 位集）                             │
│  └── 验收：约束密集问题吞吐达到理论峰值 80%+                        │
└─────────────────────────────────────────────────────────────────────┘
```

### 关键里程碑

| 阶段 | 目标 | 验收门槛 | 预计收益 |
|------|------|----------|----------|
| **Phase 2** | Batch-1 优化 | TIER0 100%，吞吐 2-3× | 立竿见影 |
| **Phase 3** | Batch-2 基础 | TIER0 100%，吞吐 5-10× | 核心突破 |
| **Phase 4** | Batch-2 优化 | 内存 -50%，吞吐 +20-30% | 工程质量 |
| Phase 5 | Batch-3A | 吞吐 +2× | 进阶优化 |
| Phase 6 | Batch-3B | 吞吐达峰值 80%+ | 研究方向 |

### 为什么这个顺序？

1. **Phase 2 优先**：
   - 邻域初始化 + Cheap Precheck 收益大（2-3×）
   - 工程风险低（不改变 kernel 架构）
   - 为 Batch-2 打好基础（代码复用）

2. **Phase 3 非 Cooperative**：
   - 避开 Jetson cooperative grid 限制
   - blocks_per_world=1 简化同步逻辑
   - 并发度无硬限制（硬件自动调度）

3. **Phase 4 内存优化**：
   - Batch-2 稳定后再做 CoW（降低风险）
   - SoA 布局针对 Orin L2 Cache

4. **Phase 5/6 可选**：
   - 根据实际性能需求决定是否推进
   - Batch-3A 适合负载不均衡场景
   - Batch-3B 是理论上限（研究价值）

---

## 11. 验收标准

### 11.1 正确性验收

| 阶段 | TIER0 | TIER1 | 统计口径 |
|------|-------|-------|----------|
| Batch-1 | 100% | 100% | 删值一致性（与 CPU AC3bit 对比） |
| Batch-2 | 100% | 100% | 删值一致性 |
| Batch-3A | 100% | 100% | 删值一致性 |

### 11.2 性能验收

| 指标 | Batch-1 | Batch-2 | Batch-3A | 统计口径 |
|------|---------|---------|----------|----------|
| Probe 吞吐 | baseline | **5-15×** | **20-50×** | probes/second |
| GPU 利用率 | ~10% | ~50-70% | ~80-90% | nsight metrics |
| Precheck 短路率（早失败） | N/A | ~0%（AC snapshot 前提） | ~0%（AC snapshot 前提） | (precheck 判定 DWO) / 总 probes |
| 完整 GAC 平均迭代 | baseline | 相同 | 相同 | 对未短路 probes 的平均迭代数 |

### 11.3 测试用例

```bash
# TIER0: 快速验证
queens-4, queens-12, test.xml, langford-2-4

# TIER1: 中等规模
langford-3-9, rand-2-40-*, driverlogw-01c-sat

# 性能基准（吞吐测试）
queens-100, rand-2-40-80-103-800-*
```

---

## 12. 关键设计决策总结

1. **SAC 语义**：要求跑到 AC 不动点，但 frontier 可用邻域初始化（等价且更快）

2. **World 内同步**：Jetson 优先 `blocks_per_world=1`，避免跨 block 同步复杂度

3. **快照共享**：Batch-1 的关键优化，只需 1 份快照服务所有串行 probe

4. **两级短路**：Cheap Precheck 大幅减少无效 GAC 传播

5. **DWO 检测延迟**：移出热路径，在迭代边界集中检查

6. **Copy-on-Write**：O(1) hash 查找，稀疏存储修改

7. **约束亲和性**：软绑定 Block ↔ 约束范围，优化 bitSup 缓存

8. **约束聚合**：同一 cid 的 bitSup 只加载一次，服务多个 world

---

## 附录 A：已实现代码位置

| 组件 | 文件 | 说明 |
|------|------|------|
| BatchProbeManager | `include/solver/gpu/batch_probe_manager.h` | Host 端管理器 |
| PersistentBatchProbeKernel | `src/solver/gpu/GModel.cu:1183` | Batch-1 Kernel |
| InitializeFrontierForVariable | `src/solver/gpu/GModel.cu:1011` | Frontier 初始化 |
| RunGACToFixpoint | `src/solver/gpu/GModel.cu:1058` | GAC 传播 |
| ExecuteBatch | `src/solver/gpu/batch_probe_manager.cu:320` | 批量执行入口 |

---

## 附录 B：参考文档

- [BATCH_AC_GPU_BATCH2_BATCH3_DESIGN.md](BATCH_AC_GPU_BATCH2_BATCH3_DESIGN.md) - Batch-2/3 原始设计
- [Batch_AC.md](Batch_AC.md) - SAC 理论基础

---

## 附录 C：V2 更新日志

### V2.0 主要修正（2025-01）

1. **SAC 语义精确化**（§1.2）
   - 明确"跑到 AC 不动点"vs"frontier 初始化"的区别
   - 添加邻域激活的前提条件（snapshot 已 AC）
   - 对齐当前代码现状（GModel.cu:1039 仍是全激活）
   - 提供可配置策略和 debug/fallback 机制

2. **Batch-2 同步机制澄清**（§2.2-2.3）
   - 强制 `blocks_per_world=1`，使用普通 launch（非 cooperative）
   - 澄清 `max_grid_size` 概念（建议值 vs 硬限制）
   - 说明并发度无硬限制（硬件自动调度）

3. **Cheap Precheck bitSup 方向细节**（§5.1）
   - 补充 bitSupData 的 uint2 方向编码规则
   - 添加 `is_x` 参数正确选择 `.x` 或 `.y`
   - 参考 ExecuteConstraintCheck_BpC 实现（GModel.cu:734）

4. **只读数据优化方案分离**（§5.4）
   - 方案 A：Managed + MemAdvise（当前推荐）
   - 方案 B：Device-Only + __ldg（高级优化）
   - 避免 cudaMalloc 和 cudaMemAdvise 混用

5. **验收指标修正**（§11.2）
   - 新增 "Precheck 短路率" 指标
   - "平均 GAC 迭代" 改为 "完整 GAC 平均迭代"（仅统计通过 precheck 的 probes）

6. **路线图重组**（§10）
   - 按落地优先级排序（而非并行度层次）
   - Phase 2 最优先（邻域初始化 + Cheap Precheck）
   - 明确 Batch-2 使用非 cooperative launch
   - 添加关键里程碑和收益预估

---

*文档版本：v2.0*
*最后更新：2025-01-26*
*作者：CPIM 团队*
*评审：GPT-4 / Claude*
