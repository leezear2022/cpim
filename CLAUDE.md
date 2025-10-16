# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

CPIM is a CUDA-accelerated Constraint Satisfaction Problem (CSP) solver that implements various arc consistency algorithms (AC3, FC, SAC, RPC, etc.) with both CPU and GPU implementations. The project parses XCSP3 format constraint problems and solves them using MAC (Maintaining Arc Consistency) search with different consistency enforcement algorithms.

## Build System

### Build Commands

```bash
# Build the project
mkdir build && cd build
cmake ..
make

# The main executable is 'cpim' which will be in the build directory
```

### Dependencies

- **CUDA Toolkit**: Required for GPU acceleration (CUDA 11.0+)
  - CUDA architectures: 60, 70, 80
  - Location: `/usr/local/cuda`
- **libxml2**: XML parsing for XCSP3 files
- **Xerces-C**: XML processing
- **GTest**: Testing framework
- **gflags**: Command-line flag parsing
- **glog**: Logging framework
- **libunwind**: Stack unwinding
- **Abseil**: Google's C++ common libraries

### Build Configuration

The project uses CMake with CUDA support. Key build settings:
- C++ Standard: C++20
- CUDA Standard: C++20
- Main CMakeLists.txt at root builds both the solver and xcsp3parser submodule

## Architecture

### Core Components

1. **HModel (Hierarchical Model)** - `include/xcsp3model/HModel.h`
   - High-level constraint model representation
   - `HVar`: Variable wrapper with domains
   - `HTab`: Constraint table wrapper
   - `HModel`: Complete problem model with variables, constraints, subscriptions, and neighborhood structure

2. **Network** - `include/Network.h`, `src/Network.cpp`
   - Runtime constraint network with domain tracking across search levels
   - `IntVar`: Variables with multi-level domain support (bitset-based)
   - `Tabular`: Table constraints with tuple storage
   - `Network`: Manages variables, constraints, subscriptions, and search levels
   - Supports backtracking with `NewLevel()`, `BackTo()`, `ClearLevel()`

3. **Arc Consistency Algorithms** - `include/Solver.h`, `src/AC*.cpp`
   - Base class: `AC` with virtual `enforce()` method
   - Implementations: `AC3`, `AC3bit`, `AC3rm`, `FC`, `FCbit`
   - SAC variants: `SAC1`, `SAC3` (singleton arc consistency)
   - Path consistency: `lMaxRPC`, `RPC3`
   - Neighborhood AC: `NSAC`

4. **Search** - `src/MAC.cpp`, `include/Solver.h`
   - `MAC` class implements the main search algorithm
   - Supports multiple variable heuristics: `VRH_DOM_MIN`, `VRH_DOM_WDEG_MIN`, etc.
   - Supports multiple value heuristics: `VLH_MIN`, etc.
   - Tracks search statistics (nodes, time, solutions)

5. **GPU Solver** - `include/cuSAC.cuh`, `src/cuSAC.cu`
   - `CModel`: CUDA-based constraint model
   - GPU-accelerated GAC (Generalized Arc Consistency) enforcement
   - Uses texture memory for constraint storage
   - Bitset representation for domains on GPU
   - Multi-level support for backtracking on GPU

6. **XCSP3 Parser** - `xcsp3parser/` subdirectory
   - Third-party parser for XCSP3 XML format
   - Integrated as a static library
   - `XBuilder` class (in `include/xcsp3model/XBuilder.h`) bridges parser to HModel

### Data Flow

```
XCSP3 XML file
  → XBuilder (parses and builds HModel)
  → HModel (high-level model)
  → Network (runtime network with multi-level domains)
  → MAC search with AC algorithm
    ├→ CPU: AC3/AC3bit/FC/SAC/RPC/NSAC
    └→ GPU: CModel (parallel GAC enforcement)
  → Solution or statistics
```

### Key Design Patterns

1. **Multi-level Domain Management**: Variables maintain separate domain copies for each search level to enable efficient backtracking
2. **Bitset Optimization**: Domains and supports use bitsets (64-bit) for fast operations
3. **Subscription Architecture**: Each variable maintains a list of constraints that involve it
4. **Hybrid CPU/GPU**: Same problem can be solved on CPU (MAC class) or GPU (CModel class)

## Common Development Tasks

### Running the Solver

The main entry point is `samples/main.cu`. Current configuration:
- Default benchmark: `../samples/bench/BMPath.xml`
- Time limit: 900 seconds (900000ms)
- Default algorithm: `AC_3bit` with `VRH_DOM_MIN` / `VLH_MIN`

To run:
```bash
./cpim
```

### Testing Different Algorithms

Edit `samples/main.cu` to change the AC algorithm:
```cpp
// Available algorithms (from Solver.h):
// AC_3, AC_3bit, AC_3rm, A_FC, A_FC_bit, A_NSAC, CA_LMRPC_BIT, CA_RPC3
MAC mac(n, AC_3bit, Heuristic::VRH_DOM_MIN, Heuristic::VLH_MIN);
```

### Adding New Arc Consistency Algorithms

1. Inherit from `AC` base class in `include/Solver.h`
2. Implement `ConsistencyState enforce(vector<IntVar*>& x_evt, int level)` method
3. Add algorithm to `ACAlgorithm` enum
4. Create implementation in `src/YourAlgorithm.cpp`

