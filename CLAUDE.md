# CLAUDE.md

## 项目概述

CPIM 是 CUDA 加速的约束满足问题 (CSP) 求解器，实现了多种弧一致性算法 (AC3, FC, SAC, RPC 等)，支持 CPU 和 GPU 求解。解析 XCSP3 格式问题文件，使用 MAC 搜索算法求解。

## 构建与运行

```bash
# 克隆仓库
git clone https://github.com/leezear2022/cpim.git
cd cpim

# 构建
mkdir -p build && cd build
cmake ..
make -j4

# CPU 求解器
./cpim_test_parser --bench_path=../tests/data/bench/queens-4_ext.xml

# 验证工具
./verify_gac --input=../tests/data/bench/queens-4_ext.xml
./verify_search --input=../tests/data/bench/queens-4_ext.xml

# Python 测试
cd .. && python3 tests/python/batch_test_v2.py --tier=0
```

**依赖**: CUDA 11.0+, libxml2, glog, gflags, Abseil (通过 CMake FetchContent 自动获取)

## 核心架构

详见 [docs/architecture/ARCHITECTURE.md](docs/architecture/ARCHITECTURE.md)

| 层 | 组件 | 位置 | 说明 |
|---|------|------|------|
| 模型 | IntermediateModel | src/model/ | 标准化的约束模型表示 |
| 网络 | Network, IntVar | src/solver/common/ | 运行时网络（统一 Trail 回溯） |
| CPU 求解器 | AC3bit, MAC, SAC | src/solver/cpu/ | CPU 弧一致性和搜索 |
| GPU 求解器 | GModel, cuSAC | src/solver/gpu/ | GPU 加速传播 |
| 基础组件 | UnifiedTrail | src/base/ | 统一 CPU/GPU 回溯系统 |

**数据流**: `XCSP3 → Parser → IntermediateModel → Network → MAC + AC → Solution`

## 文档导航

详见 [docs/README.md](docs/README.md)

### 快速链接

| 类别 | 文档 |
|------|------|
| 系统架构 | [docs/architecture/ARCHITECTURE.md](docs/architecture/ARCHITECTURE.md) |
| 应用程序参考 | [docs/guides/APPS_REFERENCE.md](docs/guides/APPS_REFERENCE.md) |
| 测试指南 | [docs/guides/TESTING_GUIDE.md](docs/guides/TESTING_GUIDE.md) |
| AC 算法 | [docs/algorithms/consistency/AC_HIERARCHY.md](docs/algorithms/consistency/AC_HIERARCHY.md) |
| SAC 算法 | [docs/algorithms/consistency/SAC_ALGORITHMS.md](docs/algorithms/consistency/SAC_ALGORITHMS.md) |
| MAC 搜索 | [docs/algorithms/search/MAC_SEARCH.md](docs/algorithms/search/MAC_SEARCH.md) |
| 变量启发式 | [docs/algorithms/heuristics/VARIABLE_HEURISTICS.md](docs/algorithms/heuristics/VARIABLE_HEURISTICS.md) |
| GPU 实现 | [docs/gpu/GMODEL_ARCHITECTURE.md](docs/gpu/GMODEL_ARCHITECTURE.md) |

## 目录结构

```
cpim/
├── apps/              # 应用程序入口
│   ├── cpim_test_parser.cpp  # CPU 求解器主程序
│   ├── verify_gac.cpp        # GAC 验证工具
│   └── verify_search.cpp     # 搜索验证工具
├── src/
│   ├── base/          # 基础组件（Trail 等）
│   ├── model/         # 模型层（解析、归一化）
│   └── solver/
│       ├── common/    # CPU/GPU 共享代码（Network, Solver）
│       ├── cpu/       # CPU 算法（AC3bit, MAC, SAC）
│       └── gpu/       # GPU 算法（GModel, cuSAC）
├── include/           # 公共头文件
├── tests/
│   ├── cpp/           # C++ 单元测试
│   ├── python/        # Python 测试脚本
│   └── data/bench/    # 测试实例
├── third_party/
│   └── xcsp3parser/   # XCSP3 解析器
├── docs/
│   ├── README.md      # 文档导航入口
│   ├── architecture/  # 架构文档
│   ├── algorithms/    # 算法理论（AC, SAC, MAC, 启发式）
│   ├── gpu/           # GPU 实现文档
│   ├── guides/        # 使用指南
│   ├── planning/      # 活跃开发规划
│   ├── performance/   # 性能分析
│   ├── bugfixes/      # Bug 修复记录
│   └── archive/       # 归档文档
└── benchmarks/        # 大型测试实例（.gitignore）
```

