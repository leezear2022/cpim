# CPIM 项目指南

> 本文档为 AI 助手（Claude、Codex、Gemini 等）提供统一的项目上下文。

## 项目概述

CPIM 是 CUDA 加速的约束满足问题 (CSP) 求解器，实现了多种弧一致性算法 (AC3, SAC, RPC 等)，支持 CPU 和 GPU 求解。解析 XCSP3 格式问题文件，使用 MAC 搜索算法求解。

**核心特性**：
- CPU 算法：AC3bit, RPC3, lMaxRPC, MAC, SAC
- GPU 算法：GModel (统一内存), Batch2/Batch3 并行探测
- 统一 Trail 回溯系统（CPU/GPU 共享）
- 可插拔变量选择启发式（MinDomain, DOM/DEG, DOM/DDEG）

---

## 构建与运行

```bash
# 构建
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# CPU 求解器
./cpim_test_parser --bench_path=../tests/data/bench/queens-4_ext.xml

# GPU 求解器
./gmodel_solver --input=../tests/data/bench/queens-4_ext.xml

# 验证工具
./verify_gac --input=../tests/data/bench/queens-4_ext.xml
./verify_search --input=../tests/data/bench/queens-4_ext.xml

# Python 测试
cd .. && python3 tests/python/batch_test_v2.py --tier=0
```

**依赖**: CUDA 11.0+, libxml2, glog, gflags, Abseil (CMake FetchContent 自动获取)

---

## 核心架构

| 层 | 组件 | 位置 | 说明 |
|---|------|------|------|
| 模型 | IntermediateModel | src/model/ | 标准化约束模型 |
| 网络 | Network, IntVar | src/solver/common/ | 运行时网络 + Trail |
| CPU 求解器 | AC3bit, MAC, SAC | src/solver/cpu/ | CPU 弧一致性和搜索 |
| GPU 求解器 | GModel | src/solver/gpu/ | GPU 加速传播 |
| 基础组件 | UnifiedTrail | src/base/ | 统一回溯系统 |

**数据流**: `XCSP3 → Parser → IntermediateModel → Network → MAC + AC → Solution`

---

## 目录结构

```
cpim/
├── apps/                  # 应用程序入口
│   ├── cpim_test_parser.cpp   # CPU 求解器
│   ├── gmodel_solver.cpp      # GPU 求解器
│   ├── verify_gac.cpp         # GAC 验证
│   ├── verify_search.cpp      # 搜索验证
│   ├── sac_benchmark.cpp      # SAC-GPU 基准测试
│   └── ...
├── src/
│   ├── base/              # 基础组件（Trail）
│   ├── model/             # 模型层（解析、归一化）
│   └── solver/
│       ├── common/        # CPU/GPU 共享（Network, Selector）
│       ├── cpu/           # CPU 算法
│       └── gpu/           # GPU 算法
├── include/               # 公共头文件
├── tests/
│   ├── cpp/               # C++ 单元测试
│   ├── python/            # Python 测试脚本
│   └── data/bench/        # 测试实例
├── docs/                  # 文档（见 docs/README.md）
└── benchmarks/            # 大型测试实例（.gitignore）
```

---

## 文档导航

详见 [docs/README.md](docs/README.md)

| 类别 | 文档 |
|------|------|
| 系统架构 | [docs/architecture/ARCHITECTURE.md](docs/architecture/ARCHITECTURE.md) |
| 应用程序 | [docs/guides/APPS_REFERENCE.md](docs/guides/APPS_REFERENCE.md) |
| 测试指南 | [docs/guides/TESTING_GUIDE.md](docs/guides/TESTING_GUIDE.md) |
| AC 算法 | [docs/algorithms/consistency/AC_HIERARCHY.md](docs/algorithms/consistency/AC_HIERARCHY.md) |
| SAC 算法 | [docs/algorithms/consistency/SAC_ALGORITHMS.md](docs/algorithms/consistency/SAC_ALGORITHMS.md) |
| GPU 实现 | [docs/gpu/GMODEL_ARCHITECTURE.md](docs/gpu/GMODEL_ARCHITECTURE.md) |

---

## 测试与验证

### 分层测试

```bash
# TIER 0: 快速验证（Queens-4/12 等小实例）
python3 tests/python/batch_test_v2.py --tier=0

# TIER 1: 中等规模（Langford, rand-2-* 等）
python3 tests/python/batch_test_v2.py --tier=1

# TIER 2: 大规模（完整 benchmarks/）
python3 tests/python/batch_test_v2.py --tier=2
```

### 验证工具

- `verify_gac` - 验证 GAC 传播正确性
- `verify_search` - 验证 MAC 搜索正确性
- `compare_cpu_gpu.py` - CPU/GPU 节点数对比

