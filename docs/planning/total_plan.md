下面是一份**面向代码落地**的工程实现计划（按“能直接开分支干活”的粒度写），基于你目前 CPIM 的现状（ACGPU 已跑通、SACGPU 还在演进、Phase5/Batch‑3D 争议点明确）以及你刚上传的设计文档里给出的主线结论（**(N)SACQ + budget + batched‑inst 为主线，Phase5/Batch‑3D 作为可选加速器**） 。

同时我把你刚发现的 **Tensor Core 单比特 BMMA/WMMA（AND/XOR + POPC）** 纳入“可插拔后端”计划里：CUDA Guide 和 PTX ISA 都把 b1 的 bit-matrix MMA 写得很明确（`bmma_sync` / `wmma.mma.*.popc`），并且 AND+POPC 在 **compute capability 8.0+** 可用。
Jetson Orin 是 Ampere + Tensor Cores 的平台，这条线是值得做的（至少做 microkernel 验证上限）。([NVIDIA][1])

---

## 0. 目标定义与边界

### 0.1 你们要解决的“工程主问题”

* **把 SAC/MSAC 这类强剪枝的“重复 probe + 内层 AC 不动点”**，变成 GPU 上稳定吞吐的批处理任务；
* **用更强剪枝减少搜索树规模**，让整体求解更偏“推理密集”而不是“搜索密集”（符合 GPU 擅长的计算模式）；
* **长尾可控**：任何一个 probe/world 不能把整个批次拖死（尤其 Orin/UMA + cooperative persistent kernel 的场景）。

### 0.2 correctness 红线（必须写进工程规范）

* Budget 触发时，只能返回 **UNKNOWN / 不删**（sound but incomplete），绝不允许“软错误多删”。
* 任何新后端（bitGEMM/bmma）上线必须跑 **CPU reference 对拍**（随机实例 + 回归集合）。

---

## 1. 总体架构：把 ACGPU/SACGPU/bitGEMM 解耦成三层

建议你把代码结构（无论现在目录怎么摆）按逻辑拆成 3 层接口，后续加速器/策略才能插得进去：

### Layer A：Problem / Encoding 层（bitDom/bitSup 以及邻域信息）

职责：负责数据布局、预计算、转置/packing、邻域 mask 等。

* `bitDom`：当前单 world 域表示（你已有）。
* `bitSup`：二元约束支持表示（你已有）。
* `NeighbourhoodMask`（新增）：对每个 focal 变量 i，给出 NSAC 的 allowed constraints/allowed vars 过滤信息。

### Layer B：Propagation Engine 层（AC 引擎 + budget + backend）

职责：给定 “一个 world 的 CSP 状态”和“frontier/queue”，执行 GAC 到 fixpoint 或 budget 终止。

* `PropagateAC(world, scope_filter, budget) -> {OK, DWO, UNKNOWN}`
* `backend` 可选：SIMT bitset（现有 CUDA core）、bitGEMM‑SIMT、bitGEMM‑BMMA（Tensor Core）。

> CUDA 官方把 b1 的 `bmma_sync` 定义为：对 128-bit 行/列做 AND/XOR 后 POPC 累加，AND 需要 CC 8.0+。

### Layer C：Singleton/Worklist 层（(N)SACQ / SACQ‑adj / budget 管控）

职责：顶层调度“哪些变量/值要 probe”，以及 probe 结果如何回写主问题。

* `NSACQ` 默认路径（只重入队邻域变量、probe 只跑邻域子图 AC）。
* `SACQ-adj`/`SACQ` 可作为更强但更贵的可选项（受 budget 约束）。

---

## 2. Phase 划分的工程实现计划（从最稳到最激进）

下面每一阶段都写 **改哪些模块/要新增什么数据结构/验收标准**。你可以按阶段开分支，任何阶段都能单独合并，不会把工程拖进“大重构黑洞”。

---

## Phase 1：做“可观测性”与回归基线（先让后续优化能被量化）

### 1.1 增加统一的统计与 tracing

新增 `Stats` 结构（host + device）：

