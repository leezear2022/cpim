# 归档文档

此目录包含历史或过期的文档，保留供参考。

## 目录结构

```
archive/
├── gpu_cmodel/           # CModel（已被 GModel 取代）
├── batch_ac_versions/    # Batch AC 历史版本
└── obsolete/             # 其他过期文档
```

## gpu_cmodel/

CModel 是早期的 GPU 约束模型实现，已被更优化的 GModel 取代。

| 文档 | 说明 |
|------|------|
| GPU_CMODEL_PLAN.md | 初始设计 |
| GPU_CMODEL_OPTIMIZATION_PLAN.md | 优化计划 |
| GPU_CMODEL_OPT_PLAN.md | 简化优化 |
| GPU_CMODEL_REFACTOR_PLAN.md | 重构计划 |

当前实现：[GModel 架构](../gpu/GMODEL_ARCHITECTURE.md)

## batch_ac_versions/

Batch AC 的迭代设计版本。

| 文档 | 说明 |
|------|------|
| BATCH_AC_GPU_DESIGN.md | 初始设计 |
| BATCH_AC_GPU_COMPREHENSIVE_DESIGN.md | V1 综合设计 |
| BATCH_AC_GPU_BATCH2_BATCH3_DESIGN.md | Batch2/3 设计 |
| ... | 其他历史版本 |

当前实现：[Batch AC 设计 V2](../gpu/BATCH_AC_DESIGN.md)

## obsolete/

其他过期文档。

| 文档 | 说明 |
|------|------|
| cp_schemes/ | 早期 CP 方案探索 |
| GPU_*_PLAN.md | 未实施的 GPU 优化计划 |

---

## 已删除文档

以下文档已从代码库中删除（2025-01）：

| 文档 | 删除原因 |
|------|----------|
| cusac_architecture_comparison.md | cuSAC 已废弃 |
| cusac_optimization_plan.md | cuSAC 已废弃 |
| SOLVER_TEST_REPORT.md | 过期测试报告 |
| MODERNIZATION_PLAN.md | 被 V2 取代 |
| cpim_modernization_plan.md | 与 V2 重复 |
| MODERNIZATION_MEMO.md | 被 V2 取代 |
| CPIM_COMPREHENSIVE_REFACTORING_GUIDE.md | 未实施 |
