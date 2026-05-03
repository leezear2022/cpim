---
status: active
updated: 2026-05-03
---

# macOS Metal 统一内存迁移计划

> 目标：在不破坏现有 Jetson/CUDA 路径的前提下，为 Apple Silicon Mac 增加一条
> Metal 后端路线。Metal v1 已先以 GAC-only MVP 验证 Metal compute 与统一内存
> 数据布局；Metal v1.1-v1.8 将其加固为可自动回归、可消融、可批量扫描的 correctness backend，
> 后续再评估 SAC/Batch 路径。

## 1. 结论与边界

- 迁移策略采用 **CUDA 与 Metal 并存**，不做“一次性替换 CUDA”。
- Metal v1 采用 **独立 MetalGacSolver + 后端无关 DeviceLayout**：解析 XCSP3、
  构建共享位集布局、运行普通 Bitmap GAC，并与 CPU GAC baseline 对齐。
- Metal v1.2 已抽出 Metal runtime 封装；v1.3 增加 GAC benchmark 与 CSV 观测，
  但不进入 storage mode 优化、simdgroup/texture 优化或 SAC 数据流。
- Metal v1.4/v1.5 增加 readonly shared/private storage 与 flags/compact frontier
  两组可回退消融；默认仍保持 shared + flags。
- Metal v1.6 借用既有 tier0~3 用例分级，新增批量 ablation 扫描脚本，用 CSV 记录
  storage/frontier 组合在更大样例面上的 correctness 与耗时趋势。
- Metal v1.7 补齐 `dispatch_count` CSV、错误分类与分析脚本，并把 invalid relation
  parser 崩溃改为可恢复错误，便于 TIER3 支持面分析。
- Metal v1.8 增强 parser 支持面：离散域 range token，并将 global allDifferent
  与 predicate/intension 明确归类为 unsupported。
- Metal v2.0/v2.1 转入 GAC 性能优先路线：新增长期优化计划、prepared/reusable
  runner API、runner/kernel variant 消融开关，并把 setup 拆分为 prepare/reset。
- CUDA persistent kernel、Batch-3A、FQ-PT、OW 系列优化不进入 Metal v1。
- `CPIM_ENABLE_CUDA=AUTO|ON|OFF` 与 `CPIM_ENABLE_METAL=AUTO|ON|OFF` 已作为构建开关落地；
  `--backend=cpu|cuda|metal|auto` 仍保留为后续统一 CLI 草案。
- v1 不复用或改造 `GModel/GModelSolver`，只共享纯 C++ 数据布局；CUDA 主线保持原行为。

### Metal v1 实现状态

- `include/model/device_layout.h` / `src/model/device_layout.cpp` 提供纯 C++ `DeviceModelLayout`
  builder，移除 Metal 路径对 CUDA `uint2/uint3/int2` 的依赖。
- `include/solver/metal/metal_gac_solver.h` 与 `src/solver/metal/metal_gac_solver.mm`
  封装 Metal shared `MTLBuffer`、pipeline、host dispatch loop 与结果读取。
- `src/solver/metal/gac_kernels.metal` 采用标量 atomic 路径处理
  `(constraint, direction, value)` GAC 检查。
- `apps/compare_cpu_metal.cpp` 提供 CPU vs Metal correctness 入口，支持直接 XML 文件和
  bench manifest 的首个实例。

### Metal v1.1 正确性加固状态

- `MetalGacStats` 新增 `device_name`，`compare_cpu_metal` 输出
  `[Metal] device=<name> ...`，用于确认实际跑在 macOS Metal device 上。
- CPU/Metal 对比语义：
  - 先比较 `cpu_stats.inconsistent == metal_stats.inconsistent`；
  - 如果两边都 inconsistent，只检查结果向量形状合法，不强制逐 word 对齐最终
    `bit_dom`，因为 CPU 早停与 Metal 同 dispatch 并发删除可能留下不同最终域；
  - 如果两边都 consistent，继续严格比较 `domain_sizes` 与 `bit_dom`；
  - `budget_exceeded` 一律视为失败。
