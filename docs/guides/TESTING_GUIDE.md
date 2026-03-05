# CPIM 测试指南

## 概述

本文档记录 CPIM 求解器的测试流程、验证工具和最佳实践。

## 验证工具

### 1. verify_gac - GAC 传播正确性验证

**功能**: 验证 AC3bit 的 GAC（广义弧一致性）传播是否正确

**原理**:
- 使用迭代不动点法计算"正确的" GAC 结果作为参考
- 与 AC3bit 的域缩减结果逐变量对比
- 检测过度剪枝（OVER-PRUNED）和不足剪枝（UNDER-PRUNED）

**使用方法**:
```bash
cd build
./verify_gac --input=<xcsp_file>
```

**输出解读**:
- `AC3bit result: consistent/inconsistent` - AC3bit 判定结果
- `Deletions: N` - 域值删除数量
- `MISMATCH` - 域不匹配（存在错误）
- `All domains match` - 验证通过

**UNSAT 处理**: 当问题不可满足时，AC3bit 可能提前终止，此时只需验证两者都检测到 UNSAT 即可。

### 2. verify_search - MAC 搜索过程验证

**功能**: 验证 MAC 搜索算法的正确性

**原理**:
- 对小规模问题（搜索空间 < 1,000,000）使用穷举法
- 检查 MAC 找到的解是否在穷举解集中
- 对大规模问题使用 `solution_check()` 验证解的有效性

**使用方法**:
```bash
cd build
./verify_search --input=<xcsp_file> --timeout=10
```

**验证内容**:
- 解的正确性（满足所有约束）
- SAT/UNSAT 判定一致性
- 搜索统计信息（num_positive, num_negative）

## 已修复的 Bug

| # | 问题 | 位置 | 发现方式 | 修复 |
|---|------|------|----------|------|
| 1 | `get_solution()` 使用 `I[i].a()` 而非正确的域值 | `src/MAC.cpp:255-262` | 人工代码审查 | 改用 `n_->vars[i]->head(level)` |
| 2 | UNSAT 时无条件调用 `get_solution()` | `samples/main_new_parser.cpp` | 运行时崩溃 | 添加 `num_sol > 0` 检查 |
| 3 | 找到解后未设置 `statistics_.num_sol` | `src/MAC.cpp:134` | verify_search 发现 | 添加 `statistics_.num_sol = 1` |

## 已验证的测试实例

### GAC 验证通过
| 类型 | 实例 | 结果 |
|------|------|------|
| Langford | langford-2-4, langford-3-9, langford-3-11 | ✅ SAT |
| Graph | graphw-05, graphw-06 | ✅ UNSAT |
| BlackHole | BlackHole-4-4-e-0, BlackHole-4-4-e-3 | ✅ SAT |
| Random CSP | rand-2-40-8-753-100-0, rand-2-40-80-103-800-0 | ✅ SAT |
| Driver | driverlogw-09-sat | ✅ SAT |

### 搜索验证通过
| 实例 | 变量数 | 搜索空间 | 验证方式 |
|------|--------|----------|----------|
| test.xml | 3 | 27 | 穷举 |
| queens-4 | 4 | 256 | 穷举 |
| langford-2-4 | 8 | 16M | solution_check |

## 测试流程规范

### 新功能开发流程

1. **单元测试**: 新增算法应有独立的正确性测试
2. **集成测试**: 与 MAC 搜索集成后进行端到端测试
3. **回归测试**: 确保不影响现有功能

### 修复 Bug 后的验证流程

```bash
# 1. 重新编译
cd build && make -j4

# 2. GAC 验证（选择几个代表性实例）
./verify_gac --input=../samples/bench/queens-4_ext.xml
./verify_gac --input=../benchmarks/langford/langford-2-4-ext.xml

# 3. 搜索验证
./verify_search --input=../samples/bench/test.xml
./verify_search --input=../samples/bench/queens-4_ext.xml

# 4. 功能测试
./cpim_test_parser --input=../samples/bench/queens-12_ext.xml
```

### 对比测试（与 OR-Tools 纯 CP）

**推荐使用新的测试框架**（使用 OR-Tools constraint_solver，启发式与 CPIM 对齐）：

```bash
# 快速验证（TIER0, 12 实例，~2-3 分钟）
./scripts/quick_test.sh

# 标准回归（TIER1, 39 实例，~10-15 分钟）
python3 samples/batch_test_v2.py --tier=1 --timeout=60 --stats

# 完整验证（TIER2, 79 实例，~30-60 分钟）
./scripts/full_test.sh

# 单文件测试
python3 samples/solve_xcsp_ortools_cp.py <xcsp_file> --verbose
```

