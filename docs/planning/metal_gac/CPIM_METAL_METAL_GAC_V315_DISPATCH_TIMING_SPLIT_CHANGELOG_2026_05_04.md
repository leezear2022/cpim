---
status: active
updated: 2026-05-04T10:42:48Z
type: changelog
topic: cpim-metal
slug: metal-gac-v315-dispatch-timing-split
stage: s07
---

# Metal GAC v3.15 Dispatch Timing Split Changelog

## Summary

- Implemented additive dispatch timing split for Metal GAC.
- TIER2 evidence shows the baseline GAC path is dominated by command-buffer
  non-kernel time, not CPU encode time.
- No runtime policy or `solve_ms` semantics changed.

## Changes

- `include/solver/metal/metal_runtime.h`
  - Added `MetalDispatchTimings::encode_ms`.
- `src/solver/metal/metal_runtime.mm`
  - Measures command encoding wall time in compute dispatch and blit reset helpers.
  - Keeps `wall_ms` as command buffer submit/wait time.
- `include/solver/metal/metal_gac_solver.h`
  - Added `dispatch_encode_ms`, `dispatch_wait_ms`, and
    `dispatch_non_kernel_ms` to `MetalGacStats`.
- `src/solver/metal/metal_gac_solver.mm`
  - Accumulates encode/wait/non-kernel timing while preserving existing
    `dispatch_ms` and `kernel_ms`.
- `apps/benchmark_metal_gac.cpp`
  - CSV and summary now emit the three dispatch split fields.
- `tests/python/metal_gac_ablation.py`
  - Preserves the new fields in ablation CSV.
- `tests/python/metal_gac_analyze.py`
  - Mode summary shows encode/wait/non-kernel totals and shares.
  - Added `[dispatch timing split]` with per-dispatch timing.
  - Runtime policy summary now marks the v3.14 allowlist as `decision=eligible`
    when selected rows beat fallback and have no regression rows.

## Validation

- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py codex-docops-logic/scripts/dol.py`
- `cmake --build build_metal --target benchmark_metal_gac -j8`
- Single fixture:
  - `out/metal_gac_v315_dispatch_timing_split_single.csv`
  - CPU/Metal verify passed.
  - analyzer showed `non_kernel_share=0.94` on `gac_bitwords2.xml`.
- TIER2:
  - `out/metal_gac_v315_dispatch_timing_split_tier2.csv`
  - 380/383 OK; 3 errors are historical `unsupported_non_binary_extension`.
  - shared+flags baseline: `solve_ms p50=0.236 p95=5.403`.
  - dispatch split:
    - `encode=0.013ms`;
    - `wait=0.773ms`;
    - `kernel=0.105ms`;
    - `non_kernel=0.668ms`;
    - `encode_per_dispatch=0.0039ms`;
    - `wait_per_dispatch=0.2356ms`;
    - `kernel_per_dispatch=0.0321ms`;
    - `non_kernel_per_dispatch=0.2035ms`;
    - `non_kernel_share=0.86`.
- v3.14 allowlist re-analysis:
  - selected-vs-shared+flags `p50=0.62x p95=0.68x`;
  - better `4/4`;
  - regressions over 1.05 threshold: 0;
  - analyzer decision: `eligible`.

## Decisions

- The dominant Metal GAC cost is not CPU-side encode; it is command buffer wait /
  non-kernel overhead around short kernels.
- Do not spend the next step on micro-optimizing encode code paths.
- Keep v3.14 allowlist default-off but evidence-backed for BH-4-4; do not promote CTA
  globally.
- Prefer next-stage work that amortizes dispatch cost: Batch/SAC multi-instance
  throughput, command buffer fusion where semantics allow, or a purpose-built
  persistent/batched GAC probe.

## Follow-Ups

- Add a default-off batch throughput benchmark before widening any runtime GAC policy.
- If continuing GAC-only, test command-buffer fusion for repeated host rounds before
  adding more kernel variants.
- Keep dispatch split fields in future CSVs so CTA/bulk experiments can be explained by
  kernel work versus non-kernel overhead.

## Links

- plan:
  [Metal GAC v3.15 Dispatch Timing Split Plan](CPIM_METAL_METAL_GAC_V315_DISPATCH_TIMING_SPLIT_PLAN_2026_05_04.md)
- roadmap:
  [Metal GAC Long Term Optimization](../METAL_GAC_LONG_TERM_OPTIMIZATION.md)
