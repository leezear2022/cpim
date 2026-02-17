# CPIM 文档导航

> CPIM - CUDA 加速的约束满足问题求解器

## 快速开始

- [项目主 README](../README.md) - 构建与运行
- [测试指南](guides/TESTING_GUIDE.md) - 分层测试体系
- [应用程序参考](guides/APPS_REFERENCE.md) - 所有可执行程序
- [SAC preprocess 评测教程](guides/SAC_PREPROCESS_GUIDE.md) - SAC/MSAC/NSAC 评测入口

## 核心文档

| 文档 | 说明 |
|------|------|
| [CLAUDE.md](../CLAUDE.md) | 项目总览、架构、开发计划 |
| [系统架构](architecture/ARCHITECTURE.md) | 多级域网络、Trail 系统 |

## 算法文档

| 类别 | 文档 | 说明 |
|------|------|------|
| 一致性 | [AC算法层次](algorithms/consistency/AC_HIERARCHY.md) | AC3bit, RPC3, lMaxRPC |
| 一致性 | [SAC算法](algorithms/consistency/SAC_ALGORITHMS.md) | SAC1, SAC3, MSAC 设计 |
| 搜索 | [MAC搜索](algorithms/search/MAC_SEARCH.md) | 回溯搜索与传播 |
| 启发式 | [变量选择](algorithms/heuristics/VARIABLE_HEURISTICS.md) | MinDomain, DOM/DEG, DOM/DDEG |

## GPU 实现

| 文档 | 说明 |
|------|------|
| [GModel架构](gpu/GMODEL_ARCHITECTURE.md) | GPU 模型核心设计 |
| [Batch AC设计](gpu/BATCH_AC_DESIGN.md) | Batch2/Batch3 并行探测 |
| [Batch AC概念](gpu/BATCH_AC_CONCEPTS.md) | Batch AC 理论基础 |
| [GAC 方案对比](gpu/GPU_GAC_SCHEME_COMPARISON.md) | GPU GAC 实现策略 |

## 开发资源

| 目录 | 内容 |
|------|------|
| [planning/](planning/) | 开发规划（活跃） |
| [implementation/](implementation/) | 实现详情 |
| [bugfixes/](bugfixes/) | Bug 修复记录 |
| [performance/](performance/) | 性能分析 |
| [testing/](testing/) | 测试报告 |
| [archive/](archive/) | 归档文档 |

### 关键规划文档

- [现代化计划 V2](planning/MODERNIZATION_PLAN_V2.md) - 总体开发规划
- [**SAC-GPU 设计**](planning/SACGPU_DESIGN.md) - bitGEMM 化、SACq/NSACq、长尾控制
- [SACGPU 新方案独立裁决（2026-02）](planning/SACGPU_NEW_CODEX_REVIEW_2026_02.md) - FQ-PT-CID-CTA-MB 的裁决结论、分阶段落地门槛与回退条件
- [Batch-3A Walkthrough（2026-02）](planning/BATCH3A_WALKTHROUGH_2026_02.md) - 从核心思想到代码落点的阅读路径（Manager / Kernel / SAC3 接入）
- [Batch-3A 性能回退复盘（2026-01）](planning/BATCH3A_POSTMORTEM_2026_01.md) - 为什么 Batch-3A 在 Orin 上结构性慢于 Stage2，以及是否值得继续投入/如何止损
- [Batch-3A Dynamic Submission 队列版（2026-02）](planning/BATCH3A_DYNAMIC_SUBMISSION_QUEUE_DESIGN.md) - “结尾提交”worklist 替代“开头扫全约束”，显式利用稀疏性（PSTRds/PCTds 风格）
- [dGPU 内存规划（RTX 4060/4090）](planning/DGPU_MEMORY_PLAN.md) - 从 Jetson UMA 迁移到 PCIe dGPU 的 GPU-resident 数据流与 memcpy 规避/隐藏策略（可消融/可回退）
- [SACGPU 下一步执行清单（10 分钟超时版）](planning/SACGPU_NEXT_ACTIONS_10MIN_TIMEOUT.md) - 可执行命令清单
- [Phase 1.5 启发式](planning/PHASE_1.5_HEURISTICS_DESIGN.md) - 变量选择启发式
- [Propagator 框架](planning/PROPAGATOR_FRAMEWORK_DESIGN.md) - Phase 2 重构计划
- [自适应引擎](planning/ADAPTIVE_ENGINE_DESIGN.md) - CPU/GPU 切换策略

---

## 文档规范

### 命名规则

| 类型 | 格式 | 示例 |
|------|------|------|
| 设计文档 | `<TOPIC>_DESIGN.md` | `BATCH_AC_DESIGN.md` |
| 架构文档 | `<TOPIC>_ARCHITECTURE.md` | `GMODEL_ARCHITECTURE.md` |
| 指南文档 | `<TOPIC>_GUIDE.md` | `TESTING_GUIDE.md` |
| 算法文档 | `<TOPIC>_ALGORITHMS.md` | `SAC_ALGORITHMS.md` |
| 备忘录 | `<TOPIC>_MEMO.md` | `OPTIMIZATION_MEMO.md` |

### 状态标记

在文档开头使用以下标记：

```markdown
---
status: active | completed | archived
---
```

### 目录规则

- **algorithms/**: 算法理论与设计
- **gpu/**: GPU 实现相关
- **planning/**: 仅放活跃的开发计划
- **archive/**: 过期或历史文档

### 新文档流程

1. 根据内容类型选择目录
2. 使用规范命名格式
3. 在本文档添加导航链接
4. 如适用，在 CLAUDE.md 添加引用
