---
status: active
updated: 2026-05-04T00:00:00Z
type: plan
topic: cpim-metal
slug: metal-gac-v311-dirty-var-pull
stage: s07
---

# Metal GAC v3.11 Dirty Var Pull Plan

## Goal

- 在 v3.10 `bulk_sync_mask` 因双 dispatch 成本被拒后，回到 CTA worklist 路线。
- 新增 default-off `dirty_var_pull` handoff，判断跨 owner constraint push 是否是
  CTA p95 的主要成本。
- 保持 `frontier_mode=auto`、默认 `cta_owner_mode=modulo`、默认
  `cta_queue_mode=local_only` 和稳定 fallback 不变。

## Scope

- 新增 `MetalCtaHandoffMode { push_constraints, dirty_var_pull }`。
- benchmark 新增 `--cta_handoff_mode=push_constraints|dirty_var_pull`，默认
  `push_constraints`。
- CTA kernel 在 `dirty_var_pull` 下：
  - same-owner subscription 继续进入 CTA local queue；
  - cross-owner subscription 不再直接写 global next active；
  - 改为标记 `dirty_var_epochs[var] = frontier_epoch`；
  - 保留 queue overflow 与 pending spill 的原安全回写路径。
- Host 在 CTA dispatch 后扫描 dirty vars 的 subscriptions，重建下一轮
  `next_active_constraints`。
- 新增 stats：
  - `dirty_var_count`
  - `dirty_pull_scan_count`
  - `dirty_pull_hit_count`
  - `cross_push_avoided_count`
- analyzer 将 `cta_handoff_mode` 纳入 CTA gate 分组，避免把 push 与 pull 数据混读。

## Validation

- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py codex-docops-logic/scripts/dol.py`
- `cmake --build build_metal --target benchmark_metal_gac -j8`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=1 --warmup=0 --verify=true --runner_mode=prepared --frontier_mode=cta_worklist --kernel_variant=word_parallel --bitsup_layout=directional --cta_owner_mode=vebo_weighted --cta_queue_mode=bounded_replay --cta_local_round_budget=16 --cta_replay_round_budget=8 --cta_handoff_mode=dirty_var_pull --csv=out/metal_gac_v311_dirty_var_pull_single.csv`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=cta --runs=3 --warmup=1 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=vebo_weighted --cta-queue-mode=bounded_replay --cta-local-round-budget=16 --cta-replay-round-budget=8 --cta-handoff-mode=dirty_var_pull --cpu-timing --timeout=60 --csv=out/metal_gac_v311_dirty_var_pull_smoke.csv`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=cta --runs=5 --warmup=2 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=vebo_weighted --cta-queue-mode=bounded_replay --cta-local-round-budget=16 --cta-replay-round-budget=8 --cta-handoff-mode=dirty_var_pull --cpu-timing --timeout=300 --csv=out/metal_gac_v311_dirty_var_pull_tier2.csv --quiet`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v35_tier2_baseline.csv out/metal_gac_v35_tier2_frontier.csv out/metal_gac_v39_vebo_weighted_local16_tier2.csv out/metal_gac_v310_bulk_sync_mask_tier2.csv out/metal_gac_v311_dirty_var_pull_tier2.csv --top=10 --recommend-policy --baseline-mode shared+flags --regression-threshold 1.05 --min-runs 3`
- `git diff --check`
- `python3 codex-docops-logic/scripts/dol.py lint --soft`

## Results

- single smoke：CPU verify 通过；`deletions=78`，无 queue/seed/budget overflow。
- metal-smoke：15/15 OK，`avg_solve_ms=0.448`。
- TIER2：380/383 OK，3 个 ERROR 均为历史 `unsupported_non_binary_extension`。
- v3.11 absolute：`solve_ms p50=0.470 p95=3.202`。
- 相对 v3.9 `vebo_weighted local=16 replay=8`：
  - v3.9：`solve_ms p50=0.513 p95=3.357`；
  - v3.11：`solve_ms p50=0.470 p95=3.202`；
  - per-instance：45/76 个实例更快，25/76 个实例慢于 v3.9 超过 5%。
- dirty stats：
  - `cross_push_avoided_count sum=175752`；
  - `cta_cross_push_count sum=0`；
  - `dirty_pull_scan_count sum=110256`；
  - `dirty_pull_hit_count sum=67170`。
- Combined CTA gate：
  - `cta_vs_shared+flags p50=1.31x p95=3.00x`；
  - `cta_vs_best_worklist p50=1.48x p95=2.98x`；
  - `host_round_ratio_vs_baseline p50=1.00x p95=1.00x`；
  - `decision=report_only`。

## Decision

- `dirty_var_pull` 作为 default-off handoff 实验路径保留。
- 它证明跨 owner push 可被 host pull 替代且 correctness 成立，并小幅改善 v3.9 CTA
  p50/p95。
- 但 shared+flags / best worklist gate 仍被拒，`auto` 不读取该路径。
- 下一步若继续 CTA，应只做更窄的 bucket policy 或 owner locality/hub 类实验；
  若目标是默认性能，应回到 `shared+worklist`/flags 的 dispatch 成本优化。
