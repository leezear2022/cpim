# Batch AC-GPU 扁平化任务队列备忘（Probe × GAC 合并调度）

> 目标读者：实现 Batch‑3A（异步调度）/Phase4+ 性能优化的开发者  
> 平台假设：Jetson Orin（SM 数量少、UMA、cooperative grid 可驻留 blocks 上限较小）  
> 关联文档：`docs/planning/BATCH_AC_GPU_COMPREHENSIVE_DESIGN_V2.md`（Phase 5/6）

---

## 1. 背景：为什么要“扁平化”

Batch‑2 的常见实现是两层嵌套：

- 外层：一个 block（或一个 world）处理一个 `ProbeTask (X=a)`，probe 之间按批次并行/串行
- 内层：`RunGACToFixpoint` 用 frontier（bitmap/queue）驱动，迭代直到不动点

问题在于：probe 的收敛轮数差异很大（秒失败 vs 长传播），导致 **负载不均衡**；并且在 Jetson 上想用 cooperative 做跨 block 同步会被“可驻留 blocks 上限”卡住。

“扁平化”的核心思想：把两层工作统一成“一维任务流”，用 GPU 端 work‑stealing 动态调度，让资源跟着热点走。

---

## 2. 正确性约束：为什么不能只用 `<子任务名, cid>`

把任务写成 `<子任务名, 约束名(cid)>` 并随意调度会破坏 `RunGACToFixpoint` 的语义，因为：

- AC/GAC 到不动点依赖 **域收缩的因果链**（删值触发邻接约束再检查）
- 传播是“混沌迭代”（chaotic iteration）：允许乱序，但必须在同一 world 的域上闭包收敛

因此，**任务必须绑定 world（probe 的私有域）**：

- 最小正确任务：`<world_id, cid>`
- 更适合 Jetson 的高效任务：`<cid, world_mask>`（同一约束聚合多个 world）

只要每个 world 的域只做“单调删值”（不回填），并且对约束的检查/入队是幂等的（可重复），则乱序调度不会改变最终不动点，只会影响性能。

---

## 3. 任务粒度方案对比

### 3.1 方案 A：细粒度 `<world_id, cid>`

- **优点**：实现最直观；易复用现有 `ExecuteConstraintCheck_BpC`（改成传入 world 的 `bitDom/dom_size` 指针即可）
- **缺点**：bitSup 访问复用差（同一 cid 可能在不同 block/时刻反复加载）；全局原子操作频繁（push/pop 次数多）

适合：作为“扁平化可行性/正确性”的第一版原型。

### 3.2 方案 B（推荐）：聚合 `<cid, world_mask>`

一个任务代表“对同一个约束 `cid`，同时为多个 world 执行检查”：

- **优点**：同一 `cid` 的 bitSup 更容易在同一个 SM/同一个 block 内复用（纹理/L2/shared）；push/pop 次数显著减少；天然适合 **约束亲和性**（block↔约束范围软绑定）
- **缺点**：需要改造约束检查内核：要么“一个 block 顺序处理 mask 中多个 world”，要么做“warp‑per‑world”并行处理

适合：Jetson Orin 的主力路线（省带宽、抗负载不均衡）。

---

## 4. 推荐扁平化设计：`<cid, world_mask>` 聚合队列

### 4.1 数据结构（概念级）

**WorldWorkspace（每个 world 一份私有域）**

- `bitDom[w][var][word]`：域位图（或 Phase4 的 CoW/Delta）
- `dom_size[w][var]`：域大小（可延迟更新）
- `inconsistent[w]`：DWO 标记（true 后可跳过重计算，但仍需清理队列位）

**TaskQueue（全局共享，按约束聚合）**

- `constraint_masks[cid] : u32`：第 `k` 位表示 world `k` 需要检查 `cid`
- `active_chunks[chunk] : u32`：chunk=cid/32；bit 表示该 chunk 内哪些 cid 可能非空

> 说明：`active_chunks` 只是加速索引，**权威数据是 `constraint_masks[cid]`**。允许 active 位“虚报”，但不能漏报。

### 4.2 入队（push）

当某个 world 的变量域发生删值，需要把其邻接约束入队：

1. `constraint_masks[cid] |= (1u << world)`
2. `active_chunks[cid/32] |= (1u << (cid%32))`

