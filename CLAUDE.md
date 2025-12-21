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
│   ├── architecture/  # 架构文档
│   ├── guides/        # 使用指南
│   ├── planning/      # 开发规划
│   └── performance/   # 性能分析
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

## 关键文件

### 应用程序
- [apps/cpim_test_parser.cpp](apps/cpim_test_parser.cpp) - CPU 求解器入口
- [apps/verify_gac.cpp](apps/verify_gac.cpp) - GAC 验证工具
- [apps/verify_search.cpp](apps/verify_search.cpp) - 搜索验证工具

### 核心实现
- [src/solver/cpu/MAC.cpp](src/solver/cpu/MAC.cpp) - MAC 搜索算法
- [src/solver/cpu/AC3bit.cpp](src/solver/cpu/AC3bit.cpp) - AC3bit 传播
- [src/solver/common/Network.cpp](src/solver/common/Network.cpp) - 多级域网络
- [src/base/unified_trail.cpp](src/base/unified_trail.cpp) - 统一 Trail 回溯系统
- [src/solver/gpu/GModel.cu](src/solver/gpu/GModel.cu) - 简化 GPU 模型

### 测试
- [tests/python/batch_test_v2.py](tests/python/batch_test_v2.py) - 批量测试脚本
- [tests/python/tier_definitions.py](tests/python/tier_definitions.py) - 分层测试集定义

## 开发规划

详见 [docs/planning/](docs/planning/)

**当前阶段**: Phase 1.1 - 统一 CPU/GPU Trail 回溯系统（已完成）

**下一步**:
- Phase 1.2: GPU Trail 集成
- Phase 1.3: 自适应 CPU/GPU 切换引擎
- Phase 2: Propagator 框架重构

完整计划：[docs/planning/MODERNIZATION_PLAN_V2.md](docs/planning/MODERNIZATION_PLAN_V2.md)

## 注意事项

- **通信语言**: 全程使用中文交流
- **主程序**: `apps/cpim_test_parser.cpp`
- **测试实例**: `tests/data/bench/`（小型）, `benchmarks/`（大型，需单独下载）
- **第三方库**: `third_party/xcsp3parser` 包含在主仓库中
