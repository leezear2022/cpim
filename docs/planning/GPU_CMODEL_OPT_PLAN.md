# CModel 优化与改造计划（cuSAC · Jetson Orin）

本文聚焦于 `CModel` 及其相关 CUDA 内核（`cuSAC.cu/.cuh`）的性能优化与结构演进，
在不改变外部接口的前提下优先获取显著收益；必要时再进行有限的内部重构。

## 1. 结论与范围
- 第一阶段无需对 `CModel` 做“大改”即可获得明显收益（约 2–3×）。
  - 方向：用订阅表驱动事件、重排 Kernel 线程组织、合并原子写回、压缩事件标记、移除调试回读。
  - 外部接口保持不变，仅在内核参数与内部成员使用上做增强。
- 若进一步引入持久化 Kernel 或 trail 回溯以避免整层拷贝，则属于“中等规模”的内部重构，
  仍可保持 `CModel` 对外接口不变。

## 2. 改动规模档位
- 小改（建议先做）
  - 订阅表驱动事件标记（替代 `neiCon` 扫描）、Kernel 2D 线程组织与束内归约、写回原子降压、事件位图、关闭 printf/回读。
  - 不改 `CModel` 公共接口，仅新增/传参已有成员（如 `d_subscription`/`d_subscription_offset`）。
- 中改
  - Trail 替代整层 `bitDom` 复制、设备侧事件队列；`CModel` 内新增 trail/队列缓冲与回滚逻辑。
- 大改（可选）
  - 持久化 Kernel + 设备侧调度；`solve()` 以启动/停止 persistent kernel 为主，Host 往返最小化。

## 3. 阶段 A：不改接口的快收益
1) 订阅驱动的事件标记（替代全表扫描）
- 现状：传播后通过 `neiCon` 2D 纹理按变量整行扫描约束，O(|C|) 放大开销。
- 方案：在内核中直接使用 `d_subscription` 和 `d_subscription_offset` 标记相邻约束，
  不再依赖 `neiCon`；`compress_Main()` 继续基于 `d_ConPre` 收敛。
- 影响文件：`src/cuSAC.cu`（`CsCheckMain*` 参数与内部触发逻辑）。

2) Kernel 线程组织与归约重排
- 现状：以 `kBitDomIntSize*32` 为 X 维，`kBitDomIntSize>1` 时在 kernel 内循环访问 `bitSup`，
  分支/循环与占用效率不佳。
- 方案：采用 2D block（`blockDim.x=32` 为 bit 位，`blockDim.y=kBitDomIntSize` 为 word 段），
  去掉循环；Y 维共享内存切片，X 维用 `__ballot_sync` 聚合，一次性写回；统一处理大域场景。
- 纹理访问保持 3D `texObj_BitSup`，仅重排访存与归约写回路径。

3) 写回与删值计数的原子优化
- 方案：在共享内存暂存“写前 word”，warp 聚合后计算 `__popc` 差分，合并一次 `atomicSub`；
  若同一变量多个 word 变化，先局部累加再一次写回，降低原子热点。

4) 事件标记位图化
- 方案：将 `d_ConPre` 由 `int` 数组改为位图（`uint32_t` 位），内核中按 bit 置位；
  `compress_Main()` 先将位图膨胀或改用 `cub::DeviceSelect::If`。

5) 关闭调试打印与 Host 回读
- 方案：以编译宏（如 `CPIM_GPU_DEBUG`）屏蔽 `printf` 与 `bitDomCopy()`；Release 下默认关闭。

6) 架构与编译
- Jetson Orin（Ampere）建议 CMake 增加 `-DCMAKE_CUDA_ARCHITECTURES="80;87"`。

## 4. 阶段 B：回溯与调度（按收益推进）
1) Trail 替代整层复制
- 现状：`CreateNewLevel()` 复制整层 `bitDom` 与 `d_cur_dom_size`，带宽与延迟开销大。
- 方案：设备侧 trail（变更日志）记录 `(var, word_idx, old_word)` 与 `dom_size` 旧值；
  回溯时逐条回滚；工作域只保留一层。
- 影响：在 `AssignValue/RemoveValue/CsCheckMain*` 的写回路径顺手记录 trail；
  `BackLevel()` 改为按 trail 回滚。

2) 设备侧事件队列与持久化 Kernel（可选）
- 方案：以环形队列在设备端维护 `(c,x)` 事件；一个 persistent kernel 驻留 SM 周期性“取事件→传播→写回→判停”。
- Host 仅播种初始事件与外层搜索决策；也可用 CUDA Graph 捕获 `compress→cscheck→reduce→swap` 子图做重放。

## 5. 关键改动点（代码锚点）
- 传播核：`src/cuSAC.cu` 中 `CsCheckMain`/`CsCheckMainAfterDecision`。
- 赋值/删值：`AssignValue`、`RemoveValue`（可在写回同时写 trail）。
- 事件结构：`d_MCon`、`d_ConPre`、`d_MConEvt`、`d_subscription`/`d_subscription_offset` 构建于 `BuildBitModel`。
- 层管理：`CreateNewLevel`/`BackLevel`（阶段 B 改为 trail 机制）。

## 6. 与现有专题文档的关系
- CsCheck 内核细化：参见 `aig_docs/GPU_CSCHECK_OPT_PLAN.md`。
- GAC 流水线与队列：参见 `aig_docs/GPU_GAC_PIPELINE_PLAN.md`。
- 模型/数据布局：参见 `aig_docs/GPU_MODEL_DATA_PLAN.md`。
- 本文聚焦在 `CModel` 改动的“边界与落地顺序”，以便与上述专题组合实施。

## 7. 里程碑与验收指标
- M0（阶段 A 完成）
  - 订阅驱动事件触发、2D block 重排、原子合并、事件位图、关闭调试回读。
  - KPI：单轮传播耗时↓，kernel 调用次数↓，删值/事件比↑，Host API 时间占比↓。
- M1（阶段 B-1）
  - Trail 回溯替代整层复制；`CreateNewLevel`/`BackLevel` 改造完成。
  - KPI：回溯密集场景下内存带宽占用与整体耗时显著下降。
- M2（阶段 B-2，可选）
  - 设备侧事件队列 + Persistent Kernel 或 CUDA Graph 重放版本对比验收。

## 8. 验证与回归
- 构建：
  - `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_ARCHITECTURES="80;87"`
  - `cmake --build build -j$(nproc)`
- 解析/冒烟：
  - `./build/cpim_test_parser --list_only --bench_manifest samples/bench/BMPath.xml`
  - `./build/cpim_test_parser`（解析首个实例并输出统计）
- 回归：针对同一基线对比 `enforceGAC` 总时长、迭代次数、事件数、删值量、Host API 时间占比。
- Nsight：采集内核带宽、L2 命中、占用、分支效率；确保禁用内核 `printf`。

## 9. 风险与回退
- 订阅触发正确性：确保删值后的相邻约束均被重新检查（幂等与不丢事件）。
- Trail 回滚一致性：所有写回路径必须记录 old 值；回滚顺序与写入顺序一致。
- 预案：通过宏开关保留“旧路径”（`neiCon` 扫描/整层复制），便于 A/B 与快速回退。

## 10. 下一步（实施建议）
- 先实施阶段 A：不改接口、改 kernel 与事件触发，通常即可获得第一阶收益。
- 随后评估回溯压力决定是否引入 trail；最后再考虑 Persistent/Graph。

