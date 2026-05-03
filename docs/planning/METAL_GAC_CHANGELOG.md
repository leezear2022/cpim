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
| [DocOps microdocs changelog](metal_gac/CPIM_METAL_DOCOPS_MICRODOCS_CHANGELOG_2026_05_03.md) | active | DocOps Logic 生成独立小计划/小 changelog 的通用能力 |

## 小计划

| 文档 | 状态 | 摘要 |
|------|------|------|
| [v2 guard / v3 entry plan](metal_gac/METAL_GAC_V2_GUARD_V3_ENTRY_PLAN_2026_05_03.md) | active | v2 regression guard、baseline evidence、v3 recommender 入口 |
| [DocOps microdocs plan](metal_gac/CPIM_METAL_DOCOPS_MICRODOCS_PLAN_2026_05_03.md) | active | 在 DocOps Logic 插件中加入独立小计划/小 changelog 生成能力 |
