# CPIM 项目工作总结 (2025-12-21)

## 项目概述

**CPIM** (Constraint Programming on GPU) 是一个 CUDA 加速的约束满足问题 (CSP) 求解器，实现了多种弧一致性算法，支持 CPU 和 GPU 混合求解。

- **仓库**: https://github.com/leezear2022/cpim
- **主要语言**: C++17, CUDA 11.0+
- **核心算法**: MAC (Maintaining Arc Consistency) + 多种 AC 算法（AC3bit, RPC3, lMaxRPC, NSAC, SAC）
- **输入格式**: XCSP3 (XML-based CSP 格式)
- **构建系统**: CMake + Ninja/Make

---

## 当前会话完成的工作

### 1. Propagator 框架实验与决策 (Phase 2.1)

#### 背景
最初尝试实现一个通用的 Propagator 框架（受 Gecode/OR-Tools 启发），将约束传播抽象化。

#### 实现内容
- ✅ 定义了 `Propagator` 抽象接口 ([include/solver/common/propagator.h](../../include/solver/common/propagator.h))
- ✅ 实现了 `PropagationEngine` 调度器（优先级队列，增量传播）
- ✅ 迁移 AC3bit 逻辑到 `TableConstraintPropagator`（保持位向量优化）
- ✅ 集成到 MAC 搜索器（可选路径，`--use_propagator_framework` 参数）
- ✅ 编译通过，基本功能验证

#### 关键发现
经过分析现有的强一致性算法（RPC3, lMaxRPC, NSAC, SAC），发现 **Propagator 框架不适合**：

1. **不兼容全局推理算法**:
   - RPC3/lMaxRPC 需要维护变量对之间的支持关系 (`r_1_`, `r_2_`)
   - SAC 需要嵌套 GAC 调用 + 试探性赋值
   - NSAC 需要邻域约束分析 (`common_neibor_`)
   - 这些都是 **全局推理**，而非局部传播

2. **现有 AC 基类层次结构已经完美**:
   ```cpp
   AC (基类)
   ├── AC3 - 基础弧一致性
   ├── AC3bit - 位向量优化（二元约束）
   ├── RPC3 - 关系路径一致性（继承 AC3bit）
   ├── lMaxRPC - 轻量级 MaxRPC（继承 AC3bit）
   ├── NSAC - 邻域单例 AC（继承 AC3bit）
   └── SAC1/SAC3 - 单例 AC（包装 AC 实例）
   ```

3. **用户真实需求**:
   - 不是构建通用 Propagator 框架
   - 而是 **对比 GPU 与不同 CPU 一致性算法的性能**

#### 决策与行动
- ✅ **保存** Propagator 框架实现到 `experimental/propagator-framework` 分支
- ✅ **移除** Propagator 框架，回归 AC 算法层次结构
- ✅ 专注于优化现有算法和构建对比测试框架

**相关 Commits**:
- `6e6851f` - feat: Phase 1.5 + 2.1 - Variable Heuristics & Propagator Framework
- `2ab67c5` - revert: Remove Propagator framework - return to AC algorithm hierarchy

---

### 2. AC 算法对比测试框架 (选项 1 实施)

#### 目标
构建灵活的测试框架，支持对比不同 CPU 一致性算法与 GPU 的性能。

#### 实现内容

**2.1 CPU 求解器算法选择功能**

修改 [apps/cpim_test_parser.cpp](../../apps/cpim_test_parser.cpp):
- ✅ 添加 `--ac_algorithm` 命令行参数
- ✅ 实现 `ParseACAlgorithm()` 字符串→枚举转换
- ✅ 支持算法: AC3, AC3bit, RPC3, lMaxRPC, NSAC, SAC1, SAC3
- ✅ 默认 AC3bit（保持向后兼容）

```bash
# 用法示例
./build/cpim_test_parser --bench_path=tests/data/bench/queens-4_ext.xml --ac_algorithm=RPC3
```

