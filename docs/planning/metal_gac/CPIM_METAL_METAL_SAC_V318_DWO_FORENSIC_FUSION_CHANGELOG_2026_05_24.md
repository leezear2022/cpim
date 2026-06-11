---
status: active
updated: 2026-05-27T02:49:35Z
type: changelog
topic: cpim-metal
slug: metal-sac-v318-dwo-forensic-fusion
stage: s07
---

# Metal SAC v3.18 DWO Forensics Command Fusion NSACQ Throughput Changelog

## Summary

- Updated the Metal GAC/SAC big plan after review of the v3.17 host-side NSACQ
  evidence.
- The roadmap now explicitly freezes single-instance Metal GAC performance work
  as a correctness/fallback path.
- The new performance main line is Batch/SAC/NSACQ throughput, gated first on raw
  Metal DWO trust.

## Changes

- Added the standalone v3.18 small plan:
  `docs/planning/metal_gac/CPIM_METAL_METAL_SAC_V318_DWO_FORENSIC_FUSION_PLAN_2026_05_24.md`.
- Added this standalone v3.18 small changelog.
- Updated the long-term roadmap with the two-layer bottleneck split:
  - GAC-only: short-kernel command-buffer / wait / non-kernel fixed cost;
  - NSACQ: raw Metal DWO trust, CPU-confirm guard cost, and batch/queue policy.
- Added three priority experiments:
  - `dwo_forensic_oracle`;
  - `command_buffer_fusion`;
  - `nsacq_batch_policy_sacq_compare`.
- Updated the Metal GAC/SAC changelog index, migration plan, and `CHANGES_ZH.md`.
- DocOps roadmap stays at `rm: v03`.

## Implementation Slice 1

- Added `--dwo_forensics=true|false` to `benchmark_metal_sac`.
- Added optional final world-domain readback to `MetalBatchProbeRunner`:
  - `MetalBatchProbeOptions::collect_world_domains`;
  - `world_bit_dom()` and `world_domain_sizes()` accessors.
- Added rejected-DWO forensic CSV fields:
  - raw / confirmed / rejected DWO counts;
  - `nsacq_raw_dwo_precision`;
  - `dwo_forensic_checked`;
  - `dwo_domain_size_popcount_mismatch_count`;
  - rejected empty/nonempty domain counts;
  - first rejected probe var/value, first empty var/popcount/domain-size, and
    snapshot / allowed-mask hashes.
- Updated `tests/python/metal_sac_ablation.py` to carry the new fields and expose
  `--dwo-forensics` / `--no-dwo-forensics`.
- Kept the feature benchmark-only; no solver default, GAC `auto`, CUDA path, or
  search behavior changed.

## Implementation Slice 2

- Added Metal-side DWO status debug words for the first status transition:
  `status_var`, `status_cid`, `status_dir`, `old_size`, `deletion_count`, and
  `round`.
- The first rejected-DWO sample after instrumentation showed
  `status_cid=-1`, which points to the init-kernel missing-value check rather
  than a revise-kernel support failure.
- Fixed `sac_probe_init_kernel` so singleton worlds are built directly from the
  immutable snapshot:
  - world bit domains no longer race between "copy snapshot" threads and the
    `cid==0` singleton writer;
  - world domain sizes are seeded as `1` for the focal variable only when the
    singleton value exists in the snapshot;
  - missing-value DWO checks read `snapshot_bit_dom` instead of the mutable
    world buffer being initialized in the same dispatch.
- After the fix, TIER0 limited NSACQ forensics produced `852/852` confirmed DWO
  and `0` rejected DWO across 36 measured rows.
- Full TIER2 NSACQ forensics produced 228 OK rows, 3 historical
  `unsupported_non_binary_extension` rows, 286,744 probes, 19,885 confirmed DWO,
  0 rejected DWO, and 0 UNKNOWN.

## Implementation Slice 3

