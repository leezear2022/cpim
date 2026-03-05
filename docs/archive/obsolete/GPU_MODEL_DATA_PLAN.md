# 模型与数据表示改进计划（Jetson Orin + CUDA 12）

目标：在维持算法语义不变的前提下，围绕 Jetson SoC（统一内存、带宽受限、SM 数量较少）优化模型与数据表示，使传播与搜索阶段的访存更高效、开销更可控。

## 1. 统一数据源与归一化
- 以 `IntermediateModel` + `ModelNormalizer` 为唯一数据源：域统一为 `[0..n-1]`、扩展约束语义统一为 supports。
- 构建 GPU 侧结构时避免 HModel 中转，减少一次编码与内存复制。

## 2. 变量域（bitDom）
- SoA + 对齐加载：将 bitDom 以变量为主维、按 `uint4`/128-bit 对齐组织，内核中使用矢量化加载。
- K 层叠加：`[level][var][word]`，尽量保证 level 连续，提高 prefetch 命中。
- 小域特化：域长≤32 走单 word 快路径（减少循环与分支）。
- UM 建议：`__managed__` + `cudaMemAdviseSetPreferredLocation(GPU)` + 关键阶段 `cudaMemPrefetchAsync`。

## 3. 约束支持（bitSup）
- CSR/COO 紧凑化：为每个约束按变量/值构建支持索引，采用 `(offsets, indices)` 表达，减少稀疏段空洞。
- 位集对齐：支持位图按 `uint4` 对齐，批量 AND/OR；对小元数约束尝试按列块化以提升 cache 亲和性。
- 只读数据：避免纹理滥用，优先 L2 友好的全局内存；确有二维稀疏列访问优势时再启用纹理对象。

## 4. 订阅与邻接（事件驱动）
- 统一为 CSR：`var_offsets` + `var2cons`；设备端直接使用，省去 Host 压缩。
- 事件队列：设备内常驻环形队列，写入被删值影响的 `(c, x)` 键；减少 compress 阶段扫描成本。
- Scope 预排序：按变量度或域长升序存储 scope，减少位运算总量（先用“最便宜”的维度过滤）。

## 5. ID 映射与紧凑化
- 变量/约束 ID 重新编号为密集区间 `[0..N)`，保证数组式索引与顺序访存。
- 值 ID 与域索引等价（已由 Normalizer 实现），避免查表。

## 6. 内存管理与生命周期
- 统一内存池：使用 `cudaMallocAsync` + `cudaMemPool_t` 管理临时缓冲（事件队列、压缩中间产物）。
- 设备常量：尺寸、度数组、偏移量等放入 `__constant__` 或 `__managed__` 只读区域。
- 生命周期：CModelAdapter 构建后产物保持“只读 + 常驻”，搜索期间不再重分配。

## 7. 度量与可观测性
- 结构内附带轻量计数：删值总量、事件数、GAC 迭代次数、bit 运算字数等，供 Nsight 校验。
- 关闭 `printf`，改为写回统计缓冲；必要时按迭代周期抽样。

## 8. 与执行模型的配合
- 固定网格 + 线程内边界检查，便于构建 `CUDA Graph`（减少每轮参数更新）。
- 逐步迁移到 Persistent Kernel：设备端维护事件队列与迭代判停，Host 只做外层搜索调度。

## 9. 迁移步骤
- S0：落地 `IntermediateModel → CModelAdapter` 构建 GPU 结构（bitDom/bitSup/CSR）。
- S1：改造 bitDom/bitSup 为 SoA + 对齐加载；值≤32 的特化路径。
- S2：CSR 订阅与设备端事件队列，替换 Host 压缩。
- S3：UM 策略与内存池引入；核内 `printf` 全面移除。
- S4：配合 `CUDA Graph`/Persistent Kernel 的固定网格与边界检查。
- S5：持续调优（域排序、scope 预排序、带宽/占用权衡）。

## 10. 预期收益与权衡
- 预期减少 Host 往返、降低不必要的 memcpy，同步开销显著下降。
- SoA + 对齐加载可改善 L2/L1 带宽利用；CSR 结构减少稀疏访问空洞。
- Persistent/Graphs 可降低启动开销，但需处理占用与同步复杂度；保持宏开关以便回退。