* AC 传播：`iters`, `processed_constraints`, `deleted_values`, `frontier_pushes`
* SAC：`num_probes`, `num_unknown`, `num_dwo`, `avg/p95/p99 probe iters`
* GPU：kernel 时间、SM 利用率（用 nvtx 或 cuda events）
* *关键*：记录 `world_mask popcount` 分布（你们 Phase5 里已经强调它决定聚合收益）

验收标准：

* 能一键输出 CSV（每个实例一行 + 关键分位数）。
* 能在不开 profiler 的情况下定位“长尾来自哪里”。

### 1.2 建立 CPU reference 对拍框架

* 先不求快，求“可对拍”：同一实例同一分支策略，CPU AC/SAC 与 GPU AC/SAC 的删值集合一致。
* 随机 CSP 生成器：二元随机网络（n、d、tightness 可控），用于 fuzz。

验收标准：

* PR 级 CI：小规模随机实例 100~1000 个对拍通过（AC + SAC 子集）。

---

## Phase 2：把 ACGPU 变成“可 budget 的 AC 引擎”（SAC 能否控长尾的地基）

你们文档主线说得很硬：**没有 budget 的 SAC/MSAC 迟早被长尾拖死；budget 的语义必须是 UNKNOWN 不删**。

### 2.1 在 world workspace 里加入 budget 计数器

每个 world 增加：

* `iter_cnt`
* `work_cnt`（处理了多少 cid / word chunk）
* `status`（OK / DWO / UNKNOWN）

### 2.2 修改 persistent GAC 主循环：超预算直接标 UNKNOWN 并提前退出该 world

* 不影响其它 world（同一 kernel 内部要保证每个 world 可独立停机）。
* 预算项建议至少两类：

  * `max_iters`（fixpoint 轮数上限）
  * `max_work`（总处理约束/总检查次数上限）

验收标准：

* 能构造“极难实例”让 UNKNOWN 出现，但不会崩溃/死锁；
* UNKNOWN 的 world **不产生任何删值回写**（soundness）。

---

## Phase 3：实现主线算法 (N)SACQ（队列驱动 + 邻域限制），用现有 batched-inst 跑起来

这是你附件里“拍板”的主线：Phase5/Batch‑3D 放可选加速器，先把 SAC 变成 GPU‑friendly 的 **可预算、可降级、队列驱动一致性家族**。

### 3.1 新增 NSACQ worklist（host 侧）

核心接口建议长这样：

```cpp
struct SacBudget { ... };        // probe budget + queue budget
enum SacMode { NSACQ, SACQ_ADJ, SACQ_FULL };

SacResult EnforceSAC_Worklist(SacMode mode,
                              SacBudget B,
                              Backend backend);
```

worklist 元素建议先做 “变量 i”，而不是 “(i,a)”：

* 出队 `i` → 生成 `dom(i)` 的所有 probe worlds
* 收集结果：DWO → 删 (i,a)，UNKNOWN → 不删但计数
* 若发生删值 → 只重入队 `neighbours(i)`（NSACQ / SACQ-adj）

### 3.2 预计算 neighbour 过滤信息（encoding 层新增）

对每个变量 i：

* `NbVars[i]`：邻居变量列表
* `AllowedConstraintsMask[i]`：邻域诱导子图上的约束集合（bitset）

GPU 端在入队/扩 frontier 时过滤（只允许邻域约束进入传播队列）。

### 3.3 budget 双阀门落地（内层 probe + 外层队列）

按你文档建议的“两道阀门”：

* **内层**：每个 probe world 的 budget（Phase2 已实现）
* **外层**：NSACQ 的队列 budget：

  * `max_total_probes`
  * `max_queue_pops`
  * `max_reenqueue`
  * `max_queue_len`（硬上限）

超过外层预算：直接停止 NSACQ，回到搜索（或仅 AC）继续——这依然 sound，只是剪枝少一些。

验收标准：

* NSACQ 能在大实例上稳定完成（即使产生 UNKNOWN 也能返回）；
* 相比 SAC‑1（全量扫描 repeat），probe 数量显著下降；
* UNKNOWN 比例可控并可统计。

