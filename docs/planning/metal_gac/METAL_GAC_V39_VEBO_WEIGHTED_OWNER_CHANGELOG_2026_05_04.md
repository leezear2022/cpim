---
status: active
updated: 2026-05-03T18:49:44Z
type: changelog
topic: cpim-metal
slug: metal-gac-v39-vebo-weighted-owner
stage: s07
---

# Metal GAC v3.9 VEBO Weighted Owner Changelog

## Summary

- 新增 default-off `--cta_owner_mode=vebo_weighted`。
- Host owner map 构建改为 variable-degree ordered、constraint-weighted greedy。
- 新增 `owner_weight_balance_p95` 用于解释 weighted owner load。
- TIER2 显示 vebo weighted owner 改善 CTA p95，但仍未通过 promote gate。

## Changes

- `MetalCtaOwnerMode` 新增 `kVeboWeighted`。
- `benchmark_metal_gac --cta_owner_mode` 支持
  `modulo|static_edge_cut|vebo_weighted`。
- `metal_gac_ablation.py --cta-owner-mode` 支持 `vebo_weighted`。
- `metal_gac_analyze.py` mode summary 和 CTA gate 显示
  `owner_weight_balance_p95`。
- Host `BuildCtaOwnerMap()` 新增 `BuildVeboWeightedOwnerMap()`：
  - variable degree 降序遍历；
  - constraint weight 使用 endpoint degree 与 `bit_words`；
  - soft load 内优先 locality，再以 weighted load/count tie-break。

## Validation

- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py codex-docops-logic/scripts/dol.py`
- `cmake --build build_metal --target benchmark_metal_gac -j8`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=1 --warmup=0 --verify=true --runner_mode=prepared --frontier_mode=cta_worklist --kernel_variant=word_parallel --bitsup_layout=directional --cta_owner_mode=vebo_weighted --cta_queue_mode=bounded_replay --cta_local_round_budget=8 --cta_replay_round_budget=8 --csv=out/metal_gac_v39_vebo_weighted_single.csv`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=cta --runs=3 --warmup=1 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=vebo_weighted --cta-queue-mode=bounded_replay --cta-local-round-budget=8 --cta-replay-round-budget=8 --cpu-timing --timeout=60 --csv=out/metal_gac_v39_vebo_weighted_smoke.csv`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=cta --runs=5 --warmup=2 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=vebo_weighted --cta-queue-mode=bounded_replay --cta-local-round-budget=8 --cta-replay-round-budget=8 --cpu-timing --timeout=300 --csv=out/metal_gac_v39_vebo_weighted_tier2.csv --quiet`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=cta --runs=5 --warmup=2 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=vebo_weighted --cta-queue-mode=bounded_replay --cta-local-round-budget=16 --cta-replay-round-budget=8 --cpu-timing --timeout=300 --csv=out/metal_gac_v39_vebo_weighted_local16_tier2.csv --quiet`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v35_tier2_baseline.csv out/metal_gac_v35_tier2_frontier.csv out/metal_gac_v38_bounded_replay_tier2.csv out/metal_gac_v39_vebo_weighted_tier2.csv out/metal_gac_v39_vebo_weighted_local16_tier2.csv --top=10 --recommend-policy --baseline-mode shared+flags --regression-threshold 1.05 --min-runs 3`

## Results

- metal-smoke：15/15 OK。
- TIER2：380/383 OK，3 个 ERROR 均为历史 `unsupported_non_binary_extension`。
- `vebo_weighted local=16 replay=8`：
  - `solve_ms p50=0.513 p95=3.325`；
  - `cta_vs_shared+flags p50=1.41x p95=3.14x`；
  - `cta_vs_best_worklist p50=1.40x p95=3.13x`；
  - `owner_balance_p95_avg=1.28`；
  - `owner_weight_balance_p95_avg=1.22`；
  - `budget_spill_p95=0`；
  - `decision=report_only`。

## Decision

- `vebo_weighted_owner` 比 v3.8 owner map 更有信号，但仍不满足 auto promote。
- 下一步若继续 CTA，应围绕 owner locality/cross-push hybrid 调优，而不是回到
  seed/overflow/budget 协议。
