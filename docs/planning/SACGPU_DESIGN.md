---
status: active
---

# CPIM：SAC/MSAC 的 GPU 加速设计（统一版）

> **状态**：设计 + 实现同步中（对齐 2026-01 代码现状）
> **平台**：Jetson Orin（8 SM，UMA；`concurrentManagedAccess==0`）
> **目标**：在大实例/长尾实例上实现可控、可预算、可回退的 SAC/MSAC/NSAC 加速，
> 并保证 sound（宁可少删，不许多删）
>
> **代码锚点（现状）**
> - `src/solver/gpu/GModelSolver.cu`：`GModelSolver::EnforceSAC1()`
> - `include/solver/gpu/batch_probe_manager.h`：Batch-2 Stage1/Stage2/Auto
> - `src/model/gmodel_adapter.cu`：只读数据 hint（`cudaMemAdviseSetReadMostly`）
>
> 本文整合 `docs/planning/SACGPU_DESIGN.md` 与 `docs/planning/SACGPU_DESIGN copy.md` 的内容，
> 作为主入口。

---

## 0. 术语与“扁平化”的层次

为避免讨论时“flatten”指代不清，本文区分三层（由易到难）：

1. **Probe-level 扁平化（已落地）**：把“对每个 probe 独立跑一轮 AC”的串行结构，
   改成 GPU 上 worker blocks 从 probe 队列拉任务（Batch-2 Stage 2 / Persistent Blocks），
   解决 probe 间负载不均衡。
2. **Constraint-level 扁平化（Phase 5 / Batch-3A/3D）**：把“每个 probe 内部的 AC 传播队列”
   跨 probes 扁平化为 `<cid, world_mask>` 任务队列（约束聚合），追求 bitSup 复用与更强负载均衡。
3. **Word/value-level 打散（Batch-3D 激进形态）**：把任务单位进一步细化为
   `<world,cid>`/`<world,cid,word>` 甚至 `<world,cid,value>`，试图消除单个长尾 probe 的内部长尾。

本文主线优先级：先把（1）+ 算法 budget/降级做扎实，再用数据判定（2）（3）是否值得做。

## 1. 核心矛盾与问题定义

### 1.1 为什么要做 SAC/MSAC 的 GPU 加速？

- **强推理（SAC/MSAC）能显著缩小搜索树**，减少回溯和节点数
- 代价是大量"试值 probe + AC 到不动点"的重复传播，CPU 上成本极高
- GPU 适合把 **probe 独立世界（world）并行化**：每个 probe 是一个"复制后的 CSP 世界"

### 1.2 三个核心矛盾

#### 矛盾 A：算法长尾不是"并行度"能直接抹平的

SAC 的本质是：对每个候选值做一次"赋值→AC 到不动点→判不一致"。当 `tightness=0.9` 时，证明"不 SAC"往往要跑很深的传播序列，天然长尾。

> **结论**: 如果不改变"每次 probe 都从零做大量 support 检查"的事实，再怎么调度也会被长尾吞掉。

#### 矛盾 B：warp 利用率与内存形态错误

`bit_dom_int_size=6` 时 lane→word 只活 6/32；再叠加 `__ffs` 扫 bit 的分歧，算力侧天然吃不满。

> **结论**: Batch 维度应该用来填满 warp（lane→world），而不是继续 lane→word。

#### 矛盾 C：bitSup 复用需要 dom 矩阵化

每个 value 都要读对侧域 `dom_y[w]`。批处理后拥有 `B × W` 的域矩阵，如果仍按 bitGEMV 做，dom 侧会被重复读。

> **结论**: 必须把 bitGEMV 变成 bitGEMM，让 `dom_y` 以矩阵形态进 shared/L2。

### 1.3 代码现状对齐（截至 2026-01）

这份设计并非“从零开始”，当前仓库已具备可跑通的主线与大量关键优化：

- **Probe-level 并行（Batch-2）**：Stage 1（Micro-Batch）与 Stage 2（Persistent Blocks），并有 Auto 选择。
- **传播内核优化**：NEIGHBOR_ACTIVATION、Warp-per-Word、域大小增量更新、只读数据 hint 等已落地并验证。
- **SAC 主线集成**：`GModelSolver::EnforceSAC1()` 已接入 dirty-set 增量任务收集与 Stage auto 选择。
- **仍需落地/加强的关键点**：
  - “真 NSAC”的 `allowed-constraints mask`（让 singleton test 的 AC 真正只在邻域子图传播）
  - budget 触发后的 **UNKNOWN 语义与可观测统计闭环**（UNKNOWN 一律不删）
  - Phase 5（Batch-3A/3D）的约束聚合/扁平化队列：作为可选加速器的数据驱动启用/回退

---

