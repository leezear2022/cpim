---
status: active
updated: 2026-05-03
---

# Metal GAC Changelog Index

> 本文件只做 Metal GAC 小计划/小 changelog 的索引。每个小计划、每个小
> changelog 都必须单独成文，放在 `docs/planning/metal_gac/` 下。

## 落账规则

- 大 changelog：`CHANGES_ZH.md`，记录跨模块、可发布、可回顾的摘要。
- 小 changelog：每个版本段一个独立文档，不把多个版本段塞进同一个文档。
- 小计划：每个执行计划一个独立文档，不只写在路线文档或对话里。
- 索引：本文只保留链接、状态和约束，不承载详细计划或详细 changelog。
- 路线文档：`METAL_GAC_LONG_TERM_OPTIMIZATION.md` 只记录长期方向和当前摘要。
- 迁移文档：`METAL_MIGRATION_PLAN.md` 只记录 Metal 后端整体状态。

每次实现或计划调整必须至少更新：
- `CHANGES_ZH.md`
- 本索引
- 一个新的小计划或小 changelog 文档
- 对应路线/迁移文档中的状态摘要或下一步链接

## 小 Changelog

| 文档 | 状态 | 摘要 |
|------|------|------|
| [v1 baseline changelog](metal_gac/METAL_GAC_V1_BASELINE_CHANGELOG_2026_05_02.md) | active | Metal GAC v1.x correctness、runtime、benchmark 与 unsupported 分类 |
| [v2.0-v2.1 changelog](metal_gac/METAL_GAC_V20_V21_CHANGELOG_2026_05_03.md) | active | prepared runner、CSV 口径与 `solve_ms` |
| [v2.2-v2.4 changelog](metal_gac/METAL_GAC_V22_V24_CHANGELOG_2026_05_03.md) | active | worklist、word_parallel、directional bitSup |
| [v2.5-v2.9 changelog](metal_gac/METAL_GAC_V25_V29_CHANGELOG_2026_05_03.md) | active | auto 封版、epoch worklist、blit reset、simdgroup 决策 |
| [v3 policy recommender changelog](metal_gac/METAL_GAC_V3_POLICY_RECOMMENDER_CHANGELOG_2026_05_03.md) | active | evidence recommender、bottleneck shares、simdgroup gate |
| [v3 CPU vs Metal changelog](metal_gac/METAL_GAC_V3_CPU_METAL_COMPARISON_CHANGELOG_2026_05_03.md) | active | CPU timing、Metal/CPU ratio、TIER2 对照结论 |
| [v3.4 CTA-local worklist changelog](metal_gac/CPIM_METAL_METAL_GAC_V3_CTA_WORKLIST_CHANGELOG_2026_05_03.md) | implemented | `cta_worklist` 实验路径、CTA-local queue、多轮 local propagation |
| [v3.5 CTA evidence changelog](metal_gac/METAL_GAC_V35_CTA_EVIDENCE_CHANGELOG_2026_05_03.md) | completed | CTA worklist TIER2 gate 未通过，保持 report-only |
| [v3.6 CTA owner partition changelog](metal_gac/METAL_GAC_V36_CTA_OWNER_PARTITION_EXPLORATION_CHANGELOG_2026_05_03.md) | active | `owner_map_static` 已落地但 TIER2 gate 未通过，继续 report-only |
| [v3.7 seed/overflow changelog](metal_gac/METAL_GAC_V37_SEED_OVERFLOW_CHANGELOG_2026_05_03.md) | active | 拆分 CTA queue/seed/budget overflow，`spill_replay` 仍保持 report-only |
| [v3.8 budget/replay changelog](metal_gac/METAL_GAC_V38_BUDGET_REPLAY_CHANGELOG_2026_05_03.md) | active | `bounded_replay` 与 local budget 评估未通过，下一步转向 `vebo_weighted_owner` |
| [v3.9 VEBO weighted owner changelog](metal_gac/METAL_GAC_V39_VEBO_WEIGHTED_OWNER_CHANGELOG_2026_05_04.md) | active | `vebo_weighted_owner` 改善 CTA p95 但仍保持 report-only |
| [v3.10 bulk sync mask changelog](metal_gac/METAL_GAC_V310_BULK_SYNC_MASK_CHANGELOG_2026_05_04.md) | active | `bulk_sync_mask` 两阶段 deletion mask 路径与大例子 gate |
| [v3.11 dirty var pull changelog](metal_gac/METAL_GAC_V311_DIRTY_VAR_PULL_CHANGELOG_2026_05_04.md) | active | CTA 跨 owner push 改为 dirty-var pull，略优于 v3.9 但仍 report-only |
| [v3.12 dirty pull hybrid changelog](metal_gac/METAL_GAC_V312_DIRTY_PULL_HYBRID_CHANGELOG_2026_05_04.md) | active | dirty pull 按变量订阅度阈值 hybrid，压低 CTA 尾部但仍 report-only |
| [v3.13 bucket policy simulation changelog](metal_gac/METAL_GAC_V313_BUCKET_POLICY_SIMULATION_CHANGELOG_2026_05_04.md) | active | analyzer 模拟 BH-like allowlist bucket policy，不改变 runtime auto |
| [v3.14 runtime bucket allowlist changelog](metal_gac/CPIM_METAL_METAL_GAC_V314_RUNTIME_BUCKET_ALLOWLIST_CHANGELOG_2026_05_04.md) | active | default-off runtime `bh_cta_allowlist`，只在 BH-4-4 安全 bucket 启用 CTA hybrid8 |
| [v3.15 dispatch timing split changelog](metal_gac/CPIM_METAL_METAL_GAC_V315_DISPATCH_TIMING_SPLIT_CHANGELOG_2026_05_04.md) | active | 拆分 encode/wait/kernel/non-kernel 时间，确认 GAC baseline 主要受 command buffer non-kernel 成本限制 |
| [v3.16 Metal Batch/SAC batch probe changelog](metal_gac/CPIM_METAL_METAL_BATCH_SAC_V316_BATCH_PROBE_CHANGELOG_2026_05_04.md) | active | default-off Metal batch singleton probe benchmark，验证 SAC probe dispatch 摊薄信号 |
| [v3.17 Metal SAC host NSACQ changelog](metal_gac/CPIM_METAL_METAL_SAC_V317_HOST_NSACQ_CHANGELOG_2026_05_09.md) | active | default-off host-side NSACQ queue、allowed constraint mask、DWO writeback 与 post-delete GAC |
| [v3.18 Metal SAC DWO forensics changelog](metal_gac/CPIM_METAL_METAL_SAC_V318_DWO_FORENSIC_FUSION_CHANGELOG_2026_05_24.md) | active | DWO forensics clean；bounded command fusion 将 TIER2 command-buffer/probe avg 降到 0.0076 |
| [v3.18 Metal SAC fusion rounds sweep changelog](metal_gac/CPIM_METAL_METAL_SAC_V318_FUSION_ROUNDS_SWEEP_CHANGELOG_2026_05_27.md) | active | `fusion_rounds=2/4/8` sweep，当前保留 `4` 作为 balanced explicit bounded setting |
| [DocOps microdocs changelog](metal_gac/CPIM_METAL_DOCOPS_MICRODOCS_CHANGELOG_2026_05_03.md) | active | DocOps Logic 生成独立小计划/小 changelog 的通用能力 |

