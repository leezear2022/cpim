---
status: active
updated: 2026-05-04T10:23:53Z
type: changelog
topic: cpim-metal
slug: metal-gac-v314-runtime-bucket-allowlist
stage: s07
---

# Metal GAC v3.14 Runtime Bucket Allowlist Changelog

## Summary

- Implemented default-off runtime `bh_cta_allowlist` policy for Metal GAC v3.14.
- The policy materializes the v3.13 report-only BH-4-4 bucket recommendation
  without changing `frontier_mode=auto` or stable fallback.

## Changes

- `apps/benchmark_metal_gac.cpp`
  - Added `--policy_mode=none|bh_cta_allowlist`.
  - Added runtime policy decision before constructing the Metal runner.
  - BH-4-4 shape-matched inputs now select CTA hybrid8:
    `cta_worklist + word_parallel + directional + vebo_weighted +
    bounded_replay + dirty_var_pull + local=16 + replay=8 + dirty_min=8`.
  - Non-matching inputs keep the requested options.
  - CSV and summary now record `policy_mode`, `policy_selected`,
    `policy_reason`, and `policy_bucket`.
- `tests/python/metal_gac_ablation.py`
  - Added `--policy-mode`.
  - Added `--mode-preset=bucket_policy`.
  - Preserves policy fields in ablation CSV.
- `tests/python/metal_gac_analyze.py`
  - Added runtime policy summary and selected-vs-fallback analysis.
  - Mode summary now displays policy mode, selected count, and bucket.

## Validation

- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py codex-docops-logic/scripts/dol.py`
- `cmake --build build_metal --target benchmark_metal_gac -j8`
- BH single:
  - policy selected true;
  - actual path `cta_worklist+word_parallel+directional`;
  - CPU/Metal verify passed.
- fallback single:
  - policy selected false;
  - reason `family_mismatch`;
  - actual path `flags+scalar+pair`;
  - CPU/Metal verify passed.
- small smoke with two BH inputs and one fallback input:
  - selected-vs-shared+flags `p50=0.53x p95=0.61x`;
  - regressions over 1.05 threshold: 0.
- TIER2:
  - `out/metal_gac_v314_runtime_bucket_allowlist_tier2.csv`
  - 380/383 OK; 3 errors are historical `unsupported_non_binary_extension`.
  - selected rows: 20, selected inputs: 4.
  - selected bucket: `BH-4-4 cons=128-511 dom<17 bitw<2`.
  - selected-vs-shared+flags: `p50=0.62x p95=0.68x`.
  - better: 4/4.
  - regressions over 1.05 threshold: 0.
  - analyzer runtime policy decision: `eligible`.

## Decisions

- Keep v3.14 default-off.
- Do not promote CTA globally.
- Treat CTA hybrid8 as a runtime allowlist candidate for BH-like propagation-heavy
  cases only.

## Follow-Ups

- If this policy remains stable across repeated Tier2/Tier3 evidence, consider
  adding a named report-only recommender output for this exact runtime rule.
- Do not widen the allowlist without a fresh bucket simulation and runtime
  regression check.
- Continue default-performance work on dispatch cost or Batch/SAC throughput.

## Links

- plan:
  [Metal GAC v3.14 Runtime Bucket Allowlist Plan](CPIM_METAL_METAL_GAC_V314_RUNTIME_BUCKET_ALLOWLIST_PLAN_2026_05_04.md)
- roadmap:
  [Metal GAC Long Term Optimization](../METAL_GAC_LONG_TERM_OPTIMIZATION.md)
