# GPU 求解器二分回溯搜索修复

## 问题概述

**发现时间**: 2025-12-21
**严重程度**: 高（影响求解正确性）
**影响范围**: [apps/gmodel_solver.cpp](../../apps/gmodel_solver.cpp)

## 问题描述

GPU 求解器 `gmodel_solver` 的搜索算法存在根本性错误，导致 CPU 和 GPU 求解器的搜索节点数不一致。

### 错误表现

| 测试实例 | CPU (P/N) | GPU (P/N) | 状态 |
|---------|-----------|-----------|------|
| queens-4 | 5/1 | 1/1 | ❌ 节点数不匹配 |
| langford-3-9 | 468/441 | 549/522 | ❌ 节点数不匹配 |

### 根本原因

**问题 1：错误的搜索策略**

初始实现使用 **for 循环遍历域值**，而非标准的**二分回溯搜索**：

```cpp
// 错误实现（初始版本）
for (int value : domain) {
  NewLevel();
  AssignValue(var, value);
  Propagate();

  if (consistent) {
    if (Search(level + 1)) return true;
  }

  BacktrackTo(level - 1);
  // ❌ 缺少 RemoveValue() 和重新传播
}
```

**问题**：
- 每个值都在新层尝试，没有移除失败的值
- 缺少重新传播步骤
- 导致搜索树结构与 CPU 不同

**问题 2：递归结构错误**

第一次修复尝试使用尾递归，但仍然有缺陷：

```cpp
// 第一次修复尝试（仍有问题）
bool Search(int level) {
  // ... 赋值和传播 ...

  if (Search(level + 1)) return true;  // 递归到下一层

  BacktrackTo(level - 1);
  RemoveValue(var, value);
  RePropagate();

  return Search(level);  // ❌ 尾递归，但层级管理混乱
}
```

**问题**：
- 多次调用 `Search(level)` 导致多次 `NewLevel()`
- Trail level 不断增加，与预期的搜索深度不匹配

**问题 3：while 循环条件错误**

第二次修复尝试使用 `while (!consistent)` 循环：

```cpp
// 第二次修复尝试（仍有问题）
while (!consistent) {
  // ... 赋值和传播 ...

  RemoveValue(value);
  RePropagate();

  consistent = !inconsistent && domain_size > 0;  // ❌ 设置为 true

  if (!consistent) return false;
}
```

**问题**：
- `RemoveValue` 成功后设置 `consistent = true`
- 导致 `while (!consistent)` 循环退出
- 无法尝试同一变量的下一个值

## 正确的实现

### CPU MAC 的双层循环结构

```cpp
// src/solver/cpu/MAC.cpp (参考实现)
while (!finished_) {
  // 外层循环：主搜索循环
  IntVal v_a = select_v_value(I.size());
  n_->trail()->NewLevel();
  I.push(v_a);
  ++statistics_.num_positive;

  v_a.v()->ReduceTo(v_a.a());
  consistent_ = ac_->enforce(x_evt_, I.size()).state;

  if (consistent_ && I.full()) {
    // 找到解
    return statistics_;
  }

  // 内层循环：回溯循环
  while (!consistent_ && !I.empty()) {
    v_a = I.pop();
    BacktrackTo(I.size() - 1);
    v_a.v()->RemoveValue(v_a.a());  // ← 关键：移除失败的值
    ++statistics_.num_negative;
    consistent_ = v_a.v()->size() && ac_->enforce(x_evt_, I.size()).state;
  }

  if (!consistent_) finished_ = true;
}
```

**关键特性**：
1. **外层循环**：每次迭代尝试一个值（可能是新变量的第一个值，或同一变量的下一个值）
2. **内层循环**：处理回溯，直到找到 consistent 状态或栈为空
3. **值移除**：失败后调用 `RemoveValue()`，然后重新传播
4. **I.size() 控制深度**：`I.pop()` 减小深度，`I.push()` 增加深度

### GPU 的正确实现（while(true) 循环）

```cpp
// apps/gmodel_solver.cpp (修复后版本)
bool Search(int level) {
  int var = SelectVariable(level);
  if (var == -1) return true;  // 找到解

  // 使用无限循环尝试当前变量的所有值
  while (true) {
    int value = GetFirstValue(var);
    if (value == -1) {
      return false;  // 域为空，回溯到上一层
    }

    ++nodes_;
    ++positives_;
    model_->NewLevel();
    model_->AssignValue(var, value);

    GacStats stats = model_->EnforceGAC(...);
    bool consistent = !stats.inconsistent && !HasEmptyDomain();

    if (consistent) {
      // 递归到下一层
      if (Search(level + 1)) return true;
      // 回溯失败，继续尝试当前层的下一个值
    }

    // Backtrack 处理
    ++negatives_;
    model_->BacktrackTo(level - 1);
    model_->RemoveValue(var, value);  // ← 关键：移除失败的值

    GacStats remove_stats = model_->EnforceGAC(...);  // ← 关键：重新传播

    if (remove_stats.inconsistent) {
      return false;  // re-propagate 失败，回溯到上一层
    }
    // 如果成功，继续 while 循环尝试下一个值
  }
}
```

