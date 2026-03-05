# CsCheck 优化计划（CUDA 核心与 Tensor Core 可行性评估）

本文针对 `CsCheck`（约束支持检查）提出在 Jetson Orin + CUDA 12 环境下的优化路线，并评估 Tensor Core 的适用性。

## 1. 问题背景与目标
- 目标：对每个事件 `(c, x)` 和域值 `a`，在当前 bitDom 下快速判定是否存在支持（GAC）。
- 现状：按 scope 逐维做位运算（AND/OR），在 GPU 上并行；但存在访存离散、内核启动/同步频繁、删值写回冲突等问题。

## 2. CUDA 核心（SM）侧优化（优先）
1) 数据布局与加载
- 采用 SoA + 128-bit 对齐（`uint4`）批量加载 bitDom/bitSup，减少指令数与对齐惩罚。
- bitDom 层叠布局 `[level][var][word]`，迭代前用 `cudaMemPrefetchAsync` 预取到 GPU。
- 使用 `__restrict__` 指针与常量限定，提升编译器优化空间。

2) 瓦片缓存与 cp.async
- 将当前 `(c, x)` 关联的多变量 bitSup/bitDom 分段搬入共享内存（`cp.async` + `cuda::pipeline`），隐藏内存延迟。
- 按 word 块大小设置 `blockDim`，实现“加载-计算-写回”流水。

3) Warp 级并行与早停
- 一个 warp 处理一个事件 `(c, x)`；各 lane 处理不同 word 段，使用 `__ballot_sync`/`__any_sync` 实现“零检测”早停。
- 对二元/小元数约束使用完全展开（unroll）减少分支与循环。

4) 作用域排序与短路
- 在构建阶段将 scope 按“当前域大小升序 / 度升序”排序；位运算从最小域开始，快速触发早停。

5) 残留（Residue）/见证缓存
- 为 `(c,x,a)` 缓存上轮找到的“见证”位置（support residue）；下次从 residue 附近开始检查，显著降低平均检查长度（AC4/Residue 思想）。
- 在 GPU 侧以紧凑数组存储 residue 索引，批量读写。

6) 删值写回与原子降压
- 采用 per-warp 本地缓冲聚合删值，warp 末尾一次性写回（或一次原子计数再顺序写入），避免热点原子。
- 如需多处写回，采用分区队列（各 CTA 局部队列 + 合并）。

7) 占用与寄存器
- 控制寄存器使用量（`-maxrregcount` 适度约束），以保证在 Orin 上足够的 active warps；使用 Nsight Compute 评估。

## 3. 执行模型配合
- 固定网格 + 线程内边界检查（`if (tid<n)`）便于 **CUDA Graph** 重放，无需每轮更新节点参数。
- **Persistent Kernel** 版本在设备端循环 `build_events → cscheck → reduce → …`，以 `cooperative_groups::grid_group` 同步，彻底消除 Host 控制开销。

## 4. Tensor Core 可行性评估
- Tensor Core（TC）擅长矩阵乘（FP16/BF16/TF32/INT8），而 CsCheck 是位集逻辑（AND/OR + 早停），与 TC 计算模型不匹配。
- 理论映射：
  - 将位块编码为 0/1 INT8，使用 `wmma::mma_sync` 统计“支持计数”（相当于点积/乘加）。
  - 但需要复杂的重排/转置/填充（m×n×k 固定形状），且以“计数”代替“快速早停”，常数开销大，多数场景收益不足。
- Ampere 上并无稳定公开的“二值 XOR+POPC 的 TC 指令”可用（该类 BMMA 更偏向新架构/专用路径）；Orin INT8 TC 也难以高效表达位逻辑。
- 结论：
  - 主线不建议 TC 化 CsCheck；继续深耕 CUDA 核心上的位运算向量化与访存组织。
  - 可在旁路实验 `dp4a`/INT8 dot 结合 `__popc` 的混合方案做计数估计，但复杂度高、未必优于纯位逻辑。

## 5. 里程碑（与 Jetson 规划对齐）
- M0：Warp-per-event 内核 + 早停 + SoA/对齐加载；去除 printf，事件/删值统计写回。
- M1：cp.async 瓦片缓存 + per-warp 删值缓冲；残留见证缓存引入。
- M2：Persistent Kernel/Graphs 重构执行模型；CSR 事件队列设备化（见 EVENT_COMPRESSION_PLAN）。
- M3：进一步按约束类型/元数做特化（2元/3元），并联调 Occupancy 与带宽。

## 6. KPI 与验收
- 单轮 CsCheck 吞吐（事件数 / 毫秒）、删值/事件比、早停命中率、总传播时长；
- 内核访存带宽、L2 命中、SM 占用；Host API 占比（Nsight Systems）。

