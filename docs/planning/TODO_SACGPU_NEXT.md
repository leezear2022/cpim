---
status: active
updated: 2026-01-16
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
- ⚠️ “真 NSAC”的 `allowed-constraints mask` 未落地（当前仍是“邻域激活”，但不做子图过滤）。
- ⚠️ Batch-3A（约束聚合）内核与 Manager 已有实现/测试，但未接入 solver 主路径。
- ❌ bitGEMM（lane→world 的内核形态 / DomSoA 数据布局）未落地。
- ❌ `bmma_sync(b1, AND+POPC)` 未落地（仅在计划文档中提及）。

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

- [ ] **P0-1d：失败概率优先（Failure Priority）**
  - 目标：高失败概率的值先 probe，快速产生删值。
  - 建议：先完成 P0-1c 并补齐统计闭环，再推进 P0-1d；优先做“软优先级（bucketed queue）”而非完整堆/优先队列。
  - 估计方法：
    - Cheap precheck 的支持计数
    - 历史 DWO 率（同变量其他值的失败率）
    - 邻域约束紧密度
  - 交付：
    - `ProbePool` 添加优先级队列
    - probe 入队时计算 `failure_score`
  - 验收：相同时间内 DWO 发现数量提升

- [ ] **P0-2：NSAC 的 `allowed-constraints mask`（真邻域子图）**
  - 目标：singleton test 的传播严格限制在 `Xi + N(Xi)` 诱导子图（NSACQ/NSAC）。
  - 交付：
    - CPU 端预计算 `allowed_cmask[focal_var][cid]`（位图 words）
    - GPU 端：frontier init / `PropagateVarToNextBitmap` 增加过滤（按 focal_var 选择 mask）
  - 验收：在同一 budget 下，平均 probe iters/work 下降；UNKNOWN 比例可控；结果 sound。

- [ ] **P0-3：统一观测入口（最小可用）**
  - 目标：能用同一套统计口径对比 Stage1/Stage2/SAC1/SAC3/（未来 Batch3A）。
  - 交付：
    - 至少支持：每批 probes 的 `num_probes / dwo / unknown / avg/p95 iters / time_ms`
    - Python 脚本输出 CSV（tier0 即可）。

### P1：主线增强（把长尾变可控、把并行度吃满）

- [ ] **P1-1：把 Batch-3A 接入 solver 主路径（可选加速器）**
  - 目标：在“欠饱和/负载不均衡”场景，允许用 Batch-3A（约束聚合）顶上。
  - 依赖：
    - **P0-1**（`UNKNOWN` 语义）：否则无法定义“budget hit → 不删 → 回退”的安全行为。
    - **P0-3**（统计基础设施）：否则无法做“欠饱和检测 → 启用/回退策略”的数据闭环。
  - 启用条件（建议先保守）：
    - `bitSup` 可放入 shared memory（已有判断）
    - `world_mask popcount` 均值/分位数达到阈值（避免聚合无收益）
  - 回退：不满足条件时回退 Stage2（Persistent Blocks）。
  - 验收：在长尾实例上 p99 时间下降或吞吐更稳定；结果一致。
  - 代码落点（建议）：
    - solver 侧入口：`src/solver/gpu/GModelSolver.cu`（SAC1/SAC3/MSAC 的 stage 选择点）
    - manager/kernel：`include/solver/gpu/batch_probe_manager.h`、`src/solver/gpu/batch_probe_manager.cu`、
      `src/solver/gpu/GModel.cu`（Batch3A wrapper/kernel）

- [ ] **P1-2：预算双阀门（per-probe + 外层 queue）工程化**
  - 目标：让 SAC3/MSAC 在难例上可控结束，并把 UNKNOWN 语义贯穿到统计与回退策略。
  - 交付：queue-level budget（max_total_probes/max_queue_len/max_requeue 等）。

- [ ] **P1-3：ProbePool 2.0（跨变量混 batch + 分桶）**
  - 目标：解决“单变量域小导致 GPU 欠饱和”；并对难 probe 降预算避免拖慢。
  - 交付：target batch size、简单分桶（按 dom_size/历史 iters）。

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
- `tests/cpp/test_stage2_persistent.cpp` / `tests/cpp/test_batch3a.cpp`（按需）
