---
status: active
updated: 2026-05-09T03:02:08Z
type: changelog
topic: cpim-metal
slug: metal-sac-v317-host-nsacq
stage: s07
---

# Metal SAC v3.17 Host NSACQ

## Summary

- Implemented a default-off host-side NSACQ prototype on top of the v3.16 Metal
  batch probe runner.
- The new path can batch singleton probes, write back DWO deletions to the host
  snapshot, run stable Metal GAC after delete batches, and continue until the host
  queue drains or a budget is reached.
- Existing GAC auto policy, v3.14 allowlist, CUDA path, and default
  `batch_probe` behavior remain unchanged.

## Changes

- Added `MetalSacMode { batch_probe, nsacq, sacq_adj, sacq_full }`.
- Added optional `allowed_constraints` mask to `MetalBatchProbeOptions`.
- Updated Metal SAC kernels so init, revise, and subscription enqueue respect the
  allowed-constraints mask.
- Kept `nsacq` on neighbor activation plus full downstream propagation; the
  allowed-constraints mask is reserved for the more aggressive `sacq_adj` branch.
- Extended `benchmark_metal_sac`:
  - `--sac_mode=batch_probe|nsacq|sacq_adj|sacq_full`;
  - `--max_sac_batches`;
  - `--outer_queue_budget`;
  - host-side queue over remaining `(var,value)` candidates;
  - CPU-confirmed DWO writeback and post-delete stable Metal GAC;
  - CSV fields for NSACQ queue/delete/GAC statistics.
- Extended `tests/python/metal_sac_ablation.py` to pass the new SAC mode and
  queue budget flags.

## Validation

- `cmake -S . -B build_metal`
- `cmake --build build_metal --target benchmark_metal_sac -j8`
- v3.16 regression:
  - `out/metal_sac_v317_batch_probe_regression_single.csv`
  - 2 probes, 2 OK, 0 DWO, 0 UNKNOWN, CPU/Metal status verify passed.
- NSACQ single:
  - `out/metal_sac_v317_nsacq_single.csv`
  - 2 probes, 2 OK, 0 DWO, 0 UNKNOWN, CPU/Metal status verify passed.
- NSACQ DWO/writeback smoke:
  - `out/metal_sac_v317_nsacq_queens4.csv`
  - 24 probes, 16 OK, 8 DWO, 0 UNKNOWN;
  - 8 host snapshot values deleted;
  - 1 post-delete stable Metal GAC run;
  - CPU/Metal status verify passed.
- NSACQ metal-smoke:
  - `out/metal_sac_v317_nsacq_smoke.csv`
  - 5/5 OK;
  - 178 probes;
  - 8 batches;
  - 10 DWO values written back;
  - 0 rejected DWO guards.
- NSACQ BH smoke:
  - `out/metal_sac_v317_nsacq_bh.csv`
  - 384 probes/run, 3 measured runs;
  - 0 DWO, 0 UNKNOWN;
  - `dispatch_per_probe=0.0182292`;
  - CPU/Metal status verify passed.
- NSACQ TIER2:
  - `out/metal_sac_v317_nsacq_tier2.csv`
  - 228/231 OK rows;
  - 3 ERROR rows are historical `unsupported_non_binary_extension`;
  - total measured probes: 278,172;
  - `dispatch_per_probe avg=0.0613 p50=0.0219 p95=0.1117`;
  - `non_kernel_per_probe avg=0.035141ms p50=0.014175ms p95=0.042302ms`;
  - `UNKNOWN` probes: 0;
  - confirmed DWO writebacks: 12,094;
  - rejected unconfirmed Metal DWO: 4,970;
  - final queue size: 0 for all supported rows.

## Decisions

- Keep v3.17 default-off and benchmark-only.
- Treat DWO writeback as safe only when the Metal status matches the CPU reference
  under the same allowed-constraints mask.
- UNKNOWN never deletes a value.
- In verified runs, an unconfirmed Metal DWO is rejected and not written back.
- TIER2 is a reject signal for promoting raw Metal DWO status: the CPU guard is
  required because async in-place probe propagation can produce more aggressive
  DWO reports on some instances.

## Follow-Ups

- If continuing SAC preprocess, first evaluate a deterministic/double-buffer probe
  path or keep CPU-confirmed DWO guard in the promote gate.
- Compare `sacq_adj` and `sacq_full` only after raw DWO status is deterministic
  enough or guarded.
- Do not connect NSACQ to search until rejected-DWO cost and correctness policy are
  resolved.

## Links

- plan:
  [Metal SAC v3.17 Host NSACQ Plan](CPIM_METAL_METAL_SAC_V317_HOST_NSACQ_PLAN_2026_05_09.md)
- roadmap:
  [Metal GAC Long Term Optimization](../METAL_GAC_LONG_TERM_OPTIMIZATION.md)