初始入队（一个 probe 开始）：

- 若 snapshot 已 AC：只 push `var` 的邻接约束（邻域激活）
- 否则：push 全部约束（安全回退）

### 4.3 出队（pop，work‑stealing）

持久线程块循环执行：

1. 先扫自己的亲和 chunk 范围（提高缓存命中/减少争抢）
2. 再全局窃取（避免饥饿）

Pop 的关键操作：

- 对 `active_chunks[chunk]` 用 `atomicExch(&..., 0)` 抢占一批候选 cid 位
- 在该 chunk 内逐位尝试 `mask = atomicExch(&constraint_masks[cid], 0)`，若 `mask!=0` 则得到任务 `{cid, mask}`
- 若提前返回，需要把未处理的候选位 `remaining_mask` 用 `atomicOr` 还回 `active_chunks[chunk]`

这种设计保证：

- 不会漏任务：生产者随时 `atomicOr`，就算消费者刚 `Exch` 清零，后续 OR 仍会重新点亮
- 允许重复/过期任务：最多浪费计算，不影响收敛正确性

### 4.4 处理任务（Process `{cid, world_mask}`）

对 `world_mask` 的两种实现路线：

- **简单版（先跑通）**：一个 block 顺序处理 mask 中每个 world（共享同一段 shared 临时缓冲）
- **高性能版（推荐）**：`warp‑per‑world`（或 `warp‑per‑world + warp‑per‑word`）  
  - 一个 block 处理一个 `cid`  
  - 多个 warp 分别负责不同 world（mask 中最多 8/16 个 world，取决于 block 的 warps 数）  
  - bitSup 的访问在同一 block 内天然复用（纹理/L2/shared 都收益）

处理逻辑（对每个 world）：

1. 若 `inconsistent[world]==true`：跳过计算（但视情况清理队列位/统计）
2. 执行约束检查（删值）
3. 若 `x/y` 域发生变化：把 `x/y` 的邻接约束 push 回队列（即 AC 的增量传播）
4. 若出现 DWO：置 `inconsistent[world]=true`，该 world 后续计算可短路

### 4.5 聚合任务的额外开销与收益边界（需要可观测与可回退）

聚合队列的关键收益来自“同一 `cid` 的 bitSup 复用 + 降低 push/pop 次数”，但它也会引入额外开销。实践上建议把聚合做成“可观测、可自适应开关”的策略，而不是一把梭。

#### 4.5.1 聚合不等于显式重排（避免高成本）

不推荐实现那种“先收集所有 `<world_id,cid>` 再 sort/group by cid”的显式重排，原因是它会额外引入：

- 任务列表写入/读回的带宽压力（Jetson 上更敏感）
- 排序/扫描/前缀和等全局同步成本

本文推荐的聚合方式是 **mailbox 聚合**：

- producer：`atomicOr(constraint_masks[cid], 1<<world)`（并点亮 `active_chunks`）
- consumer：`mask = atomicExch(constraint_masks[cid], 0)` 取走一批 worlds

这会改变调度顺序（乱序），但不会产生“显式重排/排序”的额外成本。

#### 4.5.2 聚合模式的主要额外开销来源

- **push 端原子争用**：多个 world 同时给同一个 `cid` 做 `atomicOr` 时会冲突（约束热点越强越明显）
- **pop 端扫描开销**：即便两级位图，仍然存在 `active_chunks` 的扫描与 `world_mask` 的解包（`ffs/popcount`）
- **重复/过期任务**：允许“虚报”会带来空转检查（安全但浪费算力）；需要用统计判断是否划算
- **简单实现的串行化**：若采用“一个 block 顺序处理 mask 中多个 world”，可能降低并行度；需要用 `warp‑per‑world` 把收益兑现

#### 4.5.3 建议统计指标（实现自适应开关的最小集合）

建议每个 batch（或每 N 个任务）在 device 侧累积计数，batch 结束后由 host 打印/决策：

- `task_pop_count`：成功 pop 到 `{cid,mask}` 的次数
- `task_pop_fail_count`：pop 失败/空转的次数（反映队列稀疏与终止检测成本）
- `mask_popcount_sum`：累计 `popcount(world_mask)`（衡量聚合度）
- `mask_multiworld_count`：`popcount(mask)>=2` 的任务数（衡量“真聚合”比例）
- `push_count`：push 次数（入队压力）
- `push_collision_count`：`old_mask != 0` 的 `atomicOr` 次数（可作为原子争用/热点粗指标）