---

## Phase 4：把 batched-inst “真正工程化”：跨变量 probe 池 + 负载均衡

你们 Stage2 persistent blocks “probe-per-task”已经稳，但还会出现 “某变量域很小 → 任务数塌陷 → GPU 欠饱和”。

### 4.1 改造 probe 生成：一次从 worklist 弹出多个变量，混合成全局 probe 池

* 设定一个 `target_probe_batch_size`（例如 ≥ 几千个 world）
* 当单变量域不足，继续从队列拿下一个变量补满 batch
* 让 persistent blocks 持续有活干

### 4.2 Probe 分桶调度（减少长尾拖累）

对 probe world 进行轻量分桶：

* 按 `dom_size`、`estimated_iterations`（可用历史统计）分桶
* 先跑“易收敛桶”，再跑“难桶”
* 或者对难桶使用更小预算（更快变 UNKNOWN，避免拖慢）

验收标准：

* GPU 利用率更稳定（任务不塌陷）
* p99 probe 时间下降

---

## Phase 5：bitGEMV → bitGEMM（三路线渐进实现，先拿到收益曲线）

你附件里已经给了很清楚的路线 A/B/C（从最保守到最激进）。
工程上我建议严格按 “A → B → C” 走，并要求每步都产出 microbench 数据。

### 5.1 路线 A：向量化 worlds（不改 bitDom 布局，最容易集成）

* 保持每个 world 独立 bitDom
* 在 kernel 内一次处理 4/8/16 个 world（寄存器向量化或 warp 内分配）
* 利用 ballot / warp reduce 把多个 world 的支持结果合并写回

验收：吞吐提升 & 不引入写冲突 bug。

### 5.2 路线 B：warp-per-word / warp-per-support-row（提高指令密度）

* 一个 warp 负责同一个 `(cid, x, a)` 的支持检查，但同时对多个 worlds/多个 word chunk 做归约
* 重点在 memory coalescing、共享加载 `bitSup` 的复用收益

验收：在大 domain 上收益更明显。

### 5.3 路线 C：bit-matrix（32 worlds packed，最像你想要的 bitGEMM）

* 把 32 worlds 的 `dom` 按 value 维打包成 32-bit mask
* 约束检查时对 support 相关的 value mask 做 OR-reduce，就得到 32 worlds 的支持情况

风险：trail/backtrack、dom_size、DWO 检测都要大改；因此建议先把它当 “中期重构”，不要堵住主线。

---

## Phase 6：引入 Tensor Core 单比特 BMMA/WMMA 作为可选 backend（你现在用 CUDA core 的升级方向）

这部分是你新发现的点：**不是抽象像 GEMM，而是 PTX/WMMA 真的有 b1 的 AND/XOR+POPC**。

### 6.1 先做 microkernel：验证 bmma 的“上限收益”

目标：不改求解器逻辑，仅测算子：

* 输入：A（support rows，b1），B（domain columns，b1）
* 操作：`bmma_sync` + `bmmaBitOpAND` + `bmmaAccumulateOpPOPC`
  CUDA Guide 说 AND 需要 compute capability 8.0+；并规定 b1 的存储映射、layout（A row_major / B col_major）以及 `ldm` 必须是 128 的倍数。
* 输出：popc 累加结果（>=1 即“有支持”）

验收标准：

* 在 Orin 上跑出稳定吞吐，并与 SIMT 版对比；
* 明确 packing/layout 的成本占比（是否值得全局推广）。

> Orin 是 Ampere + Tensor Cores，因此具备使用 Tensor Core 路径的硬件基础。([NVIDIA][1])

### 6.2 在 CPIM 中落地 BMMA 后端（以“替换 ExecuteConstraintCheck 的内核核心”为目标）

做法：

1. 新增 `SupportCheckBackend` 抽象：

   * `SimtBitsetBackend`（你现在的）
   * `BmmaBackend`（新）
