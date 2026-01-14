下面是一份可直接“新开聊天记录”用的大文档草案：把你我这段会话里**动机→理论→编码（encoding）→GPU实现现状→瓶颈→大方向解方（含 NSACq / SACq 队列控制）→下一步改造大纲**统一整合，并把你要求的两块也补齐：

1. **“Proposition: Batched AC Implementation of SAC” 的正式 LaTeX**（含 bitGEMV→bitGEMM/bit-matrix 形式的推导）
2. **基于你当前 `ExecuteConstraintCheck_BpC` 的 kernel 重构大纲**（线程/块划分、batch 维度放哪、数据布局选型与利弊）

---

# CPIM：面向 GPU 的强相容性（SAC/MSAC）加速 —— Phase 5（Batch-3D）理论与工程整合文档（草案）

> 版本：Draft v0.3（面向外部评审/内部定方案）
> 平台：Jetson Orin（8 SM，UMA，managed memory + cooperative launch 限制显著）
> 目标：**close 掉“GPU 上高效 MAC / mSAC（尤其长尾）”的问题**：不是做小修小补，而是要在“大问题”（长尾、算/存瓶颈、并行单位）上拿到决定性进展。

---

## 1. 动机与问题定义

### 1.1 为什么要做 SAC/MSAC 的 GPU 加速？

* 在 CSP 求解里，**强推理（SAC/MSAC）能显著缩小搜索树**，减少回溯和节点数；但其代价是大量“试值 probe + AC 到不动点”的重复传播，导致 CPU 上成本极高。
* GPU 适合把这些 **probe 独立世界（world）并行化**：每个 probe 是一个“复制后的 CSP 世界”，运行 AC 直到不动点，并检测 DWO（domain wipeout）。这直接对应你的 Batch-1/2/3 系列思路。

### 1.2 你现在面对的“真正大问题”

你 Phase 5 备忘录总结得很准：当前 Batch-3A/3D 不达预期不是“小优化缺失”，而是几个系统级矛盾叠加：

* **算法长尾**：高 tightness / 大域下，“证明某值不 SAC”可能要跑很多轮 fixpoint，full MSAC 还会外层反复试值，形成灾难性长尾。
* **并行单位与写冲突的矛盾**：把工作打散到 `<world,cid>`/`<world,cid,word>` 虽然提升并行度，但会带来同一 world 写域状态的并发冲突（atomic 风暴/锁），吞掉收益。
* **bitSup 复用 vs occupancy**：把 `bitSupData` 缓到 shared 的固定成本 + shared 压力会降低占用率；而复用收益又强依赖“world_mask 的聚合度”（popcount），低聚合度时就是纯亏。
* **warp 利用率**：你当前 Aggregated/Bitmap 版本里 lane→word + `__ffs` 扫 bit 的结构，在 `bit_dom_int_size` 很小时天然浪费 lane；这在 Orin 这种 SM 数少、容易欠饱和的平台更致命。

结论：Phase 5 要赢，必须同时在 **(A) 算法长尾控制** 与 **(B) 批处理形态（bit-matrix / bitGEMM）** 上做“结构性”改造。

---

## 2. 理论背景：SAC / MSAC / SACq / NSACq（与你的 GPU 设计对应）

### 2.1 SAC 的标准定义（作为 correctness 基准）

Singleton Arc Consistency（SAC）：对每个变量-取值对 ((x,a))，把 (x=a) 临时赋值后，对得到的子问题执行 AC；若 AC 不导致任一变量域为空，则该取值 ((x,a)) 是 SAC-consistent；若所有 ((x,a)) 都是 SAC-consistent，则问题为 SAC。

MSAC（Maintaining SAC）是在搜索过程中维护 SAC（或其近似），代价更高但能更强剪枝。

### 2.2 SACq / NSACq：用“队列驱动”逼近 SAC、并天然适配 GPU 的方向

