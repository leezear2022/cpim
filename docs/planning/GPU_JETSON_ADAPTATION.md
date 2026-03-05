# GPU 求解器 Jetson 适配与提升规划（CUDA 12）

## 目标
- 在 Jetson Orin Nano (Ampere, CUDA 12) 上稳定、高效运行 GPU 求解器。
- 降低 CPU↔GPU 数据往返与同步开销，发挥统一内存与新特性优势。
- 在不牺牲可维护性的前提下，逐步替换遗留实现细节。

## 推荐方案（Jetson 首选）
- 在 Orin 上，优先采用“持久化内核 + 约束状态机 + 设备端队列”的 GAC 传播方案，避免多波次内核启动与 Host 同步，显著改善小批量 frontiers 的利用率与整体时延。
- 设计与细节见：`aig_docs/GPU_GAC_PERSISTENT_STATE_MACHINE.md`；该方案可与现有流水线/Graphs 路线并存，通过 CMake 选项进行 A/B 切换。
- 如需渐进迁移或快速回退，可先按 `aig_docs/GPU_GAC_PIPELINE_PLAN.md` 完成“设备端压缩 + CUDA Graph 捕获”，再切换到持久化方案。

## 平台假设
- SoC 架构（CPU/GPU 共享物理内存），Compute Capability ≈ SM 8.7。
- Jetson 驱动与 CUDA 12.x 就绪；Nsight Systems/Compute 可用。

## 架构与构建
- CMake：
  - 设定 `CMAKE_CUDA_ARCHITECTURES="87"`（Orin Ampere）。
  - `set(CMAKE_CUDA_STANDARD 17)`, Release 默认开启 `-O3`。
- 工具链：启用 `-lineinfo` 以便 Nsight 分析；按需使用 `--use_fast_math`（核函数数值敏感处慎用）。

## 统一内存（UM）最佳实践
- 首选 `__managed__`/Unified Memory 取代显式 H2D/D2H 拷贝，消除不必要的 memcopy 与同步点。
- 使用 `cudaMemAdvise` 指定首选驻留（e.g. `cudaMemAdviseSetPreferredLocation` 到 GPU）+ `cudaMemPrefetchAsync` 在关键相变点预取，减少首次访问迁移抖动。
- 使用 `cudaMallocAsync`/内存池（`cudaMemPool_t`）管理临时缓冲，降低分配/释放开销；在迭代内重用缓冲。

## 数据布局与编码
- bitDom/bitSup：保持位集压缩，但统一采用 SoA + 对齐加载（`uint4`/128-bit）提升带宽利用率。
- 只读表数据（如支持集）：优先走全局内存 + L2 缓存，纹理对象仅保留对二维稀疏按列访问明显收益的路径。
- 订阅与邻接：使用 CSR 风格（`offsets`+`indices`）在 device 端一次性构建并常驻。

## Kernel/执行模型
- 传播主循环使用 **CUDA Graphs** 捕获（`enforceGAC` 迭代与决策分支），减少每轮 `<<< >>>` 启动与 host 同步开销。
- 采用 **Persistent Kernel** 或 **Cooperative Groups** 管理迭代与工作分配，避免频繁 kernel 启停；对小规模 Orin SM 数量调参 block 大小与占用。
- 使用 `warp` 原语（`__shfl_sync` 等）进行小规模归约（如 dom/deg 选择），替代多级共享内存归约。
- 在内核内避免 `printf`；将调试信息切换为按需的标志位/统计量写回。

## 流与并发
- 以单流为主，关键阶段引入第二流预取/压缩重建（如下轮候选订阅集的预处理），在 Jetson 资源有限前提下谨慎并发。
- host 侧改用 `cudaEvent` 计时与依赖，杜绝 `cudaDeviceSynchronize()` 粗暴同步。

## 与解析层衔接
- 现状依赖 `HModel`。建议新增 `CModelAdapter(IntermediateModel)`：
  - 接入 `ModelNormalizer` 输出（域=0..n-1、supports 语义统一）。
  - 直接生成 GPU 所需的 CSR/位集缓冲，避免 HModel→中转→GPU 的二次编码。
- 保留旧接口过渡，逐步将 main 流程切到 `IntermediateModel`。

## 可观测性与可靠性
- 增加设备端统计（压缩后事件数、删值计数、GAC 迭代次数等），以 ring buffer 回传；禁止内核 `printf`。
- 增加 `CUDA_CHECK` 宏与 `assert`（`-DNDEBUG` 可剔除）。

## 调参与性能基线
- 基线 KPI：
  - 单轮 `enforceGAC()` 时间、平均迭代次数；
  - 端到端求解时间与 CPU 版 MAC 的加速比；
  - SM 占用、全局带宽、L2 命中（Nsight Compute）。
- Tuning 顺序：
  1) 消除同步/拷贝 → 2) Graphs/Persistent → 3) 布局/对齐 → 4) CSR/访存聚合 → 5) Occupancy/block 大小。

## 里程碑
- M0（1 周）：UM 替换 + 取消内核 printf + 事件计时；修正所有 `cudaDeviceSynchronize()`。
- M1（1-2 周）：Graph 化 `enforceGAC` 主循环；引入 cudaMallocAsync 内存池；预取/advise 调优。
- M2（2 周）：CSR 订阅结构 + 变量选择内核 Warp 级归约；bitSup 对齐加载。
- M3（2-3 周）：新增 `IntermediateModel → CModelAdapter`；与 CPU 新管线一致化。
- M4（持续）：Nsight 驱动的细粒度优化与回归基准维护。

## 风险与回退
- Jetson 资源受限：避免过大共享内存/寄存器配置导致占用下降。
- Graphs/Persistent 引入复杂性：保持特性开关（CMake 选项），保留旧路径可快速回退。

## 附加文档
- enforceGAC 流水线一体化规划（事件压缩 × CsCheck）：`aig_docs/GPU_GAC_PIPELINE_PLAN.md`
- CsCheck 优化计划（含 Tensor Core 评估）：`aig_docs/GPU_CSCHECK_OPT_PLAN.md`
- 事件压缩优化方案：`aig_docs/GPU_EVENT_COMPRESSION_PLAN.md`
- 模型与数据表示改进计划：`aig_docs/GPU_MODEL_DATA_PLAN.md`
- 详细 Graph/Persistent 设计与实现骨架：`aig_docs/GPU_GRAPHS_GAC.md`
 - Jetson 首选方案：持久化内核 + 约束状态机：`aig_docs/GPU_GAC_PERSISTENT_STATE_MACHINE.md`
