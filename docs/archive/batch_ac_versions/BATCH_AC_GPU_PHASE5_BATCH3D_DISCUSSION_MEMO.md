# Phase 5（Batch‑3D）推进问题与讨论备忘

> 目的：总结 CPIM 在 Phase 5（Batch‑3D/约束聚合/任务打散）推进时遇到的主要
> 性能与工程问题，并记录我们对“GPU 强相容性（SAC/MSAC）加速”的阶段性共识、
> 分歧与下一步数据验证清单。
>
> 用途：作为对外沟通材料，供与外部大模型做深度方案评审；方案定稿后再回填实现计划。

---

## 0. 背景与目标

- 目标：用 GPU 加速强推理（AC/SAC/MSAC），以减少搜索树规模与回溯次数。
- 主线现状：Batch‑2（Stage 1 Micro‑Batch / Stage 2 Persistent Blocks / Auto）
  已接入 `src/solver/gpu/GModelSolver.cu:514` 的 `GModelSolver::EnforceSAC1()`，
  具备可用的 SAC1/MSAC 基础设施与回归测试。
- 平台约束：Jetson Orin（约 8 SM，UMA；`concurrentManagedAccess==0`），更容易
  memory‑bound；cooperative 驻留限制与 shared memory 压力更敏感。

---

## 1. Phase 1‑4 已完成工作概览（用于外部对齐）

本节用于给外部评审提供“到 Phase 5 之前我们已经把哪些核心能力做完了”的上下文，
避免在 Phase 5 方案讨论时重复解释基础设施。

### 1.1 Phase 1：Batch‑1 基础（单 probe 的 cooperative 持久 kernel）

- 内容：用 cooperative kernel + 快照机制，实现 “SAC probe（单例赋值）→ AC 传播到不动点”
  的 GPU 基线版本（probe 之间串行，probe 内多 block 协作）。
- 关键资产：
  - Host 管理器：`include/solver/gpu/batch_probe_manager.h:71`（`BatchProbeManager`）
  - 入口 kernel/wrapper：`src/solver/gpu/GModel.cu`（`LaunchPersistentBatchProbeKernelWrapper` 等）
- 作用：作为 correctness baseline，同时复用传播内核与数据结构（`bitDom/bitSupData/frontier`）。

### 1.2 Phase 2：Batch‑1 性能优化（已落地的部分）

已完成/已验证：
- **Frontier 邻域激活（NEIGHBOR_ACTIVATION）**：在快照已 AC 前提下，从赋值变量邻接约束起步，
  用增量扩散达到同一不动点；减少 frontier 初始化开销。
- **Cheap Precheck（早失败）**：基于 `bitSupData` 的必要条件检查（方向/索引对齐现有实现）。
  说明：在“快照确实 AC”前提下，早失败命中率理论上可能很低；工程上更接近防御性检查/断言。
- **域大小增量更新**：用 removed bits 的 `__popc` + `atomicSub` 代替全量重算，减少热路径扫描。
- **Warp‑per‑Word 约束检查**：将约束检查的并行粒度从“thread‑stripe”调整为“warp 处理一个 word”，
  用 `__ballot_sync` 聚合决策，减少 shared/atomic 开销（已推广到 Batch‑1 与 Workspace 路径）。

未做/仍可选：
- 设计文档里“把 DWO 检测完全移出热路径（changed_vars_bitmap + 边界集中检查）”的版本尚未推进，
  当前更多采用“增量更新 + 仍保留即时 DWO 早停”的折中实现。

### 1.3 Phase 3：Batch‑2 基础（probe 级空间并行，主线已接入）

- Stage 1（Micro‑Batch）：`include/solver/gpu/batch_probe_manager.h:202`（`Batch2ProbeManager`）
  - Host 分批 launch；每个 probe 一个 block；workspace 只分配 micro‑batch 大小，避免内存爆炸。
- Stage 2（Persistent Blocks）：`include/solver/gpu/batch_probe_manager.h:339`（`Batch2PersistentManager`）
  - 一个 kernel 内由持久 blocks 用 `atomicAdd(task_cursor)` 拉取任务；动态负载均衡；适合高失败率/约束密集场景。
- Auto 选择器：`include/solver/gpu/batch_probe_manager.h:477`（`AutoStageSelector`）
  - 支持采样/实测对比与缓存（`DecideCached`），用于“不同实例/不同任务分布下自动选 Stage 1/2”。
- 端到端集成：`src/solver/gpu/GModelSolver.cu:514`（`GModelSolver::EnforceSAC1()`）
  - 集成 dirty set 增量任务收集、早停预算、阶段选择缓存等，用于 SAC1/MSAC 预处理主线。

### 1.4 Phase 4：Batch‑2 工程化优化（已做/未做）