**旧版本**（使用 CP-SAT，仅供参考）：
```bash
python3 samples/solve_xcsp_ortools.py <xcsp_file>
python3 samples/batch_test.py --tier=0
```

## 测试实例位置

| 目录 | 说明 |
|------|------|
| `samples/bench/` | 小规模测试实例（queens, test.xml 等）|
| `benchmarks/langford/` | Langford 问题 |
| `benchmarks/graphs/` | 图着色问题 |
| `benchmarks/BH-4-4/` | BlackHole 问题 |
| `benchmarks/tightness*/` | 随机 CSP |
| `benchmarks/driver/` | 驾驶员日志问题 |

## 测试习惯建议

### 每次修改代码后

1. **快速验证**: 运行小实例确保基本功能正常
   ```bash
   ./verify_search --input=../samples/bench/test.xml
   ```

2. **完整验证**: 修改核心算法后运行完整测试套件

### 发现 Bug 时

1. 记录复现步骤和输入实例
2. 定位问题根因
3. 修复后添加针对性测试用例
4. 更新本文档的 Bug 列表

### 性能测试

```bash
# 带计时的求解
./cpim_test_parser --input=<large_instance> 2>&1 | grep -E "(time|nodes)"
```

## 可信基线

**cpim_test_parser (MAC + AC3bit)** 已通过以下验证：
- ✅ GAC 传播正确性（13+ 实例）
- ✅ 搜索过程正确性（穷举验证）
- ✅ 解提取正确性
- ✅ UNSAT 检测正确性

可作为 GPU 版本和其他算法的对比基线。

---

## OR-Tools 纯 CP 对比测试

### 为什么用 constraint_solver 而非 CP-SAT？

CPIM 使用传统的 **MAC（维护弧一致性）+ AC3bit** 算法，属于经典约束传播技术。OR-Tools 的 **CP-SAT** 基于 SAT 技术和混合整数规划，算法范式完全不同，无法公平对比性能。

OR-Tools 的 `constraint_solver` 模块实现传统 CP 算法（类似 CPIM），是更合适的对比基准。

### 启发式配置对齐

为了公平对比，我们配置了相同的搜索启发式策略：

| 维度       | CPIM (src/MAC.cpp)    | OR-Tools CP                  | 说明                    |
|-----------|----------------------|------------------------------|-------------------------|
| 变量选择   | `VRH_DOM_MIN`        | `CHOOSE_MIN_SIZE_LOWEST_MIN` | 最小域优先               |
| 值选择     | `VLH_MIN`            | `ASSIGN_MIN_VALUE`           | 最小值优先               |
| 传播算法   | AC3bit (GAC)         | AllowedAssignments (GAC)     | 广义弧一致性             |

**验证对齐效果**：通过对齐启发式，CPIM 和 OR-Tools CP 的搜索行为应该非常接近（分支数和回溯数相近），这样能更准确地对比求解器本身的性能，而不是被不同的启发式策略影响。

### 统计信息对照表

| 统计项       | CPIM                  | OR-Tools CP           | 含义                    |
|-------------|----------------------|----------------------|-------------------------|
| 正向决策数   | `num_positive`       | `solver.Branches()`  | 搜索树中的正向分支数     |
| 回溯次数     | `num_negative`       | `solver.Failures()`  | 搜索失败导致的回溯次数   |
| 求解时间     | `solve_time` (ms)    | `solver.WallTime()`  | 总求解时间（毫秒）       |
| GAC 删除数   | `cs.num_delete`      | N/A（内部）          | GAC 传播删除的域值数量   |

**预期结果**：由于启发式对齐，两者的 `branches` 和 `failures` 应该**非常接近甚至相同**。时间差异主要反映求解器实现效率。

---

## 测试集分类与使用指南

### 测试分层设计

| 层级   | 实例数 | 超时  | 预计时间  | 用途                  |
|--------|-------|-------|----------|----------------------|
| TIER0  | 12    | 10s   | 2-3分钟  | 快速烟测、日常开发     |
| TIER1  | 39    | 60s   | 10-15分钟| 标准回归、合并前验证   |
| TIER2  | 79    | 300s  | 30-60分钟| 完整验证、发版前测试   |
| TIER3  | 1000+ | 900s  | 数小时   | 压力测试（可选，按需）  |

### TIER0 - 快速烟测（12 个实例）

