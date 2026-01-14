# Batch-GPU SAC 优化：bitGEMM 化与 SAC-2/3 升级

> **状态**: 设计中
> **目标**: 解决 Phase 5 卡住的核心矛盾，实现"结果级别"的 SAC-GPU 加速

---

## 1. 当前面临的三个核心矛盾

### 矛盾 A：SAC/MSAC 的算法长尾不是"并行度"能直接抹平的

SAC 的本质是：对每个候选值做一次"赋值→AC 到不动点→判不一致"。当实例像 `tightness=0.9` 那种"**大量值 AC-consistent 但非 SAC-consistent**"时，证明"不 SAC"往往要跑很深的传播序列，天然长尾（full MSAC 更糟）。

这在经典 SAC 系列算法分析里是明确的：SAC-1 非常昂贵，后续 SAC-2/3 的核心就是**复用/缓存支持**来降低重复传播代价。

> **结论**: 如果不改变"每次 probe 都近似从零做大量 support 检查"的事实，Phase 5 再怎么调度也会被长尾吞掉。

### 矛盾 B：Batch-3A/3D 的主要瓶颈是 warp 利用率与内存形态

当前问题：`bit_dom_int_size=6` 时 lane→word 只活 6/32；再叠加 `__ffs` 扫 bit 的分歧，算力侧天然吃不满。这个问题不是小优化能救的，必须把并行映射换掉。

> **结论**: Batch 维度应该优先用来填满 warp（lane→world），而不是继续 lane→word。

### 矛盾 C：bitSup 复用需要配合 dom 矩阵化

现在每个 value 都要读对侧域 `dom_y[w]`。当把 probe 批起来，本质上拥有一个 `B × W` 的域矩阵；如果仍然按 bitGEMV 做，dom 侧仍然会被重复读 B 倍 × value 数倍。

> **结论**: 必须把单世界的 bitGEMV 变成"对 B 个世界同时算"的 bitGEMM，让 `dom_y` 以矩阵形态进 shared/L2，一次加载服务多个 value。

---

## 2. 两条主线优化路径

### 主线 1：Batched-AC 核心 check 变成 bitGEMM 形态（lane→world）

直接解决 Phase 5 的 warp 空转和 dom 侧重复读问题。

**核心思想**：

对固定约束 `c=(x,y)`、固定 `x=val`，单世界 support 检查是：
```
∃b∈D_y: S_c^{x→y}(val,b) = 1
```

用 bitset 就是一次 bitGEMV：`(sup_row[val] & dom_y) != 0`。

当有 B 个 worlds 时，`dom_y` 变成 `B×W` 的 bit-matrix，**同一条 sup_row 同时乘上 B 行**，这就是 bitGEMM/批量 bitGEMV。

### 主线 2：SAC 从 SAC-1 升级到带 residue/last-support 的 SAC-2/3 风格

经典 AC/SAC 系列优化的核心是 **residual support / last support**：

- 对每个 `(constraint, value)` 记住上次找到的支持
- 只要该支持仍在对侧域里，就不必再做全量扫描
- 只有支持失效才重新找

GPU 上落地的关键：
- 缓存结构必须 **world-private**（每个 probe/world 一份）
- 缓存尺寸必须可控（dom=180、约束~40 完全可控）
- 缓存访问必须配合 lane→world 的布局做到合并读写

---

## 3. 理论基础：Batched AC 实现 SAC

### Proposition (Batched AC Implementation of SAC)

