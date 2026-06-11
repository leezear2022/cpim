---
status: active
updated: 2026-05-03T14:09:46Z
type: plan
topic: cpim-metal
slug: metal-gac-v36-cta-owner-partition-exploration
stage: s07
---

# Metal GAC v3.6 CTA Owner Partition Exploration Plan

## Goal

- 承接 v3.5 `cta_worklist` TIER2 gate 未通过后的 owner partition 重设计。
- 本文是分叉探索备忘：记录值得试的方案、判退门槛、证据文件和回滚锚点；
  失败后回到本文继续开新分支。
- 初始落账为 docs-only；本轮已追加 `owner_map_static` 最小实验路径。
  `frontier_mode=auto` 和默认 fallback 仍不变。

## Scope

- 包含：
  - Metal CTA owner partition 的可试分支；
  - 每个分支的 hypothesis、实现草图、stats、smoke/TIER2 命令、promote/reject gate；
  - 与 v3.5 evidence、Metal API 约束和 GPU worklist 论文的导航链接。
- 不包含：
  - 将新 owner 分支提升为默认或 auto；
  - GPU self-dispatch / unbounded persistent kernel；
  - SAC/Batch、global AllDifferent、predicate/intension、非二元 extension 支持；
  - 将 `cta_worklist` 或任何新 owner 分支加入 `auto`。

## Tasks

### Current Anchor

- v3.5 TIER2 combined gate：
  - `cta_vs_shared+flags p50=1.27x p95=2.99x p99=4.04x`；
  - `cta_vs_best_worklist p50=1.37x p95=2.99x`；
  - `host_round_ratio_vs_baseline p50=1.00x p95=1.00x`；
  - `cta_overflow_count p95=0 max=0`；
  - `decision=report_only`。
- 结论：不能继续把 `cid % cta_count` 当作默认 owner 策略；下一步必须重设
  owner mapping、seed、cross-owner handoff 或 dispatch structure。

### Metal Constraints

- Metal 支持 indirect dispatch / indirect command buffer，可减少 CPU 参数参与；
  但 shader 内不能像 CUDA Dynamic Parallelism 那样无限自我 launch kernel。
- Threadgroup 内有 barrier 和 threadgroup memory；跨 threadgroup 没有 grid-wide sync。
- 因此真实收敛仍需要以下之一：
  - fixed-budget local propagation；
  - host outer loop；
  - CPU 预编码多轮 command/indirect dispatch；
  - 或可证明不会死锁/不会遗漏传播的 bounded queue protocol。

### Exploration Branches

优先级不再把所有分支摊平。v3.6 后续应先探索三条更有希望的主线，再把 seed、
steal、indirect multi-round 作为支撑或补充实验。

### Priority Directions

| Direction | Why It Is Promising | First Implementation | Promote Signal |
|-----------|---------------------|----------------------|----------------|
| `primal_edge_cut_owner` | 把 CSP 看成 primal graph：变量是点，二元约束是边。按 `cid % cta_count` 不理解变量局部性，容易制造跨 CTA 传播；edge-cut owner partition 的目标是最小化跨 CTA 变量边界，同时平衡 constraint 数、subscription 数和 domain work。 | Host 侧构建 constraint adjacency：两个 constraints 共享变量即相邻；先做轻量 greedy/BFS partition，不引入 METIS 依赖；输出 `owner_of_constraint[cid]` 和 per-owner load。 | `cta_cross_push_count` p50/p95 下降，`owner_balance_p95 <= 1.25`，`solve_ms p95` 不慢于 baseline 5%。 |
| `vebo_weighted_owner` | VEBO-style ordering 同时平衡边数和 unique destinations，适合避免少数高连接变量把某个 partition 压爆。对 GAC，`constraint_weight = 2 * bit_words * active_frequency`，`variable_pressure = subscription_degree`，目标是同时平衡 revise work 和 touched variables。 | 不上完整图分区库；先按 weighted constraint work 排序，用最小负载 owner + touched-variable penalty 分配；active frequency 初版用静态订阅度/历史 CSV bucket 近似。 | 比 `primal_edge_cut_owner` 更低的 `owner_balance_p95` 或 `metal_cpu_solve_ratio p50/p95`，且不增加 verify 风险。 |
| `bulk_sync_deletion_mask` | Metal 没有跨 threadgroup grid-wide sync；让 CTA local loop 直接反复改全局 domain 会放大 atomic 和顺序不确定性。改成 bulk-synchronous：每个 CTA 先算 deletion masks，merge/reduce 阶段统一 apply deletions，再产生下一轮 frontier，更接近 Gunrock frontier/BSP 模型。 | 新增 default-off 两阶段 prototype：revise kernel 写 `delete_mask[var][word]`，merge kernel 对 mask 做 atomic OR/apply，第三步生成 next frontier；第一版允许 host outer loop。 | `domain atomic` 和 `cta_cross_push_count` 下降；即使 `dispatch_count` 增加，`solve_ms p95` 不能超过 baseline 5%；若 dispatch 成本压过收益则 reject。 |