**输出示例**:
```
I1221 23:52:53.784559 304506 cpim_test_parser.cpp:202] Using AC algorithm: RPC3
I1221 23:52:53.784945 304506 cpim_test_parser.cpp:255] MAC solution (canonical indices): 1 3 0 2
I1221 23:52:53.784955 304506 cpim_test_parser.cpp:256] MAC solution (original values): 2 4 1 3
I1221 23:52:53.784993 304506 cpim_test_parser.cpp:264] MAC stats: time=0 ms, positives=4, negatives=0, nodes=0, timeout=false
```

**2.2 AC 算法自动化对比测试脚本**

创建 [tests/python/compare_ac_algorithms.py](../../tests/python/compare_ac_algorithms.py):
- ✅ 自动运行多种 AC 算法对比测试
- ✅ 表格化输出节点数、求解时间、一致性检查
- ✅ 支持 TIER 分层测试（快速烟测/标准回归/完整测试）
- ✅ 验证解的一致性（不同算法应得到相同解）

```bash
# 用法示例
python3 tests/python/compare_ac_algorithms.py --tier=0 --algorithms=AC3bit,RPC3,lMaxRPC
```

**输出示例**（预期格式）:
```
====================================================================================================
TIER 0 AC 算法对比测试 (12 个实例)
算法: AC3bit, RPC3, lMaxRPC
====================================================================================================

实例                                      状态         AC3bit       RPC3         lMaxRPC      NSAC         结果
----------------------------------------------------------------------------------------------------
queens-4_ext                             SAT          P5+N1=6      P4+N0=4      P4+N0=4      P4+N0=4      ✓
langford-3-9-ext                         SAT          P45+N12=57   P38+N8=46    P38+N8=46    TIMEOUT      ✓
...

====================================================================================================
测试总结: 10 通过, 0 失败, 2 跳过
====================================================================================================
```

**相关 Commit**:
- `08243ef` - feat: Add AC algorithm comparison framework

---

### 3. AC 算法性能分析

#### Queens-4 实例测试结果

| 算法 | 一致性强度 | 正向节点 | 回溯节点 | 总节点数 | 性能提升 | 状态 |
|------|-----------|---------|---------|---------|---------|------|
| **AC3bit** | GAC (广义弧一致) | 5 | 1 | 6 | 基线 | ✅ 正常 |
| **RPC3** | 关系路径一致 | 4 | 0 | 4 | **-33%** | ✅ 正常 |
| **lMaxRPC** | 轻量级 MaxRPC | 4 | 0 | 4 | **-33%** | ✅ 正常 |
| **NSAC** | 邻域单例 AC | - | - | - | - | ⚠️ **超时** |
| **SAC1/SAC3** | 单例 AC | - | - | - | - | 📋 未集成 |

#### 关键观察

1. **强一致性算法优势明显**:
   - RPC3 和 lMaxRPC 在 Queens-4 上减少 33% 节点数
   - 零回溯（negatives=0），搜索更高效

2. **NSAC 性能问题**:
   - Queens-4 实例超时（>10秒）
   - 可能的原因: 邻域分析计算量大，需要优化

3. **算法层次结构设计合理**:
   - 所有强一致性算法继承 AC3bit，复用位向量优化
   - SAC 通过包装 AC 实例实现，避免代码重复

---

### 4. 代码架构总结

#### 核心组件层次

