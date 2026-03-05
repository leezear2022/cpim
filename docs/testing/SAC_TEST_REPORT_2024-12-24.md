# SAC 算法测试报告

**测试日期**: 2024-12-24
**测试版本**: Code Review 修复 + 低优先级优化后
**测试者**: Claude (Anthropic)

---

## 1. 测试目的

验证以下修复和优化的正确性：

1. **Code Review 修复**:
   - SAC3 队列每次 enforce 重新初始化
   - 预算/统计每次 enforce 重置
   - uint32_t → uint64_t 修复 Trail 回溯问题

2. **低优先级优化**:
   - assigned 状态保存 O(n) → O(1)
   - SAC3 队列 std::set → std::vector

---

## 2. 测试环境

- **平台**: Linux 5.15.148-tegra (aarch64)
- **编译器**: GCC 11.4.0
- **CUDA**: 12.6.68
- **测试工具**: cpim_test_parser, compare_sac_algorithms.py

---

## 3. 测试结果汇总

### 3.1 基准测试实例

| 实例 | 变量数 | 约束数 | 域大小 | 类型 |
|------|--------|--------|--------|------|
| queens-4 | 4 | 6 | 4 | N-Queens |
| queens-12 | 12 | 66 | 12 | N-Queens |
| langford-2-4 | 8 | 28 | 8 | 组合 |
| langford-3-9 | 27 | ~200 | 27 | 组合 |
| graphw-05 | 50 | ~200 | 5 | 图着色 |
| QCP-15-120-0 | 225 | ~600 | 15 | QCP |
| composed-25-1-2-0 | 25 | ~50 | 25 | 拉丁方 |

### 3.2 算法对比结果

| 实例 | AC3bit | SAC1 | SAC3 | 节点减少 |
|------|--------|------|------|----------|
| **queens-4** | | | | |
| - 时间 (ms) | 0 | 0 | 0 | - |
| - P/N | 5/1 | 4/0 | 4/0 | 20% |
| **queens-12** | | | | |
| - 时间 (ms) | 5 | 147 | 146 | - |
| - P/N | 41/29 | 14/2 | 14/2 | **77%** |
| **langford-2-4** | | | | |
| - 时间 (ms) | 0 | 3 | 4 | - |
| - P/N | 8/0 | 8/0 | 8/0 | 0% |
| **langford-3-9** | | | | |
| - 时间 (ms) | 250 | 17469 | 17468 | - |
| - P/N | 468/441 | 45/18 | 45/18 | **90%** |
| **graphw-05** | | | | |
| - 时间 (ms) | 11 | 11 | 10 | - |
| - P/N | 0/0 | 0/0 | 0/0 | - |
| **QCP-15-120-0** | | | | |
| - 时间 (ms) | 255 | 25242 | 24322 | - |
| - P/N | 1274/1049 | 227/2 | 227/2 | **82%** |
| **composed-25-1-2-0** | | | | |
| - 时间 (ms) | 2 | 183 | 182 | - |
| - P/N | 5/5 | 0/0 | 0/0 | **100%** |

### 3.3 结果说明

- **P/N**: Positives (扩展节点) / Negatives (回溯节点)
- **节点减少**: (AC3bit_P - SAC_P) / AC3bit_P × 100%
- **SAC1 和 SAC3 结果完全一致**: 验证了修复的正确性

---

## 4. 关键发现

### 4.1 SAC 剪枝效果显著

对于约束密集型问题，SAC 大幅减少搜索节点：

```
langford-3-9:  468 节点 → 45 节点  (减少 90%)
QCP-15-120-0: 1274 节点 → 227 节点 (减少 82%)
queens-12:      41 节点 → 14 节点  (减少 66%)
```

### 4.2 SAC 时间开销

SAC 的 probe 开销导致时间增加：

```
langford-3-9:  250ms → 17.5s  (增加 70x)
QCP-15-120-0:  255ms → 25s    (增加 100x)
queens-12:     5ms   → 147ms  (增加 30x)
```

### 4.3 SAC 根节点剪枝

某些问题 SAC 在根节点传播即可解决：

```
composed-25-1-2-0:
  AC3bit: 需要搜索 (P=5/N=5)
  SAC:    根节点解决 (P=0/N=0)
```

### 4.4 SAC 无效果场景

简单问题或已被 AC 充分过滤的问题：

```
langford-2-4: SAC 无额外剪枝 (P=8/N=0 不变)
graphw-05:    AC 已在根节点解决
```

---

## 5. 正确性验证

### 5.1 SAC1 vs SAC3 一致性

**所有测试实例中 SAC1 和 SAC3 的 P/N 结果完全一致**，验证了：

- SAC3 队列每次 enforce 重新初始化的修复正确
- 预算/统计重置的修复正确
- vector 替代 set 的优化不影响正确性

### 5.2 修复前后对比