### Support Branches

| Branch | Hypothesis | Implementation Sketch | Required Stats | Evidence CSV |
|--------|------------|-----------------------|----------------|--------------|
| `owner_map_static` | `primal_edge_cut_owner` 的最小落地形态，确认 owner map 是否比 `cid % cta_count` 有收益。 | 已实现 default-off `--cta_owner_mode=static_edge_cut`：基于 constraint subscription adjacency 预计算 `owner_of_constraint[cid]`，CPU seed 与 kernel 统一读取 owner map。 | `owner_cross_push_count`、`owner_local_push_count`、`owner_balance_p95`、`owner_map_build_ms`。 | `out/metal_gac_v36_owner_map_static_smoke.csv` / `out/metal_gac_v36_owner_map_static_tier2.csv` |
| `owner_bucketed_seed` | 当前 CPU seed 到 CTA queue 的固定分发没有压缩 host round；先把 active list 按 owner 分桶，可减少 queue 空洞和 seed overhead。 | 新增 seed kernel 或 compact-style bucket pass，把 active constraints 写入 per-owner queue/tail；host 不再逐 active CPU 分发。 | `seed_dispatch_ms`、`seed_bucket_push_count`、`seed_empty_owner_count`、`host_seed_ms`。 | `out/metal_gac_v36_owner_bucketed_seed_smoke.csv` / `out/metal_gac_v36_owner_bucketed_seed_tier2.csv` |
| `dirty_var_pull` | 跨 owner 直接 push constraint 会制造 global queue pressure；标 dirty vars 后由 owner pull 本地订阅约束，可能降低 cross-owner atomics。 | 删除值时设置 `dirty_var_epoch[var]`；每个 owner 扫本地 subscribed constraints，若任一 scope var dirty 则 enqueue 本地 queue。 | `dirty_var_count`、`dirty_pull_scan_count`、`dirty_pull_hit_count`、`cross_push_avoided_count`。 | `out/metal_gac_v36_dirty_var_pull_smoke.csv` / `out/metal_gac_v36_dirty_var_pull_tier2.csv` |
| `hub_replication` | 高 fanout constraint/var 类似图里的 hub；有限复制可降低跨 owner 通信，但需要严控重复 revise。 | 对 top-K fanout constraints 或 subscriptions 做 owner-local mirror；用 global epoch 保证每轮每 cid 只提交一次有效 next active。 | `hub_replicated_count`、`hub_duplicate_suppressed_count`、`hub_cross_push_saved_count`、`mirror_bytes`。 | `out/metal_gac_v36_hub_replication_smoke.csv` / `out/metal_gac_v36_hub_replication_tier2.csv` |
| `hierarchical_steal` | 静态 owner 可能负载不均；per-CTA queue 加 bounded global overflow/steal pool 可改善 tail latency。 | Owner 先消费 local queue；local empty 时固定预算尝试从 global overflow 或邻近 owner steal；禁止无界自旋。 | `steal_attempt_count`、`steal_success_count`、`overflow_pool_push_count`、`empty_owner_rounds`。 | `out/metal_gac_v36_hierarchical_steal_smoke.csv` / `out/metal_gac_v36_hierarchical_steal_tier2.csv` |
| `indirect_multiround` | 如果主要瓶颈仍是 host round 往返，预编码多轮 indirect dispatch/ICB 可能减少 CPU wait/submit 成本。 | 探索 host 一次提交 bounded N 轮：每轮 kernel 写下一轮 indirect args；command buffer 中预放 revise/merge/reset stats pass。 | `indirect_rounds_encoded`、`indirect_rounds_used`、`host_submit_count`、`indirect_zero_work_rounds`。 | `out/metal_gac_v36_indirect_multiround_smoke.csv` / `out/metal_gac_v36_indirect_multiround_tier2.csv` |

