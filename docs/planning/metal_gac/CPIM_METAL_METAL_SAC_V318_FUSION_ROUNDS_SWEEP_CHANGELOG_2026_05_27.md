---
status: active
updated: 2026-05-27T06:55:38Z
type: changelog
topic: cpim-metal
slug: metal-sac-v318-fusion-rounds-sweep
stage: s07
---

# Metal SAC v3.18 Fusion Rounds Sweep Changelog

## Summary

- Added a benchmark-only fusion-round sweep path for Metal SAC bounded command
  fusion.
- TIER2 evidence keeps `fusion_rounds=4` as the most balanced explicit setting:
  `8` lowers avg command-buffer/probe slightly more, but raises dispatch work and
  has worse p95 non-kernel tail than `4`.
- Defaults remain unchanged: `probe_fusion=none`, CPU-confirmed DWO guard stays
  mandatory, and search integration is untouched.

## Changes

- `tests/python/metal_sac_ablation.py`:
  - added `--fusion-rounds-sweep=2,4,8`;
  - repeats the same suite for each requested `fusion_rounds` value;
  - preserves requested `probe_fusion` and `fusion_rounds` on missing/error
    rows;
  - prints `[fusion summary]` grouped by `probe_fusion/fusion_rounds`;
  - reports avg/p50/p95 for `dispatch_per_probe`,
    `command_buffer_per_probe`, and `non_kernel_per_probe`.
- Added this standalone small plan and small changelog.
- Updated navigation and summaries in:
  - `docs/planning/METAL_GAC_CHANGELOG.md`;
  - `docs/planning/METAL_GAC_LONG_TERM_OPTIMIZATION.md`;
  - `docs/planning/METAL_MIGRATION_PLAN.md`;
  - `CHANGES_ZH.md`.

## Validation

- `python3 -m py_compile tests/python/metal_sac_ablation.py codex-docops-logic/scripts/dol.py`
- `python3 tests/python/metal_sac_ablation.py --suite=metal-smoke --runs=1 --warmup=0 --probe-limit=64 --sac-mode=nsacq --activation-mode=neighbor --max-sac-batches=30 --outer-queue-budget=256 --verify-probe-limit=32 --probe-fusion=bounded --fusion-rounds-sweep=2,4,8 --dwo-forensics --timeout=120 --csv=out/metal_sac_v318_fusion_sweep_smoke.csv --quiet`
- `python3 tests/python/metal_sac_ablation.py --tier=0 --runs=3 --warmup=0 --probe-limit=256 --sac-mode=nsacq --activation-mode=neighbor --max-sac-batches=30 --outer-queue-budget=256 --verify-probe-limit=32 --probe-fusion=bounded --fusion-rounds-sweep=2,4,8 --dwo-forensics --timeout=120 --csv=out/metal_sac_v318_fusion_sweep_tier0.csv --quiet`
- `python3 tests/python/metal_sac_ablation.py --tier=2 --runs=3 --warmup=0 --probe-limit=0 --sac-mode=nsacq --activation-mode=neighbor --max-sac-batches=10000 --outer-queue-budget=0 --verify-probe-limit=32 --probe-fusion=bounded --fusion-rounds-sweep=2,4,8 --dwo-forensics --timeout=300 --csv=out/metal_sac_v318_fusion_sweep_tier2.csv --quiet`

## Evidence

- metal-smoke sweep:
  - 15 OK rows, 534 probes, 30 confirmed DWO, 0 rejected DWO, 0 UNKNOWN.
- TIER0 sweep:
  - 108 OK rows, 20,817 probes, 2,555 confirmed DWO, 0 rejected DWO,
    0 UNKNOWN.
  - `fusion_rounds=4` had the best avg non-kernel/probe:
    `0.064398ms`, versus `0.075853ms` for `2` and `0.078101ms` for `8`.
- TIER2 sweep:
  - 684 OK rows, 9 historical unsupported rows, 865,743 probes,
    65,108 confirmed DWO, 0 rejected DWO, 0 UNKNOWN.
  - `fusion_rounds=2`:
    command-buffer/probe avg `0.013363` p50 `0.003835` p95 `0.032517`;
    non-kernel/probe avg `0.018152ms` p50 `0.005203ms` p95 `0.026859ms`;
    fused wasted rounds `154`.
  - `fusion_rounds=4`:
    command-buffer/probe avg `0.007618` p50 `0.002136` p95 `0.018015`;
    non-kernel/probe avg `0.016337ms` p50 `0.004706ms` p95 `0.021674ms`;
    fused wasted rounds `438`.
  - `fusion_rounds=8`:
    command-buffer/probe avg `0.006531` p50 `0.001890` p95 `0.011447`;
    non-kernel/probe avg `0.015251ms` p50 `0.004910ms` p95 `0.023821ms`;
    fused wasted rounds `1398`.

## Decisions

- Keep `fusion_rounds=4` as the balanced explicit bounded setting for now.
- Keep `fusion_rounds=8` report-only: it improves command-buffer/probe and avg
  non-kernel/probe, but has worse p95 non-kernel/probe than `4` and much higher
  wasted rounds.
- Proceed next to NSACQ queue policy and `sacq_adj/sacq_full` comparison.

## Follow-Ups

- Add NSACQ queue policy switches for FIFO / degree priority /
  failure-priority / requeue cap.
- Compare `batch_probe`, `nsacq`, `sacq_adj`, and `sacq_full` under
  `probe_fusion=bounded --fusion_rounds=4`.

## Links

- plan:
  [Metal SAC v3.18 Fusion Rounds Sweep Plan](CPIM_METAL_METAL_SAC_V318_FUSION_ROUNDS_SWEEP_PLAN_2026_05_27.md)
- parent plan:
  [Metal SAC v3.18 DWO Forensics Command Fusion NSACQ Throughput Plan](CPIM_METAL_METAL_SAC_V318_DWO_FORENSIC_FUSION_PLAN_2026_05_24.md)
- roadmap:
  [Metal GAC Long Term Optimization](../METAL_GAC_LONG_TERM_OPTIMIZATION.md)
