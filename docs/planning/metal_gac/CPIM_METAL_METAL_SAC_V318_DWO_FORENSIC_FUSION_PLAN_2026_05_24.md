---
status: active
updated: 2026-05-27T02:49:35Z
type: plan
topic: cpim-metal
slug: metal-sac-v318-dwo-forensic-fusion
stage: s07
---

# Metal SAC v3.18 DWO Forensics Command Fusion NSACQ Throughput Plan

## Goal

- Update the Metal GAC/SAC roadmap after the v3.17 host-side NSACQ evidence.
- Freeze single-instance Metal GAC performance work as a stable correctness /
  fallback path, while moving the performance main line to Batch/SAC/NSACQ
  throughput.
- Start v3.18 with benchmark-only DWO forensics: no runtime default changes, no
  search integration, and no removal of the CPU-confirmed DWO guard.

## Scope

- Include:
  - a v3.18 decision memo in the long-term roadmap;
  - three next experiments: `dwo_forensic_oracle`,
    `command_buffer_fusion`, and `nsacq_batch_policy_sacq_compare`;
  - validation and promotion gates for DWO trust, command-buffer fixed cost, and
    NSACQ queue/batch policy.
- Exclude:
  - enabling any GAC aggressive path in `frontier_mode=auto`;
  - promoting raw Metal DWO without confirmation;
  - changing CUDA/Jetson behavior;
  - wiring Metal NSACQ into search.

## Tasks

### 1. `dwo_forensic_oracle`

- Purpose: explain the v3.17 TIER2 split between `12,094` confirmed DWO
  writebacks and `4,970` rejected unconfirmed Metal DWO reports.
- Add report/debug fields in the later implementation:
  - `raw_dwo_precision = confirmed / (confirmed + rejected)`;
  - domain-size versus bitset popcount mismatch count;
  - allowed-constraint mask hash;
  - snapshot hash;
  - first DWO var/cid/round;
  - stale frontier or mask mismatch counters.
- Implemented first slice:
  - `benchmark_metal_sac --dwo_forensics=true|false`;
  - final per-world bit-domain/domain-size readback in `MetalBatchProbeRunner`
    only when requested;
  - CSV fields for raw/confirmed/rejected DWO, raw precision, rejected-DWO
    empty/nonempty classification, domain-size/popcount mismatch count, and
    first rejected probe metadata;
  - `metal_sac_ablation.py --dwo-forensics|--no-dwo-forensics`.
- Implemented second slice:
  - DWO debug words record the first Metal-side status transition
    (`var/cid/dir/old_size/deletion_count/round`);
  - initial false DWO was traced to `sac_probe_init_kernel` reading and
    overwriting `world_bit_dom` in the same dispatch that copied the snapshot;
  - the init kernel now builds singleton domains directly from the immutable
    snapshot and reads snapshot bits for missing-value DWO checks.
- Full TIER2 evidence after the fix:
  - 228 OK rows, 3 historical `unsupported_non_binary_extension` rows;
  - 286,744 probes;
  - 19,885 raw DWO, 19,885 confirmed DWO, 0 rejected DWO;
  - 0 UNKNOWN.
- Build a deterministic oracle path for rejected-DWO replay:
  - single probe / single world;
  - full frontier reset;
  - bulk-synchronous deletion mask;
  - DWO only after popcount-confirmed empty domain.
- Promote gate:
  - raw DWO precision approaches 100%; or
  - rejected DWO rows are clearly classified into mask/frontier/domain-size/
    snapshot differences.

### 2. `command_buffer_fusion`

- Purpose: validate whether v3.15 `non_kernel_share=0.86` can be reduced by
  one-wait-per-batch execution.
- Compare:
  - current baseline;
  - one command buffer with multiple compute encoders;
  - one command buffer with one compute encoder and multiple dispatches;
  - optional bounded multi-round pre-encoded probe.
- Do not prioritize ICB/MPS/argument buffers in v3.18:
  - ICB is reserved as a later encode-overhead microbench;
  - MPS is not a natural fit for bitset frontier / DWO / allowed-mask logic;
  - argument buffers are lower priority because v3.15 encode cost is small.
- Required metrics:
  - `non_kernel_per_probe p50/p95/p99`;
  - empty-dispatch lower bound;
  - dispatches per probe;
  - GPU timing availability;
  - DWO precision must not regress.
