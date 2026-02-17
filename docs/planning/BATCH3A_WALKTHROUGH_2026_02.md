---
status: active
updated: 2026-02-07
---

# Batch-3A Walkthrough（核心思想 / 算法 / 代码实现）

> 目标读者：第一次接手 Batch-3A 代码，想快速建立“为什么这样做、主循环怎么跑、代码从哪里看起”的完整心智模型。

相关文档：
- 复盘（为什么历史版本慢）：`docs/planning/BATCH3A_POSTMORTEM_2026_01.md`
- 队列版设计（Dynamic Submission）：`docs/planning/BATCH3A_DYNAMIC_SUBMISSION_QUEUE_DESIGN.md`
- 抢救备忘录（当前瓶颈与下一步）：`docs/planning/BATCH3A_RESCUE_MEMO_2026_02.md`

---

## 1. 先讲核心思想：Batch-3A 到底在“聚合”什么？

Batch-3A 的对象不是“单个 probe”，而是**一批 worlds**（每个 world 对应一个 probe：`var=value`）。

- Stage2 的常见做法：以 probe 为粒度逐个推进传播。
- Batch-3A 的目标：把“同一条约束在多个 worlds 上的检查”合并，减少重复加载与重复调度。

Batch-3A 的关键聚合单元是：
- `<cid, world_mask>`
- 含义：约束 `cid` 需要在 `world_mask` 指定的若干 worlds 上执行检查。

这就是“Constraint Aggregation”：
- 按约束聚合工作（而不是按 world 分散工作）
- 一次加载 `bitSup(cid)`，让多个 worlds 共享

---

## 2. 算法不变量（理解正确性的关键）

### 2.1 World 写入互斥

Batch-3A 采用 block 级 world 分片：每个 block 独占一段 worlds，避免多个 block 并发写同一 world 的域状态。

落点：`src/solver/gpu/GModel.cu` 的 `Batch3AKernel_MultiBlock()`。

### 2.2 Soundness 红线

遇到预算/溢出/未收敛时，语义是 `UNKNOWN`，不能误删。

实现上用 `active_world_mask` 体现：
- 完成（收敛或 DWO）的 world 清 bit
- 未完成 world 保留 bit，最终由 host 侧收集为 UNKNOWN

落点：
- device finalize：`Batch3AKernel_MultiBlock()`
- host 收集：`Batch3AManager::CollectResults()`

### 2.3 稀疏驱动而非全量扫描

当前实现核心是 Dynamic Submission（结尾提交）：
- 初始只把 probe 变量邻接约束入队
- 每次删值后，再把受影响变量的邻接约束提交到下一轮队列

避免了历史版本“每轮扫全体约束”的固定成本。

落点：
- `Batch3AEnqueueConstraintToNextQueue()`
- `Batch3AEnqueueVarToNextQueue()`

---

## 3. 代码阅读主线（建议顺序）

1) `include/solver/gpu/batch_probe_manager.h`
- `Batch3AControl`：所有 kernel 所需指针与配置（world、队列、mask、统计）
- `Batch3AManager`：host 端执行编排

2) `src/solver/gpu/batch_probe_manager.cu`
- `AllocateMemory()`：分配 world workspace、A/B 队列、frontier mask
- `Execute()`：按 `max_worlds_` 分批执行
- `LaunchBatch3AKernel()`：填 control、清队列状态、launch
- `CollectResults()`：输出 failed / unknown

3) `src/solver/gpu/GModel.cu`
- `Batch3AKernel_MultiBlock()`：主 kernel
- `ExecuteConstraintCheck_Aggregated_*()`：三种 mapping 的约束检查算子
- `LaunchBatch3AKernelWrapper()`：计算 `G`、分片、shared memory

4) `src/solver/gpu/GModelSolver.cu`
- `EnforceSAC3()` 中 Batch-3A 的 gating 与回退策略

---

## 4. 算法流程（从一次 Execute 视角）

### Phase A：Host 准备（Manager）

`Batch3AManager::Execute()`：
- 输入是一批 probe tasks
- 若任务数 > `max_worlds_`（默认 32），按批切分
- 第一批执行前 `SaveSnapshot()` 保存当前域快照

说明：world 上限 32 来自 `u32 world_mask`。

### Phase B：Kernel Phase0（device 初始化）

`Batch3AKernel_MultiBlock()` 的 Phase0 并行完成：
- snapshot 恢复到每个 world 私有域
- 重置 workspace 状态与 `results[w]=true`
- 应用 singleton assign（把 probe 变量收缩到单值）
- 将 probe 变量邻接约束 enqueue 到队列 A