### Owner Map Static Evidence

- `owner_map_static` 已跑完 smoke 与 TIER2：
  - smoke：15/15 OK；
  - TIER2：380/383 OK，3 个 ERROR 均为历史
    `unsupported_non_binary_extension`；
  - `solve_ms p50=0.797 p95=3.503`；
  - combined gate：`cta_vs_shared+flags p50=1.83x p95=4.51x`，
    `cta_vs_best_worklist p50=1.79x p95=4.12x`；
  - `host_round_ratio_vs_baseline p50=1.00x p95=1.00x`；
  - `cta_overflow_count p95=1 max=2`；
  - `decision=report_only`。
- 判定：`owner_map_static` 不通过 promote gate，后续不进入 `auto`。
  下一条 owner 分支应优先转向 `vebo_weighted_owner`，或先单独处理 seed/overflow。

### Seed/Overflow Evidence

- v3.7 已按“先处理 seed/overflow，再考虑 `vebo_weighted_owner`”落地：
  [Metal GAC v3.7 Seed/Overflow Plan](METAL_GAC_V37_SEED_OVERFLOW_PLAN_2026_05_03.md)。
- `--cta_queue_mode=spill_replay` 拆分了 `cta_queue_overflow_count`、
  `cta_budget_spill_count` 和 `cta_seed_overflow_count`。
- TIER2 结论：
  - `queue_overflow_p95=0`；
  - `seed_overflow_p95=0`；
  - `budget_spill_p95=1`；
  - `cta_vs_shared+flags p50=1.41x p95=3.54x`；
  - `host_round_ratio_vs_baseline p50=1.00x p95=1.00x`；
  - `decision=report_only`。
- 判定：真 queue/seed overflow 不是主因；剩余瓶颈更像 local budget spill 与
  host round 未下降。下一步优先评估 local budget / bounded replay；若仍不行，
  再转向 `vebo_weighted_owner`。

## Budget/Replay Evidence

- v3.8 follow-up：
  [Metal GAC v3.8 Budget/Replay Plan](METAL_GAC_V38_BUDGET_REPLAY_PLAN_2026_05_03.md)。
- `bounded_replay local=8 replay=8` TIER2：
  - `budget_spill_p95=0`；
  - `replay_drain_p95=1`；
  - `cta_vs_shared+flags p50=1.36x p95=3.49x`；
  - `host_round_ratio_vs_baseline p50=1.00x`；
  - `decision=report_only`。
- `spill_replay local=16` TIER2：
  - `budget_spill_p95=0`；
  - `cta_vs_shared+flags p50=1.39x p95=3.57x`；
  - `host_round_ratio_vs_baseline p50=1.00x`；
  - `decision=report_only`。
- 判定：local budget / bounded replay 能清零 budget spill，但不能降低 host
  round 或解除 p95 regression。下一步转向 `vebo_weighted_owner`。

## VEBO Weighted Owner Evidence

- v3.9 follow-up：
  [Metal GAC v3.9 VEBO Weighted Owner Plan](METAL_GAC_V39_VEBO_WEIGHTED_OWNER_PLAN_2026_05_04.md)。
- `vebo_weighted local=16 replay=8` TIER2：
  - `solve_ms p50=0.513 p95=3.325`；
  - `cta_vs_shared+flags p50=1.41x p95=3.14x`；
  - `cta_vs_best_worklist p50=1.40x p95=3.13x`；
  - `owner_balance_p95_avg=1.28`；
  - `owner_weight_balance_p95_avg=1.22`；
  - `budget_spill_p95=0`；
  - `host_round_ratio_vs_baseline p50=1.00x`；
  - `decision=report_only`。
