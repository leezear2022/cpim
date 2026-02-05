---
status: active
updated: 2026-02-05
---

# Batch-3A：Dynamic Submission（PSTRds/PCTds 风格）队列版设计草案

> 目标：把 Batch‑3A 从“每轮扫描全体约束构建任务”改为 **队列驱动的动态提交**，在约束检查结束时提交下一批需要检查的约束，
> 以 **显式利用稀疏性**，并为后续 flatten（NSACQ/SACQ 的 constraint-level worklist）提供载体。

## 0. 背景与动机（来自复盘）

扫描版 Batch‑3A 的决定性瓶颈是：

- **Step1：`threadIdx.x==0` 单线程全量扫描 `cid=0..num_cons-1`**，构建 `local_task_cids/local_task_masks`；
- 多 block 会重复扫描；该成本与算子 mapping 无关，会把 bitGEMM/packing 的收益完全淹没；
- 详情见：`docs/planning/BATCH3A_POSTMORTEM_2026_01.md`。

本草案的核心改进：**取消“开头扫”，改为“结尾提交”**（Dynamic Submission），使成本与“真正活跃约束数”成正比。

## 0.1 实现状态（2026-02）

该草案已在代码中落地（队列版替换扫描版），关键代码锚点如下：

- Control/Manager：
  - `include/solver/gpu/batch_probe_manager.h`：`Batch3AControl` 新增 per-block 队列与 mask 字段
  - `src/solver/gpu/batch_probe_manager.cu`：
    - `Batch3AManager::AllocateMemory()`：分配队列/掩码缓冲区（`queue_capacity_=num_cons`）
    - `Batch3AManager::LaunchBatch3AKernel()`：launch 前 `cudaMemset` 清零 mask/queue_tail/overflow
- Device 侧队列原语 + kernel：
  - `src/solver/gpu/GModel.cu`：
    - `Batch3AEnqueueConstraintToNextQueue()` / `Batch3AEnqueueVarToNextQueue()`
    - `Batch3AKernel_MultiBlock`：双队列 A/B worklist 驱动（round-based）

> 注：当前版本仍保留 host 侧 `InitializeWorlds()` 的 snapshot 恢复（每批次）；后续可按 5.3 的建议，
> 把恢复/清零搬到 Phase0（device 内并行），进一步降低框架开销。

**初步对照结果（perf suite / mapping=2）**：
- 命令：`python3 tests/python/batch_batch3a_microbench.py --suite=perf --include-stage2=1 --mappings=2 ... --csv=out/batch3a_queue_vs_stage2_perf_10min.csv`
- 结论：`speedup_vs_stage2` 仍为 `0.03–0.12`（约 8×–30× 慢于 Stage2），未达到“可接受回退（<5×）/接近 1×”门槛。
- 含义：Dynamic Submission 解决了“开头扫全约束”的结构性问题，但 **仍不足以让 Batch‑3A 在现有样例上接近 Stage2**；
  下一步需要把 host 侧 `InitializeWorlds()` 的框架成本下沉到 device Phase0（见 5.3）。

## 1. 关键约束（必须守住）

- **World 写入互斥**：同一个 world 的域/前沿更新必须由一个 block 独占（否则会发生数据竞争，丢删值/不确定行为）。
  - 因此保留 **world 分片**：每个 block 只负责一段 `[block_world_start, block_world_start+block_world_count)` 的 worlds。
- **Soundness 红线**：任何 budget/溢出触发时只能标记 `UNKNOWN`，不得误删。
- **可回退**：运行时保持 `--sac_use_batch3a=false` 可回退 Stage2（不要求保留旧 Batch‑3A 扫描版实现）。

## 2. 总体思路（每个 block 一个 worklist）

把每个 block 的传播改为 “worklist（约束队列）驱动”：

- **队列元素**：`cid`（约束 id）
- **每个 cid 的聚合信息**：`world_mask`（本 block 局部 world 位，0..G-1），表示该 cid 需要在哪些 worlds 上检查
- **动态提交**：约束检查删除了变量域 → 遍历该变量的 subscription（邻接约束）→ 将这些约束 `enqueue` 到 *下一轮* 队列

### 2.1 为什么选“下一轮队列”（双队列）而不是单队列？

双队列（current/next）有两个实用优势：

- **实现简单且更安全**：读 current、写 next，天然避免 “push 与 pop 同队列的时序/可见性竞态”；
- **与现有 frontier_A/frontier_B 语义一致**：当前轮的激活来自上轮删值，删值产生的激活进入下一轮。

> 后续若需要更激进的 flatten，可升级为单队列 + 协作式调度，但建议先把双队列跑通并证明收益。

