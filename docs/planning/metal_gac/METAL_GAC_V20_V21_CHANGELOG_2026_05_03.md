---
status: active
updated: 2026-05-03
type: changelog
scope: metal-gac
---

# Metal GAC v2.0-v2.1 Changelog

## 摘要

- 冻结 Metal GAC v2 benchmark/CSV 口径。
- 引入 prepared runner，拆分 prepare/reset/dispatch 成本。
- 性能主指标从 wall-clock `elapsed_ms` 调整为 `solve_ms`。

## 代码面

- 新增 `MetalPreparedGacRunner`。
- `MetalGacOptions` 新增：
  - `runner_mode=cold|prepared`
  - `kernel_variant=scalar|word_parallel|simdgroup|auto`
  - `frontier_mode=worklist|auto` 预留
- `MetalGacStats` / `benchmark_metal_gac` CSV 新增：
  - `runner_mode`
  - `kernel_variant`
  - `variant_name`
  - `solve_ms`
  - `prepare_ms`
  - `reset_ms`
  - `active_constraints_total`
  - `frontier_density_avg`
- `metal_gac_ablation.py` 透传 `--runner-mode` 与 `--kernel-variant`。
- `metal_gac_analyze.py` 主分析指标切换为 `solve_ms`，旧 CSV 缺字段时回退
  `elapsed_ms`。

## 指标口径

- `solve_ms = reset_ms + dispatch_ms`。
- `reset_ms` 表示单次求解前恢复 mutable state。
- `dispatch_ms` 表示 Metal command buffer 提交并等待完成的成本。
- `setup_ms/prepare_ms` 只记录初始化成本，不进入主性能指标。

## 验证

- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py`
- `cmake --build build_metal --target benchmark_metal_gac compare_cpu_metal -j`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=2 --warmup=1 --verify=true --runner_mode=prepared --csv=out/metal_gac_v21_prepared_smoke.csv`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=1 --warmup=0 --verify=true --runner_mode=prepared --frontier_mode=worklist --kernel_variant=simdgroup --csv=out/metal_gac_v21_fallback_smoke.csv`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=baseline --runs=1 --warmup=0 --runner-mode=prepared --kernel-variant=scalar --timeout=60 --csv=out/metal_gac_v21_ablation_prepared_smoke.csv --quiet`
- `ctest --test-dir build_metal -R compare_cpu_metal --output-on-failure`
- `ctest --test-dir build_metal -R benchmark_metal_gac --output-on-failure`

## 结果摘要

- prepared smoke 通过 CPU verify。
- fallback smoke 通过 CPU verify。
- `metal-smoke` baseline prepared 扫描 5/5 OK。
- Metal compare CTest 5/5 通过，benchmark smoke 1/1 通过。

## 决策

- v2 以 `solve_ms` 的 p50/p95/p99 作为主性能口径。
- `setup_ms/prepare_ms` 只用于观察初始化成本。
- `worklist`、`word_parallel`、`simdgroup`、`auto` 先作为可观测开关接入。