- 新增 Metal correctness fixtures：
  - `tests/data/metal/gac_inconsistent.xml`：二元 supports 链触发空域；
  - `tests/data/metal/gac_bitwords2.xml`：域大小 40，覆盖 `bit_words=2` 和尾部 mask；
  - 继续复用 `tests/data/bench/test.xml` 作为 deletion case，
    `tests/data/bench/queens-4_ext.xml` 作为 no-deletion smoke。
- CTest 在 `CPIM_METAL_ENABLED` 下注册：
  `compare_cpu_metal_queens4`、`compare_cpu_metal_deletion`、
  `compare_cpu_metal_inconsistent`、`compare_cpu_metal_bitwords2`、
  `compare_cpu_metal_manifest`。

### Metal v1.2/v1.3 运行时与观测状态

- `include/solver/metal/metal_runtime.h` 与 `src/solver/metal/metal_runtime.mm`
  提供 C++ 可包含的 `MetalRuntime`、`MetalBuffer`、`MetalPipeline`，隐藏
  Objective-C `id<MTL...>` 类型。
- `MetalGacSolver` 内部改用 runtime 分配 shared buffer、加载 `gac_revise_kernel`
  并提交每轮 dispatch；public correctness 入口保持不变。
- `MetalGacStats` 新增 `dispatch_count`、`setup_ms`、`dispatch_ms`、`kernel_ms`
  与 `gpu_timing_available`，`elapsed_ms` 仍为总 wall time。
- `apps/benchmark_metal_gac.cpp` 提供 GAC-only benchmark：
  `--input`、`--metallib`、`--runs`、`--warmup`、`--max_iterations`、`--csv`、
  `--verify`、`--verbose`。
- benchmark CSV 每次 measured run 一行，记录输入规模、device、迭代/删值、
  setup/dispatch/kernel 时间与 verify 状态；不设置性能门槛。
- `benchmark_metal_gac_smoke` CTest 使用 `gac_bitwords2.xml` 做轻量回归，CSV 写入
  build 目录。

### Metal v1.4/v1.5 消融状态

- v1.4：`MetalRuntime` 新增 private buffer 上传路径；`MetalGacSolver` 可将只读
  `bit_sup`、`constraint_scopes`、subscription offsets/entries 放入
  `MTLStorageModePrivate`。
- v1.5：新增 compact frontier 路径：
  - `gac_compact_frontier_kernel` 将当前 active constraints 压缩到 compact list；
  - `gac_revise_compact_kernel` 只对 active list 做 `(constraint,direction,value)` 检查；
  - 每轮 compact 模式使用两个 dispatch，`dispatch_count` 会反映这一点。
- `benchmark_metal_gac` 新增：
  - `--readonly_storage=shared|private`
  - `--frontier_mode=flags|compact`
- CSV 新增 `readonly_storage` 与 `frontier_mode` 字段；benchmark smoke 使用
  `private + compact` 覆盖新路径。
- v1.4/v1.5 仍只做 GAC correctness + 消融，不承诺性能收益，不进入 SAC/Batch。

### Metal v1.6 tier-aware 批量扫描状态

- `tests/python/metal_gac_ablation.py` 复用 `tests/python/tier_definitions.py` 的
  TIER0~TIER3 分级，对 `benchmark_metal_gac` 做批量 correctness + 消融扫描。
- 支持 `--suite=tier|metal-smoke`：
  - `tier` 使用 `--tier=0|1|2|3` 选择既有分层样例；
  - `metal-smoke` 使用仓库内小集合，覆盖 no-deletion、deletion、inconsistent、
    `bit_words=2` 和一个更大的 queens 样例。
- 支持 `--mode-preset=baseline|storage|frontier|all`：
  - `baseline`：`shared + flags`；
  - `storage`：比较 `shared/private + flags`；
  - `frontier`：比较 `shared + flags/compact`；
  - `all`：覆盖四种 storage/frontier 组合。
