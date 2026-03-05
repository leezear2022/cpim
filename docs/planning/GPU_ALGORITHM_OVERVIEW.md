# GPU 求解器算法概览（cuSAC.cu / cuSAC.cuh）

本文概述当前 GPU 求解器的核心算法、数据表示与执行路径，便于在 Jetson 平台继续演进与优化。

## 模型与数据表示
- 变量域（bitDom）
  - 每个变量的取值域用位图压缩（`uint32` 数组），`kBitDomIntSize = ceil(maxDom/32)`。
  - 多搜索层共享一块“层叠”位图（`kAllBitDomsIntSize = kBitDomIntSize * kNumVars * kDepth`）。
- 约束与支持（bitSup）
  - 针对扩展表约束，为每个 (c, x, a) 预编码“对其它变量的支持集合”，以位图/纹理存储。
  - 约束-变量邻接与订阅采用紧凑结构（`h_subscription` + `offsets`），便于快速找到受影响约束。
- 全局常量
  - `__constant__` 持有尺寸、度（`kDeg`）、变量/约束数等；部分状态用 `__managed__`（统一内存）。

## 初始化（CModel::CModel / BuildBitModel）
1. 从 HModel 导入变量数、约束数、最大域大小；初始化设备常量。
2. 构造变量×约束邻接（订阅）矩阵，拷贝到设备，必要时绑定为纹理对象（`texObj_MCon`）。
3. 为每个变量初始化第 0 层域位图（`h_bitDom → d_bitDom`），并填充 `d_cur_dom_size`。
4. （省略）为表约束构建 bitSup（支持集合）的编码与索引结构。

## 传播（GAC：Generalized Arc Consistency）
- 主流程：
  - `enforceGAC()` → 事件压缩 `compress_Main()` 生成待检查约束列表 → kernel `CsCheckMain` 并行检查与删值 → 重复直至队列为空。
  - 判失败：若某变量域为空或核内检测到冲突，置 `GAC_success=false` 并返回。
- 核心检查思想：
  - 对约束 c 的每个 `x∈scope(c)`、域内值 a，利用 bitSup 与其它变量当前 bitDom 做位运算，判断是否存在支持。
  - 无支持则清除 `bitDom[x][a]` 并递增删值计数；受影响的约束重新入队。

## 搜索（MAC 风格的 GPU 化）
- 启发式选择（变量）：
  - `heuristic()` 使用 Thrust 在设备端计算 `ratio = dom_size / deg`，取最小者作为分支变量。
- 决策与回溯：
  - `AssignValue` kernel 在新层将变量域收缩到单值；随后 `enforceGAC(var, 1)` 做一次传播。
  - 若失败，则 `BackLevel()` 并调用 `RemoveValue` kernel 排除该值，继续 `enforceGAC(var, 0)` 传播。
  - 循环上述过程，直至找到解或超时。

## 事件压缩（compress_Main）
- 根据最近删值/赋值信息，从订阅结构快速筛出受影响约束，生成紧凑事件队列（`d_ConPre/d_MCon` 等）。
- 该步骤保证 `CsCheckMain` 仅处理“可能变化”的约束，降低无效检查。

## 复杂度与开销
- 单轮 GAC 代价 ≈ 受影响约束数 ×（按 scope 展开的 bit 运算）；位宽并行可显著削减常数。
- 搜索代价与分支顺序密切相关；当前使用 `dom/deg`，还可尝试动态度、失败计数等启发式。
- 主要开销点：
  - 事件生成/重建；
  - bitSup 与 bitDom 的访存聚合与对齐；
  - Host–Device 同步（应尽量用事件/Graphs 降低）。

## 现状局限
- 强依赖 HModel：bitSup/订阅等均由 HModel 提供；更现代的 IntermediateModel 需适配器。
- 偏旧的 CUDA 风格：内核内 `printf`、显式同步较多，未充分利用 CUDA 12 的 Graphs/内存池/统一内存最佳实践。
- 主要覆盖扩展表约束；SAC 接口存在但未完备实现。

## 与 Jetson 优化的关系
- Jetson 上 CPU/GPU 统一内存降低迁移成本，配合 `__managed__`、`cudaMemAdvise` 与 `prefetch` 可减少首次访问抖动。
- 可将 `enforceGAC` 循环捕获为 CUDA Graph，减小 kernel 启动与 host 往返；对小规模 SM 以 Persistent Kernel 提高利用率。

> 更细节的 Jetson 侧优化建议与里程碑，请参考 `aig_docs/GPU_JETSON_ADAPTATION.md`。
