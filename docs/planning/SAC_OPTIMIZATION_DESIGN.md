# SAC 优化设计方案

## 文档信息
- **作者**: Claude Sonnet 4.5
- **创建时间**: 2025-12-22
- **相关文档**: [WORK_SUMMARY_2025-12-21.md](WORK_SUMMARY_2025-12-21.md)
- **问题背景**: SAC1/SAC3 实现存在但性能极差，需要优化

---

## 1. 现状分析

### 1.1 当前实现

**SAC1** ([src/solver/cpu/SAC1.cpp](../../src/solver/cpu/SAC1.cpp)):
- 基础单例弧一致性算法
- 对每个变量的每个值进行穷举测试
- 通过 `NewLevel()` + `ReduceTo()` + `AC::enforce()` + `BacktrackTo()` 测试值的可行性

**SAC3** ([src/solver/cpu/SAC3.cpp](../../src/solver/cpu/SAC3.cpp)):
- 使用 `Qsac` 队列管理待测试值
- 实现 `BuildBranch()` 启发式值选择
- 理论上比 SAC1 更高效，但代码实现有问题

**NSAC** ([src/solver/cpu/NSAC.cpp](../../src/solver/cpu/NSAC.cpp)):
- 邻域单例弧一致性
- 只传播邻域约束，而非全局约束
- **当前状态**: Queens-4 超时 >10 秒（应该 <1ms）

### 1.2 性能瓶颈

#### 瓶颈 1: 大量回溯操作
```cpp
// SAC1::enforce() - 对每个值都要回溯
for (auto x : n_->vars) {
  for (auto a : x->values()) {
    n_->trail()->NewLevel();           // O(1)
    const int test_level = n_->trail()->CurrentLevel();

    x->ReduceTo(a);                    // O(d)
    x_evt_.push_back(x);
    result = ac_->enforce(x_evt_, test_level).state;  // O(ed^2)

    n_->trail()->BacktrackTo(test_level - 1);  // O(变更数量)
  }
}
```

**复杂度分析**:
- 对于 n 个变量，平均域大小 d
- 总测试次数: O(n * d)
- 每次测试: AC 传播 O(ed²) + 回溯 O(Δ)
- **总复杂度**: O(n * d * (ed² + Δ))

**实际测试数据** (Queens-4, n=4, d=4, e=12):
- AC3bit 求解: <1ms
- SAC1 估算: 4 * 4 * 12 * 16 = 3072 次 revise 操作
- 如果每次 revise 1μs，总时间 ~3ms（可接受）
- **问题**: 实际运行可能慢 1000x+

#### 瓶颈 2: 无增量优化

**当前实现缺陷**:
```cpp
// 测试 (x1, a1) - 传播整个网络
ac_->enforce(x_evt_, level);  // 检查所有约束

// 测试 (x1, a2) - 又从头开始
ac_->enforce(x_evt_, level);  // 重复检查大量约束
```

**理想情况**:
- 对同一变量的不同值测试，很多约束的支持集是相同的
- 应该缓存支持信息

#### 瓶颈 3: NSAC 邻域计算开销

```cpp
NSAC::NSAC(Network* m) : AC3bit(m) {
  const int vars_bit_length = ceil(float(m_->vars.size()) / BITSIZE);
  for (auto i : m_->vars) {
    neibor_[i].resize(vars_bit_length, 0);
    for (auto v : m_->neighborhood[i]) {  // ← 这里构建邻域
      auto a = GetBitIdx(v->id());
      neibor_[i][get<0>(a)].set(get<1>(a));
    }
  }
}
```

**问题**:
- 邻域计算在构造函数中只执行一次，这不是瓶颈
- **真正的瓶颈**: `revise_NSAC()` 中的嵌套测试

```cpp
int NSAC::revise_NSAC(IntVar* v, IntVar* x, const int level) {
  int num = 0;
  for (auto a : v->values()) {          // O(d)
    if (v->have(a)) {
      m_->trail()->NewLevel();
      v->ReduceTo(a);
      v->assign(true);
      const bool res = full_NSAC(v, x, level);  // ← O(邻域大小 * AC复杂度)
      m_->trail()->BacktrackTo(test_level - 1);

      if (!res) {
        v->RemoveValue(a);
        ++num;
      }
    }
  }
  return num;
}
```