## 2. 两条主线优化路径

### 主线 1：bitGEMM 化的 Batched-AC（GPU 内核形态）

把 Batched-AC 的核心 check 变成 bitGEMM 形态（lane→world），直接解决 warp 空转和 dom 重复读。

### 主线 2：NSACQ/SACQ 队列驱动（算法形态）

从 SAC-1 升级到队列驱动的 NSACQ/SACQ，用结构性局部化减少长尾。

---

## 3. 理论背景：SACQ / NSACQ

### 3.1 SACQ：队列驱动的 SAC

Wallace 的 SACQ 用 AC-3 风格队列把 SAC 外层变成 worklist：
- 从变量队列 `Q` 拿一个变量 `Xi`
- 对 `dom(Xi)` 的每个值做 singleton test
- 若删了值，则把相关变量重新入队

**关键特性**：删值后没有额外的"AC phase"，靠队列机制在后续处理中自然发现并传播。

### 3.2 NSACQ：邻域限制

NSACQ 进一步把 singleton test 里的 AC 限制在 **"Xi + neighbours(Xi)" 的邻域子图**：
- 天然减少每个 probe 的传播工作量
- 结构性抑制长尾
- Wallace 实验结论：queue-based 的 SACQ 在"最难实例"上通常更快

### 3.3 NSAC 的理论边界

Bessière & Debruyne 指出：所谓 "SAC-Neighboring-Support" 并不能推出一个值最终仍然 SAC。这说明：
- NSAC 是一个合理的"弱化一致性"
- 不能当 SAC，但可以当一个**可控的 GPU-friendly pruning 级别**

### 3.4 与 GPU 的契合

- 传统 SAC-1 的"全量扫一遍"对 GPU 不友好（控制流嵌套、重复工作多）
- SACQ/NSACQ 的队列驱动更适合 GPU 的 persistent kernel 模式
- 预算控制把长尾变成"可控的不删"

---

## 4. Proposition: Batched AC Implementation of SAC

### 4.1 正式定义（LaTeX）

```latex
\begin{proposition}[Batched AC Implementation of SAC]
\label{prop:batched-ac-sac}
Let $P=(X,D,C)$ be a CSP. For a variable-value pair $(x,a)$, denote by
$P\restriction_{x=a}$ the subproblem obtained by assigning $x=a$.
Let $\mathrm{AC}(\cdot)$ be an arc-consistency enforcement operator.

Consider the set of singleton probes
$\mathcal{W}=\{w_{x,a}\mid x\in X,\ a\in D(x)\}$, where each world
$w_{x,a}$ runs $\mathrm{AC}$ on an isolated copy of $P\restriction_{x=a}$.

Executing these worlds in a batched manner produces exactly the same SAC
classification as executing them sequentially, provided that:
(i) each world has isolated mutable state, and
(ii) the batched execution preserves per-world AC semantics.
\end{proposition}

\begin{proof}
Arc-consistency enforcement is monotone w.r.t. domain deletions, and its
fixpoint depends only on world-local state. Under assumptions (i) and (ii),
parallelizing across worlds does not introduce cross-world interference;
hence each world reaches the same terminal state as in sequential execution.
\end{proof}
```

### 4.2 从 bitGEMV 到 bitGEMM

**单世界 support check（bitGEMV）**：
```
supp(a) = (S^{x→y}_c(a))^T ⊙ B_y ≠ 0
```

**批处理世界（bitGEMM）**：
```
B_y ∈ {0,1}^{|D(y)| × m}  # m 个 world 的域矩阵
r(a)[j] = I((S(a))^T ⊙ B_y[:,j] ≠ 0)  # 第 j 个 world 的支持结果
```

**Bit-matrix packing（32 worlds per group）**：
```
M_y[v] ∈ {0,1}^32  # 第 j 位表示 v ∈ D^(j)(y)
mask(a) = ⋁_{v ∈ S(a)} M_y[v]  # 32 个 world 的支持情况
M_x[a] ← M_x[a] ∧ mask(a)  # 单指令同时处理 32 worlds
```

---

## 5. 可分级的一致性 Pipeline（Level 0-3）

### Level 0：AC（基线）
现有 EnforceGAC / PersistentGAC，MAC 的每个搜索节点都要做。

### Level 1：NSACQ（默认 GPU 强推理）
- **外层**: 变量队列（只重入队邻居）
- **内层**: singleton test 只跑 `Xi + neighbours(Xi)` 的邻域子图 AC
- **预算**: 每个 probe 设定 `max_iters / max_work`，超过标记 UNKNOWN

### Level 2：SACQ-adj / Budgeted-SACQ
- 更接近 full SAC，但用邻居入队或 queue-cap 控制增长
- 仍保留 budget

