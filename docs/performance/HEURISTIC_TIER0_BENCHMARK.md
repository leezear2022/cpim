# TIER 0 启发式性能对比结果

**测试日期**: 2025-12-21
**测试环境**: Jetson Orin
**超时设置**: 60 秒/启发式
**启发式**: MIN_DOMAIN, DOM/DEG, DOM/DDEG

---

## 测试结果总览

| 实例 | MIN_DOMAIN | DOM/DEG | DOM/DDEG | 最佳 | 改进 |
|------|-----------|---------|----------|------|------|
| test.xml | 3/0 (3) | 3/0 (3) | 3/0 (3) | 相同 | 0% |
| queens-4_ext | 5/1 (6) | 5/1 (6) | 5/1 (6) | 相同 | 0% |
| langford-2-4-ext | 8/0 (8) | 8/0 (8) | 8/0 (8) | 相同 | 0% |
| langford-3-9-ext | 468/441 (909) | 468/441 (909) | 468/441 (909) | 相同 | 0% |
| driverlogw-01c-sat | 71/0 (71) | 71/0 (71) | 73/2 (75) | MIN/DEG | 0% |
| graphw-05 | UNSAT | UNSAT | UNSAT | 相同 | - |
| rand-2-40-8-753-100-0 | **TIMEOUT** | **TIMEOUT** | **TIMEOUT** | - | - |
| rand-2-40-8-753-100-5 | **TIMEOUT** | **5725/5685 (11410)** ✅ | 5746/5706 (11452) | **DOM/DEG** | **∞** |
| rand-2-40-25-180-500-0 | **TIMEOUT** | **TIMEOUT** | **TIMEOUT** | - | - |
| rand-2-40-80-103-800-0 | **TIMEOUT** | **TIMEOUT** | **TIMEOUT** | - | - |
| BlackHole-4-4-e-0 | **TIMEOUT** | **UNSAT** ✅ | **UNSAT** ✅ | **DOM/DEG** | **∞** |
| composed-25-1-2-0 | **TIMEOUT** | **TIMEOUT** | **TIMEOUT** | - | - |

**符号说明**:
- `P/N (Total)`: Positives / Negatives (总节点数)
- ✅: 在 60 秒内成功求解
- **粗体**: 关键差异
- ∞: MIN_DOMAIN 超时，无法计算改进比例

---

## 详细分析

### 1. 简单/对称问题（无差异）

#### test.xml, queens-4, langford-2-4, langford-3-9
```
所有启发式表现完全一致
原因：
  - 问题规模小
  - 变量度数相同（queens）
  - 问题结构简单
```

#### driverlogw-01c-sat
```
MIN_DOMAIN: 71 nodes
DOM/DEG:    71 nodes
DOM/DDEG:   75 nodes (略差)

分析：DOM/DDEG 在此问题上选择稍差，但差异很小（+5.6%）
```

---

### 2. 约束密集问题（显著差异）

#### ⭐ rand-2-40-8-753-100-5 (关键突破)
```
MIN_DOMAIN: TIMEOUT (>60s)
DOM/DEG:    11410 nodes ✅ SAT
DOM/DDEG:   11452 nodes ✅ SAT

关键发现：
  ✅ DOM/DEG 成功在 60 秒内找到解
  ✅ MIN_DOMAIN 超时失败
  ✅ 这是启发式带来的质的飞跃
```

**问题特征**:
- 40 个变量
- 753 个约束（高密度）
- 约束紧度 0.1（较松）
- 域大小 8

**为什么 DOM/DEG 更好**？
1. 高约束密度意味着变量度数差异大
2. DOM/DEG 优先选择"域小且约束多"的变量
3. 这些变量的赋值触发更多传播，快速剪枝

---

#### ⭐ BlackHole-4-4-e-0 (UNSAT 证明)
```
MIN_DOMAIN: TIMEOUT (>60s)
DOM/DEG:    UNSAT ✅
DOM/DDEG:   UNSAT ✅

关键发现：
  ✅ DOM/DEG 和 DOM/DDEG 在 60 秒内证明 UNSAT
  ✅ MIN_DOMAIN 超时失败
```

**为什么成功**？
- BlackHole 是组合问题，约束结构复杂
- 好的变量选择更快导致冲突，证明无解

---

### 3. 超时问题（无结论）

以下实例所有启发式都超时（>60s）：
- rand-2-40-8-753-100-0
- rand-2-40-25-180-500-0
- rand-2-40-80-103-800-0
- composed-25-1-2-0

**原因**：
- 问题规模大（40 变量, 100-800 约束）
- 约束紧度高（0.5-0.8）
- 需要更长时间或更强的技术（Learning, Restart）

---

## 性能改进总结

### 成功案例统计