已做：
- **4.3 只读数据优化（hint）**：`src/model/gmodel_adapter.cu:472`（`OptimizeReadOnlyMemoryAdvice`）
  对 `bitSupData/d_subscription/d_subscription_offset/constraint_scopes` 设置 `cudaMemAdviseSetReadMostly`，
  并做 Jetson UMA 兼容与日志统计。说明：`cudaMemAdvise` 是 hint，不保证加速，需要用 probes/s 与 Nsight 指标量化。
- **4.2 Warp‑per‑Word**：已先在 Workspace 路径落地并验证，再推广到 Batch‑1（见 `CHANGES_ZH.md`）。

未做：
- **4.1 Copy‑on‑Write/Delta**：尚未落地（内存‑50% 目标），需要较大重构与专门的 correctness 回归。

### 1.5 “Full/fast MSAC”现象（驱动 Phase 5 的动机之一）

- full 模式：更完整、更强，但在大域/高 tightness（例如 `rand-2-40-180, t=0.9`）上可能极慢，
  典型原因是“很多值 GAC‑一致但非 SAC‑一致”，导致外层反复试值 + 内层完整推理长尾。
- fast 模式：更实用，能用于性能评测，但可能遇到 SAC‑DWO 需要回溯处理。
- 结论：即使 GPU 提供 probe 级并行，仍需要预算/降级策略与更高效的调度/复用（Phase 5 的目标）。

---

## 2. Phase 5（Batch‑3D）定义（本文语境）

设计文档里 Phase 5 对应 Batch‑3A（约束聚合 + 异步调度）。本文用“Batch‑3D”指代
更激进的一类实现设想：将工作单位从“probe/world”进一步打散到更细粒度（例如
`<world,cid>`，甚至 `<world,cid,word>`），尝试用全局队列/工作窃取消除长尾。

共同目标：
- 让 GPU 在“强推理”阶段持续饱和，减少因长尾 world/probe 导致的空转。
- 降低 `bitSupData` 的重复加载成本（通过约束聚合或约束亲和性提高复用）。

---

## 3. 当前实现现状（Batch‑3A MVP / 多 block）

代码入口与结构：
- 管理器与控制块：`include/solver/gpu/batch_probe_manager.h:639`（`Batch3AManager`）
  与 `include/solver/gpu/batch_probe_manager.h:571`（`Batch3AControl`）。
- Kernel：`src/solver/gpu/GModel.cu:3746`（`Batch3AKernel_MultiBlock`）。
- 约束检查（聚合）：`src/solver/gpu/GModel.cu:3406`
  （`ExecuteConstraintCheck_Aggregated`）。
- Launch 配置：`src/solver/gpu/GModel.cu:3934`（`LaunchBatch3AKernelWrapper`）
  当前固定 `G=4`，`block_size=128`，`num_worlds<=32` ⇒ `num_blocks<=8`。

观测现象（典型）：
- 正确性：与 Stage 2（Persistent Blocks）一致（失败 probe 集合匹配）。
- 性能：在多组实例上仍显著慢于 Stage 2（典型 4×～8× 量级）。

---

## 4. Phase 5/Batch‑3D 面临的核心问题（归纳）

### 3.1 并行度上限与“欠饱和”

- Stage 2 的并行单位是 probe（通常可开到几十 blocks），更容易填满 Orin 的 8 SM。
- Batch‑3A/3D 受 `max_worlds` 与 `worlds_per_block=G` 影响，在当前实现中并行 blocks
  很少（最多 8），容易欠饱和；即使每个 block 内做聚合，也可能总体吞吐不如 Stage 2。

### 3.2 单约束内核的 warp 利用率低（lane 空转）与分歧

当前 `ExecuteConstraintCheck_Aggregated` 的关键结构是 lane→word，再用 `__ffs`
循环扫 bit。对 `bit_dom_int_size=6`（dom=180）这类实例：
- 活跃 lane 只有 6/32，warp 利用率天然偏低。
- `__ffs` 的 while 循环会带来较大分歧与指令开销。

这会导致“即使 bitSup 复用了，计算端仍然没有把 SM 吃满”。

### 3.3 bitSup shared 缓存的固定成本与 shared memory 压力

- `bitSupData` 对 dom=180 的约束，按当前估算每约束约 17KB（量级）。
- 每个约束都要把 bitSup 拷到 shared，一方面是固定开销，另一方面显著抬高
  shared memory 使用，降低 occupancy。
- shared 只能 block 内复用，不能跨 block 共享；跨 block 的复用只能依赖 L2，
  这需要“约束亲和性/任务聚合”来提高命中率。

### 3.4 稀疏任务队列构建与调度开销

