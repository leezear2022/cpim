# MAC 搜索算法

---
status: active
---

本文档描述 CPIM 中的 MAC (Maintaining Arc Consistency) 搜索算法。

## 概述

MAC = 回溯搜索 + AC 传播

在每次决策后维护弧一致性，通过强剪枝减少搜索空间。

## 算法流程

```
MAC_Search():
    if 所有变量已赋值:
        return SAT

    选择未赋值变量 Xi（变量启发式）
    for 每个值 a ∈ Di:
        赋值 Xi = a
        执行 AC 传播
        if 无 DWO:
            result = MAC_Search()
            if result == SAT:
                return SAT
        回溯

    return UNSAT
```

## 实现

### CPU 实现

- 文件：[src/solver/cpu/MAC.cpp](../../../src/solver/cpu/MAC.cpp)
- 类：`cpim::MAC`

关键方法：
```cpp
class MAC : public Solver {
public:
    bool Solve();              // 主搜索入口
    int SelectVariable();      // 变量选择
    void AssignValue(int, int); // 赋值
    void BacktrackTo(int);     // 回溯
};
```

### GPU 实现

- 文件：[apps/gmodel_solver.cpp](../../../apps/gmodel_solver.cpp)
- 使用 GModel 进行 GPU 传播

## 回溯系统

### UnifiedTrail

统一的 CPU/GPU 回溯系统：

- 文件：[src/base/unified_trail.cpp](../../../src/base/unified_trail.cpp)
- 类：`cpim::UnifiedTrail`

功能：
1. **级别管理**：`NewLevel()`, `BacktrackTo(level)`
2. **域变化记录**：记录每次域修改
3. **快照恢复**：高效恢复到任意决策点

## 搜索统计

MAC 搜索返回两个关键指标：

| 指标 | 说明 |
|------|------|
| Positives (P) | 成功赋值数（不含回溯） |
| Negatives (N) | 失败赋值数（导致 DWO 的尝试） |

验证标准：
- CPU 和 GPU 的 P/N 必须完全匹配

## 变量选择

详见 [变量启发式](../heuristics/VARIABLE_HEURISTICS.md)

支持的启发式：
- MinDomain（最小域优先）
- DOM/DEG（域大小/度数）
- DOM/DDEG（域大小/动态度数）

## 值选择

当前实现：按域中值的顺序尝试

可扩展：
- 最小冲突
- 随机选择
- Impact-based

## 命令行使用

```bash
# CPU MAC 搜索
./cpim_test_parser --bench_path=<file>

# GPU MAC 搜索
./gmodel_solver --input=<file>

# 验证
./verify_search --input=<file>
```

## 相关文档

- [AC 层次](../consistency/AC_HIERARCHY.md) - 传播算法
- [变量启发式](../heuristics/VARIABLE_HEURISTICS.md) - 变量选择
- [UnifiedTrail](../../bugfixes/UNIFIED_TRAIL_MEMO.md) - 回溯系统