### Working with Benchmarks

Sample benchmarks are in `samples/bench/`:
- Queens problems: `queens-4_ext.xml`, `queens-12_ext.xml`
- Other instances: `haystacks-11_ext.xml`, `BMPath.xml`

To use a different benchmark, modify `X_PATH` in `samples/main.cu`.

## Important Implementation Details

### Bitset Domain Representation

- `BITSIZE = 64`: Each bitset word is 64 bits
- `DIV_BIT = 6`: Divide by 64 using right shift by 6
- `MOD_MASK = 0x3f`: Get bit offset within word using AND with 63
- Functions: `GetBitIdx()`, `FirstOne()` in `Network.h`

### Search Level Management

- Level 0: Initial problem state
- Each assignment creates a new level
- `Network::NewLevel(src)`: Copy level src to new top level
- `Network::BackTo(dest)`: Backtrack to level dest
- `IntVar` maintains `bit_doms_[level]` for each level's domain

### GPU Architecture

- Constants in device memory: `kDeviceBitDomIntSize`, `kDeviceMaxDomSize`, etc.
- Texture memory used for constraint tables (read-only)
- Unified memory (`__managed__`) for some data structures
- CUDA kernels in `src/cuSAC.cu`

### Constraint Representation

- Extension constraints stored as tables (`Tabular`)
- Intension constraints converted to tables via `HModel` expression evaluation
- Binary constraints optimized with bitset supports

## File Organization

- `include/`: All header files
  - `xcsp3model/`: High-level model classes (HModel, HVar, HTab, XBuilder)
  - `xcsp3parser/`: Parser headers (copied from submodule)
  - Root headers: Network, Solver, Timer, utility headers
- `src/`: Implementation files
  - AC algorithm implementations: `AC.cpp`, `AC3.cpp`, `AC3bit.cpp`, `AC3rm.cpp`, `FC.cpp`
  - SAC implementations: `SAC1.cpp`, `SAC3.cpp`, `NSAC.cpp`
  - Path consistency: `lMaxRPC.cpp`, `RPC3.cpp`
  - Search: `MAC.cpp`, `Solver.cpp`
  - Model building: `HModel.cpp`, `XBuilder.cpp`, `Network.cpp`
  - GPU: `cuSAC.cu`
- `samples/`: Main entry point and benchmark files
- `xcsp3parser/`: Submodule for XCSP3 parsing

## Modernization Plans

The project is undergoing a comprehensive modernization effort to transform it into an industrial-grade constraint solver library, following best practices from Google OR-Tools.

### Planning Documents

1. **`MODERNIZATION_MEMO.md`** - Quick Reference (Start Here)
   - Concise action guide (6 pages)
   - First week action plan with specific commands
   - Quick links to all resources
   - Progress tracking checklist

2. **`MODERNIZATION_PLAN_V2.md`** - Complete Plan (Detailed Guide)
   - Based on Google OR-Tools best practices
   - 7-phase roadmap (Phase 0-6 + ongoing Phase 7)
   - Detailed code examples and architecture designs
   - Technology stack: C++20 + Abseil + GoogleTest + Modern CMake/Bazel

3. **`MODERNIZATION_PLAN.md`** - Original Plan (Reference)
   - Initial 6-phase modernization plan
   - Baseline version before OR-Tools study
   - Useful for comparison

### Key Modernization Goals

1. **Modular Architecture**: Clear separation (base/model/solver/algorithms)
2. **Dependency Injection**: Following OR-Tools pattern for flexible propagator system
3. **Fluent API**: ModelBuilder with method chaining for ease of use
4. **Trail-based Backtracking**: Replace full-copy backtracking with incremental trail system (30-50% performance gain)
5. **Type Safety**: Smart pointers, `std::optional`, `absl::Status`, strong enum classes
6. **Test Coverage**: GoogleTest framework with 80%+ coverage target

### Technology Stack Upgrades

- **Abseil**: Google's C++ library (`absl::Span`, `absl::flat_hash_map`, `absl::Status`)
- **Modern CMake**: Target-based dependency management
- **Bazel**: Optional secondary build system
- **GoogleTest + Google Benchmark**: Comprehensive testing

### Reference Documents

- `aig_docs/cpu_modernization_overview.md` - Modernization suggestions summary
- `aig_docs/cpim_modernization_plan.md` - Initial modernization ideas
- `aig_docs/CPIM_COMPREHENSIVE_REFACTORING_GUIDE.md` - Detailed refactoring guide

### Current Status

- [x] Phase 0 Planning Complete
- [ ] Phase 0: Technology Stack Upgrade (Next Step)
- [ ] Phase 1: Modular Refactoring
- [ ] Phase 2: Dependency Injection Framework
- [ ] Phase 3: Trail Backtracking System
- [ ] Phase 4: Search Engine & Heuristics
- [ ] Phase 5: Modern CMake + Bazel
- [ ] Phase 6: API Design & Examples
- [ ] Phase 7: Advanced Features (Ongoing)

## Notes

- **Communication Language**: Always communicate with the user in Chinese (全程使用中文交流)
- The codebase contains both Chinese and English comments
- Some code is commented out in CMakeLists.txt and main.cu (older configurations)
- Recent commits focus on heuristic kernels and solver testing
- The GPU solver (CModel) and CPU solver (MAC) can solve the same problem for comparison