```
┌─────────────────────────────────────────────────────────────┐
│  Application Layer (apps/)                                  │
│  - cpim_test_parser: CPU 求解器入口                         │
│  - gmodel_solver: GPU 求解器入口                            │
│  - verify_gac, verify_search: 验证工具                      │
└─────────────────────────────────────────────────────────────┘
                            ↓
┌─────────────────────────────────────────────────────────────┐
│  Solver Layer (src/solver/)                                 │
│  - MAC: Maintaining Arc Consistency 搜索                    │
│  - AC 算法层次:                                             │
│    - AC3bit (位向量优化)                                    │
│    - RPC3 (关系路径一致)                                    │
│    - lMaxRPC (轻量级 MaxRPC)                                │
│    - NSAC (邻域单例 AC)                                     │
│    - SAC1/SAC3 (单例 AC)                                    │
│  - VariableSelector: 变量选择启发式                         │
└─────────────────────────────────────────────────────────────┘
                            ↓
┌─────────────────────────────────────────────────────────────┐
│  Network Layer (src/solver/common/)                         │
│  - Network: 运行时变量网络                                  │
│  - IntVar: 整数变量（多级域表示）                           │
│  - Tabular: 表约束                                          │
└─────────────────────────────────────────────────────────────┘
                            ↓
┌─────────────────────────────────────────────────────────────┐
│  Base Layer (src/base/)                                     │
│  - UnifiedTrail: 统一回溯系统（CPU/GPU 共享）               │
└─────────────────────────────────────────────────────────────┘
                            ↓
┌─────────────────────────────────────────────────────────────┐
│  Model Layer (src/model/)                                   │
│  - IntermediateModel: 标准化约束模型                        │
│  - ModelNormalizer: 模型归一化                              │
│  - XcspParser: XCSP3 解析                                   │
└─────────────────────────────────────────────────────────────┘
```

#### AC 算法继承关系

```cpp
// include/Solver.h

class AC {
 public:
  virtual PropagationResult enforce(const std::vector<IntVar*>& vars, int level) = 0;
  virtual ~AC() = default;
 protected:
  Network* m_;
};

class AC3 : public AC { /* 基础 AC3 算法 */ };

class AC3bit : public AC {
 protected:
  std::vector<std::vector<std::bitset<BITSIZE>>> bitSup_;  // 位向量优化
  int max_bitDom_size_;
};

class RPC3 : public AC3bit {
 private:
  std::vector<std::vector<std::vector<int>>> r_1_;  // 变量对支持关系
  std::vector<std::vector<std::vector<int>>> r_2_;
};

class lMaxRPC : public AC3bit {
 private:
  // PC witness 检测，Hall 区间分析
};

class NSAC : public AC3bit {
 private:
  std::vector<std::vector<std::vector<IntVar*>>> common_neibor_;  // 邻域约束
};

class SAC1 {
 private:
  AC* ac_;  // 包装 AC 实例，嵌套调用
};
```

#### 关键数据结构

**IntVar (多级域表示)**:
```cpp
class IntVar {
 private:
  std::vector<int> vals_;                     // 链表域（支持增量删除）
  std::vector<std::bitset<BITSIZE>> bitDom_; // 位域（快速查询）
  int size_;                                  // 当前域大小
};
```

**Tabular (表约束)**:
```cpp
class Tabular : public Constraint {
 public:
  std::vector<IntVar*> scope;        // 约束作用域变量
  std::vector<std::vector<int>> tuples_;  // 允许的元组
  int arity;                         // 约束元数
  bool sat(const std::vector<int>& tuple);  // 元组检查
};
```

**UnifiedTrail (回溯系统)**:
```cpp
class UnifiedTrail {
 public:
  void NewLevel();                    // 创建新搜索层
  void BacktrackTo(int level);        // 回溯到指定层
  void SaveDomainChange(IntVar* var, int removed_value);
 private:
  std::vector<std::vector<DomainChange>> trail_;  // 分层域变化记录
};
```

---

## 技术细节补充

### 1. AC3bit 位向量优化原理

**问题**: AC3 在检查支持时需要遍历另一个变量的整个域，时间复杂度 O(d)。