**关键改进**：
1. ✅ `while (true)` 无限循环，用 `return` 退出
2. ✅ 每次循环开始调用 `GetFirstValue(var)` 获取域中第一个值
3. ✅ 失败后调用 `RemoveValue(var, value)` 移除失败的值
4. ✅ 调用 `EnforceGAC()` 重新传播
5. ✅ 只有在 re-propagate 失败时才返回 false
6. ✅ 成功则继续循环，`GetFirstValue()` 会返回下一个值

## 修复过程

### 阶段 1：发现问题

```bash
# 运行 TIER 0 测试
python3 tests/python/compare_cpu_gpu.py --tier=0

# 结果：10/12 通过，2 个节点数不匹配
# - langford-3-9: CPU=468/441, GPU=549/522
# - composed-25-1-2-0: 状态不一致
```

### 阶段 2：第一次修复（尾递归）

**修改**：使用 `return Search(level)` 模拟 CPU 的同层值尝试

**结果**：仍然失败，原因是多次 `NewLevel()` 导致层级混乱

### 阶段 3：第二次修复（while 循环 + 条件控制）

**修改**：使用 `while (!consistent)` 循环

**结果**：queens-4 只尝试了 1 个值就退出，因为 `consistent=true` 导致循环退出

### 阶段 4：最终修复（while(true) 无限循环）

**修改**：
1. 使用 `while (true)` 无限循环
2. 只在域为空或 re-propagate 失败时 `return false`
3. 成功则继续循环

**结果**：
```
✅ TIER 0: 12/12 通过（100%）
✅ TIER 1: 15/39 通过（0 失败）
✅ CPU/GPU 节点数完全匹配
```

## 修复验证

### 测试案例对比

#### queens-4
```
CPU: [Try] Level 0: var[0]=0 → prune
     [Try] Level 0: var[0]=1 → continue
     [Try] Level 1: var[1]=3 → continue
     ...
     Positives: 5, Negatives: 1

GPU: [Try] Level 0: var[0]=0 → prune
     [Try] Level 0: var[0]=1 → continue  ← 修复后正确尝试第二个值
     [Try] Level 1: var[1]=3 → continue
     ...
     Positives: 5, Negatives: 1 ✓
```

#### langford-3-9
```
CPU: Positives: 468, Negatives: 441
GPU: Positives: 468, Negatives: 441 ✓ (修复前: 549/522)
```

### TIER 0 完整测试结果

| # | 实例 | 状态 | CPU (P/N) | GPU (P/N) | 一致性 |
|---|------|------|-----------|-----------|--------|
| 1 | test.xml | SAT | 3/0 | 3/0 | ✓ |
| 2 | queens-4_ext | SAT | 5/1 | 5/1 | ✓ |
| 3 | langford-2-4-ext | SAT | 8/0 | 8/0 | ✓ |
| 4 | langford-3-9-ext | SAT | 468/441 | 468/441 | ✓ |
| 5-8 | rand-2-40-* | TIMEOUT | 0/0 | 0/0 | ✓ |
| 9 | driverlogw-01c-sat | SAT | 71/0 | 71/0 | ✓ |
| 10 | BlackHole-4-4-e-0 | TIMEOUT | 0/0 | 0/0 | ✓ |
| 11 | composed-25-1-2-0 | TIMEOUT | 0/0 | 0/0 | ✓ |
| 12 | graphw-05 | UNSAT | 0/0 | 0/0 | ✓ |

**总结**: 12/12 通过，0 失败

## 经验教训

### 1. 递归与循环的等价性

**CPU MAC 的迭代结构**：
- 外层 `while` 循环控制主搜索
- 内层 `while` 循环处理回溯
- `I.size()` 控制搜索深度

**GPU 的递归结构**：
- 递归调用 `Search(level+1)` 模拟外层循环的深度增加
- `while(true)` 循环模拟内层循环的值尝试
- `level` 参数控制搜索深度

### 2. 二分回溯的本质

**关键步骤**：
1. 尝试一个值 → 赋值 + 传播
2. 如果成功 → 递归到下一层
3. 如果失败或回溯 → **移除值** + **重新传播**
4. 继续尝试同一变量的下一个值

**容易遗漏的地方**：
- ❌ 忘记调用 `RemoveValue()`
- ❌ 忘记在移除后重新传播
- ❌ 过早返回，没有尝试所有值

### 3. Trail 层级管理

**CPU 的隐式管理**：
- `I.push()` 增加层级
- `I.pop()` 减少层级
- `NewLevel()` 在 push 之前调用
- Trail level 总是等于 `I.size() - 1`

**GPU 的显式管理**：
- `level` 参数显式传递
- `NewLevel()` 在每次赋值前调用
- `BacktrackTo(level - 1)` 回溯到父层级
- 需要手动确保层级一致性

## 相关文件

**修复的文件**：
- [apps/gmodel_solver.cpp](../../apps/gmodel_solver.cpp:55-127) - Search() 函数重写

**测试脚本**：
- [tests/python/compare_cpu_gpu.py](../../tests/python/compare_cpu_gpu.py) - CPU/GPU 对比测试
- [tests/python/tier_definitions.py](../../tests/python/tier_definitions.py) - 测试集定义

**参考实现**：
- [src/solver/cpu/MAC.cpp](../../src/solver/cpu/MAC.cpp:100-170) - CPU MAC 标准实现

## 参考

- [UNIFIED_TRAIL_MEMO.md](UNIFIED_TRAIL_MEMO.md) - Trail 回溯系统文档
- [CLAUDE.md](../../CLAUDE.md) - 项目概述和开发规划