核心派生指标（host 侧计算）：

- `avg_worlds_per_task = mask_popcount_sum / task_pop_count`
- `multiworld_ratio = mask_multiworld_count / task_pop_count`
- `pop_fail_ratio = task_pop_fail_count / (task_pop_count + task_pop_fail_count)`
- `push_collision_ratio = push_collision_count / push_count`

#### 4.5.4 自适应开关与回退策略（建议带滞回）

经验上，当聚合度不足时，聚合队列会退化成“额外原子 + 额外扫描”，此时应回退到非聚合方案（例如 Batch‑2：block=worker 拉 `ProbeTask`，每 world 内部仍用 bitmap frontier 跑到不动点）。

建议的启停阈值（先用经验值，后续用 profiling 校准）：

- **启用聚合（满足其一即可）**：
  - `avg_worlds_per_task >= 1.5`，或
  - `multiworld_ratio >= 0.30`
- **关闭聚合（满足其一即可）**：
  - `avg_worlds_per_task < 1.3` 且 `pop_fail_ratio` 偏高（例如 `>0.4`），或
  - `push_collision_ratio` 极高（例如 `>0.8`）且吞吐没有提升

实现建议：

- 用“滞回”避免抖动：例如连续 2-3 个 batch 达到关闭条件才回退；连续 2-3 个 batch 达到开启条件才启用
- 允许 `active_chunks` “虚报”：减少维护成本，但要用 `pop_fail_ratio` 监控空转扫描是否过高
- 简单实现先只做“聚合/非聚合二选一”；更细的策略（限制每次处理的 worlds 上限、剩余 mask 回插）可在稳定后再加

---

## 5. 终止检测（全局收敛/退出条件）

推荐先用“简单可靠”的全局判定：

- `in_flight`：当前正在处理的任务数（pop 成功时 `++`，处理结束 `--`）
- 当 `PopTask` 失败（没有任务）时：
  - 若 `in_flight==0` 且 `active_chunks[]` 全 0，则全局队列空，所有 world 均已收敛或已 DWO，可退出
  - 否则继续循环（可加退避：`__nanosleep()`/固定次数空转后再扫）

优化版（后续再做）：维护 `active_chunk_nonzero_count`（chunk 从 0→非0 时 `++`，非0→0 时 `--`），避免频繁扫描 `active_chunks[]`。

---

## 6. Jetson Orin 适配要点（与 cooperative 解耦）

- **不依赖 cooperative launch**：该扁平化队列使用普通 kernel launch + 原子即可；避免 Jetson 上 cooperative “可驻留 blocks 上限”导致的硬约束。
- **持久线程块数量 K 的选择**：
  - 以 occupancy 计算的“推荐并发 blocks”为下界（例如 `max_active_blocks_per_sm * num_sms`）
  - 实际 K 可以略高（普通 launch 会排队调度），但不宜过高以免原子争抢与 L2 抖动
- **只读数据优化**：
  - `bitSupData/texObj_BitSup`、`subscription`、`constraint_scopes` 建议 `cudaMemAdviseSetReadMostly`
  - Jetson UMA（`concurrentManagedAccess==0`）避免盲目 `cudaMemPrefetchAsync`

---

## 7. 渐进落地建议（从易到难）

1. **先实现方案 A（`<world_id,cid>`）**：快速验证“扁平化 + 终止检测”正确性
2. **再切换到方案 B（`<cid,world_mask>`）**：降低原子开销 + 提升 bitSup 复用
3. **再上 `warp‑per‑world` 约束检查**：把聚合任务真正变成吞吐优势
4. **最后叠加 Phase4 的 CoW/Delta 存储**：降低 `B×全域复制` 的内存压力

验收建议：

- 正确性：与 CPU（AC3bit/SAC baseline）逐实例对照“删值一致性/失败 probe 集合一致”
- 性能：probes/s、`avg_worlds_per_task`（聚合度）、`pop_fail_ratio`、bitSup 访问带宽、原子冲突率（可用 Nsight/自定义计数）