- Implemented bounded multi-round first slice:
  - `MetalBatchProbeOptions::probe_fusion = none|bounded`;
  - `fusion_rounds`, default `4`;
  - `benchmark_metal_sac --probe_fusion=none|bounded --fusion_rounds=<n>`;
  - `metal_sac_ablation.py --probe-fusion --fusion-rounds`;
  - one command buffer encodes `init + K * (revise + clear_active + frontier)`
    for the first segment, then `K * (...)` for later segments.
- Full TIER2 evidence:
  - `probe_fusion=none`: 228 OK rows, 3 historical unsupported rows, 291,297
    probes, 24,402 confirmed DWO, 0 rejected, 0 UNKNOWN,
    `command_buffer_per_probe avg=0.0528`, `non_kernel_per_probe avg=0.0310ms`;
  - `probe_fusion=bounded`: 228 OK rows, 3 historical unsupported rows,
    289,657 probes, 22,777 confirmed DWO, 0 rejected, 0 UNKNOWN,
    `command_buffer_per_probe avg=0.0076`, `non_kernel_per_probe avg=0.0198ms`.
- Fusion-round sweep follow-up:
  - standalone plan/changelog:
    [Metal SAC v3.18 Fusion Rounds Sweep Plan](CPIM_METAL_METAL_SAC_V318_FUSION_ROUNDS_SWEEP_PLAN_2026_05_27.md);
  - TIER2 `fusion_rounds=2/4/8` all kept 0 rejected DWO and 0 UNKNOWN;
  - `fusion_rounds=4` had the best p95 non-kernel/probe (`0.021674ms`),
    while `8` reduced command-buffer/probe but raised wasted rounds and p95 tail;
  - keep `4` as the balanced explicit bounded setting.

### 3. `nsacq_batch_policy_sacq_compare`

- Purpose: keep the CPU-confirmed DWO guard while evaluating whether NSACQ queue
  and batch policies can preserve throughput and deletion value.
- Compare modes:
  - `batch_probe`;
  - `nsacq`;
  - `sacq_adj`;
  - `sacq_full`.
- Batch-size sweep:
  - `64`, `128`, `256`, `512`, `1024`.
- Queue-policy sweep:
  - FIFO;
  - degree priority;
  - failure-priority;
  - requeue cap.
- Required metrics:
  - confirmed deletions per probe;
  - confirmed deletions per millisecond;
  - `guard_ms / total_ms`;
  - final queue size;
  - remaining values;
  - UNKNOWN count;
  - rejected DWO rate.

## Validation

- Docs/static:
  - `python3 -m py_compile codex-docops-logic/scripts/dol.py`
  - `git diff --check`
  - `python3 codex-docops-logic/scripts/dol.py lint --soft`
- First implementation slice:
  - `python3 -m py_compile tests/python/metal_sac_ablation.py codex-docops-logic/scripts/dol.py`
  - `cmake --build build_metal --target benchmark_metal_sac -j8`
  - `./build_metal/benchmark_metal_sac --input=tests/data/bench/queens-4_ext.xml --runs=1 --warmup=0 --probe_limit=16 --sac_mode=nsacq --activation_mode=neighbor --max_sac_batches=2 --outer_queue_budget=16 --max_probe_rounds=1000 --verify=true --verify_probe_limit=16 --dwo_forensics=true --csv=out/metal_sac_v318_dwo_forensics_queens4.csv`
  - `python3 tests/python/metal_sac_ablation.py --instances tests/data/bench/queens-4_ext.xml --runs=1 --warmup=0 --probe-limit=16 --sac-mode=nsacq --activation-mode=neighbor --max-sac-batches=2 --outer-queue-budget=16 --max-probe-rounds=1000 --verify-probe-limit=16 --dwo-forensics --timeout=60 --csv=out/metal_sac_v318_dwo_forensics_ablation_queens4.csv`
  - `python3 tests/python/metal_sac_ablation.py --tier=0 --runs=3 --warmup=0 --probe-limit=256 --sac-mode=nsacq --activation-mode=neighbor --max-sac-batches=30 --outer-queue-budget=256 --max-probe-rounds=10000 --verify-probe-limit=32 --dwo-forensics --timeout=120 --csv=out/metal_sac_v318_forensics_tier0_fixed_r3.csv --quiet`
  - `./build_metal/benchmark_metal_sac --input=benchmarks/driver/driverlogw-01c-sat_ext.xml --runs=3 --warmup=0 --probe_limit=256 --sac_mode=nsacq --activation_mode=neighbor --max_sac_batches=30 --outer_queue_budget=256 --max_probe_rounds=10000 --verify=true --verify_probe_limit=32 --dwo_forensics=true --csv=out/metal_sac_v318_forensics_driver_fixed.csv`
  - `python3 tests/python/metal_sac_ablation.py --tier=2 --runs=3 --warmup=0 --probe-limit=0 --sac-mode=nsacq --activation-mode=neighbor --max-sac-batches=10000 --outer-queue-budget=0 --max-probe-rounds=10000 --verify-probe-limit=32 --dwo-forensics --timeout=300 --csv=out/metal_sac_v318_forensics_tier2_full_r3.csv --quiet`