**AC3bit 解决方案**:
```cpp
// 位向量支持索引: bitSup_[IntConValIndex][bitDom_idx]
// IntConValIndex = constraint_id * max_domain_size * max_arity + var_idx * max_domain_size + value

// 支持检查（O(d/64) 位运算）
bool AC3bit::seek_support(const IntConVal& c_val, const int p) {
  const int idx = m_->GetIntConValIndex(c_val);

  for (IntVar* y : c_val.c()->scope) {
    if (y->id() != c_val.v()->id()) {
      // 对另一个变量的位域进行与运算
      for (int i = 0; i < y->bitDom().size(); ++i) {
        if ((bitSup_[idx][i] & y->bitDom()[i]).any()) {
          return true;  // 找到支持
        }
      }
    }
  }
  return false;
}
```

**性能提升**: 64x 加速（64位 bitset 一次处理 64 个值）

---

### 2. RPC3 路径一致性原理

**定义**: 值 (x, a) 是路径一致的，当且仅对于任意约束 C(x, y)，存在支持值 (y, b)，使得 (y, b) 也有路径一致的支持。

**实现**:
```cpp
class RPC3 : public AC3bit {
 private:
  // r_1_[var_idx][value] = 支持该值的约束-值对集合
  std::vector<std::vector<std::vector<int>>> r_1_;
  std::vector<std::vector<std::vector<int>>> r_2_;

  bool revise_rpc(const IntConVal& c_val);  // 检查路径一致性
};
```

**效果**: 比 GAC 更强的域过滤 → 减少搜索树节点数

---

### 3. MAC 搜索流程

```cpp
SearchStatistics MAC::enforce(const int time_limits) {
  // 1. 初始传播
  consistent_ = ac_->enforce(n_->vars, 0).state;
  if (!consistent_) return statistics_;

  while (!finished_) {
    // 2. 变量选择（DOM/DEG, DOM/DDEG, DOM/WDEG, VSIDS）
    IntVal v_a = select_v_value(I.size());

    // 3. 赋值
    n_->trail()->NewLevel();
    I.push(v_a);
    v_a.v()->ReduceTo(v_a.a());

    // 4. 传播
    auto cs = ac_->enforce(x_evt_, I.size());
    consistent_ = cs.state;

    if (consistent_ && I.full()) {
      // 找到解
      get_solution();
      return statistics_;
    }

    if (!consistent_) {
      // 5. 回溯
      I.pop();
      n_->trail()->BacktrackTo(I.size() - 1);
      v_a.v()->RemoveValue(v_a.a());

      // 6. 回溯后传播
      consistent_ = v_a.v()->size() && ac_->enforce(x_evt_, I.size()).state;
    }
  }
}
```

---

## 当前代码库状态

### Git 分支结构

```
refactor/directory-reorganization (当前分支)
├── 6e6851f - feat: Phase 1.5 + 2.1 - Variable Heuristics & Propagator Framework
├── 2ab67c5 - revert: Remove Propagator framework - return to AC algorithm hierarchy
└── 08243ef - feat: Add AC algorithm comparison framework (HEAD)

experimental/propagator-framework (实验性分支)
└── 6e6851f - 保存 Propagator 框架实现
```

### 关键文件清单

**应用程序**:
- [apps/cpim_test_parser.cpp](../../apps/cpim_test_parser.cpp) - CPU 求解器（新增 --ac_algorithm 参数）
- [apps/gmodel_solver.cpp](../../apps/gmodel_solver.cpp) - GPU 求解器
- [apps/verify_gac.cpp](../../apps/verify_gac.cpp) - GAC 验证工具
- [apps/verify_search.cpp](../../apps/verify_search.cpp) - 搜索验证工具

**AC 算法实现**:
- [src/solver/cpu/AC3.cpp](../../src/solver/cpu/AC3.cpp) - 基础 AC3
- [src/solver/cpu/AC3bit.cpp](../../src/solver/cpu/AC3bit.cpp) - 位向量优化
- [src/solver/cpu/RPC3.cpp](../../src/solver/cpu/RPC3.cpp) - 关系路径一致性
- [src/solver/cpu/lMaxRPC.cpp](../../src/solver/cpu/lMaxRPC.cpp) - 轻量级 MaxRPC
- [src/solver/cpu/NSAC.cpp](../../src/solver/cpu/NSAC.cpp) - 邻域单例 AC
- [src/solver/cpu/SAC1.cpp](../../src/solver/cpu/SAC1.cpp) - 单例 AC
- [src/solver/cpu/MAC.cpp](../../src/solver/cpu/MAC.cpp) - MAC 搜索