## 测试与验证

详见 [docs/guides/TESTING_GUIDE.md](docs/guides/TESTING_GUIDE.md)

### 验证工具

- `verify_gac` - 验证 AC3bit GAC 传播正确性
- `verify_search` - 验证 MAC 搜索过程正确性

### 分层测试

```bash
# TIER 0: 快速验证（Queens-4, Queens-12 等小实例）
python3 tests/python/batch_test_v2.py --tier=0

# TIER 1: 中等规模（Langford, 小型 rand-2-* 等）
python3 tests/python/batch_test_v2.py --tier=1

# TIER 2: 大规模（完整 benchmarks/）
python3 tests/python/batch_test_v2.py --tier=2
```

## 已修复的 Bug

详见 [docs/bugfixes/](docs/bugfixes/)

| # | 问题 | 位置 | 状态 | 文档 |
|---|------|------|------|------|
| 1 | get_solution() 错误索引 | src/solver/cpu/MAC.cpp:255 | ✅ | BUG_FIX_MAC_GET_SOLUTION.md |
| 2 | UNSAT 时调用 get_solution | apps/cpim_test_parser.cpp | ✅ | 同上 |
| 3 | 找到解后未设置 num_sol | src/solver/cpu/MAC.cpp:134 | ✅ | 同上 |
| 4-6 | Trail 回溯缺陷（3个） | src/base/, src/solver/common/ | ✅ | UNIFIED_TRAIL_MEMO.md |
| 7 | GPU 二分回溯搜索错误 | apps/gmodel_solver.cpp:55-127 | ✅ | GPU_BINARY_BACKTRACK_FIX.md |

## 关键文件

### 应用程序

详见 [docs/guides/APPS_REFERENCE.md](docs/guides/APPS_REFERENCE.md)

**求解器**
- [apps/cpim_test_parser.cpp](apps/cpim_test_parser.cpp) - CPU 求解器入口
- [apps/gmodel_solver.cpp](apps/gmodel_solver.cpp) - GPU 求解器

**验证工具**
- [apps/verify_gac.cpp](apps/verify_gac.cpp) - GAC 验证工具
- [apps/verify_search.cpp](apps/verify_search.cpp) - 搜索验证工具

**基准测试**
- [apps/sac_benchmark.cpp](apps/sac_benchmark.cpp) - SAC-GPU 基准测试
- [apps/benchmark_probe_throughput.cpp](apps/benchmark_probe_throughput.cpp) - Batch 探测吞吐量
- [apps/compare_cpu_gpu.cpp](apps/compare_cpu_gpu.cpp) - CPU/GPU 对比

**调试工具**
- [apps/dump_gmodel.cpp](apps/dump_gmodel.cpp) - GModel 导出
- [apps/cpim_dump.cpp](apps/cpim_dump.cpp) - 模型导出
- [apps/cpim_gac_cpu.cpp](apps/cpim_gac_cpu.cpp) - CPU GAC 工具

### 核心实现
- [src/solver/cpu/MAC.cpp](src/solver/cpu/MAC.cpp) - MAC 搜索算法
- [src/solver/cpu/AC3bit.cpp](src/solver/cpu/AC3bit.cpp) - AC3bit 传播
- [src/solver/common/Network.cpp](src/solver/common/Network.cpp) - 多级域网络
- [src/base/unified_trail.cpp](src/base/unified_trail.cpp) - 统一 Trail 回溯系统
- [src/solver/gpu/GModel.cu](src/solver/gpu/GModel.cu) - 简化 GPU 模型
- [src/solver/common/variable_selector.cpp](src/solver/common/variable_selector.cpp) - 变量选择启发式（Phase 1.5）