- CSV 逐 run 记录 `OK/MISSING/TIMEOUT/ERROR` 状态；外部 `benchmarks/...` 数据未签入时，
  脚本会尝试映射到 `tests/data/bench/...` 的同名样例，并可用 `--record-missing`
  保留缺失项，避免大批量扫描被单个缺文件打断。
- v1.6 的用途是扩大 correctness 覆盖面和收集趋势数据，不设置性能门槛，不改变
  `compare_cpu_metal` 或 CPU/CUDA CLI 语义。

### Metal v1.7 分析与错误分类状态

- `benchmark_metal_gac` CSV 新增 `dispatch_count`，与 stdout 中的调度次数保持一致。
- `tests/python/metal_gac_ablation.py` CSV 新增：
  - `error_category`：将 unsupported、parse、timeout、verification mismatch 等失败
    归类；
  - `dispatch_count`：透传 benchmark 每次 measured run 的 dispatch 次数。
- 新增 `tests/python/metal_gac_analyze.py`：
  - 汇总 OK/ERROR/TIMEOUT/MISSING；
  - 按 mode 输出 avg/p50/p95/p99、setup/dispatch/kernel、dispatch_count；
  - 按 family 和 error category 汇总失败；
  - 比较 `private/compact` 相对 `shared+flags` 的倍率和胜出个数。
- `LibXml2Parser` 对 constraint 引用无效 relation ID 改为返回
  `INVALID_ARGUMENT`，不再触发 `CHECK` 终止子进程。
- v1.7 重跑结果：
  - TIER2 all-mode：76/79 实例 OK，3 个 `rand-8-20-5` 为
    `unsupported_non_binary_extension`；
  - TIER3 baseline：818/1065 实例 OK，247 个 ERROR 全部可分类，无 timeout。

### Metal v1.8 parser 支持面状态

- `LibXml2Parser::ParseDomainValues()` 支持混合离散域 token，例如
  `0..2 6..7 12`，统一展开为枚举域。
- `LibXml2Parser::ParseConstraints()` 遇到 `reference="global:allDifferent"`
  明确返回 `UNIMPLEMENTED`；predicate/intension `P*` reference 也明确返回
  `UNIMPLEMENTED`。
- 不将 global `AllDifferent` 展开为 pairwise `!=`：pairwise 分解虽然满足性等价，
  但不是 global AllDifferent GAC，不能计入 Metal GAC correctness 支持面。
- `metal_gac_ablation.py` / `metal_gac_analyze.py` 新增
  `unsupported_predicate_intension` 分类。
- v1.8 重跑结果：
  - TIER2 all-mode 保持 76/79 实例 OK；
  - TIER3 baseline 保持 818/1065 实例 OK；
  - 剩余 ERROR 为 `unsupported_predicate_intension`、
    `unsupported_non_binary_extension` 与 `unsupported_global_alldifferent`。

### Metal v2.0/v2.1 GAC 性能路线状态

- 新增 [Metal GAC 长期优化路线](METAL_GAC_LONG_TERM_OPTIMIZATION.md)，将 v2 拆为
  baseline freeze、prepared runner、frontier worklist、位集算子与内存布局阶段。
- 新增 [Metal GAC Changelog Index](METAL_GAC_CHANGELOG.md)，作为 Metal GAC
  小计划/小 changelog 索引；每个小计划和小 changelog 都单独成文，放在
  `docs/planning/metal_gac/`；全局大 changelog 仍为 `CHANGES_ZH.md`。
- `MetalGacOptions` 新增：
  - `runner_mode = cold|prepared`
  - `kernel_variant = scalar|word_parallel|simdgroup|auto`
  - `frontier_mode` 预留 `worklist|auto`，当前未实现专用 kernel 时回退 stable
    `flags + scalar`，并在 `variant_name` 中记录。
