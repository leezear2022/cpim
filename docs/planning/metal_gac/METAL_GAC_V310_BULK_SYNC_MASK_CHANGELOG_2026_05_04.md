---
status: active
updated: 2026-05-04T00:00:00Z
type: changelog
topic: cpim-metal
slug: metal-gac-v310-bulk-sync-mask
stage: s07
---

# Metal GAC v3.10 Bulk Sync Deletion Mask Changelog

## Summary

- 新增 default-off `frontier_mode=bulk_sync_mask`。
- 新增 bulk revise/apply 两阶段 deletion mask 路径，避免 revise 阶段直接修改 domain。
- CSV、benchmark stdout、ablation、analyzer 新增 bulk mask stats。
- analyzer 新增 `large_any` / `large_prop` gate，用于解释大例子和传播重例子表现。
- Tensor-core-like 约束检查不进入 v3.10 主线，后续如需评估单独开
  `packed8_probe` microbench。

## Changes

- `MetalFrontierMode` 新增 `kBulkSyncMask`。
- `benchmark_metal_gac --frontier_mode` 支持 `bulk_sync_mask`。
- `gac_revise_bulk_mask_kernel`：
  - 读取 active constraints、directional bitSup 与当前 domain snapshot；
  - 只对 `delete_masks[var][word]` 做 atomic OR；
  - 记录 proposed deletion count。
- `gac_apply_bulk_mask_kernel`：
  - drain deletion mask；
  - 统一 apply 到 `bit_dom/domain_sizes`；
  - 对 changed var 的 subscriptions 生成下一轮 worklist frontier；
  - 记录 actual deletion、changed word、frontier push stats。
- `metal_gac_ablation.py` 新增 `--mode-preset=bulk_sync`。
- `metal_gac_analyze.py`：
  - mode summary 显示 bulk stats；
  - 新增 `[bulk sync mask gate]`；
  - 新增 `large_any` 与 `large_prop` 分类。

## Validation

- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py codex-docops-logic/scripts/dol.py`
- `cmake --build build_metal --target benchmark_metal_gac -j8`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=1 --warmup=0 --verify=true --runner_mode=prepared --frontier_mode=bulk_sync_mask --kernel_variant=word_parallel --bitsup_layout=directional --csv=out/metal_gac_v310_bulk_sync_mask_single.csv`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=bulk_sync --runs=3 --warmup=1 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cpu-timing --timeout=60 --csv=out/metal_gac_v310_bulk_sync_mask_smoke.csv`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=bulk_sync --runs=5 --warmup=2 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cpu-timing --timeout=300 --csv=out/metal_gac_v310_bulk_sync_mask_tier2.csv --quiet`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v35_tier2_baseline.csv out/metal_gac_v35_tier2_frontier.csv out/metal_gac_v39_vebo_weighted_local16_tier2.csv out/metal_gac_v310_bulk_sync_mask_tier2.csv --top=10 --recommend-policy --baseline-mode shared+flags --regression-threshold 1.05 --min-runs 3`

## Results

- single smoke：CPU verify 通过。
- single smoke stats：`deletions=78`，
  `bulk_mask_proposed_deletion_count=78`，
  `bulk_mask_actual_deletion_count=78`，
  `bulk_mask_changed_word_count=4`，
  `bulk_mask_frontier_push_count=1`，
  `bulk_mask_rounds=2`。
- metal-smoke：15/15 OK，`avg_solve_ms=0.694`。
- TIER2：380/383 OK，3 个 ERROR 均为历史 `unsupported_non_binary_extension`。
- TIER2 absolute：`solve_ms p50=0.712 p95=10.474`，
  `bulk_mask_actual_deletion_count_avg=168.54`，
  `bulk_mask_rounds_avg=3.30`。
- Combined gate：
  - `bulk_vs_shared+flags p50=1.86x p95=3.17x`；
  - `large_any p50=1.87x p95=2.98x`；
  - `large_prop p50=1.94x p95=2.76x`；
  - `dispatch_ratio_vs_baseline p50=2.00x p95=2.04x`；
  - `actual_deletion_mismatch_rows=0`。

## Decision

- v3.10 已具备可消融的 bulk-synchronous deletion mask 路径。
- correctness 成立，但 TIER2 与 `large_prop` 都未通过 promote gate；本轮拒绝主打
  “大传播例子有效”。
- 失败主因是 bulk path 每轮 revise/apply 两个 dispatch，`dispatch_ratio_vs_baseline`
  约为 2x。
- `frontier_mode=auto` 不读取 bulk path。
