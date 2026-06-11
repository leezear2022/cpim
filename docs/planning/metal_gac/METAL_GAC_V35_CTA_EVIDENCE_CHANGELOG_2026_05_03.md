---
status: completed
updated: 2026-05-03
type: changelog
topic: cpim-metal
slug: metal-gac-v35-cta-evidence
stage: s07
---

# Metal GAC v3.5 CTA Evidence / Auto Gate Changelog

## Summary

- 增加 `cta_worklist` 的 evidence gate，不改变默认 fallback 或 `auto`。
- analyzer 新增 `[cta worklist gate]` report-only section。
- 本轮结论必须基于 TIER2 p50/p95/p99 与 CPU verify，不凭 smoke 提升策略。

## Changes

- `metal_gac_analyze.py`：
  - 输出 `cta_worklist` 相对 baseline 的 p50/p95/p99 ratio；
  - 输出 `cta_worklist` 相对旧 worklist 的 p50/p95 ratio；
  - 输出 host round、CTA queue/cross/overflow 和 Metal/CPU ratio gate；
  - 输出 `decision=eligible|report_only` 与失败原因。
- 新增独立小计划：
  `docs/planning/metal_gac/METAL_GAC_V35_CTA_EVIDENCE_PLAN_2026_05_03.md`。
- 更新 Metal GAC 索引、长期路线、迁移计划和 `CHANGES_ZH.md`。

## Validation

- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py codex-docops-logic/scripts/dol.py`
- `cmake --build build_metal --target benchmark_metal_gac compare_cpu_metal -j`
- `ctest --test-dir build_metal -R compare_cpu_metal --output-on-failure`
- `ctest --test-dir build_metal -R benchmark_metal_gac --output-on-failure`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=cta --runs=3 --warmup=1 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cpu-timing --timeout=60 --csv=out/metal_gac_v35_smoke_cta.csv`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=baseline --runs=5 --warmup=2 --runner-mode=prepared --cpu-timing --timeout=300 --csv=out/metal_gac_v35_tier2_baseline.csv --quiet`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=frontier --runs=5 --warmup=2 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cpu-timing --timeout=300 --csv=out/metal_gac_v35_tier2_frontier.csv --quiet`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=cta --runs=5 --warmup=2 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cpu-timing --timeout=300 --csv=out/metal_gac_v35_tier2_cta.csv --quiet`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v35_tier2_baseline.csv out/metal_gac_v35_tier2_frontier.csv out/metal_gac_v35_tier2_cta.csv --top=10 --recommend-policy --baseline-mode shared+flags --regression-threshold 1.05 --min-runs 3`
- `git diff --check`
- `python3 codex-docops-logic/scripts/dol.py lint --soft`

## Results

- metal-smoke CTA：15/15 OK，Metal faster `0/15`。
- TIER2 baseline：380/383 rows OK，3 个 ERROR 均为
  `unsupported_non_binary_extension`；`shared+flags` `solve_ms p50=0.253 p95=5.109`，
  Metal/CPU ratio `p50=30.69x p95=524.36x`。
- TIER2 frontier：1140/1149 rows OK，9 个 ERROR 均为历史 unsupported；
  `shared+worklist` `solve_ms p50=0.328 p95=5.351`。
- TIER2 CTA：380/383 rows OK，3 个 ERROR 均为历史 unsupported；
  `cta_worklist` `solve_ms p50=0.453 p95=5.356`，Metal/CPU ratio
  `p50=52.43x p95=487.58x`，Metal faster `0/380`。
- Combined `[cta worklist gate]`：
  - `cta_vs_shared+flags p50=1.27x p95=2.99x p99=4.04x`；
  - `cta_vs_best_worklist p50=1.37x p95=2.99x`；
  - `host_round_ratio_vs_baseline p50=1.00x p95=1.00x`；
  - `cta_overflow_count p95=0 max=0`；
  - `decision=report_only`，原因：
    `baseline_p95_regression,worklist_p95_regression,host_round_not_reduced`。

## Decisions

- `auto` 保持保守路径，不因 smoke 通过而启用 `cta_worklist`。
- `cta_worklist` 未通过 TIER2 p95 gate，保持 report-only。
- CTA 没有降低 TIER2 `host_round_count`；下一步不应继续沿单实例 CTA 方向
  硬推默认策略。
- 若继续优化 Metal，应优先转向 Batch/SAC 多任务吞吐或重新设计 owner partition，
  而不是进入 simdgroup。

## Links

- plan:
  [Metal GAC v3.5 CTA Evidence Plan](METAL_GAC_V35_CTA_EVIDENCE_PLAN_2026_05_03.md)
- roadmap:
  [Metal GAC Long Term Optimization](../METAL_GAC_LONG_TERM_OPTIMIZATION.md)
