---
status: active
updated: 2026-01-25
---

# TODO：SACGPU / Batch-AC 下一阶段工作备忘（2026-01）

本文是“接下来一段时间的实施清单”，用于把 `SACGPU_DESIGN.md` 与 `total_plan.md`
落到可执行的任务项，并对照当前代码状态标注优先级与依赖关系。

## 0. 范围与硬约束（不做会翻车）

- **Soundness 红线**：任何 budget/降级触发时只能返回 `UNKNOWN` 并且**不删值**。
- **可回退**：任何新路径（NSAC mask / Batch-3A / bitGEMM / BMMA）都要能运行时回退到
  Stage2（Batch-2 Persistent）稳定路径。
- **可观测**：必须能量化“长尾来自哪里”，否则后续优化无法闭环。
- **目标平台**：Jetson Orin（SM 8.7，UMA，`concurrentManagedAccess==0`）。

## 1. 文档“可信度分层”（避免被过时文档误导）

- **主入口（以此为准）**
  - `docs/planning/SACGPU_DESIGN.md`：统一设计主线与优先级。
  - `docs/planning/total_plan.md`：工程拆解（Phase/PR 列表）。
- **实现对齐（用于判断“做没做/怎么做”）**
  - `docs/implementation/SAC3_MSAC_IMPLEMENTATION.md`：SAC3/MSAC 现状实现说明。
  - `docs/planning/SAC_OPTIMIZATION_ROADMAP.md`：SAC1→SAC3 的已完成里程碑。
  - `docs/gpu/BATCH_AC_DESIGN.md`：Batch-1/2/3 的边界、正确性前提（snapshot 已 AC 等）。
- **归档/历史（参考思路，不作为现状依据）**
  - `docs/archive/**`、`docs/archive/obsolete/**`。

## 2. 现状快照（2026-01）

- ✅ Probe-level 扁平化：Batch-2 Stage1/Stage2/Auto + Precheck 已落地。
- ✅ SAC1（dirty-set + 早停）与 SAC3（probe 队列驱动）已落地；搜索中的轻量 MSAC 已落地。
- ✅ P0-1：Stage2 已支持 `ProbeStatus {kOK,kDWO,kUNKNOWN}` 与统计闭环；仅对 `kDWO` 删值。
- ✅ P0-1a：Stage2 已支持停滞检测（stagnation-based soft budget）。
- ✅ P0-1b：Stage2 已支持工作量子（quantum）检查基础设施（默认关闭）。
- ✅ P0-1c：SAC3 已接入 deferred recheck（UNKNOWN 入队，邻域 epoch 变化后重检）。
- ✅ P0-1d（V1）：SAC3 ProbeQueue 支持 failure-priority 分桶调度（dom/deg/hist DWO，默认关闭）。
- ✅ P0-2：NSAC `allowed-constraints mask` 已落地（singleton test 传播限制到 `Xi + N(Xi)` 诱导子图）。
- ✅ Batch-3A（约束聚合）已接入 solver 主路径（gating + 运行时开关）；但在当前测试用例上评估慢 10–25x，
  建议默认关闭，仅保留用于消融/复测。
- ❌ bitGEMM（lane→world 的内核形态 / DomSoA 数据布局）未落地。
- ❌ `bmma_sync(b1, AND+POPC)` 未落地（仅在计划文档中提及）。

### 2.1 对照 `SACGPU_DESIGN.md` 的 “flatten” 三层现状

- ✅ **Probe-level flatten**：已落地（Batch-2 Stage2 persistent blocks + Stage auto）。
- ✅/⚠️ **Constraint-level flatten**：Batch-3A 已可跑且已接主路径，但当前评测显示慢 10–25x，默认应保持关闭。
- ❌ **Word/value-level flatten**：Batch-3D（更细粒度任务打散）尚未进入主线实现。

### 2.2 Preprocess 评测/回归基线现状（面向“推理能力”口径）

