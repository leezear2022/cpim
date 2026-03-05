# SAC 算法实现文档 (Code Review 用)

## 概述

本文档描述 CPIM 项目中 SAC (Singleton Arc Consistency) 算法族的实现，供 Code Review 使用。

**实现时间**: 2024年12月
**实现者**: Claude (Anthropic AI)
**代码位置**: `src/solver/cpu/MSAC3bit.cpp`

---

## 1. SAC 算法原理

### 1.1 什么是 SAC

SAC (Singleton Arc Consistency) 是比 AC (Arc Consistency) 更强的一致性级别：

```
定义：一个 CSP 是 SAC 的，当且仅当：
  对于每个变量 x 和其域中的每个值 a，
  临时赋值 x=a 后运行 AC，如果 AC 失败，则 a 是 SAC 不一致的，可以删除。
```

### 1.2 SAC1 vs SAC3

| 算法 | 复杂度 | 策略 |
|------|--------|------|
| SAC1 | O(en²d³) | 每轮全扫描所有 (变量,值) 对 |
| SAC3 | O(en²d²) | 增量队列，只检查受影响的值 |

其中 e=约束数, n=变量数, d=最大域大小

---

## 2. 代码结构

### 2.1 类层次

```
AC (抽象基类)
 └── AC3 (基础 AC3 算法)
      └── AC3bit (位运算优化的 AC3)

MSAC3bit (SAC 实现，使用 AC3bit 作为内核)
 ├── kernel_: AC3bit*     // AC 传播内核
 ├── config_: MSACConfig  // SAC1/SAC3 模式配置
 ├── pending_queue_       // SAC3 的待检查队列
 └── in_queue_            // 队列成员标记
```

### 2.2 核心文件

| 文件 | 作用 |
|------|------|
| `include/Solver.h` | MSACConfig, MSACStats, MSAC3bit 类声明 |
| `src/solver/cpu/MSAC3bit.cpp` | MSAC3bit 实现 |
| `src/solver/cpu/AC3bit.cpp` | AC3bit 内核实现 |
| `src/base/unified_trail.cpp` | Trail 回溯系统 |

---

## 3. 关键实现细节

### 3.1 MSACConfig 配置结构

```cpp
struct MSACConfig {
  enum Mode { SAC1, SAC3 };
  Mode mode = SAC1;           // 默认 SAC1 模式
  int max_probes = -1;        // probe 次数限制 (-1=无限)
  double max_time_ms = -1;    // 时间限制 (-1=无限)
  int depth_limit = -1;       // 搜索深度限制 (-1=无限)
};
```

### 3.2 enforce() 主流程

```cpp
ConsistencyState MSAC3bit::enforce(vector<IntVar*>& x_evt, int level) {
  // Phase 1: 先运行 AC3bit 达到 AC
  ConsistencyState cs = kernel_->enforce(x_evt, level);
  if (!cs.state) return cs;  // AC 已失败

  // Phase 2: SAC 不动点循环
  bool changed = true;
  while (changed && ShouldContinueProbe(level)) {
    changed = false;

    // 收集候选值
    vector<pair<IntVar*, int>> candidates;
    SelectCandidates(candidates, level);

    // Probe 每个候选
    for (auto& [x, a] : candidates) {
      if (!x->have(a)) continue;  // 值可能已被删除

      if (!ProbeValue(x, a, level)) {
        // Probe 失败 → 删除值
        x->RemoveValue(a);

        // SAC3: 将邻域值入队
        if (config_.mode == MSACConfig::SAC3) {
          EnqueueNeighborhood(x, a);
        }

        // 检查域是否为空
        if (x->faild()) {
          cs.state = false;
          return cs;
        }

        // 重新传播
        vector<IntVar*> evt = {x};
        cs = kernel_->enforce(evt, level);
        if (!cs.state) return cs;

        changed = true;
      }
    }
  }

  cs.state = true;
  return cs;
}
```

### 3.3 ProbeValue() 核心逻辑

