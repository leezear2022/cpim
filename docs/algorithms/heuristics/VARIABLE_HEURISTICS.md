# 变量选择启发式

---
status: active
---

本文档描述 CPIM 中的变量选择启发式。

## 概述

变量选择启发式决定搜索中下一个赋值的变量。好的启发式可以显著减少搜索节点数。

## 实现

- 头文件：[include/solver/common/variable_selector.h](../../../include/solver/common/variable_selector.h)
- 实现：[src/solver/common/variable_selector.cpp](../../../src/solver/common/variable_selector.cpp)

## 支持的启发式

### MinDomain（最小域优先）

**默认启发式**

选择当前域大小最小的变量。

```
选择 argmin_{Xi 未赋值} |Di|
```

特点：
- 简单高效
- Fail-first 原则：可能失败的变量优先尝试

### DOM/DEG（域大小/度数）

选择 (域大小 / 静态度数) 最小的变量。

```
选择 argmin_{Xi 未赋值} |Di| / degree(Xi)
```

其中 `degree(Xi)` = Xi 参与的约束数

特点：
- 平衡域大小和约束密度
- 适合约束密集问题

### DOM/DDEG（域大小/动态度数）

选择 (域大小 / 动态度数) 最小的变量。

```
选择 argmin_{Xi 未赋值} |Di| / ddeg(Xi)
```

其中 `ddeg(Xi)` = Xi 与未赋值变量共享的约束数

特点：
- 考虑当前搜索状态
- 更精确但开销更大

## 性能对比

### 启发式效果

| 问题类型 | 最佳启发式 |
|----------|------------|
| 均匀约束 | MinDomain |
| 约束密集 | DOM/DEG 或 DOM/DDEG |
| 不规则结构 | DOM/DDEG |

### 典型结果

queens-4 示例（所有启发式相同，因为变量度数相同）：
- MinDomain: P=5, N=1
- DOM/DEG: P=5, N=1
- DOM/DDEG: P=5, N=1

## 命令行使用

```bash
# CPU 求解器
./cpim_test_parser --bench_path=<file> --heuristic=min_domain
./cpim_test_parser --bench_path=<file> --heuristic=dom_deg
./cpim_test_parser --bench_path=<file> --heuristic=dom_ddeg

# GPU 求解器
./gmodel_solver --input=<file> --heuristic=min_domain
./gmodel_solver --input=<file> --heuristic=dom_deg
./gmodel_solver --input=<file> --heuristic=dom_ddeg
```

## 代码结构

```cpp
// 抽象接口
class VariableSelector {
public:
    virtual int SelectVariable() = 0;
};

// 具体实现
class MinDomainSelector : public VariableSelector {...};
class DomDegSelector : public VariableSelector {...};
class DomDDegSelector : public VariableSelector {...};

// 工厂函数
VariableSelector* CreateSelector(const std::string& type, Network* net);
```

## 扩展计划

### DOM/WDEG（加权度数）

动态调整约束权重：

```
weight(c) += 1  // 当约束 c 导致 DWO 时
选择 argmin |Di| / Σ weight(c)，c ∈ constraints(Xi)
```

### Impact-Based Search

基于值选择对域的影响：

```
impact(Xi, a) = 1 - Π (|Dj'| / |Dj|)
选择最大 impact 的变量
```

## 相关文档

- [MAC 搜索](../search/MAC_SEARCH.md) - 使用启发式的搜索框架
- [Phase 1.5 设计](../../planning/PHASE_1.5_HEURISTICS_DESIGN.md) - 启发式实现计划
- [基准测试脚本](../../guides/APPS_REFERENCE.md) - benchmark_heuristics.py