- ✅ 已建立 “可中断/可续跑/CSV 输出” 的 preprocess 跑批工具链（`select_sac_preprocess_benches.py`、`batch_sac_benchmark.py`）。
- ✅ 已引入更适合“每次改动都跑”的 **回归/性能哨兵 suite**（`--suite=regression/perf`）。
- ⚠️ `status=TIMEOUT` 在 preprocess CSV 里通常表示 **`max_sac_rounds` 用尽未收敛**，不等价于 wall-time 超时（wall-time 由 `--timeout` 控制）。
- ⚠️ 已知异常样例（用于扫描时 exclude）：`benchmarks/graphs/graphw-*`（解析/输出不稳定）；`benchmarks/marc/large-92-unsat_ext.xml` 在 Orin 8G 上可能 OOM。

## 3. TODO 清单（按优先级与依赖排序）

### P0：必须先做（稳定性/正确性/闭环）

- [x] **P0-1：显式 `UNKNOWN` 语义与统计闭环**（基础已完成 0c87a46）
  - 目标：probe 执行若 hit budget，则标记 `UNKNOWN`，结果一律"不删"。
  - 交付：
    - `ProbeStatus { OK, DWO, UNKNOWN }`（Stage1/Stage2/Batch3A 统一口径）✅
    - 统计：unknown_rate、budget_hit_count、p95/p99 iters/work ✅
    - solver 回写：仅对 `DWO` 执行 `RemoveValue`。✅
  - 验收：启用极小 budget 时不误删；关闭 budget 时结果与当前一致。

- [x] **P0-1a：停滞检测（Stagnation Detection）**（已完成）
  - 目标：用多指标判定"长尾"，而非硬 max_iters 截断。
  - 检测指标：
    - `Δdeletions`：连续 k 轮 deletions==0（停滞）✅
    - `frontier_popcount`：活跃约束数很大但删除几乎没有 ✅
    - `deletions / work_cnt`：单位工作产出低于阈值 ✅
  - 交付：
    - `WorldWorkspace` 添加字段：`stagnation_count`, `last_deletions`, `last_frontier_popcount`, `work_cnt` ✅
    - `Batch2PersistentControl` 添加配置：`stagnation_threshold`, `min_productivity`, `enable_stagnation_check` ✅
    - `RunGACToFixpoint_BlockSync` 每轮后检查停滞 ✅
  - 验收：`batch_test_v2.py --tier=0` 通过

- [x] **P0-1b：时间片调度基础设施**（部分完成）
  - 目标：每个 world 处理固定量子后让出，长尾不阻塞整批。
  - 已完成：
    - `GACTimesliceState` 结构体（预留暂停/恢复）✅
    - `WorldWorkspace` 添加 `total_constraints_checked`, `quantum_exceeded` ✅
    - `Batch2PersistentControl` 添加 `quantum_cid`, `enable_quantum_check` ✅
    - `RunGACToFixpoint_BlockSync` 工作量子检查逻辑 ✅
  - 待完成：
    - 完整 yield/resume 逻辑（需 Batch-3A 载体稳定）
    - Host 端循环处理 yield 的 probe
  - 验收：最慢 probe 不超过平均的 5 倍

