---
status: active
updated: 2026-05-03T15:09:49Z
type: plan
topic: cpim-metal
slug: metal-gac-v37-seed-overflow
stage: s07
---

# Metal GAC v3.7 Seed/Overflow First Plan

## Goal

- 在进入 `vebo_weighted_owner` 前，先拆清 CTA worklist 的 seed/overflow 瓶颈。
- 判断 v3.6 `owner_map_static` 未过 gate 的原因，是 queue/seed/budget 协议，
  还是 owner mapping 本身仍不够好。
- 所有改动保持 default-off，不改变 `frontier_mode=auto`、默认 owner 或 stable
  fallback。

## Scope

- 新增 `--cta_queue_mode=local_only|spill_replay`，默认 `local_only`。
- 拆分 CTA overflow stats：
  - `cta_queue_overflow_count`
  - `cta_budget_spill_count`
  - `cta_seed_overflow_count`
  - `cta_overflow_count` 继续保留为兼容总数。
- 新增 seed owner stats：
  - `seed_owner_nonempty_count`
  - `seed_empty_owner_count`
  - `seed_max_owner_load`
  - `seed_owner_balance_p95`
- 不实现 `vebo_weighted_owner`，不调整 queue capacity 布局，不改变 auto policy。

## Validation

- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py codex-docops-logic/scripts/dol.py`
- `cmake --build build_metal --target benchmark_metal_gac -j8`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=1 --warmup=0 --verify=true --runner_mode=prepared --frontier_mode=cta_worklist --kernel_variant=word_parallel --bitsup_layout=directional --cta_owner_mode=static_edge_cut --cta_queue_mode=spill_replay --csv=out/metal_gac_v37_seed_overflow_single.csv`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=cta --runs=3 --warmup=1 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=static_edge_cut --cta-queue-mode=spill_replay --cpu-timing --timeout=60 --csv=out/metal_gac_v37_seed_overflow_smoke.csv`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=cta --runs=5 --warmup=2 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=static_edge_cut --cta-queue-mode=spill_replay --cpu-timing --timeout=300 --csv=out/metal_gac_v37_seed_overflow_tier2.csv --quiet`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v35_tier2_baseline.csv out/metal_gac_v35_tier2_frontier.csv out/metal_gac_v36_owner_map_static_tier2.csv out/metal_gac_v37_seed_overflow_tier2.csv --top=10 --recommend-policy --baseline-mode shared+flags --regression-threshold 1.05 --min-runs 3`
- `git diff --check`
- `python3 codex-docops-logic/scripts/dol.py lint --soft`

## Results

- single smoke：CPU verify 通过，三类拆分 overflow 均为 0。
- metal-smoke：15/15 OK。
- TIER2：380/383 OK，3 个 ERROR 均为历史 `unsupported_non_binary_extension`。
- TIER2 `spill_replay`：`solve_ms p50=0.478 p95=3.853`，
  `metal_cpu_solve_ratio p50=60.02x p95=403.55x`。
- Combined gate：
  - `cta_vs_shared+flags p50=1.41x p95=3.54x`；
  - `cta_vs_best_worklist p50=1.49x p95=3.46x`；
  - `host_round_ratio_vs_baseline p50=1.00x p95=1.00x`；
  - `cta_queue_overflow_count p95=0`；
  - `cta_seed_overflow_count p95=0`；
  - `cta_budget_spill_count p95=1`；
  - `decision=report_only`。
- 结论：v3.6 的 overflow 主要不是 seed/queue 真满，而是 local budget spill。
  继续做 seed/queue 分配收益有限；下一步应优先评估 local budget / bounded replay，
  若仍不降 host round，再进入 `vebo_weighted_owner`。

## Rollback

- 不传 `--cta_queue_mode=spill_replay` 即回到旧 `local_only`。
- 不传 `--cta_owner_mode=static_edge_cut` 即回到旧 `modulo` owner。
- `frontier_mode=auto` 不读取 v3.7 新路径。