- 判定：`vebo_weighted_owner` 是正向但不足以 promote 的 owner 分支；若继续
  CTA，下一步应做 owner locality/cross-push hybrid，而不是回到 seed/budget。

## Bulk Sync Mask Evidence

- v3.10 follow-up：
  [Metal GAC v3.10 Bulk Sync Mask Plan](METAL_GAC_V310_BULK_SYNC_MASK_PLAN_2026_05_04.md)。
- `frontier_mode=bulk_sync_mask` 已作为 default-off 路径接入：
  - revise kernel 只写 `delete_masks[var][word]`；
  - apply kernel 统一更新 `bit_dom/domain_sizes` 并生成下一轮 frontier；
  - `auto` 不读取该路径。
- 初步 smoke：
  - single：CPU verify 通过，`deletions=78`，
    `bulk_mask_proposed_deletion_count=78`，
    `bulk_mask_actual_deletion_count=78`；
  - metal-smoke：15/15 OK。
- TIER2：
  - 380/383 OK，3 个 ERROR 均为历史 `unsupported_non_binary_extension`；
  - `solve_ms p50=0.712 p95=10.474`；
  - `bulk_vs_shared+flags p50=1.86x p95=3.17x`；
  - `large_any p50=1.87x p95=2.98x`；
  - `large_prop p50=1.94x p95=2.76x`；
  - `dispatch_ratio_vs_baseline p50=2.00x p95=2.04x`；
  - `actual_deletion_mismatch_rows=0`。
- 判定：correctness 成立，但 revise/apply 双 dispatch 成本压过收益；本分支保持
  report-only，不主打“大传播例子有效”。

## Dirty Var Pull Evidence

- v3.11 follow-up：
  [Metal GAC v3.11 Dirty Var Pull Plan](METAL_GAC_V311_DIRTY_VAR_PULL_PLAN_2026_05_04.md)。
- `--cta_handoff_mode=dirty_var_pull` 已作为 default-off CTA handoff 接入：
  - same-owner subscription 继续进入 CTA local queue；
  - cross-owner subscription 改为标记 dirty var；
  - host 轮末扫描 dirty vars 的 subscriptions，生成下一轮 frontier；
  - `auto` 不读取该路径。
- TIER2：
  - 380/383 OK，3 个 ERROR 均为历史 `unsupported_non_binary_extension`；
  - `solve_ms p50=0.470 p95=3.202`；
  - 相对 v3.9 `vebo_weighted local=16 replay=8`：
    `p50 0.513 -> 0.470`，`p95 3.357 -> 3.202`；
  - 45/76 个实例更快，25/76 个实例慢于 v3.9 超过 5%；
  - `cross_push_avoided_count sum=175752`；
  - `dirty_pull_scan_count sum=110256`；
  - `dirty_pull_hit_count sum=67170`；
  - `cta_cross_push_count sum=0`；
  - `cta_vs_shared+flags p50=1.31x p95=3.00x`；
  - `cta_vs_best_worklist p50=1.48x p95=2.98x`；
  - `host_round_ratio_vs_baseline p50=1.00x p95=1.00x`；
  - `decision=report_only`。
- 判定：dirty-var handoff 小幅改善 CTA owner 路线，但仍未通过 shared+flags 或
  best worklist gate。若继续 CTA，应转向更窄的 bucket policy 或 hub/locality
  hybrid；若目标是默认性能，应回到 `shared+worklist` / flags 的 dispatch 成本优化。

## Dirty Pull Hybrid Evidence

- v3.12 follow-up：
  [Metal GAC v3.12 Dirty Pull Hybrid Plan](METAL_GAC_V312_DIRTY_PULL_HYBRID_PLAN_2026_05_04.md)。
- `--cta_dirty_pull_min_degree` 已作为 default-off CTA handoff 阈值接入：
  - `degree(target) >= threshold` 时 dirty pull；
  - `degree(target) < threshold` 时 direct global push；
  - threshold 默认 0，完全复现 v3.11。