2. BmmaBackend 的输入数据布局：

   * A：按 `(cid, x, a)` 的 support 行打包成 b1 tile（对应 WMMA 的 m8n8k128 形状要求）
   * B：把多个 worlds 的 `dom(y)` 组织成列向量（col_major），满足 bmma load 规则。
3. 输出 popc 之后，你仍然要把结果变回：

   * “删值 mask”（world 内）
   * 更新 frontier/队列（world 内）

验收标准：

* correctness 对拍通过（Phase1 的对拍框架）；
* 只在符合条件时启用（大 domain、batch 足够大、GPU 支持）。

### 6.3 风险控制：把它当“可插拔实验后端”，默认不替换主线

PTX 文档也明确说：sub-byte/single-bit WMMA 是 preview feature，未来兼容性没有保证。
因此工程上：

* CMake 选项 `CPIM_ENABLE_BMMA`（默认 OFF）
* 运行时检测 + fallback 到 SIMT
* 在论文/报告里可以作为“硬件对齐加速”亮点，但工程上要稳。

---

## Phase 7：回到系统层：搜索与剪枝的平衡策略（让“剪枝变强 → 树变小”在工程上可控）

你们的目标不是“kernel 快”，是“总求解更快”。这一阶段做 policy：

### 7.1 自适应一致性策略（Policy Engine）

输入：实例特征 + 运行时统计（UNKNOWN 比例、probe p99、删值率、队列增长率）
输出：选择哪种一致性级别

* 只 AC
* NSACQ（默认）
* SACQ-adj（更强）
* Full SACQ（只在少数场景启用）

### 7.2 “UNKNOWN 再处理”机制

UNKNOWN 不删，但可以做：

* 延迟到更深层节点再试（树变小后再强剪枝）
* 或在回溯时用更小 batch 再 probe

验收标准：UNKNOWN 不会“无限堆积”，且能带来实际剪枝收益。

---

## Phase 8：为 FPGA/NPU/RRAM 预留接口（不马上做，但让现在的代码不锁死）

既然你核心思路是 **encoding→bitGEMM/bitGEMV**，那最好的工程姿势是先定义一个“可移植算子 IR”：

### 8.1 定义 BitMatrix Operator API（跨后端）

* `and_popc_gemm(A_support, B_domains) -> popc_counts`
* `or_reduce / any_nonzero`
* `apply_pruning_mask`

CPU 参考实现必须有（用于验证与回归）。

### 8.2 FPGA 方向（优先级次于 GPU，但你说也重要）

* 先输出离线 packed 的 bit-matrix 文件（A/B），用 HLS 做一个 `AND+POPC` 或 `OR` 的原型 kernel（相当于 bitGEMM 的一小块）
* 重点是：把你们 encoding 的数据布局固定下来，未来才能复用

---

## 最后：你应该怎么“开分支”推进（建议的分支/提交策略）

* `feat/metrics-and-regression`：Phase1（统计 + 对拍）
* `feat/budgeted-ac-engine`：Phase2（budget + UNKNOWN）
* `feat/nsacq-worklist`：Phase3（NSACQ + queue cap）
* `feat/probe-pool-scheduler`：Phase4（跨变量 probe 池 + 分桶）
* `feat/bitgemm-route-A` / `feat/bitgemm-route-B`：Phase5
* `exp/bmma-microkernel`：Phase6.1（先别合主干）
* `feat/backend-bmma`：Phase6.2（有数据再合）
* `feat/policy-engine`：Phase7

---

## 你会得到什么“可交付物”

1. **可复现实验脚本**：每个 Phase 都能跑一个 `bench`，输出 CSV
2. **正确性回归**：CPU reference 对拍 + 随机 fuzz
3. **工程上可控的 SACGPU**：NSACQ + budget 默认启用，长尾不炸
4. **bitGEMM 的收益曲线**：路线 A/B/C 的性能/复杂度对比
5. **Tensor Core 1-bit BMMA 的数据支撑**：决定是否投入大改造

   * b1 BMMA/WMMA 的语义与使用规则在 CUDA Guide/PTX 里是有据可依的（AND/XOR + POPC，b1 layout/ldm 要求）。

---