- 新增 `MetalPreparedGacRunner`：一次 `Prepare()` 初始化 runtime/pipeline/read-only
  buffer，多次 `Run()` 只 reset mutable state。
- `MetalGacStats`/benchmark CSV 新增 `prepare_ms`、`reset_ms`、
  `solve_ms`、`active_constraints_total`、`frontier_density_avg`、
  `kernel_variant` 与 `variant_name`，旧 timing 字段继续保留。
- v2.1 的主求解口径使用 `solve_ms = reset_ms + dispatch_ms`；
  `setup_ms/prepare_ms` 只作为初始化成本单独观测。
- v2.2-v2.4 已落地：
  - `frontier_mode=worklist|auto`；
  - `kernel_variant=word_parallel|auto`；
  - `bitsup_layout=pair|directional|auto`；
  - `simdgroup` 当前显式回退到 `word_parallel`，后续单独评估。
- v2.4 smoke：`metal-smoke` all-mode 60/60 OK；TIER2 all-mode 2280/2298 rows OK，
  18 个 ERROR 均为历史 unsupported non-binary extension。
- v2.5-v2.9 已收尾：
  - `variant_name` 与新增 `effective_frontier_mode/effective_kernel_variant/`
    `effective_bitsup_layout` 一起记录真实执行路径；
  - `frontier_mode=auto` / `kernel_variant=auto` 采用封版保守规则；TIER2 收尾
    数据发现较宽的 worklist auto 会让 p95 超过 baseline 5%，word_parallel auto
    会让 p50 超过 baseline 5%，因此 v2 auto 降级到 `flags + scalar`；
  - worklist 使用 epoch/stamp 去重，避免每轮清零 next frontier；
  - `reset_mode=cpu|blit|auto` 支持 prepared runner 的 GPU-side copy/fill reset，
    CSV 新增 `reset_dispatch_ms`；
  - `metal_gac_analyze.py` 输出 recommended policy summary，按实例特征分桶比较
    flags/compact/worklist/auto；
  - `simdgroup` 在 v2 封版时继续 fallback 到 `word_parallel`，进入 v3 的条件是
    word_parallel 数据证明位集 intersection 或 domain atomic 成为主要瓶颈。
- v2.9 验收结果：TIER2 baseline `shared+flags+scalar+pair`
  `solve_ms p50=0.261 p95=5.132`；auto 封版路径 `flags+scalar+pair`
  `solve_ms p50=0.259 p95=5.079`，满足 p50/p95 不慢于 baseline 5% 的门槛。

## 2. 当前 CUDA/Jetson UMA 依赖点

### 构建入口

- 迁移前 `CMakeLists.txt` 以 `project(cpim LANGUAGES CXX CUDA)` 声明 CUDA 为必选语言，
  并在多个 target 中直接链接 `cuda` / `cudart`。
- `gmodel_solver`、`compare_cpu_gpu`、`sac_benchmark`、`benchmark_probe_throughput`
  等 GPU 入口直接编译 `.cu` 文件；Mac 上已先将 CUDA target 改为可选。

### 公共类型与模型结构

- `include/GModel.cuh` 直接包含 `cuda_runtime.h` 和 `cooperative_groups.h`。
- 公共数据结构使用 `uint2`、`uint3`、`int2`、`cudaTextureObject_t`、
  `cudaArray_t` 等 CUDA 类型，Metal 不能直接复用这些头文件。
- `GModelData` 同时承担 host API 与 device kernel view 的角色，需要拆成后端无关
  POD 视图和 CUDA/Metal 私有视图。

### 模型构建与统一内存

- `src/model/gmodel_adapter.cu` 使用 `cudaMallocManaged` 分配 `bitDom`、
  `bitSupData`、`constraint_scopes`、订阅表和辅助数组。
- `bitSup` 额外构建 CUDA 3D texture，依赖 `cudaMalloc3DArray`、
  `cudaMemcpy3D` 和 `cudaCreateTextureObject`。