## 3. 数据结构草案（Batch3AControl 增补）

> 注：这里按“可实现/可跑通优先”，不追求最省内存。后续可用 `epoch+稀疏表` 优化清零成本。

对每个 block `b`（最多 16）分配：

- `u32* frontier_mask_A[b]`：长度 `num_cons`，每个 cid 的局部 `world_mask`
- `u32* frontier_mask_B[b]`：长度 `num_cons`
- `int* cid_queue_A[b]`：长度 `queue_capacity`（建议先取 `num_cons`）
- `int* cid_queue_B[b]`：长度 `queue_capacity`
- `int tail_A[b] / tail_B[b]`：当前写入位置（head 由线程 0 顺序推进）
- `int overflow_flag[b]`：溢出标记（触发后该 block 直接退出并保留 active_world_mask → UNKNOWN）

enqueue 原语（在 device 侧）：

```cpp
// local_w: [0,G)
old = atomicOr(&frontier_mask_B[cid], (1u << local_w));
if (old == 0) {
  pos = atomicAdd(&tail_B, 1);
  if (pos < queue_capacity) queue_B[pos] = cid;
  else overflow_flag = 1;
}
```

pop 原语（线程 0）：

```cpp
cid = queue_A[head++];
mask = atomicExch(&frontier_mask_A[cid], 0);
// mask==0 说明是重复/陈旧条目，可直接跳过
```

## 4. Kernel 伪代码（block 内 worklist 驱动）

```cpp
Phase0:
  - 对本 block 的 worlds：复制 snapshot → singleton assign → 初始 enqueue(var_id 的 subscription) 到 A

for (round = 0; round < max_rounds; ++round) {
  head = 0;
  while (head < tail_A) {
    cid = queue_A[head++];
    mask = atomicExch(mask_A[cid], 0);
    if (mask == 0) continue;

    load bitSup(cid) -> shared
    ExecuteConstraintCheck(cid, mask, block_world_start, block_world_count)
      - 若删值：对删值变量 x/y 做 enqueue 到 queue_B（mask_B）
      - 若 DWO：标记 ws->inconsistent_flag
  }

  if (tail_B == 0) break;  // 收敛
  swap(A, B); tail_B = 0;  // 进入下一轮
}

Finalize:
  - 若该 world DWO：清 active_world_mask bit
  - 若收敛：清 active_world_mask bit
  - 若溢出/round 达上限：保持 active_world_mask bit → UNKNOWN
```

## 5. 集成点（代码改动路径）

### 5.1 替换掉 kernel 的 “Step1 扫描构建任务”

- 现状：`Batch3AKernel_MultiBlock` 每轮 `threadIdx.x==0` 全量扫 `cid=0..num_cons-1`。
- 新版：移除该 Step1，改为 “处理 queue_A”。

### 5.2 传播提交：从 `PropagateVarToNextBitmap()` 改为 `EnqueueVarToNextQueue()`

现状（lane0）：

- `PropagateVarToNextBitmap(var, model, ws->frontier_B)`

新版（lane0）：

- `EnqueueVarToNextQueue(var, local_w, model, control, mask_B, queue_B, tail_B)`

### 5.3 初始化：Phase0 不再依赖 host 侧“逐 world memcpy/memset”

建议（可选但强烈推荐）：

- 把 `InitializeWorlds()` 的 snapshot restore + frontier 清零搬到 Phase0（device 内并行完成），
  这样能显著降低 host 侧 `cudaMemcpy/cudaMemset/cudaDeviceSynchronize` 的框架成本。

## 6. 观测与验收（最小闭环）

新增统计（每个 batch / 每个 block）：

- `queue_push_count / queue_pop_count / queue_max_len`
- `overflow_count`
- `rounds_executed`

验收：

- `tests/cpp/test_batch3a.cpp`（如仍保留）与 Stage2 结果一致；
- `python3 tests/python/batch_test_v2.py --tier=0` 结果不劣化（历史基线 8/12）；
- microbench：至少证明 “去掉全量扫描后，Batch‑3A 的 kernel time 不再被固定框架成本支配”。

## 7. 风险与后续方向

- **内存开销**：`2 * num_blocks * num_cons * sizeof(u32)` 的 mask 可能较大；必要时引入 `epoch+稀疏表`。
- **低聚合度**：若 `world_mask popcount` 长期很低，即使队列化也未必比 Stage2 更快（这属于问题本质，不是实现瑕疵）。
- **进一步 flatten**：若该队列版跑通且有收益，再考虑把任务单位从 `<cid, world_mask>` 推进到 `<cid, world_mask, word_range>`，
  或跨 probes 的 constraint-level flatten（Batch‑3D 方向）。