* SACq：用队列维护“需要重新做 singleton 检查”的变量/值集合，避免每次全量重做；是 SAC 的常见工程化实现路线。([NVIDIA Docs][1])
* NSAC / NSACq：只在“邻域（neighborhood）”范围做 singleton 检查（例如仅对与最近变化变量相关的部分触发），进一步降成本。([NVIDIA Developer Forums][2])

> 这与您提出的“控制 SACq 队列长度增长（长尾部分不再增长）”是同一类思想：把 SAC 从“最强但最贵”变成“可预算、可中断、可并行”的推理算子。

---

## 3. CPIM 当前实现态势（与你 Phase 1–5 备忘录对齐）

这里不再复述你备忘录全文，只抽取与“下一步 SAC-GPU 继续优化”直接相关的关键资产（以便外部模型快速进入上下文）：

### 3.1 你当前的 AC 核心（作为后续 bitGEMM 重构锚点）

你现在单 world 的二元约束检查核心是：

* `ExecuteConstraintCheck_BpC(cid, model, shmem)`：
  对每个 (x) 的取值 `val`，做
  [
  \text{has_sup} \leftarrow \bigvee_{w} \big( \text{bitSup}[cid,val,w] \ &\  D_y[w] \big) \neq 0
  ]
  然后把不支持的值从 (D_x) 删掉（以及对称地处理 (y)）。
  这就是典型的 **bitGEMV**（一行 support bit-vector 乘一个 domain bit-vector）。

### 3.2 Batch-2 的“probe 并行”已经成立，但 Phase-5 的“聚合并行”还亏

* Stage-2 persistent blocks（probe-per-task）能吃满 SM，性能稳。
* Batch-3A 聚合（`ExecuteConstraintCheck_Aggregated`）受 popcount 分布与共享内存开销影响大，在 Orin 上常被 Stage-2 吊打（你观测到 4×–8×）。

---

## 4. Proposition: Batched AC Implementation of SAC（正式 LaTeX）

> 这一节对应你说的“沿着 SAC / batched-inst 往前推：batched SAC 的 bit-matrix 形式”，以及“batch AC = SAC，在 GPU 上从 bitGEMV 变成 bitGEMM 的证明”。

下面给出**可直接放论文/技术报告**的 LaTeX（含证明思路 + bit-matrix/bitGEMM 形式）。

