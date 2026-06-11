---
status: active
updated: 2026-05-04T00:00:00Z
type: plan
topic: cpim-metal
slug: metal-gac-v312-dirty-pull-hybrid
stage: s07
---

# Metal GAC v3.12 Dirty Pull Hybrid Plan

## Goal

- 在 v3.11 `dirty_var_pull` 小幅改善 CTA 但仍未过 gate 后，评估 dirty pull 是否应只
  作用于高度变量。
- 新增 default-off `cta_dirty_pull_min_degree`，低度变量 cross-owner handoff 继续走
  direct push，高度变量才 dirty pull。
- 默认阈值为 0，完全复现 v3.11；`frontier_mode=auto` 不读取该实验路径。

## Scope

- 新增 `MetalGacOptions::cta_dirty_pull_min_degree`。
- benchmark 新增 `--cta_dirty_pull_min_degree=<N>`。
- CTA kernel 在 `dirty_var_pull` 下按 `subscription_degree(target)` 判定：
  - `degree >= N`：标记 dirty var，host 轮末 pull subscriptions；
  - `degree < N`：回退到 direct global push。
- 新增 stats：
  - `dirty_pull_fallback_push_count`
- ablation/analyzer 透传并分组 `cta_dirty_pull_min_degree`。

## Validation

- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py codex-docops-logic/scripts/dol.py`
- `cmake --build build_metal --target benchmark_metal_gac -j8`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=1 --warmup=0 --verify=true --runner_mode=prepared --frontier_mode=cta_worklist --kernel_variant=word_parallel --bitsup_layout=directional --cta_owner_mode=vebo_weighted --cta_queue_mode=bounded_replay --cta_local_round_budget=16 --cta_replay_round_budget=8 --cta_handoff_mode=dirty_var_pull --cta_dirty_pull_min_degree=16 --csv=out/metal_gac_v312_dirty_pull_hybrid16_single.csv`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=cta --runs=3 --warmup=1 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=vebo_weighted --cta-queue-mode=bounded_replay --cta-local-round-budget=16 --cta-replay-round-budget=8 --cta-handoff-mode=dirty_var_pull --cta-dirty-pull-min-degree=16 --cpu-timing --timeout=60 --csv=out/metal_gac_v312_dirty_pull_hybrid16_smoke.csv`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=cta --runs=5 --warmup=2 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=vebo_weighted --cta-queue-mode=bounded_replay --cta-local-round-budget=16 --cta-replay-round-budget=8 --cta-handoff-mode=dirty_var_pull --cta-dirty-pull-min-degree=16 --cpu-timing --timeout=300 --csv=out/metal_gac_v312_dirty_pull_hybrid16_tier2.csv --quiet`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=cta --runs=5 --warmup=2 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=vebo_weighted --cta-queue-mode=bounded_replay --cta-local-round-budget=16 --cta-replay-round-budget=8 --cta-handoff-mode=dirty_var_pull --cta-dirty-pull-min-degree=8 --cpu-timing --timeout=300 --csv=out/metal_gac_v312_dirty_pull_hybrid8_tier2.csv --quiet`

## Results

- single smoke：CPU verify 通过。
- hybrid16 metal-smoke：15/15 OK，`avg_solve_ms=0.440`。
- hybrid16 TIER2：380/383 OK，3 个 ERROR 均为历史
  `unsupported_non_binary_extension`。
- hybrid16 absolute：`solve_ms p50=0.576 p95=2.723`。
- hybrid8 TIER2：380/383 OK。
- hybrid8 absolute：`solve_ms avg=0.837 p50=0.488 p95=2.192 p99=3.797`。
- 对比：
  - v3.9 push：`p50=0.513 p95=3.357`；
  - v3.11 dirty all：`p50=0.470 p95=3.202`；
  - v3.12 hybrid8：`p50=0.488 p95=2.192`；
  - v3.12 hybrid16：`p50=0.576 p95=2.723`。
- hybrid8 stats：
  - `cross_push_avoided_count sum=173638`；
  - `dirty_pull_fallback_push_count sum=2133`；
  - `dirty_pull_scan_count sum=107335`；
  - `dirty_pull_hit_count sum=64739`。
- Combined gate for hybrid8：
  - `cta_vs_shared+flags p50=1.41x p95=3.21x`；
  - `cta_vs_best_worklist` 仍未通过；
  - `host_round_ratio_vs_baseline p50=1.00x`；
  - `decision=report_only`。
- Bucket signal：
  - `BH-4-4` bucket 继续推荐 CTA，`p50_ratio=0.40 p95_ratio=0.43`，
    `regressions_gt_threshold=0`。

## Decision

- `cta_dirty_pull_min_degree=8` 是当前 CTA dirty handoff 的最好尾部折中。
- 但它仍未通过 shared+flags / best worklist promote gate，不进入 `auto`。
- v3.12 结论：CTA 路线可主打少数 bucket，尤其 BH-like propagation-heavy case；
  不适合作为全局单实例 GAC 默认路径。