```latex
\begin{proposition}[Batched AC Implementation of SAC]
\label{prop:batched-ac-implements-sac}
Let $P = (X, D, C)$ be a binary CSP.
For any variable $x \in X$ and value $a \in D(x)$, denote by
$P[x \leftarrow a]$ the derived CSP in which the domain of $x$ is restricted
to the singleton $\{a\}$ (all other domains unchanged).
Let $\mathrm{AC}(\cdot)$ denote the (least) arc-consistent closure operator
that applies arc-revision until a fixpoint (or detects inconsistency).

Consider a batch $\mathcal{B} = \{(x_i,a_i)\}_{i=1}^{B}$ of singleton assignments.
Define the \emph{batched state} as a product domain
$\mathbf{D} = (D^{(1)},\ldots,D^{(B)})$ where $D^{(i)}$ is the domain store for
instance $P[x_i \leftarrow a_i]$.
Let $\mathbf{F}$ be the batched AC operator that, for every constraint
$c \in C$, applies the standard arc-revision independently to each component
$D^{(i)}$ (i.e., pointwise on the product lattice), iterating until a joint fixpoint.

Then, for every $i \in \{1,\ldots,B\}$, the $i$-th component of the batched fixpoint
equals the standalone AC closure:
\[
\mathbf{F}^{\star}(\mathbf{D})[i] \;=\; \mathrm{AC}\!\left(P[x_i \leftarrow a_i]\right),
\]
and therefore the singleton assignment $(x_i,a_i)$ is \emph{singleton arc-consistent}
iff the batched component is not wiped out (no domain becomes empty) at the fixpoint.
\end{proposition}
```

### 证明

Arc-revision 对二元约束 `c=(u,v)` 是有限域格上的单调算子：只移除值，不添加值。全局 AC 算子 `AC(·)` 是这些单调 revision 组合的最小不动点。

定义乘积格 `L = L_1 × ... × L_B`，其中 `L_i` 是实例 `P[x_i ← a_i]` 的域格。批处理算子 `F` 对每个分量独立应用相同的 arc-revision 规则（逐点），因此 `F` 在 `L` 上单调，且分解为 `F(D) = (F(D^(1)), ..., F(D^(B)))`。

由 Tarski 不动点定理，`F` 的最小不动点是各分量最小不动点的元组：
```
F*(D) = (F*(D^(1)), ..., F*(D^(B)))
      = (AC(P[x_1 ← a_1]), ..., AC(P[x_B ← a_B]))
```

因此每个批处理分量等于独立 AC 闭包，分量 `i` 检测到不一致当且仅当 `D^(i)` 中某个域变空。这正是 `(x_i, a_i)` 的 SAC 测试。

> **核心意义**: AC 传播是"按实例逐点"的单调闭包算子，batch 只是 SIMD，不改变语义。把它落到 bit-matrix（bitGEMM）是实现层面的等价变换。

---

## 4. bitGEMV → bitGEMM 重构方案

### 4.1 当前实现抽象

`ExecuteConstraintCheck_BpC`（单 world）可以抽象成：

```cpp
for each val:
    // 取一行支持 bitset
    S[val, w] (w 是 word)
    // bitGEMV
    has_sup = OR_w ((S[val,w] & dom_other[w]) != 0)
    // 若无支持则删该 val
```

### 4.2 批处理版本

当有 B 个 worlds 时，把对侧域堆成矩阵 `DomOther[B][W]`：

```cpp
for each val:
    for each world b:
        has_sup[b] = OR_w ((S[val,w] & DomOther[b][w]) != 0)
```

这就是 `S[val,:]` 同时乘上 B 行 —— **批量 bitGEMV / bitGEMM**。

### 4.3 实现选型

#### Option-1（强烈推荐）：Warp = 1 个 value，lane = world

**适用场景**：
- `W = bit_dom_int_size` 很小（如 6），导致 lane→word 极度浪费
- B 典型取 16/32，刚好填满一个 warp

**关键数据布局（SoA）**：

```cpp
// 现在单 world
bitDom[var][word]

// batched 要变成（world 连续）
DomWord[var][word][world]

// 内存地址
dom_ptr = dom_soa + ((var * W + word) * B + world)
```

这样一个 warp 的 32 lanes 读同一个 `(var,word)` 的 32 个 world 会是完美合并读。

**Kernel 结构**：

```
grid:  blockIdx.x = cid（或 cid-chunk）
block: 128/256 threads（4/8 warps）

warp 分工:
  - warp 0..k-1 覆盖 val 维：每 warp 负责一段 val
  - lane = world：每条 lane 对应一个 world（probe）
```

**共享内存**：

```cpp
// 对固定 cid，把对侧域矩阵一次性搬进 shared
__shared__ uint32_t sh_dom_x[W][B];
__shared__ uint32_t sh_dom_y[W][B];

// 大小：2 * W * B * 4 bytes
// 例如 W=6, B=32 ⇒ 2*6*32*4 = 1536 bytes，非常小
```

**单 warp 核心循环**：

