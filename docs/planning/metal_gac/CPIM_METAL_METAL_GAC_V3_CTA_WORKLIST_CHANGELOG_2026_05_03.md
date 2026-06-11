---
status: implemented
updated: 2026-05-03
type: changelog
topic: cpim-metal
slug: metal-gac-v3-cta-worklist
stage: s07
---

# Metal GAC v3.4 CTA-local Persistent Worklist Changelog

## Summary

- 实现 Metal GAC v3.4 实验路径：`frontier_mode=cta_worklist`。
- 默认 Metal fallback 和 `auto` policy 不变。
- v3.3 simdgroup gate 结论保持：当前瓶颈仍以 dispatch/round 往返为主，不进入
  simdgroup kernel 实现。

## Changes

- 新增独立小计划：
  `docs/planning/metal_gac/CPIM_METAL_METAL_GAC_V3_CTA_WORKLIST_PLAN_2026_05_03.md`。
- `MetalFrontierMode` / `benchmark_metal_gac --frontier_mode` 新增
  `cta_worklist`。
- 新增 Metal kernel `gac_revise_cta_worklist_kernel`：
  - per-CTA queue A/B；
  - per-CTA stamp 去重；
  - 单 dispatch 内最多 8 轮 local worklist；
  - 跨 CTA 传播写 global next active list。
- `MetalGacStats` / benchmark CSV / ablation CSV / analyzer 新增：
  - `cta_local_rounds`
  - `cta_queue_push_count`
  - `cta_cross_push_count`
  - `cta_overflow_count`
  - `host_round_count`
- `metal_gac_ablation.py --mode-preset=cta` 新增 CTA 实验扫描入口。
- 实现继续从 CUDA Batch3A 借鉴：
  - per-block/per-CTA queue；
  - per-CTA dedup mask/stamp；
  - A/B queue 双缓冲；
  - overflow fallback。
- 计划明确不采用：
  - 多个 CTA 竞争同一个全局 c queue；
  - CUDA cooperative persistent kernel；
  - v3.4 第一版 GPU self-dispatch；
  - 未经 evidence gate 的 simdgroup kernel。
- 更新 Metal GAC 索引、长期路线、迁移计划与 `CHANGES_ZH.md`。

## Validation

- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py codex-docops-logic/scripts/dol.py`
- `cmake --build build_metal --target benchmark_metal_gac compare_cpu_metal -j`
- `ctest --test-dir build_metal -R compare_cpu_metal --output-on-failure`
- `ctest --test-dir build_metal -R benchmark_metal_gac --output-on-failure`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=3 --warmup=1 --verify=true --runner_mode=prepared --frontier_mode=cta_worklist --kernel_variant=word_parallel --bitsup_layout=directional --csv=out/metal_gac_v34_cta_worklist_smoke.csv`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=cta --runs=2 --warmup=1 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --timeout=60 --csv=out/metal_gac_v34_cta_smoke.csv`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v34_cta_smoke.csv --top=5 --recommend-policy --baseline-mode shared+flags --regression-threshold 1.05 --min-runs=2`
- `git diff --check`

## Decisions

- 下一步优化主线锁定为 dispatch/round 压缩，而不是位集微优化。
- CTA-local queue 是第一版安全设计：跨 CTA 传播先写 global flags，再由 host
  outer loop 播种下一次 dispatch。
- `auto` 初期不启用该路径；只有 analyzer evidence 证明 p50/p95 与
  `metal_cpu_solve_ratio` 明显改善后，再另起计划讨论是否纳入 recommender。

## Follow-Ups

- 重跑 CPU vs Metal TIER2 auto 对照，判断 dispatch 往返是否被有效压缩。
- 对比 `flags/worklist/cta_worklist` 三组 TIER2 p50/p95；若 `cta_worklist`
  p95 仍差，则保持 report-only，不进 `auto`。

## Links

- plan:
  [Metal GAC v3.4 CTA-local Persistent Worklist Plan](CPIM_METAL_METAL_GAC_V3_CTA_WORKLIST_PLAN_2026_05_03.md)
- roadmap:
  [Metal GAC Long Term Optimization](../METAL_GAC_LONG_TERM_OPTIMIZATION.md)
