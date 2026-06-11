---
status: active
updated: 2026-05-04T11:07:20Z
type: changelog
topic: cpim-metal
slug: metal-batch-sac-v316-batch-probe
stage: s07
---

# Metal Batch/SAC v3.16 Batch Probe Changelog

## Summary

- Implemented the default-off Metal Batch/SAC batch probe benchmark MVP.
- The first BH smoke shows batch probes can strongly amortize command-buffer
  overhead: `dispatch_per_probe=0.0182` and `non_kernel_per_probe≈0.0038ms`.
- TIER2 confirms the same default-off path is correctness-clean on supported Metal
  binary-extension rows: 380/383 OK, 3 historical unsupported rows, 0 status
  mismatches, 0 UNKNOWN probes.
- No GAC `auto` policy, runtime allowlist, CUDA path, or search/SAC preprocess path
  changed.

## Changes

- Added `MetalBatchProbeRunner` and public Metal SAC probe types.
- Added Metal kernels:
  - `sac_probe_init_kernel`;
  - `sac_probe_revise_kernel`;
  - `sac_probe_frontier_kernel`;
  - `sac_probe_mark_unknown_kernel`.
- Added `benchmark_metal_sac`:
  - runs stable Metal GAC to obtain the AC snapshot;
  - generates singleton probe worlds from remaining snapshot values;
  - verifies probe status against a CPU reference when requested;
  - writes a dedicated CSV separate from Metal GAC data.
- Added `tests/python/metal_sac_ablation.py` for metal-smoke and tier scans.
- Added CMake wiring for the new benchmark target and reused the existing
  `metal_gac.metallib`.

## Validation

- `python3 -m py_compile tests/python/metal_sac_ablation.py codex-docops-logic/scripts/dol.py`
- `cmake -S . -B build_metal`
- `cmake --build build_metal --target benchmark_metal_sac -j8`
- Single fixture:
  - `out/metal_sac_v316_batch_probe_single.csv`
  - 2 probes, 2 OK, 0 DWO, 0 UNKNOWN;
  - CPU/Metal status verify passed.
- BH smoke:
  - `out/metal_sac_v316_batch_probe_bh.csv`
  - measured runs: 3;
  - 384 probes per run;
  - 384 OK, 0 DWO, 0 UNKNOWN;
  - CPU/Metal status verify passed;
  - `dispatch_per_probe=0.0182292`;
  - `non_kernel_per_probe≈0.0037-0.0039ms`;
  - `probes_per_sec≈195k-202k`.
- metal-smoke suite:
  - `out/metal_sac_v316_batch_probe_smoke.csv`
  - 15/15 OK rows;
  - `avg_dispatch_per_probe=0.9536`;
  - small fixtures remain too small to amortize dispatch reliably.
- TIER2 evidence:
  - `out/metal_sac_v316_batch_probe_tier2.csv`
  - 380/383 OK rows;
  - 3 ERROR rows are historical `unsupported_non_binary_extension` inputs;
  - supported rows verified 32 probes/run with `verify_mismatches=0`;
  - `UNKNOWN` probes: 0;
  - total measured probes: 163,605;
  - `dispatch_per_probe avg=0.0527 p50=0.0182 p95=0.1402`;
  - `non_kernel_per_probe avg=0.014634ms p50=0.005242ms p95=0.023366ms`;
  - `probes_per_sec avg≈144k p50≈146k p95≈255k`.

## Decisions

- Keep v3.16 as a benchmark/probe path only.
- Treat TIER2 and BH as positive evidence that batch singleton probes can amortize
  command-buffer fixed cost, especially on 512-probe and BH-like buckets.
- Continue to require CPU reference status checks before any DWO probe is allowed to
  delete values in a future SAC preprocess.

## Follow-Ups

- If v3.17 proceeds, implement host-side NSACQ with queue budget, allowed
  constraints mask, and DWO writeback after each batch.
- If TIER2 fails to amortize dispatch outside BH-like buckets, keep Metal SAC as a
  report-only throughput probe and investigate command-buffer fusion or CUDA SAC
  hardening.

## Links

- plan:
  [Metal Batch/SAC v3.16 Batch Probe Plan](CPIM_METAL_METAL_BATCH_SAC_V316_BATCH_PROBE_PLAN_2026_05_04.md)
- roadmap:
  [Metal GAC Long Term Optimization](../METAL_GAC_LONG_TERM_OPTIMIZATION.md)
