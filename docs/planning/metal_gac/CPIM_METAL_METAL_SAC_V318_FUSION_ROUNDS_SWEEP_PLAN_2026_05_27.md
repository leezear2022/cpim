---
status: active
updated: 2026-05-27T06:55:38Z
type: plan
topic: cpim-metal
slug: metal-sac-v318-fusion-rounds-sweep
stage: s07
---

# Metal SAC v3.18 Fusion Rounds Sweep Plan

## Goal

- Finish the bounded command-buffer fusion first pass by scanning
  `fusion_rounds=2/4/8`.
- Keep this as benchmark-only evidence: no default change, no search
  integration, and no removal of the CPU-confirmed DWO guard.
- Decide whether `fusion_rounds=4` remains the balanced explicit default before
  moving to NSACQ queue policy and `sacq_adj/sacq_full` comparison.

## Scope

- Include:
  - `metal_sac_ablation.py --fusion-rounds-sweep=2,4,8`;
  - grouped summary by `probe_fusion/fusion_rounds`;
  - avg/p50/p95 for `dispatch_per_probe`, `command_buffer_per_probe`, and
    `non_kernel_per_probe`;
  - smoke, TIER0, and TIER2 evidence.
- Exclude:
  - changing `probe_fusion=none` default;
  - changing `frontier_mode=auto` or the v3.14 GAC allowlist;
  - changing raw DWO promotion policy;
  - adding ICB, MPS, or argument-buffer paths.

## Tasks

- Extend `metal_sac_ablation.py`:
  - parse `--fusion-rounds-sweep` as comma-separated positive integers;
  - run each requested fusion round value over the same input set;
  - preserve requested `probe_fusion` and `fusion_rounds` on missing/error rows;
  - print grouped fusion summary with avg/p50/p95 metrics.
- Run evidence:
  - metal-smoke sweep for script correctness;
  - TIER0 sweep for quick trend;
  - TIER2 sweep for acceptance-level direction.
- Record results in the v3.18 plan/changelog, roadmap, migration plan, and
  `CHANGES_ZH.md`.

## Validation

- Static:
  - `python3 -m py_compile tests/python/metal_sac_ablation.py codex-docops-logic/scripts/dol.py`
  - `git diff --check`
  - `python3 codex-docops-logic/scripts/dol.py lint --soft`
- Smoke:
  - `python3 tests/python/metal_sac_ablation.py --suite=metal-smoke --runs=1 --warmup=0 --probe-limit=64 --sac-mode=nsacq --activation-mode=neighbor --max-sac-batches=30 --outer-queue-budget=256 --verify-probe-limit=32 --probe-fusion=bounded --fusion-rounds-sweep=2,4,8 --dwo-forensics --timeout=120 --csv=out/metal_sac_v318_fusion_sweep_smoke.csv --quiet`
- TIER0:
  - `python3 tests/python/metal_sac_ablation.py --tier=0 --runs=3 --warmup=0 --probe-limit=256 --sac-mode=nsacq --activation-mode=neighbor --max-sac-batches=30 --outer-queue-budget=256 --verify-probe-limit=32 --probe-fusion=bounded --fusion-rounds-sweep=2,4,8 --dwo-forensics --timeout=120 --csv=out/metal_sac_v318_fusion_sweep_tier0.csv --quiet`
- TIER2:
  - `python3 tests/python/metal_sac_ablation.py --tier=2 --runs=3 --warmup=0 --probe-limit=0 --sac-mode=nsacq --activation-mode=neighbor --max-sac-batches=10000 --outer-queue-budget=0 --verify-probe-limit=32 --probe-fusion=bounded --fusion-rounds-sweep=2,4,8 --dwo-forensics --timeout=300 --csv=out/metal_sac_v318_fusion_sweep_tier2.csv --quiet`

## Decision Rule

- Keep `fusion_rounds=4` as the first explicit bounded setting if it has the
  best or near-best p95 `non_kernel_per_probe` without excessive wasted rounds.
- Treat `fusion_rounds=8` as throughput-biased only if its p95 does not regress
  and wasted rounds stay explainable.
- Reject any setting that produces CPU/Metal verification mismatch, rejected
  DWO, UNKNOWN, or materially worse p95 tail.

## Rollback

- Do not pass `--fusion-rounds-sweep`; the script returns to a single
  `--fusion-rounds` value.
- Use `--probe-fusion=none` to keep the v3.17/v3.18 stable benchmark path.

## Links

- changelog:
  [Metal SAC v3.18 Fusion Rounds Sweep Changelog](CPIM_METAL_METAL_SAC_V318_FUSION_ROUNDS_SWEEP_CHANGELOG_2026_05_27.md)
- parent plan:
  [Metal SAC v3.18 DWO Forensics Command Fusion NSACQ Throughput Plan](CPIM_METAL_METAL_SAC_V318_DWO_FORENSIC_FUSION_PLAN_2026_05_24.md)
- roadmap:
  [Metal GAC Long Term Optimization](../METAL_GAC_LONG_TERM_OPTIMIZATION.md)