```cpp
bool MSAC3bit::ProbeValue(IntVar* x, int a, int level) {
  ++stats_.num_probes;

  // 关键：禁止 weight 更新（防止污染启发式）
  ScopedWeightUpdates guard(kernel_, false);

  // 记录当前 Trail 层级
  const int before_level = m_->trail()->CurrentLevel();

  // 优化：只保存 x 的 assigned 状态（只有 x 会被 ReduceTo 修改）
  const bool x_was_assigned = x->assigned();

  // 创建新的 Trail 层进行测试
  m_->trail()->NewLevel();

  // 临时赋值 x=a
  x->ReduceTo(a);
  x->assign(true);

  // 运行 AC 传播
  vector<IntVar*> evt = {x};
  ConsistencyState cs = kernel_->enforce(evt, level);

  // 回溯到测试前状态
  m_->trail()->BacktrackTo(before_level);

  // 优化：只恢复 x 的 assigned 状态 O(1)
  x->assign(x_was_assigned);

  if (!cs.state) {
    ++stats_.num_probe_fail;
  }

  return cs.state;
}
```

### 3.4 SAC3 队列管理

```cpp
// 优化后使用 vector 替代 set，O(log k) → O(1)
void MSAC3bit::InitializeQueue() {
  pending_queue_.clear();
  in_queue_.resize(m_->vars.size());
  for (auto* x : m_->vars) {
    in_queue_[x->id()].assign(x->capacity() + 1, false);
  }

  // 所有未赋值变量的所有值入队
  for (auto* x : m_->vars) {
    if (!x->assigned()) {
      for (int a = x->head(); a != Limits::INDEX_OVERFLOW; a = x->next(a)) {
        pending_queue_.push_back({x, a});  // O(1)
        in_queue_[x->id()][a] = true;
      }
    }
  }
}

void MSAC3bit::EnqueueValue(IntVar* x, int a) {
  // 使用 in_queue_ 去重，避免 set 开销
  if (!in_queue_[x->id()][a]) {
    pending_queue_.push_back({x, a});
    in_queue_[x->id()][a] = true;
  }
}

void MSAC3bit::EnqueueNeighborhood(IntVar* x, int deleted_val) {
  // 将 x 的所有邻域变量的所有值入队
  auto it = m_->subscription.find(x);
  if (it == m_->subscription.end()) return;

  for (auto* c : it->second) {      // 遍历 x 参与的约束
    for (auto* y : c->scope) {       // 遍历约束的变量
      if (y != x && !y->assigned()) {
        for (int b = y->head(); b != Limits::INDEX_OVERFLOW; b = y->next(b)) {
          EnqueueValue(y, b);
        }
      }
    }
  }
}
```

---

## 4. 发现并修复的 Bug

### 4.1 Bug: TrailEntry 使用 32 位存储 64 位域

**问题描述**:
- `BITSIZE = 64`（域用 64 位 bitset 表示）
- `TrailEntry.old_bits` 原来是 `uint32_t`
- 导致高 32 位（值 32-63）在回溯时丢失

**症状**:
```
问题实例: rand-2-40-80-103-800-1
OR-Tools: SAT (有解)
CPIM: UNSAT (错误)

调试发现:
  V31 应该有 16 个值: [0,8,20,33,35,47,48,50,52,55,56,57,61,65,69,78]
  回溯后只剩 6 个值: [0,8,20,65,69,78]  (值 32-63 全丢了)
```

**修复**:
```cpp
// include/base/unified_trail.h
struct TrailEntry {
  Type type;
  uint8_t padding[7];      // 对齐到 8 字节
  int32_t var_id;
  int32_t word_index;
  uint64_t old_bits;       // 修改: uint32_t → uint64_t
};

// include/Network.h
void RestoreBitWord(int word_idx, uint64_t bits);  // 同步修改

// src/solver/common/Network.cpp
void IntVar::RestoreBitWord(int word_idx, uint64_t bits) {
  bit_doms_[word_idx] = std::bitset<BITSIZE>(bits);
  // 重新计算 top_size
  top_size = 0;
  for (const auto& w : bit_doms_) {
    top_size += w.count();
  }
}
```