- [x] **P0-1c：延后复查队列（Deferred Recheck）**（V1 邻域 epoch 已落地）
  - 目标：UNKNOWN 的 probe 不丢弃，放入"待复查队列"，邻域变化时重跑。
  - 背景：P0-1a（停滞检测）/P0-1b（工作量子）会产生 UNKNOWN；如果 UNKNOWN 直接丢弃，会导致“长尾值永远不复查”，
    剪枝收益不足，最终在难例上体现为搜索/预处理超时。
  - 关键设计（建议分两档实现，先做 V1）：
    - **V1（推荐，低开销）**：邻域 epoch（neighbourhood epoch）
      - `var_version[v]`：每次 `RemoveValue(v,*)` 后递增。
      - `nb_epoch[i]`：当 `i` 的任一邻居变量发生删值时递增（可复用 `EnqueueNeighborhood()` 的邻接遍历）。
      - DeferredProbe 记录：`{var_id, value, nb_epoch_snapshot, retry, enqueue_round}`。
      - 复查条件：`nb_epoch[var_id] > nb_epoch_snapshot` 才重跑（允许少量 false positive；false negative 只会少删，仍 sound）。
    - **V2（更精确，但更重）**：邻域版本快照（per-probe snapshot）
      - DeferredProbe 记录邻域变量的 `var_version` 快照（`neighbours(var_id)` 的版本数组）。
      - 复查条件：`any neighbor var_version > snapshot`。
  - 交付：
    - `include/GModelSolver.h`：添加版本/epoch 追踪容器与运行时开关、统计结构。
    - `src/solver/gpu/GModelSolver.cu`：
      - 添加 `DeferredProbeQueue`（上限、重检次数、超期清理）。
      - 集成到 `EnforceSAC3()`：每轮优先取 `deferred_queue.CollectReadyTasks()`，否则再从 `ProbeQueue` 出队。
      - 三态处理：`kDWO` → 删值 + 版本/epoch 递增 + 邻域入队；`kUNKNOWN` → 入 deferred queue；`kOK` → 无操作。
    - `include/solver/gpu/batch_probe_manager.h`/`src/solver/gpu/batch_probe_manager.cu`：补齐“host 能拿到每个 task 的三态结果”
      的最小接口（例如返回 unknown 列表或暴露 `task_status` 只读视图），避免 UNKNOWN 任务在 host 层丢失。
    - 统计闭环：`deferred_in/out/hit/stale/retry` 与 “UNKNOWN → deferred → DWO” 的转化率。
  - 运行时开关（建议）：
    - `enable_deferred_queue`：总开关（关闭时回退到当前行为）。
    - `max_deferred_retries` / `max_deferred_queue_size` / `max_deferred_age_rounds`：防止队列膨胀与无限重检。
  - 验收：UNKNOWN 的值在邻域变化后被重新检测

- [x] **P0-1d：失败概率优先（Failure Priority）**（V1 分桶调度已落地，待数据验证/调参）
  - 目标：高失败概率的值先 probe，快速产生删值。
  - 定位：这是 **host 侧调度/排序策略**，与 Stage2 的 `UNKNOWN` 语义（P0-1）/停滞检测（P0-1a）/量子检查（P0-1b）
    以及 deferred recheck（P0-1c）**一起形成闭环**：
    - 先跑“更可能 DWO”的 probe → 早删值 → 队列更快收缩、后续传播更轻；
    - 对“明显长尾/无产出”的 probe：Stage2 提前标 `UNKNOWN` → 进入 deferred queue → **邻域删值变化后再重检**（暂停≠放弃，仍 sound）。
  - 建议实现路线：优先做 **软优先级（bucketed queue）**，避免堆/优先队列的维护开销与原子热点。
    - V1（低风险）：只用 var 级 cheap 特征（`dom_size`/`degree`/历史 DWO 率）打分并分桶。
    - V2（可选）：引入更细粒度的 value 级信号（例如“支持稀疏度”/popcount 近似），但必须明确其计算/搬运开销。
  - 估计信号（从便宜到昂贵）：
    - `score_dom_deg = degree(var) / max(1, dom_size(var))`（约束更紧密的 var 先跑）
    - `score_hist = EMA(dwo)`（同 var 历史 probe 的 DWO 命中率）
    - `score_precheck`（可选）：cheap precheck 的统计信号（例如短路命中率，或扩展为“支持计数”）
  - 交付（建议落点）：
    - `src/solver/gpu/GModelSolver.cu` 的 `ProbeQueue`：从 FIFO 改为 `N` 个 bucket 的 `deque`（保持 `in_queue_bits_` 去重不变）
    - `GModelSolver`：新增运行时开关/桶数/权重，并输出 per-bucket 命中率统计（用于验证“更可能 DWO”）
  - 验收（可观测）：
    - 相同 budget/time 下，`dwo_count/time_ms` 上升或 `total_probes_checked` 下降
    - per-bucket `dwo_hit_rate` 单调递减（否则说明评分无效）
    - `enable_failure_priority=false` 时行为回退到当前 FIFO（便于消融）

