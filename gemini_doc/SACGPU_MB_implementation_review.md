# FQ-PT（P0/P1/P2）慢于 Batch2 的归因报告与优化路线（可转发）

## 摘要
1. 在固定 5 例、固定参数（`--num_probes=64 --warmup=1 --iterations=5`）下，`P0/P1/P2` 全部慢于 `Stage-2(Batch2)`，中位慢约 `9x`。
2. 主要瓶颈不在约束检查算子本身，而在“任务调度与并发控制开销”：全局 MPMC 队列、world 锁、pending/stale 原子、分桶串行决策。
3. 当前基准集全部 `bit_dom_int_size=1`，导致 P2 并行检查路径受门控基本不生效。
4. rand 两例出现高 `ovf` 与高 `unknown`，说明队列容量与入队策略是关键退化点。
5. 建议先做“调度面止损”（队列/锁/分桶算法），再做“算子并行增强”。

## 现状证据（2026-02-17 实测）
测试口径：`./build/sac_benchmark --mode=<stage2|fqpt> --input=<case> --num_probes=64 --warmup=1 --iterations=5`

| case | stage2(ms) | p0(ms) | p1(ms) | p2(ms) | p0/stage2 | p1/stage2 | p2/stage2 |
|---|---:|---:|---:|---:|---:|---:|---:|
| tests/data/bench/queens-4_ext.xml | 1.97 | 7.26 | 6.93 | 7.27 | 3.69x | 3.52x | 3.69x |
| tests/data/bench/queens-12_ext.xml | 2.16 | 42.46 | 42.52 | 42.54 | 19.66x | 19.69x | 19.69x |
| tests/data/bench/haystacks-11_ext.xml | 4.17 | 55.42 | 51.15 | 55.31 | 13.29x | 12.27x | 13.26x |
| tests/data/bench/rand-2-23/rand-23-23-253-131-46021_ext.xml | 5.64 | 52.58 | 54.74 | 49.53 | 9.32x | 9.71x | 8.78x |
| tests/data/bench/rand-2-23/rand-23-23-253-131-55021_ext.xml | 5.64 | 50.61 | 46.53 | 52.28 | 8.97x | 8.25x | 9.27x |

辅助观测：
1. rand 两例 `ovf/unknown` 很高（约 `75/56` 量级），例如 `tests/data/bench/rand-2-23/rand-23-23-253-131-46021_ext.xml` 在 P0/P1/P2 分别约 `75/56`、`76/56`、`75/58`。
2. 非 rand 3 例仍有明显锁重试，`lock_retry` 均值约 `7.5k`（P0/P1/P2 都存在），说明锁冲突是常态。
3. `Stage-2` 行里 `checks=0` 来自 benchmark 统计未接线，不代表未做传播（`apps/sac_benchmark.cpp:503`）。

## 根因分析（代码锚点）
1. **任务粒度过细，调度成本压过计算成本**  
FQPT 以 `(world,cid)` 为任务粒度，每个任务都要走队列、去重、pending、可能锁竞争：`src/solver/gpu/GModel.cu:2564`、`src/solver/gpu/GModel.cu:2604`、`src/solver/gpu/GModel.cu:2718`、`src/solver/gpu/GModel.cu:2975`。  
Batch2 是“每个 block 拉 probe 后在本地 workspace 连续跑传播”：`src/solver/gpu/GModel.cu:3397`、`src/solver/gpu/GModel.cu:3419`、`src/solver/gpu/GModel.cu:3565`。

2. **world 锁竞争显著，形成大量无效重试**  
FQPT 同一 world 可被多 block 抢占，导致 `atomicCAS(lock)` + backoff 重试链：`src/solver/gpu/GModel.cu:2975`、`src/solver/gpu/GModel.cu:3122`。  
统计上 `lock_fail/lock_retry` 长期偏高，直接吞吐损耗。

3. **P1 分桶决策是 thread0 串行 O(n²)，收益易被吃掉**  
`best_cid` 的选择通过双循环扫描本地缓冲：`src/solver/gpu/GModel.cu:2882`。  
当 `local_cap=64` 时，分桶决策本身已是可见成本；而当前 `avg_bucket_size≈2`、`avg_bucket_utilization≈0.5`，并不高。

4. **P2 在当前基准上“门没开”**  
P2 受 `model.bit_dom_int_size > 1` 限制：`src/solver/gpu/GModel.cu:2785`。  
本次 5 个固定样例实测均为 `bit_dom_int_size=1`，因此并行检查路径基本不触发，P2 无法体现设计收益。

5. **全局队列容量与 UNKNOWN 退化触发频繁**  
初始化与运行时溢出都会把 world 标记为 UNKNOWN：`src/solver/gpu/batch_probe_manager.cu:2917`、`src/solver/gpu/GModel.cu:2726`。  
在 rand 两例中已出现高频 overflow，造成性能抖动和有效吞吐下降。

