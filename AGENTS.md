# Repository Guidelines

## Project Structure & Module Organization
The solver core lives in `src/`, mixing CPU algorithms (`AC3.cpp`, `MAC.cpp`, `FC.cpp`) with GPU kernels (`cuSAC.cu`) and the modern parser stack under `src/model/`. Public interfaces stay mirrored in `include/`. XCSP2 benchmarks and manifests sit in `samples/bench/`; store new `.xml` cases there and keep `BMPath.xml` style manifests close to their batches. Use `build/` for generated artifacts; avoid touching `deprecated/` and `xcsp3parser/` unless migrating legacy logic.

## Build, Test, and Development Commands
- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` for a clean Release configure; add `-DCMAKE_CUDA_ARCHITECTURES="60;70;80"` when tuning GPU binaries.
- `cmake --build build -j$(nproc)` builds every target, including the parser test harness.
- `./build/cpim_test_parser --list_only` prints resolved bench files and their XCSP format; add `--bench_path samples/bench` to scan an entire directory or `--bench_manifest samples/bench/BMPath.xml` for curated manifests.
- `./build/cpim_test_parser` (without flags) parses the first discovered XCSP2 instance and dumps stats for smoke validation.

## Coding Style & Naming Conventions
Write Modern C++17 with RAII and Abseil helpers where useful. Follow the prevailing two-space indentation, same-line braces, and keep lines under roughly 100 characters. Use `PascalCase` for types, lower_snake_case for functions that mirror STL semantics (`push_back`), and ALL_CAPS for constants. Prefer `absl::Status` / `StatusOr` over raw error codes, and document host/device boundaries in CUDA code when behavior diverges.

## Testing Guidelines
Lean on `cpim_test_parser` for quick regression checks: point it at new manifests or individual `.xml` files and confirm the reported XCSP format matches expectations. When adding unit suites, wire GoogleTest through CMake and register with `add_test` so `ctest --output-on-failure` under `build/` exercises them. Log solver runtime or memory shifts in merge notes whenever propagation loops or CUDA kernels change.

## Commit & Pull Request Guidelines
Keep commit subjects concise (≤60 characters) and action oriented; Mandarin phrasing that matches the existing history is fine. Group related edits together, describe algorithmic intent in the body, and attach parser/solver output snippets or performance deltas for non-trivial changes. Pull requests should call out touched areas (`src/model`, `samples/bench`, etc.) and link tracked issues when applicable.

## Documentation & Collaboration
Refer to `CHANGES_ZH.md` for the latest中文修改清单，并在该文件中持续更新新增改动。全程用中文交流。

优化文档入口：面向 Jetson Orin 的 GPU 侧优化与迁移方案以 `aig_docs/GPU_JETSON_ADAPTATION.md` 为总入口，内含推荐方案与各专题链接（如 `aig_docs/GPU_GAC_PERSISTENT_STATE_MACHINE.md`、`aig_docs/GPU_GAC_PIPELINE_PLAN.md`）。

## 环境信息（Jetson Orin Nano Super 8G）
- 操作系统：Ubuntu 22.04.5 LTS（内核 `5.15.148-tegra`，`uname -a`）
- CPU：ARM Cortex-A78AE 六核（单线程/核，最高 1.73 GHz，`lscpu`）
- GPU：NVIDIA Orin (nvgpu)，驱动 540.4.0，CUDA 12.6（`nvidia-smi`）
- 内存：7.4 GiB 总计，约 2.9 GiB 已用（`free -h`）
- 系统盘：/dev/nvme0n1p1，约 937 GB 可用 854 GB（`df -h /`）

## GPU Solver Overview

### CModel（传统 GPU 求解器）
- GPU 代码集中于 `include/cuSAC.cuh`, `src/cuSAC.cu`，入口类为 `cpim::CModel`，通过 `CModel::CModel(const HModel&)` 从 HModel 导入变量、约束。
- `BuildBitModel` 将 HModel 的域与表约束编码成 GPU bitset/纹理，初始化订阅结构与设备常量（变量度、域尺寸、约束邻接）。
- `solve()` 先执行 `enforceGAC()` 在 GPU 上做全局弧一致性，随后使用 dom/deg 启发式选择变量，借助 `AssignValue`/`RemoveValue` 等 CUDA kernel 做赋值与回溯，整个传播通过 `CsCheckMain` 系列 kernel 实现。
- 内核关键概念：`bitDom` 表示变量域的位图、`bitSup` 存储约束支持集合、`subscription` 记录变量参与的约束；所有结构通过 `thrust::device_vector` 与 `__managed__` 内存管理。
- 当前实现紧耦合 HModel：仍依赖 HModel 提供的变量映射、约束列表与订阅信息；若要换用 IntermediateModel，需要补全这些派生数据并重写 `CModel` 构造流程。

### GModel（简化的统一内存 GPU 模型）
- **位置**：`include/GModel.cuh`, `src/GModel.cu`，主程序 `samples/dump_gmodel.cpp`
- **设计原则**：
  - 统一内存（Unified Memory）：所有数据使用 `cudaMallocManaged`，利用 Jetson Orin 的集成统一内存架构
  - 最小化设计：只包含核心的 `bitDom`（变量域位图）和 `bitSup`（约束支持位图）
  - 零拷贝访问：CPU 和 GPU 共享同一块物理内存，无需显式数据传输
- **构建流程**：`GModel::GModel(const IntermediateModel&)` 从 IntermediateModel 构造，无需依赖 HModel
- **数据布局**：
  - `bitDom[var_id * bit_dom_int_size + word_idx]`：变量域的 32 位字数组
  - `bitSup[c * bitsup_per_constraint + ...]`：二元约束的支持位集，使用 `uint2` 存储两个变量方向的支持
- **GPU 验证**：`VerifyOnGPU()` 方法启动 GPU kernel 读取数据并通过 `printf` 输出，验证统一内存的 CPU/GPU 一致性
- **Jetson 适配**：自动检测设备的 `concurrentManagedAccess` 属性，对于 Jetson Orin（值为 0），跳过不支持的 `cudaMemPrefetchAsync` 调用
- **测试工具**：`build/dump_gmodel --input=<path_to_xml> [--max_print=N]` 可解析 XCSP 文件、构建 GModel、打印数据结构并在 GPU 上验证

### 优化与迁移文档
- Jetson 适配与优化规划见：`aig_docs/GPU_JETSON_ADAPTATION.md`。
- 算法与数据结构详情：`aig_docs/GPU_ALGORITHM_OVERVIEW.md`。