- `cudaMemPrefetchAsync`、`cudaMemAdviseSetReadMostly` 是 CUDA managed memory
  hint，Metal 没有等价 API。

### Kernel 与调度模型

- `src/solver/gpu/GModel.cu` 深度依赖 CUDA 语法和语义：
  `__global__`、`__device__`、`threadIdx`、`blockIdx`、`__shared__`、
  `__syncthreads()`、`__ballot_sync()`、CUDA atomics。
- `EnforceGAC_Persistent()` 使用 `cooperative_groups::this_grid()` 做 grid-wide sync。
  Metal compute 没有同构的单 dispatch 全网格同步，必须改成 host 多轮 dispatch
  或重新设计设备侧状态机。
- `src/solver/gpu/batch_probe_manager.cu` 的 Batch/SAC manager 大量使用
  `cudaMallocManaged`、`cudaMemcpy`、`cudaDeviceSynchronize`、`cudaGetDeviceProperties`
  和 CUDA occupancy/SM 数量启发式。

## 3. Metal 目标架构

### 构建与后端分层

- 增加后端开关，但保持默认行为不破坏 Jetson：
  - `CPIM_ENABLE_CUDA=AUTO|ON|OFF`：控制 CUDA target。
  - `CPIM_ENABLE_METAL=AUTO|ON|OFF`：仅在 Apple 平台启用 Metal target。
  - 非 CUDA 平台仍能构建 `cpim_model`、`cpim_solver_cpu`、`cpim_test_parser`。
- Metal v1 后端目录：
  - `include/solver/metal/metal_runtime.h`
  - `include/solver/metal/metal_gac_solver.h`
  - `src/solver/metal/metal_runtime.mm`
  - `src/solver/metal/metal_gac_solver.mm`
  - `src/solver/metal/gac_kernels.metal`
  - `apps/compare_cpu_metal.cpp`
  - `apps/benchmark_metal_gac.cpp`
- C++ 调用层优先使用 Objective-C++ `.mm` 直接调用 Metal API；如后续希望纯 C++
  风格，可再评估 metal-cpp。

### 后端无关类型

- 新增后端无关 POD 类型：
  - `DeviceInt2 { int32_t x; int32_t y; }`
  - `DeviceUInt2 { uint32_t x; uint32_t y; }`
  - `DeviceUInt3 { uint32_t x; uint32_t y; uint32_t z; }`
  - `DeviceModelLayout`：保存 `bit_dom`、`domain_sizes`、`bit_sup`、约束 scope 与订阅 CSR。
- CUDA 后端在 `.cu` 内把 POD 视图映射到 CUDA kernel 参数。
- Metal shader 使用同布局 struct，避免 host/device ABI 因 CUDA 类型泄漏而分叉。

### Metal 内存策略

- Apple Silicon v1 使用 `MTLStorageModeShared` 作为默认统一内存策略：
  - 可变状态：`bitDom`、`d_cur_dom_size`、frontier、tasks、results。
  - CPU 构建后只读元数据：`bitSupData`、`constraint_scopes`、`subscription`、
    `subscription_offset`。
- v1 不使用 Metal texture 存 `bitSup`，先用扁平 `MTLBuffer` 复刻 CUDA
  `bitSupData` 访问路径，降低迁移复杂度。
- v2 可将只读大表切到 `MTLStorageModePrivate` 并通过 blit 初始化，以评估缓存与带宽收益。
- CPU/GPU 相位边界以 Metal command buffer completion 为准；CPU 不在 command buffer
  未完成时读取或修改 GPU 正在写的 shared buffer。

## 4. 分阶段实施路线

### Phase 0：文档与基线

- 保留本计划文档作为后续任务入口。
- 在 Apple Silicon 上记录当前状态：CPU target 是否可构建、CUDA target 是否被 CMake
  阻塞、Metal 工具链是否可用。