---

## 5. 与 MAC 求解器的集成

### 5.1 使用方式

```cpp
// apps/cpim_test_parser.cpp
MAC* mac = new MAC(n, A_MSAC3bit, Heuristic::Var::DOM_WDEG, ...);
```

### 5.2 ACAlgorithm 枚举扩展

```cpp
enum ACAlgorithm {
  AC_3,
  AC_3bit,
  // ...
  A_MSAC3bit,  // 新增
};
```

### 5.3 MAC 构造器中的实例化

```cpp
case A_MSAC3bit:
  ac_ = new MSAC3bit(n_);
  break;
```

---

## 6. 测试方法

### 6.1 测试脚本

**主测试脚本**: `tests/python/batch_test_v2.py`

```bash
# 运行 TIER 0 测试（快速验证）
python3 tests/python/batch_test_v2.py --tier=0

# 运行 TIER 1 测试（更全面）
python3 tests/python/batch_test_v2.py --tier=1

# 测试单个实例
./build/cpim_test_parser --bench_path=tests/data/bench/queens-4_ext.xml
```

### 6.2 测试分层

| TIER | 实例数 | 说明 |
|------|--------|------|
| 0 | 12 | 快速验证（Queens, 小型问题） |
| 1 | 39 | 中等规模（Langford, rand-2-*） |
| 2 | 100+ | 大规模（完整 benchmarks） |

### 6.3 验证方法

测试脚本对比 CPIM 和 OR-Tools 的结果：

```python
def compare_results(cpim_result, ortools_result):
    # 1. SAT/UNSAT 一致性
    if cpim_result.sat != ortools_result.sat:
        return "CORRECTNESS BUG"

    # 2. 如果都是 SAT，验证解的正确性
    if cpim_result.sat and ortools_result.sat:
        if not verify_solution(cpim_result.solution):
            return "INVALID SOLUTION"

    return "MATCH"
```

### 6.4 测试结果示例

```
=== TIER 1 测试结果 ===
总计: 39 个实例
匹配: 27 (69%)
超时: 12 (31%)
正确性错误: 0

性能对比:
  queens-4: CPIM=5ms, OR-Tools=2ms
  langford-3-9: CPIM=89ms, OR-Tools=12ms
```

---

## 7. 已知限制

1. **性能**: SAC 增加了大量 probe 开销，对简单问题可能比纯 AC 慢（30-100x）
2. **SAC3 增量初始化**: 当前每次 enforce 全量初始化队列，未实现跨节点增量（Antigravity 建议的优化方向）
3. **并行化**: 当前 probe 是串行的，未利用 GPU

---

## 8. 文件清单

```
src/solver/cpu/MSAC3bit.cpp     # SAC 主实现 (284 行)
include/Solver.h                # 类声明 (MSACConfig, MSAC3bit)
src/base/unified_trail.cpp      # Trail 系统
include/base/unified_trail.h    # Trail 头文件
src/solver/common/Network.cpp   # IntVar::RestoreBitWord
include/Network.h               # IntVar 声明
tests/python/batch_test_v2.py   # 批量测试脚本
tests/python/tier_definitions.py # 测试分层定义
```

---

## 9. Code Review 后的修复 (2024-12-24)

根据 Code Review 反馈，修复了以下问题：

### 9.1 SAC3 队列回溯问题 (高优先级)

**问题**: SAC3 队列只在首次初始化，不随 Trail 回溯恢复，导致 SAC3 退化为 AC。

**修复**: 每次 enforce() 都重新初始化队列

```cpp
// 修复前：只初始化一次
if (config_.mode == MSACConfig::SAC3 && !queue_initialized_) {
  InitializeQueue();
}

// 修复后：每次都初始化
if (config_.mode == MSACConfig::SAC3) {
  InitializeQueue();
}
```

**验证**: queens-12 的 SAC3 从 P=41/N=29 (与 AC3bit 相同) → P=14/N=2 (与 SAC1 一致)