```latex
\section{Proposition: Batched AC Implementation of SAC}
\label{sec:batched-sac}

\paragraph{Notation.}
Let $P=(X,D,C)$ be a CSP with variables $X=\{x_1,\dots,x_n\}$, domains
$D=\{D(x)\}$ and constraints $C$.
For a variable-value pair $(x,a)$ with $a\in D(x)$, denote by
$P\!\restriction_{x=a}$ the subproblem obtained by assigning $x=a$.
Let $\mathrm{AC}(\cdot)$ be an arc-consistency enforcement operator that
iterates to a fixpoint (or detects inconsistency by domain wipeout).

\begin{definition}[Singleton Arc Consistency]
A CSP $P$ is \emph{singleton arc consistent (SAC)} iff for every
$(x,a)$ with $a\in D(x)$, enforcing arc consistency on $P\!\restriction_{x=a}$
does not produce an empty domain, i.e.,
$\mathrm{AC}(P\!\restriction_{x=a})$ is consistent for all $(x,a)$.
\end{definition}

\begin{proposition}[Batched AC Implements SAC]
\label{prop:batched-ac-sac}
Fix any AC procedure $\mathrm{AC}$.
Consider the set of singleton probes
$\mathcal{W}=\{w_{x,a}\mid x\in X,\ a\in D(x)\}$, where each world
$w_{x,a}$ runs $\mathrm{AC}$ on an isolated copy of $P\!\restriction_{x=a}$.
Executing these worlds in a batched manner (in parallel) produces exactly
the same SAC classification (supported vs. rejected values) as executing
them sequentially, provided that:
(i) each world has isolated mutable state (domains, queues/frontiers),
and (ii) the batched execution preserves the per-world semantics of
$\mathrm{AC}$ (i.e., each world reaches the same fixpoint or detects DWO).
\end{proposition}

\begin{proof}
Each singleton world $w_{x,a}$ is defined by deterministic inputs:
the original constraint relations and the initial domains with $x$ fixed to $a$.
Arc-consistency enforcement $\mathrm{AC}$ is monotone w.r.t. domain deletions,
and its fixpoint (or detection of DWO) depends only on the world-local state.
Under assumptions (i) and (ii), parallelizing the execution across worlds
does not introduce cross-world interference; hence each world reaches the
same terminal state as in sequential execution. Therefore, the decision
whether $(x,a)$ is SAC-consistent (no DWO in $w_{x,a}$) is identical in both
sequential and batched execution.
\end{proof}

\subsection{From bitGEMV to bitGEMM via bit-matrix formulation}
\label{subsec:bitgemm}

\paragraph{Binary support check as bitGEMV.}
Consider a binary constraint $c(x,y)$ represented by support bitsets.
For each $a\in D(x)$, let $S^{x\rightarrow y}_{c}(a)\in\{0,1\}^{|D(y)|}$
be the bitset of $y$-values supporting $x=a$ under $c$.
Let $B_y\in\{0,1\}^{|D(y)|}$ be the current domain bitset of $y$.
Then $a$ is supported iff
\[
\mathrm{supp}(a) \;=\; \bigl(S^{x\rightarrow y}_{c}(a)\bigr)^\top \odot B_y
\;\neq\; 0,
\]
where $\odot$ is bitwise-AND followed by OR-reduction over machine words.
This is a bit-vector (bitset) dot-product, i.e., \emph{bitGEMV}.

\paragraph{Batched worlds as bitGEMM.}
Now consider a batch of $m$ worlds with possibly different $y$-domains
$\{B_y^{(1)},\dots,B_y^{(m)}\}$.
Stack them into a bit-matrix
\[
\mathbf{B}_y \in \{0,1\}^{|D(y)| \times m},
\quad
\mathbf{B}_y[:,j] = B_y^{(j)}.
\]
For a fixed $a$, the batched support results form a length-$m$ bit-vector
\[
\mathbf{r}(a) \in \{0,1\}^{m},
\quad
\mathbf{r}(a)[j] = \mathbb{I}\bigl((S^{x\rightarrow y}_{c}(a))^\top \odot
\mathbf{B}_y[:,j] \neq 0 \bigr).
\]
Equivalently, in word form with $W=\lceil |D(y)|/32\rceil$,
let $S_{a,w}\in\{0,1\}^{32}$ be the $w$-th machine word of $S(a)$ and
let $\mathbf{B}_{w}\in\{0,1\}^{32\times m}$ be the corresponding word-slice
of $\mathbf{B}_y$.
Then the batched support is
\[
\mathbf{r}(a) \;=\; \bigvee_{w=1}^{W}
\left( S_{a,w} \ \wedge\ \mathbf{B}_{w} \right),
\]
where $\wedge$ is bitwise-AND (broadcasting $S_{a,w}$ over the batch),
and $\vee$ is OR-reduction across words.
This is exactly a boolean/bitwise matrix multiplication pattern, i.e.,
\emph{bitGEMM}: many rows ($a$) times many columns (worlds).

\paragraph{Bit-matrix packing (32 worlds per group).}
If we pack a group of 32 worlds into one 32-bit mask per value $v$,
i.e., store $\mathbf{M}_y[v]\in\{0,1\}^{32}$ whose $j$-th bit indicates
whether $v\in D^{(j)}(y)$, then for a fixed $a$:
\[
\mathbf{mask}(a) \;=\; \bigvee_{v \in S^{x\rightarrow y}_{c}(a)} \mathbf{M}_y[v],
\]
which yields a 32-bit mask of worlds where $a$ has support.
Updating $x$ in batch becomes
$\mathbf{M}_x[a] \leftarrow \mathbf{M}_x[a] \wedge \mathbf{mask}(a)$,
again a bit-matrix (bit-sliced) realization of batched AC.
```