6. **数据局部性天然不如 Batch2**  
FQPT 为每个 world 分配 workspace（`max_tasks_` 级别）：`src/solver/gpu/batch_probe_manager.cu:2575`。  
Batch2 是每个 block 固定复用一个 workspace：`src/solver/gpu/GModel.cu:3405`。  
前者跨 world 调度更随机，cache/L2 复用更弱。

## 关键代码片段（可给其他模型直接看）

### 片段 A：P2 触发门控（当前基准不满足）
`src/solver/gpu/GModel.cu:2781`
```cpp
const bool enable_parallel_group_check =
    enable_grouping &&
    (control->enable_parallel_group_check != 0) &&
    (model.bit_dom_int_size > 1);
```

### 片段 B：FQPT 全局 MPMC 队列的 CAS + 自旋
`src/solver/gpu/GModel.cu:2572`
```cpp
while (true) {
  const unsigned long long tail = FQPTLoadU64(control->enqueue_pos);
  const unsigned long long head = FQPTLoadU64(control->dequeue_pos);
  if (tail + (unsigned long long)count >
      head + (unsigned long long)control->queue_capacity) {
    return false;
  }
  if (atomicCAS(control->enqueue_pos, tail, tail + count) == tail) {
    base = tail;
    break;
  }
}
```

### 片段 C：P1 分桶的 thread0 O(n²) 选桶
`src/solver/gpu/GModel.cu:2882`
```cpp
int best_cid = -1;
int best_count = 0;
for (int i = pop_head; i < pop_count; ++i) {
  const int cid_i = local_pop[i].cid;
  int count = 0;
  for (int j = pop_head; j < pop_count; ++j) {
    if (local_pop[j].cid == cid_i) ++count;
  }
  if (count > best_count) {
    best_count = count;
    best_cid = cid_i;
  }
}
```

### 片段 D：Batch2 的 block 本地连续执行模型
`src/solver/gpu/GModel.cu:3405`
```cpp
WorldWorkspace* ws = &control->workspaces[block_id];
while (true) {
  if (threadIdx.x == 0) {
    shared_base_task_id = atomicAdd(control->task_cursor, chunk_size);
  }
  __syncthreads();
  if (base_task_id >= control->num_tasks) break;
  // 对 task 连续恢复快照 + 初始化frontier + RunGACToFixpoint_BlockSync
}
```

## 优化方案（按优先级）

### A. 先做止损（1-2 天，低风险）
1. 将 `queue_capacity` 改为按 `num_worlds * avg_degree` 自适应上限，避免 rand 场景高 `ovf/unknown`。  
2. 对 `bit_dom_int_size==1` 直接关闭 P1/P2 分桶路径（或提高触发阈值），避免“分桶成本 > 收益”。  
3. 增加运行时“锁冲突退化”：`lock_retry` 超阈值时自动降低并行度或切回 P0。

### B. 调度面重构（3-5 天，中风险）
1. 把 P1 的 O(n²) 选桶改为 O(n) 计数（小哈希/固定桶）并减少 thread0 串行压缩。  
2. 引入 world-owner（分片）策略：同一 world 优先由固定 block/warp 处理，显著降低 lock CAS。  
3. 把 `pending/processed` 原子改为 CTA 局部累加 + 批量 flush，减少全局原子频度。

### C. 算子并行增强（5-8 天，中高风险）
1. 单独实现 `bit_dom_int_size==1` 的小域并行检查路径（当前是最大缺口）。  
2. P2 保持“warp 私有 scratch + 单点 commit”原则，但先在可触发样例验证（`bit_dom_int_size>1`）。

## Public API / 类型影响
1. 本报告不要求立即改 public API。  
2. 若执行 A/B/C，建议新增可选控制字段：`max_queue_capacity_auto`、`lock_retry_degrade_threshold`、`enable_small_dom_parallel_check`。  
3. 现有统计字段已足够做第一轮判定（`stale/lock_fail/lock_retry/avg_bucket_*`）。

## 测试场景与验收标准
1. 固定 5 例继续保留，用于回归与止损门判断。  
2. 必须新增至少 3 个 `bit_dom_int_size>1` 样例，否则 P2 无法被有效评估。  
3. 正确性门槛：`ctest --test-dir build -R test_fqpt_baseline --output-on-failure` 通过，且 `unknown=0` 时与 Stage2 一致（`tests/cpp/test_fqpt_baseline.cpp:171`）。  
4. 性能门槛：先看 `ovf/unknown` 是否显著下降，再看 `avg_time_ms` 与 `lock_retry` 是否同步下降。  
5. 发布策略：全部新路径默认 `off`，仅在门槛达标后考虑默认开启。

## 假设与默认值
1. 平台假设：Jetson Orin（仓库目标平台）。  
2. 基准参数默认：`--num_probes=64 --warmup=1 --iterations=5`。  
3. soundness 不变：UNKNOWN 不删值。  
4. 结论针对当前代码与当前固定 5 例；不同数据分布需要复测后再裁决。