### Level 3：Full SACQ
- 仅在少数节点启用（root 或疑难值）
- 追求接近 SAC fixpoint

---

## 6. 双阀门预算控制

### 阀门 A：内层（per-probe）
```cpp
struct ProbeBudget {
    int max_iters;           // 最大迭代轮数
    int max_work_items;      // 最大处理约束数
    int max_frontier_words;  // 最大 frontier 扩张
};
```
- hit budget → `status = UNKNOWN`（不删）
- 只有 `status == DWO` 才删值

### 阀门 B：外层（queue-level）
```cpp
struct QueueBudget {
    int max_total_probes;    // 总 probe 数上限
    int max_queue_len;       // 队列长度上限
    int max_requeue_count;   // 最大重入队次数
};
```
- hit budget → 停止本轮强推理，回退到 MAC

### 安全语义

关键分清两类"变弱"：
- **安全的变弱（sound but incomplete）**: budget 触发时返回 UNKNOWN/不删，只是不如 SAC 强，但绝不误删
- **不安全的变弱（unsound）**: 允许"软错误"导致"多删值"，直接错误

**原则：任何 budget/降级都必须走"宁可少删、不许多删"的语义**

---

## 7. bitGEMV → bitGEMM 内核重构方案

### 7.1 Option-1（推荐）：Warp = 1 个 value，lane = world

**数据布局（SoA）**：
```cpp
// 现有
bitDom[var][word]
// 改为（world 连续）
DomWord[var][word][world]
// 地址：dom_soa + ((var * W + word) * B + world)
```

**Kernel 结构**：
```
grid:  blockIdx.x = cid
block: 128/256 threads (4/8 warps)
warp:  覆盖 val 维，lane = world
```

**共享内存**：
```cpp
__shared__ uint32_t sh_dom_x[W][B];  // W=6, B=32 → 768 bytes
__shared__ uint32_t sh_dom_y[W][B];  // 总计 ~1.5 KB
```

**核心循环**：
```cpp
// 对固定 val，每 lane(world)
uint32_t acc = 0;
for (int w = 0; w < W; ++w) {
    acc |= (S[val, w] & sh_dom_other[w][lane]);
}
bool has_sup = (acc != 0);
```

> 这直接把"活跃 lane 只有 6/32"翻成"活跃 lane 32/32"。**质变**。

### 7.2 Option-2：Warp = word，lane = world
适用于 `max_dom_size` 很大时，当前参数不是首选。

### 7.3 Option-3：32 worlds packed（bit-matrix）
最激进，单指令处理 32 worlds，但需要重构 dom_size/trail。

### 7.4 选型对比表

| 选型 | 并行映射 | 优点 | 风险/缺点 | 推荐度 |
|------|----------|------|-----------|--------|
| **batch 放 value 维** | thread=(world,val) | 直观；容易表达 bitGEMM | 写回删值难做 ballot；线程数爆炸 | ★★☆☆☆ |
| **batch 放 word 维** | warp 固定 word | 可继续用 ballot；删值结构稳定 | 需要 word-major layout；调度要精细 | ★★★★☆ |
| **bit-matrix** | lane=yval，寄存器里是 32 worlds | 单指令同时处理 32 worlds | dom_size/DWO/trail 大重构 | ★★★★☆（中期） |

### 7.5 Phase 5（Batch-3A/3D）：约束聚合/扁平化队列的难点与启用条件

约束聚合（`<cid, world_mask>`）的收益不是必然：它要同时满足“聚合度足够高”和“GPU 并行度欠饱和”，
否则会被队列构建/调度/共享内存成本吞掉。

#### 7.5.1 主要难点
- **乱序调度下的语义**：需要保证 per-world 的 AC 不动点语义（soundness），并尽量保持可复现性。
- **队列/位图热点**：即便 world 状态隔离，frontier/active 标记/统计仍可能形成全局原子热点。
- **shared memory 压力**：缓存 bitSup 会压低 occupancy（Orin 的 SM 少，尤其敏感）。

#### 7.5.2 建议启用条件（数据驱动）
- `world_mask popcount` 的均值/分位数足够大（例如 mean≥2~3），否则 bitSup 复用收益低。
- 活跃 blocks 长期欠饱和（例如 < `2×SM`）或 probe 内部长尾占主导，才值得用聚合来“填满”GPU。
- `shared_mem_bytes` 不至于把 occupancy 压到极低（用 occupancy API/实测确认）。

#### 7.5.3 回退策略
- 不满足启用条件时默认回退 Stage 2（probe-level persistent blocks），优先保证稳定吞吐与简单性。

---

## 8. NSAC 实现：allowed-constraints mask

### 8.1 预计算（CPU 端）

