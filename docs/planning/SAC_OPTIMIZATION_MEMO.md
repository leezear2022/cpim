# SAC 优化备忘录

## 文档信息
- **创建时间**: 2025-12-25
- **最后更新**: 2025-12-25
- **相关文档**:
  - [SAC_OPTIMIZATION_DESIGN.md](SAC_OPTIMIZATION_DESIGN.md) - 详细设计方案
  - [SAC_IMPLEMENTATION_REVIEW.md](../implementation/SAC_IMPLEMENTATION_REVIEW.md) - 实现评审

---

## 1. 当前状态总结

### 1.1 已完成的优化

| 优化项 | 完成日期 | 修改文件 | 效果 |
|--------|----------|----------|------|
| 统计信息修复 | 2025-12-24 | `MSAC3bit.cpp`, `Solver.h` | 正确报告全局 probe 数 |
| 启发式 Probe 排序 | 2025-12-24 | `MSAC3bit.cpp:SelectCandidates()` | 小域优先，更早发现失败 |
| SAC3 增量队列初始化 | 2025-12-25 | `MSAC3bit.cpp`, `unified_trail.h` | 减少初始队列 20-60% |
| SAC3 DFS 风格重构 | 2025-12-25 | `MSAC3bit.cpp:enforce()` | 符合原论文，SAC1/SAC3 分离 |

### 1.2 当前性能数据

**queens-12 基准测试**:
```
SAC1: 607 probes, 31 failed, P=14/N=2
SAC3: 653 probes, 31 failed, P=14/N=2
```

**langford-3-9 基准测试**:
```
SAC1: 9219 probes, 280 failed, P=45/N=18
SAC3: 9387 probes, 247 failed, P=45/N=18
```

### 1.3 发现的问题

**SAC3 probes 比 SAC1 多**:
- 原因：删除值后，邻域值被重新入队，导致已成功 probe 的值被重复检查
- 影响：queens-12 多 7.6% probes，langford-3-9 多 1.8%
- 解决方案：Phase 2.1 避免重复 Probe

---

## 2. 待优化项目

### Phase 2.1: 避免重复 Probe（推荐下一步）

**问题描述**:
```
当前行为：
1. Probe(A) 成功
2. Probe(B) 失败，删除 B
3. EnqueueNeighborhood(B) 将 A 重新入队
4. Probe(A) 再次执行（重复！）

期望行为：
- 如果 A 上次 probe 成功后，其支持集未变化，跳过重复 probe
```

**实现方案**:
```cpp
// 在 MSAC3bit 类中添加
struct ProbeCache {
  int trail_pos_at_success;  // 成功时的 Trail 位置
};
std::vector<std::vector<ProbeCache>> probe_cache_;  // [var_id][val]

// 在 probe 前检查
bool need_reprobe(IntVar* x, int a) {
  auto& cache = probe_cache_[x->id()][a];
  // 如果 Trail 位置未变（域未变），跳过
  return m_->trail()->Size() != cache.trail_pos_at_success;
}
```

**预期收益**: 减少 10-30% 重复 probes

**复杂度**: 中等（2-3 小时）

---

### Phase 2.2: 快速支持检测

**问题描述**:
- 当前每次 probe 都执行完整的 AC 传播
- 很多值明显无支持，可以快速检测并跳过

**实现方案**:
```cpp
bool quick_support_test(IntVar* x, int a) {
  for (auto c : m_->subscription[x]) {
    // 使用 AC3bit 的 bitSup_ 快速检查
    IntConVal cv(c, x, a);
    if (!kernel_->seek_support(cv, 0)) {
      return false;  // 无支持，直接删除
    }
  }
  return true;  // 可能有支持，需要完整 probe
}
```

**预期收益**: 快速拒绝 30-50% 无支持值

**复杂度**: 中等（需要暴露 AC3bit 内部接口）

---

### Phase 3: SAC-SDS（支持驱动的 SAC）

**核心思想**:
- 维护每个值的支持计数器
- 只测试支持数 = 0 的值
- 删除值时更新相关支持计数

**理论复杂度**:
- SAC1: O(n²d³) - 每轮全扫描
- SAC3: O(n²d²) - 增量队列
- SAC-SDS: O(删除数 × AC成本) - 只测试必要的值

**预期收益**: 测试次数减少 80%+

**复杂度**: 高（需要支持计数数据结构）

---

### Phase 4: GPU 并行 SAC

**核心思想**:
- 多个 probe 可以并行执行
- GPU 上批量测试值的一致性

**挑战**:
- Trail 的线程安全
- 回溯的并行化
- 内存同步开销

**预期收益**: 10-50x 加速（困难实例）

**复杂度**: 非常高

---

## 3. SAC1 vs SAC3 算法对比

### 3.1 SAC1（广度优先，全扫描）

```cpp
while (changed) {
  changed = false;
  for each (var, val) in 所有剩余值:  // ← 每轮全扫描
    if (Probe(var, val) fails):
      remove(val)
      changed = true
}
```

