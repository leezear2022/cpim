---
status: active
updated: 2026-02-07
---

# Batch‑3A「抢救」备忘录（2026-02）

> 目的：结合“算法精神（sound + 利用稀疏性 + 尽量 device 端闭环）”与现有代码，
> 把 Batch‑3A（Constraint Aggregation）当前 **所有已知/可能的不足**系统列出来，
> 并记录最近两次关键改动（Dynamic Submission 队列版、Phase0 device 初始化）的**问题→解法→提升幅度**，
> 便于后续逐个击破。

相关主文档：
- 复盘（扫描版为何结构性慢）：`docs/planning/BATCH3A_POSTMORTEM_2026_01.md`
- 设计/实现（Dynamic Submission 队列版）：`docs/planning/BATCH3A_DYNAMIC_SUBMISSION_QUEUE_DESIGN.md`

---

## 0. 算法精神（本备忘录的评估标准）

1) **Soundness 红线**：任何 overflow / budget / early stop 只能得到 `UNKNOWN`，不得误删（宁可少删，不许多删）。
2) **显式利用稀疏性**：成本应尽量与“活跃约束/真实删值传播”成正比，而不是与 `num_cons` 成正比。
3) **尽量 device 端闭环**：host 只负责提交任务、启动一次（或少次）kernel；初始化/清零/搬运尽量融合到 device Phase0，
   或通过 epoch/lazy-clear 避免全量清零。
4) **可消融与可回退**：任何优化都必须能参数化打开/关闭，保持 Stage2 可回退基线。

---

## 1. 当前实现概览（便于定位）

### 1.1 Host/Manager（分批 + launch）

- 批处理框架：`src/solver/gpu/batch_probe_manager.cu`
  - `Batch3AManager::Execute()`：按 `max_worlds_<=32` 分批执行
  - `Batch3AManager::SaveSnapshot()`：保存 snapshot（bitDom + dom_size）
  - `Batch3AManager::LaunchBatch3AKernel()`：拷任务、清队列/掩码、填 control、launch、同步

### 1.2 Device/kernel（world 分片 + 双队列 worklist）

- Kernel：`src/solver/gpu/GModel.cu`
  - `Batch3AKernel_MultiBlock()`：每个 block 处理一段 worlds（互斥写入），内部用 A/B 双队列推进传播
  - `Batch3AEnqueueConstraintToNextQueue()` / `Batch3AEnqueueVarToNextQueue()`：Dynamic Submission 原语

---

## 2. 已解决的两次关键瓶颈（问题→解法→提升）

### 2.1 扫描版的“开头扫全约束”（杀手级）→ Dynamic Submission 队列版

**问题（扫描版）**：
- Kernel 每轮 `threadIdx.x==0` 全量扫描 `cid=0..num_cons-1` 构建 `local_task_cids/local_task_masks`
- 成本 `O(num_cons * worlds_per_block)`，并且 multi‑block 会重复扫描
- 完全无法利用稀疏性；会把 mapping=2 的 shared packing/bitGEMM 原型收益淹没

**解法（队列版）**：
- 取消“开头扫”，改为 **结尾提交**：
  - 初始只 enqueue probe var 的邻接约束
  - 每次约束检查若删值，则对删值变量的 subscription enqueue 到 *next* 队列
  - 通过 per‑cid `world_mask` 聚合 `<cid, world_mask>`，用 A/B 双队列轮换推进

**提交**：`ce18a6b`（`feat(sac-gpu): Batch-3A 动态提交队列版`）

**实现落点**：
- `src/solver/gpu/GModel.cu`：`Batch3AKernel_MultiBlock`（worklist）+ enqueue 原语
- `src/solver/gpu/batch_probe_manager.cu`：增加 per‑block 队列/mask 缓冲区并在 launch 前清零

**提升幅度（已观测）**：
- 在 perf suite / mapping=2 上，去掉扫描后整体仍慢于 Stage2：
  - 扫描版基线（历史）：`speedup_vs_stage2≈0.03–0.11`
  - 队列版（Phase0 下沉前）：`out/batch3a_queue_vs_stage2_perf_10min.csv` 中 `speedup_vs_stage2≈0.03–0.12`
- 解释：Dynamic Submission **确实干掉了 kernel 内的结构性串行扫**，但当时整体仍被 host 框架成本淹没（见 3.2）。

> 结论：Dynamic Submission 是“必要条件”，但不是“充分条件”。

### 2.2 host 侧 InitializeWorlds 逐 world 初始化（纯框架成本）→ Phase0 下沉到 device

**问题（队列版初版仍然存在）**：
- `InitializeWorlds()` 在 host 侧对每个 world 做多次 CUDA API：
  - D2D memcpy：恢复 snapshot 到 `ws->bitDom/ws->d_cur_dom_size`
  - memset：清 `ws->frontier_A/B`（队列版实际上不再依赖）
  - 还会 `cudaDeviceSynchronize()`