### 测试脚本

**批量测试**
- [tests/python/batch_test_v2.py](tests/python/batch_test_v2.py) - 分层批量测试（主测试脚本）
- [tests/python/tier_definitions.py](tests/python/tier_definitions.py) - 分层测试集定义

**对比测试**
- [tests/python/compare_cpu_gpu.py](tests/python/compare_cpu_gpu.py) - CPU/GPU 节点数对比
- [tests/python/compare_ac_algorithms.py](tests/python/compare_ac_algorithms.py) - AC 算法对比
- [tests/python/compare_sac_algorithms.py](tests/python/compare_sac_algorithms.py) - SAC 算法对比
- [tests/python/compare_activation_strategies.py](tests/python/compare_activation_strategies.py) - 激活策略对比
- [tests/python/compare_batch2_tier0.py](tests/python/compare_batch2_tier0.py) - Batch2 测试

**基准测试**
- [tests/python/benchmark_heuristics.py](tests/python/benchmark_heuristics.py) - 启发式性能对比

**外部求解器**
- [tests/python/solve_xcsp_ortools.py](tests/python/solve_xcsp_ortools.py) - OR-Tools SAT 求解
- [tests/python/solve_xcsp_ortools_cp.py](tests/python/solve_xcsp_ortools_cp.py) - OR-Tools CP 求解

## 开发规划

详见 [docs/planning/](docs/planning/)

**当前阶段**: Phase 1.5 完成，准备进入 Phase 1.3 或 Phase 2

### 已完成阶段

#### Phase 1.1: 统一 CPU/GPU Trail 回溯系统 ✅
- UnifiedTrail 基础架构
- CPU Network 单层域 + Trail 集成
- 修复 3 个 Trail 回溯缺陷
- 文档：[UNIFIED_TRAIL_MEMO.md](docs/bugfixes/UNIFIED_TRAIL_MEMO.md)

#### Phase 1.2: GPU Trail 集成与搜索验证 ✅
**架构改造**：
- GModel 单层域架构重构（内存节省 92%）
- UnifiedTrail 集成到 GModel
- GPU kernel 单层索引适配（BitmapGAC, PersistentGAC, ExecuteConstraintCheck）
- EnforceGAC/EnforceGAC_Persistent 域快照与 Trail 记录
- AssignValue/RemoveValue Host 端 Trail 记录
- 优化的 BacktrackTo 批量域恢复逻辑

**搜索算法修复**：
- 修复 gmodel_solver 二分回溯搜索逻辑（while(true) 循环模拟 CPU MAC）
- 实现正确的值移除 + 重新传播机制
- 修复层级管理和节点计数

**验证成果**：
- ✅ TIER 0 完整通过（12/12 实例）
- ✅ TIER 1 部分通过（15/39 实例，0 失败）
- ✅ CPU/GPU 节点数精确匹配（Positives/Negatives 完全一致）
- 关键测试案例：
  - queens-4: P=5, N=1 ✓
  - langford-3-9: P=468, N=441 ✓
  - test.xml, langford-2-4, driverlogw-01c-sat 等全部通过

**相关文件**：
- [apps/gmodel_solver.cpp](apps/gmodel_solver.cpp:55-127) - GPU 求解器 Search() 实现
- [tests/python/compare_cpu_gpu.py](tests/python/compare_cpu_gpu.py) - CPU/GPU 对比测试脚本

#### Phase 1.5: 变量选择启发式增强 ✅
**设计文档**: [docs/planning/PHASE_1.5_HEURISTICS_DESIGN.md](docs/planning/PHASE_1.5_HEURISTICS_DESIGN.md)

**核心实现**：
- ✅ 可插拔 VariableSelector 框架
  - 抽象接口 + 三种具体实现（MinDomain, DOM/DEG, DOM/DDEG）
  - [include/solver/common/variable_selector.h](include/solver/common/variable_selector.h)
  - [src/solver/common/variable_selector.cpp](src/solver/common/variable_selector.cpp)