如果你愿意，我可以把这份计划再进一步“工程化”成 **具体到函数/结构体字段的改动清单**（比如：每个 world workspace 加哪些字段、哪些 kernel 参数要新增、NSACQ 的 allowed_mask 放在哪里、probe pool 用什么 queue 实现等），让你可以直接按 checklist 修改代码。

[1]: https://www.nvidia.com/en-eu/autonomous-machines/embedded-systems/jetson-orin/ "https://www.nvidia.com/en-eu/autonomous-machines/embedded-systems/jetson-orin/"

----

好的，那我就把之前那份“分阶段路线图”，进一步**落到“可以直接开 Issue / 开 PR 的工程改动清单”**（尽量按你附件里出现的现有模块命名：`GModel / GModelSolver / BatchProbeManager / ExecuteConstraintCheck_BpC / PropagateVarToNextBitmap / d_subscription / frontier bitmap` 等），并且把 **BMMA(1‑bit Tensor Core)** 作为可插拔后端写进实现计划里。

> 说明：我这里无法直接浏览你 GitHub repo 的文件树，但会严格对齐你文档里已出现的符号与模块边界；你把它当成“工程蓝图 + checklist”，落地时只需要把我写的“目标文件/函数名”映射到你 repo 里对应实现即可。

---

## PR 0：可观测性与回归骨架（先让后面每一步都能量化）

### 0.1 新增统一 Stats/Trace（Host+Device）

**新增结构：**

```cpp
struct ProbeStats {
  int32_t status_ok;
  int32_t status_dwo;
  int32_t status_unknown;
  int32_t iters_sum;
  int32_t iters_p95;   // host 侧后处理即可
  int32_t work_sum;
};

struct SacStats {
  int64_t total_probes;
  int64_t total_queue_pops;
  int64_t total_reenqueue;
  ProbeStats probes;
};

struct AcStats {
  int64_t total_cid_checks;
  int64_t total_value_deletes;
  int64_t total_iters;
};
```

**落地点（建议）：**

* `GModelSolver`：保存整次求解的统计与每个节点统计
* `BatchProbeManager`：对每个 batch 记录 probe 完成率、p99 时间
* GPU kernel：每个 world 的 `iter/work/status` 写回（见 PR1）

**验收：**

* 有 `--dump_csv`，每个实例输出 1 行：`ac_time / sac_time / probes / unknown_rate / p95_iters / deletes / nodes`。

### 0.2 正确性对拍（CPU reference）

最小可行：只做 **二元约束 + bitDom/bitSup** 这一子集的 AC / NSACQ。

* 随机二元 CSP 生成（n, d, tightness）
* 同一随机种子：CPU AC 和 GPU AC 结果一致；CPU NSACQ 和 GPU NSACQ “删值集合一致”（budget=无限时）。

---

## PR 1：Budgeted AC 引擎（SAC 的地基：按住长尾，UNKNOWN 不删）

你文档主线已经明确：必须有 budget，触发预算时返回 UNKNOWN 并且不删，保证 soundness。

### 1.1 World 控制块（每个 probe/world 一份）

**新增结构（device memory）：**

```cpp
enum WorldStatus : int32_t { WS_OK=0, WS_DWO=1, WS_UNKNOWN=2 };

struct WorldCtrl {
  int32_t iter_cnt;
  int32_t work_cnt;      // 处理的cid/word块计数
  int32_t status;
};
```

**新增参数：**

```cpp
struct AcBudget {
  int32_t max_iters;
  int32_t max_work;
};
```

### 1.2 修改 persistent AC 主循环：可“按 world 提前退出”

在你现有“每轮 grid.sync() / 直到 frontier 为空”的循环里加：

* 每个 world 每轮结束 `iter_cnt++`
* 每处理一个 cid 或 word chunk `work_cnt++`
* 超阈值：

  * `status = WS_UNKNOWN`
  * 清空该 world 的 frontier（或打标记让它不再参与后续轮）
  * 但**不能让整个 kernel early-return**（必须 per-world 退出）

**验收：**