- `--num-probes=256` 时 batch 会拆成 8 份（world 上限 32），这段固定成本会被放大 8 倍

**解法**：
- 把 snapshot restore + world 状态初始化下沉到 `Batch3AKernel_MultiBlock` 的 Phase0（device 并行）
- `Batch3AManager::Execute()` 跳过 `InitializeWorlds()`（host 不再逐 world memcpy/memset）
- 队列版不再依赖 `ws->frontier_A/B`，Phase0 **不清零** frontier bitmap（避免 `O(num_cons)` 纯开销）

**提交**：`7737389`（`feat(sac-gpu): Batch-3A Phase0 下沉 snapshot 初始化`）

**提升幅度（smoke 已观测）**：
- `out/batch3a_queue_phase0init_smoke.csv`（perf suite 取 3 个实例，mapping=2，对照 Stage2）：
  - `speedup_vs_stage2≈0.17–0.26`
  - 相比 Phase0 下沉前的 `0.03–0.12`，回退显著收敛（但仍慢于 Stage2）

> 结论：host 初始化是“决定性框架瓶颈之一”，下沉能显著改善；但仍有更大的固定成本（见 3.1/3.2）。

---

## 3. 仍然存在的（可能的）不足清单（按优先级）

> 说明：这些是“可能不足”，其中一些需要 microbench/trace/Nsight 才能定责。
> 这里先把所有嫌疑点列全，后续逐项验证与止损。

### 3.1 每个 batch 仍要清零巨大的 per‑constraint mask（忽视稀疏性）

**现状**：`Batch3AManager::LaunchBatch3AKernel()` 每次都做：

- `cudaMemset(d_block_frontier_mask_A_, 0, mask_bytes)`
- `cudaMemset(d_block_frontier_mask_B_, 0, mask_bytes)`

其中 `mask_bytes = kMaxBatch3ABlocks * num_cons * sizeof(u32)`，且 A/B 两份都清。

**为什么这是“固定大头”**：
- 成本与 `num_cons` 成正比，和“本轮实际活跃 cid 数”无关
- 还乘上 `kMaxBatch3ABlocks`（即使实际 blocks 更少也照清）

**算法精神冲突点**：这是“host 侧的全量清零”，和我们想要的“成本随稀疏性变化”相反。

**候选解法方向（后续逐项评估）**：
1) **只清实际 blocks**（不要用 `kMaxBatch3ABlocks` 清满 16 份）
2) **epoch/lazy-clear**：mask 从 “u32” 升级为 “(epoch,mask)” 或 “epoch + sparse touched list”
3) **稀疏 touched 清理**：记录本轮 touched cid 列表，仅清 touched；避免全量清零

### 3.2 过多的强制同步（把 memcpy/memset/kernel 完全串行化）

`Batch3AManager::LaunchBatch3AKernel()` 当前有：
- launch 前 `cudaDeviceSynchronize()`
- launch 后 `cudaDeviceSynchronize()`

**为什么重要**：
- 这些同步会让 H2D memcpy、mask 清零（`cudaMemset`）与 kernel 彻底串行，无法 overlap；
- 在 `--num-probes` 被拆成多个 batch 时，这个同步成本会被倍增。

**候选解法方向**：
1) 移除 launch 前 `cudaDeviceSynchronize()`（默认 stream 的顺序性足够保证 `cudaMemcpy/cudaMemset → kernel` 的依赖）
2) 引入 stream + `cudaMemsetAsync/cudaMemcpyAsync`，用单点 `cudaStreamSynchronize` 收敛同步点

### 3.3 32‑world 上限导致多 batch（固定成本被“乘法放大”）

**现状**：
- Batch‑3A 以 `u32 world_mask` 聚合，天然上限 32 worlds；
- `--num-probes=256` 会拆成 8 个 batch（每个 batch 都要：H2D 拷任务 + 清 mask/队列 + 同步 + kernel）。

**算法精神冲突点**：
我们希望“摊平开销、一次 launch 处理尽量多的工作”，但 32‑world 天花板让 Batch‑3A 很难像 Stage2 persistent 那样摊平。

**候选解法方向**（从轻到重）：
1) 轻量：microbench/实际调用里优先用 `--num-probes<=32` 的实验配置做 kernel‑only characterization（避免框架成本被倍增）
2) 中量：把一次 preprocess 的 probes 做“分组重排”，尽量让同一 batch 内 `world_mask popcount` 更高（提高聚合度来摊平开销）
3) 重构：扩展到 `u64 world_mask`（<=64 worlds）或 “多段 world_mask（tile）”，但会牵动队列原语与数据结构

### 3.4 per‑cid bitSup 装载与 pack/unpack：浅传播下难以摊平