## 小计划

| 文档 | 状态 | 摘要 |
|------|------|------|
| [v2 guard / v3 entry plan](metal_gac/METAL_GAC_V2_GUARD_V3_ENTRY_PLAN_2026_05_03.md) | active | v2 regression guard、baseline evidence、v3 recommender 入口 |
| [v3 policy recommender plan](metal_gac/METAL_GAC_V3_POLICY_RECOMMENDER_PLAN_2026_05_03.md) | active | v3 evidence、report-only recommender、simdgroup/threadgroup gate |
| [v3 CPU vs Metal plan](metal_gac/METAL_GAC_V3_CPU_METAL_COMPARISON_PLAN_2026_05_03.md) | active | GAC solve time 的 CPU/Metal 正式对照 |
| [v3.4 CTA-local worklist plan](metal_gac/CPIM_METAL_METAL_GAC_V3_CTA_WORKLIST_PLAN_2026_05_03.md) | implemented | per-CTA queue/stamp/tail、local multi-round kernel、host outer loop |
| [v3.5 CTA evidence plan](metal_gac/METAL_GAC_V35_CTA_EVIDENCE_PLAN_2026_05_03.md) | completed | TIER2 对比 flags/worklist/cta_worklist，结论不进入 auto |
| [v3.6 CTA owner partition plan](metal_gac/METAL_GAC_V36_CTA_OWNER_PARTITION_EXPLORATION_PLAN_2026_05_03.md) | active | v3.5 之后的 owner partition 分叉探索，下一步转向 `vebo_weighted_owner` |
| [v3.7 seed/overflow plan](metal_gac/METAL_GAC_V37_SEED_OVERFLOW_PLAN_2026_05_03.md) | active | 先拆 seed/queue/budget overflow，再决定是否进入 `vebo_weighted_owner` |
| [v3.8 budget/replay plan](metal_gac/METAL_GAC_V38_BUDGET_REPLAY_PLAN_2026_05_03.md) | active | local budget 与 bounded replay 能清零 budget spill，但不能通过 p95/host-round gate |
| [v3.9 VEBO weighted owner plan](metal_gac/METAL_GAC_V39_VEBO_WEIGHTED_OWNER_PLAN_2026_05_04.md) | active | weighted owner load 更均衡，但 p95/host-round gate 仍未通过 |
| [v3.10 bulk sync mask plan](metal_gac/METAL_GAC_V310_BULK_SYNC_MASK_PLAN_2026_05_04.md) | active | default-off bulk-synchronous deletion mask，目标转向大例子 / 传播重例子 |
| [v3.11 dirty var pull plan](metal_gac/METAL_GAC_V311_DIRTY_VAR_PULL_PLAN_2026_05_04.md) | active | default-off dirty-var handoff，替代 CTA cross push 但不进入 auto |
| [v3.12 dirty pull hybrid plan](metal_gac/METAL_GAC_V312_DIRTY_PULL_HYBRID_PLAN_2026_05_04.md) | active | dirty-var pull 增加 degree threshold，保留 BH-like bucket 信号 |
| [v3.13 bucket policy simulation plan](metal_gac/METAL_GAC_V313_BUCKET_POLICY_SIMULATION_PLAN_2026_05_04.md) | active | report-only bucket policy simulation，只选择无 regression 的安全 bucket |
| [v3.14 runtime bucket allowlist plan](metal_gac/CPIM_METAL_METAL_GAC_V314_RUNTIME_BUCKET_ALLOWLIST_PLAN_2026_05_04.md) | active | default-off runtime bucket allowlist，未命中时回退用户请求路径 |
| [v3.15 dispatch timing split plan](metal_gac/CPIM_METAL_METAL_GAC_V315_DISPATCH_TIMING_SPLIT_PLAN_2026_05_04.md) | active | additive timing split，不改变 `solve_ms`，用于判断下一步是否应转向 Batch/SAC 吞吐 |
| [v3.16 Metal Batch/SAC batch probe plan](metal_gac/CPIM_METAL_METAL_BATCH_SAC_V316_BATCH_PROBE_PLAN_2026_05_04.md) | active | Metal-first batch probe benchmark MVP，不接搜索或完整 SAC preprocess |
| [v3.17 Metal SAC host NSACQ plan](metal_gac/CPIM_METAL_METAL_SAC_V317_HOST_NSACQ_PLAN_2026_05_09.md) | active | host-side NSACQ queue、allowed constraint mask、DWO writeback 与 post-delete GAC |
| [v3.18 Metal SAC DWO forensics plan](metal_gac/CPIM_METAL_METAL_SAC_V318_DWO_FORENSIC_FUSION_PLAN_2026_05_24.md) | active | DWO forensics 与 bounded command fusion 已落地，后续 NSACQ policy/SACQ 对照 |
| [v3.18 Metal SAC fusion rounds sweep plan](metal_gac/CPIM_METAL_METAL_SAC_V318_FUSION_ROUNDS_SWEEP_PLAN_2026_05_27.md) | active | bounded fusion 段长扫描，TIER2 后保留 `fusion_rounds=4` 为均衡消融设置 |
| [DocOps microdocs plan](metal_gac/CPIM_METAL_DOCOPS_MICRODOCS_PLAN_2026_05_03.md) | active | 在 DocOps Logic 插件中加入独立小计划/小 changelog 生成能力 |