- TIER2：
  - hybrid16：380/383 OK，`solve_ms p50=0.576 p95=2.723`；
  - hybrid8：380/383 OK，`solve_ms avg=0.837 p50=0.488 p95=2.192`。
- hybrid8 对比：
  - v3.9 push：`p50 0.513 -> 0.488`，`p95 3.357 -> 2.192`；
  - v3.11 dirty-all：`p50 0.470 -> 0.488`，`p95 3.202 -> 2.192`。
- hybrid8 stats：
  - `cross_push_avoided_count sum=173638`；
  - `dirty_pull_fallback_push_count sum=2133`；
  - `dirty_pull_scan_count sum=107335`；
  - `dirty_pull_hit_count sum=64739`。
- Gate：
  - `cta_vs_shared+flags p50=1.41x p95=3.21x`；
  - `host_round_ratio_vs_baseline p50=1.00x`；
  - `decision=report_only`。
- Bucket signal：
  - `BH-4-4` bucket `p50_ratio=0.40 p95_ratio=0.43`，
    `regressions_gt_threshold=0`。
- 判定：hybrid threshold 能明显压低 CTA tail，但不能通过全局 gate。CTA 后续只适合
  做 BH-like bucket policy；默认性能线应转向 dispatch 成本或 Batch/SAC。

## Bucket Policy Simulation Evidence

- v3.13 follow-up：
  [Metal GAC v3.13 Bucket Policy Simulation Plan](METAL_GAC_V313_BUCKET_POLICY_SIMULATION_PLAN_2026_05_04.md)。
- analyzer 新增 `[bucket policy simulation]`：
  - 候选路径按完整参数分组，避免 CTA variants 或 baseline variants 混读；
  - bucket 只有在 `p95 <= 1.0x` 且 regression rows 为 0 时才 eligible；
  - policy simulation 中 eligible bucket 走候选路径，其它输入回退 fallback。
- Combined simulation：
  - fallback：`shared+flags`；
  - eligible buckets：1；
  - selected inputs：4/76；
  - policy：`p50=0.312ms p95=1.696ms`；
  - fallback：`p50=0.312ms p95=2.987ms`；
  - `regressions_gt_threshold=0`。
- Eligible bucket：
  - `family=BH-4-4 cons=128-511 dom=<17 bitw=<2 density=0.10-0.50`；
  - selected CTA path：`vebo_weighted + bounded_replay + dirty_var_pull +
    local=16 + replay=8 + dirty_min=8`；
  - bucket `p50=0.33x p95=0.34x`。
- 判定：CTA 可以作为 report-only allowlist bucket policy，但不应全局进入
  `frontier_mode=auto`。

## Runtime Bucket Allowlist Evidence

- v3.14 follow-up：
  [Metal GAC v3.14 Runtime Bucket Allowlist Plan](CPIM_METAL_METAL_GAC_V314_RUNTIME_BUCKET_ALLOWLIST_PLAN_2026_05_04.md)。
- `--policy_mode=bh_cta_allowlist` 已作为 default-off runtime policy 接入：
  - 命中 `BH-4-4 cons=128-511 dom<17 bitw<2` 时启用 CTA hybrid8；
  - 未命中时保留用户请求路径；
  - `frontier_mode=auto` 不读取该 policy。
- CSV / analyzer 新增 runtime policy summary：
  - `policy_mode`
  - `policy_selected`
  - `policy_reason`
  - `policy_bucket`
- TIER2：
  - 380/383 OK，3 个 ERROR 均为历史 `unsupported_non_binary_extension`；
  - selected rows：20；
  - selected inputs：4/76；
  - selected bucket：`BH-4-4 cons=128-511 dom<17 bitw<2`；
  - selected-vs-shared+flags：`p50=0.62x p95=0.68x`；
  - better：4/4；
  - regressions over 1.05：0。
- 判定：v3.14 证明 CTA hybrid8 可作为 BH-like runtime allowlist probe；
  仍不支持全局 CTA promote。默认性能线继续转向 dispatch 成本或 Batch/SAC。

## Dispatch Timing Split Evidence

