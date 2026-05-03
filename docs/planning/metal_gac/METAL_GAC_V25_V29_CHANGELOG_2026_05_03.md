---
status: active
updated: 2026-05-03
type: changelog
scope: metal-gac
---

# Metal GAC v2.5-v2.9 Changelog

## 摘要

- v2.x 从“可消融路径”收敛为可解释、可回退、可封版的 GAC 性能后端。
- 默认 stable fallback 继续保持：
  `cold + shared + flags + scalar + pair + cpu reset`。
- `auto` 封版策略保守降级到 `flags + scalar + pair`，原因是 TIER2 数据显示
  激进 auto 的 p50/p95 会超过 baseline 5% 门槛。

## 代码面

- `MetalGacOptions` / `benchmark_metal_gac` 新增：
  - `reset_mode=cpu|blit|auto`
- `MetalGacStats` / CSV 新增：
  - `reset_dispatch_ms`
  - `effective_frontier_mode`
  - `effective_kernel_variant`
  - `effective_bitsup_layout`
  - `worklist_push_count`
  - `worklist_rounds`
  - `worklist_epoch_resets`
- `variant_name` 与 `effective_*` 一起记录真实执行路径。
- scalar flags/compact 实际仍读 pair bitSup；当 requested directional 但执行路径
  未使用 directional 时，effective bitsup 明确记录为 `pair`。
- worklist 路径使用 epoch/stamp 去重，避免每轮清零 next frontier。
- prepared runner 保留初始 mutable snapshot，支持 blit reset。
- `metal_gac_ablation.py` 新增 `--mode-preset=auto` 与 `--reset-mode`。
- `metal_gac_analyze.py` 新增 recommended policy summary。

## 验证

- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py`
- `cmake --build build_metal --target benchmark_metal_gac compare_cpu_metal -j`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=1 --warmup=0 --verify=true --runner_mode=prepared --frontier_mode=worklist --kernel_variant=word_parallel --bitsup_layout=directional --reset_mode=blit --csv=out/metal_gac_v2x_worklist_word_blit_smoke.csv`
- `ctest --test-dir build_metal -R compare_cpu_metal --output-on-failure`
- `ctest --test-dir build_metal -R benchmark_metal_gac --output-on-failure`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=all --runs=2 --warmup=1 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --timeout=60 --csv=out/metal_gac_v2x_smoke_all.csv`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=auto --runs=3 --warmup=1 --runner-mode=prepared --timeout=60 --csv=out/metal_gac_v2x_smoke_auto.csv --quiet`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=all --runs=5 --warmup=2 --runner-mode=prepared --timeout=300 --csv=out/metal_gac_v2x_tier2_all.csv --quiet`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=auto --runs=5 --warmup=2 --runner-mode=prepared --timeout=300 --csv=out/metal_gac_v2x_tier2_auto.csv --quiet`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v2x_tier2_all.csv out/metal_gac_v2x_tier2_auto.csv --top=10`
- `git diff --check`

## 结果摘要

- worklist + word_parallel + directional + blit reset smoke 通过 CPU verify。
- `metal-smoke` all-mode：60/60 OK。
- `metal-smoke` auto：15/15 OK。
- TIER2 all-mode：2280/2298 rows OK；18 个 ERROR 均为
  `unsupported_non_binary_extension`。
- TIER2 auto：380/383 rows OK；3 个 ERROR 均为
  `unsupported_non_binary_extension`。
- TIER2 baseline `shared+flags+scalar+pair`：
  - `solve_ms p50=0.261`
  - `solve_ms p95=5.132`
- TIER2 auto 封版路径 `flags+scalar+pair`：
  - `solve_ms p50=0.259`
  - `solve_ms p95=5.079`
- 结论：auto p50/p95 不慢于 baseline 5%，满足 v2 封版门槛。

## 决策

- `frontier_mode=auto` 在 v2 封版时不启用 worklist。
- `kernel_variant=auto` 在 v2 封版时不启用 word_parallel。
- `simdgroup` 不进入 v2；显式 `simdgroup` 继续 fallback 到 `word_parallel`。
- `worklist`、`word_parallel`、`directional`、`blit reset` 保留为显式消融路径。

## 后续依赖

- 下一步计划见
  [Metal GAC v2 Guard / v3 Entry Plan](METAL_GAC_V2_GUARD_V3_ENTRY_PLAN_2026_05_03.md)。
