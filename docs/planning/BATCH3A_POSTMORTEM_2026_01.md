---
status: active
updated: 2026-02-05
---

# Batch-3A（Constraint Aggregation）性能回退复盘（2026-01）

> 目的：把 Batch‑3A（含 P2-1/P2-2 路线）的 **“为什么慢、慢在哪里、能不能救、值不值得救”** 讲清楚，
> 并附上关键问题代码锚点，便于交给其他模型/同事复核与提出替代方案。

## 0. 一句话结论

在 Jetson Orin（UMA）+ 当前实现形态下，Batch‑3A（尤其 mapping=2 的 shared packing）对 Stage2（Batch‑2 Persistent）
出现 **结构性灾难回退**：在 perf/stress 样例上 `speedup_vs_stage2≈0.03–0.11`（约 9×–30× 慢），在大实例
`large-80/84` 上也只有 `≈0.028–0.042`（约 24×–36× 慢）。

这不是“再调 G/padding 就能救”的量级；瓶颈主要来自 **kernel 内单线程全量扫描构建任务** + **host 分批初始化/同步开销**，
算子层面的优化（G 参数化、padding）被框架成本完全淹没。

> **更新（2026-02）**：为消除 3.1 的“开头扫全约束”杀手级开销，已实现 Batch‑3A 的
> **Dynamic Submission（结尾提交）队列版**（见 `docs/planning/BATCH3A_DYNAMIC_SUBMISSION_QUEUE_DESIGN.md`），
> 将任务构建从“每轮扫描 `cid=0..num_cons-1`”替换为 “device 侧 `<cid, world_mask>` worklist + 双队列”。
> 初步对照（`out/batch3a_queue_vs_stage2_perf_10min.csv`）显示 mapping=2 仍为 `speedup_vs_stage2≈0.03–0.12`
>（8×–30× 慢于 Stage2），说明“开头扫”并非唯一决定性瓶颈；**host 侧分批初始化/同步**仍然是需要优先处理的框架成本。
> 本复盘仍保留用于解释 **为什么扫描版会结构性回退**，并作为后续优化（把初始化下沉到 device Phase0 等）的对照基线。
>
> **更新（2026-02-05）**：已将 `InitializeWorlds()` 的 snapshot restore + world 状态初始化下沉到
> `Batch3AKernel_MultiBlock` 的 Phase0（device 并行执行），并在 `Batch3AManager::Execute()` 中跳过
> host 侧逐 world 的 `cudaMemcpy/cudaMemset` 循环。smoke 对照（3 个 perf 样例，见
> `out/batch3a_queue_phase0init_smoke.csv`）显示 `speedup_vs_stage2≈0.17–0.26`（仍慢于 Stage2，但框架回退明显收敛）。
> 这验证了 3.2 的判断：host 初始化是主要瓶颈之一；后续若继续“救 Batch‑3A”，应优先把剩余 host 框架成本继续下沉到 device
>（例如把 task batching/拷贝进一步合并，或把更多统计/清零移到 kernel 内）。

## 1. 如何复现（同口径对照）

使用 microbench 工具（Stage2 baseline + Batch‑3A 对照）：

```bash
# perf：关键节点哨兵
python3 tests/python/batch_batch3a_microbench.py \
  --suite=perf --timeout=600 \
  --include-stage2=1 --mappings=2 \
  --g-values=0,8,16,32 --padding-values=0,1 \
  --num-probes=256 --warmup=1 --iterations=3 \
  --csv=out/batch3a_vs_stage2_perf_10min.csv --resume --flush-every=1

# stress：大实例门槛验证（会遇到 large-92 OOM）
python3 tests/python/batch_batch3a_microbench.py \
  --suite=stress --timeout=1200 \
  --include-stage2=1 --mappings=2 \
  --g-values=0,8,16,32 --padding-values=0,1 \
  --num-probes=256 --warmup=1 --iterations=3 \
  --csv=out/batch3a_vs_stage2_stress_20min.csv --resume --flush-every=1
```

