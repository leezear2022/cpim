---
status: active
updated: 2026-05-03T15:09:49Z
type: changelog
topic: cpim-metal
slug: metal-gac-v37-seed-overflow
stage: s07
---

# Metal GAC v3.7 Seed/Overflow Changelog

## Summary

- 新增 default-off `--cta_queue_mode=spill_replay`。
- 拆分 CTA overflow 语义，区分 local queue 真满、local round budget spill 与
  host seed overflow。
- 新增 seed owner balance 统计，用于解释初始 active list 是否天然偏斜。
- `cta_worklist` 继续 report-only，不进入 `auto`。

## Changes

- `MetalGacOptions` 新增 `MetalCtaQueueMode`。
- `MetalGacStats`、benchmark CSV/stdout、ablation CSV 新增：
  - `cta_queue_mode`
  - `cta_queue_overflow_count`
  - `cta_budget_spill_count`
  - `cta_seed_overflow_count`
  - `seed_owner_nonempty_count`
  - `seed_empty_owner_count`
  - `seed_max_owner_load`
  - `seed_owner_balance_p95`
- `gac_revise_cta_worklist_kernel`：
  - local queue slot 超 capacity 计入 `cta_queue_overflow_count`；
  - `spill_replay` 下 local budget 用完后的 pending work 计入
    `cta_budget_spill_count`；
  - `cta_overflow_count` 继续作为兼容总数。
- Host `SeedCtaQueues()` 记录 per-round owner load 与 seed overflow。
- `metal_gac_analyze.py`：
  - mode summary 显示 owner/queue mode 与拆分 stats；
  - CTA gate 按 `owner_mode + queue_mode` 分组；
  - 新 CSV 使用 queue/seed overflow 判断 `overflow_ok`，旧 CSV 回退到
    `cta_overflow_count`。

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

- metal-smoke：15/15 OK。
- TIER2：380/383 OK，3 个 ERROR 均为历史 `unsupported_non_binary_extension`。
- `spill_replay` TIER2：`solve_ms p50=0.478 p95=3.853`，
  `metal_cpu_solve_ratio p50=60.02x p95=403.55x`。
- Combined CTA gate：
  - `queue_overflow_p95=0`；
  - `seed_overflow_p95=0`；
  - `budget_spill_p95=1`；
  - `host_round_ratio_vs_baseline p50=1.00x`；
  - `decision=report_only`。
- 结论：seed/queue 真 overflow 不是主因；剩余问题集中在 local budget spill 与
  host round 未下降。

## Decisions

- `spill_replay` 不进入 `auto`。
- 下一步优先做 local budget / bounded replay 评估；若 host round 仍不降，再进入
  `vebo_weighted_owner`。

## Links

- plan:
  [Metal GAC v3.7 Seed/Overflow Plan](METAL_GAC_V37_SEED_OVERFLOW_PLAN_2026_05_03.md)
- roadmap:
  [Metal GAC Long Term Optimization](../METAL_GAC_LONG_TERM_OPTIMIZATION.md)