- Implemented bounded command-buffer fusion for SAC batch probes:
  - `MetalRuntime::Dispatch1DBatch()` encodes multiple 1D compute dispatches in
    one command buffer;
  - `MetalBatchProbeOptions::probe_fusion` supports `none` and `bounded`;
  - `fusion_rounds` defaults to `4`;
  - `sac_probe_clear_active_counts_kernel` and
    `sac_probe_frontier_fused_kernel` record per-slot active counts for host
    convergence checks after each fused segment.
- Added benchmark/script controls:
  - `benchmark_metal_sac --probe_fusion=none|bounded --fusion_rounds=<n>`;
  - `metal_sac_ablation.py --probe-fusion --fusion-rounds`.
- Added CSV/report fields:
  - `probe_fusion`, `fusion_rounds`;
  - `command_buffer_count`, `command_buffer_per_probe`;
  - `stats_fusion_rounds`, `fused_rounds_encoded`,
    `fused_rounds_wasted`.
- Full TIER2 comparison:
  - `probe_fusion=none`: 228 OK rows, 3 historical unsupported rows, 291,297
    probes, 24,402 confirmed DWO, 0 rejected DWO, 0 UNKNOWN,
    `command_buffer_per_probe avg=0.0528`,
    `non_kernel_per_probe avg=0.0310ms`;
  - `probe_fusion=bounded`: 228 OK rows, 3 historical unsupported rows,
    289,657 probes, 22,777 confirmed DWO, 0 rejected DWO, 0 UNKNOWN,
    `command_buffer_per_probe avg=0.0076`,
    `non_kernel_per_probe avg=0.0198ms`.
- Fusion-round sweep is now tracked in its own microdocs:
  - [Metal SAC v3.18 Fusion Rounds Sweep Plan](CPIM_METAL_METAL_SAC_V318_FUSION_ROUNDS_SWEEP_PLAN_2026_05_27.md)
  - [Metal SAC v3.18 Fusion Rounds Sweep Changelog](CPIM_METAL_METAL_SAC_V318_FUSION_ROUNDS_SWEEP_CHANGELOG_2026_05_27.md)
  - TIER2 sweep keeps `fusion_rounds=4` as the balanced explicit bounded setting.

## Validation