- Command fusion slice:
  - `./build_metal/benchmark_metal_sac --input=tests/data/bench/queens-4_ext.xml --runs=1 --warmup=0 --probe_limit=16 --sac_mode=nsacq --verify=true --verify_probe_limit=16 --probe_fusion=none --csv=out/metal_sac_v318_fusion_none_smoke.csv`
  - `./build_metal/benchmark_metal_sac --input=tests/data/bench/queens-4_ext.xml --runs=3 --warmup=0 --probe_limit=64 --sac_mode=nsacq --verify=true --verify_probe_limit=64 --probe_fusion=bounded --fusion_rounds=4 --dwo_forensics=true --csv=out/metal_sac_v318_fusion_bounded_smoke.csv`
  - `python3 tests/python/metal_sac_ablation.py --tier=0 --runs=3 --warmup=0 --probe-limit=256 --sac-mode=nsacq --activation-mode=neighbor --max-sac-batches=30 --outer-queue-budget=256 --verify-probe-limit=32 --probe-fusion=bounded --fusion-rounds=4 --dwo-forensics --timeout=120 --csv=out/metal_sac_v318_fusion_tier0_bounded.csv --quiet`
  - `python3 tests/python/metal_sac_ablation.py --tier=2 --runs=3 --warmup=0 --probe-limit=0 --sac-mode=nsacq --activation-mode=neighbor --max-sac-batches=10000 --outer-queue-budget=0 --verify-probe-limit=32 --probe-fusion=bounded --fusion-rounds=4 --dwo-forensics --timeout=300 --csv=out/metal_sac_v318_fusion_tier2_bounded.csv --quiet`
  - `python3 tests/python/metal_sac_ablation.py --tier=2 --runs=3 --warmup=0 --probe-limit=0 --sac-mode=nsacq --activation-mode=neighbor --max-sac-batches=10000 --outer-queue-budget=0 --verify-probe-limit=32 --probe-fusion=none --fusion-rounds=4 --dwo-forensics --timeout=300 --csv=out/metal_sac_v318_fusion_tier2_none.csv --quiet`
- Future implementation guards to keep in this plan:
  - `cmake --build build_metal --target benchmark_metal_sac -j8`
  - `python3 tests/python/metal_sac_ablation.py --suite=metal-smoke --sac-mode=nsacq --runs=1 --warmup=0 --probe-limit=64 --verify-probe-limit=32 --timeout=120 --csv=out/metal_sac_v318_smoke_nsacq.csv`
  - `python3 tests/python/metal_sac_ablation.py --tier=2 --sac-mode=nsacq --runs=3 --warmup=1 --probe-limit=512 --verify-probe-limit=32 --timeout=300 --csv=out/metal_sac_v318_tier2_nsacq.csv --quiet`
  - targeted rejected-DWO replay CSVs under `out/metal_sac_v318_*`.

## Rollback

- The first implementation slice is benchmark-only. Disable
  `--dwo_forensics` / `--no-dwo-forensics` to avoid rejected-DWO world-domain
  readback overhead.
- Do not pass future v3.18 flags to remain on v3.17 `sac_mode=nsacq` or v3.16
  `sac_mode=batch_probe`.
- CPU-confirmed DWO guard remains mandatory until the forensic path proves raw
  Metal DWO is trustworthy.

## Links

- changelog:
  [Metal SAC v3.18 DWO Forensics Command Fusion NSACQ Throughput Changelog](CPIM_METAL_METAL_SAC_V318_DWO_FORENSIC_FUSION_CHANGELOG_2026_05_24.md)
- roadmap:
  [Metal GAC Long Term Optimization](../METAL_GAC_LONG_TERM_OPTIMIZATION.md)
