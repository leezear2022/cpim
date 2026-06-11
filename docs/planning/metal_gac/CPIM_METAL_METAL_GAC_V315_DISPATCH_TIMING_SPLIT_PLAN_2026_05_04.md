---
status: active
updated: 2026-05-04T10:42:48Z
type: plan
topic: cpim-metal
slug: metal-gac-v315-dispatch-timing-split
stage: s07
---

# Metal GAC v3.15 Dispatch Timing Split Plan

## Goal

- 将 Metal GAC 的 command buffer 成本拆成 encode / wait / kernel / non-kernel
  四类，验证 v3.10-v3.14 反复出现的 dispatch 瓶颈到底来自 CPU encode，还是来自
  command buffer 提交等待与 GPU fixed cost。
- 不改变任何 solver policy、`solve_ms` 口径或 runtime fallback。
- 为下一步 deciding between "继续 GAC kernel 分支" 和 "转向 Batch/SAC 吞吐" 提供
  更硬的证据。

## Scope

- 包含：
  - `MetalDispatchTimings` 新增 `encode_ms`；
  - benchmark CSV 新增 `dispatch_encode_ms`、`dispatch_wait_ms`、
    `dispatch_non_kernel_ms`；
  - analyzer mode summary 和 `[dispatch timing split]` 输出 per-dispatch 拆分；
  - TIER2 baseline 重新采样。
- 不包含：
  - 修改 `dispatch_ms` 或 `solve_ms` 历史语义；
  - 改写 `frontier_mode=auto`；
  - 引入异步 multi-command-buffer pipeline、persistent kernel 或 batch execution。

## Tasks

- runtime：
  - 在 `MetalRuntime::Dispatch1D()` 和 blit copy/fill 路径记录 command encoding
    wall time；
  - 保留 `wall_ms` 为 command buffer 提交到完成的时间；
  - 用 GPU counter 可用时的 `kernel_ms` 计算
    `dispatch_non_kernel_ms = max(0, dispatch_ms - kernel_ms)`。
- solver stats：
  - `dispatch_encode_ms` 累加 encoding wall time；
  - `dispatch_wait_ms` 累加 command buffer wait wall time；
  - `dispatch_non_kernel_ms` 累加 wait minus GPU kernel elapsed。
- benchmark / CSV：
  - 继续输出旧 `dispatch_ms`、`kernel_ms`；
  - 追加新字段，旧 CSV analyzer 缺字段时按 0 处理。
- analyzer：
  - mode summary 显示 `encode`、`wait`、`non_kernel`、
    `encode_share`、`non_kernel_share`；
  - 新增 `[dispatch timing split]`，输出 per-dispatch encode/wait/kernel/non-kernel。

## Validation

- 静态与构建：
  - `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py codex-docops-logic/scripts/dol.py`
  - `cmake --build build_metal --target benchmark_metal_gac -j8`
- 单例：
  - `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=3 --warmup=1 --verify=true --runner_mode=prepared --frontier_mode=flags --kernel_variant=scalar --bitsup_layout=pair --csv=out/metal_gac_v315_dispatch_timing_split_single.csv`
  - `python3 tests/python/metal_gac_analyze.py out/metal_gac_v315_dispatch_timing_split_single.csv --top=5 --baseline-mode shared+flags --min-runs 3`
- TIER2：
  - `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=baseline --runs=5 --warmup=2 --runner-mode=prepared --kernel-variant=scalar --bitsup-layout=pair --cpu-timing --timeout=300 --csv=out/metal_gac_v315_dispatch_timing_split_tier2.csv --quiet`
  - `python3 tests/python/metal_gac_analyze.py out/metal_gac_v315_dispatch_timing_split_tier2.csv --top=10 --baseline-mode shared+flags --regression-threshold 1.05 --min-runs 3`

## Rollback

- 新字段都是 additive，旧 CSV 仍可分析。
- 不改变 `dispatch_ms` / `solve_ms` 口径；如 timing split 有问题，移除新字段即可
  回到旧统计行为。
- runtime policy、CTA、bulk sync、auto fallback 均不读取该实验路径。

## Links

- changelog:
  [Metal GAC v3.15 Dispatch Timing Split Changelog](CPIM_METAL_METAL_GAC_V315_DISPATCH_TIMING_SPLIT_CHANGELOG_2026_05_04.md)
- roadmap:
  [Metal GAC Long Term Optimization](../METAL_GAC_LONG_TERM_OPTIMIZATION.md)