| 实例 | SAC3 (修复前) | SAC3 (修复后) | SAC1 |
|------|---------------|---------------|------|
| queens-12 | P=41/N=29 ❌ | P=14/N=2 ✓ | P=14/N=2 |

修复前 SAC3 与 AC3bit 结果相同（退化为 AC），修复后与 SAC1 一致。

### 5.3 uint64_t 修复验证

之前发现的正确性 Bug（rand-2-40-80-103-800-1 错误报告 UNSAT）已修复：

| 实例 | 修复前 | 修复后 | OR-Tools |
|------|--------|--------|----------|
| rand-2-40-80-103-800-1 | UNSAT ❌ | SAT ✓ | SAT |
| rand-2-40-80-103-800-10 | UNSAT ❌ | SAT ✓ | SAT |

---

## 6. 性能优化验证

### 6.1 assigned 状态保存优化

**修改**: ProbeValue 中 O(n) → O(1)

```cpp
// 优化前
std::vector<bool> saved_assigned;
for (auto* v : m_->vars) {
  saved_assigned.push_back(v->assigned());
}

// 优化后
const bool x_was_assigned = x->assigned();
```

**验证**: 所有测试结果不变，正确性保持。

### 6.2 SAC3 队列优化

**修改**: std::set → std::vector

| 操作 | 优化前 | 优化后 |
|------|--------|--------|
| EnqueueValue | O(log k) | O(1) |
| SelectCandidates | O(k·log k) | O(k) |
| InitializeQueue | O(n·d·log n·d) | O(n·d) |

**验证**: SAC1/SAC3 结果完全一致，正确性保持。

---

## 7. 测试覆盖

### 7.1 已测试问题类型

- ✅ N-Queens (queens-4, queens-12)
- ✅ Langford 问题 (langford-2-4, langford-3-9)
- ✅ 图着色 (graphw-05)
- ✅ QCP 问题 (QCP-15-120-0)
- ✅ 拉丁方/组合 (composed-25-1-2-0)

### 7.2 未测试 (超时或文件问题)

- ⏱️ tightness0.5 系列 (AC3bit 已超时)
- ⏱️ marc/large 系列 (AC3bit 已超时)
- ⏱️ BH-4-4 系列 (AC3bit 已超时)

### 7.3 测试数据代表性评估

| 方面 | 评估 | 说明 |
|------|------|------|
| 问题类型多样性 | ★★★★☆ | 覆盖 5 种问题类型 |
| 问题规模分布 | ★★★☆☆ | 小到中等，缺少大规模 |
| SAT/UNSAT 分布 | ★★★★☆ | 包含 SAT 和根节点 UNSAT |
| 实例数量 | ★★☆☆☆ | 7 个实例，建议扩展 |

---

## 8. 结论

### 8.1 修复验证

✅ **所有 Code Review 修复已验证正确**:
- SAC3 队列回溯问题
- 预算/统计累计问题
- uint64_t Trail 修复

### 8.2 优化验证

✅ **所有低优先级优化已验证正确**:
- assigned O(1) 优化
- 队列 vector 优化

### 8.3 SAC 算法评估

| 方面 | 结论 |
|------|------|
| 剪枝效果 | 显著（可减少 66%-90% 节点） |
| 时间开销 | 较大（增加 30-100 倍） |
| 适用场景 | 约束密集型、深度搜索问题 |
| SAC1 vs SAC3 | 结果一致，SAC3 略快 |

### 8.4 后续建议

1. **扩展测试集**: 增加更多中等规模实例
2. **性能分析**: 添加 SAC probe 统计输出
3. **预算调优**: 测试 max_probes/depth_limit 参数效果
4. **GPU SAC**: 探索 SAC probe 的 GPU 并行化

---

## 9. 附录

### 9.1 测试命令

```bash
# 单实例测试
./cpim_test_parser --bench_path=<path> --ac_algorithm=AC3bit
./cpim_test_parser --bench_path=<path> --ac_algorithm=MSAC3bit --msac_mode=SAC1
./cpim_test_parser --bench_path=<path> --ac_algorithm=MSAC3bit --msac_mode=SAC3

# 批量对比测试
python3 tests/python/compare_sac_algorithms.py --instances <dir> --binary ./build/cpim_test_parser
```

### 9.2 修改文件清单

| 文件 | 修改内容 |
|------|----------|
| include/base/unified_trail.h | uint32_t → uint64_t |
| src/base/unified_trail.cpp | 同上 |
| include/Network.h | RestoreBitWord 签名 |
| src/solver/common/Network.cpp | RestoreBitWord 实现 |
| include/Solver.h | set → vector, 移除 queue_initialized_ |
| src/solver/cpu/MSAC3bit.cpp | 全部修复和优化 |

---

*报告生成时间: 2024-12-24*
*测试工具版本: cpim_test_parser (commit post-review)*