- v3.15 follow-up：
  [Metal GAC v3.15 Dispatch Timing Split Plan](CPIM_METAL_METAL_GAC_V315_DISPATCH_TIMING_SPLIT_PLAN_2026_05_04.md)。
- 新增 additive CSV/stats 字段：
  - `dispatch_encode_ms`
  - `dispatch_wait_ms`
  - `dispatch_non_kernel_ms`
- 不改变 `dispatch_ms`、`solve_ms` 或任何 runtime policy。
- TIER2 baseline：
  - 380/383 OK，3 个 ERROR 均为历史 `unsupported_non_binary_extension`；
  - `solve_ms p50=0.236 p95=5.403`；
  - `encode=0.013ms`；
  - `wait=0.773ms`；
  - `kernel=0.105ms`；
  - `non_kernel=0.668ms`；
  - `encode_per_dispatch=0.0039ms`；
  - `non_kernel_per_dispatch=0.2035ms`；
  - `non_kernel_share=0.86`。
- 判定：当前 GAC-only baseline 不是 CPU encode 主导，而是短 kernel 周围的
  command buffer wait / non-kernel 固定成本主导。继续单实例 CTA/bulk kernel
  变体的边际收益有限；下一步应优先评估 Batch/SAC 多任务吞吐、command-buffer
  fusion 或其它能摊薄 dispatch 的方案。

## Batch/SAC Batch Probe Evidence

- v3.16 follow-up：
  [Metal Batch/SAC v3.16 Batch Probe Plan](CPIM_METAL_METAL_BATCH_SAC_V316_BATCH_PROBE_PLAN_2026_05_04.md)。
- 新增 default-off `benchmark_metal_sac`：
  - stable Metal GAC 先得到 AC snapshot；
  - snapshot 中 remaining `(var,value)` 生成 singleton probe worlds；
  - `neighbor` 模式只激活 singleton var subscriptions；
  - `UNKNOWN` 保守保留，不产生删值；
  - CSV 与 Metal GAC 分离。
- Correctness smoke：
  - `gac_bitwords2.xml`：2 probes，2 OK，0 DWO，0 UNKNOWN；
  - CPU/Metal probe status verify 通过。
- BH throughput smoke：
  - `BlackHole-4-4-e-0_ext.xml`：384 probes/run，3 measured runs；
  - CPU/Metal probe status verify 通过；
  - `dispatch_per_probe=0.0182292`；
  - `non_kernel_per_probe≈0.0037-0.0039ms`；
  - `probes_per_sec≈195k-202k`。
- TIER2：
  - 380/383 OK；
  - 3 个 ERROR 均为历史 `unsupported_non_binary_extension`；
  - supported rows `verify_mismatches=0`、`UNKNOWN=0`；
  - total measured probes：163,605；
  - `dispatch_per_probe avg=0.0527 p50=0.0182 p95=0.1402`；
  - `non_kernel_per_probe avg=0.014634ms p50=0.005242ms p95=0.023366ms`；
  - `probes_per_sec avg≈144k p50≈146k p95≈255k`。
- 判定：大批量 SAC probe 能明显摊薄 v3.15 的 command buffer non-kernel 成本；
  小 fixture 仍太小，只能作为 correctness smoke。v3.16 保持 benchmark-only；
  若继续推进，则进入 host-side NSACQ、queue budget 和 DWO writeback。

## Host-Side NSACQ Evidence

- v3.17 follow-up：
  [Metal SAC v3.17 Host NSACQ Plan](CPIM_METAL_METAL_SAC_V317_HOST_NSACQ_PLAN_2026_05_09.md)。
- 新增 default-off `--sac_mode=nsacq|sacq_adj|sacq_full`：
  - host 侧维护 remaining-value queue；
  - Metal batch probe 可读取 allowed-constraints mask；
  - DWO probe 经 CPU reference guard 确认后才写回 host snapshot；
  - 每批确认删除后重新运行 stable Metal GAC；
  - `UNKNOWN` 和 rejected DWO 都不产生删值。
- Correctness/writeback smoke：
  - `queens-4_ext.xml`：24 probes，16 OK，8 DWO，0 UNKNOWN；
  - 8 values written back；
  - 1 次 post-delete stable Metal GAC；
  - 0 rejected DWO。