CSV 关键列：
- `avg_time_ms` / `probes_per_sec`：绝对吞吐
- `stage2_avg_time_ms` / `speedup_vs_stage2`：对 Stage2 baseline 的同口径 speedup

## 2. 现象摘要（从数据到判断）

### 2.1 perf（浅传播为主）

`out/batch3a_vs_stage2_perf_10min.csv` 中 `mapping=2` 的 best‑of‑G 结果：所有实例 `speedup_vs_stage2 < 1.0`，
通常 0.03–0.11。

### 2.2 stress（大实例：large-80/84/92）

`out/batch3a_vs_stage2_stress_20min.csv` 中：
- `benchmarks/marc/large-80-unsat_ext.xml`：Stage2 约 22ms；Batch‑3A(mapping=2) 最好约 535ms（≈0.04）
- `benchmarks/marc/large-84-unsat_ext.xml`：Stage2 约 23ms；Batch‑3A(mapping=2) 最好约 547ms（≈0.042）
- `benchmarks/marc/large-92-unsat_ext.xml`：Stage2 构建模型阶段触发 CUDA OOM（见 `src/model/gmodel_adapter.cu:79`）

## 3. 主要瓶颈（代码锚点 + 原因）

> 下面只列“决定性瓶颈”。一些看起来“可疑”的点（如 shared bank conflict）在当前数据下并非主因，
> 因为即便修正 padding，整体仍是 10×～30× 回退。

### 3.1 Kernel 内“构建活跃约束列表”是单线程全量扫描（杀手级）

在 `Batch3AKernel` / `Batch3AKernel_MultiBlock` 的每一轮迭代里，`threadIdx.x == 0` 会：
1) 线性扫描 `cid=0..num_cons-1`
2) 对每个 cid 再扫描本 block 的 worlds，看 `frontier_A[cid]` 是否置位
3) 生成 `<cid, world_mask>` 的任务数组 `local_task_cids/local_task_masks`

这段逻辑本质是串行 `O(num_cons * block_world_count)`，并且 multi‑block 会被 **每个 block 重复一遍扫描**。

代码（节选）来自 **扫描版 Batch‑3A**（已被 2026-02 的 Dynamic Submission 队列版替换，保留于本文用于解释瓶颈根因）：

```cpp
// Step 1: thread 0 构建稀疏约束任务列表（只包含活跃约束）
if (threadIdx.x == 0) {
  int task_count = 0;
  for (int cid = 0; cid < num_cons && task_count < 512; ++cid) {
    u32 mask = 0;
    for (int i = 0; i < block_world_count; ++i) {
      ...
      const u32 frontier_word = ws->frontier_A[cid / 32];
      if (frontier_word & (1u << (cid % 32))) {
        mask |= (1u << i);
      }
    }
    if (mask != 0) { local_task_cids[task_count] = cid; ...; ++task_count; }
  }
  num_local_tasks = task_count;
}
```

后果：
- 这一步的成本与算子无关（与 mapping=0/1/2 基本同阶），会把任何 bitGEMM/packing 优化“淹没”。
- `task_count < 512` 还引入了 **每轮最多处理 512 个活跃约束** 的硬上限（不一定不 sound，但会拖慢收敛）。

### 3.2 Batch‑3A manager 的 host 分批 + 初始化 + 同步开销巨大（256 probes → 8 个 batch）

Batch‑3A 的 world 上限是 32（`u32 world_mask`），`--num-probes=256` 会被拆成 8 个 batch。
每个 batch 都会做：
- 保存快照（D2D memcpy + synchronize）
- 初始化每个 world 的 workspace：恢复 snapshot、清 bitmap（大量 `cudaMemcpy/cudaMemset`）
- 再 launch kernel，并且前后多次 `cudaDeviceSynchronize()`

代码锚点在 `src/solver/gpu/batch_probe_manager.cu`：

```cpp
for (int offset = 0; offset < num_tasks; offset += max_worlds_) {
  const int batch_size = std::min(max_worlds_, num_tasks - offset);
  if (offset == 0) { SaveSnapshot(); }
  InitializeWorlds(batch_size);
  BuildConstraintTasks(batch_size);
  LaunchBatch3AKernel(batch_size);
  CollectResults(...);
}
```