- 不修改算法与后端。

### Phase 1：Mac CPU 可构建

- 将 CUDA 从全局必选改为可选 target。
- 保证 `cpim_model`、`cpim_solver_cpu`、`cpim_test_parser` 在 macOS 上可构建。
- 验收：`cpim_test_parser` 能跑 TIER0 中的小样例；CUDA target 在 Jetson 上默认仍可构建。

### Phase 2：后端类型隔离

- 从公共头中移除 CUDA-only 类型泄漏，建立后端无关 POD view。
- 保持 CUDA 实现行为不变，仅做类型边界整理。
- 验收：Jetson CUDA TIER0 结果不变；Mac CPU 构建不需要 CUDA SDK。

### Phase 3：Metal GAC MVP（已落地，v1.1 加固中）

- 从 `IntermediateModel` 构建 `DeviceModelLayout`，再上传为 `MTLStorageModeShared`
  `MTLBuffer`。
- 迁移普通 Bitmap GAC，不迁移 persistent kernel：
  - 每轮 dispatch 一个 Metal compute kernel 处理当前 frontier。
  - Host 检查 next frontier 是否为空，然后 swap/clear。
  - 用 command buffer completion 作为相位同步。
- Kernel 线程映射：
  - 每个线程处理一个 `(constraint, direction, value)`。
  - 使用 atomic 清除 `bit_dom`、更新 `domain_sizes`、扩张 next frontier。
  - v1 暂不使用 threadgroup memory、simdgroup ballot 或 texture。
- 验收：
  - `queens-4_ext.xml`、`test.xml`、`BMPath.xml` 首个实例 CPU vs Metal 判定一致；
  - `gac_inconsistent.xml` 覆盖 CPU/Metal 都判空域时的 relaxed final-domain 对比；
  - `gac_bitwords2.xml` 覆盖跨 word 域和尾 mask；
  - `ctest --test-dir build_metal -R compare_cpu_metal --output-on-failure`
    作为 Metal correctness 回归入口。

### Phase 3.1：Metal GAC v1.1 自动回归

- 保持 GAC-only，不进入 MAC、SAC 或 Batch。
- `compare_cpu_metal` 使用真实 device name 输出确认 Metal device：
  `[Metal] device=Apple ... iterations=...`。
- CTest 仅在 `CPIM_METAL_ENABLED` 时注册 Metal 测试，避免 CPU-only 构建引用 Metal target。
- CPU-only guard：
  `cmake -S . -B build_cpu -DCPIM_ENABLE_CUDA=OFF -DCPIM_ENABLE_METAL=OFF`
  后应能构建 `cpim_gac_cpu`。
- DocOps 交接证据：
  `python3 codex-docops-logic/scripts/dol.py lint --soft` 与
  `python3 codex-docops-logic/scripts/dol.py solve --stub --mode check`。

### Phase 3.2：Metal GAC v1.2 运行时抽象（已落地）

- 将 device、queue、library、pipeline、buffer 与 dispatch 封装进 Metal runtime。
- public header 不暴露 Objective-C/Metal SDK 类型；调用方只操作 move-only C++ RAII 对象。
- `MetalGacSolver` 继续使用 shared `MTLBuffer` 和 host 多轮 dispatch，不改变 GAC 语义。
- `MTLStorageModePrivate`、blit 初始化和 runtime 级 buffer policy 留给后续性能阶段。

### Phase 3.3：Metal GAC v1.3 性能观测（已落地）

- `MetalGacStats` 输出总耗时、setup、dispatch wall time、GPU kernel time 与
  `gpu_timing_available`。
- 新增 `benchmark_metal_gac`，默认 warmup 后做多次 measured run，`--verify=true`
  时每次用 CPU GAC oracle 校验结果。
- CSV 字段：
  `input,run,device,num_vars,num_constraints,max_dom_size,bit_words,iterations,`
  `deletions,inconsistent,budget_exceeded,elapsed_ms,setup_ms,dispatch_ms,`
  `kernel_ms,gpu_timing_available,verified`。
