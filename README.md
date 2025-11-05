# CPIM - CUDA-accelerated Constraint Satisfaction Problem Solver

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++](https://img.shields.io/badge/C++-17-blue.svg)](https://isocpp.org/)
[![CUDA](https://img.shields.io/badge/CUDA-11.0+-green.svg)](https://developer.nvidia.com/cuda-toolkit)

English | [简体中文](README_ZH.md)

CPIM is a high-performance Constraint Satisfaction Problem (CSP) solver with both CPU and GPU parallel solving capabilities, implementing various arc consistency algorithms (AC3, SAC, RPC, etc.).

> **CPIM** = **C**onstraint **P**rogramming **I**n **M**emory computing

## ✨ Key Features

- 🚀 **Hybrid CPU/GPU Solving**: Supports both CPU (MAC) and GPU (CUDA) accelerated solving
- 📊 **Multiple AC Algorithms**: AC3, AC3bit, AC3rm, FC, SAC1, SAC3, RPC3, lMaxRPC, NSAC
- 📁 **XCSP3 Support**: Full support for XCSP3 format constraint problems
- ⚡ **High-Performance Parsing**: Using libxml2 + Abseil, 10x faster than Xerces-C
- 🎯 **Smart Heuristics**: Support for multiple variable and value ordering heuristics
- 🧩 **Modular Design**: Clear architecture, easy to extend with new algorithms
- 💾 **Bitset Optimization**: 64-bit bitset domain representation with efficient backtracking

## 🆕 Recent Updates (2025-10-16)

### ✅ Modernized XCSP3 Parser

We completed a comprehensive parser refactoring:

- **libxml2 replaces Xerces-C**: Library size reduced from ~10MB to ~1MB
- **Abseil C++ Integration**: Using Google's high-performance container library
- **Modern C++17**: RAII, smart pointers, strong-typed IDs, absl::Status
- **Fluent API**: ModelBuilder provides cleaner model construction interface
- **Performance Boost**: queens-4_ext.xml parsing time 1.26ms

See [MODERNIZATION_PLAN_V2.md](MODERNIZATION_PLAN_V2.md) for details.

## 📋 System Requirements

- **OS**: Linux (tested on Ubuntu 20.04+)
- **Compiler**: GCC 9.0+ (C++17 support required)
- **CUDA**: CUDA Toolkit 11.0+ (for GPU solving)
- **GPU**: NVIDIA GPU with Compute Capability 6.0+ (Pascal architecture or newer)

## 🔧 Dependencies

### Required Dependencies

```bash
# Ubuntu/Debian
sudo apt-get install -y \
    cmake \
    build-essential \
    libxml2-dev \
    libgoogle-glog-dev \
    libunwind-dev \
    pkg-config

# CUDA Toolkit (for GPU solving)
# Download from https://developer.nvidia.com/cuda-downloads
```

### Auto-Downloaded Dependencies

The following libraries are automatically downloaded via CMake FetchContent during build:

- **Abseil C++** (20240722.0 LTS) - Google's C++ common libraries
- ~~Xerces-C~~ (removed, replaced with libxml2)

### Optional Dependencies

- **GTest** - Unit testing (optional)
- **gflags** - Command-line flag parsing (optional)

## 🏗️ Build Instructions

### Quick Build

```bash
# Clone repository
git clone https://github.com/leezear2022/cpim.git
cd cpim

# Create build directory
mkdir build && cd build

# Configure and compile
cmake ..
make -j4

# Test new parser
./cpim_test_parser ../samples/bench/queens-4_ext.xml
```

### CMake Options

```bash
# Release mode (optimized build)
cmake -DCMAKE_BUILD_TYPE=Release ..

# Specify CUDA architectures
cmake -DCMAKE_CUDA_ARCHITECTURES="60;70;80" ..

# Verbose build output
make VERBOSE=1
```

## 🚀 Quick Start

### 1️⃣ Test New Parser

```bash
cd build
./cpim_test_parser ../samples/bench/queens-4_ext.xml
```

Expected output:
```
I1016 09:15:27.756088 Parse completed in 1.258555ms
Model: queens-4_ext
Variables: 4
Constraints: 6
Domains: 1
```

### 2️⃣ Using New Parser (C++ Code)

```cpp
#include "model/xcsp_parser.h"

using namespace cpim::model;

int main() {
  // Create parser
  auto parser = XcspParser::Create(ParserType::kLibXml2);

  // Parse model
  auto model_or = parser->Parse("benchmark.xml");
  if (!model_or.ok()) {
    LOG(ERROR) << model_or.status();
    return 1;
  }

  // Use model
  const auto& model = *model_or;
  LOG(INFO) << "Variables: " << model.variables().size();
  LOG(INFO) << "Constraints: " << model.constraints().size();

  return 0;
}
```

### 3️⃣ Legacy Solver (To Be Migrated)

```cpp
// Note: This code uses the old architecture and will be updated in future versions
#include "xcsp3model/HModel.h"
#include "Network.h"
#include "Solver.h"

HModel hm = HModelNode::Make();
// ... load model
Network net(hm);
MAC solver(net, AC_3bit, VRH_DOM_MIN, VLH_MIN);
solver.solve(900000); // 900-second timeout
```

## 🏛️ Architecture Overview

```
XCSP3 File
    ↓
┌─────────────────────────────────┐
│  LibXml2Parser (New)            │  ← Uses libxml2 + Abseil
│  - ModelBuilder (Fluent API)    │
│  - IntermediateModel (IR)       │
└─────────────────────────────────┘
    ↓
┌─────────────────────────────────┐
│  Adapters (Planned)             │
│  - NetworkAdapter (CPU)         │
│  - CModelAdapter (GPU)          │
└─────────────────────────────────┘
    ↓
┌──────────────┬──────────────────┐
│   Network    │     CModel       │
│  (CPU Model) │   (GPU Model)    │
└──────────────┴──────────────────┘
    ↓                  ↓
┌──────────────┬──────────────────┐
│     MAC      │   GPU Solver     │
│ (CPU Search) │  (GPU Search)    │
└──────────────┴──────────────────┘
```

### Core Modules

#### 📦 model/ (New Parser Module)

- **types.h/cpp** - Strong-typed ID definitions (DomainId, VariableId, ConstraintId)
- **xcsp_parser.h/cpp** - Parser abstract interface (Strategy pattern)
- **libxml2_parser.h/cpp** - libxml2 implementation (RAII resource management)
- **model_builder.h/cpp** - Fluent model builder (Builder pattern)
- **intermediate_model.h/cpp** - Intermediate model representation (Query interface)

#### 🧠 Core Solvers

- **HModel** - High-level constraint model (variables, constraints, domains)
- **Network** - Runtime constraint network (multi-level domain management, backtracking)
- **MAC** - Maintaining Arc Consistency search (CPU)
- **CModel** - CUDA constraint model (GPU parallel)

#### 🔍 Consistency Algorithms

| Algorithm | Type | Features |
|-----------|------|----------|
| AC3 | Arc Consistency | Classic algorithm |
| AC3bit | Arc Consistency | Bitset-optimized version |
| AC3rm | Arc Consistency | Residual maintenance |
| FC | Forward Checking | Lightweight filtering |
| FCbit | Forward Checking | Bitset-optimized version |
| SAC1 | Singleton Arc Consistency | Strong filtering |
| SAC3 | Singleton Arc Consistency | SAC optimized version |
| RPC3 | Path Consistency | Stronger consistency |
| lMaxRPC | Path Consistency | Limited MaxRPC |
| NSAC | Neighborhood SAC | Neighborhood singleton consistency |

## 📂 Project Structure

```
cpim/
├── include/              # Header files
│   ├── model/           # New parser module (Modern C++17)
│   │   ├── types.h      # Strong-typed IDs, domains, variables, constraints
│   │   ├── xcsp_parser.h
│   │   ├── libxml2_parser.h
│   │   ├── model_builder.h
│   │   └── intermediate_model.h
│   ├── xcsp3model/      # High-level model
│   │   └── HModel.h
│   ├── Network.h        # Runtime network
│   ├── Solver.h         # Solvers and algorithms
│   ├── cuSAC.cuh        # GPU solver
│   └── ...
├── src/                 # Implementation files
│   ├── model/          # New parser module implementation
│   ├── HModel.cpp
│   ├── Network.cpp
│   ├── MAC.cpp         # CPU search
│   ├── AC*.cpp         # Various AC algorithms
│   ├── SAC*.cpp        # SAC algorithms
│   ├── RPC*.cpp        # RPC algorithms
│   ├── cuSAC.cu        # GPU solver
│   └── ...
├── samples/             # Sample programs
│   ├── main_new_parser.cpp  # New parser test
│   └── bench/          # Test benchmarks
│       ├── queens-4_ext.xml
│       ├── queens-12_ext.xml
│       └── ...
├── test/                # Test utilities
│   ├── test_multilevel.cpp  # Multi-level functionality test
│   ├── gmodel_solver.cpp    # GModel MAC solver
│   ├── debug_bitsup.cpp     # Constraint table debugger
│   └── test_cpu_mac.cpp     # CPU MAC test
├── deprecated/          # Deprecated code
│   ├── xcsp3model/     # Old Xerces-C parser
│   └── README.md       # Migration guide
├── xcsp3parser/         # XCSP3 parser library (submodule)
├── CMakeLists.txt
├── CLAUDE.md           # Claude Code project guide
├── MODERNIZATION_PLAN_V2.md  # Modernization plan
└── README.md           # This file
```

## 🎯 Heuristic Strategies

### Variable Ordering Heuristics

- `VRH_DOM_MIN` - Minimum domain first (default)
- `VRH_DOM_WDEG_MIN` - Minimum weighted-degree/domain ratio
- `VRH_DOM_DEG_MIN` - Minimum degree/domain ratio

### Value Ordering Heuristics

- `VLH_MIN` - Minimum value first (default)

## 📊 Benchmarks

The project includes several XCSP3 format benchmarks:

```bash
samples/bench/
├── queens-4_ext.xml       # 4-Queens problem
├── queens-12_ext.xml      # 12-Queens problem
├── haystacks-11_ext.xml   # Haystacks problem
└── BMPath.xml             # Benchmark path (indirect reference)
```

### Performance Examples

| Benchmark | Parse Time | Solver | Notes |
|-----------|-----------|--------|-------|
| queens-4_ext.xml | 1.26ms | LibXml2Parser | 4 variables, 6 constraints |

## 🗺️ Development Roadmap

### ✅ Phase 0: Modernized Parser (Completed)

- [x] Replace Xerces-C with libxml2
- [x] Integrate Abseil C++
- [x] Strong-typed ID system
- [x] ModelBuilder fluent API
- [x] IntermediateModel intermediate representation
- [x] Test program cpim_test_parser

### 🔄 Phase 1: Adapter Layer (Planned)

- [ ] IntermediateModel → Network adapter (CPU solver)
- [ ] IntermediateModel → CModel adapter (GPU solver)
- [ ] Integration into MAC and GPU solving pipelines

### 🔮 Phase 2: Trail Backtracking System (Planned)

- [ ] Replace full-copy backtracking with incremental trail system
- [ ] Expected performance gain: 30-50%

### 🚀 Phase 3: Advanced Features (Future)

- [ ] Learning heuristics (VSIDS, CHB)
- [ ] Parallel search
- [ ] Lazy clause generation
- [ ] More global constraints

See [MODERNIZATION_PLAN_V2.md](MODERNIZATION_PLAN_V2.md) for details.

## 📚 Documentation

- **[CLAUDE.md](CLAUDE.md)** - Project overview and development guide (Chinese)
- **[MODERNIZATION_PLAN_V2.md](MODERNIZATION_PLAN_V2.md)** - Detailed modernization plan
- **[MODERNIZATION_MEMO.md](MODERNIZATION_MEMO.md)** - Quick reference and action checklist
- **[deprecated/README.md](deprecated/README.md)** - Legacy code migration guide
- **[GPU_JETSON_ADAPTATION.md](aig_docs/GPU_JETSON_ADAPTATION.md)** - Jetson (CUDA 12) adaptation and optimization plan
- **[GPU_ALGORITHM_OVERVIEW.md](aig_docs/GPU_ALGORITHM_OVERVIEW.md)** - GPU solver algorithm and data structure overview

## 🤝 Contributing

Contributions are welcome! If you'd like to contribute to CPIM:

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/AmazingFeature`)
3. Commit your changes (`git commit -m 'Add some AmazingFeature'`)
4. Push to the branch (`git push origin feature/AmazingFeature`)
5. Open a Pull Request

### Code Standards

- Follow existing code style
- Add tests for new features
- Use modern C++17 features
- Prefer Abseil containers and utilities
- Add clear comments (English or Chinese)

## 📄 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## 👨‍💻 Author

- **Lee** - [leezear2022@github](https://github.com/leezear2022)

## 🙏 Acknowledgments

- **XCSP3** - Constraint problem standard format
- **Abseil** - Google's C++ common libraries
- **libxml2** - High-performance XML parser
- **CUDA** - NVIDIA parallel computing platform
- **Claude Code** - AI-assisted development tool

## 📞 Contact

- **Issues**: [https://github.com/leezear2022/cpim/issues](https://github.com/leezear2022/cpim/issues)
- **Email**: leezear@live.cn

---

<div align="center">

**⭐ If this project helps you, please give us a Star! ⭐**

Made with ❤️ by Lee | Powered by CUDA & Modern C++

</div>
