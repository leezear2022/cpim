---
status: implemented
updated: 2026-05-03
type: plan
topic: cpim-metal
slug: metal-gac-v3-cta-worklist
stage: s07
---

# Metal GAC v3.4 CTA-local Persistent Worklist Plan

## Goal

- 针对 v3 CPU vs Metal evidence 中暴露的 dispatch 往返瓶颈，设计下一步
  Metal GAC 优化：在单次 kernel dispatch 内完成 CTA/threadgroup 局部多轮
  worklist 推进。
- 借鉴 CUDA Batch3A dynamic submission 的 per-block queue/mask/tail 结构，
  但不照搬 CUDA cooperative persistent kernel，也不让多个 CTA 直接竞争同一个
  全局 constraint queue。
- 第一版目标是证明“减少 host round 数”能否显著降低
  `metal_cpu_solve_ratio` 与 `dispatch_share`，不是立刻改变默认 Metal fallback。

## Scope

- 新增可消融路径：`frontier_mode=cta_worklist`。
- 每个 CTA/threadgroup 拥有独立的 active queue、next queue、dedup stamp/mask、
  tail 与 overflow 标志。
- CTA 内可执行固定预算的 local rounds；跨 CTA 的传播第一版只写 global flags，
  由 host outer loop 或 merge kernel 重新播种下一次 dispatch。
- 保持默认稳定路径：
  `cold + shared + flags + scalar + pair + cpu reset`。
- 不在本计划内实现 SAC/Batch、global AllDifferent、predicate/intension、
  非二元 extension，也不实现真正 GPU self-dispatch。
- 第一版真实执行组合为 `cta_worklist + word_parallel + directional`；其它请求
  不改变默认 `auto` policy。

## Tasks

- v3.4.0 evidence guard：
  - 固定 CPU vs Metal auto、baseline、aggressive 三组 guard CSV；
  - analyzer 继续报告 `dispatch_share`、`kernel_share`、`reset_share`、
    `metal_cpu_solve_ratio`；
  - 只有当当前 evidence 仍显示 dispatch/round 是主瓶颈时进入 kernel prototype。
- v3.4.1 CTA queue layout：
  - 已新增 `cta_queue_A/B[cta_count][queue_capacity]`；
  - 已新增 `cta_stamps[cta_count][num_constraints]`；
  - 已新增 `cta_tail_A/B[cta_count]`；
  - 约束到 CTA 的 owner 初版使用 `cid % cta_count` 静态 partition，避免多个
    CTA 写同一队列。
- v3.4.2 CTA-local revise kernel：
  - 已新增 `gac_revise_cta_worklist_kernel`；
  - 一个 threadgroup 消费自己的 queue；
  - local loop 内用 A/B queue 双缓冲推进最多 8 轮；
  - enqueue 使用 CTA-local stamp 去重；
  - domain deletion 使用 atomic clear，依靠 old-value popcount 避免重复扣
    `domain_sizes`；
  - 影响到非本 CTA owner 的 constraint 时只写 global cross flags。
- v3.4.3 host/merge outer loop：
  - 第一版保留 host outer loop：每个 dispatch 前由 CPU 将 global active list
    分发到 CTA owner queue；
  - 已记录 `cta_local_rounds`、`cta_queue_push_count`、
    `cta_cross_push_count`、`cta_overflow_count` 与 `host_round_count`。
- v3.4.4 fallback 与 auto gate：
  - overflow、budget exceeded、verification mismatch 时回退到 v2 stable worklist
    或 `flags+scalar`；
  - `auto` 不在第一版改默认，只由 analyzer recommender 报告是否值得启用。

## Validation

- 文档/静态：
  - `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py codex-docops-logic/scripts/dol.py`
  - `git diff --check`
  - `python3 codex-docops-logic/scripts/dol.py lint --soft`
- prototype correctness：
  - `cmake --build build_metal --target benchmark_metal_gac compare_cpu_metal -j`
  - `ctest --test-dir build_metal -R compare_cpu_metal --output-on-failure`
  - `ctest --test-dir build_metal -R benchmark_metal_gac --output-on-failure`
  - `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=3 --warmup=1 --verify=true --runner_mode=prepared --frontier_mode=cta_worklist --kernel_variant=word_parallel --bitsup_layout=directional --csv=out/metal_gac_v34_cta_worklist_smoke.csv`
  - `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=cta --runs=2 --warmup=1 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --timeout=60 --csv=out/metal_gac_v34_cta_smoke.csv`
- performance/evidence：
  - `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=auto --runs=5 --warmup=2 --runner-mode=prepared --cpu-timing --timeout=300 --csv=out/metal_gac_v34_tier2_auto.csv --quiet`
  - `python3 tests/python/metal_gac_analyze.py out/metal_gac_v34_tier2_auto.csv --top=10 --recommend-policy --baseline-mode shared+flags --regression-threshold 1.05 --min-runs 3`
- acceptance：
  - 不允许 CPU/Metal verification mismatch；
  - `cta_overflow_count > 0` 的实例必须自动回退并分类记录；
  - `host_round_count` 相比 v2 worklist/flags 必须下降；
  - 若 `metal_cpu_solve_ratio` p50/p95 没有明显下降，则不把该路径加入
    recommender 的 preferred policy。

## Rollback

- 不启用 `cta_worklist` 时现有 Metal 路径完全不变。
- 新 kernel variant 与 queue buffer 只在显式实验 flag 下分配和运行。
- overflow 或 local budget hit 会把剩余本地 queue 溢出到 global next active list；
  CPU verify 失败时 benchmark 直接失败，默认 fallback 不受影响。

## Links

- changelog:
  [Metal GAC v3.4 CTA-local Persistent Worklist Changelog](CPIM_METAL_METAL_GAC_V3_CTA_WORKLIST_CHANGELOG_2026_05_03.md)
- roadmap:
  [Metal GAC Long Term Optimization](../METAL_GAC_LONG_TERM_OPTIMIZATION.md)