**复杂度**:
- 对于 Queens-4: 每个变量有 3 个邻居（同行/列/对角线）
- `full_NSAC()` 对每个邻居运行 AC 传播
- 总复杂度: O(n * d * (邻域大小 * ed²))
- Queens-4: 4 * 4 * (3 * 12 * 16) ≈ 9216 次 revise
- **但为什么超时 >10 秒？** → 需要性能分析

---

## 2. 优化策略

### 策略 1: 位向量加速 SAC (立即可行)

**核心思想**: 复用 AC3bit 的位向量优化

#### 实现方案

**2.1.1 缓存支持索引**

```cpp
class SAC1_Optimized : public SAC1 {
 private:
  // 缓存每个值的支持位向量（从 AC3bit 继承）
  AC3bit* ac_bit_;  // 强制使用 AC3bit

  // 快速失败检测
  std::vector<std::bitset<BITSIZE>> value_support_cache_;
};
```

**2.1.2 快速支持检测**

```cpp
bool SAC1_Optimized::quick_support_test(IntVar* x, int a) {
  // 对值 (x, a) 做快速检测
  for (auto c : n_->subscription[x]) {
    IntConVal cv(c, x, a);

    // 直接使用 AC3bit 的 bitSup_ 检查支持
    if (!ac_bit_->seek_support(cv, 0)) {
      return false;  // 无支持，跳过完整测试
    }
  }
  return true;  // 可能有支持，需要完整测试
}

bool SAC1_Optimized::enforce(vector<IntVar*> x_evt, const int level) {
  // ... 初始 AC 传播 ...

  for (auto x : n_->vars) {
    for (auto a : x->values()) {
      if (x->have(a)) {
        // **优化 1: 快速失败检测**
        if (!quick_support_test(x, a)) {
          x->RemoveValue(a);
          continue;  // 跳过昂贵的回溯测试
        }

        // **优化 2: 仅在需要时才回溯测试**
        n_->trail()->NewLevel();
        const int test_level = n_->trail()->CurrentLevel();

        x->ReduceTo(a);
        x->assign(true);
        x_evt_.push_back(x);
        result = ac_->enforce(x_evt_, test_level).state;
        x_evt_.clear();

        n_->trail()->BacktrackTo(test_level - 1);

        if (!result) {
          x->RemoveValue(a);
        }
      }
    }
  }
}
```

**预期收益**:
- 快速失败: 减少 30-50% 的回溯测试
- Queens-4: 3072 → ~1500 次回溯

---

### 策略 2: SAC-SDS (支持驱动的 SAC) (中期)

**参考论文**: Bessière et al. (2008) - "SAC with Support-Driven Singleton Checks"

**核心思想**:
- 不对每个值单独测试
- 从约束的支持关系推导出必须删除的值

#### 实现方案

```cpp
class SAC_SDS : public AC3bit {
 public:
  SAC_SDS(Network* m);
  ConsistencyState enforce(vector<IntVar*>& x_evt, const int level) override;

 private:
  // 支持计数器: support_count_[IntConValIndex] = 支持该值的元组数量
  std::vector<int> support_count_;

  // 待删除值队列
  std::queue<IntConVal> pending_removals_;

  bool propagate_singleton_removals(const int level);
  void update_support_counts(const IntConVal& removed);
};
```