* 构造 tightness 极高实例：能稳定结束，不会 hang；
* `UNKNOWN` world **不回写删值**（回写阶段要显式判断）。

---

## PR 2：SAC 外层重构为 Worklist：NSACQ / SACQ‑adj / SACQ（主线）

这一步是“真正把 SAC 从 SAC‑1 的全量反复扫，变成 GPU 友好的队列驱动”。

### 2.1 新接口：EnforceSAC_Worklist

按你文档里给的接口直接做（这里我补齐必要字段）：

```cpp
enum class SacMode { kNSACQ, kSACQAdj, kSACQFull };

struct SacBudget {
  int32_t max_total_probes;
  int32_t max_probes_per_var;
  int32_t max_queue_len;
  int32_t max_queue_pops;
  AcBudget probe_budget;  // PR1
};

SacStats GModelSolver::EnforceSAC_Worklist(SacMode mode,
                                           SacBudget B,
                                           Backend backend);
```

### 2.2 Worklist 数据结构（host 侧）

你可以先用 CPU deque（够用），后面再换成 bitmap queue / device queue。

* `std::deque<int> Q;`
* `in_queue[var]` bitset 防重复
* 选择策略：先实现 `max_degree` 或 `min_dom`，不用纠结论文。

### 2.3 Worklist 主循环伪代码（可直接编码）

```cpp
// 0) 先做一次全局 AC
if (PropagateAC(full_scope, /*budget=*/inf) == DWO) return UNSAT;

// 1) init queue
Q = all_vars;

// 2) while queue not empty and within budget
while (!Q.empty() && queue_pops < B.max_queue_pops) {
  i = pop(Q);

  // 3) probe all values in dom(i) or until max_probes_per_var
  build_probe_batch_for_var(i);

  run_batched_probes_via_BatchProbeManager(probe_budget=B.probe_budget);

  // 4) collect results
  for each value a in dom(i):
     if probe(i=a) == DWO => delete (i,a) in master
     else if probe(i=a) == UNKNOWN => keep (i,a), stats++

  // 5) if master changed, re-enqueue
  if (domain_changed(i)) {
     if mode==kSACQFull: enqueue(all vars)
     else               enqueue(neighbours(i))   // NSACQ / SACQAdj
  }

  // 6) queue cap
  if (Q.size() > B.max_queue_len) break; // stop SAC, return (sound)
}
```

---

## PR 3：NSAC 的关键：allowed‑constraints mask（邻域诱导子图过滤）

这是你文档里强调的“**NSAC 真省钱**”关键点：不改 `ExecuteConstraintCheck_BpC` 核心，只在“入队/扩 frontier”阶段做过滤。

### 3.1 预计算邻域与 allowed 约束 bitset（host）

对每个 focal 变量 `i`：

* `NbVars[i]`: 邻居变量列表（int array）
* `AllowedMask[i]`: bitset over constraints `cid`（u32 words）

构造方法：

* 对每个约束 `cid: (u,v)`
  若 `u ∈ S_i` 且 `v ∈ S_i`（其中 `S_i = {i} ∪ N(i)`）则 `AllowedMask[i].set(cid)=1`

### 3.2 GPU 端传入 allowed_mask 指针（按“本次 probe 的 focal var”选择）

在 `ProbeWorld` / workspace 中增加：

```cpp
struct ProbeMeta {
  int32_t focal_var;
  // 可选：focal_value
  const uint32_t* allowed_cmask; // points into device array [focal_var][words]
};
```

### 3.3 修改 producer：PropagateVarToNextBitmap

在你现在的逻辑里（遍历 `d_subscription[var]` 推 cid 到 next frontier），加一层过滤：

```cpp
if (allowed_cmask[cid>>5] & (1u << (cid & 31))) {
  atomicOr(&next_frontier[cid>>5], 1u << (cid & 31));
}
```

### 3.4 Frontier 初始化策略（2 选 1）

* 快速：从 `assigned_var` 相关 cid 开始，但过滤 allowed
* 更接近定义：`frontier = allowed_mask` 全量启动（更重但更稳定）

**验收：**

