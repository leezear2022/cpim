# MSAC（SAC3bit）在 CPU/GPU 求解器上的统一集成设计（基于 UnifiedTrail）

本方案按“你要的叙述方式”组织：把 SAC 当作一种一致性维护算法来用，从 **MAC → MSAC** 讲清楚；同时把我之前指出的工程硬约束（weight 污染、预算/超时、GPU batch 的可行边界）补齐。

---

## 0. 目标与非目标

### 目标

1) **CPU 侧可宣称算法从 MAC 变为 MSAC**：把 `SAC3(内嵌 AC3bit)` 作为一种 `ACAlgorithm` 供搜索器选择。  
2) **GPU 侧把 SAC 的大量传播压到 GPU**：在 GPU 求解器内部嵌入“CPU 侧的 SAC 控制逻辑”，但每次传播由 GPU 的 GAC 执行，以提高 GPU 负载、降低 CPU 负载。  
3) **对照实验清晰**：CPU 上的 SAC 版本作为 baseline，慢是预期；重点比较“GPU 批处理/持久化传播对 SAC 的加速幅度”。

补充说明（与你的动机一致）：本项目引入 MSAC 的首要目的不是“把求解器变强”，而是为 **SAC-GPU** 提供一组可复现、口径清晰的对照实验（CPU baseline / GPU 加速 / batch 化差异）。

### 非目标

- 这份文档不追求把 “SAC + 选哪个 AC 最快” 做到最优组合（那属于后续工程调参/优化）。
- 这份文档不强制你必须使用 LookAhead 叙述；LookAhead 仅作为备选方案留到后续讨论（见文末“方案A”）。

---

## 1. 术语（先把词讲清楚）

### 1.1 ACAlgorithm（基础一致性算法）

在你当前代码体系里，`ACAlgorithm` 就是 `MAC`（搜索器）在每次赋值/删值后调用的“基础传播算法”，例如 `AC3bit / RPC3 / lMaxRPC / NSAC`。它们对当前域做传播删值，返回一致/不一致。

### 1.2 SAC 与 probe

SAC（Singleton Arc Consistency）做的事是：对候选值 `(x=a)` 做一次 **probe（试探）**：

`NewLevel → 临时把 x 固定为 a（ReduceTo）→ 运行一次一致性传播 → Backtrack 恢复`

如果 probe 导致不一致，则 `a` 在当前节点不可能，应该在当前节点“真实删除”。

### 1.3 MSAC

当你在搜索过程中维护的不是普通 AC，而是 SAC（或其变体，如 SAC3），你可以把“MAC + SAC”叙述为 **MSAC**。  
这里我们把 **`SAC3(内嵌 AC3bit)` 固定为一个整体一致性算法**，记为 **MSAC3bit**（名称仅为叙述方便）。

---

## 2. 统一硬约束（你绕不开的 4 件事）

### 2.1 UnifiedTrail 只回滚域，不回滚副作用

当前 `UnifiedTrail` 回滚的是 `IntVar` 的 `bitDom` 修改；不会回滚 `Tabular::weight`、统计计数器、日志、外部缓存/队列。

### 2.2 Probe 失败不能污染启发式权重（Tabular::weight）

你现在的传播实现里，在检测到失败时会对约束做 `++c->weight`（用于 DOM/WDEG 等启发式）。  
但 SAC probe 的失败是“临时试探失败”，不等价于搜索树上的真实失败；如果 probe 也更新权重，启发式会被污染，结果不可复现/对比不公平。

**硬约束**：probe 阶段必须禁止任何 `weight` 更新（以及任何会影响启发式决策的全局统计）。

### 2.3 统计口径必须分层

**硬约束**：
- 节点数/回溯数（positive/negative）由上层搜索维护（MAC 或 GModelSolver）。
- SAC 内部只维护自己的指标：probe 次数、删值数、probe 耗时、是否因预算提前退出等。

否则“节点/回溯”会被 probe 淹没，失去搜索树意义。

### 2.4 SAC 必须预算化，否则 time_limit 形同虚设

SAC 可能在单次 `enforce()` 内做很多 probe。如果它不检查预算/超时，上层搜索来不及打断，time_limit 失去意义。

