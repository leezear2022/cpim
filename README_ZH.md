# CPIM - CUDA-accelerated Constraint Satisfaction Problem Solver

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++](https://img.shields.io/badge/C++-17-blue.svg)](https://isocpp.org/)
[![CUDA](https://img.shields.io/badge/CUDA-11.0+-green.svg)](https://developer.nvidia.com/cuda-toolkit)

[English](README.md) | 简体中文

CPIM 是一个高性能约束满足问题（CSP）求解器，支持 CPU 和 GPU 并行求解，实现了多种弧一致性算法（AC3, SAC, RPC 等）。

> **CPIM** = **C**onstraint **P**rogramming **I**n **M**emory computing (存算一体约束编程求解器)

## ✨ 主要特性

- 🚀 **CPU/GPU 混合求解**：同时支持 CPU (MAC) 和 GPU (CUDA) 加速求解
- 📊 **多种 AC 算法**：AC3, AC3bit, AC3rm, FC, SAC1, SAC3, RPC3, lMaxRPC, NSAC
- 📁 **XCSP3 支持**：完整支持 XCSP3 格式约束问题
- ⚡ **高性能解析**：使用 libxml2 + Abseil，比 Xerces-C 快 10 倍
- 🎯 **智能启发式**：支持多种变量和值选择启发式
- 🧩 **模块化设计**：清晰的架构，易于扩展新算法
- 💾 **位集优化**：使用 64 位位集表示域，高效的回溯机制

## 🆕 最近更新 (2025-10-16)

### ✅ 现代化 XCSP3 解析器

我们完成了解析器的全面重构：

- **libxml2 替代 Xerces-C**：库体积从 ~10MB 降至 ~1MB
- **Abseil C++ 集成**：使用 Google 的高性能容器库
- **现代 C++17**：RAII、智能指针、强类型 ID、absl::Status
- **流式 API**：ModelBuilder 提供更简洁的模型构建接口
- **性能提升**：queens-4_ext.xml 解析时间 1.26ms

详见 [MODERNIZATION_PLAN_V2.md](MODERNIZATION_PLAN_V2.md)

## 📋 系统要求

- **操作系统**：Linux (测试于 Ubuntu 20.04+)
- **编译器**：GCC 9.0+ (支持 C++17)
- **CUDA**：CUDA Toolkit 11.0+ (GPU 求解)
- **GPU**：NVIDIA GPU with Compute Capability 6.0+ (Pascal 架构及以上)

## 🔧 依赖项

### 必需依赖

```bash
# Ubuntu/Debian
sudo apt-get install -y \
    cmake \
    build-essential \
    libxml2-dev \
    libgoogle-glog-dev \
    libunwind-dev \
    pkg-config

# CUDA Toolkit (GPU 求解)
# 从 https://developer.nvidia.com/cuda-downloads 下载安装
```

### 自动下载依赖

以下库会在构建时通过 CMake FetchContent 自动下载：

- **Abseil C++** (20240722.0 LTS) - Google 的 C++ 公共库
- ~~Xerces-C~~ (已移除，使用 libxml2 替代)

### 可选依赖

- **GTest** - 单元测试（可选）
- **gflags** - 命令行参数解析（可选）

## 🏗️ 构建说明

### 快速构建

```bash
# 克隆仓库
git clone https://github.com/leezear2022/cpim.git
cd cpim

# 创建构建目录
mkdir build && cd build

# 配置和编译
cmake ..
make -j4

# 测试新解析器
./cpim_test_parser ../samples/bench/queens-4_ext.xml
```

### CMake 选项

```bash
# Release 模式（优化编译）
cmake -DCMAKE_BUILD_TYPE=Release ..

# 指定 CUDA 架构
cmake -DCMAKE_CUDA_ARCHITECTURES="60;70;80" ..

# 详细构建输出
make VERBOSE=1
```

## 🚀 快速开始

### 1️⃣ 测试新解析器

```bash
cd build
./cpim_test_parser ../samples/bench/queens-4_ext.xml
```

输出示例：
```
I1016 09:15:27.756088 Parse completed in 1.258555ms
Model: queens-4_ext
Variables: 4
Constraints: 6
Domains: 1
```

### 2️⃣ 使用新解析器（C++ 代码）

```cpp
#include "model/xcsp_parser.h"

using namespace cpim::model;

int main() {
  // 创建解析器
  auto parser = XcspParser::Create(ParserType::kLibXml2);

  // 解析模型
  auto model_or = parser->Parse("benchmark.xml");
  if (!model_or.ok()) {
    LOG(ERROR) << model_or.status();
    return 1;
  }

  // 使用模型
  const auto& model = *model_or;
  LOG(INFO) << "Variables: " << model.variables().size();
  LOG(INFO) << "Constraints: " << model.constraints().size();

  return 0;
}
```

### 3️⃣ 旧求解器（即将迁移）

```cpp
// 注意：此代码使用旧架构，将在未来版本中更新
#include "xcsp3model/HModel.h"
#include "Network.h"
#include "Solver.h"

HModel hm = HModelNode::Make();
// ... 加载模型
Network net(hm);
MAC solver(net, AC_3bit, VRH_DOM_MIN, VLH_MIN);
solver.solve(900000); // 900秒超时
```

## 🏛️ 架构概览

```
XCSP3 文件
    ↓
┌─────────────────────────────────┐
│  LibXml2Parser (新)             │  ← 使用 libxml2 + Abseil
│  - ModelBuilder (流式 API)      │
│  - IntermediateModel (中间表示) │
└─────────────────────────────────┘
    ↓
┌─────────────────────────────────┐
│  适配器 (计划中)                 │
│  - NetworkAdapter (CPU)         │
│  - CModelAdapter (GPU)          │
└─────────────────────────────────┘
    ↓
┌──────────────┬──────────────────┐
│   Network    │     CModel       │
│  (CPU 模型)  │   (GPU 模型)     │
└──────────────┴──────────────────┘
    ↓                  ↓
┌──────────────┬──────────────────┐
│     MAC      │   GPU Solver     │
│  (CPU 搜索)  │   (GPU 搜索)     │
└──────────────┴──────────────────┘
```

### 核心模块

#### 📦 model/ (新解析模块)

- **types.h/cpp** - 强类型 ID 定义（DomainId, VariableId, ConstraintId）
- **xcsp_parser.h/cpp** - 解析器抽象接口（策略模式）
- **libxml2_parser.h/cpp** - libxml2 实现（RAII 资源管理）
- **model_builder.h/cpp** - 流式模型构建器（Builder 模式）
- **intermediate_model.h/cpp** - 中间模型表示（查询接口）

#### 🧠 核心求解器

- **HModel** - 高层约束模型（变量、约束、域）
- **Network** - 运行时约束网络（多层域管理、回溯）
- **MAC** - 维持弧一致性搜索（CPU）
- **CModel** - CUDA 约束模型（GPU 并行）

#### 🔍 一致性算法

| 算法 | 类型 | 特点 |
|------|------|------|
| AC3 | 弧一致性 | 经典算法 |
| AC3bit | 弧一致性 | 位集优化版本 |
| AC3rm | 弧一致性 | 残差维护 |
| FC | 前向检查 | 轻量级过滤 |
| FCbit | 前向检查 | 位集优化版本 |
| SAC1 | 单态弧一致性 | 强过滤 |
| SAC3 | 单态弧一致性 | SAC 优化版本 |
| RPC3 | 路径一致性 | 更强的一致性 |
| lMaxRPC | 路径一致性 | 限制 MaxRPC |
| NSAC | 邻居 SAC | 邻域单态一致性 |

## 📂 项目结构

```
cpim/
├── include/              # 头文件
│   ├── model/           # 新解析模块（现代 C++17）
│   │   ├── types.h      # 强类型 ID、域、变量、约束
│   │   ├── xcsp_parser.h
│   │   ├── libxml2_parser.h
│   │   ├── model_builder.h
│   │   └── intermediate_model.h
│   ├── xcsp3model/      # 高层模型
│   │   └── HModel.h
│   ├── Network.h        # 运行时网络
│   ├── Solver.h         # 求解器和算法
│   ├── cuSAC.cuh        # GPU 求解器
│   └── ...
├── src/                 # 实现文件
│   ├── model/          # 新解析模块实现
│   ├── HModel.cpp
│   ├── Network.cpp
│   ├── MAC.cpp         # CPU 搜索
│   ├── AC*.cpp         # 各种 AC 算法
│   ├── SAC*.cpp        # SAC 算法
│   ├── RPC*.cpp        # RPC 算法
│   ├── cuSAC.cu        # GPU 求解器
│   └── ...
├── samples/             # 示例程序
│   ├── main_new_parser.cpp  # 新解析器测试
│   └── bench/          # 测试基准
│       ├── queens-4_ext.xml
│       ├── queens-12_ext.xml
│       └── ...
├── test/                # 测试工具
│   ├── test_multilevel.cpp  # 多层级功能测试
│   ├── gmodel_solver.cpp    # GModel MAC 求解器
│   ├── debug_bitsup.cpp     # 约束表调试工具
│   └── test_cpu_mac.cpp     # CPU MAC 测试
├── deprecated/          # 已弃用代码
│   ├── xcsp3model/     # 旧 Xerces-C 解析器
│   └── README.md       # 迁移指南
├── xcsp3parser/         # XCSP3 解析器库（子模块）
├── CMakeLists.txt
├── CLAUDE.md           # Claude Code 项目说明
├── MODERNIZATION_PLAN_V2.md  # 现代化计划
└── README.md           # 本文件
```

## 🎯 启发式策略

### 变量选择启发式

- `VRH_DOM_MIN` - 最小域优先（默认）
- `VRH_DOM_WDEG_MIN` - 加权度最小域优先
- `VRH_DOM_DEG_MIN` - 度最小域优先

### 值选择启发式

- `VLH_MIN` - 最小值优先（默认）

## 📊 基准测试

项目包含多个 XCSP3 格式的基准测试：

```bash
samples/bench/
├── queens-4_ext.xml       # 4-皇后问题
├── queens-12_ext.xml      # 12-皇后问题
├── haystacks-11_ext.xml   # Haystacks 问题
└── BMPath.xml             # 基准路径（间接引用）
```

### 性能示例

| 基准 | 解析时间 | 求解器 | 备注 |
|------|---------|--------|------|
| queens-4_ext.xml | 1.26ms | LibXml2Parser | 4个变量, 6个约束 |

## 🗺️ 开发路线图

### ✅ Phase 0: 现代化解析器（已完成）

- [x] libxml2 替换 Xerces-C
- [x] Abseil C++ 集成
- [x] 强类型 ID 系统
- [x] ModelBuilder 流式 API
- [x] IntermediateModel 中间表示
- [x] 测试程序 cpim_test_parser

### 🔄 Phase 1: 适配器层（计划中）

- [ ] IntermediateModel → Network 适配器（CPU 求解器）
- [ ] IntermediateModel → CModel 适配器（GPU 求解器）
- [ ] 集成到 MAC 和 GPU 求解流程

### 🔮 Phase 2: Trail 回溯系统（计划中）

- [ ] 替换完整拷贝回溯为增量 trail 系统
- [ ] 预期性能提升：30-50%

### 🚀 Phase 3: 高级特性（未来）

- [ ] 学习启发式（VSIDS, CHB）
- [ ] 并行搜索
- [ ] Lazy clause generation
- [ ] 更多全局约束

详见 [MODERNIZATION_PLAN_V2.md](MODERNIZATION_PLAN_V2.md)

## 📚 文档

- **[CLAUDE.md](CLAUDE.md)** - 项目概览和开发指南（中文）
- **[MODERNIZATION_PLAN_V2.md](MODERNIZATION_PLAN_V2.md)** - 现代化详细计划
- **[MODERNIZATION_MEMO.md](MODERNIZATION_MEMO.md)** - 快速参考和行动清单
- **[deprecated/README.md](deprecated/README.md)** - 旧代码迁移指南
- **[GPU_JETSON_ADAPTATION.md](aig_docs/GPU_JETSON_ADAPTATION.md)** - Jetson (CUDA 12) 适配与优化规划
- **[GPU_ALGORITHM_OVERVIEW.md](aig_docs/GPU_ALGORITHM_OVERVIEW.md)** - GPU 求解器算法与数据结构概览

## 🤝 贡献

欢迎贡献！如果您想为 CPIM 做出贡献：

1. Fork 本仓库
2. 创建特性分支 (`git checkout -b feature/AmazingFeature`)
3. 提交更改 (`git commit -m 'Add some AmazingFeature'`)
4. 推送到分支 (`git push origin feature/AmazingFeature`)
5. 开启 Pull Request

### 代码规范

- 遵循现有代码风格
- 新功能需要添加测试
- 使用现代 C++17 特性
- 优先使用 Abseil 容器和工具
- 添加清晰的注释（中英文均可）

## 📄 许可证

本项目采用 MIT 许可证 - 详见 [LICENSE](LICENSE) 文件

## 👨‍💻 作者

- **Lee** - [leezear2022@github](https://github.com/leezear2022)

## 🙏 致谢

- **XCSP3** - 约束问题标准格式
- **Abseil** - Google 的 C++ 公共库
- **libxml2** - 高性能 XML 解析器
- **CUDA** - NVIDIA 并行计算平台
- **Claude Code** - AI 辅助开发工具

## 📞 联系方式

- **Issues**: [https://github.com/leezear2022/cpim/issues](https://github.com/leezear2022/cpim/issues)
- **Email**: leezear@live.cn

---

<div align="center">

**⭐ 如果这个项目对您有帮助，请给我们一个 Star！⭐**

Made with ❤️ by Lee | Powered by CUDA & Modern C++

</div>