* NSACQ 相比 SACQ probe 更短、更少 UNKNOWN；
* 统计上 `probe_budget` 能下降而不损失太多删值。

---

## PR 4：Probe 池工程化：跨变量混合 batch + 分桶（解决“域小欠饱和”与长尾）

你现在 Stage2 persistent blocks 的“probe-per-task”很好用，但会遇到“单变量域太小，batch 不足、SM 欠饱和”。

### 4.1 全局 ProbePool

在 worklist 弹出 var 时，不是“只为这一个 var 生成 probes”，而是：

* 设目标 `target_batch_worlds`（例如 >= 4096）
* 持续从队列弹 var，把它们的 `(var,value)` probe 追加到 `ProbePool`
* Pool 满了再 launch 一次 Stage2

### 4.2 分桶调度（建议先做最便宜的）

按估计难度把 probe 分桶：

* `bucket = dom_size(var)` 或 `estimated_iters(var)`（用历史统计）

策略：

* 先跑“易桶”，尽快删值并触发 re-enqueue（提高收益/时间）
* “难桶”预算更紧（更容易 UNKNOWN，不拖死）

---

## PR 5：bitGEMV → bitGEMM 的渐进实现（先 A/B，C 作为中期重构）

你文档里已经总结：Phase5/Batch‑3D 是可选加速器，主线先稳；bit-matrix packing（Batch‑3D/C）改动巨大，应后置。

### 5.1 路线 A：SIMT 向量化 worlds（低风险）

* 不改 `bitDom` 布局
* 一个 warp 或一个 CTA 同时处理多个 worlds（4/8/16）
* `bitSup` 共享加载复用，提高算术密度

### 5.2 路线 B：warp‑per‑supportrow（更像 bitGEMM 的调度）

* 一个 warp 负责 `(cid, x, a)` 的 support row
* warp 内多个 lane 扫 domain word，做 `AND` 后归约（any/popcount）
* 可选：用 `__ballot_sync` 把多个 worlds 的结果打包

### 5.3 路线 C：bit‑matrix（32 worlds 打包）

* 这一步会触及：trail/backtrack、dom_size 维护、DWO 检测
* 建议等 NSACQ+budget+ProbePool 先稳定后再做

---

## PR 6（实验后端）：Tensor Core 单比特 BMMA backend（AND+POPC）

你现在是 CUDA core 的 AND+POPC（或 AND+any-nonzero）版本；这里给一个“可插拔后端”的落地计划：先 microkernel，再替换传播内核的核心算子。

### 6.0 为什么在 Orin 上可做（硬件/平台 gating）

* Jetson Orin 系列是 Ampere 架构 GPU，带 Tensor Cores（官方规格页写了 Ampere GPU + Tensor Cores）。([NVIDIA][1])
* JetPack AArch64 平台的 compute capability 也是 8.7（NVIDIA TensorRT 10.7 support matrix 直接给了这一平台的 CC）。([NVIDIA Docs][2])
* CUDA 的 b1 `bmma_sync` 明确支持 `bmmaBitOpAND`，但要求 **compute capability ≥ 8.0**，累加方式为 POPC。([NVIDIA Docs][3])

因此：对 Orin（8.7）来说，`bmmaBitOpAND + POPC` 在门槛上是满足的。([NVIDIA Docs][2])

### 6.1 EXP‑1：bmma microkernel bench（不改求解器逻辑）

**目标**：回答“上限吞吐能比 CUDA core 快多少？packing 成本占多少？”

* 实现一个 `bench_bmma_and_popc.cu`
* 输入 A/B 都用 b1 packed
* 调用 `nvcuda::wmma::experimental::bmma_sync`（或更底层 `mma.sync`）
* 注意约束：b1 `load_matrix_sync` 的 `ldm` 需要是 128 的倍数；B1 布局固定 A row_major、B col_major。([NVIDIA Docs][3])

验收：输出 ops/s、吞吐与 SIMT baseline 对比。

### 6.2 EXP‑2：在 CPIM 里做 Backend 抽象 + BMMA 后端

增加后端接口（最小）：