**特点**:
- 每轮检查所有剩余值
- 简单但可能有冗余工作
- 适合密集约束问题（如 N-Queens）

### 3.2 SAC3（深度优先，增量队列）

```cpp
queue = 所有值
while (queue not empty):
  (var, val) = queue.pop()
  if (Probe(var, val) fails):
    remove(val)
    EnqueueNeighbors(var)  // ← 只入队邻域
```

**特点**:
- 只检查可能受影响的值
- 需要精确的邻域追踪
- 适合稀疏约束问题

### 3.3 当前实现细节

**SAC3 使用 DFS（栈）顺序**:
```cpp
pending_queue_.push_back({x, a});  // 入队尾
auto [x, a] = pending_queue_.back();  // 取队尾
pending_queue_.pop_back();  // 出队尾
```
- LIFO 顺序：新入队的值优先检查
- 更快发现局部不一致

**增量初始化**:
```cpp
void InitializeQueueIncremental() {
  // 从 Trail 找出 AC 阶段修改的变量
  for (int i = ac_start_trail_pos_; i < trail->Size(); ++i) {
    modified_vars.insert(trail->GetEntry(i).var_id);
  }
  // 只入队修改变量及其邻域
  for (var in modified_vars):
    EnqueueValue(var, all_values)
    EnqueueNeighborhood(var)
}
```

---

## 4. 代码位置索引

### 核心文件

| 文件 | 功能 |
|------|------|
| `include/Solver.h:665-701` | MSAC3bit 类定义 |
| `include/Solver.h:383-418` | MSACStats 统计结构 |
| `src/solver/cpu/MSAC3bit.cpp:60-185` | enforce() 主循环（SAC1/SAC3 分支） |
| `src/solver/cpu/MSAC3bit.cpp:252-318` | InitializeQueueIncremental() |
| `include/base/unified_trail.h:115` | GetEntry() 接口 |

### 关键函数

```cpp
// SAC3 DFS 主循环
while (!pending_queue_.empty() && ShouldContinueProbe(level)) {
  auto [x, a] = pending_queue_.back();
  pending_queue_.pop_back();
  // ... probe logic
}

// SAC1 BFS 主循环
while (changed && ShouldContinueProbe(level)) {
  vector<pair<IntVar*, int>> candidates;
  SelectCandidates(candidates, level);  // 全扫描
  for (auto& [x, a] : candidates) {
    // ... probe logic
  }
}
```

---

## 5. 测试命令

### 基本测试
```bash
cd /home/lee/Codes/cpim/build

# SAC1 测试
./cpim_test_parser --bench_path=../tests/data/bench/queens-12_ext.xml \
  --ac_algorithm=MSAC3bit --msac_mode=SAC1 --msac_verbose_stats

# SAC3 测试
./cpim_test_parser --bench_path=../tests/data/bench/queens-12_ext.xml \
  --ac_algorithm=MSAC3bit --msac_mode=SAC3 --msac_verbose_stats

# 详细日志
GLOG_v=1 ./cpim_test_parser --bench_path=../tests/data/bench/queens-12_ext.xml \
  --ac_algorithm=MSAC3bit --msac_mode=SAC3
```

### 对比测试
```bash
# SAC1 vs SAC3 对比
echo "=== SAC1 ===" && ./cpim_test_parser ... --msac_mode=SAC1 2>&1 | grep "Total probes"
echo "=== SAC3 ===" && ./cpim_test_parser ... --msac_mode=SAC3 2>&1 | grep "Total probes"
```

---

## 6. 下一步行动

### 推荐顺序

1. **Phase 2.1: 避免重复 Probe**（2-3 小时）
   - 解决 SAC3 probes 比 SAC1 多的问题
   - 不需要大改架构

2. **Phase 2.2: 快速支持检测**（3-4 小时）
   - 需要暴露 AC3bit::seek_support() 接口
   - 可以与 Phase 2.1 并行开发

3. **Phase 3: SAC-SDS**（1-2 天）
   - 需要设计支持计数数据结构
   - 显著性能提升

4. **Phase 4: GPU 并行**（长期）
   - 研究级工作
   - 需要解决 Trail 线程安全

### 验收标准

| Phase | 验收标准 |
|-------|----------|
| 2.1 | SAC3 probes <= SAC1 probes |
| 2.2 | 快速拒绝率 > 30% |
| 3 | 测试次数减少 > 50% |
| 4 | 困难实例加速 > 10x |

---

## 7. 参考文献

1. Bessière, C., & Debruyne, R. (2005). *Optimal and Suboptimal Singleton Arc Consistency Algorithms*. IJCAI 2005.
2. Bessière, C., et al. (2008). *In the Quest of the Best Form of Local Consistency for Weighted CSP*. IJCAI 2008.
3. Lecoutre, C., & Cardon, S. (2005). *A Greedy Approach to Establish Singleton Arc Consistency*. CP 2005.

---

**文档版本**: 1.0
**状态**: 活跃开发中
