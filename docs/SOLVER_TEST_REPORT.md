# CPIM 求解器正确性测试报告

**测试日期**: 2024-12-17
**测试版本**: GPU-Codex 分支

## 测试概述

使用 OR-Tools CP-SAT 作为参考基准，对 CPIM MAC+AC3bit CPU 求解器进行正确性验证。

## BUG 修复记录

### BUG #1: MAC::get_solution() 返回错误解 (已修复)

**发现于**: queens-12 测试
**文件**: `src/MAC.cpp`

**问题**: `get_solution()` 使用 `I[i].a()` 获取解，但 `I[i]` 是第 i 个赋值操作，不是变量 i 的值。

**修复**: 从变量域获取解值 `n_->vars[i]->head(level)`

### BUG #2: UNSAT 情况下错误返回"解" (已修复)

**发现于**: graphw-05, graphw-06 测试
**文件**: `samples/main_new_parser.cpp`

**问题**: 无条件调用 `mac.get_solution()`，即使初始 AC 传播检测到不一致也会填充 solution vector。

**修复**: 删除无条件的 `get_solution()` 调用（MAC::enforce 已在找到解时内部调用）。

## 测试结果

### 测试配置
- 超时限制: 60 秒/实例
- AC 算法: AC3bit
- 变量启发式: DOM_MIN
- 值启发式: MIN

### 汇总统计

| 指标 | 结果 |
|------|------|
| 总测试实例 | 15 |
| OR-Tools/CPIM 匹配 | 8/15 (53%) |
| 验证通过 (SAT) | 4/4 (100%) |
| 验证失败 | 0 |

### 详细结果

| 文件 | OR-Tools | CPIM | 匹配 | 验证 |
|------|----------|------|------|------|
| XMLFile.xml | SAT(12ms) | SAT(0ms) | OK | OK |
| langford-3-9-ext.xml | SAT(72ms) | SAT(334ms) | OK | OK |
| langford-3-11-ext.xml | UNSAT(2.7s) | UNSAT(30s) | OK | N/A |
| langford-2-4-ext.xml | SAT(6ms) | SAT(0ms) | OK | OK |
| rand-2-40-8-753-100-0_ext.xml | SAT(485ms) | SAT(33s) | OK | OK |
| graphw-05_ext.xml | UNSAT(74ms) | UNSAT(12ms) | OK | N/A |
| graphw-06_ext.xml | UNSAT(125ms) | UNSAT(3ms) | OK | N/A |
| rand-2-40-80-103-800-0_ext.xml | TIMEOUT | TIMEOUT | OK | N/A |

### DIFF 分析 (7 个不匹配)

所有不匹配都是因为 **CPIM 超时** 而非错误结果：

| 文件 | OR-Tools | CPIM | 原因 |
|------|----------|------|------|
| driverlogw-09-sat_ext.xml | SAT(3.1s) | TIMEOUT | CPIM 较慢 |
| BlackHole-4-4-e-0_ext.xml | UNSAT(236ms) | TIMEOUT | CPIM 较慢 |
| BlackHole-4-4-e-3_ext.xml | UNSAT(66ms) | TIMEOUT | CPIM 较慢 |
| rand-2-40-8-753-100-1_ext.xml | UNSAT(42s) | TIMEOUT | CPIM 较慢 |
| rand-2-40-8-753-100-2_ext.xml | SAT(585ms) | TIMEOUT | CPIM 较慢 |
| rand-2-40-80-103-800-1_ext.xml | SAT(34s) | TIMEOUT | CPIM 较慢 |
| rand-2-40-180-84-900-0_ext.xml | SAT(14s) | TIMEOUT | CPIM 较慢 |

## 结论

1. **正确性验证通过**: 所有 CPIM 找到的解都通过了约束验证 (4/4 = 100%)
2. **UNSAT 检测正确**: graphw-05/06 等 UNSAT 实例正确识别
3. **性能差距**: CPIM 比 OR-Tools 慢 10-100 倍，导致部分实例超时

## 建议

1. **性能优化**: 考虑实现更高效的传播算法或启用 GPU 加速
2. **扩展测试**: 增加更多小规模实例以验证正确性
3. **GPU 测试**: 验证 GModel GPU 求解器的正确性

## 相关文件

| 文件 | 说明 |
|------|------|
| `samples/batch_test.py` | 批量测试脚本 |
| `samples/solve_xcsp_ortools.py` | OR-Tools 求解器 |
| `docs/BUG_FIX_MAC_GET_SOLUTION.md` | BUG #1 修复文档 |
| `build/cpim_test_parser` | CPIM CPU 求解器 |