```cpp
struct SupportCheckBackend {
  __device__ virtual uint32_t check_support_batch(...) = 0;
};

struct SimtBackend : SupportCheckBackend { ... };
struct BmmaBackend : SupportCheckBackend { ... };
```

并加两个开关：

* 编译期开关：`CPIM_ENABLE_BMMA`
* 运行时 gating：`if (device_cc >= 80 && batch_size >= threshold) use_bmma`

> PTX ISA 对 single‑bit `mma.sync` 的语义也写得非常直白：对单比特，乘法被逻辑运算替换，并对结果 popc 后累加。([NVIDIA Docs][4])
> 这就是你们“support existence / intersection size”天然匹配的原因。

### 6.3 风险控制：BMMA 是“preview feature”，并且 XOR 变体在新架构有弃用信号

CUDA Guide 说明 sub-byte WMMA 属于 preview，API/数据结构可能变；并且指出 `b1 + XOR` 在 sm_90 有弃用/移除提示。([NVIDIA Docs][3])
所以工程上：

* **默认用 SIMT 后端**
* BMMA 作为可选后端，优先走 AND 路线（更贴合你们 AND+POPC 的语义）

---

## PR 7（可选加速器）：Phase5 / Batch‑3D 的“gather‑apply‑scatter”两阶段写回

你文档里已经把 Phase5/Batch‑3D 定位成“可选加速器”，我建议如果继续推进，优先走“解耦写回”而不是赌 matching。

### 7.1 Gather：并行 `<world,cid>` 只写 removal buffer

* `removal[var][word] |= mask`（atomicOr，冲突可控）

### 7.2 Apply：按 `<world,var>` 应用删除到 bitDom + dom_size

* `bitDom &= ~removal`
* dom_size popcount（可用前缀和或局部 popc）

### 7.3 Scatter：根据 changed vars 生成下一轮 frontier

* 与你现有的 `PropagateVarToNextBitmap` 兼容

---

# 交付物与验收（每个 PR 都能独立 merge）

**每个 PR 的最小验收必须包含：**

1. correctness：CPU 对拍 / fuzz（PR0 的框架）
2. stats：至少能看到 `UNKNOWN`、`DWO`、`iters p95` 等
3. perf：在 2~3 个代表性大实例上不回退（或解释回退原因）

---

## 你现在可以立刻开的一组 Issue（按优先级）

1. **Budgeted AC（per-world UNKNOWN）**（PR1）
2. **NSACQ worklist + queue cap**（PR2）
3. **allowed‑constraints mask + PropagateVarToNextBitmap 过滤**（PR3）
4. **ProbePool 跨变量混 batch**（PR4）
5. **BMMA microkernel bench（独立实验目录）**（PR6.1）

---

如果你接下来把你 repo 里 **3 个关键文件/入口**的路径发我一眼就行（不用贴代码）：

* `GModel` 的定义位置
* `ExecuteConstraintCheck_BpC` 在哪个 `.cu/.cuh`
* `BatchProbeManager`/Stage2 persistent 的入口文件

我就能把上面每个 PR 的“目标函数名”进一步精准到你 repo 的真实路径（比如直接写：改 `src/GModel.cu` 的哪个 kernel、改 `include/*.cuh` 的哪个 struct）。

[1]: https://www.nvidia.com/en-eu/autonomous-machines/embedded-systems/jetson-orin/?utm_source=chatgpt.com "Jetson AGX Orin for Next-Gen Robotics | NVIDIA"
[2]: https://docs.nvidia.com/deeplearning/tensorrt/archives/tensorrt-1070/support-matrix/index.html?utm_source=chatgpt.com "Support Matrix :: NVIDIA Deep Learning TensorRT Documentation"
[3]: https://docs.nvidia.com/cuda/archive/12.1.1/cuda-c-programming-guide/index.html?utm_source=chatgpt.com "CUDA C++ Programming Guide"
[4]: https://docs.nvidia.com/cuda/archive/11.0_GA/parallel-thread-execution/index.html?utm_source=chatgpt.com "PTX ISA :: CUDA Toolkit Documentation"
