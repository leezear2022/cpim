# 统一内存下的约束传播优化与动态 CPU/GPU 执行（Jetson Orin）

本文总结在 Jetson Orin（UMA）平台上，基于 GModel 简化位集结构与现有 CModel 传播框架，如何优化约束传播（GAC）并实现统一内存零拷贝前提下的 CPU/GPU 动态执行与 GPU 端效率提升方案。

## 背景与目标
- 现状（参考 src/cuSAC.cu）：`compress_Main()`（事件压缩）+ `CsCheckMain`（传播）循环，多次 kernel 启动、Host/Device 往返频繁。
- GModel（include/GModel.cuh, src/GModel.cu）使用统一内存（cudaMallocManaged）存放 `bitDom`/`bitSup`，在 Orin 上 `concurrentManagedAccess=0`，无需 `cudaMemPrefetchAsync`。
- 目标：
  - 数据单源：CPU/GPU 共享一份统一内存，零显式拷贝。
  - 事件驱动：只处理受影响的约束，直到收敛。
  - 动态调度：依据负载阈值在 CPU 与 GPU 间切换。
  - GPU 高效化：减少 launch/重建开销，采用“持久化 kernel + 设备队列”。

## 总体策略
- 单一真源数据：`bitDom`/`bitSup` 常驻统一内存，CPU 与 GPU 均直接访问。
- 事件驱动传播：维护“脏约束队列”，仅对受影响约束/变量值做传播检查。
- 动态后端选择：基于队列规模、域宽（`bit_dom_int_size`）、拓扑密度，自动切换 CPU（小批次、低并发）或 GPU（大批次、易并行）。
- 持续化执行：优先使用“持久化 kernel + 设备侧队列”，替代多次 kernel 启动与 Host 压缩。

## 统一内存（UMA）下的动态执行
- 相位隔离：
  - GPU 执行窗口内，CPU 只读 `bitDom` 与元数据；GPU 完成后再进入下一轮 CPU 决策/修改。
  - 同步建议：`cudaEventQuery`/`cudaEventSynchronize` 控制相位边界；必要处使用 `__threadfence_system()` 确保 CPU 可见。
- 调度阈值（可调 flags）：
  - small：事件数 < `4 × SM` 且 `bit_dom_int_size ≤ 2` → CPU 路径更快。
  - medium：事件数在 `[4×SM, 32×SM]` → 单次 GPU 批处理。
  - large：事件数 ≥ `32×SM` → 启用持久化 kernel。
- UMA 亲和性（Orin 特性）：
  - Orin 上 `concurrentManagedAccess=0`，默认不做 `cudaMemPrefetchAsync`。
  - 可尝试对大数组调用 `cudaMemAdviseSetAccessedBy(device)`（失败忽略）。元数据（队列头/计数器）可用 `cudaHostAllocPinned`。

## GPU 传播内核优化
- 减少小批量高频 launch：
  - 现状：每轮“压缩→launch→压缩→…”（参考 `compress_Main` 与 `CsCheckMain`）。
  - 方案：持久化 kernel（Persistent Kernel）+ 设备侧环形队列/工作窃取，内核驻场拉取任务直到 Host 标记“空队列”。
- 设备侧队列与压缩：
  - 用 warp 级 `ballot_sync` + 前缀和做紧凑写入，替代 Host 侧 `thrust::copy_if + fill`。
  - 新事件（由域变更引发）在内核内原地入队，减少 Host/Device 往返。
- 线程映射与访存：
  - 建议“每 warp 处理一个域字（uint32）”，warp 内步进循环多个字；减少低效小 block。
  - 共享内存/寄存器 staging：两个变量的域切片先加载到寄存器或共享内存；按字执行 AND/OR/NOT 与 `__popc` 计数，支持早停。
  - 访存对齐：`bitDom`/`bitSup` 以 128-bit 边界对齐；布局为 AoSoA 保证合并访问。
- 缓存与只读路径：
  - `bitSup` 走只读缓存（或 3D/2D 纹理）；为指针加 `const __restrict__`，帮助编译器发射 LDG。
  - `bitDom` 为写多读多，不建议纹理化，但可按字对齐并减少回写频次。