---

## 代码规范

### 风格

- C++17，RAII，Abseil 辅助
- 两空格缩进，同行大括号，行宽 ≤100
- 类型：`PascalCase`；函数：`lower_snake_case`；常量：`ALL_CAPS`
- 错误处理：`absl::Status` / `StatusOr`
- CUDA：标注 host/device 边界

### 提交

- 标题简洁（≤60 字符），动词开头
- 中文提交信息与历史保持一致
- 描述算法意图，附上性能数据
- 格式：
  ```
  <type>: <简述>

  <详细说明>

  Co-Authored-By: <AI Model> <noreply@anthropic.com>
  ```

---

## 关键文件

### 应用程序

| 文件 | 说明 |
|------|------|
| apps/cpim_test_parser.cpp | CPU 求解器入口 |
| apps/gmodel_solver.cpp | GPU 求解器 |
| apps/sac_benchmark.cpp | SAC-GPU 基准测试 |
| apps/verify_gac.cpp | GAC 验证 |
| apps/verify_search.cpp | 搜索验证 |

### 核心实现

| 文件 | 说明 |
|------|------|
| src/solver/cpu/MAC.cpp | MAC 搜索算法 |
| src/solver/cpu/AC3bit.cpp | AC3bit 传播 |
| src/solver/gpu/GModel.cu | GPU 模型 |
| src/base/unified_trail.cpp | 统一 Trail |
| src/solver/common/variable_selector.cpp | 变量启发式 |

### 测试脚本

| 文件 | 说明 |
|------|------|
| tests/python/batch_test_v2.py | 分层批量测试 |
| tests/python/compare_cpu_gpu.py | CPU/GPU 对比 |
| tests/python/compare_sac_algorithms.py | SAC 算法对比 |

---

## GPU 求解器

### GModel（当前使用）

统一内存 GPU 模型，针对 Jetson Orin 优化。

- **位置**: `include/GModel.cuh`, `src/solver/gpu/GModel.cu`
- **特点**:
  - 统一内存（`cudaMallocManaged`）
  - 单层域架构（内存节省 92%）
  - UnifiedTrail 集成
  - Batch2/Batch3 并行探测

### 数据布局

```cpp
bitDom[var_id * bit_dom_int_size + word_idx]  // 变量域位图
bitSup[c * bitsup_per_constraint + ...]       // 约束支持位集
```

### Jetson 适配

- 自动检测 `concurrentManagedAccess`
- 跳过不支持的 `cudaMemPrefetchAsync`

---

## 开发规划

**当前阶段**: Phase 1.5 完成

### 已完成

| Phase | 内容 | 状态 |
|-------|------|------|
| 1.1 | 统一 CPU/GPU Trail 回溯系统 | ✅ |
| 1.2 | GPU Trail 集成与搜索验证 | ✅ |
| 1.5 | 变量选择启发式增强 | ✅ |

### 计划中

| Phase | 内容 | 文档 |
|-------|------|------|
| 1.3 | 自适应 CPU/GPU 切换 | [ADAPTIVE_ENGINE_DESIGN.md](docs/planning/ADAPTIVE_ENGINE_DESIGN.md) |
| 2 | Propagator 框架重构 | [PROPAGATOR_FRAMEWORK_DESIGN.md](docs/planning/PROPAGATOR_FRAMEWORK_DESIGN.md) |

完整规划：[MODERNIZATION_PLAN_V2.md](docs/planning/MODERNIZATION_PLAN_V2.md)

---

## 已修复的 Bug

| # | 问题 | 位置 | 文档 |
|---|------|------|------|
| 1-3 | MAC get_solution 错误 | MAC.cpp | BUG_FIX_MAC_GET_SOLUTION.md |
| 4-6 | Trail 回溯缺陷 | unified_trail.cpp | UNIFIED_TRAIL_MEMO.md |
| 7 | GPU 二分回溯错误 | gmodel_solver.cpp | GPU_BINARY_BACKTRACK_FIX.md |

---

## 环境信息

**目标平台**: Jetson Orin Nano Super 8G

- OS: Ubuntu 22.04.5 LTS (kernel 5.15.148-tegra)
- CPU: ARM Cortex-A78AE 6-core
- GPU: NVIDIA Orin, CUDA 12.6
- Memory: 7.4 GiB (统一内存架构)

---

## 注意事项

- **通信语言**: 全程使用中文交流
- **变更日志**: 更新 `CHANGES_ZH.md`
- **测试实例**: `tests/data/bench/`（小型）, `benchmarks/`（大型）
- **第三方库**: `third_party/xcsp3parser` 包含在仓库中