- ✅ GModel 启发式支持
  - 添加 var_degrees（变量度数）
  - 添加 constraint_scopes_cpu（CPU 友好格式）
  - GModelAdapter 自动计算启发式数据结构

- ✅ gmodel_solver 集成
  - 命令行选项：`--heuristic=min_domain|dom_deg|dom_ddeg`
  - 默认使用 MinDomain（与 Phase 1.2 行为一致）
  - 所有启发式保证 CPU/GPU 节点数匹配

**验证成果**：
- ✅ 编译通过，所有启发式正常工作
- ✅ queens-4 测试：所有启发式 P=5/N=1（预期，变量度数相同）
- ✅ test.xml 测试：所有启发式 P=3/N=0（预期）
- ✅ CPU/GPU 一致性保持：节点数完全匹配

**测试工具**：
- [tests/python/benchmark_heuristics.py](tests/python/benchmark_heuristics.py) - 启发式性能对比脚本

**下一步优化**：
- 测试约束密集问题（预期 DOM/DDEG 有显著提升）
- 添加 DOM/WDEG（加权度数）
- 添加 Impact-Based Search

### 计划中的阶段

#### Phase 1.3: 自适应 CPU/GPU 切换引擎（待实施）
**设计文档**: [docs/planning/ADAPTIVE_ENGINE_DESIGN.md](docs/planning/ADAPTIVE_ENGINE_DESIGN.md)

**核心方案**：
- **方案 1（优先）**: 基于问题特征的静态选择
  - ProblemProfiler 分析器（变量数、约束数、域大小、传播复杂度）
  - 启发式决策规则（小问题→CPU，大问题→GPU）
  - 实现 cpim_adaptive_solver 应用

- **方案 2（后期）**: 混合执行模式
  - CPU 负责搜索（变量/值选择）
  - GPU 负责传播（并行 GAC）
  - 异步传播管线

- **方案 3（长期）**: 运行时自适应切换
  - 运行时性能监控
  - 动态模式切换

**状态**: 设计完成，暂不实施

#### Phase 2: Propagator 框架重构（待实施）
**设计文档**: [docs/planning/PROPAGATOR_FRAMEWORK_DESIGN.md](docs/planning/PROPAGATOR_FRAMEWORK_DESIGN.md)

**核心目标**: 参考 OR-Tools CP-SAT 架构，重构为模块化的 Propagator 框架

**四阶段计划**：
- **Phase 2.1（2-3周）**: 基础框架
  - IntegerVariable 接口（统一 CPU/GPU 域抽象）
  - Propagator 接口（可插拔传播器）
  - EventNotifier（事件驱动机制）
  - PropagationEngine（传播引擎 + 不动点计算）

- **Phase 2.2（3-4周）**: Propagator 实现
  - AllDifferent（AC 传播）
  - TableConstraint（GAC 算法）
  - Linear（线性约束）
  - Global Constraints（Element, Cumulative 等）

- **Phase 2.3（2-3周）**: 现有代码迁移
  - MAC 搜索器重构
  - AC3bit 转换为 Propagator
  - XcspParser 适配新框架

- **Phase 2.4（3-4周）**: GPU 集成
  - GPUIntegerVariable（通过 GModel 代理）
  - GPUDomainManager（批量操作）
  - GPUPropagatorAdapter（CPU→GPU 自动适配）

**预期收益**：
- ✅ 可扩展性（易于添加新约束类型）
- ✅ CPU/GPU 统一抽象（降低维护成本）
- ✅ 事件驱动优化（减少无效传播）
- ✅ 支持用户自定义约束

**状态**: 设计完成，暂不实施

完整现代化计划：[docs/planning/MODERNIZATION_PLAN_V2.md](docs/planning/MODERNIZATION_PLAN_V2.md)

## 注意事项

- **通信语言**: 全程使用中文交流
- **主程序**: `apps/cpim_test_parser.cpp`
- **测试实例**: `tests/data/bench/`（小型）, `benchmarks/`（大型，需单独下载）
- **第三方库**: `third_party/xcsp3parser` 包含在主仓库中
