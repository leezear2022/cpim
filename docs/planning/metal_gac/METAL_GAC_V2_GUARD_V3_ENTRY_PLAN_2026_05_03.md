---
status: active
updated: 2026-05-03
type: plan
scope: metal-gac
---

# Metal GAC v2 Guard / v3 Entry Plan

## 目标

- 把 Metal GAC v2 封版路径固定为可测试、可解释、可回退的稳定基线。
- 在不改变默认行为的前提下，进入 v3 policy recommender。
- 先做推荐和证据积累，不直接把激进路径设为默认。

## P0：v2 Regression Guard

### 目标

- 任何后续 v3/SAC 改动都不能破坏 v2 fallback 和显式消融路径。

### 任务

- 固定 `frontier_mode=auto kernel_variant=auto bitsup_layout=auto` 的 effective path：
  `flags + scalar + pair`。
- 固定显式激进路径 smoke：
  `worklist + word_parallel + directional + blit reset`。
- 固定 CSV schema：
  - `effective_frontier_mode`
  - `effective_kernel_variant`
  - `effective_bitsup_layout`
  - `reset_mode`
  - `reset_dispatch_ms`
  - `worklist_push_count`
  - `worklist_rounds`
  - `worklist_epoch_resets`
- 将 unsupported non-binary extension 保持为分类错误，不计入性能失败。

### 验收

- `ctest --test-dir build_metal -R compare_cpu_metal --output-on-failure`
- `ctest --test-dir build_metal -R benchmark_metal_gac --output-on-failure`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=auto --runs=3 --warmup=1 --runner-mode=prepared --timeout=60 --csv=out/metal_gac_v2_guard_auto.csv`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=1 --warmup=0 --verify=true --runner_mode=prepared --frontier_mode=worklist --kernel_variant=word_parallel --bitsup_layout=directional --reset_mode=blit --csv=out/metal_gac_v2_guard_worklist_word_blit.csv`

## P1：Baseline Evidence 固化

### 目标

- 把 v2 封版数据变成可复跑证据，而不是一次性手工结果。

### 任务

- 保留可复跑命令：
  - `metal-smoke all`
  - `metal-smoke auto`
  - `TIER2 all`
  - `TIER2 auto`
- 分析输出必须包含：
  - p50/p95/p99 `solve_ms`
  - unsupported 分类
  - recommended policy summary
  - auto vs baseline 5% 门槛结论

### 验收

- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v2x_tier2_all.csv out/metal_gac_v2x_tier2_auto.csv --top=10`
- 分析输出明确列出 baseline 与 auto 的 p50/p95。
- 若 auto p50 或 p95 慢于 baseline 超过 5%，必须继续保持或降级 auto。

## P2：v3 Policy Recommender

### 目标

- 先输出推荐，不直接改变默认路径。

### 输入特征

- family
- `num_constraints`
- `max_dom_size`
- `bit_words`
- `frontier_density_avg`
- `effective_frontier_mode`
- `effective_kernel_variant`
- `effective_bitsup_layout`

### 输出

- 推荐 flags/compact/worklist/word_parallel 的条件。
- 每个推荐附带：
  - 命中实例数
  - 对 baseline 的 p50/p95 比值
  - 劣化超过 5% 的实例清单

### 验收

- recommender 输出只作为报告，不改变 `auto` 默认策略。
- 推荐必须能回溯到 CSV 数据，不允许纯经验判断。

## P3：Simdgroup / Threadgroup Staging 决策

### 进入条件

- word_parallel 数据证明 `kernel_ms` 是主要瓶颈。
- 至少连续两组 tier 数据中，`kernel_ms` p95 明显高于 host dispatch/reset 成本。
- CPU verify 和所有消融路径稳定。

### 暂缓条件

- 瓶颈仍是 host dispatch/reset。
- 结果受实例噪声影响明显。
- word_parallel 只在少数实例受益，且 recommended policy 置信度不足。

## P4：Metal SAC/Batch Correctness MVP

### 前置条件

- P0 regression guard 已稳定。
- v2 baseline evidence 可复跑。

### 第一目标

- CPU/Metal correctness 对齐。
- 不先追 SAC/Batch 性能。
- 不修改 CUDA/Jetson stable path。