**测试框架**:
- [tests/python/compare_ac_algorithms.py](../../tests/python/compare_ac_algorithms.py) - **新增** AC 算法对比测试
- [tests/python/compare_cpu_gpu.py](../../tests/python/compare_cpu_gpu.py) - CPU vs GPU 对比测试
- [tests/python/batch_test_v2.py](../../tests/python/batch_test_v2.py) - 批量测试
- [tests/python/tier_definitions.py](../../tests/python/tier_definitions.py) - 测试集分层定义

**文档**:
- [docs/planning/PROPAGATOR_FRAMEWORK_DESIGN.md](PROPAGATOR_FRAMEWORK_DESIGN.md) - Propagator 框架设计（已废弃）
- [docs/planning/PHASE_1.5_COMPLETION_SUMMARY.md](PHASE_1.5_COMPLETION_SUMMARY.md) - 变量选择启发式总结
- [docs/architecture/ARCHITECTURE.md](../architecture/ARCHITECTURE.md) - 整体架构文档

---

## 待解决问题

### 1. NSAC 性能问题 (⚠️ 高优先级)

**现象**: Queens-4 实例超时（>10秒），而 AC3bit 仅需 <1ms

**可能原因**:
- 邻域约束分析计算复杂度过高
- `common_neibor_` 数据结构构建低效
- 传播策略不够优化

**调试步骤**:
```bash
# 1. 使用 VLOG 查看详细日志
GLOG_v=2 ./build/cpim_test_parser --bench_path=tests/data/bench/queens-4_ext.xml --ac_algorithm=NSAC

# 2. 使用 gprof 性能分析
g++ -pg -o cpim_test_parser ...
./cpim_test_parser ...
gprof cpim_test_parser gmon.out > analysis.txt
```

**修复建议**:
- 优化 `common_neibor_` 构建（使用 absl::flat_hash_map）
- 添加早期终止条件（邻域为空时跳过）
- 考虑懒惰计算（按需构建邻域）

---

### 2. SAC1/SAC3 集成

**当前状态**: SAC1/SAC3 实现存在，但未通过 `ACAlgorithm` 枚举集成到 MAC

**集成步骤**:
1. 在 `include/Solver.h` 枚举中添加 `AC_SAC1`, `AC_SAC3`
2. 在 `MAC.cpp` 构造函数中添加 SAC1/SAC3 分支
3. 验证嵌套 AC 调用是否正确处理 Trail 回溯
4. 测试性能（SAC 通常极慢，仅用于困难实例）

---

### 3. GPU vs CPU 多算法对比测试

**目标**: 扩展 `compare_cpu_gpu.py`，支持对比 GPU 与多种 CPU AC 算法

**实现思路**:
```python
# tests/python/compare_cpu_gpu_multi_ac.py
def test_gpu_vs_cpu_algorithms():
    gpu_result = run_gpu_solver(instance)

    cpu_results = {}
    for alg in ["AC3bit", "RPC3", "lMaxRPC"]:
        cpu_results[alg] = run_cpu_solver(instance, ac_algorithm=alg)

    # 对比：
    # 1. GPU 解是否正确（与所有 CPU 算法一致）
    # 2. GPU 节点数 vs 各 CPU 算法节点数
    # 3. GPU 时间 vs 各 CPU 算法时间
```

---

## 下一步计划

### 短期 (1-2 周)

1. **修复 NSAC 性能问题**:
   - [ ] 性能分析（gprof/valgrind）
   - [ ] 优化邻域计算
   - [ ] 验证正确性

