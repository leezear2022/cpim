# Batch AC-GPU（Batch-2 / Batch-3）设计摘要：面向 Jetson Orin 的高并行与持久线程块

本文是给“其它模型/读者”快速理解用的 **自包含摘要**：在现有 Batch-1（时间维 batch）
基础上，如何进一步利用 GPU 高并行，设计 Batch-2（空间维 micro-batch，多世界并行）
以及 Batch-3（更高并行/更高吞吐）的版本，并明确 Jetson Orin（UMA + cooperative 限制）
下的约束与取舍。

## 0. 基本定义：Batch AC vs SAC

- **Batch AC（SAC-checking pass）**：给定当前域状态 `D`，对一批 `(X_i=a)` 的
  singleton 世界分别运行 AC 到不动点，输出每个 probe 是否产生 DWO（空域）。
- **SAC（SAC-closure）**：需要外层反复执行上述 pass，删值并再次检查，直到域不再变化。

结论：Batch AC 是实现 SAC 的关键算子，但不是 SAC 的完整闭包过程。

## 1. 现有基础（Batch-1）与瓶颈

Batch-1（时间维 batch）典型做法：
- 只保存 **一份**“基线域快照”（`bitDom + d_cur_dom_size`）。
- 在一个持久化 cooperative kernel 内：逐个取 probe → 恢复快照 → 单例赋值 → 运行 GAC 到不动点
  → 写回结果 → 继续下一个 probe。

优点：内存小、复用现有 GAC kernel 容易、很适合 Jetson UMA。  
瓶颈：probe 之间仍然 **串行**（虽然每个 probe 内部的传播是并行的），吞吐受限。

## 2. Jetson Orin 适配约束（决定 Batch-2/3 的形态）

1) **UMA（统一内存）**：CPU/GPU 共享物理内存，避免显式拷贝；但要避免 CPU 频繁触碰
   GPU 正在重度写的区域，否则会引发页迁移/同步抖动。

2) **cooperative launch 可驻留 grid 限制**：`cg::grid_group::sync()` 需要 cooperative launch；
   cooperative kernel 的可用 blocks 总数通常小（与你看到的 `max_grid_size` 同量级），这会直接限制：
   - Batch-2 同时并行的 world 数 `B`
   - 每个 world 能分到的 blocks 数 `blocks_per_world`

3) **持久线程块（persistent blocks）优先**：在 Jetson 上减少 kernel launch 与 CPU 参与更关键；
   但要警惕“全局同步（grid.sync）过多导致空转”。

## 3. Batch-2：空间维 micro-batch（多世界并行，world replication）

### 3.1 核心思想
一次性在 GPU 上同时维护 `B` 个独立世界（world），每个 world 都有自己的域与 frontier，
并行运行 AC 到不动点。并行性来自：
- world 维并行（多个 probe 同时跑）
- 约束/值 维并行（每个 world 内仍用现有 BpC 传播逻辑）

### 3.2 数据结构（最直接、最易落地的版本）

共享只读（所有 world 复用）：
- `bitSupData` / `constraint_scopes` / `subscription(CSR)`

每 world 私有（需要复制）：
- `bitDom_batch[B][bit_doms_int_size]`
- `dom_size_batch[B][num_vars]`
- `frontier_A_batch[B][bitmap_words]`
- `frontier_B_batch[B][bitmap_words]`
- `control_batch[B]`（`scanner_index / inconsistent / iterations / frontier_nonempty ...`）
- `result_ok[B]`（probe 是否一致）

内存粗估（仅域位图）：
```
bytes(bitDom_batch) ≈ B * num_vars * bit_dom_int_size * 4
```
在 Jetson 上，实际 `B` 往往先被 cooperative grid 限制卡住，而不是被显存卡住。

### 3.3 Kernel 组织（persistent + cooperative 的 world 分组）

推荐映射：
- `world_id = blockIdx.x / blocks_per_world`
- `local_block = blockIdx.x % blocks_per_world`
- 每个 world 的 blocks 共享该 world 的 `control/frontier/bitDom` 指针。

关键约束：
```
B * blocks_per_world <= max_grid_size  (cooperative 可驻留 blocks 上限)
```
因此：
- 若 `max_grid_size≈32`：常见配置是 `B=16, blocks_per_world=2` 或 `B=32, blocks_per_world=1`
- `blocks_per_world=1` 会降低单 world 内吞吐，但提高并发 probes 数