- 验收：
  - `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=5 --warmup=2 --verify=true --csv=out/metal_gac_v13_smoke.csv`
  - `ctest --test-dir build_metal -R benchmark_metal_gac --output-on-failure`

### Phase 3.4：Metal GAC v1.4 readonly storage 消融（已落地）

- readonly 元数据支持 `shared` 与 `private` 两种 storage：
  - mutable：`bit_dom`、`domain_sizes`、frontier、stats 仍固定 shared；
  - readonly：`bit_sup`、scopes、subscription CSR 可通过 blit 上传到 private。
- benchmark flag：
  `--readonly_storage=shared|private`。
- 默认 `shared`，保证 v1.1-v1.3 行为不变。

### Phase 3.5：Metal GAC v1.5 compact frontier 消融（已落地）

- 新增 compact frontier kernel，把 active constraints 压缩成列表，再只 dispatch
  active constraints 的 revise tasks。
- benchmark flag：
  `--frontier_mode=flags|compact`。
- 默认 `flags`，compact 作为可消融路径；两种模式都必须通过 CPU verify。
- 验收：
  - `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=5 --warmup=2 --verify=true --readonly_storage=shared --frontier_mode=flags --csv=out/metal_gac_v14_shared_flags.csv`
  - `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=5 --warmup=2 --verify=true --readonly_storage=private --frontier_mode=compact --csv=out/metal_gac_v15_private_compact.csv`
  - `ctest --test-dir build_metal -R benchmark_metal_gac --output-on-failure`

### Phase 3.6：Metal GAC v1.6 tier-aware ablation 扫描（已落地）

- 复用 `tests/python/tier_definitions.py` 的 TIER0~TIER3 定义，避免为 Metal 另造一套
  benchmark 分级。
- 新增 `tests/python/metal_gac_ablation.py`，在一个入口里批量运行
  `benchmark_metal_gac`，汇总 storage/frontier 消融结果。
- 缺失外部 benchmark 文件时记录 `MISSING`，可继续扫描已签入的小样例；真实大样例补齐后
  同一脚本可直接扩大到 TIER1~TIER3。
- 验收：
  - `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=all --runs=2 --warmup=1 --csv=out/metal_gac_v16_smoke.csv`
  - `python3 tests/python/metal_gac_ablation.py --tier=0 --mode-preset=baseline --runs=1 --warmup=0 --record-missing --csv=out/metal_gac_v16_tier0_baseline_smoke.csv`

### Phase 3.7：Metal GAC v1.7 扫描分析闭环（已落地）

- `benchmark_metal_gac` CSV 输出 `dispatch_count`，让 compact frontier 的额外 dispatch
  成本能进入离线分析。
- `metal_gac_ablation.py` 将失败行归类到稳定的 `error_category`，避免后续只靠
  stderr 文本做人工归因。
- `metal_gac_analyze.py` 作为标准分析入口，覆盖状态计数、错误 family、mode 汇总、
  相对 baseline 倍率与最慢实例列表。
- parser 对无效 relation ID 返回 recoverable `INVALID_ARGUMENT`，TIER3 扫描中此类样例
  归类为 `parse_invalid_relation_id`，不再产生 fatal stack。
- 验收：
  - `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=all --runs=2 --warmup=1 --csv=out/metal_gac_v17_smoke.csv`
  - `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=all --runs=3 --warmup=1 --csv=out/metal_gac_v17_tier2_all.csv --quiet`
  - `python3 tests/python/metal_gac_ablation.py --tier=3 --mode-preset=baseline --runs=1 --warmup=0 --csv=out/metal_gac_v17_tier3_baseline.csv --quiet`
  - `python3 tests/python/metal_gac_analyze.py out/metal_gac_v17_tier2_all.csv out/metal_gac_v17_tier3_baseline.csv --top=8`