**用途**：每次代码修改后立即运行，快速发现明显问题

**覆盖特点**：
- ✅ 极小实例（test.xml）- 可穷举验证
- ✅ 经典问题（Langford, N-Queens）
- ✅ UNSAT 实例（graphs）- 验证不可满足性检测
- ✅ 不同紧度（0.1/0.5/0.8）- 约束强度梯度
- ✅ 多样化类型（BlackHole, Composed, Driver）

**运行方式**：
```bash
./scripts/quick_test.sh
# 或
python3 samples/batch_test_v2.py --tier=0 --timeout=10
```

**典型实例**：
- `samples/bench/test.xml` - 3 变量，最小测试
- `benchmarks/langford/langford-2-4-ext.xml` - 8 变量，经典组合问题
- `benchmarks/graphs/graphw-05_ext.xml` - UNSAT 实例

### TIER1 - 标准回归（39 个实例）

**用途**：合并代码前必须通过的测试，确保核心功能正常

**覆盖特点**：
- TIER0 全部 12 个 + 23 个扩展实例
- 覆盖 Tightness 0.1/0.5/0.8 梯度（14 个）
- 多样化问题类型（Composed, Driver, BlackHole, Graphs）

**运行方式**：
```bash
python3 samples/batch_test_v2.py --tier=1 --timeout=60 --stats
```

**验收标准**：
- 100% SAT/UNSAT 一致性（与 OR-Tools 对比）
- 无严重性能退化（时间增长 <20%）

### TIER2 - 完整验证（79 个实例）

**用途**：重大修改或发版前的完整验证

**覆盖特点**：
- TIER1 全部 39 个 + 40 个扩展实例
- 增加 Tightness 0.2/0.35/0.9 系列（更多难度）
- 添加 rand-2-23 全部 10 个实例（复杂 CSP）
- 覆盖更多约束类型和边界情况

**运行方式**：
```bash
./scripts/full_test.sh
# 或
python3 samples/batch_test_v2.py --tier=2 --timeout=300 --stats --export-csv=results.csv
```

**验收标准**：
- ≥95% SAT/UNSAT 一致性（允许少量超时）
- 无正确性错误（解验证失败）

### TIER3 - 夜间全量（1000+ 实例，可选）

**用途**：压力测试，发现罕见边界情况

**说明**：TIER3 包含所有支持的测试实例（排除 predicates/WCSP），实例数量较多（1000+），建议按需使用或仅在夜间运行。

**运行方式**：
```bash
# 默认运行 TIER2（推荐）
./scripts/nightly_test.sh

# 如需运行完整 TIER3
NIGHTLY_TIER=3 ./scripts/nightly_test.sh
```

---

## 测试实例分布

### 按类型分类

| 类型         | TIER0 | TIER1 | TIER2 | 特点               |
|-------------|-------|-------|-------|--------------------|
| Langford    | 2     | 4     | 4     | 经典组合问题        |
| Tightness0.1| 2     | 7     | 7     | 松散约束，易解      |
| Tightness0.5| 1     | 6     | 6     | 中等紧度            |
| Tightness0.8| 1     | 5     | 5     | 紧密约束            |
| Tightness0.9| 0     | 0     | 5     | 极难实例            |
| Composed    | 1     | 5     | 10    | 组合问题            |
| Graphs      | 1     | 3     | 3     | UNSAT 检测          |
| Driver      | 1     | 3     | 5     | 实际应用            |
| Rand-2-23   | 0     | 0     | 10    | 随机 CSP            |
| 其他        | 3     | 6     | 24    | 多样化覆盖          |
| **总计**    | **12**| **39**| **79**| **广泛覆盖**        |

### 按难度分类

| 难度   | 实例数 | 典型求解时间 | 代表实例                          |
|--------|-------|-------------|----------------------------------|
| 简单   | ~30   | <1s         | langford-2-4, tightness0.1 系列   |
| 中等   | ~35   | 1-10s       | langford-3-9, tightness0.5 系列   |
| 困难   | ~14   | 10-60s      | tightness0.8/0.9, composed-25     |
| UNSAT  | ~5    | 变化大      | graphs 系列                       |

---

## 回归测试工作流

### 日常开发流程

```
代码修改
    ↓
编译: cd build && cmake .. && make -j4
    ↓
快速验证: ./scripts/quick_test.sh  (2-3 分钟)
    ├─ verify_gac（GAC 正确性）
    ├─ verify_search（搜索正确性）
    └─ TIER0（12 实例对比）
    ↓
PASS → 提交代码
FAIL → 修复 → 重新测试
```

