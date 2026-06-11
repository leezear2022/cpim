---
status: active
updated: 2026-05-03
type: changelog
topic: cpim-metal
slug: metal-gac-v3-cpu-metal-comparison
stage: s07
---

# Metal GAC v3 CPU vs Metal Comparison Changelog

## Summary

- 新增 CPU vs Metal GAC solve time 对照能力。
- CPU timing 默认关闭，只在 benchmark/ablation 显式开启时生效。
- 当前 evidence 显示：在 TIER2 auto 口径下，CPU GAC 明显快于 Metal GAC。

## Changes

- `benchmark_metal_gac`：
  - 新增 `--cpu_timing`、`--cpu_warmup`、`--cpu_runs`；
  - CSV 新增 `cpu_timing_enabled`、`cpu_solve_ms`、`cpu_iterations`、
    `cpu_deletions`、`cpu_inconsistent`、`metal_cpu_solve_ratio`、
    `metal_faster_than_cpu`；
  - summary 输出 CPU solve p50/p95/p99 与 Metal/CPU ratio。
- `metal_gac_ablation.py`：
  - 新增 `--cpu-timing`、`--cpu-warmup`、`--cpu-runs`；
  - 批量 CSV 透传 CPU timing 字段。
- `metal_gac_analyze.py`：
  - overview 和 mode summary 输出 CPU solve 与 Metal/CPU ratio；
  - 新增 `[metal vs cpu]` section；
  - ratio `< 1.0` 表示 Metal 更快，ratio `> 1.0` 表示 CPU 更快。

## Validation

- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py codex-docops-logic/scripts/dol.py`
- `cmake --build build_metal --target benchmark_metal_gac compare_cpu_metal -j`
- `ctest --test-dir build_metal -R compare_cpu_metal --output-on-failure`
- `ctest --test-dir build_metal -R benchmark_metal_gac --output-on-failure`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=3 --warmup=1 --verify=true --cpu_timing=true --runner_mode=prepared --frontier_mode=flags --kernel_variant=scalar --bitsup_layout=pair --csv=out/metal_gac_v3_cpu_metal_smoke.csv`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=auto --runs=5 --warmup=2 --runner-mode=prepared --cpu-timing --timeout=60 --csv=out/metal_gac_v3_cpu_metal_smoke_auto.csv`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=auto --runs=5 --warmup=2 --runner-mode=prepared --cpu-timing --timeout=300 --csv=out/metal_gac_v3_cpu_metal_tier2_auto.csv --quiet`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v3_cpu_metal_smoke_auto.csv out/metal_gac_v3_cpu_metal_tier2_auto.csv --top=10 --recommend-policy --baseline-mode shared+flags --regression-threshold 1.05 --min-runs 3`

## Results

- metal-smoke auto：25/25 OK，Metal faster `0/25`，ratio p50 `2180.97x`。
- TIER2 auto：380/383 rows OK，3 个 ERROR 均为
  `unsupported_non_binary_extension`。
- TIER2 auto CPU vs Metal：
  - Metal solve `p50=0.439ms p95=5.050ms`；
  - CPU solve `p50=0.008438ms p95=0.071971ms`；
  - Metal/CPU ratio `p50=33.65x p95=427.90x`；
  - Metal faster `0/380`。
- combined smoke + TIER2：Metal faster `0/405`，ratio p50 `35.47x`。

## Decisions

- 当前 GAC-only Metal 不比 CPU 快；在 TIER2 auto 口径下 CPU 明显更快。
- 当前瓶颈仍主要是 Metal host dispatch 往返，不应进入 simdgroup kernel 实现。
- 后续 Metal 优化应优先减少 dispatch/round 次数，或转向更适合 GPU 的
  SAC/Batch 批量工作负载。

## Links

- plan:
  [Metal GAC v3 CPU vs Metal Comparison Plan](METAL_GAC_V3_CPU_METAL_COMPARISON_PLAN_2026_05_03.md)
- roadmap:
  [Metal GAC Long Term Optimization](../METAL_GAC_LONG_TERM_OPTIMIZATION.md)