- TIER2：
  - 228/231 OK；
  - 3 个 ERROR 均为历史 `unsupported_non_binary_extension`；
  - total measured probes：278,172；
  - `UNKNOWN=0`；
  - confirmed DWO writeback：12,094；
  - rejected unconfirmed Metal DWO：4,970；
  - `dispatch_per_probe avg=0.0613 p50=0.0219 p95=0.1117`。
- 判定：v3.17 完成安全 benchmark-only NSACQ 回路，但 raw Metal DWO status
  不能 promote；CPU-confirmed DWO guard 是必要条件。下一步若继续 SAC，
  应优先评估 deterministic/double-buffer probe，或把 guard 成本纳入 promote gate。

### Shared Gates

- Promote gate：
  - `CPU/Metal verification mismatch = 0`；
  - `solve_ms p95 <= shared+flags baseline * 1.05`；
  - 相对 v3.5 `cta_worklist` 的 `solve_ms p50/p95` 至少一项改善；
  - `host_round_count p50` 或 `metal_cpu_solve_ratio p50/p95` 至少一项下降；
  - `cta_overflow_count p95 = 0`，或 overflow 行全部可解释且不影响 verify。
- Reject gate：
  - 任一分支 `solve_ms p95` 比 `shared+flags` 慢超过 5%；
  - 出现 CPU/Metal verification mismatch；
  - `cta_overflow_count p95 > 0` 且无法解释；
  - 新增 stats 不能说明瓶颈是否移动。
- Exit rule：
  - 若 `primal_edge_cut_owner`、`vebo_weighted_owner`、
    `bulk_sync_deletion_mask`、`dirty_var_pull` 和 `dirty_pull_hybrid` 都不能降低
    `host_round_count` 或通过 shared+flags / best worklist gate，v3 结论转向
    allowlist bucket policy、Batch/SAC 多任务吞吐或基础 dispatch 成本优化，不再
    硬推单实例全局 CTA。

## Validation

- 文档与 DocOps：
  - `git diff --check`
  - `python3 codex-docops-logic/scripts/dol.py lint --soft`
- 静态：
  - `python3 -m py_compile codex-docops-logic/scripts/dol.py`
- 后续每个分支复用：
  - `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=cta --runs=3 --warmup=1 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=<mode> --cpu-timing --timeout=60 --csv=out/metal_gac_v36_<branch>_smoke.csv`
  - `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=cta --runs=5 --warmup=2 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=<mode> --cpu-timing --timeout=300 --csv=out/metal_gac_v36_<branch>_tier2.csv --quiet`

## Rollback

- `owner_map_static` 是 default-off；不传 `--cta_owner_mode=static_edge_cut`
  即回到旧 `modulo` owner。
- 所有后续分支必须显式 flag/default-off，并能回退到 v2/v3 stable fallback。
- 任一 rejected 分支保留 evidence CSV 和 changelog 结论，不从历史文档删除。

## Links

- changelog:
  [Metal GAC v3.6 CTA Owner Partition Exploration Changelog](METAL_GAC_V36_CTA_OWNER_PARTITION_EXPLORATION_CHANGELOG_2026_05_03.md)
- roadmap:
  [Metal GAC Long Term Optimization](../METAL_GAC_LONG_TERM_OPTIMIZATION.md)
- references:
  [Apple Indirect Command Encoding](https://developer.apple.com/documentation/metal/indirect-command-encoding),
  [Apple MTLComputeCommandEncoder](https://developer.apple.com/documentation/Metal/MTLComputeCommandEncoder),
  [Gunrock frontier/load balancing](https://arxiv.org/abs/1501.05387),
  [Merrill GPU graph traversal](https://research.nvidia.com/sites/default/files/pubs/2012-02_Scalable-GPU-Graph/ppo213s-merrill.pdf),
  [Dynamic load balancing on GPUs](https://diglib.eg.org/items/80970ff5-d565-4a2e-88cc-cc9701e4ad9e),
  [Whippletree dynamic GPU workloads](https://arbook.icg.tugraz.at/schmalstieg/Schmalstieg_286.pdf)
