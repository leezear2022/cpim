---
status: active
updated: 2026-05-03T15:28:08Z
type: plan
topic: cpim-metal
slug: metal-gac-v38-budget-replay
stage: s07
---

# Metal GAC v3.8 Budget/Replay Plan

## Goal

- 在转向 `vebo_weighted_owner` 前，先评估 CTA local budget 与 bounded replay
  是否能解释 v3.7 的 `cta_budget_spill_count`。
- 所有改动保持 default-off；默认 `cta_queue_mode=local_only`、
  `cta_local_round_budget=8`，`frontier_mode=auto` 不读取新路径。

## Scope

- 新增 `--cta_local_round_budget=N`，默认 8。
- 新增 `--cta_replay_round_budget=N`，默认 8，仅
  `--cta_queue_mode=bounded_replay` 时生效。
- 扩展 `--cta_queue_mode=local_only|spill_replay|bounded_replay`。
- 新增 CTA replay stats：
  - `cta_budget_replay_rounds`
  - `cta_budget_replay_drain_count`
  - `cta_budget_replay_spill_count`
- analyzer 按 owner、queue mode、local budget、replay budget 分组，避免不同
  budget 数据混合解释。

## Validation

- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py codex-docops-logic/scripts/dol.py`
- `cmake --build build_metal --target benchmark_metal_gac -j8`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=1 --warmup=0 --verify=true --runner_mode=prepared --frontier_mode=cta_worklist --kernel_variant=word_parallel --bitsup_layout=directional --cta_owner_mode=static_edge_cut --cta_queue_mode=bounded_replay --cta_local_round_budget=8 --cta_replay_round_budget=8 --csv=out/metal_gac_v38_bounded_replay_single.csv`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=cta --runs=3 --warmup=1 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=static_edge_cut --cta-queue-mode=bounded_replay --cta-local-round-budget=8 --cta-replay-round-budget=8 --cpu-timing --timeout=60 --csv=out/metal_gac_v38_bounded_replay_smoke.csv`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=cta --runs=5 --warmup=2 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=static_edge_cut --cta-queue-mode=bounded_replay --cta-local-round-budget=8 --cta-replay-round-budget=8 --cpu-timing --timeout=300 --csv=out/metal_gac_v38_bounded_replay_tier2.csv --quiet`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=cta --runs=5 --warmup=2 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=static_edge_cut --cta-queue-mode=spill_replay --cta-local-round-budget=16 --cta-replay-round-budget=0 --cpu-timing --timeout=300 --csv=out/metal_gac_v38_local_budget16_tier2.csv --quiet`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v35_tier2_baseline.csv out/metal_gac_v35_tier2_frontier.csv out/metal_gac_v36_owner_map_static_tier2.csv out/metal_gac_v37_seed_overflow_tier2.csv out/metal_gac_v38_bounded_replay_tier2.csv out/metal_gac_v38_local_budget16_tier2.csv --top=10 --recommend-policy --baseline-mode shared+flags --regression-threshold 1.05 --min-runs 3`
- `git diff --check`
- `python3 codex-docops-logic/scripts/dol.py lint --soft`

## Results

- single smoke：CPU verify 通过。
- metal-smoke：15/15 OK。
- bounded replay TIER2：380/383 OK，3 个 ERROR 均为历史
  `unsupported_non_binary_extension`。
- `bounded_replay local=8 replay=8`：
  - `cta_vs_shared+flags p50=1.36x p95=3.49x`
  - `cta_vs_best_worklist p50=1.40x p95=3.43x`
  - `host_round_ratio_vs_baseline p50=1.00x p95=1.00x`
  - `queue_overflow_p95=0`
  - `seed_overflow_p95=0`
  - `budget_spill_p95=0`
  - `replay drain p95=1`
  - `decision=report_only`
- `spill_replay local=16`：
  - `cta_vs_shared+flags p50=1.39x p95=3.57x`
  - `cta_vs_best_worklist p50=1.28x p95=3.49x`
  - `budget_spill_p95=0`
  - `host_round_ratio_vs_baseline p50=1.00x`
  - `decision=report_only`

## Decision

- local budget / bounded replay 能清零 budget spill，但不能降低 host round，也不能解除
  baseline/worklist p95 regression。
- 结论：queue/seed/budget 协议不是 CTA worklist 当前 TIER2 p95 失败主因。
- 下一步应转向 `vebo_weighted_owner`，继续保持 `cta_worklist` report-only。