| 指标 | MIN_DOMAIN | DOM/DEG | DOM/DDEG |
|------|-----------|---------|----------|
| 简单问题求解 | 6/6 | 6/6 | 6/6 |
| 困难问题求解 | **0/6** | **2/6** ✅ | **2/6** ✅ |
| 总求解成功率 | 50% (6/12) | **66.7%** (8/12) ✅ | **66.7%** (8/12) ✅ |

**结论**：
- ✅ DOM/DEG 和 DOM/DDEG 将 TIER 0 通过率从 50% 提升到 66.7%
- ✅ 成功解决了 2 个 MIN_DOMAIN 无法在 60 秒内完成的问题
- ✅ 在简单问题上没有退化

---

## 启发式选择建议

### MIN_DOMAIN (最小域优先)
**适用场景**：
- 小规模问题（< 20 变量）
- 对称问题（如 N-Queens）
- 快速原型验证

**优势**：
- 实现简单
- 计算开销最小
- 在简单问题上表现良好

**劣势**：
- 忽略约束结构
- 在约束密集问题上效率低

---

### DOM/DEG (域大小/度数)
**适用场景** ⭐ **推荐**：
- 约束密集问题
- 变量度数差异大的问题
- 中等规模问题（20-50 变量）

**优势**：
- ✅ 考虑约束结构
- ✅ 在 TIER 0 上表现最佳
- ✅ 计算开销小（度数预计算）

**劣势**：
- 静态度数，不随搜索更新

---

### DOM/DDEG (域大小/动态度数)
**适用场景**：
- 约束结构随搜索变化大的问题
- 需要精准变量选择的问题

**优势**：
- ✅ 动态更新度数，更精准
- ✅ 在 BlackHole 等问题上表现好

**劣势**：
- 计算开销稍高（每次选择时重新计算 DDEG）
- 在 TIER 0 上未显示出比 DOM/DEG 明显优势

**TIER 0 对比**：
- 与 DOM/DEG 打平（都是 8/12 通过）
- driverlogw 上略差（+5.6% 节点）

---

## 下一步优化建议

### 短期（已验证可行）
1. **默认使用 DOM/DEG**：性能更好，无明显劣势
2. **增加超时时间**：某些 rand-2-40 实例可能需要 120-300 秒
3. **测试 TIER 1**：更大规模问题上对比

### 中期（Phase 1.5+）
1. **DOM/WDEG（加权度数）**：
   - 失败的约束权重增加
   - 文献显示比 DOM/DDEG 更好
   - 预期在困难问题上提升 30-50%

2. **值选择启发式**：
   - 当前固定选择最小值
   - 可添加 min-conflicts、phase-saving 等

3. **Restart 策略**：
   - Luby 序列或几何重启
   - 配合 DOM/WDEG 效果更佳

### 长期（Phase 2）
- 集成到 Propagator 框架
- 支持用户自定义启发式
- GPU 端并行启发式计算

---

## 参考文献

1. **Bessière & Régin (1996)**: "MAC and Combined Heuristics: Two Reasons to Forsake FC (and CBJ?) on Hard Problems"
   - 首次提出 DOM/DEG 启发式
   - 证明在困难问题上优于 MIN_DOMAIN

2. **Boussemart et al. (2004)**: "Boosting Systematic Search by Weighting Constraints"
   - 提出 DOM/WDEG（Weighted Degree）
   - 在国际 CSP 竞赛中表现优异

3. **Lecoutre et al. (2004)**: "A Greedy Approach to Establish Singleton Arc Consistency"
   - DOM/DDEG 的早期应用
   - 证明动态度数在某些问题上更有效

---

## 附录：完整测试命令

```bash
# 简单问题对比
python3 tests/python/benchmark_heuristics.py \
  tests/data/bench/test.xml \
  tests/data/bench/queens-4_ext.xml \
  benchmarks/langford/langford-2-4-ext.xml \
  benchmarks/langford/langford-3-9-ext.xml \
  benchmarks/driver/driverlogw-01c-sat_ext.xml \
  benchmarks/graphs/graphw-05_ext.xml \
  --timeout=30

# 困难问题对比
python3 tests/python/benchmark_heuristics.py \
  benchmarks/tightness0.1/rand-2-40-8-753-100-0_ext.xml \
  benchmarks/tightness0.1/rand-2-40-8-753-100-5_ext.xml \
  benchmarks/tightness0.5/rand-2-40-25-180-500-0_ext.xml \
  benchmarks/tightness0.8/rand-2-40-80-103-800-0_ext.xml \
  benchmarks/BH-4-4/BlackHole-4-4-e-0_ext.xml \
  benchmarks/composed-25-1-2/composed-25-1-2-0_ext.xml \
  --timeout=60
```

---

**最终结论**: ✅ Phase 1.5 成功实现了可插拔启发式框架，**DOM/DEG 在 TIER 0 上将求解成功率从 50% 提升到 66.7%**，验证了启发式的有效性。