### 合并前验证流程

```
准备合并到主分支
    ↓
完整测试: ./scripts/full_test.sh  (30-60 分钟)
    ├─ TIER1（39 实例，<60s）
    └─ TIER2（79 实例，<300s）
    ↓
检查标准:
    - TIER1 全部通过（100%）
    - TIER2 ≥95% 通过（允许超时）
    - 无 SAT/UNSAT 不一致
    - 无解验证失败
    ↓
PASS → 合并
FAIL → 分析 → 修复 → 重新测试
```

### 自动化测试（可选）

```bash
# 添加到 Git hooks（.git/hooks/pre-commit）
#!/bin/bash
cd build && make -j4 && cd ..
./scripts/quick_test.sh || exit 1

# 或使用 CI/CD（GitHub Actions, GitLab CI 等）
# 在每次 push 时自动运行 TIER1
```

---

## 性能基线参考

### TIER1 典型性能（参考环境：Intel i7-10700K, Ubuntu 20.04）

| 类型          | 实例数 | CPIM 平均时间 | OR-Tools 平均时间 | 分支数差异 |
|--------------|-------|--------------|------------------|-----------|
| Langford     | 4     | ~1.2s        | ~1.8s            | <5%       |
| Tightness0.1 | 7     | ~0.5s        | ~0.8s            | <3%       |
| Tightness0.5 | 6     | ~5.3s        | ~6.9s            | <8%       |
| Tightness0.8 | 5     | ~12.3s       | ~15.7s           | <10%      |

**注意**：
1. 性能因硬件、编译选项而异，以上仅供参考
2. **分支数差异** 反映启发式对齐效果，差异应 <10%
3. 如果分支数差异 >20%，说明启发式可能未正确对齐

---

## 高级用法

### 解验证

```bash
# 自动验证 CPIM 找到的解
python3 samples/batch_test_v2.py --tier=0 --verify
```

### CSV 导出

```bash
# 导出详细结果到 CSV（用于数据分析）
python3 samples/batch_test_v2.py --tier=2 --export-csv=results.csv

# 分析结果（使用 pandas/Excel）
import pandas as pd
df = pd.read_csv('results.csv')
print(df[df['match'] == False])  # 查看不一致的实例
```

### 单文件调试

```bash
# 详细日志模式
python3 samples/solve_xcsp_ortools_cp.py samples/bench/test.xml --verbose

# 对比 CPIM 和 OR-Tools
./build/cpim_test_parser --bench_path=samples/bench/test.xml
python3 samples/solve_xcsp_ortools_cp.py samples/bench/test.xml --verbose
```

---

## 故障排除

### 问题 1：OR-Tools 未安装

**错误信息**：`Error: OR-Tools constraint_solver not installed`

**解决方案**：
```bash
pip3 install ortools
```

### 问题 2：分支数差异过大

**现象**：CPIM 和 OR-Tools 的 branches/failures 差异 >20%

**可能原因**：
1. 启发式未正确对齐（检查 `solve_xcsp_ortools_cp.py:114-117`）
2. CPIM 使用了不同的启发式（检查 `samples/main_new_parser.cpp` 的 MAC 构造参数）

**调试方法**：
```bash
# 查看详细搜索过程
GLOG_v=2 ./build/cpim_test_parser --bench_path=<file>
python3 samples/solve_xcsp_ortools_cp.py <file> --verbose
```

### 问题 3：测试时间过长

**现象**：TIER0 测试超过 5 分钟

**可能原因**：
1. 编译优化未开启（检查 `-DCMAKE_BUILD_TYPE=Release`）
2. 实例超时设置过大

**解决方案**：
```bash
# 重新编译（Release 模式）
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j4

# 减少超时时间
python3 samples/batch_test_v2.py --tier=0 --timeout=5
```

---

## 贡献指南

### 添加新测试实例

1. 将 XCSP 文件放入 `benchmarks/` 对应子目录
2. 更新 `tests/tier_definitions.py`（如果需要加入标准测试集）
3. 运行验证：
   ```bash
   ./build/verify_gac --input=<new_file>
   ./build/verify_search --input=<new_file>
   ```
4. 更新本文档的测试实例表格

### 报告测试问题

当发现测试失败时，请提供：
1. 失败的实例文件路径
2. CPIM 和 OR-Tools 的输出对比
3. 详细的错误信息（使用 `--verbose`）
4. 环境信息（OS, CUDA 版本, OR-Tools 版本）
