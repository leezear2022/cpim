# BUG 修复报告：MAC::get_solution() 返回错误解

## 问题描述

**发现日期**: 2024-12-17
**严重程度**: 严重 (导致求解器返回无效解)
**影响范围**: 所有使用 MAC 搜索的求解

### 症状

使用 `cpim_test_parser` 求解 queens-12 问题时，返回的解违反了约束条件：

```
VIOLATION: constraint 23 (V2, V5) tuple=(4, 7)
VIOLATION: constraint 30 (V3, V4) tuple=(9, 10)
VIOLATION: constraint 31 (V3, V5) tuple=(9, 7)
```

OR-Tools 对同一问题返回正确解，仅需 41ms。

## 根因分析

### 调试过程

1. **验证约束数据正确性** - 确认 `ModelNormalizer` 正确将 conflicts 转为 supports
2. **验证 Network 构造** - 确认约束和 subscription 正确
3. **验证 AC3bit 传播** - 确认 `bitSup_` 构建正确，`seek_support` 返回正确结果
4. **追踪搜索过程** - 发现 V5=7 在搜索中被正确删除，但最终解中仍包含 V5=7

### 根本原因

**BUG 位置**: `src/MAC.cpp` 第 255-267 行

```cpp
// 错误代码
void MAC::get_solution() {
  solution.resize(n_->vars.size());
  for (int i = 0; i < n_->vars.size(); ++i) {
    solution[i] = I[i].a();  // BUG: I[i] 是第 i 个赋值，不是变量 i 的值！
  }
  // ...
}
```

**问题**: `AssignedStack I` 存储的是 **赋值操作序列**，`I[i]` 返回第 i 个被赋值的变量-值对，而不是变量 i 的赋值值。

例如：
- `I[0]` 可能是 `(var[3], 5)` - 表示第一个赋值是 var[3]=5
- `I[1]` 可能是 `(var[7], 2)` - 表示第二个赋值是 var[7]=2

但错误代码将 `I[0].a()` (即 5) 赋给了 `solution[0]`，导致变量值错位。

## 修复方案

从变量当前域获取解，而不是从赋值栈：

```cpp
// 修复后代码
void MAC::get_solution() {
  solution.resize(n_->vars.size());
  int level = I.size();
  for (int i = 0; i < n_->vars.size(); ++i) {
    // 从变量域获取唯一剩余值（AC 传播后每个变量域应只剩一个值）
    solution[i] = n_->vars[i]->head(level);
  }
  // ...
}
```

## 相关修复

同时修复了 `IntVar::ReduceTo()` 未设置 `assigned_` 标志的问题：

```cpp
void IntVar::ReduceTo(const int a, const int p) {
  const auto index = GetBitIdx(a);
  for (auto& v : bit_doms_[p]) v.reset();
  bit_doms_[p][get<0>(index)].set(get<1>(index));
  top_size = 0;
  assigned_[p] = true;  // 新增：标记变量已赋值
}
```

## 修改文件

| 文件 | 修改内容 |
|------|----------|
| `src/MAC.cpp` | 修复 `get_solution()` 从域获取解 |
| `src/Network.cpp` | 修复 `ReduceTo()` 设置 assigned 标志 |

## 验证

修复后验证通过：

```bash
# 正常模式
./cpim_test_parser --bench_path=samples/bench/queens-12_ext.xml

# 调试模式（启用解验证）
GLOG_v=1 ./cpim_test_parser --bench_path=samples/bench/queens-12_ext.xml
```

输出：
```
MAC solution (canonical indices): 0 2 4 10 7 9 11 3 1 6 8 5
MAC solution (original values): 1 3 5 11 8 10 12 4 2 7 9 6
All constraints satisfied!
```

## 调试方法

使用 glog 的 VLOG 机制控制调试输出：

```cpp
// 代码中使用 VLOG
VLOG(1) << "Debug message";           // 级别 1
VLOG(2) << "More detailed message";   // 级别 2

// 条件检查
if (VLOG_IS_ON(1)) {
    // 仅在调试模式下执行的代码
}
```

启用方式：
```bash
# 环境变量方式
GLOG_v=1 ./cpim_test_parser ...

# 或在代码中设置
FLAGS_v = 1;
```

## 经验教训

1. **索引语义要清晰**: `I[i]` 和 `solution[i]` 的 `i` 含义不同，需要明确区分
2. **添加解验证**: 求解器应内置解验证功能，便于发现类似问题
3. **使用 VLOG 分级**: 调试信息应使用 VLOG 而非 LOG，便于控制输出

## 测试对比

| 问题 | OR-Tools | CPIM (修复后) |
|------|----------|---------------|
| queens-12 | 41ms, 正确 | 6ms, 正确 |
| haystacks-11 | 0.4s, INFEASIBLE | - |
| rand-2-23 | >30s | >30s |