这一步是从历史 host 初始化下沉来的，主要为了去掉 host 端逐 world memcpy/memset 开销。

### Phase C：双队列 worklist 主循环（A/B 轮换）

每一轮：
1. 从当前队列取 `cid`
2. 通过 `mask_cur[cid]` 得到本轮需要处理的 local worlds
3. 加载该 `cid` 的 `bitSup` 到 shared
4. 执行约束检查（mapping 0/1/2 三选一）
5. 若删值，调用 `Batch3AEnqueueVarToNextQueue()` 把邻接约束提交到 next 队列
6. 当前队列耗尽后：
- next 为空则收敛
- next 非空则 swap A/B 进入下一轮

若队列溢出 `overflow_flag=1`，该 block 提前退出并保留 UNKNOWN 语义。

### Phase D：收尾与结果回传

device 收尾：
- DWO 或收敛 world 会从 `active_world_mask` 清掉
- 未收敛 world 保留 bit（UNKNOWN）

host `CollectResults()`：
- `active_world_mask` 仍置位 => UNKNOWN
- `results[w]==false` => failed（DWO）
- 否则视为通过

---

## 5. 三种 mapping（算子形态）

`Batch3ACheckMapping`：
- `kWarpPerWorld`（0）：每个 warp 处理一个 world，当前默认基线
- `kSubwarpPerWorld`（1）：一个 warp 切多个 subwarp 并行处理多个 worlds
- `kWarpPerWordLaneWorld`（2）：warp-per-word + lane-per-world，配合 shared dom packing

入口参数：
- `--batch3a_check_mapping`
- `--batch3a_subwarp_size`
- `--batch3a_worlds_per_block`
- `--batch3a_shmem_padding`

配置落点：`apps/sac_benchmark.cpp` 的 `ConfigureBatch3AManager()`。

---

## 6. 在 SAC3 里的接入与回退策略

`GModelSolver::EnforceSAC3()` 中，Batch-3A 是可选加速器（默认关闭）：

触发条件（简化）：
- `batch3a_config_.enabled == true`
- 当前批任务规模在 `[min_worlds, max_worlds]`
- 非 deferred 批次
- `IsSuitableForBatch3A()` 为 true
- NSAC mask 未启用（启用时强制回退 Stage2，保证语义一致）

如果条件不满足，走 Stage1/Stage2 原路径。

---

## 7. 关键数据结构速记

`Batch3AControl`（`include/solver/gpu/batch_probe_manager.h`）里最关键的几组字段：

- world 维度：`num_worlds`、`world_probes`、`workspaces`
- 快照：`domain_snapshot`、`dom_size_snapshot`
- 队列：`block_cid_queue_A/B`、`block_queue_tail_A/B`、`block_overflow`
- frontier mask：`block_frontier_mask_A/B`
- 全局状态：`active_world_mask`、`global_iteration`
- 调参：`check_mapping`、`subwarp_size`、`requested_worlds_per_block`、`shmem_padding`

可以把它理解为“host 与 kernel 的共享控制块”。

---

## 8. 如何快速验证你读懂了

### 8.1 正确性对照（Batch3A vs Stage2）

```bash
./build/test_batch3a --input=tests/data/bench/queens-4_ext.xml --num_probes=10
```

该测试会做结果对齐，并打印聚合潜力分析。

### 8.2 吞吐对照（Stage2 vs Batch3A）

```bash
./build/sac_benchmark --input=tests/data/bench/queens-4_ext.xml --mode=stage2 --num_probes=32
./build/sac_benchmark --input=tests/data/bench/queens-4_ext.xml --mode=batch3a --num_probes=32
```

若要批量扫参数，用：`tests/python/batch_batch3a_microbench.py`。

---

## 9. 目前实现的现实边界（你要有预期）

结合当前代码与已有数据，Batch-3A 目前仍是“可消融、可回退”的实验路径，而非默认主路径：
- 在 Orin 上相对 Stage2 仍有明显回退（详见 rescue/postmortem 文档）
- 但 Dynamic Submission + Phase0 device 初始化已经显著缩小回退

因此建议心智模型是：
- **语义正确优先**（UNKNOWN 兜底）
- **以实验验证为驱动**（mapping/G/padding 与实例分布强相关）
- **主线保留 Stage2 回退**（工程稳定性优先）