**算法流程**:
```cpp
bool SAC_SDS::enforce(vector<IntVar*>& x_evt, const int level) {
  // 1. 初始 GAC 传播
  auto res = AC3bit::enforce(x_evt, level);
  if (!res.state) return res;

  // 2. 初始化支持计数器
  for (auto c : m_->tabs) {
    for (auto x : c->scope) {
      for (auto a : x->values()) {
        if (x->have(a)) {
          IntConVal cv(c, x, a);
          int count = count_supports(cv);  // 使用 bitSup_ 快速计数
          support_count_[m_->GetIntConValIndex(cv)] = count;

          if (count == 0) {
            pending_removals_.push(cv);
          }
        }
      }
    }
  }

  // 3. 传播单例删除
  while (!pending_removals_.empty()) {
    IntConVal cv = pending_removals_.front();
    pending_removals_.pop();

    if (!cv.v()->have(cv.a())) continue;  // 已删除

    // 测试删除该值是否导致失败
    m_->trail()->NewLevel();
    const int test_level = m_->trail()->CurrentLevel();

    cv.v()->RemoveValue(cv.a());
    vector<IntVar*> evt = {cv.v()};
    auto cs = AC3bit::enforce(evt, test_level);

    if (!cs.state) {
      // 该值是 SAC 必须的，不能删除
      m_->trail()->BacktrackTo(test_level - 1);
    } else {
      // 该值可以安全删除
      m_->trail()->BacktrackTo(test_level - 1);
      cv.v()->RemoveValue(cv.a());  // 真正删除

      // 更新依赖该值的支持计数
      update_support_counts(cv);
    }
  }

  return true;
}

void SAC_SDS::update_support_counts(const IntConVal& removed) {
  // 遍历所有使用 removed 作为支持的值
  for (auto c : m_->subscription[removed.v()]) {
    for (auto t : c->tuples()) {
      // 如果元组包含 removed.a()，则依赖该元组的值支持数 -1
      // ... (使用 bitSup_ 快速查找)
    }
  }
}
```

**复杂度**:
- 支持计数初始化: O(edt) (e=约束数, d=域大小, t=元组数)
- 单例测试: 仅测试支持数=0 的值，远少于 O(nd)
- **预期**: Queens-4 测试次数 < 100

---

### 策略 3: 修复 NSAC 性能问题 (高优先级)

#### 问题诊断步骤

**3.1 性能分析**
```bash
# 1. 使用 GLOG 详细日志
GLOG_v=3 timeout 30s ./build/cpim_test_parser \
  --bench_path=tests/data/bench/queens-4_ext.xml \
  --ac_algorithm=NSAC 2>&1 | tee nsac_debug.log

# 2. 使用 gprof 性能分析
g++ -pg -O2 -o cpim_test_parser_prof ...
./cpim_test_parser_prof --bench_path=tests/data/bench/queens-4_ext.xml --ac_algorithm=NSAC
gprof cpim_test_parser_prof gmon.out > nsac_profile.txt
```

**3.2 预期瓶颈**

基于代码分析，可能的问题：

**问题 A: `full_NSAC()` 重复计算**
```cpp
bool NSAC::full_NSAC(IntVar* v, IntVar* x, const int level) {
  // ...
  while (!q_nei_.empty()) {
    IntVar* y = q_nei_.pop();

    for (auto c : m_->subscription[y]) {
      IntVar* z = (c->scope[0] == y) ? c->scope[1] : c->scope[0];

      // ← 这里可能重复 push 同一个变量
      if ((is_neibor(v, z) == false) || (v == z)) continue;
      res = revise(arc(c, z), level);
      // ...
    }
  }
}
```

**修复方案**:
```cpp
bool NSAC::full_NSAC(IntVar* v, IntVar* x, const int level) {
  bool res;
  q_nei_.clear();

  // 使用位集合避免重复
  std::vector<bool> in_queue(m_->vars.size(), false);

  for (auto c : m_->subscription[v]) {
    IntVar* y = (c->scope[0] == v) ? c->scope[1] : c->scope[0];
    res = revise(arc(c, y), level);

    if (!in_queue[y->id()]) {
      q_nei_.push(y);
      in_queue[y->id()] = true;
    }

    if (res && y->faild()) {
      ++(c->weight);
      return false;
    }
  }

  while (!q_nei_.empty()) {
    IntVar* y = q_nei_.pop();
    in_queue[y->id()] = false;  // 标记已处理

    for (auto c : m_->subscription[y]) {
      IntVar* z = (c->scope[0] == y) ? c->scope[1] : c->scope[0];

      if (!is_neibor(v, z) || v == z) continue;

      res = revise(arc(c, z), level);
      if (res) {
        if (z->faild()) {
          ++(c->weight);
          return false;
        }

        if (!in_queue[z->id()]) {
          q_nei_.push(z);
          in_queue[z->id()] = true;
        }
      }
    }
  }

  return true;
}
```