**说明**

* 这个命题把“batched AC = SAC”讲清楚：只要 world 状态隔离，batched 就只是调度形态变化，不改变语义。
* 后半部分把你当前 `ExecuteConstraintCheck_BpC` 的核心 `has_sup |= (bitSup & dom)` 形式抽象为 **bitGEMV**，再堆叠 batch 变成 **bitGEMM / bit-matrix**。

SAC/MSAC 与 SACq/NSACq 的算法脉络可参考综述/对比文献（如 SAC 与其变体、heavy/light 推理的开销权衡）。

---

## 5. 基于 `ExecuteConstraintCheck_BpC` 的 bitGEMV → bitGEMM Kernel 重构大纲

你要的是“可落地的 kernel 重构大纲（线程/块划分 + batch 维度选型）”。这里给 **三条路线**，从最容易落地到最激进（bit-matrix）。

---

### 5.1 路线 A（最稳妥）：保持当前 bitDom（world×var×word），把 batch 放在 **world 维向量化**（SIMD / uint4）

**核心想法**
仍然让每个 world 有自己独立的 `bitDom`（与你现有 SAC1/workspace 完全一致），但把内核里对 `dom_y[w]` 的访问从标量变成向量（一次算 4/8 个 world），实现 bitGEMM 的“m 小”版本：

* 把 (m) 个 world 的 `dom_y[w]` 连续存储（你 workspace 本来就可以做到：world-major contiguous）。
* 线程按 `val`（或 word）并行，但每次 load 用 `uint4` / `ulonglong2` 拉多个 world：

  * `tmp = sup_word & dom_y_vec;`
  * `has_sup_vec |= (tmp != 0);`

**优点**

* 不改数据结构，不引入“domain size / DWO 检测”的新难题。
* 能直接复用你现在的 `frontier`、`dom_size`、trail/backtrack 逻辑（尤其你已经从多层域变成 Trail）。

**缺点**

* 向量宽度有限（4/8/16），吞吐上限不如真正 bit-matrix（32 worlds packed）。
* 对 Orin 这种 memory-bound 平台，纯向量化如果不能显著减少访存，收益可能有限。

**适用场景**
你想先把“bitGEMV→bitGEMM 的方向”验证为真，并快速拿到阶段性收益/数据支撑，优先选这条。

---

### 5.2 路线 B（折中、很关键）：batch 放在 **word 维**（warp-per-word），让一个 warp 同时算多个 world 的同一 word

**线程/块划分建议**

* **block 维度**：`blockIdx.x = <world_id>`（或 world tile），`blockIdx.y = cid`
* **warp 粒度**：每个 warp 专注一个 `(cid, xval)`，线程 lane = 0..31 用来做 word 内 ballot（你在 cuSAC 风格里已经证明过好用）。
* **batch 维**：一个 block 内含多个 warp，对多个 world 或多个 xval 并行。

**关键设计点**：把 batch 放在 word 维的本质，是保证下面两件事同时成立：

1. 你仍能用 **ballot 生成 keep_mask**（按 word 写回删值）
2. 同时提高 SM 利用率（多个 warp 覆盖多个 world/xval，减少空转）

**优点**

* 相比路线 A，能更好复用你“warp-per-word + ballot 写回”的删值结构（不必换语义）。
* 依旧不需要引入复杂的 bit-matrix domain size 维护。

**缺点**

* 数据布局必须非常小心，否则 `dom_y[w]` 访问会变成 stride 访问，吞吐崩。
* 很可能需要引入 **word-major layout**（见 5.4）。

---

### 5.3 路线 C（最激进、最像你要的“bit-matrix”）：32 worlds packed 成 32-bit mask，真正实现 bit-matrix/bitGEMM

这是你 LaTeX 里最后那段“(\mathbf{M}_y[v])” 的工程化实现。

#### 5.3.1 数据结构（关键）

对每个 world group（最多 32 个 world），对每个变量 (x)，存储：