```cpp
// 每个变量的 allowed 约束位图
uint32_t allowed_mask[num_vars][constraint_bitmap_words];

// 对每个约束 cid : (u,v)
// 若 u ∈ S_i && v ∈ S_i，则置位 allowed_mask[i][cid]
```

### 8.2 GPU 端传播

```cpp
// PropagateVarToNextBitmap 改成：
for each cid in d_subscription[var]:
    if (allowed_mask[focal_var][cid/32] & (1u << (cid%32))) {
        atomicOr(&frontier[cid/32], 1u << (cid%32));
    }
```

这让 frontier 的增长半径被结构性截断，而不是靠 budget 硬砍。

---

## 9. 代码修改大纲

### 9.1 新增结构体

```cpp
enum class SacMode { kNSACQ, kSACQAdj, kSACQFull };

struct SacBudget {
    int max_total_probes;
    int max_probes_per_var;
    int max_probe_iters;
    int max_probe_workitems;
    int max_queue_len;
};

struct ProbeStatus {
    int iter;
    int work_items;
    enum { OK, DWO, UNKNOWN } status;
};
```

### 9.2 新增入口

```cpp
GacStats GModelSolver::EnforceSAC_Worklist(SacMode mode, SacBudget B);
```

### 9.3 关键修改点

| 位置 | 修改 |
|------|------|
| `GModelSolver.cu` | 新增 worklist 外层循环 |
| `BatchProbeManager` | 添加 budget 字段和 UNKNOWN 语义 |
| `GModel.cu` | 添加 allowed_mask 过滤 |
| Persistent kernel | 添加 iter/work 计数和中断逻辑 |

### 9.4 NSACQ 的 top-level 逻辑

```cpp
// 对应 Wallace Fig.2
Q ← X;  // 初始化队列
OK ← AC(P);  // 先做一次 AC
while (Q 非空 && 预算未超) {
    Xi ← Select(Q);  // 可用 min-dom / max-degree
    for (a ∈ dom(Xi)) {
        status = SingletonTest(Xi, a, budget);
        if (status == DWO) {
            Remove(Xi, a);
            Q.insert(neighbours(Xi));  // NSACQ: 只邻居入队
        }
    }
}
```

---

## 10. 数据闭环：必跑的 6 项指标

1. `world_mask popcount` 的分布（mean/p50/p95/max）
2. 每 probe 的 `(iterations, deletions)` 分布（tail 强度）
3. 启动 blocks 数 vs SM 数（欠饱和频率）
4. `bitSupData` 访存：L2 hit、global load throughput
5. 预算触发频率：UNKNOWN 占比 vs 节点数减少
6. A/B/C 三种 kernel 在 microbench 与 e2e 上的收益差异

---

## 11. 实施优先级

| 优先级 | 任务 | 预期收益 |
|--------|------|----------|
| **P0** | NSACQ worklist + budget + UNKNOWN 语义 | 按住长尾，大实例可跑 |
| **P1** | allowed-constraints mask（真 NSAC） | 结构性截断传播半径 |
| **P2** | bitGEMM 内核（Option-1） | warp 利用率 6/32 → 32/32 |
| **P3** | Auto 策略（UNKNOWN 比例触发回退） | 自适应一致性级别 |

---

## 12. 理论对接：SAC-Opt

Bessière & Debruyne 的最优 SAC 算法核心：
- 每个值 `(Xi=a)` 对应一个子问题 `P|i=a`
- 把全局删值 delta **增量传播到所有仍活着的子问题**
- 某个子问题失败，就把对应值广播到所有子问题

这与当前架构的对应：
- **world 池长期驻留**（persistent blocks）
- **全局删值增量广播**（master → workspaces）
- **budget 控长尾**（UNKNOWN 不删）

> 这是"彻底 flatten SAC"的理论对应物：把 SAC 变成"许多 AC 子问题的并行/增量传播"。

---

## 参考文献

1. [Wallace - Light-Weight vs Heavy-Weight SAC/NSAC](https://cdn.aaai.org/ocs/10410/10410-46149-1-PB.pdf) - AAAI
2. [Wallace - SACQ/NSACQ Algorithms](https://cse.unl.edu/~choueiry/Documents/Wallace-nSAC-2015.pdf)
3. [Bessière & Debruyne - Optimal SAC Algorithms](https://scispace.com/pdf/optimal-and-suboptimal-singleton-arc-consistency-algorithms-vm2nggykar.pdf)
4. [Bessière & Debruyne - Theoretical Analysis of SAC](https://scispace.com/pdf/theoretical-analysis-of-singleton-arc-consistency-1h185oqzc7.pdf)
5. [IJCAI - On Neighborhood Singleton Consistencies](https://www.ijcai.org/proceedings/2017/0102.pdf)
