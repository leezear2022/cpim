---
status: active
updated: 2026-05-04T00:00:00Z
type: changelog
topic: cpim-metal
slug: metal-gac-v312-dirty-pull-hybrid
stage: s07
---

# Metal GAC v3.12 Dirty Pull Hybrid Changelog

## Summary

- 新增 `--cta_dirty_pull_min_degree`，让 `dirty_var_pull` 只作用于订阅度足够高的变量。
- 低度变量 cross-owner handoff 回退 direct push，避免 host dirty scan 反噬。
- CSV、stdout、ablation、analyzer 新增阈值和 fallback push 统计。

## Changes

- `MetalGacOptions` 新增 `cta_dirty_pull_min_degree`，默认 0。
- `GacParams` / `GacParamsMetal` 新增 `cta_dirty_pull_min_degree`。
- `gac_revise_cta_worklist_kernel`：
  - `degree(target) >= cta_dirty_pull_min_degree` 时继续 dirty pull；
  - 否则 direct push 到 global next active；
  - direct fallback 计入 `dirty_pull_fallback_push_count`。
- `MetalGacStats` 新增 `dirty_pull_fallback_push_count`。
- `benchmark_metal_gac` CSV/stdout 新增：
  - `cta_dirty_pull_min_degree`
  - `dirty_pull_fallback_push_count`
- `metal_gac_ablation.py` 新增 `--cta-dirty-pull-min-degree`。
- `metal_gac_analyze.py` 将 threshold 纳入 CTA gate 分组，并显示 fallback push。

## Validation

- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py codex-docops-logic/scripts/dol.py`
- `cmake --build build_metal --target benchmark_metal_gac -j8`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=1 --warmup=0 --verify=true --runner_mode=prepared --frontier_mode=cta_worklist --kernel_variant=word_parallel --bitsup_layout=directional --cta_owner_mode=vebo_weighted --cta_queue_mode=bounded_replay --cta_local_round_budget=16 --cta_replay_round_budget=8 --cta_handoff_mode=dirty_var_pull --cta_dirty_pull_min_degree=16 --csv=out/metal_gac_v312_dirty_pull_hybrid16_single.csv`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=cta --runs=3 --warmup=1 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=vebo_weighted --cta-queue-mode=bounded_replay --cta-local-round-budget=16 --cta-replay-round-budget=8 --cta-handoff-mode=dirty_var_pull --cta-dirty-pull-min-degree=16 --cpu-timing --timeout=60 --csv=out/metal_gac_v312_dirty_pull_hybrid16_smoke.csv`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=cta --runs=5 --warmup=2 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=vebo_weighted --cta-queue-mode=bounded_replay --cta-local-round-budget=16 --cta-replay-round-budget=8 --cta-handoff-mode=dirty_var_pull --cta-dirty-pull-min-degree=16 --cpu-timing --timeout=300 --csv=out/metal_gac_v312_dirty_pull_hybrid16_tier2.csv --quiet`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=cta --runs=5 --warmup=2 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=vebo_weighted --cta-queue-mode=bounded_replay --cta-local-round-budget=16 --cta-replay-round-budget=8 --cta-handoff-mode=dirty_var_pull --cta-dirty-pull-min-degree=8 --cpu-timing --timeout=300 --csv=out/metal_gac_v312_dirty_pull_hybrid8_tier2.csv --quiet`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v35_tier2_baseline.csv out/metal_gac_v35_tier2_frontier.csv out/metal_gac_v39_vebo_weighted_local16_tier2.csv out/metal_gac_v311_dirty_var_pull_tier2.csv out/metal_gac_v312_dirty_pull_hybrid8_tier2.csv --top=10 --recommend-policy --baseline-mode shared+flags --regression-threshold 1.05 --min-runs 3`

## Results

- hybrid16 single / smoke correctness 通过。
- hybrid16 TIER2：380/383 OK，`solve_ms p50=0.576 p95=2.723`。
- hybrid8 TIER2：380/383 OK，`solve_ms avg=0.837 p50=0.488 p95=2.192`。
- hybrid8 相对 v3.11：
  - p50 略慢：`0.470 -> 0.488`；
  - p95 明显改善：`3.202 -> 2.192`。
- hybrid8 相对 v3.9：
  - p50 改善：`0.513 -> 0.488`；
  - p95 改善：`3.357 -> 2.192`。
- hybrid8 gate 仍未通过：
  - `cta_vs_shared+flags p50=1.41x p95=3.21x`；
  - `decision=report_only`。
- bucket evidence：
  - `BH-4-4` bucket `p50_ratio=0.40 p95_ratio=0.43`，
    `regressions_gt_threshold=0`。

## Decision

- v3.12 hybrid threshold 证明 dirty pull 的尾部可调，但不能解决全局 CTA p95 gate。
- `cta_dirty_pull_min_degree=8` 保留为 report-only 最佳候选。
- 后续不再继续硬推全局 CTA；若继续，只做 BH-like bucket policy 或转向
  Batch/SAC / dispatch 成本优化。
