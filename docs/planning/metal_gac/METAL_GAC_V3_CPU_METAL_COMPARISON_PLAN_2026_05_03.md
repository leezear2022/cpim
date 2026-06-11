---
status: active
updated: 2026-05-03
type: plan
topic: cpim-metal
slug: metal-gac-v3-cpu-metal-comparison
stage: s07
---

# Metal GAC v3 CPU vs Metal Comparison Plan

## Goal

- 正式回答 Metal GAC 与 CPU GAC 哪个更快。
- 对照口径只比较 GAC propagation solve time，不计 parser、normalize、
  DeviceLayout，也不计 Metal prepare/setup。
- CPU timing 只作为 benchmark evidence，不改变 correctness verify、solver 默认路径
  或 auto policy。

## Scope

- `benchmark_metal_gac` 增加可选 CPU timing。
- CSV、ablation 和 analyzer 增加 CPU/Metal ratio 观测。
- 文档记录性能结论与后续优化方向。
- 不迁移 SAC/Batch，不改变 Metal kernel 或 CPU GAC 语义。

## Tasks

- benchmark：
  - 新增 `--cpu_timing=true|false`，默认 `false`；
  - 新增 `--cpu_warmup` 与 `--cpu_runs`，负数时分别沿用 `--warmup` 与 `--runs`；
  - CPU timing 每次新建 `GacCpuRunner`，只计 `Run()` 时间；
  - CSV 写入 `cpu_solve_ms`、CPU stats 与 `metal_cpu_solve_ratio`。
- ablation：
  - 新增 `--cpu-timing`、`--cpu-warmup`、`--cpu-runs`；
  - 透传 benchmark CPU timing 字段到批量 CSV。
- analyzer：
  - overview 与 mode summary 输出 CPU solve 与 Metal/CPU ratio；
  - 新增 `[metal vs cpu]` section；
  - 固定解释：ratio `< 1.0` 表示 Metal 更快。

## Validation

- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py codex-docops-logic/scripts/dol.py`
- `cmake --build build_metal --target benchmark_metal_gac compare_cpu_metal -j`
- `ctest --test-dir build_metal -R compare_cpu_metal --output-on-failure`
- `ctest --test-dir build_metal -R benchmark_metal_gac --output-on-failure`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=3 --warmup=1 --verify=true --cpu_timing=true --runner_mode=prepared --frontier_mode=flags --kernel_variant=scalar --bitsup_layout=pair --csv=out/metal_gac_v3_cpu_metal_smoke.csv`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=auto --runs=5 --warmup=2 --runner-mode=prepared --cpu-timing --timeout=60 --csv=out/metal_gac_v3_cpu_metal_smoke_auto.csv`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=auto --runs=5 --warmup=2 --runner-mode=prepared --cpu-timing --timeout=300 --csv=out/metal_gac_v3_cpu_metal_tier2_auto.csv --quiet`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v3_cpu_metal_smoke_auto.csv out/metal_gac_v3_cpu_metal_tier2_auto.csv --top=10 --recommend-policy --baseline-mode shared+flags --regression-threshold 1.05 --min-runs 3`

## Rollback

- 不传 `--cpu_timing=true` / `--cpu-timing` 即回到旧 benchmark 扫描成本。
- 新 CSV 字段对旧 analyzer 兼容；旧 CSV 缺字段时 ratio section 自动跳过。
- Metal 默认 fallback 不变。

## Links

- changelog:
  [Metal GAC v3 CPU vs Metal Comparison Changelog](METAL_GAC_V3_CPU_METAL_COMPARISON_CHANGELOG_2026_05_03.md)
- roadmap:
  [Metal GAC Long Term Optimization](../METAL_GAC_LONG_TERM_OPTIMIZATION.md)