- 局部传播与脏掩码：
  - 内核内维护值级别“脏掩码”，仅扫描受影响字位；
  - 将支持表预压缩为“值→候选 word 列表”的稀疏索引，跳过空 word。

## 数据结构与内存布局建议（GModel 扩展）
- 统一内存事件队列：环形 buffer（head/tail 原子索引）+ 设备消费；CPU/GPU 共用。
- `dirty_word_mask`：每变量维护按 word 的脏位图，辅助 GPU 内核直接定位受影响范围。
- `bitSup` 方向分离与扁平化：将二元约束的两个方向拆为两段连续数组（x→y 与 y→x），按 word 扁平布局，避免 `uint2` 解包开销。
- 常量化元数据：将 `bit_dom_int_size`、`max_dom_size`、`num_vars` 等拷入 `__constant__`，减少反复读取成本。

## 调度与收敛控制
- 订阅驱动：沿用订阅表（变量→约束）投递事件，仅对受影响约束传播；设备侧自维护事件入队。
- 早停：若任何变量域为空，内核设置全局失败标志并全局观察退出。
- 粗粒度回合：将“assign/remove→一次大传播→收敛”整合为“GPU 持久化传播达稳定后交回 CPU 决策”，降低 Host-Device 抖动。

## Jetson Orin 特化建议
- 小而稳的并行度：以 `SM × (2~4)` 个 blocks 启动持久化 kernel，内部用工作队列自平衡。
- 避免动态并行（DP）：在 Orin 上 DP 收益有限，统一使用单层持久化 + 队列。
- CPU 快路径 NEON：小队列/小域时走 CPU，并用 NEON 向量化（`vandq_u32`/`veorq_u32` + `vaddvq_u32` 或 `__builtin_popcount`）。

## 代码落点与改造指引
- 队列构建：
  - 现状：`compress_Main()` 使用 Thrust（`copy_if`/`fill`）。
  - 改造：设备侧紧凑写入（warp ballot + prefix sum）+ 持久化内核内部自维护队列。
- 内核形状：
  - 现状：`CsCheckMain<<<num_ConEvt, kBitDomIntSize*32>>>`（每轮重新启动）。
  - 改造：固定网格（`#SM × 常数`），每 warp 拉取任务，warp 内按字处理。
- 事件合并：
  - 现状：传播后回到 Host 侧再次压缩并重启。
  - 改造：内核内原地标记并入队新事件，直到队列空。
- 常量区：恢复 `__constant__` 元数据一次性拷入，避免重复读取（参考 src/cuSAC.cu 顶部常量声明）。

## 渐进落地步骤
1) 在 GModel 增加统一内存事件队列与调度标志，提供 CPU 端位集 AC-3 快路径（NEON 可选）。
2) 实现最小持久化传播 kernel（仅传播不回溯），接管 `compress_Main + CsCheckMain` 职责。
3) 加入调度器（通过 absl::flags 暴露阈值），依据事件规模/域宽在 CPU/GPU 之间切换。
4) 深化 GPU 优化：按字处理、共享内存 staging、warp ballot + `__popc` 早停，`bitSup` 方向分离并扁平化。
5) 基准验证：使用 `samples/bench` 多个实例记录“事件规模/域宽/迭代次数/耗时”，微调阈值；保持回退开关。

## 与现有文档的关系
- 设计细节与状态机方案：`aig_docs/GPU_GAC_PERSISTENT_STATE_MACHINE.md`。
- 流水线与 Graph 方案：`aig_docs/GPU_GAC_PIPELINE_PLAN.md`。
- 本文是面向统一内存与动态调度的一页式实施综述，优先级：先完成统一内存数据单源 + 调度，再逐步引入持久化 kernel 与设备队列。

## 运行与验证建议
- 编译（Orin，SM 87）：`cmake -S . -B build_orin -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_ARCHITECTURES=87 && cmake --build build_orin -j$(nproc)`。
- 解析与构建 GModel 并做 GPU 一致性验证：
  - `./build_orin/dump_gmodel --input=samples/bench/queens-4_ext.xml --max_print=8`。
- 指标采集：
  - 事件规模、bit_dom_int_size、迭代次数、Host/Device 同步次数、单轮传播耗时、总时延。

—— 完 ——