- [x] **P0-2：NSAC 的 `allowed-constraints mask`（真邻域子图）**（已落地）
  - 目标：singleton test 的传播严格限制在 `Xi + N(Xi)` 诱导子图（NSACQ/NSAC）。
  - 交付：
    - CPU 端预计算 `allowed_masks[focal_var][cid]`（位图 words）
    - GPU 端：frontier init + frontier 扩张（`PropagateVarToNextBitmap`）按 focal_var 做 mask 过滤
  - 验收：在同一 budget 下，平均 probe iters/work 下降；UNKNOWN 比例可控；结果 sound。

- [x] **P0-3：统一观测入口（最小可用）**（已落地）
  - 目标：能用同一套统计口径对比 Stage1/Stage2/SAC1/SAC3/（未来 Batch3A）。
  - 交付：
    - 至少支持：每批 probes 的 `num_probes / dwo / unknown / avg/p95 iters / time_ms`
    - Python 脚本输出 CSV（tier0 即可）。
  - 现状（已完成）：
    - `apps/sac_benchmark.cpp` 增加 preprocess 模式：`--mode=sac1_preprocess` / `--mode=sac3_preprocess`，并统一输出字段：
      `Total time/Total probes/Total deletions/Avg/P95/Max iterations/Unknown probes/Status`。
    - `tests/python/select_sac_preprocess_benches.py` / `tests/python/batch_sac_benchmark.py` 支持 `--mode`，CSV 增加 `mode/p95_iterations` 字段。
    - 现在 suite/脚本可直接 A/B：`full_sac(stage2)` vs `sac3_preprocess(flatten nsacq/sacq)`（同一口径、同一解析器）。

### P1：主线增强（把长尾变可控、把并行度吃满）

- [x] **P1-1：把 Batch-3A 接入 solver 主路径（可选加速器）**（最小风险接入已落地）
  - 目标：在“欠饱和/负载不均衡”场景，允许用 Batch-3A（约束聚合）顶上。
  - 依赖：
    - **P0-1**（`UNKNOWN` 语义）：否则无法定义“budget hit → 不删 → 回退”的安全行为。
    - **P0-3**（统计基础设施）：否则无法做“欠饱和检测 → 启用/回退策略”的数据闭环。
  - 最小风险 gating（本次落地）：
    - `nsac_mask_enabled=true` 时 **禁用 Batch-3A 并回退 Stage2**，保证默认路径始终是“真 NSAC”。
    - 提供运行时开关 `--sac_use_batch3a`（仅在 `nsac_mask=false` 时可生效）。
    - Batch-3A 结束后若 `active_world_mask` 仍有 bit，则视为 `UNKNOWN`（不删、可进入 deferred recheck）。
  - 启用条件（建议先保守）：
    - `bitSup` 可放入 shared memory（已有判断）
    - `world_mask popcount` 均值/分位数达到阈值（避免聚合无收益）
  - 回退：不满足条件时回退 Stage2（Persistent Blocks）。
  - 验收：在长尾实例上 p99 时间下降或吞吐更稳定；结果一致。
  - 现状评估：在当前测试用例上 A/B 测试显示 Batch-3A 相比 Stage2 慢 10–25x，建议保持 default-off，
    仅保留 `--sac_use_batch3a` 用于消融与未来复测（以数据驱动决定是否继续投入）。
  - 代码落点（建议）：
    - solver 侧入口：`src/solver/gpu/GModelSolver.cu`（SAC1/SAC3/MSAC 的 stage 选择点）
    - manager/kernel：`include/solver/gpu/batch_probe_manager.h`、`src/solver/gpu/batch_probe_manager.cu`、
      `src/solver/gpu/GModel.cu`（Batch3A wrapper/kernel）

