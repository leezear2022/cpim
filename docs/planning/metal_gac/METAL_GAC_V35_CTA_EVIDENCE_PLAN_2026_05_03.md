---
status: completed
updated: 2026-05-03
type: plan
topic: cpim-metal
slug: metal-gac-v35-cta-evidence
stage: s07
---

# Metal GAC v3.5 CTA Evidence / Auto Gate Plan

## Goal

- 正式评估 `cta_worklist` 是否值得进入 recommender，甚至后续进入 `auto` 候选。
- 对比 `flags / worklist / cta_worklist`，只用 `solve_ms`、
  `metal_cpu_solve_ratio`、`host_round_count` 与 `cta_*` 统计做结论。
- 不修改默认 fallback，不修改 `frontier_mode=auto` 的 effective path。

## Tasks

- 跑三组 TIER2 evidence：
  - baseline：`shared + flags + scalar + pair`
  - frontier：`flags/compact/worklist + word_parallel + directional`
  - CTA：`shared + cta_worklist + word_parallel + directional`
- 增强 analyzer 的 report-only gate：
  - `cta_worklist` 相对 `shared+flags` 的 p50/p95/p99 ratio；
  - `cta_worklist` 相对旧 `worklist` 的 p50/p95 ratio；
  - `host_round_count` 相对 baseline 是否下降；
  - `cta_overflow_count`、`cta_cross_push_count`、`cta_queue_push_count`；
  - `metal_cpu_solve_ratio` 是否优于 baseline 对照。
- 更新 Metal GAC 索引、长期路线、迁移计划和 `CHANGES_ZH.md`。
- DocOps 追加 `ch/va`，roadmap 保持 `v03`。

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

## Gate

- CPU/Metal verification mismatch 必须为 0。
- historical unsupported non-binary extension 继续分类，不计入性能失败。
- `cta_worklist` 只有同时满足以下条件才进入下一轮 recommender 候选：
  - 相对 `shared+flags` 的 `solve_ms` p95 不慢于 5%；
  - 相对旧 `worklist` 的 p95 不差；
  - `host_round_count` p50 低于 baseline；
  - `cta_overflow_count` p95 为 0；
  - `metal_cpu_solve_ratio` p50 或 p95 至少一项优于 baseline。
- 若 p50 变快但 p95 变差超过 5%，结论为 report-only，不进入 `auto`。

## Links

- changelog:
  [Metal GAC v3.5 CTA Evidence Changelog](METAL_GAC_V35_CTA_EVIDENCE_CHANGELOG_2026_05_03.md)
- roadmap:
  [Metal GAC Long Term Optimization](../METAL_GAC_LONG_TERM_OPTIMIZATION.md)
