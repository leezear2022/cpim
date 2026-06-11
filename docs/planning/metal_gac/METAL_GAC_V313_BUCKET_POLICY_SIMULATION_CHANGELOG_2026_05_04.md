---
status: active
updated: 2026-05-04T00:00:00Z
type: changelog
topic: cpim-metal
slug: metal-gac-v313-bucket-policy-simulation
stage: s07
---

# Metal GAC v3.13 Bucket Policy Simulation Changelog

## Summary

- `metal_gac_analyze.py --recommend-policy` 新增 report-only bucket policy
  simulation。
- analyzer 用完整候选路径分组，避免不同 CTA 参数和不同 baseline kernel/layout 混读。
- combined TIER2 evidence 显示只选择 BH-4-4 bucket 可降低整体 p95，且没有
  regression rows。

## Changes

- 新增 `candidate_key()` / `candidate_name()`：
  - non-CTA 路径区分 storage/frontier/kernel/bitsup/reset；
  - CTA 路径额外区分 owner/queue/handoff/local/replay/dirty threshold。
- 新增 `print_bucket_policy_simulation()`：
  - 按 feature bucket 统计候选路径相对 fallback 的 p50/p95/regression；
  - 只把 `p95 <= 1.0x` 且 regression 为 0 的 bucket 标记为 eligible；
  - 模拟 eligible bucket 选择候选路径，其余输入回退 fallback。
- 新增 CLI：
  - `--bucket-min-instances`
- `--recommend-policy` 输出新增 `[bucket policy simulation]` 与 `[eligible buckets]`。

## Validation

- `python3 -m py_compile tests/python/metal_gac_analyze.py tests/python/metal_gac_ablation.py codex-docops-logic/scripts/dol.py`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v35_tier2_baseline.csv out/metal_gac_v35_tier2_frontier.csv out/metal_gac_v39_vebo_weighted_local16_tier2.csv out/metal_gac_v311_dirty_var_pull_tier2.csv out/metal_gac_v312_dirty_pull_hybrid8_tier2.csv --top=10 --recommend-policy --baseline-mode shared+flags --regression-threshold 1.05 --min-runs 3 --bucket-min-instances 3`

## Results

- eligible bucket count：1。
- selected inputs：4/76。
- policy absolute：`p50=0.312ms p95=1.696ms`。
- fallback absolute：`p50=0.312ms p95=2.987ms`。
- policy relative：`p50=1.00x p95=1.00x`，`better=4/76`，
  `regressions_gt_threshold=0`。
- eligible bucket：
  - `BH-4-4` feature bucket；
  - CTA hybrid8 path；
  - `p50=0.33x p95=0.34x`。

## Decision

- v3.13 不改变 runtime `auto`。
- bucket simulation 作为 report-only evidence 保留。
- 若后续产品化，只允许显式 allowlist bucket，而不是泛化为全局 CTA。