### Phase 3.8：Metal GAC v1.8 parser 支持面（已落地）

- 支持混合离散域 range token，将 `0..2 6..7 12` 这类 domain 展开为枚举值。
- parser 对 `global:allDifferent` 明确返回 unsupported，避免把 pairwise `!=`
  分解误记为 global AllDifferent GAC。
- predicate/intension reference 明确返回 unsupported，并在扫描 CSV 中归类为
  `unsupported_predicate_intension`。
- 验收：
  - `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=all --runs=3 --warmup=1 --csv=out/metal_gac_v18_tier2_all.csv --quiet`
  - `python3 tests/python/metal_gac_ablation.py --tier=3 --mode-preset=baseline --runs=1 --warmup=0 --csv=out/metal_gac_v18_tier3_baseline.csv --quiet`

### Phase 4：SAC/Batch 迁移

- 优先迁移 Batch-2 Stage1/Stage2 中语义稳定的 probe 路径。
- 不直接移植 CUDA cooperative persistent blocks；Metal 版采用 host dispatch loop
  或独立设备队列设计。
- 保留 `UNKNOWN`、budget、deferred recheck、NSAC mask 的 soundness 语义。
- 验收：SAC preprocess TIER0 CPU/CUDA/Metal 三方删值结果一致；性能先记录不设硬门槛。

### Phase 5：性能对照与优化

- 建立 Mac Metal 专用 benchmark CSV 字段：
  - runner_mode、storage_mode、frontier_mode、kernel_variant、bitsup_layout、
    reset_mode、effective_*、variant_name、dispatch_count、solve_ms、
    prepare_ms、reset_ms、reset_dispatch_ms、kernel_ms、worklist_*、
    p50/p95/p99。
- 基于 v2.x CSV 再评估 worklist、SoA/packing、simdgroup ballot、threadgroup
  memory 与 texture 的收益；v2 已先落地 directional bitSup packing、word-parallel
  载体、epoch worklist 和 blit reset，texture 暂缓。
- 只有在 GAC/SAC MVP 语义稳定后，再重新评估 Batch-3A/FQ-PT 是否值得迁移。

## 5. CUDA 到 Metal 的替换规则

| CUDA 机制 | Metal v1 处理方式 |
|----------|-------------------|
| `cudaMallocManaged` | `MTLBuffer` + `MTLStorageModeShared` |
| `cudaMemcpy` / `cudaMemset` | CPU `memcpy` shared buffer 或 blit/compute clear |
| CUDA 3D texture | v1 改为扁平 `MTLBuffer` 访问 |
| `__global__` kernel launch | Metal compute pipeline + command encoder dispatch |
| `__shared__` | `threadgroup` memory |
| `__syncthreads()` | `threadgroup_barrier` |
| `__ballot_sync()` | Metal simdgroup vote/ballot；不满足时走保守路径 |
| `cooperative_groups::grid_group::sync()` | 拆成多 dispatch + host 判停，或后续重设状态机 |
| `cudaDeviceSynchronize()` | command buffer `waitUntilCompleted` 或 completion handler |
| `cudaMemAdvise` / prefetch | 无直接等价；改用 storage mode 与相位边界控制 |

## 6. 回退与验收原则

- CUDA/Jetson 路径始终保留，Metal 后端 default-off，直到 CPU/CUDA/Metal TIER0 对齐。
- 任意 Metal kernel 结果不确定时，回退 CPU 或 CUDA stable path，不允许误删值。
- 性能优化不得改变搜索节点数、SAC 删除语义或 `UNKNOWN` 的保守语义。
- 每个阶段都更新 `CHANGES_ZH.md`，并记录测试命令与关键指标。

## 7. Python 环境

- 文档阶段不需要 Python 环境。
- 进入 Phase 5 后，如需批量统计和画图，可在仓库外或 `.venv/` 中建立 Python 环境，
  复用 `tests/python` 现有 CSV/批量运行脚本风格。