2. **完善对比测试框架**:
   - [ ] 扩展 `compare_ac_algorithms.py` 支持 TIER 1/2
   - [ ] 创建 `compare_cpu_gpu_multi_ac.py`
   - [ ] 自动生成性能报告（markdown/CSV）

3. **文档化**:
   - [ ] 创建 `docs/algorithms/AC_HIERARCHY.md`（AC 算法层次结构详解）
   - [ ] 更新 `CLAUDE.md`（反映当前架构）
   - [ ] 添加使用示例到 README

### 中期 (3-4 周)

4. **集成 SAC1/SAC3**:
   - [ ] 添加到 `ACAlgorithm` 枚举
   - [ ] 集成到 MAC 构造函数
   - [ ] 性能测试和优化

5. **GPU 求解器优化**:
   - [ ] 分析 GPU vs CPU 性能差异
   - [ ] 识别 GPU 瓶颈（传播 vs 搜索）
   - [ ] 探索 GPU 并行搜索

6. **大规模基准测试**:
   - [ ] 运行 TIER 2 完整测试集
   - [ ] 对比不同算法在各类问题上的表现
   - [ ] 生成性能报告和图表

### 长期 (Phase 3)

7. **自适应 CPU/GPU 切换引擎**:
   - [ ] 启发式判断何时使用 GPU（问题规模、约束密度）
   - [ ] 实现运行时切换
   - [ ] 性能验证

8. **扩展约束类型**:
   - [ ] AllDifferent 全局约束
   - [ ] Element 索引约束
   - [ ] Cumulative 资源约束

---

## 参考资料

### 论文
1. Bessière, C. (2006). *Constraint Propagation*. Handbook of Constraint Programming.
2. Lecoutre, C., & Hemery, F. (2007). *A Study of Residual Supports in Arc Consistency*. IJCAI 2007.
3. Lecoutre, C. (2011). *STR2: Optimized Simple Tabular Reduction for Table Constraints*. CP 2011.
4. Wallace, R. J. (2015). *Neighbourhood Singleton Arc Consistency*. AI Communications.

### 代码库
- OR-Tools (Google): https://github.com/google/or-tools
- Gecode: https://www.gecode.org/
- Choco Solver: https://github.com/chocoteam/choco-solver

### XCSP 格式
- XCSP3 规范: http://www.xcsp.org/

---

## 联系与协作

**项目维护者**: leezear2022
**GitHub**: https://github.com/leezear2022/cpim
**最后更新**: 2025-12-21

**当前工作分支**: `refactor/directory-reorganization`
**实验性分支**: `experimental/propagator-framework` (Propagator 框架实现，已归档)

---

## 附录: 快速上手命令

```bash
# 1. 克隆仓库
git clone https://github.com/leezear2022/cpim.git
cd cpim

# 2. 构建
mkdir -p build && cd build
cmake ..
make -j4

# 3. 测试不同 AC 算法
./cpim_test_parser --bench_path=../tests/data/bench/queens-4_ext.xml --ac_algorithm=AC3bit
./cpim_test_parser --bench_path=../tests/data/bench/queens-4_ext.xml --ac_algorithm=RPC3
./cpim_test_parser --bench_path=../tests/data/bench/queens-4_ext.xml --ac_algorithm=lMaxRPC

# 4. 运行 AC 算法对比测试
cd ..
python3 tests/python/compare_ac_algorithms.py --tier=0 --algorithms=AC3bit,RPC3,lMaxRPC

# 5. CPU vs GPU 对比测试
python3 tests/python/compare_cpu_gpu.py --tier=0

# 6. 批量测试（分层）
python3 tests/python/batch_test_v2.py --tier=0  # 快速烟测
python3 tests/python/batch_test_v2.py --tier=1  # 标准回归
python3 tests/python/batch_test_v2.py --tier=2  # 完整测试
```

---

**文档版本**: 1.0
**生成时间**: 2025-12-21 23:55 UTC+8
**生成工具**: Claude Sonnet 4.5 via Claude Code
