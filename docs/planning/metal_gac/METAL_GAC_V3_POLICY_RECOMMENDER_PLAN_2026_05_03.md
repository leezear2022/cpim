---
status: active
updated: 2026-05-03T07:42:45Z
type: plan
topic: cpim-metal
slug: metal-gac-v3-policy-recommender
stage: s07
---

# Metal GAC v3 Policy Recommender Plan

## Goal

- 启动 Metal GAC v3，主线为 performance evidence、report-only policy
  recommender 与 simdgroup/threadgroup staging gate。
- 不改变 v2 封版默认路径；`cold + shared + flags + scalar + pair + cpu reset`
  继续作为 stable fallback。
- 用可复跑 CSV 数据判断哪些实例适合 `worklist/word_parallel/directional/blit`，
  以及 simdgroup 是否值得进入真实 kernel 实现。

## Scope

- 包含：
  - v2 regression/evidence guard；
  - `metal_gac_analyze.py` 瓶颈分解与推荐报告；
  - v3 simdgroup/threadgroup 是否进入实现的证据门槛。
- 不包含：
  - SAC/Batch 迁移；
  - global AllDifferent、predicate/intension、非二元 extension 支持；
  - 直接修改 `frontier_mode=auto` 或 `kernel_variant=auto` 的 effective default。

## Tasks

- v3.0 regression/evidence guard：
  - 固定 v2 baseline、auto、explicit aggressive 三组 smoke 命令；
  - 保留 unsupported 分类，不把 unsupported non-binary/global/predicate
    计入性能失败；
  - 每次 v3 改动后继续能回退到 v2 fallback。
- v3.1 analyzer bottleneck model：
  - 从现有 CSV 推导 `kernel_share`、`dispatch_share`、`reset_share`；
  - 对 worklist 输出 `worklist_push_per_round`；
  - 按 family、constraint 数、domain size、bit_words、frontier density 分桶。
- v3.2 report-only policy recommender：
  - 新增 `--recommend-policy`；
  - 新增 `--baseline-mode shared+flags`、`--regression-threshold 1.05`、
    `--min-runs 3`；
  - 输出推荐路径、命中实例数、p50/p95 比值、超过阈值的劣化清单与置信度。
- v3.3 simdgroup/threadgroup gate：
  - 先不实现复杂 simdgroup kernel；
  - 只有当连续 tier 数据显示 `kernel_ms` p95 明显高于 dispatch/reset，且
    word-parallel 数据指向 bitSup intersection 或 domain atomic 瓶颈时，才单独
    进入 simdgroup/threadgroup staging 实现计划。

## Validation

- 静态：
  - `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py codex-docops-logic/scripts/dol.py`
  - `git diff --check`
  - `python3 codex-docops-logic/scripts/dol.py lint --soft`
- v2 guard：
  - `ctest --test-dir build_metal -R compare_cpu_metal --output-on-failure`
  - `ctest --test-dir build_metal -R benchmark_metal_gac --output-on-failure`
  - `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=auto --runs=3 --warmup=1 --runner-mode=prepared --timeout=60 --csv=out/metal_gac_v3_guard_auto.csv`
  - `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=1 --warmup=0 --verify=true --runner_mode=prepared --frontier_mode=worklist --kernel_variant=word_parallel --bitsup_layout=directional --reset_mode=blit --csv=out/metal_gac_v3_guard_aggressive.csv`
- recommender evidence：
  - `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=all --runs=5 --warmup=2 --runner-mode=prepared --timeout=300 --csv=out/metal_gac_v3_tier2_all.csv --quiet`
  - `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=auto --runs=5 --warmup=2 --runner-mode=prepared --timeout=300 --csv=out/metal_gac_v3_tier2_auto.csv --quiet`
  - `python3 tests/python/metal_gac_analyze.py out/metal_gac_v3_tier2_all.csv out/metal_gac_v3_tier2_auto.csv --top=10 --recommend-policy --baseline-mode shared+flags --regression-threshold 1.05 --min-runs 3`

## Rollback

- Recommender 只是报告输出，删除或不传 `--recommend-policy` 即回到旧分析行为。
- benchmark CSV schema 不变；solver API 和默认 effective path 不变。
- 如果 v3 recommendation 结论不稳定，继续保持 v2 `auto -> flags+scalar+pair`。

## Links

- changelog:
  [Metal GAC v3 Policy Recommender Changelog](METAL_GAC_V3_POLICY_RECOMMENDER_CHANGELOG_2026_05_03.md)
- roadmap:
  [Metal GAC Long Term Optimization](../METAL_GAC_LONG_TERM_OPTIMIZATION.md)