**硬约束**：SAC 内部必须支持至少两类预算：
- `max_probes`（最多试探多少个值）
- `max_time_ms`（本次 enforce 最多花多少时间）

预算耗尽时的语义：
- 允许提前退出（少删一些值），但必须保持 **soundness**：只能删除“已经证明不可能”的值，不能为了“赶时间”删未证明的值。

---

## 3. CPU 方案：MSAC3bit 作为 ACAlgorithm（SAC3 内嵌 AC3bit）

这里按你的偏好叙述：把 MSAC3bit 当成一个“传播算法”供 MAC 选择，从而可以说算法从 MAC 变 MSAC。

### 3.1 结构（黑盒是可以的，但要带开关）

建议实现一个新的 AC 类（示意命名）：
- `class MSAC3bit : public AC`
- 内部固定持有 `AC3bit`（或 `AC3bit+rm`）作为 kernel，一切 probe 都调用这个 kernel。

从 `MAC` 的视角：
- `ac_` 仍是 `AC*`，只不过可能指向 `MSAC3bit`。
- `MAC` 的节点/回溯统计逻辑不变。

### 3.2 “probe 禁 weight 更新”怎么实现（最少侵入做法）

你说“MSAC 黑盒内部不统计 weight”，这还不够，因为 **probe 里调用的 AC3bit 可能会自己 `++c->weight`**。

最少侵入做法是给传播层加一个“是否允许更新权重”的开关，并让所有 `++weight` 受控：

- 在 `AC`（或更底层）引入一个运行时标志：`allow_weight_updates`（默认 `true`）。
- probe 调用 kernel 传播前临时置 `false`，结束后恢复。
- 所有出现 `++c->weight` 的地方都必须检查该标志。

实现形式可以是：
- `AC::SetAllowWeightUpdates(bool)` + RAII `ScopedWeightUpdates`；或
- “probe 专用 AC3bit_NoWeight” 变体（代码重复更大，不推荐）。

**硬要求**：不管你怎么做，probe 传播路径必须保证不会修改 `Tabular::weight`。

### 3.3 enforce 语义（必须说清楚）

`MSAC3bit::enforce(x_evt, level)` 的逻辑建议定义为：
1) 先跑一次 kernel AC3bit 的基础传播（真实传播，允许 weight 更新）。
2) 若一致，则进入 SAC3 的循环：
   - 选取候选 `(x=a)`（可用 SAC3 的队列/启发式，也可先用 SAC1 全扫起步）
   - 对 `(x=a)` 做 probe（禁 weight 更新，受预算控制）
   - probe 失败则在当前层真实删值，并再次调用 kernel 做真实传播（允许 weight 更新）
   - 直到固定点或预算耗尽

### 3.4 预算与超时（强制落在 MSAC 黑盒里）

建议 MSAC3bit 持有一份配置（示意）：
- `max_probes`
- `max_time_ms`
- `depth_limit`（可选：只在浅层做 MSAC）

这些预算只影响“额外剪枝力度”，不影响正确性：
预算低 → 少删值 → 搜索树更大（慢）但不出错。

### 3.5 CPU baseline 的定位（你要的叙述）

- CPU 的 MSAC3bit 作为对照实现，可以不做 GPU batch、不做快照，仅保证语义正确、口径清晰。
- 慢是预期：它用来对照 “GPU 执行传播 + 批处理/持久化” 的加速幅度。

---

## 4. GPU 方案：在 GPU 求解器内嵌 MSAC 控制，用 GPU 执行传播

### 4.1 核心叙述（符合你的目标）

- MSAC 的“控制逻辑”（挑选 probe、循环、固定点）放在 host（CPU）侧执行，作为黑盒模块嵌入 GPU 求解器。
- 每次需要执行“传播”时，不在 CPU 上跑 AC3bit，而是调用 GPU 的 GAC（`GModel::EnforceGAC` / `EnforceGAC_Persistent`）。

效果：把 SAC 内的大量传播计算压到 GPU，提高 GPU 利用率、降低 CPU 负载。

