---
status: active
updated: 2026-05-03T14:09:46Z
type: changelog
topic: cpim-metal
slug: metal-gac-v36-cta-owner-partition-exploration
stage: s07
---

# Metal GAC v3.6 CTA Owner Partition Exploration Changelog

## Summary

- 新增 v3.6 CTA owner partition 分叉探索备忘。
- 已追加 `owner_map_static` 最小实验路径；默认仍是 `modulo` owner，不改变
  `auto` 或 stable fallback。
- v3.6 承接 v3.5 结论：当前 `cid % cta_count` CTA owner 策略未降低 host round，
  且 TIER2 p95 明显回退，因此后续只能以 evidence gate 方式分支探索。
- 根据后续判断，v3.6 优先级调整为三条主线：
  `primal_edge_cut_owner`、`vebo_weighted_owner`、`bulk_sync_deletion_mask`。

## Changes

- 新增独立小计划：
  `docs/planning/metal_gac/METAL_GAC_V36_CTA_OWNER_PARTITION_EXPLORATION_PLAN_2026_05_03.md`。
- 新增独立小 changelog：
  `docs/planning/metal_gac/METAL_GAC_V36_CTA_OWNER_PARTITION_EXPLORATION_CHANGELOG_2026_05_03.md`。
- 计划文档新增 3 个优先方向：
  - `primal_edge_cut_owner`：把 CSP 看成变量 primal graph，用 edge-cut owner
    partition 降低跨 CTA 变量边界；
  - `vebo_weighted_owner`：借鉴 VEBO-style weighted ordering，平衡
    `2 * bit_words * active_frequency` 与 variable subscription pressure；
  - `bulk_sync_deletion_mask`：CTA 先算 deletion masks，merge/reduce 阶段统一
    apply deletions，再生成下一轮 frontier。
- 原 6 个后续分支保留为支撑或补充实验：
  - `owner_map_static`
  - `owner_bucketed_seed`
  - `dirty_var_pull`
  - `hub_replication`
  - `hierarchical_steal`
  - `indirect_multiround`
- 每个分支都固定记录 hypothesis、实现草图、stats、evidence CSV、promote gate
  和 reject gate。
- 更新 Metal GAC 索引、长期路线、迁移计划和 `CHANGES_ZH.md` 导航。
- DocOps 保持 `rm: v03`，追加 doc/ch/va 事件。
- 实现 `owner_map_static`：
  - 新增 `MetalCtaOwnerMode` 与 `--cta_owner_mode=modulo|static_edge_cut`；
  - Host 侧构建 `owner_of_constraint[cid]` buffer，`static_edge_cut` 使用
    constraint subscription adjacency 的轻量 greedy owner 分配；
  - CTA seed 与 `gac_revise_cta_worklist_kernel` 统一读取 owner map；
  - CSV/analyzer 新增 `cta_owner_mode`、`owner_map_build_ms`、
    `owner_balance_p95`、`owner_local_push_count`、`owner_cross_push_count`；
  - `metal_gac_ablation.py --cta-owner-mode` 转发实验开关，analyzer 按 owner
    mode 拆分 CTA summary/gate。

## Validation

- `python3 -m py_compile codex-docops-logic/scripts/dol.py`
- `git diff --check`
- `python3 codex-docops-logic/scripts/dol.py lint --soft`
- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py codex-docops-logic/scripts/dol.py`
- `cmake --build build_metal --target benchmark_metal_gac -j8`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=1 --warmup=0 --verify=true --runner_mode=prepared --frontier_mode=cta_worklist --kernel_variant=word_parallel --bitsup_layout=directional --cta_owner_mode=static_edge_cut --csv=out/metal_gac_v36_owner_map_static_single.csv`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=cta --runs=3 --warmup=1 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=static_edge_cut --cpu-timing --timeout=60 --csv=out/metal_gac_v36_owner_map_static_smoke.csv`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v36_owner_map_static_smoke.csv --top=5 --recommend-policy --baseline-mode shared+flags --regression-threshold 1.05 --min-runs 3`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=cta --runs=5 --warmup=2 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=static_edge_cut --cpu-timing --timeout=300 --csv=out/metal_gac_v36_owner_map_static_tier2.csv --quiet`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v35_tier2_baseline.csv out/metal_gac_v35_tier2_frontier.csv out/metal_gac_v36_owner_map_static_tier2.csv --top=10 --recommend-policy --baseline-mode shared+flags --regression-threshold 1.05 --min-runs 3`

## Results

- 单例 `gac_bitwords2.xml`：CPU verify 通过，`host_round_count=1`，
  `owner_balance_p95=1.000`，`owner_cross_push_count=0`。
- metal-smoke `static_edge_cut`：15/15 OK。
- TIER2 `static_edge_cut`：380/383 OK，3 个 ERROR 均为历史
  `unsupported_non_binary_extension`；`solve_ms p50=0.797 p95=3.503`。
- Combined CTA gate：
  - `cta_vs_shared+flags p50=1.83x p95=4.51x`；
  - `cta_vs_best_worklist p50=1.79x p95=4.12x`；
  - `host_round_ratio_vs_baseline p50=1.00x p95=1.00x`；
  - `cta_overflow_count p95=1 max=2`；
  - `decision=report_only`。
- 结论：`owner_map_static` 不通过 v3.6 promote gate；下一步不应提升到
  `auto`，应转向 `vebo_weighted_owner` 或专门处理 seed/overflow。

## Decisions

- `cta_worklist` 仍保持 report-only，不进入 `frontier_mode=auto`。
- 不继续沿 `cid % cta_count` 单一路径推进；后续先做
  `primal_edge_cut_owner` 和 `vebo_weighted_owner` 的 owner partition evidence。
- `owner_map_static` 作为显式实验开关落地，默认仍使用旧 `modulo` owner；
  TIER2 gate 后保持 report-only。
- `bulk_sync_deletion_mask` 是更大改动，只在 owner partition 仍被 atomic/cross-push
  卡住时推进。
- 若三条优先主线仍不能降低 `host_round_count` 或 `metal_cpu_solve_ratio`，下一步
  转向 Batch/SAC 多任务吞吐，而不是继续单实例 CTA。

## Follow-Ups

- 第一实现建议从 `primal_edge_cut_owner` 的最小版 `owner_map_static` 开始，因为它
  不要求 GPU self-dispatch，也最容易解释 cross-owner push 是否下降。
- 第二实现建议做 `vebo_weighted_owner`，它比完整 METIS 轻，更适合 v3.6 第一轮。
- `owner_bucketed_seed` 紧随其后，用于区分 owner 策略问题和 seed overhead 问题。
- `indirect_multiround` 只在 analyzer 继续显示 host submit/round 为主瓶颈时推进。

## Links

- plan:
  [Metal GAC v3.6 CTA Owner Partition Exploration Plan](METAL_GAC_V36_CTA_OWNER_PARTITION_EXPLORATION_PLAN_2026_05_03.md)
- roadmap:
  [Metal GAC Long Term Optimization](../METAL_GAC_LONG_TERM_OPTIMIZATION.md)
