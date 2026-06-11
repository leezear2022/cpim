---
status: active
updated: 2026-05-09T03:02:08Z
type: plan
topic: cpim-metal
slug: metal-sac-v317-host-nsacq
stage: s07
---

# Metal SAC v3.17 Host NSACQ

## Goal

- Extend the v3.16 batch probe benchmark into a default-off host-side NSACQ
  prototype.
- Keep the scope benchmark-only: no search integration, no default policy change,
  and no mutation of `benchmark_metal_gac` / `frontier_mode=auto`.
- Validate the core SAC loop: batch singleton probes, DWO writeback to the host
  snapshot, stable Metal GAC after each delete batch, and conservative UNKNOWN
  handling.

## Scope

- Include:
  - `--sac_mode=batch_probe|nsacq|sacq_adj|sacq_full`;
  - allowed-constraints mask support in the Metal batch probe runner;
  - host-side candidate queue, queue budget, batch budget, DWO writeback, and
    post-delete GAC;
  - CSV fields for NSACQ batches, queue activity, delete count, rejected DWO
    guards, GAC reruns, and final domain size.
- Exclude:
  - full search integration;
  - persistent GPU-side SAC queue;
  - CUDA path changes;
  - `frontier_mode=auto` or v3.14 allowlist changes.

## Tasks

- Add `MetalSacMode`.
- Add optional `allowed_constraints` to `MetalBatchProbeOptions`.
- Make `sac_probe_init_kernel`, `sac_probe_revise_kernel`, and subscription
  enqueue respect the allowed-constraints mask.
- Extend `benchmark_metal_sac`:
  - default remains `batch_probe`;
  - `nsacq` keeps the v3.16 semantics: neighbor activation with full downstream
    propagation;
  - `sacq_adj` expands allowed vars to adjacent variables;
  - `sacq_full` allows all constraints and requeues all remaining values after a
    delete batch;
  - DWO clears the singleton value from the host snapshot;
  - in `--verify=true`, DWO writeback is guarded by a CPU reference confirmation;
  - every delete batch runs stable Metal GAC on the updated snapshot.
- Extend `tests/python/metal_sac_ablation.py` with `--sac-mode`,
  `--max-sac-batches`, and `--outer-queue-budget`.

## Validation

- Build:
  - `cmake -S . -B build_metal`
  - `cmake --build build_metal --target benchmark_metal_sac -j8`
- Correctness smoke:
  - `./build_metal/benchmark_metal_sac --input=tests/data/bench/queens-4_ext.xml --runs=1 --warmup=0 --probe_limit=64 --verify=true --verify_probe_limit=64 --activation_mode=neighbor --sac_mode=nsacq --max_sac_batches=100 --outer_queue_budget=0 --csv=out/metal_sac_v317_nsacq_queens4.csv`
- Suite smoke:
  - `python3 tests/python/metal_sac_ablation.py --suite=metal-smoke --runs=1 --warmup=0 --probe-limit=64 --sac-mode=nsacq --activation-mode=neighbor --verify-probe-limit=32 --timeout=120 --csv=out/metal_sac_v317_nsacq_smoke.csv`
- TIER2 evidence:
  - `python3 tests/python/metal_sac_ablation.py --tier=2 --runs=3 --warmup=1 --probe-limit=512 --sac-mode=nsacq --activation-mode=neighbor --verify-probe-limit=32 --timeout=300 --csv=out/metal_sac_v317_nsacq_tier2.csv --quiet`

## Rollback

- Do not pass `--sac_mode=nsacq|sacq_adj|sacq_full` to stay on the v3.16
  benchmark-only batch probe path.
- Existing GAC benchmark, Metal GAC auto policy, v3.14 runtime allowlist, and CUDA
  targets do not read the new SAC mode.
- If NSACQ shows status mismatches or unsafe UNKNOWN handling, keep v3.17
  report-only and continue using v3.16 batch probe as the throughput probe.

## Links

- changelog:
  [Metal SAC v3.17 Host NSACQ Changelog](CPIM_METAL_METAL_SAC_V317_HOST_NSACQ_CHANGELOG_2026_05_09.md)
- roadmap:
  [Metal GAC Long Term Optimization](../METAL_GAC_LONG_TERM_OPTIMIZATION.md)