### 4.2 GPU 上 probe 的域恢复：默认 Snapshot-based

因为 probe 深度=1，且删值可能很多，建议 GPU probe 使用快照绕过 trail：

probe 流程（两阶段，必须明确）：
1) **probe 阶段（不写 trail）**
   - `SnapshotState()`（至少包含 `bitDom`，以及所有影响传播/搜索的元数据）
   - 禁用 trail 记录：`SetTrailRecording(false)`
   - 试探赋值 `AssignValue(var, val)`（不写 trail）
   - 执行一次 GPU GAC（建议用增量入口：只激活 `var` 邻接）
   - 恢复快照 `RestoreState()`
   - `SetTrailRecording(true)`
2) **真实删值阶段（写 trail）**
   - probe 失败 → 在当前层 `RemoveValue(var, val)`（写 trail）
   - 再执行一次“真实 GAC”（写 trail）

### 4.3 “batch 化 AC”到底指什么（把边界讲死）

SAC 的多个 probe **不能共享同一份域状态并行跑**；要真并行，必须为每个 probe 准备独立域副本/工作区（内存很贵）。

因此在 Jetson/UMA 上更现实的 batch 是两类：

**Batch-1（推荐，近期可落地）**：一次启动持久化/长驻传播内核，连续处理多个 probe 任务  
- 目标：摊薄 kernel launch latency 与 host 调度开销  
- 内存：只需要 1 份 snapshot（每次 probe 仍是“串行域状态”）

**Batch-2（长期）**：micro-batch 并行 probe（B 份域副本 + 并行传播）  
- 目标：真正并行多个 probe  
- 代价：需要 `B * sizeof(model_state)` 的额外空间，Jetson 8GB 很快顶不住

你要突出“GPU batch AC 加速 SAC”，建议先用 Batch-1：**一批 probe 由 GPU 持久化传播内核连续执行**，再在 host 汇总结果、做真实删值。

### 4.4 GPU 侧统计口径

同 CPU：
- 搜索层统计 `num_positive/num_negative`
- MSAC 模块统计 `num_probes/num_probe_fail/num_removed/timeout_hits`

GPU 的 `gac_time/gac_iterations/gac_deletions` 可以单独统计为“传播工作量”，用于说明“GPU 负载提升”。

---

## 5. 实现计划（SAC 算法族，按依赖顺序）

这份计划是“实验驱动”的：先保证语义与口径正确，再把 GPU 负载做起来；CPU 版本只需要足够正确与可复现。

### Phase 0：统一语义开关与统计口径（必须先做）

**0.1 `allow_weight_updates`（probe 必须禁 weight）**
- 目标：任何 probe 内部调用传播（AC3/AC3bit/RPC3/lMaxRPC/NSAC 等）时，都不会执行 `++Tabular::weight`。
- 落地建议：在 AC 层引入一个开关（例如 `AC::SetAllowWeightUpdates(bool)` 或 `PropagationOptions`），并把所有 `++c->weight` 包上条件判断。
- 验收：开启/关闭开关时，除 weight 外的删值结果一致；probe 反复执行不会改变后续搜索的启发式选择（可用固定随机种子/固定实例对比）。

**0.2 MSAC 统计结构（与搜索统计分离）**
- 目标：节点/回溯仍由搜索层维护；MSAC 只统计 probe 工作量与删值贡献。
- 建议字段：`num_probes / num_probe_fail / num_removed / probe_time_ms / exited_by_budget`。
- 验收：开启 MSAC 后，`num_positive/num_negative` 不因 probe 激增而失真；MSAC 的统计可单独输出。

**0.3 预算机制（per-enforce）**
- 目标：MSAC 的 `enforce()` 内支持 `max_probes` 与 `max_time_ms`，预算耗尽允许提前退出但不破坏 soundness。
- 验收：设置极小预算时仍能稳定运行且不误删值；设置无限/很大预算时结果稳定收敛。

### Phase 1：CPU 侧 SAC 算法族（baseline + 可控）

**1.1 统一命名与组合（先定死一个组合）**
- 目标：先把组合定死为 `MSAC3bit = SAC3(AC3bit-kernel)`，避免配置爆炸。
- 输出形态：作为一个可选的 `ACAlgorithm`（供 `MAC` 选择）。