### 9.2 预算/统计问题 (中优先级)

**问题 1**: stats_ 不重置，导致 max_probes 变成全局累计，搜索早期耗尽后永久停止 probe。

**问题 2**: stats_.Reset() 导致最终报告显示 0（Antigravity Code Review 发现）。

**修复**: 分离局部统计（预算控制）和全局统计（最终报告）

```cpp
// include/Solver.h
struct MSACStats {
  // 局部统计（每次 enforce 重置，用于预算控制）
  int num_probes = 0;
  int num_probe_fail = 0;
  int num_removed = 0;
  double probe_time_ms = 0.0;
  bool exited_by_budget = false;

  // 全局统计（累计，用于最终报告）
  int64_t total_probes = 0;
  int64_t total_probe_fail = 0;
  int64_t total_removed = 0;
  double total_probe_time_ms = 0.0;
  int total_enforce_calls = 0;

  void Reset();              // 只重置局部
  void AccumulateToGlobal(); // 累加到全局
};

// src/solver/cpu/MSAC3bit.cpp
ConsistencyState MSAC3bit::enforce(...) {
  stats_.Reset();  // 重置局部统计
  // ... 执行 SAC ...
  stats_.AccumulateToGlobal();  // 累加到全局
  return cs;
}
```

**验证**: queens-12 现在正确显示 Total probes: 599 (26 failed)

### 9.3 预算触发时 candidates 丢失 (中优先级)

**问题**: 预算耗尽时 break，剩余 candidates 丢失。

**修复**: 通过每次重新初始化队列隐式解决 —— 下次 enforce() 会重新拾取这些值。

## 10. 低优先级优化 (已完成 2024-12-24)

### 10.1 assigned 状态保存优化 ✅

**问题**: 每个 probe 保存/恢复所有变量 assigned_，O(n) 开销

**分析**: ProbeValue 只对单个变量 x 调用 ReduceTo()，只有 x 的 assigned 会被修改

**修复**:
```cpp
// 优化前：O(n)
std::vector<bool> saved_assigned;
for (auto* v : m_->vars) {
  saved_assigned.push_back(v->assigned());
}
// ... probe ...
for (size_t i = 0; i < m_->vars.size(); ++i) {
  m_->vars[i]->assign(saved_assigned[i]);
}

// 优化后：O(1)
const bool x_was_assigned = x->assigned();
// ... probe ...
x->assign(x_was_assigned);
```

### 10.2 SAC3 队列数据结构优化 ✅

**问题**: std::set 的 insert/erase O(log n)，且不需要排序

**修复**: 改用 vector + in_queue_ bitset 标记

```cpp
// Solver.h 修改
std::vector<std::pair<IntVar*, int>> pending_queue_;  // set → vector

// InitializeQueue: O(log k) → O(1)
pending_queue_.push_back({x, a});  // 替代 insert

// SelectCandidates: O(k·log k) → O(k)
for (auto& [x, a] : pending_queue_) { ... }
pending_queue_.clear();  // 替代逐个 erase

// EnqueueValue: O(log k) → O(1)
if (!in_queue_[x->id()][a]) {
  pending_queue_.push_back({x, a});
  in_queue_[x->id()][a] = true;
}
```

**验证**: SAC1/SAC3 结果完全一致，正确性保持

---

## 11. 相关文档

- [CLAUDE.md](../../CLAUDE.md) - 项目总览
- [UNIFIED_TRAIL_MEMO.md](../bugfixes/UNIFIED_TRAIL_MEMO.md) - Trail 系统设计
- [ARCHITECTURE.md](../architecture/ARCHITECTURE.md) - 整体架构
- [SAC_TEST_REPORT_2024-12-24.md](../testing/SAC_TEST_REPORT_2024-12-24.md) - SAC 测试报告

---

*文档创建时间: 2024-12-24*
*最后更新: 2024-12-24 (统计问题修复 - Antigravity Code Review)*
*生成者: Claude (Anthropic)*
