---
status: active
updated: 2026-05-03T07:42:45Z
type: changelog
topic: cpim-metal
slug: metal-gac-v3-policy-recommender
stage: s07
---

# Metal GAC v3 Policy Recommender Changelog

## Summary

- 启动 Metal GAC v3，roadmap 从 `v02` bump 到 `v03`。
- v3 主线落在 evidence/recommender/simdgroup gate，不切换到 SAC/Batch。
- 首个实现是 report-only policy recommender；不修改 Metal solver 默认路径。

## Changes

- `tests/python/metal_gac_analyze.py`：
  - 新增 `--recommend-policy`；
  - 新增 `--baseline-mode`、`--regression-threshold`、`--min-runs`；
  - mode summary 新增 `kernel_share`、`dispatch_share`、`reset_share`、
    `worklist_push_per_round`；
  - recommender 输出全局 candidate、bucket candidate、baseline bottleneck counts
    与超过阈值的 policy regression 样例。
- 文档：
  - 新增本 v3 小计划与小 changelog；
  - 更新 Metal GAC changelog 索引；
  - 更新长期路线、迁移计划和 `CHANGES_ZH.md`。
- DocOps：
  - `cpim-metal` roadmap bump 到 `v03`，事件保留 `rm.hist`；
  - 追加 v3 change/validation evidence。

## Validation

- `python3 -m py_compile tests/python/metal_gac_analyze.py`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v2x_tier2_all.csv out/metal_gac_v2x_tier2_auto.csv --top=3 --recommend-policy --baseline-mode shared+flags --regression-threshold 1.05 --min-runs 3`
- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py codex-docops-logic/scripts/dol.py`
- `git diff --check`
- `python3 codex-docops-logic/scripts/dol.py lint --soft`
- `ctest --test-dir build_metal -R compare_cpu_metal --output-on-failure`
- `ctest --test-dir build_metal -R benchmark_metal_gac --output-on-failure`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=auto --runs=3 --warmup=1 --runner-mode=prepared --timeout=60 --csv=out/metal_gac_v3_guard_auto.csv`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=1 --warmup=0 --verify=true --runner_mode=prepared --frontier_mode=worklist --kernel_variant=word_parallel --bitsup_layout=directional --reset_mode=blit --csv=out/metal_gac_v3_guard_aggressive.csv`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=all --runs=5 --warmup=2 --runner-mode=prepared --timeout=300 --csv=out/metal_gac_v3_tier2_all.csv --quiet`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=auto --runs=5 --warmup=2 --runner-mode=prepared --timeout=300 --csv=out/metal_gac_v3_tier2_auto.csv --quiet`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v3_tier2_all.csv out/metal_gac_v3_tier2_auto.csv --top=10 --recommend-policy --baseline-mode shared+flags --regression-threshold 1.05 --min-runs 3`

## Results

- `compare_cpu_metal`：5/5 passed。
- `benchmark_metal_gac`：1/1 passed。
- v3 auto guard：15/15 OK，`shared+auto` prepared 平均 `solve_ms=0.534`。
- v3 explicit aggressive smoke：CPU verify 通过，effective path 为
  `worklist+word_parallel+directional`，`worklist_rounds=2`，
  `worklist_epoch_resets=0`。
- v2x CSV recommender 验证显示 baseline bottleneck counts 为 `dispatch=76`；
  当前 evidence 不支持直接进入 simdgroup kernel 实现。
- v3 TIER2 all：2280/2298 rows OK，18 个 ERROR 均为
  `unsupported_non_binary_extension`；baseline `shared+flags+scalar+pair`
  `solve_ms p50=0.290 p95=4.995`。
- v3 TIER2 auto：380/383 rows OK，3 个 ERROR 均为
  `unsupported_non_binary_extension`；`shared+auto` effective path 仍为
  `flags+scalar+pair`，`solve_ms p50=0.223 p95=6.293`。
- v3 combined recommender：baseline bottleneck counts 为 `dispatch=76`；
  global `shared+auto` candidate 的 `p50_ratio=0.78`、`p95_ratio=1.54`，
  且 19 个实例超过 5% regression threshold，因此不能提升 auto policy。
- 高置信 bucket 只出现在局部实例族，例如 `composed-25-1-2`
  低密度 bucket 推荐 `private+worklist`，`p50_ratio=0.74 p95_ratio=0.93`；
  `tightness0.2` 和部分 `tightness0.9` bucket 推荐 `shared+auto`，但仍只作为报告。

## Decisions

- v3 初期 recommender 只输出报告，不改变 `auto` effective path。
- 当前 v2x evidence 显示 baseline bottleneck 主要仍是 dispatch；因此 simdgroup
  不在本次直接实现。
- v3 TIER2 evidence 再次显示瓶颈是 dispatch，而不是 kernel；simdgroup/threadgroup
  staging 继续留在 gate 后，不进入实现。
- `shared+auto` p50 明显好于 baseline，但 p95 regression 超过 5%，所以 v3
  不改变 effective auto policy。
- 性能主指标继续使用 `solve_ms`；`prepare_ms/setup_ms` 不进入主指标。

## Follow-Ups

- 下一步应把 report-only recommender 收敛成 machine-readable 输出，例如
  JSON/CSV summary，便于后续 CI 或文档自动引用。
- 若连续两组 tier evidence 证明 `kernel_ms` p95 成为主瓶颈，再新增
  simdgroup/threadgroup staging 小计划。

## Links

- plan:
  [Metal GAC v3 Policy Recommender Plan](METAL_GAC_V3_POLICY_RECOMMENDER_PLAN_2026_05_03.md)
- roadmap:
  [Metal GAC Long Term Optimization](../METAL_GAC_LONG_TERM_OPTIMIZATION.md)