**问题 B: `is_neibor()` 调用开销**
```cpp
bool NSAC::is_neibor(IntVar* x, IntVar* v) {
  // 位图查找，应该很快 O(1)
  auto a = GetBitIdx(v->id());
  return neibor_[x][get<0>(a)].test(get<1>(a));
}
```

这个应该不是瓶颈，除非 `neibor_` 构建有问题。

**问题 C: `revise()` 在测试层级上的问题**

```cpp
// NSAC::revise_NSAC() 中
m_->trail()->NewLevel();
const int test_level = m_->trail()->CurrentLevel();

v->ReduceTo(a);
v->assign(true);
const bool res = full_NSAC(v, x, level);  // ← 传递的是原始 level

// full_NSAC() 中调用
res = revise(arc(c, y), level);  // ← 使用原始 level
```

**这可能是 Bug！** - `revise()` 应该使用 `test_level` 而不是原始 `level`

**修复**:
```cpp
int NSAC::revise_NSAC(IntVar* v, IntVar* x, const int level) {
  int num = 0;
  for (auto a : v->values()) {
    if (v->have(a)) {
      m_->trail()->NewLevel();
      const int test_level = m_->trail()->CurrentLevel();

      v->ReduceTo(a);
      v->assign(true);

      // **修复: 传递 test_level**
      const bool res = full_NSAC(v, x, test_level);

      m_->trail()->BacktrackTo(test_level - 1);

      if (!res) {
        v->RemoveValue(a);
        ++num;
      }
    }
  }
  return num;
}

bool NSAC::full_NSAC(IntVar* v, IntVar* x, const int level) {
  // level 现在是 test_level，正确传递给 revise()
  // ...
}
```

---

### 策略 4: SAC-Opt (最优 SAC 实现) (长期)

**参考**: Lecoutre & Cardon (2005) - "A Greedy Approach to Establish Singleton Arc Consistency"

**核心优化**:
1. **值排序**: 优先测试最可能失败的值（减少无效测试）
2. **早期终止**: 检测到固定点时停止
3. **邻域传播**: 仅传播受影响的约束（类似 NSAC）
4. **并行化**: 独立值可以并行测试（未来 GPU 加速）

**伪代码**:
```cpp
class SAC_Opt : public AC3bit {
 private:
  // 值优先级队列（按支持数排序）
  std::priority_queue<IntConVal, ..., SupportCountComparator> value_queue_;

  // 已测试值缓存
  std::unordered_set<IntConVal> tested_values_;

 public:
  bool enforce_sac_opt(const int level) {
    // 1. 初始 GAC
    auto res = AC3bit::enforce(m_->vars, level);
    if (!res.state) return false;

    // 2. 初始化优先级队列
    for (auto x : m_->vars) {
      for (auto a : x->values()) {
        if (x->have(a)) {
          int support_count = compute_support_count(x, a);
          value_queue_.push({x, a, support_count});
        }
      }
    }

    // 3. 贪心测试
    while (!value_queue_.empty()) {
      auto [x, a, count] = value_queue_.top();
      value_queue_.pop();

      if (!x->have(a)) continue;  // 已被删除
      if (tested_values_.count({x, a})) continue;  // 已测试

      // 快速支持检测
      if (count == 0) {
        x->RemoveValue(a);
        propagate_removal(x, a, level);
        continue;
      }

      // 完整测试
      m_->trail()->NewLevel();
      const int test_level = m_->trail()->CurrentLevel();

      x->ReduceTo(a);
      auto cs = AC3bit::enforce({x}, test_level);

      m_->trail()->BacktrackTo(test_level - 1);

      if (!cs.state) {
        x->RemoveValue(a);
        propagate_removal(x, a, level);
      } else {
        tested_values_.insert({x, a});
      }
    }

    return true;
  }
};
```

---

## 3. 实施路线图

### Phase 1: 立即修复 (1-2 天)

**目标**: 修复 NSAC 性能问题，使其能正常工作