### 3.4 执行流程（每个 world 独立收敛）
对 world `w`：
1) 初始化：从 base 快照拷贝到 `bitDom_batch[w]` / `dom_size_batch[w]`
2) 施加单例赋值 `(var_id, value)`
3) 初始化 frontier：优先用 `subscription` 激活邻接约束（增量）；必要时可退化为全约束
4) GAC 到不动点：
   - 从 frontier 抢约束 id（word-level bitmap 扫描）
   - 执行 `ExecuteConstraintCheck_BpC`（复用现有逻辑，指针换成 world-local）
   - 推送邻接约束到 next frontier
   - 检测 DWO（`inconsistent_flag`）与 next frontier 为空（收敛）
5) 写回 `result_ok[w]`

### 3.5 优缺点
优点：
- 结构直观，复用现有 BpC kernel 代码成本最低
- 吞吐显著提升（多个 probe 同时跑），更接近“Batch-2”的目标

缺点（Jetson 视角）：
- cooperative grid 限制可能把 `B` 卡在 8~32 的量级
- 每个 probe 仍要做一次“基线域复制到 world”的初始化，有额外带宽成本
- world 之间如果用 `grid.sync()` 强制锁步，会导致收敛快的 world 空转

## 4. Batch-3：更高并行/更高吞吐（两条路线）

Batch-3 的目标是进一步减少 Batch-2 的空转与初始化开销，并提高算子复用度。

### 4.1 Batch-3A（工程优先）：跨 world 异步调度 + work stealing

核心思想：
- 不再让所有 world lockstep 同步迭代；改为“任务驱动”的异步推进
- persistent blocks 从全局队列抢任务，任务可定义为：
  - `(world_id, cid)`：处理 world 的某个约束
  - 或 `(world_id, bitmap_word)`：扫描并弹出若干 cid

数据结构变化：
- 仍然是 world replication（`bitDom_batch` 等不变）
- 增加全局任务队列/环形缓冲（设备端）与 per-world 的活跃标记

优点：
- 显著减少“部分 world 已收敛但仍等待同步”的空转
- 更贴合 Jetson（SM 少，空转代价更大）

难点：
- 需要可靠的设备端队列与终止检测（所有 world 都空队列才结束）
- Debug 复杂度上升

### 4.2 Batch-3B（算子极致）：world 位切片（world-SIMD），frontier 变成 world mask

核心思想：
- 把世界维度打包进一个 `u32 world_mask`（最多 32 个 world/warp）
- 让一次 revise/支持判定同时更新 32 个 world（更像 boolean bitGEMM）

关键重构（影响最大）：
- 域表示从“值 bitset”变为“world mask”：
  - `dom_mask[var][value] : u32` 表示哪些 world 里该 value 仍在域中
- frontier 从“约束 bitmap（cid set）”变为“每约束一个 world_mask”：
  - `frontier_mask_A[cid] : u32`

传播时：
- 只对 `active_mask = frontier_mask_A[cid]` 的 world 计算支持/删值
- 变更传播用 `atomicOr(frontier_mask_B[nbr_cid], changed_world_mask)`

优点：
- 复用 `bitSup` 行访问：一次加载可服务 32 个 world
- 算子层更接近 bitGEMM，上限最高

难点：
- 数据结构变动大，需要重新设计 `ExecuteConstraintCheck` 的内核形态
- 需要维护 per-world 的 DWO/域大小（可用增量计数或按需重算）

## 5. 推荐落地路线（现实可控）

1) **先做 Batch-2（B=8/16 起步）**：验证“并发 probes”带来的吞吐收益，并跑 TIER0/TIER1 正确性。
2) **再做 Batch-3A（异步调度）**：解决 Jetson 上 lockstep 空转，通常性价比最高。
3) **最后评估 Batch-3B（world-SIMD）**：作为长期/研究型优化方向，需求明确再投入。

## 6. 与现有框架的接口建议（保持可回退）

建议对外暴露统一入口（示意）：
- `RunBatchACPass(probes, micro_batch=B, mode=Batch2/Batch3A/Batch1_fallback)`
- 若设备不支持 cooperative 或资源不足：自动回退到 Batch-1 或单 probe（sequential）

并明确：
- Batch-2/3 只负责 SAC-checking pass；SAC-closure 的外层循环在 host 侧控制。

---

**一句话总结**  
Batch-2 = “复制 B 个世界并行跑 AC（持久线程块 + per-world frontier）”；  
Batch-3A = “不锁步，用任务队列跨世界偷活减少空转”；  
Batch-3B = “世界位切片，把 32 个世界塞进位运算，逼近 bitGEMM 上限”。  

