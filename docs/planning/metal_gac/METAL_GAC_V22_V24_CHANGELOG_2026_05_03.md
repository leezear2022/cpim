---
status: active
updated: 2026-05-03
type: changelog
scope: metal-gac
---

# Metal GAC v2.2-v2.4 Changelog

## 摘要

- 将 `worklist`、`word_parallel`、`directional bitSup` 从占位推进为真实可消融路径。
- 默认路径不变，所有新路径必须通过 CPU verify。
- `simdgroup` 保留为显式 fallback，不在本段实现复杂 lane reduction。

## 代码面

- `DeviceModelLayout` 新增 directional `bit_sup_words`：
  `bit_sup_words[cid][dir][value][word]`。
- `MetalGacSolver` 新增：
  - worklist active constraint 双缓冲；
  - directional bitSup buffer；
  - worklist revise pipeline；
  - word-parallel flags / active-list / worklist pipelines。
- `.metal` 新增 kernels：
  - `gac_revise_worklist_kernel`
  - `gac_revise_word_flags_kernel`
  - `gac_revise_word_active_kernel`
  - `gac_revise_word_worklist_kernel`
- `benchmark_metal_gac` 新增：
  - `--bitsup_layout=pair|directional|auto`
- `metal_gac_ablation.py` / `metal_gac_analyze.py` 新增 `bitsup_layout` 维度。
- `metal_gac_ablation.py --mode-preset=all` 扩展为：
  `shared/private x flags/compact/worklist`。

## 验证

- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py`
- `cmake --build build_metal --target benchmark_metal_gac compare_cpu_metal -j`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=3 --warmup=1 --verify=true --runner_mode=prepared --frontier_mode=worklist --kernel_variant=word_parallel --bitsup_layout=directional --csv=out/metal_gac_v24_worklist_word_directional_smoke.csv`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=1 --warmup=0 --verify=true --runner_mode=prepared --frontier_mode=auto --kernel_variant=simdgroup --bitsup_layout=auto --csv=out/metal_gac_v24_auto_simdgroup_smoke.csv`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=all --runs=2 --warmup=1 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --timeout=60 --csv=out/metal_gac_v24_smoke.csv`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=all --runs=5 --warmup=2 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --timeout=300 --csv=out/metal_gac_v24_tier2.csv --quiet`
- `ctest --test-dir build_metal -R compare_cpu_metal --output-on-failure`
- `ctest --test-dir build_metal -R benchmark_metal_gac --output-on-failure`

## 结果摘要

- worklist + word_parallel + directional smoke 通过 CPU verify。
- `auto+simdgroup+auto` 明确记录为
  `auto+simdgroup+auto->flags+word_parallel+directional`。
- `metal-smoke` all-mode：60/60 OK。
- TIER2 all-mode：2280/2298 rows OK，18 个 ERROR 均为
  `unsupported_non_binary_extension`。
- TIER2 `shared+flags word_parallel+directional`：
  `solve_ms avg=0.819 p50=0.340 p95=5.362 p99=5.891`。
- TIER2 `shared+worklist word_parallel+directional`：
  `solve_ms avg=0.826 p50=0.356 p95=5.274 p99=5.917`。

## 决策

- worklist 和 word_parallel 均保留为显式消融路径。
- pair bitSup、scalar flags/compact 旧路径保留。
- v2 不引入 Metal texture/read-only cache。
- v2 不实现真正 simdgroup reduction。
- CUDA/Jetson 路径不受影响。