**任务**:
1. ✅ 添加性能分析日志
   ```cpp
   // NSAC.cpp 中添加计数器
   int num_full_nsac_calls = 0;
   int num_revise_calls = 0;

   VLOG(2) << "NSAC stats: full_nsac=" << num_full_nsac_calls
           << ", revise=" << num_revise_calls;
   ```

2. ✅ 修复 `full_NSAC()` 层级传递 Bug
   - 修改 [src/solver/cpu/NSAC.cpp:61](../../src/solver/cpu/NSAC.cpp#L61)
   - 传递 `test_level` 而非 `level`

3. ✅ 添加重复队列检测
   - 使用 `std::vector<bool> in_queue` 避免重复

4. ✅ 测试验证
   ```bash
   ./build/cpim_test_parser --bench_path=tests/data/bench/queens-4_ext.xml --ac_algorithm=NSAC
   # 预期: <100ms (而非 >10s)
   ```

**预期结果**: NSAC 在 Queens-4 上运行时间 <100ms

---

### Phase 2: 优化 SAC1 (3-5 天)

**目标**: 实现 SAC1_Optimized，减少 50% 回溯测试

**任务**:
1. ✅ 创建 `SAC1_Optimized` 类
   - 文件: `src/solver/cpu/SAC1_Optimized.cpp`
   - 继承: `class SAC1_Optimized : public SAC1`

2. ✅ 实现快速支持检测
   ```cpp
   bool quick_support_test(IntVar* x, int a);
   ```

3. ✅ 添加统计信息
   ```cpp
   struct SACStats {
     int total_values;
     int quick_rejected;
     int full_tested;
     int removed;
   };
   ```

4. ✅ 集成到 `ACAlgorithm` 枚举
   ```cpp
   enum ACAlgorithm {
     // ...
     AC_SAC1_OPT,  // 新增
   };
   ```

5. ✅ 测试对比
   ```bash
   python3 tests/python/compare_ac_algorithms.py \
     --tier=0 \
     --algorithms=AC3bit,SAC1,SAC1_OPT
   ```

**预期结果**:
- SAC1_OPT 比 SAC1 快 2-3x
- Queens-4: SAC1 ~10ms → SAC1_OPT ~3ms

---

### Phase 3: 实现 SAC-SDS (1-2 周)

**目标**: 实现支持驱动的 SAC，显著减少测试次数

**任务**:
1. ✅ 设计支持计数数据结构
2. ✅ 实现 `count_supports()` 函数
3. ✅ 实现 `propagate_singleton_removals()`
4. ✅ 集成到 MAC 搜索
5. ✅ 大规模测试（TIER 1/2）

**预期结果**:
- Queens-4: 测试次数 < 100 (vs SAC1 的 3072)
- 困难实例（Langford-3-9）: SAC-SDS 能在合理时间内求解

---

### Phase 4: SAC-Opt 与并行化 (长期)

**目标**: 最先进的 SAC 实现，探索 GPU 加速

**任务**:
1. ✅ 实现值优先级队列
2. ✅ 实现贪心测试策略
3. ✅ 探索 CPU 多线程并行（OpenMP）
4. ✅ 探索 GPU 并行测试（CUDA）

**预期结果**:
- CPU 多线程: 4-8x 加速（4-8 核）
- GPU 并行: 10-50x 加速（困难实例）

---

## 4. 测试计划

### 4.1 单元测试

**测试文件**: `tests/cpp/test_sac_optimizations.cpp`

```cpp
TEST(SACTest, QuickSupportTest) {
  // 测试快速支持检测的正确性
}

TEST(SACTest, SAC1_vs_SAC1_Optimized_Correctness) {
  // 确保优化版本产生相同的结果
}

TEST(SACTest, NSAC_Performance) {
  // Queens-4 应该 <100ms
}
```

### 4.2 性能基准测试

**测试脚本**: `tests/python/benchmark_sac.py`

```python
#!/usr/bin/env python3
"""SAC 算法性能对比测试"""

ALGORITHMS = ["AC3bit", "NSAC", "SAC1", "SAC1_OPT", "SAC_SDS"]
INSTANCES = [
    "queens-4_ext.xml",
    "queens-8_ext.xml",
    "langford-2-4_ext.xml",
    "langford-3-9_ext.xml",
]

def benchmark():
    results = {}
    for alg in ALGORITHMS:
        for inst in INSTANCES:
            time, nodes = run_solver(alg, inst)
            results[(alg, inst)] = (time, nodes)

    # 生成表格报告
    print_table(results)
```

**预期输出**:
```
====================================================================
SAC 算法性能对比
====================================================================
实例                  AC3bit    NSAC      SAC1      SAC1_OPT  SAC_SDS
--------------------------------------------------------------------
queens-4_ext          <1ms      50ms      10ms      3ms       2ms
                      P=5/N=1   P=4/N=0   P=4/N=0   P=4/N=0   P=4/N=0
queens-8_ext          2ms       500ms     200ms     60ms      40ms
langford-3-9_ext      50ms      TIMEOUT   5s        1.5s      800ms
====================================================================
```

---

## 5. 关键代码示例

### 5.1 SAC1_Optimized 完整实现框架

```cpp
// include/Solver.h
class SAC1_Optimized : public SAC1 {
 public:
  SAC1_Optimized(Network* n, ACAlgorithm a);
  bool enforce(vector<IntVar*> x_evt, const int level) override;

  struct Stats {
    int total_values = 0;
    int quick_rejected = 0;
    int full_tested = 0;
    int removed = 0;

    void print() const {
      LOG(INFO) << "SAC1_Optimized stats:";
      LOG(INFO) << "  Total values: " << total_values;
      LOG(INFO) << "  Quick rejected: " << quick_rejected
                << " (" << (100.0 * quick_rejected / total_values) << "%)";
      LOG(INFO) << "  Full tested: " << full_tested
                << " (" << (100.0 * full_tested / total_values) << "%)";
      LOG(INFO) << "  Removed: " << removed;
    }
  };

  const Stats& stats() const { return stats_; }

 private:
  AC3bit* ac_bit_;  // 强制类型为 AC3bit 以访问 bitSup_
  Stats stats_;

  bool quick_support_test(IntVar* x, int a, const int level);
};
```

```cpp
// src/solver/cpu/SAC1_Optimized.cpp
#include "Solver.h"

namespace cpim {

SAC1_Optimized::SAC1_Optimized(Network* n, ACAlgorithm a) : SAC1(n, a) {
  // 确保使用 AC3bit 或其子类
  ac_bit_ = dynamic_cast<AC3bit*>(ac_);
  if (!ac_bit_) {
    LOG(FATAL) << "SAC1_Optimized requires AC3bit or subclass";
  }
}

bool SAC1_Optimized::quick_support_test(IntVar* x, int a, const int level) {
  // 对值 (x, a) 进行快速支持检测
  for (auto c : n_->subscription[x]) {
    IntConVal cv(c, x, a);

    // 使用 AC3bit 的 seek_support() 检查是否有支持
    if (!ac_bit_->seek_support(cv, level)) {
      return false;  // 无支持
    }
  }

  return true;  // 所有约束都有支持
}

bool SAC1_Optimized::enforce(vector<IntVar*> x_evt, const int level) {
  // 1. 初始 AC 传播
  ConsistencyState cs = ac_->enforce(n_->vars, level);
  bool result = cs.state;
  del_ += cs.num_delete;
  x_evt_.clear();

  if (!result) return false;

  // 2. SAC 固定点迭代
  bool modified = false;
  do {
    modified = false;

    for (auto x : n_->vars) {
      for (auto a : x->values()) {
        if (x->have(a)) {
          ++stats_.total_values;

          // **优化 1: 快速支持检测**
          if (!quick_support_test(x, a, level)) {
            ++stats_.quick_rejected;
            ++stats_.removed;
            ++del_;

            x->RemoveValue(a);
            x_evt_.push_back(x);
            cs = ac_->enforce(x_evt_, level);
            result = cs.state;
            del_ += cs.num_delete;
            x_evt_.clear();

            if (!result) return false;

            modified = true;
            continue;  // 跳过完整测试
          }

          // **优化 2: 完整测试（仅对快速检测通过的值）**
          ++stats_.full_tested;

          n_->trail()->NewLevel();
          const int test_level = n_->trail()->CurrentLevel();

          x->ReduceTo(a);
          x->assign(true);
          x_evt_.push_back(x);
          result = ac_->enforce(x_evt_, test_level).state;
          x_evt_.clear();
          x->assign(false);

          n_->trail()->BacktrackTo(test_level - 1);

          if (!result) {
            ++stats_.removed;
            ++del_;

            x->RemoveValue(a);
            x_evt_.push_back(x);
            cs = ac_->enforce(x_evt_, level);
            result = cs.state;
            del_ += cs.num_delete;
            x_evt_.clear();

            if (!result) return false;

            modified = true;
          }
        }
      }
    }
  } while (modified);

  // 3. 输出统计信息
  if (VLOG_IS_ON(1)) {
    stats_.print();
  }

  return true;
}

}  // namespace cpim
```

---

## 6. 性能预测

### 6.1 理论分析

| 算法 | 测试次数 (Queens-4) | 每次测试成本 | 总时间估算 |
|------|-------------------|------------|----------|
| **SAC1** | 16 (4×4) | AC传播 ≈ 1ms | ~16ms |
| **SAC1_Optimized** | 快速拒绝 ~8<br>完整测试 ~8 | 快速: 0.01ms<br>完整: 1ms | ~8ms |
| **NSAC** | 16 × 邻域 (3) = 48 | AC传播 ≈ 0.3ms | ~15ms |
| **SAC-SDS** | ~5 (基于支持计数) | AC传播 ≈ 1ms | ~5ms |

### 6.2 实测目标

**Queens-4** (n=4, d=4, e=12):
- AC3bit: <1ms (基线)
- NSAC: <50ms (修复后)
- SAC1: <20ms
- SAC1_Optimized: <10ms
- SAC-SDS: <5ms

**Langford-3-9** (n=27, d=9, e=72):
- AC3bit: ~50ms
- NSAC: <5s (修复后)
- SAC1: <30s
- SAC1_Optimized: <10s
- SAC-SDS: <3s

---

## 7. 相关文献

1. **Bessière, C. (2006)**. *Constraint Propagation*. Handbook of Constraint Programming.
   - 经典教材，SAC 算法理论基础

2. **Bessière, C., & Debruyne, R. (2005)**. *Optimal and Suboptimal Singleton Arc Consistency Algorithms*. IJCAI 2005.
   - SAC1, SAC2, SAC3 算法详解

3. **Lecoutre, C., & Cardon, S. (2005)**. *A Greedy Approach to Establish Singleton Arc Consistency*. CP 2005.
   - SAC-Opt 贪心优化

4. **Bessière, C., Cardon, S., et al. (2008)**. *In the Quest of the Best Form of Local Consistency for Weighted CSP*. IJCAI 2008.
   - SAC-SDS 支持驱动方法

5. **Wallace, R. J. (2015)**. *Neighbourhood Singleton Arc Consistency*. AI Communications.
   - NSAC 算法及其优化

---

## 8. 总结

### 当前架构优势
1. ✅ **UnifiedTrail 支持嵌套回溯** - SAC 的核心需求
2. ✅ **AC3bit 位向量优化** - 可直接复用
3. ✅ **模块化设计** - 易于扩展新算法

### 推荐实施顺序
1. **立即** (1-2 天): 修复 NSAC 性能问题 → 验证架构正确性
2. **短期** (1 周): 实现 SAC1_Optimized → 快速收益
3. **中期** (2-3 周): 实现 SAC-SDS → 显著性能提升
4. **长期** (1-2 月): SAC-Opt + 并行化 → 研究级性能

### 风险与挑战
- ⚠️ SAC 本质上是 NP-难问题，某些实例可能永远很慢
- ⚠️ 需要仔细测试正确性（SAC 的语义很复杂）
- ⚠️ 并行化需要处理 Trail 的线程安全问题

---

**文档版本**: 1.0
**最后更新**: 2025-12-22
**审阅状态**: 待用户审阅
