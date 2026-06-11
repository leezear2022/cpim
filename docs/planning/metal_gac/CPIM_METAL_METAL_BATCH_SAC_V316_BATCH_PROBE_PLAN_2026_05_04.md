---
status: active
updated: 2026-05-04T11:04:16Z
type: plan
topic: cpim-metal
slug: metal-batch-sac-v316-batch-probe
stage: s07
---

# Metal Batch/SAC v3.16 Batch Probe Plan

## Goal

- 实现 Metal-first、default-off 的 Batch/SAC singleton probe benchmark。
- 用多 world probe 批量验证 v3.15 结论：SAC 类工作负载能否摊薄短 kernel 周围的
  command buffer non-kernel 固定成本。
- 本轮只做 benchmark MVP，不接完整 SAC preprocess、不接搜索路径、不改变 GAC
  `auto` 或 v3.14 runtime allowlist。

## Scope

- 包含：
  - Metal batch probe runner；
  - 多 world probe init/revise/frontier kernels；
  - `benchmark_metal_sac` app；
  - `tests/python/metal_sac_ablation.py`；
  - CPU reference probe status verify。
- 不包含：
  - 完整 NSACQ/SACQ convergence；
  - DWO probe 回写主 domain；
  - 搜索集成；
  - BMMA/Tensor-core-like 后端。

## Tasks

- 新增语义：
  - `MetalSacProbeStatus { ok, dwo, unknown }`；
  - `MetalSacActivationMode { neighbor, full }`；
  - `MetalSacProbeTask { var_id, value, task_id }`；
  - `MetalSacBudget { max_probe_rounds }`；
  - `MetalBatchProbeStats` 记录 probes、OK/DWO/UNKNOWN、rounds、dispatch、
    encode/wait/non-kernel、probes/sec、dispatch_per_probe、
    non_kernel_per_probe。
- Runner 行为：
  - 先用 stable Metal GAC 生成 AC snapshot；
  - 从 snapshot 中仍存在的 `(var,value)` 生成 probe tasks；
  - 每个 probe 是一个 world，domain/frontier 展开为 `[world][...]`；
  - `neighbor` 初始 frontier 只激活 singleton var 的 subscription constraints；
  - `full` 初始 frontier 激活全部 constraints；
  - 超过 `max_probe_rounds` 的仍 active world 标为 UNKNOWN，绝不当作 DWO。
- Benchmark 行为：
  - `--probe_limit=0` 表示使用全部 snapshot values；
  - `--verify=true` 时用 CPU reference 比较 probe status；
  - CSV 与 Metal GAC CSV 分离。

## Validation

- 静态与构建：
  - `python3 -m py_compile tests/python/metal_sac_ablation.py codex-docops-logic/scripts/dol.py`
  - `cmake --build build_metal --target benchmark_metal_sac -j8`
- 单例：
  - `./build_metal/benchmark_metal_sac --input=tests/data/metal/gac_bitwords2.xml --runs=1 --warmup=0 --probe_limit=64 --verify=true --activation_mode=neighbor --csv=out/metal_sac_v316_batch_probe_single.csv`
- BH smoke：
  - `./build_metal/benchmark_metal_sac --input=benchmarks/BH-4-4/BlackHole-4-4-e-0_ext.xml --runs=3 --warmup=1 --probe_limit=512 --verify=true --activation_mode=neighbor --csv=out/metal_sac_v316_batch_probe_bh.csv`
- Suite smoke：
  - `python3 tests/python/metal_sac_ablation.py --suite=metal-smoke --runs=3 --warmup=1 --probe-limit=128 --verify-probe-limit=64 --timeout=120 --csv=out/metal_sac_v316_batch_probe_smoke.csv`
- TIER2：
  - `python3 tests/python/metal_sac_ablation.py --tier=2 --runs=5 --warmup=2 --probe-limit=512 --verify-probe-limit=32 --timeout=300 --csv=out/metal_sac_v316_batch_probe_tier2.csv --quiet`

## Rollback

- 新 target 是 default-off；不运行 `benchmark_metal_sac` 即无行为变化。
- `benchmark_metal_gac`、`frontier_mode=auto`、v3.14 `bh_cta_allowlist` 和 CUDA
  targets 不读取新路径。
- 若 probe status mismatch 或 UNKNOWN 语义异常，保留 benchmark evidence，
  不进入 v3.17 SAC preprocess。

## Links

- changelog:
  [Metal Batch/SAC v3.16 Batch Probe Changelog](CPIM_METAL_METAL_BATCH_SAC_V316_BATCH_PROBE_CHANGELOG_2026_05_04.md)
- roadmap:
  [Metal GAC Long Term Optimization](../METAL_GAC_LONG_TERM_OPTIMIZATION.md)
