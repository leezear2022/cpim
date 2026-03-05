# SAC 算法

---
status: active
---

本文档描述 CPIM 中的单例弧一致性 (Singleton Arc Consistency) 算法。

## 概述

SAC 是比 AC 更强的一致性级别。对于变量 Xi 和值 a ∈ Di：

> 如果将 Xi 赋值为 a 后执行 AC，导致 Domain Wipe-Out (DWO)，则从 Di 中删除 a。

## 算法变体

### SAC1

**基础 SAC 算法**

```
对于每个变量 Xi:
    对于每个值 a ∈ Di:
        将 Xi 赋值为 a
        执行 AC
        如果 DWO:
            从 Di 中删除 a
        回溯
重复直到不动点
```

特点：
- 简单实现
- 时间复杂度：O(en²d⁴)

### SAC3

**优化的 SAC 算法**

关键优化：
1. 增量 AC：利用上一轮的支持信息
2. 跳过策略：跳过已知 SAC 一致的值
3. 提前终止：检测不动点

### MSAC

**维护 SAC (Maintaining SAC)**

在搜索过程中维护 SAC：

```
MAC 搜索中:
    选择变量 Xi
    对于每个值 a ∈ Di:
        执行 SAC 探测
        如果 SAC 一致:
            赋值 Xi = a
            继续搜索
```

特点：
- 搜索节点数 < MAC（更强剪枝）
- 每节点开销 > MAC（SAC 探测）

## GPU 实现

### Batch2 Persistent

**批量 SAC 探测**

- 文件：[src/solver/gpu/batch_probe_manager.cu](../../../src/solver/gpu/batch_probe_manager.cu)
- 类：`Batch2PersistentManager`

核心思想：
1. 并行探测所有 (变量, 值) 对
2. GPU 执行 GAC 传播
3. 收集 DWO 结果

### 吞吐量

| 平台 | 探测吞吐量 |
|------|------------|
| Tegra (Jetson) | ~59,000 probes/sec |
| RTX 4090 (估计) | ~500,000-1,000,000 probes/sec |

## SAC 价值分析

### 根节点 vs 搜索中

| 时机 | 典型删除数 |
|------|------------|
| 根节点 | 较少（域已精简） |
| 搜索中 | 较多（约束更紧） |

示例（rand-2-40-180，tightness=0.9）：
- 根节点 SAC：7 删除 / 7200 值
- 搜索中 SAC（1 次赋值后）：3992 删除

### SAC-GPU 策略

> GPU 处理计算密集型的剪枝，减少逻辑密集型的搜索

```
剪枝强度 ↑ → 搜索节点 ↓
GPU 探测吞吐量 ↑ → SAC 开销 ↓
```

## 命令行使用

```bash
# SAC 基准测试
./sac_benchmark --input=<file> --test_full_sac=true

# MSAC 模拟
./sac_benchmark --input=<file> --test_msac=true --msac_mode=parallel
```

### MSAC 模式

| 模式 | 说明 |
|------|------|
| fast | 仅 GAC 回溯（最快） |
| full | 完整 SAC 收敛（最慢） |
| parallel | GPU 并行探测（推荐） |

## 相关文档

- [AC 层次](AC_HIERARCHY.md) - 弧一致性算法
- [Batch AC 设计](../../gpu/BATCH_AC_DESIGN.md) - GPU 实现详情
- [SAC 实现](../../implementation/SAC3_MSAC_IMPLEMENTATION.md) - 实现细节