`InitializeWorlds()` 也在 host 侧对每个 world 做多次 CUDA API 调用：

```cpp
for (int w = 0; w < num_worlds; ++w) {
  cudaMemcpy(ws.bitDom, d_snapshot_, ... cudaMemcpyDeviceToDevice);
  cudaMemcpy(ws.d_cur_dom_size, d_dom_size_snapshot_, ... cudaMemcpyDeviceToDevice);
  cudaMemset(ws.frontier_A, 0, ...);
  cudaMemset(ws.frontier_B, 0, ...);
  ...
}
```

对比 Stage2（Batch‑2 Persistent）：
- Stage2 也做 snapshot，但随后通过 persistent kernel 在 device 内部处理大量任务，host 侧框架更轻；
- Stage2 不需要 32‑world 分批，也不需要每批都做一次“重初始化 + 反复同步”。

### 3.3 mapping=2 的 shared packing（Phase A/C）属于额外搬运，在低聚合度/浅传播下永远摊不平

mapping=2（`kWarpPerWordLaneWorld`）通过 shared 把 AoS dom 变成类似 SoA 的 `[word][world]`，
但它的代价是显式的 pack/unpack（global↔shared 的转置搬运）和同步。

在当前 perf/stress 里：
- `world_mask popcount` 多数时候偏低
- 传播迭代也不够深（“算子 compute”占比不够）

因此算子再快也无法覆盖搬运/同步开销。

### 3.4 large-92 的 OOM：容量上限对 “更重的 Batch‑3A/flatten” 极不友好

`benchmarks/marc/large-92-unsat_ext.xml` 在 stage2 baseline 的建模阶段就触发 CUDA OOM
（`src/model/gmodel_adapter.cu:79`），此时继续增加 Batch‑3A 的额外 workspace/buffer 只会更难跑通。

## 4. 能不能优化？能，但属于“研究原型完善”，不应再作为主线投入

如果目标是把 Batch‑3A 做成论文/ablation 的完整原型，**最关键的优化** 是把 3.1 的“单线程全量扫 cid”
改成并行/稀疏化的任务构建，例如：
- 以 bitmap word 为单位并行扫描（避免逐 cid）
- 让 warp 的 lane=world，用 `__ballot_sync` 直接生成 `world_mask`（把 per-cid×world 的嵌套循环变成 warp 级向量化）
- 用设备端队列/前缀和做 compaction，去掉 `local_task_cids[512]` 的硬上限

但即便如此，3.2 的 host 分批/初始化/同步仍然存在；在 Orin 上很难把当前 24×–36× 回退拉回到 >1× 的收益区间。

因此建议：
- Batch‑3A：保留 default‑off（仅用于论文/消融），不再投入“为了主线收益”的工程量；
- P2 主线：转向 Stage2/Probe‑level 的 SoA/bit‑matrix/BMMA（1‑bit Tensor Core）等“算子形态”方向。

## 5. 相关代码锚点（便于快速定位）

- 内核任务构建（串行扫描，历史实现）：见本文 3.1 的节选（扫描版已在 2026-02 被队列版替换）
- Dynamic Submission 队列版：`src/solver/gpu/GModel.cu`（`Batch3AEnqueue*` + `Batch3AKernel_MultiBlock`）
- 内核入口与 G 参数：`src/solver/gpu/GModel.cu`（`LaunchBatch3AKernelWrapper`）
- Batch‑3A 批处理框架：`src/solver/gpu/batch_probe_manager.cu`（`Batch3AManager::Execute()`）
- Batch‑3A world 初始化：`src/solver/gpu/batch_probe_manager.cu`（`Batch3AManager::InitializeWorlds()`）
- Stage2 对照实现：`src/solver/gpu/batch_probe_manager.cu`（`Batch2PersistentManager::ExecutePersistentBlocks()`）
- OOM 触发点：`src/model/gmodel_adapter.cu:79`