- [x] **P1-2：预算双阀门（per-probe + 外层 queue）工程化**（已落地）
  - 目标：让 SAC3 在难例上可控结束，并把 UNKNOWN 语义贯穿到统计与回退策略（宁可少删，不许多删）。
  - 交付（已完成）：
    - `include/GModelSolver.h`：新增 `GModelSolver::SacQueueBudgetConfig`（max_total_probes/max_queue_size/max_total_requeues/max_requeues_per_var）。
    - `src/solver/gpu/GModelSolver.cu`：
      - SAC3 支持 `max_total_probes`（到达上限则 early_stop，语义与现有 TIMEOUT 对齐）。
      - ProbeQueue 支持 `max_queue_size`（溢出丢弃新入队）与 requeue 上限（停止/跳过邻域扩张）。
    - `apps/sac_benchmark.cpp`：preprocess 模式增加可配置 flags（`--sac_max_total_probes/--sac_max_queue_size/...`）。
  - 验收：budget 打开/关闭均不影响 soundness；开启极小预算时只会少删（早停/丢弃入队），不会多删。

- [x] **P1-3：ProbePool 2.0（跨变量混 batch + 分桶）**（已落地：Stage 选择“分桶缓存”）
  - 目标：解决“任务规模变化导致 AutoStageSelector 缓存污染”，避免出现“第一次大 batch → 后续小 batch 也被迫 Stage2”
    的欠饱和/高开销情况。
  - 交付（已完成）：
    - `include/solver/gpu/batch_probe_manager.h` / `src/solver/gpu/batch_probe_manager.cu`：
      - `AutoStageSelector::DecideCached()` 改为 **按任务量分桶缓存（medium/large）**；
      - 每次调用先走 `DecideByTaskCount()`：小任务直接 Stage1，不再受缓存影响。
  - 备注：该改动对 SAC1/SAC3/MSAC 的 “Stage 1 vs Stage 2” 自适应选择更鲁棒；无需改动 kernel 语义。

### P2：bitGEMM/算子形态（以数据驱动决定是否推进重构）

- [ ] **P2-1：bitGEMM Route-A（低风险，先拿收益曲线）**
  - 目标：不改全局数据布局，先在 kernel 内对多 world 做复用/向量化，测“上限”。
  - 载体：优先放在 Batch-3A（同 cid 多 world）或 Stage2（block 内处理多个 task）。
  - 验收：给出 microbench + e2e 两条曲线，证明是否值得做 Route-B/C。

- [ ] **P2-2：bitGEMM Route-B（lane→world / warp 形态重排）**
  - 目标：把 `lane` 从 `word/value` 改为 `world`，提升 warp 利用率与 dom 复用。
  - 说明：这一步通常需要引入 SoA/packing（实现成本显著上升，需评估收益）。

### P3：BMMA（1-bit Tensor Core）实验后端（创新点）

- [ ] **P3-1：EXP-1 microkernel：`bmma_sync(b1, AND+POPC)` 上限验证**
  - 目标：回答“在 Orin/RTX 上，BMMA 的吞吐上限 vs CUDA-core AND/POPC”。
  - 输出：吞吐（ops/s）、packing 成本占比、阈值建议（多大 batch 才划算）。

- [ ] **P3-2：把 BMMA 作为可插拔后端接入（建议插入点）**
  - **推荐插入点**：在 **P1-1（Batch-3A 接主路径）之后**。
    - 理由：Batch-3A 天然形成“同 cid 多 world”的 world-group，更容易摊平 b1 packing。
    - 关键前提：需要 **world 列矩阵 packing 的“载体稳定”**（world-group 固定、布局/对齐可复用）。
      否则每次检查都要在线 pack/transpose，packing 成本会吞掉 BMMA 的计算收益。
  - 运行时 gating：
    - `device_cc >= 80` 且 `world_group >= 8` 且 `K(域宽)` 足够大时启用；
    - 否则回退到现有 SIMT 位运算后端。
  - 说明：BMMA 不应替换主线默认后端；先以可控实验后端上线。

## 4. 最小回归清单（每次合并前必跑）

- `python3 tests/python/batch_test_v2.py --tier=0`
- `python3 tests/python/batch_test_v3.py --tier=0`（CPIM CPU vs CPIM GPU vs OR-Tools CP）
- `tests/cpp/test_stage2_persistent.cpp` / `tests/cpp/test_batch3a.cpp`（按需）