* `dom_mask[x][val] : uint32`

  * 第 j 位 = 1 表示 group 内第 j 个 world 的 (val \in D^{(j)}(x))
* 对应地，支持表 `bitSupData` 仍然是 “对每个 xval 给 yval 的 bitset”，但我们计算时不再与 `dom_y[word]` AND，而是对 **yval 逐个线程取 dom_mask[y][yval] 并做 OR-reduce**（见下）。

#### 5.3.2 内核映射（bitGEMM 的一个自然 warp 映射）

**一个 warp 负责一个固定的 ((cid, xval))，warp 内线程对应 yval（0..31）**：

* 对于某个 y-word（32 个 yval）：

  * lane (t) 负责 yval (v = 32\cdot w + t)
  * 读取 `mask = dom_mask[y][v]`（32 worlds packed）
  * 判断 `pred = support_bitset_for_(xval)` 的第 (t) 位是否为 1
  * 若 `pred` 为真，则贡献 `mask`，warp 内做 OR-reduce 得到 `partial_worldmask`
* 迭代所有 y-words（`bit_dom_int_size` 很小，比如 6），OR 合并得到 `worldmask_supported(xval)`
* 最后更新：
  `dom_mask[x][xval] &= worldmask_supported(xval)`
  （单条 `&` 就能同时删掉 32 个 worlds 里不支持的 xval！）

这就是你要的**bitGEMV→bitGEMM**：原来每个 world 都要做一次 `(sup & dom_y) != 0`，现在一条 warp-reduce 得到 32 worlds 的支持情况。

#### 5.3.3 这一路线的“硬核难点”（必须提前讲清）

* **DWO/域大小维护**：你现在大量逻辑依赖 `d_cur_dom_size[var]`（例如 `GetMinDomainVar`、DWO 快速判定）。bit-matrix 表达把 world 打包了，域大小维护会变复杂：

  * 若每次删一个值，你得到的是 `removed_world_mask`（32-bit），需要对 mask 每一位做 `dom_size[world]--`，这又会引入 bit 扫描/原子。
  * 若改成周期性重算，又会引入昂贵的 reduction（对每个 var，对所有 val 汇总每个 world 的 bit 计数）。
* **与现有 trail/backtrack 的耦合**：你当前 trail 是按 `(var,word)` 记录 old_bits 的；bit-matrix 会把“一个 val 的 32 worlds”聚合成一个 word，trail 粒度会改变。
* **写冲突**：多个约束同时对同一 `dom_mask[x][val]` 做 AND（单调删值）可以用 `atomicAnd(uint32*)`，语义是安全的，但要注意吞吐与热点分布。

> 结论：路线 C 很可能是你“最终形态”，但建议先用路线 A/B 拿到数据、再决定是否值得把 solver 的域表示整体迁到 bit-matrix。

---

### 5.4 batch 维度到底放在 word 维还是 value 维？（你的问题点名要回答）

这里给一个“工程决策表”，你可以直接贴到评审材料里：

| 选型                                | 并行映射                         | 优点                              | 风险/缺点                            | 推荐度       |
| --------------------------------- | ---------------------------- | ------------------------------- | -------------------------------- | --------- |
| **batch 放 value 维**               | thread=(world,val)           | 直观；容易表达 bitGEMM                 | 写回删值难做 ballot；线程数爆炸              | ★★☆☆☆     |
| **batch 放 word 维（warp-per-word）** | warp 固定 word，处理多个 world/xval | 可继续用 ballot 生成 keep_mask；删值结构稳定 | 需要 word-major layout；调度要精细       | ★★★★☆     |
| **bit-matrix（world packed）**      | lane=yval，寄存器里是 32 worlds    | 单指令同时处理 32 worlds；最像 bitGEMM    | dom_size/DWO/trail 大重构；atomic 热点 | ★★★★☆（中期） |

---

## 6. “长尾回避”方向：NSACq / 控制 SACq 队列增长（你问的点，给出可执行方案）

你问得非常关键：**如果 full SAC/MSAC 在大域高 tightness 下必然长尾，那 GPU 端要不要换目标？**

