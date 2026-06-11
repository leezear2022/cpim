---
status: active
updated: 2026-05-03T15:28:08Z
type: changelog
topic: cpim-metal
slug: metal-gac-v38-budget-replay
stage: s07
---

# Metal GAC v3.8 Budget/Replay Changelog

## Summary

- 新增 default-off `bounded_replay` CTA queue mode。
- 新增可调 CTA local round budget 与 replay budget。
- 新增 replay 统计，确认 budget spill 是否能通过有限本地回放消除。
- TIER2 结果显示 budget spill 可清零，但 p95 和 host round gate 仍不过。

## Changes

- `MetalCtaQueueMode` 新增 `kBoundedReplay`。
- `MetalGacOptions` 新增：
  - `cta_local_round_budget`
  - `cta_replay_round_budget`
- `benchmark_metal_gac` 新增：
  - `--cta_queue_mode=bounded_replay`
  - `--cta_local_round_budget`
  - `--cta_replay_round_budget`
- `gac_revise_cta_worklist_kernel`：
  - base local budget 用完后，`bounded_replay` 可继续执行有限 replay rounds；
  - replay drain 时不计入 budget spill；
  - replay 后仍有 pending 才计入 `cta_budget_spill_count`。
- CSV/stdout/ablation/analyzer 新增：
  - `cta_local_round_budget`
  - `cta_replay_round_budget`
  - `cta_budget_replay_rounds`
  - `cta_budget_replay_drain_count`
  - `cta_budget_replay_spill_count`
- analyzer CTA gate 改为按 owner、queue mode、local budget、replay budget 分组。

## Validation

- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py codex-docops-logic/scripts/dol.py`
- `cmake --build build_metal --target benchmark_metal_gac -j8`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=1 --warmup=0 --verify=true --runner_mode=prepared --frontier_mode=cta_worklist --kernel_variant=word_parallel --bitsup_layout=directional --cta_owner_mode=static_edge_cut --cta_queue_mode=bounded_replay --cta_local_round_budget=8 --cta_replay_round_budget=8 --csv=out/metal_gac_v38_bounded_replay_single.csv`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=cta --runs=3 --warmup=1 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=static_edge_cut --cta-queue-mode=bounded_replay --cta-local-round-budget=8 --cta-replay-round-budget=8 --cpu-timing --timeout=60 --csv=out/metal_gac_v38_bounded_replay_smoke.csv`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=cta --runs=5 --warmup=2 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=static_edge_cut --cta-queue-mode=bounded_replay --cta-local-round-budget=8 --cta-replay-round-budget=8 --cpu-timing --timeout=300 --csv=out/metal_gac_v38_bounded_replay_tier2.csv --quiet`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=cta --runs=5 --warmup=2 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=static_edge_cut --cta-queue-mode=spill_replay --cta-local-round-budget=16 --cta-replay-round-budget=0 --cpu-timing --timeout=300 --csv=out/metal_gac_v38_local_budget16_tier2.csv --quiet`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v35_tier2_baseline.csv out/metal_gac_v35_tier2_frontier.csv out/metal_gac_v36_owner_map_static_tier2.csv out/metal_gac_v37_seed_overflow_tier2.csv out/metal_gac_v38_bounded_replay_tier2.csv out/metal_gac_v38_local_budget16_tier2.csv --top=10 --recommend-policy --baseline-mode shared+flags --regression-threshold 1.05 --min-runs 3`

## Results

- `bounded_replay local=8 replay=8`：380/383 OK，`budget_spill_p95=0`，
  `replay_drain_p95=1`，`cta_vs_shared+flags p95=3.49x`。
- `spill_replay local=16`：380/383 OK，`budget_spill_p95=0`，
  `cta_vs_shared+flags p95=3.57x`。
- 两个 v3.8 实验都保持 `decision=report_only`，原因均包括
  `baseline_p95_regression`、`worklist_p95_regression` 与
  `host_round_not_reduced`。

## Decision

- 不把 `bounded_replay` 或 `local_budget=16` 纳入 `auto`。
- 下一步进入 `vebo_weighted_owner`，优先解释 seed balance p95 约 3.36 和
  owner load skew，而不是继续调整 seed/overflow/budget 协议。