**1.2 实现 `MSAC3bit : public AC`（接口对齐 MAC）**
- 目标：让 `MAC` 无感调用，返回 `ConsistencyState`，内部持有 `AC3bit` kernel。
- 必须点：
  - probe 前后切换 `allow_weight_updates=false/true`
  - probe 使用 `trail->NewLevel()/BacktrackTo()`（CPU 走 UnifiedTrail）
  - 真删值后必须再跑一次 kernel 真实传播（允许 weight）
  - 固定点循环 + 预算检查
- 验收：在小实例上（如 queens-4_ext）可复现删值与解；预算不同只影响剪枝强度，不影响正确性。

**1.3 CPU 对照实现：SAC1bit（只用于 correctness baseline）**
- 目的：提供一个最直观的 SAC baseline（全扫 `(x=a)`），用于校验 `MSAC3bit` 的删值不越界（尤其是 weight 开关、trail 回滚边界是否正确）。
- 约束：不需要快；只需要正确、日志清晰、可在小实例上跑完。

**1.4 CLI/脚本与实验口径**
- 增加可复现实验入口（建议）：
  - `--ac_algorithm=MSAC3bit`
  - `--msac_max_probes / --msac_max_time_ms / --msac_depth_limit`
  - `--msac_verbose_stats`
- Python 对比脚本扩展：
  - CPU：`AC3bit` vs `MSAC3bit`（同一实例，删值/节点/时间）
  - CPU baseline：`SAC1bit`（小实例 correctness）

### Phase 2：GPU 侧 MSAC（主目标：提升 GPU 负载，减少 CPU 负载）

**2.1 GModel 支持 probe 的 snapshot 与禁 trail 记录**
- 目标：GPU probe 默认 snapshot-based（参见 `docs/planning/Batch_AC.md` 的 Batch-1/Batch-2 边界）。
- 需要接口：
  - `SetTrailRecording(bool)`
  - `SnapshotState/RestoreState`（至少 `bitDom` + 会影响传播/搜索的元数据）
- 验收：probe 结束后状态完全恢复（包含域与元数据）；连续 probe 不累积“脏状态”。

**2.2 在 GPU 求解器内嵌 MSAC 控制逻辑（控制在 host，传播在 GPU）**
- 目标：复用 MSAC 的控制结构（挑 probe、预算、固定点），但把 kernel propagation 替换为 `GModel::EnforceGAC(_Persistent)`。
- 验收：同一实例上，GPU-MSAC 与 CPU-MSAC 在“删掉哪些值（在同等预算下）”上保持一致或可解释差异（如传播实现不同导致的顺序差异）。

**2.3 Batch-1：持久化/长驻传播内核连续处理 probe（重点卖点）**
- 目标：把“一次 probe 一次 kernel”的 launch 开销摊薄到“一批 probe 一次（或少量）kernel”。
- 形式：device 端队列/状态机（可对齐 `aig_docs/GPU_GAC_PERSISTENT_STATE_MACHINE.md` 的思路），一次启动后循环处理 probe 列表。
- 验收：相同 probe 数量下，kernel launch 次数显著下降；`gac_time` 更接近纯计算时间；CPU 占用下降。

### Phase 3（可选，长期）：Batch-2 micro-batch（近似 bitGEMM）

只有当 Phase 2 的 Batch-1 已经跑通且证明确有瓶颈时才考虑：
- 维护 `B` 个世界的域与元数据，micro-batch 并行推进到不动点；
- 重点挑战是内存与 per-world 传播状态管理（Jetson 8GB 需严格算账）。

---

## 6. 方案A（待讨论）：LookAhead 叙述方式

备选叙述是：`ACAlgorithm` 仍选 AC3bit/RPC3 等基础算法，SAC 作为“可选 LookAhead 阶段”接在每次基础传播之后执行。  
它的优点是把“预算化/触发条件”表达得更自然；缺点是叙述上不如 MSAC 直观。后续再讨论是否需要在文档/论文里同时保留两种表述。