- `python3 -m py_compile codex-docops-logic/scripts/dol.py`
- `python3 -m py_compile tests/python/metal_sac_ablation.py codex-docops-logic/scripts/dol.py`
- `cmake --build build_metal --target benchmark_metal_sac -j8`
- `./build_metal/benchmark_metal_sac --input=tests/data/bench/queens-4_ext.xml --runs=1 --warmup=0 --probe_limit=16 --sac_mode=nsacq --activation_mode=neighbor --max_sac_batches=2 --outer_queue_budget=16 --max_probe_rounds=1000 --verify=true --verify_probe_limit=16 --dwo_forensics=true --csv=out/metal_sac_v318_dwo_forensics_queens4.csv`
- `python3 tests/python/metal_sac_ablation.py --instances tests/data/bench/queens-4_ext.xml --runs=1 --warmup=0 --probe-limit=16 --sac-mode=nsacq --activation-mode=neighbor --max-sac-batches=2 --outer-queue-budget=16 --max-probe-rounds=1000 --verify-probe-limit=16 --dwo-forensics --timeout=60 --csv=out/metal_sac_v318_dwo_forensics_ablation_queens4.csv`
- `python3 tests/python/metal_sac_ablation.py --tier=0 --runs=3 --warmup=0 --probe-limit=256 --sac-mode=nsacq --activation-mode=neighbor --max-sac-batches=30 --outer-queue-budget=256 --max-probe-rounds=10000 --verify-probe-limit=32 --dwo-forensics --timeout=120 --csv=out/metal_sac_v318_forensics_tier0_fixed_r3.csv --quiet`
- `./build_metal/benchmark_metal_sac --input=benchmarks/driver/driverlogw-01c-sat_ext.xml --runs=3 --warmup=0 --probe_limit=256 --sac_mode=nsacq --activation_mode=neighbor --max_sac_batches=30 --outer_queue_budget=256 --max_probe_rounds=10000 --verify=true --verify_probe_limit=32 --dwo_forensics=true --csv=out/metal_sac_v318_forensics_driver_fixed.csv`
- `python3 tests/python/metal_sac_ablation.py --tier=2 --runs=3 --warmup=0 --probe-limit=0 --sac-mode=nsacq --activation-mode=neighbor --max-sac-batches=10000 --outer-queue-budget=0 --max-probe-rounds=10000 --verify-probe-limit=32 --dwo-forensics --timeout=300 --csv=out/metal_sac_v318_forensics_tier2_full_r3.csv --quiet`
- `./build_metal/benchmark_metal_sac --input=tests/data/bench/queens-4_ext.xml --runs=1 --warmup=0 --probe_limit=16 --sac_mode=nsacq --verify=true --verify_probe_limit=16 --probe_fusion=none --csv=out/metal_sac_v318_fusion_none_smoke.csv`
- `./build_metal/benchmark_metal_sac --input=tests/data/bench/queens-4_ext.xml --runs=3 --warmup=0 --probe_limit=64 --sac_mode=nsacq --verify=true --verify_probe_limit=64 --probe_fusion=bounded --fusion_rounds=4 --dwo_forensics=true --csv=out/metal_sac_v318_fusion_bounded_smoke.csv`
- `python3 tests/python/metal_sac_ablation.py --tier=0 --runs=3 --warmup=0 --probe-limit=256 --sac-mode=nsacq --activation-mode=neighbor --max-sac-batches=30 --outer-queue-budget=256 --verify-probe-limit=32 --probe-fusion=bounded --fusion-rounds=4 --dwo-forensics --timeout=120 --csv=out/metal_sac_v318_fusion_tier0_bounded.csv --quiet`
- `python3 tests/python/metal_sac_ablation.py --tier=2 --runs=3 --warmup=0 --probe-limit=0 --sac-mode=nsacq --activation-mode=neighbor --max-sac-batches=10000 --outer-queue-budget=0 --verify-probe-limit=32 --probe-fusion=bounded --fusion-rounds=4 --dwo-forensics --timeout=300 --csv=out/metal_sac_v318_fusion_tier2_bounded.csv --quiet`
- `python3 tests/python/metal_sac_ablation.py --tier=2 --runs=3 --warmup=0 --probe-limit=0 --sac-mode=nsacq --activation-mode=neighbor --max-sac-batches=10000 --outer-queue-budget=0 --verify-probe-limit=32 --probe-fusion=none --fusion-rounds=4 --dwo-forensics --timeout=300 --csv=out/metal_sac_v318_fusion_tier2_none.csv --quiet`
- `git diff --check`
- `python3 codex-docops-logic/scripts/dol.py lint --soft`

## Decisions

- Current route has no major direction drift.
- Single-instance Metal GAC is no longer the performance main line; it remains a
  stable fallback and instrumentation path.
- Do not continue broad CTA/owner/bulk-sync promotion work outside narrow
  allowlists.
- Do not promote raw Metal DWO while v3.17 still has `4,970` rejected
  unconfirmed Metal DWO reports.
- Keep CPU-confirmed DWO guard until v3.18 forensics either proves raw DWO trust
  or classifies the rejected cases.
- The init-kernel race is fixed for TIER0 and full TIER2 evidence. CPU-confirmed
  DWO guard remains the conservative default until the next plan decides whether
  raw DWO can be promoted or only used for report-only throughput experiments.

## Follow-Ups

- First implement `dwo_forensic_oracle`; correctness trust gates every later SAC
  throughput claim.
- Then test `command_buffer_fusion` to see whether one-wait-per-batch can lower
  `non_kernel_per_probe`.
- Then compare `nsacq`, `sacq_adj`, and `sacq_full` with batch-size and queue
  policy sweeps while keeping the CPU-confirmed guard.

## Links

- plan:
  [Metal SAC v3.18 DWO Forensics Command Fusion NSACQ Throughput Plan](CPIM_METAL_METAL_SAC_V318_DWO_FORENSIC_FUSION_PLAN_2026_05_24.md)
- roadmap:
  [Metal GAC Long Term Optimization](../METAL_GAC_LONG_TERM_OPTIMIZATION.md)
