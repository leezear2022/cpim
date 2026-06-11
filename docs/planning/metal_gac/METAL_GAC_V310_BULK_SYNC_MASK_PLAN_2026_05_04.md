---
status: active
updated: 2026-05-04T00:00:00Z
type: plan
topic: cpim-metal
slug: metal-gac-v310-bulk-sync-mask
stage: s07
---

# Metal GAC v3.10 Bulk Sync Deletion Mask Plan

## Goal

- 从 CTA owner 路线切到 default-off `bulk_sync_deletion_mask`。
- 目标不再是让 CTA worklist 全局 promote，而是评估“大例子 / 传播重例子”
  是否能通过 bulk-synchronous deletion mask 受益。
- `frontier_mode=auto`、默认 `frontier_mode=flags`、CTA owner/queue 实验路径均不变。

## Scope

- 新增 `frontier_mode=bulk_sync_mask`，要求 directional bitSup，effective kernel 固定为
  `word_parallel`；缺少 directional bitSup 时显式 fallback 到 `flags`。
- 新增两阶段 Metal kernel：
  - revise 阶段只计算 `delete_masks[var][word]`，不直接修改 domain；
  - apply 阶段统一 drain mask、更新 `bit_dom/domain_sizes`，并生成下一轮 frontier。
- 新增 bulk stats：
  - `bulk_mask_proposed_deletion_count`
  - `bulk_mask_actual_deletion_count`
  - `bulk_mask_changed_word_count`
  - `bulk_mask_frontier_push_count`
  - `bulk_mask_rounds`
- analyzer 新增 `large_any` / `large_prop` gate，单独解释大例子表现。
- Tensor-core-like 路线不进入本轮 correctness path；CUDA 参考继续按 bitset、warp/subwarp
  与 shared packing 路线解释。

## Validation

- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py codex-docops-logic/scripts/dol.py`
- `cmake --build build_metal --target benchmark_metal_gac -j8`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=1 --warmup=0 --verify=true --runner_mode=prepared --frontier_mode=bulk_sync_mask --kernel_variant=word_parallel --bitsup_layout=directional --csv=out/metal_gac_v310_bulk_sync_mask_single.csv`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=bulk_sync --runs=3 --warmup=1 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cpu-timing --timeout=60 --csv=out/metal_gac_v310_bulk_sync_mask_smoke.csv`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=bulk_sync --runs=5 --warmup=2 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cpu-timing --timeout=300 --csv=out/metal_gac_v310_bulk_sync_mask_tier2.csv --quiet`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v35_tier2_baseline.csv out/metal_gac_v35_tier2_frontier.csv out/metal_gac_v39_vebo_weighted_local16_tier2.csv out/metal_gac_v310_bulk_sync_mask_tier2.csv --top=10 --recommend-policy --baseline-mode shared+flags --regression-threshold 1.05 --min-runs 3`
- `git diff --check`
- `python3 codex-docops-logic/scripts/dol.py lint --soft`

## Results

- single smoke：CPU verify 通过；`deletions=78`，
  `bulk_mask_proposed_deletion_count=78`，
  `bulk_mask_actual_deletion_count=78`。
- metal-smoke：15/15 OK，`avg_solve_ms=0.694`。
- TIER2：380/383 OK，3 个 ERROR 均为历史 `unsupported_non_binary_extension`。
- TIER2 absolute：`solve_ms p50=0.712 p95=10.474`，
  `dispatch_count_avg=6.61`，`bulk_mask_actual_deletion_count_avg=168.54`，
  `bulk_mask_rounds_avg=3.30`。
- Combined gate：
  - `bulk_vs_shared+flags p50=1.86x p95=3.17x`；
  - `large_any p50=1.87x p95=2.98x`；
  - `large_prop p50=1.94x p95=2.76x`；
  - `dispatch_ratio_vs_baseline p50=2.00x p95=2.04x`；
  - `actual_deletion_mismatch_rows=0`。

## Decision

- `bulk_sync_mask` 已作为 report-only frontier path 接入，但 v3.10 gate 拒绝 promote。
- correctness 成立，`actual_deletion_mismatch_rows=0`；失败主因是每轮 revise/apply
  两次 dispatch 带来的成本翻倍。
- 不主打“大传播例子有效”；下一步若继续 bulk 路线，应先评估 apply/revise fusion、
  indirect multiround 或 dirty-var pull，避免继续增加 host dispatch。