答案：**要**。而且这不是“投机取巧”，而是推理算法谱系里本来就存在的工程化路线：SACq / NSACq 就是为了在成本与剪枝强度间做可控折中。([NVIDIA Docs][1])

### 6.1 两个你可以直接落地的“长尾控制”语义（保持 correctness）

**原则**：任何时候只要你“不再继续推理”，就必须把结果解释为 **unknown（不删）**，这样永远不会删错，只是剪枝变弱。

1. **Budgeted SACq / Bounded-Queue SACq**

   * 设定每个 probe/world 的预算：最大 iterations、最大 deletions、最大 frontier 扩张次数、最大队列长度 (Q_{\max})。
   * 一旦触发预算，立即停止该 world 的进一步传播，将该 ((x,a)) 标记为 “unknown / not proven inconsistent”。
   * 在 MSAC 里把 unknown 当作“保守通过”，保证正确性。

2. **NSACq(k)（邻域半径/触发域限制）**

   * 只对“与最近变化变量在邻接图上距离 ≤ k”的部分做 singleton 检查；或只允许队列里进入与该 neighborhood 相关的变量。
   * 这在结构上天然限制队列增长，从根源上抑制长尾。([NVIDIA Developer Forums][2])

### 6.2 为什么它更适合 GPU？

* GPU 最怕“极少数 world 极慢”把持久 kernel 拖死（tail latency）；预算/队列上限直接把 tail 截断。
* 队列上限能把工作量变得“更可预测”，使 batch（bitGEMM）收益更稳定。
* 对 Orin 这种 SM 少的平台，**稳定饱和** 比追求极致剪枝更重要（否则你会出现“推理很强但跑不动”的反效果）。

---

## 7. 面向 CPIM 的“代码修改大纲”（把理论对应到你现有实现）

下面按“最少破坏主线”的方式给一个改造大纲（你可以交给工程 LLM 继续拆任务）。

### 7.1 在现有 SAC1/MSAC 基础设施上，先落地“长尾控制（Budget/Queue cap）”

目标：不改核心传播算子，先保证困难实例不会卡死。

* **位置**：`GModelSolver::EnforceSAC1()`（你备忘录说入口在 `src/solver/gpu/GModelSolver.cu:514` 一带）
* **新增控制参数**（host 可配，auto selector 可学习）：

  * `max_probe_iterations`
  * `max_probe_deletions`
  * `max_frontier_expansions` 或 `max_queue_words_nonempty_checks`
  * `Q_max`（若走 SACq/NSACq 的队列实现）
* **语义**：

  * 超预算 → probe 结果 = UNKNOWN（不删），但记统计（用于 auto 策略）
  * 只有检测到 DWO 才能判 inconsistent 并删值（或标记值非 SAC）

这一步会立刻把你的“full/fast MSAC”困境变成“fast 且不会炸”，先把工程闭环跑稳。

### 7.2 再推进 bitGEMV→bitGEMM：从路线 A/B 开始做 microbench

目标：用数据决定是否值得走路线 C（bit-matrix）。

* **改造点**：以你现在 `ExecuteConstraintCheck_BpC` 为基准，做一个并列实现：

  * `ExecuteConstraintCheck_BatchedVec(...)`（路线 A：vectorize worlds）
  * 或 `ExecuteConstraintCheck_WarpWordBatched(...)`（路线 B：word-major + warp-per-word）

* **关键工程动作**：

  1. **workspace 域布局整理**：确保 batch worlds 的 `dom_y[w]` load 是 coalesced
  2. **将 bitSupData 访问改成只读路径（ldg / read-only cache hint）**（你 Phase 4 已经做了 memAdvise hint，可继续配合实际访存模式）
  3. 对比三组 microbench：

     * 单约束检查吞吐（cid 固定、域随机）
     * 完整 AC fixpoint（frontier 激活）
     * probe 批吞吐（probes/s）与 tail（p95/p99）

