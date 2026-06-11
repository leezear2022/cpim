---
status: active
updated: 2026-05-04T00:00:00Z
type: changelog
topic: cpim-metal
slug: metal-gac-v311-dirty-var-pull
stage: s07
---

# Metal GAC v3.11 Dirty Var Pull Changelog

## Summary

- 新增 CTA worklist default-off `dirty_var_pull` handoff。
- 跨 owner subscription 不再直接 push global next active，而是标记 dirty var；
  host 轮末扫描 dirty vars 的 subscriptions 生成下一轮 frontier。
- CSV、benchmark stdout、ablation、analyzer 新增 dirty pull stats。
- analyzer CTA gate 将 `cta_handoff_mode` 纳入分组，避免与旧 push 数据混读。

## Changes

- `MetalCtaHandoffMode` 新增：
  - `push_constraints`
  - `dirty_var_pull`
- `MetalGacOptions` 新增 `cta_handoff_mode`，默认 `push_constraints`。
- `benchmark_metal_gac` 新增 `--cta_handoff_mode`。
- Metal CTA kernel 新增 `cta_dirty_var_epochs` buffer：
  - same-owner enqueue 仍走 CTA local queue；
  - cross-owner enqueue 在 `dirty_var_pull` 下只标脏变量；
  - `cross_push_avoided_count` 记录被跳过的 global cross push。
- Host 侧新增 dirty-var pull：
  - CTA dispatch 完成后扫描 dirty vars；
  - 按 subscription table 去重填充 `next_active_constraints`；
  - 统计 scan/hit。
- `metal_gac_ablation.py` 透传 `--cta-handoff-mode` 并写入 CSV。
- `metal_gac_analyze.py`：
  - mode summary 显示 handoff 与 dirty stats；
  - CTA gate 按 owner、queue、handoff、local budget、replay budget 分组；
  - 输出 `dirty_pull_stats`。

## Validation

- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py codex-docops-logic/scripts/dol.py`
- `cmake --build build_metal --target benchmark_metal_gac -j8`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=1 --warmup=0 --verify=true --runner_mode=prepared --frontier_mode=cta_worklist --kernel_variant=word_parallel --bitsup_layout=directional --cta_owner_mode=vebo_weighted --cta_queue_mode=bounded_replay --cta_local_round_budget=16 --cta_replay_round_budget=8 --cta_handoff_mode=dirty_var_pull --csv=out/metal_gac_v311_dirty_var_pull_single.csv`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=cta --runs=3 --warmup=1 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=vebo_weighted --cta-queue-mode=bounded_replay --cta-local-round-budget=16 --cta-replay-round-budget=8 --cta-handoff-mode=dirty_var_pull --cpu-timing --timeout=60 --csv=out/metal_gac_v311_dirty_var_pull_smoke.csv`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=cta --runs=5 --warmup=2 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=vebo_weighted --cta-queue-mode=bounded_replay --cta-local-round-budget=16 --cta-replay-round-budget=8 --cta-handoff-mode=dirty_var_pull --cpu-timing --timeout=300 --csv=out/metal_gac_v311_dirty_var_pull_tier2.csv --quiet`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v35_tier2_baseline.csv out/metal_gac_v35_tier2_frontier.csv out/metal_gac_v39_vebo_weighted_local16_tier2.csv out/metal_gac_v310_bulk_sync_mask_tier2.csv out/metal_gac_v311_dirty_var_pull_tier2.csv --top=10 --recommend-policy --baseline-mode shared+flags --regression-threshold 1.05 --min-runs 3`

## Results

- single smoke：CPU verify 通过。
- metal-smoke：15/15 OK，`avg_solve_ms=0.448`。
- TIER2：380/383 OK，3 个 ERROR 均为历史 `unsupported_non_binary_extension`。
- TIER2 absolute：`solve_ms p50=0.470 p95=3.202`。
- 对比 v3.9 `vebo_weighted local=16 replay=8`：
  - `p50 0.513 -> 0.470`；
  - `p95 3.357 -> 3.202`；
  - 45/76 个实例更快。
- dirty handoff 计数：
  - `cross_push_avoided_count sum=175752`；
  - `dirty_pull_scan_count sum=110256`；
  - `dirty_pull_hit_count sum=67170`；
  - `cta_cross_push_count sum=0`。
- Combined gate：
  - `cta_vs_shared+flags p50=1.31x p95=3.00x`；
  - `cta_vs_best_worklist p50=1.48x p95=2.98x`；
  - `host_round_ratio_vs_baseline p50=1.00x p95=1.00x`；
  - `decision=report_only`。

## Decision

- v3.11 correctness 与消融接入完成。
- `dirty_var_pull` 比 v3.9 CTA 有小幅收益，但没有通过 shared+flags 或 best worklist
  promote gate。
- 新路径保持 report-only，`frontier_mode=auto` 不读取。