```cpp
// 对固定 val，每 lane(world)
uint32_t acc = 0;
for (int w = 0; w < W; ++w) {
    acc |= (S[val, w] & sh_dom_other[w][lane]);
}
bool has_sup = (acc != 0);

// 删值：warp-cooperative 用 ballot 形成 32-world mask
```

> 这直接把"活跃 lane 只有 6/32"翻成"活跃 lane 32/32"。**质变**。

#### Option-2：Warp = 1 个 word，lane = world

适用于 `max_dom_size` 很大时，需要更均匀的工作量。但当前 max_dom_size=180 不极端，不是首选。

#### Option-3：CTA = 1 个 (cid, world-tile)，lane = word

适用于 B 特别大（>32）。但会回到 lane→word 的浪费问题，对当前参数不友好。

---

## 5. 实现步骤

### Step 0：替换约束检查核（先验证收益）

新增函数：

```cpp
__device__ void ExecuteConstraintCheck_Batched(
    int cid,
    GModel* model,
    SharedMem* shmem,
    int world_base,
    int B_tile
) {
    // 按 Option-1 把 (x,y) 两侧域的 W×B_tile 搬入 shared
    // 对 val=0..max_dom_size-1 做 batched 支持检查
    // 输出：每个 world 的 x_changed/y_changed、删值计数、inconsistent_flag
}
```

**先把单约束检查变成真正的 bitGEMM，能单独 microbench 出吞吐（values/s × worlds）**。

### Step 1：升级 frontier 粒度

当前 `BitmapGAC` 用 `frontier_A/frontier_B`（全局一份）。

批处理版本（推荐）：
- frontier 仍按 world 私有
- 存成 SoA：`frontier_word[cid_word][world]` 便于 lane→world 合并读写

### Step 2：SAC driver 换 backend

SAC1/MSAC 语义不变，只是"probe 的 AC 实现"换成批处理版本：

```cpp
// probe/world 初始化
AssignValue(var, val)  // 变成对某个 world 的域做赋值（SoA 里一列）

// 调用 batched-AC kernel
EnforceGAC_Batched(world_base, B_tile)

// 对 inconsistent 的 worlds
// 在父问题里删掉对应值
```

---

## 6. 验证闭环（避免盲目大重构）

建议做一个硬核但短的验证：

1. 选一个卡死的族（如 `rand-2-40-180, t=0.9`），固定若干 `cid`
2. 写 microbench：只跑 `ExecuteConstraintCheck_*`，不跑全 GAC
3. 对比三版：

| 版本 | 实现 |
|------|------|
| A | 当前单 world `ExecuteConstraintCheck_BpC` |
| B | batched 但仍按 bitGEMV（worlds 循环展开） |
| C | Option-1 的 batched bitGEMM（lane→world + shared dom matrix） |

**只要 C 对 B 有明显倍数提升，Phase 5 就有"物理基础"；否则继续做 3D 调度大概率是徒劳**。

---

## 7. SAC-2/3 residue/last-support 结构（后续）

如果确定要冲 full MSAC，需要考虑：

### 数据结构

```cpp
// 每个 (constraint, value, direction) 记住 last support
// world-private，存成 SoA
last_support[cid][val][world]  // 存对侧域中的具体 bit 位置
```

### 访问模式

1. 检查 last support 是否仍有效：`(dom_other[world] >> last_sup) & 1`
2. 若有效：跳过全量扫描
3. 若失效：做全量扫描，更新 last_support

### 收益

- 避免每轮全量扫 `W` 个 word
- 对 tightness 高的实例有数量级改善

---

## 8. 关键文件

| 文件 | 说明 |
|------|------|
| src/solver/gpu/GModel.cu | GPU 模型核心 |
| include/GModel.cuh | GModel 头文件 |
| apps/sac_benchmark.cpp | SAC 基准测试 |
| docs/archive/batch_ac_versions/ | 历史设计文档 |

---

## 参考文献

1. [A New Algorithm for Singleton Arc Consistency](https://cdn.aaai.org/FLAIRS/2004/Flairs04-047.pdf) - FLAIRS 2004
2. [A Study of Residual Supports in Arc Consistency](https://www.ijcai.org/Proceedings/07/Papers/018.pdf) - IJCAI 2007
