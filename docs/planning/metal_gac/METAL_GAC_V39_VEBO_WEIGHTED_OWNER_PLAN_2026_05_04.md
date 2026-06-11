---
status: active
updated: 2026-05-03T18:49:44Z
type: plan
topic: cpim-metal
slug: metal-gac-v39-vebo-weighted-owner
stage: s07
---

# Metal GAC v3.9 VEBO Weighted Owner Plan

## Goal

- 在 v3.8 证明 seed/overflow/budget 协议不是主因后，进入
  `vebo_weighted_owner`。
- 目标是降低 owner load skew，观察它是否能降低 CTA worklist p95 和 host round。
- 所有改动保持 default-off；默认 `cta_owner_mode=modulo`、`frontier_mode=auto`
  不读取新路径。

## Scope

- 新增 `--cta_owner_mode=vebo_weighted`。
- Host 侧构建 `owner_of_constraint[cid]`：
  - 先按 variable degree 生成 VEBO 风格变量顺序；
  - 对约束赋予 `bit_words * (degree(x) + degree(y))` 权重；
  - 在 soft count/weight 约束内优先保留已分配邻居 locality；
  - 其次按 owner weighted load、owner count、owner id 兜底。
- 新增 CSV/stdout/analyzer 字段：
  - `owner_weight_balance_p95`
- 复用 v3.8 的 `bounded_replay`，避免 budget spill 混淆 owner 结果。

## Validation

- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py codex-docops-logic/scripts/dol.py`
- `cmake --build build_metal --target benchmark_metal_gac -j8`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=1 --warmup=0 --verify=true --runner_mode=prepared --frontier_mode=cta_worklist --kernel_variant=word_parallel --bitsup_layout=directional --cta_owner_mode=vebo_weighted --cta_queue_mode=bounded_replay --cta_local_round_budget=8 --cta_replay_round_budget=8 --csv=out/metal_gac_v39_vebo_weighted_single.csv`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=cta --runs=3 --warmup=1 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=vebo_weighted --cta-queue-mode=bounded_replay --cta-local-round-budget=8 --cta-replay-round-budget=8 --cpu-timing --timeout=60 --csv=out/metal_gac_v39_vebo_weighted_smoke.csv`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=cta --runs=5 --warmup=2 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=vebo_weighted --cta-queue-mode=bounded_replay --cta-local-round-budget=8 --cta-replay-round-budget=8 --cpu-timing --timeout=300 --csv=out/metal_gac_v39_vebo_weighted_tier2.csv --quiet`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=cta --runs=5 --warmup=2 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=vebo_weighted --cta-queue-mode=bounded_replay --cta-local-round-budget=16 --cta-replay-round-budget=8 --cpu-timing --timeout=300 --csv=out/metal_gac_v39_vebo_weighted_local16_tier2.csv --quiet`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v35_tier2_baseline.csv out/metal_gac_v35_tier2_frontier.csv out/metal_gac_v38_bounded_replay_tier2.csv out/metal_gac_v39_vebo_weighted_tier2.csv out/metal_gac_v39_vebo_weighted_local16_tier2.csv --top=10 --recommend-policy --baseline-mode shared+flags --regression-threshold 1.05 --min-runs 3`
- `git diff --check`
- `python3 codex-docops-logic/scripts/dol.py lint --soft`

## Results

- single smoke：CPU verify 通过。
- metal-smoke：15/15 OK。
- `vebo_weighted local=8 replay=8` TIER2：380/383 OK，3 个 ERROR 均为历史
  `unsupported_non_binary_extension`。
- `vebo_weighted local=16 replay=8` TIER2：380/383 OK，3 个 ERROR 同上。
- 相对 v3.8 `static_edge_cut bounded_replay local=8 replay=8`：
  - absolute `solve_ms p95` 从 `3.737ms` 降到 `3.325ms`；
  - `owner_balance_p95_avg` 从 `1.36` 降到 `1.28`；
  - `owner_weight_balance_p95_avg` 为 `1.22`；
  - `budget_spill_p95=0`；
  - `cta_cross` 从 `613.17` 降到 `461.30`；
  - `host_round_ratio_vs_baseline p50=1.00x`。
- Combined gate：
  - `cta_vs_shared+flags p50=1.41x p95=3.14x`；
  - `cta_vs_best_worklist p50=1.40x p95=3.13x`；
  - `decision=report_only`。

## Decision

- `vebo_weighted_owner` 有正向信号，是目前 CTA owner 分支中更好的
  report-only 候选。
- 仍不进入 `frontier_mode=auto`，原因是 per-input p95 regression 与 host round
  gate 均未通过。
- 若继续单实例 GAC 性能线，下一步应做 owner locality/cross-push hybrid 调优；
  若不继续硬推 CTA，则回到 Batch/SAC 多任务吞吐方向。