当前稀疏队列由 thread 0 串行构建（每轮生成本 block 的 `(cid, local_world_mask)` 列表）：
- 激活率高时，这段串行构建可能接近 O(C) 热点。
- 更激进的 flatten/聚合会进一步引入“重新排队/重建队列”的额外开销，
  可能吞掉 bitSup 复用收益。

### 3.5 “彻底打散”的并发写冲突（world ownership vs 原子风暴）

你提出的“把任务彻底打散（`<world,cid>`/`<world,cid,word>`）”能扩大并行度，
但代价是同一 world 的域状态共享：
- 多 worker 同时更新同一 world 的 `bitDom/dom_size/frontier` 会引入大量原子/锁，
  很容易形成原子风暴，整体变慢。
- 保留 world ownership（一个 block/warp 独占写）能保持无锁写回，但任务调度自由度下降，
  难以完全消除长尾。

因此 Phase 5 的关键不是“更细就一定更快”，而是要在“更细的并行度”与“写冲突”
之间找到可控的折中。

### 3.6 强相容性在大域/高 tightness 下的长尾与成本爆炸

以 `rand-2-40-180, tightness=0.9` 为代表：
- “证明某个值不 SAC”可能需要很长的 fixpoint 传播（轮次多、删值多、访存多）。
- Full MSAC 外层还存在“试值→完整 SAC→再试值”的串行循环。

结论：即使 probe 层并行做得很好，仍需要引入预算/降级机制才能避免被长尾拖死。

---

## 5. 讨论中形成的阶段性共识

1. GPU 更适合并行推理（批量 AC/SAC），并行搜索分支不是当前主攻方向。
2. Phase 5 必须是“自适应启用”的加速器：
   - 只有当约束聚合度（`world_mask` popcount）足够高、且并行 blocks 能饱和 SM 时
     才考虑启用 Batch‑3A/3D；
   - 否则回退到 Stage 2（probe‑per‑block）更稳、更快。
3. 面对长尾，强相容性要“可预算/可降级”：
   - 可以在超时/超迭代/删值率过低时中断；
   - 中断的语义应当是 unknown（不删），保证正确性；
   - 可降级到 NSAC 家族或有界传播，追求性价比而非最强一致性。
4. 换更强 GPU（如 4090）通常会显著提升吞吐，但无法从根本上消除算法长尾，
   预算/降级仍是工程必需品。

---

## 6. 待外部评审的关键问题（用于对话清单）

### 5.1 工作单位与同步语义

- 最合适的工作单位是什么？
  - `(<cid, world_mask>)`：复用 bitSup，写冲突少，但需要足够聚合度。
  - `(<world, cid>)`：并行度大，但写冲突风险高，需要 world ownership 或 CoW。
  - `(<world, cid, word>)`：更细粒度，但调度与合并开销可能更高。
- 是否存在“读并行、写独占”的可行结构（减少原子风暴但提升并行度）？

### 5.2 bitSup 复用收益边界与预测

- 真实 benchmark 上 `world_mask` 的 popcount 分布如何（p50/p95/mean）？
- 哪些实例族会出现“高聚合度”（popcount≥2/4/8）？
- 如何用少量采样快速决定启用/回退（类似 Stage auto selector）？

### 5.3 Orin 与 4090 的策略差异

- Orin 更容易 memory‑bound：shared 缓存是否划算？是否应更偏向 L2 亲和性？
- 4090 的带宽与 L2 更大：Batch‑3A/3D 的收益边界是否更宽？
- 如果切换离散 GPU，managed memory/预取策略是否需要调整？

---

## 7. 建议的最小实验集（先用数据决定方向）

优先采集“能决定 Phase 5 是否值得”的指标，而不是先做大重构。

建议补充统计（按 iteration/constraint/world）：
- `world_mask` popcount 分布（mean/p50/p95/max）
- 活跃约束比例（稀疏队列长度 / `num_constraints`）
- 每 probe 的 `iterations/deletions` 分布（长尾强度）
- bitSup bytes、shared_mem_bytes、实际启动 blocks 数（是否欠饱和）

初步决策规则（示例，需用数据校准）：
- mean(popcount) < 1.5：优先回退 Stage 2
- blocks < 2*num_sms：倾向欠饱和，优先回退 Stage 2
- bitSup bytes 很大且 popcount 低：shared 缓存不划算，优先回退

---

## 8. 与主线优化的关系（建议）

短期目标（优先）：
- 先把 SAC/MSAC 主线封版：Stage1/Stage2/Auto + 预算/降级策略，
  保证在“困难实例”上也不会卡死。

中期目标（可选）：
- 若数据证明存在稳定的“高聚合度/强不均衡”实例族，再投入 Batch‑3A/3D：
  重点是“自适应启用 + 约束亲和性 + 队列构建开销控制”，而不是盲目细粒度打散。