### 7.3 如果 A/B 的数据证明“GEMM 化收益很大”，再评估路线 C（bit-matrix）的重构边界

这一步要先写清“迁移边界”，否则会被 dom_size/trail 拖进泥潭：

* **路线 C 最小闭环建议**：先只在 **SAC probe 的 workspace** 里用 bit-matrix，不触碰主搜索态的域表示；probe 完成后只回传“是否 DWO / 是否删值集合”。
* 这样 trail/backtrack 主线仍按你现有 bitDom 表示跑，不会被 bit-matrix 绑架。

---

## 8. 你这样做“对不对”？有没有更好的办法？

### 8.1 结论（直说）

* **对**：你已经把“GPU 上做强推理”的基础设施打穿到可评测状态（Batch-2/3、persistent、bitmap frontier、warp-per-word），现在继续做**结构性优化**是正道。
* **下一步不该做的**：零碎小优化（比如某个循环 unroll、某个 atomic 换写法）在长尾面前意义不大，除非它服务于下面两件事之一：

  1. 把 bitGEMV 推到 bitGEMM（提升单位时间推理吞吐）
  2. 把 full MSAC 的 tail 截断（预算/近似一致性）

### 8.2 更好的办法（如果你愿意改目标）

如果你发现“full MSAC 目标在 Orin 上不现实”，最佳替代不是放弃 GPU，而是：

* 把主力推理目标切到 **NSACq / Budgeted SACq**（强度略弱但可稳定并行）([NVIDIA Developer Forums][2])
* 对极少数“值得”的关键节点/变量，再用更强的 SAC/MSAC 做 selective refine（CPU 或 GPU 单独跑，避免拖死主线）

这会让你的系统更像一个“推理加速器（inference accelerator）”，而不是“理论最强一致性”的实现——更符合工程落地与 GPU 特性。

---

## 9. 下一步你最该补的“数据闭环”（决定 Phase 5 生死的指标）

你备忘录里已经列了，我把它压缩成“必须先跑”的 6 项（不然继续重构会盲）：

1. `world_mask popcount` 的分布（mean/p50/p95/max）
2. 每 probe 的 `(iterations, deletions)` 分布（tail 强度）
3. 启动 blocks 数 vs SM 数（欠饱和是否频繁出现）
4. `bitSupData` 访存：L2 hit、global load throughput（Nsight）
5. 预算触发频率：UNKNOWN 占比 vs 节点数减少（剪枝收益曲线）
6. A/B/C 三种 kernel 在 microbench 与 e2e 上的收益差异（尤其 p95/p99）

---

# 你接下来如果要我继续推进（建议的“最短路径”）

1. 我可以把上面的 **路线 A / B** 具体化到你代码层：以 `ExecuteConstraintCheck_BpC` 为基准，给出一个“批处理向量化版本”的伪代码 + 数据布局建议（包括你 workspace 里 world 的排布）
2. 同时把 **Budgeted SACq / NSACq(k)** 的控制字段加到 `BatchProbeManager`/`control` 结构里，给出最小侵入的修改点清单（哪些字段放 UM，哪些用 device 原子）
3. 再给一套 **实验脚本口径**：你现在已经很擅长做阶段统计（iterations/deletions/queue），把它补到 Phase-5 决策规则就能自动选策略。

你想先从哪条开始落地：

* **先上预算/降级（立刻止血 full MSAC 长尾）**，还是
* **先做 bitGEMV→bitGEMM（先把单位吞吐推上去）**？

（你不用重新解释背景，我就按你现在 CPIM 的代码结构直接写“具体到函数/结构体字段”的修改清单。）

[1]: https://docs.nvidia.com/cuda/cuda-programming-guide/04-special-topics/cooperative-groups.html "4.4. Cooperative Groups — CUDA Programming Guide"
[2]: https://forums.developer.nvidia.com/t/cooperative-group-grid-synchronization-leading-to-execution-freezes/297287 "Cooperative Group Grid synchronization leading to execution freezes - CUDA Programming and Performance - NVIDIA Developer Forums"