**现状**（在 kernel 内每处理一个 cid）：
1) 从 global 读取 `bitSupData[cid]` 到 shared（一次加载，多 world 共享）
2) mapping=2 时还要做 shared packing（AoS→SoA）+ unpack（SoA→AoS）

**为什么会慢**：
- 如果 `world_mask popcount` 低、或迭代轮次浅，“每个 cid 的固定搬运成本”占比会非常高；
- 即使算子本体（AND/POPC/bitGEMM）变快，也很容易被搬运/同步吞掉。

**候选解法方向**：
1) 做“成本归因”：用 CUDA event 把 Phase0 / 清 mask / cid 循环（含 bitSup load）分别计时，明确谁是大头
2) 只在“高聚合度/深传播”的 hard cases 才启用 mapping=2（否则直接回退更轻的 mapping 或 Stage2）

### 3.5 enqueue 原子热点与重复项：队列会“变脏”，导致无效工作与 overflow 风险

**现状**：
- enqueue 依赖 `atomicOr(mask_next[cid], bit)` + `atomicAdd(tail_next)`；
- 同一轮/不同线程/不同 worlds 会反复触发同一 cid 的 enqueue（虽然 `atomicOr` 能合并 mask，但 tail 侧仍可能被重复 push）。

**后果**：
- 原子热点（tail/mask）会在约束密集/删值多时放大；
- `queue_capacity=num_cons` 在重复 push 情况下并不一定够，导致 overflow → UNKNOWN（sound 但会损剪枝与吞吐）。

**候选解法方向**：
1) 让 “push 唯一性”更强：用 epoch/标记让同一 cid 在同一轮最多 push 一次（mask 合并即可）
2) 将 `queue_capacity` 从 `num_cons` 提升为 `k * num_cons`（短期救急，但会更吃内存/清零更贵）
3) 引入“稀疏 touched list + lazy clear”，同时也能解决 3.1 的清零成本

### 3.6 host 侧 `cudaMemset` 的范围过大（清了未用 blocks）

**现状**：
- mask 清零使用 `kMaxBatch3ABlocks * num_cons` 的固定大小（而实际 `num_blocks_` 往往更小）。

**候选解法方向**：
- 把清零范围改为 `num_blocks_ * num_cons`（A/B 两份同理），这是低风险、立竿见影的“减法”。

### 3.7 `cudaMemcpy`/`cudaMemset` 的调用数量仍多（可融合/可批量化）

即使不做 epoch/lazy-clear，仍可以做：
- 合并多个小 memset 为一个大 memset（如果内存布局连续）
- 用 stream + async，把 “H2D 拷 probe + 清队列/掩码” 组织成一段批处理，减少同步点

### 3.8 观测不足：缺少 “固定开销 vs 算子开销” 的同口径分解

当前 microbench 只看 end‑to‑end 时间（含 host 端清零/同步），但缺少以下关键字段：
- Phase0（restore+assign）耗时
- mask/queue 清零耗时
- kernel 处理 cid 的耗时（以及有效 cid 数/重复率）

> 结论：若不把时间分解出来，后续优化容易“摸黑调参”。

---

## 4. 建议的“逐个击破”顺序（低风险→高收益优先）

> 这部分不是强制路线，只是把“最可能快速降固定开销”的项放前面。

1) **缩小清零范围（3.6）**：先把 `kMaxBatch3ABlocks` 改为实际 `num_blocks_`
2) **去掉 launch 前同步（3.2）**：验证默认 stream 顺序性后移除 pre‑sync
3) **mask lazy‑clear（3.1 + 3.5）**：epoch/稀疏 touched list 二选一，目标是消灭 `O(num_cons)` 的 host 清零
4) **原子热点治理（3.5）**：减少重复 push（同轮同 cid 只 push 一次）
5) **按聚合度 gating mapping=2（3.4）**：避免在低 popcount/浅传播时硬上 pack/unpack

每做完一步，都用同口径 microbench 记录：
- `speedup_vs_stage2` 的分布是否整体上移
- 失败/overflow/unknown 是否上升（soundness 仍需守住）

---

## 5. 复现实验（建议保留同一口径 CSV 以便画曲线）

```bash
python3 tests/python/batch_batch3a_microbench.py \
  --suite=perf \
  --timeout=600 \
  --csv=out/batch3a_queue_vs_stage2_perf_10min.csv \
  --resume --flush-every=1 \
  --include-stage2=1 \
  --mappings=2 \
  --g-values=0,8,16,32 \
  --padding-values=0,1 \
  --num-probes=256 \
  --warmup=1 \
  --iterations=3
```

建议新增一个“Phase0 下沉后的全量 perf”文件名（避免 resume 复用旧数据）：
- `out/batch3a_queue_phase0init_vs_stage2_perf_10min.csv`
