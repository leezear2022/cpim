# CModel 优化与重构整合计划（cuSAC · Jetson Orin）

本文将 `GPU_CMODEL_OPT_PLAN.md` 的“快速收益优化”与 `GPU_CMODEL_REFACTOR_PLAN.md` 的“渐进式重构里程碑（M0–M5）”合并，形成一份统一、可执行、可回退的整合方案。目标是在保持 CModel 外部接口基本不变的前提下，获得 2–3x 的端到端加速，并为后续更深层重构保留演进空间。

## 1. 结论与范围
- 阶段A（快收益）：无需对 CModel 做“大改”。通过订阅驱动事件、Kernel 组织重排、原子降压、事件位图化、去调试回读与同步，即可获得显著收益（约 2–3x）。
- 阶段B（有限重构）：引入 trail 回溯（替代整层复制）、设备端事件队列、CUDA Graph 或持久化 Kernel，进一步降低往返与带宽开销。对外接口保持不变，属于内部实现重构。

## 2. KPI 与度量
- enforceGAC 总时长（ms）、每轮 events/ms、删值/事件比、迭代次数。
- Kernel 启动次数、Host API 时间占比、SM 占用、L2 命中与内存带宽（Nsight）。
- 回溯场景下的层推进/回滚时延与带宽占用。

## 3. 路线图（整合版里程碑）
- M0（基础清理与基线，低风险，1周）
  - 移除/宏控 Kernel printf；设备端统计替代打印。
  - 减少 `cudaDeviceSynchronize`：用 Stream+Event 串接关键内核，最终一次同步。
  - 增加 CUDA API 错误检查宏；清理大量注释代码与区域折叠。
  - CMake 架构：添加 `-DCMAKE_CUDA_ARCHITECTURES="80;87"`（Orin Ampere）。
  - 产出：基线脚本与指标汇总，作为后续对照。

- M1（适配器层，低风险，1.5周）
  - 引入 `CModelAdapter`（FromHModel/FromIntermediate），解耦 HModel，与 Normalizer/IntermediateModel 对齐。
  - `CModel` 新构造函数接受 Adapter，旧构造委托到新构造；功能等价、性能无回归。

- M2（事件驱动与设备压缩，中风险，1周）
  - 订阅表驱动事件：在传播内核中使用 `d_subscription + d_subscription_offset` 直接标记相邻约束，移除 `neiCon` 全表扫描。
  - 事件标记位图化：`d_ConPre` 由 int 改为位图；压缩前按需膨胀或使用 CUB 选择。
  - 设备端压缩：将 `compress_Main()` 下沉为 device compact 内核；引入戳记去重（stamp）避免每轮清零。

- M3（CsCheck Kernel 重排与原子降压，中风险，1周）
  - 线程组织：2D block（`x=32` 为 bit，`y=kBitDomIntSize` 为 word 段），去掉对 word 段的 for 循环，y 维共享内存切片，x 维 `__ballot_sync` 聚合一次性写回。
  - 写回与计数：共享内存暂存“写前 word”，差分 `__popc` 聚合后一次 `atomicSub`，减少热点原子。
  - 小域特化：域长≤32 走单 word 快路径；统一大域路径，去分支与特殊 case。
  - 禁用默认回读：以宏关闭 `bitDomCopy()` 等调试回读。

- M4（执行模型：CUDA Graph/固定网格，中风险，1周）
  - 固定网格+边界检查：Graph 捕获 `compress→cscheck→reduce→swap` 子图，动态大小通过 device 端计数指针读入。
  - Host 仅回放子图直至终止，减少 launch 开销；保留非 Graph 路径以便回退。

- M5（回溯与持久化，可选，中高风险，2–3周）
  - Trail 回溯：记录 `(var, word_idx, old_word)` 与 `dom_size` 旧值，替代 `CreateNewLevel()` 整层复制；`BackLevel()` 按 trail 回滚。
  - 设备队列/持久化 Kernel：以环形队列维护 `(c,x)` 事件，persistent kernel 驻留 SM，循环“取事件→传播→判停”。与 Graph 保持宏开关二选一，便于 A/B。

## 4. 关键实施要点（与现有代码对齐）
- 传播内核 CsCheck（订阅驱动 + 2D block）
  - 以订阅 CSR 替代 `neiCon` 扫描，仅触发相邻约束。
  - 共享内存缓存 x/y 的 bitDom word 段，`__ballot_sync` 得到每 32 值的支持掩码，warp 首 lane 对应 word 写回。
  - 写回前后差分计算删值个数，合并一次 `atomicSub(dom_size)`，并置位事件位图。

- 事件压缩（Device Compact + 戳记）
  - 使用 `stamp` 数组替代清零：置位时写入当前轮戳记，压缩核仅拣选等于当前戳记的约束。
  - 压缩结果写入 `d_MConEvt`（设备侧计数器 `atomicAdd` 增长）。

- Trail 回溯（阶段B）
  - 在 `AssignValue/RemoveValue/CsCheckMain*` 的写回路径同步写入 trail（按顺序），回溯按逆序恢复。
  - 保持单层工作域：`bitDom` 不再层叠复制，仅 trail 驱动回滚。

- 内存与对齐
  - bitDom/bitSup SoA 布局、`uint4` 对齐加载；UM 建议 `cudaMemAdvisePreferredLocation(GPU)` + 关键阶段 `cudaMemPrefetchAsync`。
  - 临时缓冲（事件队列等）使用 `cudaMallocAsync` + `cudaMemPool_t` 复用。

## 5. 风险与回退策略
- 回退开关（CMake）：
  - `CPIM_GPU_M0_OPTIMIZATIONS`、`CPIM_GPU_USE_ADAPTER`、`CPIM_GPU_DEVICE_COMPRESS`、`CPIM_GPU_USE_GRAPHS`、`CPIM_GPU_WARP_OPTIMIZATIONS`、`CPIM_GPU_USE_PERSISTENT`。
- 正确性保障：
  - 订阅触发的完备性（删值→相邻约束必重检）；Trail 回滚顺序/幂等；事件戳记不遗漏。
- 调试策略：
  - 禁止默认 kernel 打印；统一设备端统计写回；必要时用宏开启少量抽样日志。

## 6. 验收与回归
- 构建：
  - `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_ARCHITECTURES="80;87"`
  - `cmake --build build -j$(nproc)`
- 解析/冒烟：
  - `./build/cpim_test_parser --list_only --bench_manifest samples/bench/BMPath.xml`
  - `./build/cpim_test_parser`（解析首个实例并输出统计）
- 指标：
  - 单轮传播耗时↓、kernel 次数↓、Host API 时间占比↓、删值/事件比↑、早停命中率↑，且与 CPU 参考一致。
- Nsight：
  - 关注 L2 命中、带宽、分支效率、占用；确认无 `printf` 干扰。

## 7. 与专题文档的关系
- CsCheck 内核细化：`aig_docs/GPU_CSCHECK_OPT_PLAN.md`
- GAC 流水线与队列：`aig_docs/GPU_GAC_PIPELINE_PLAN.md`
- 模型/数据表示：`aig_docs/GPU_MODEL_DATA_PLAN.md`
- 渐进式重构详情：`aig_docs/GPU_CMODEL_REFACTOR_PLAN.md`
- 快速收益摘要：`aig_docs/GPU_CMODEL_OPT_PLAN.md`

## 8. 实施顺序与时间线（合并版）
- 周1：M0 完成并产出基线（禁 printf、降同步、设备统计、错误检查）。
- 周2–3：M1 适配器 + M2 订阅驱动与设备压缩（含位图/戳记）。
- 周4：M3 CsCheck 2D block 与原子降压，关闭默认回读。
- 周5：M4 Graph 固定网格版本（保留回退）。
- 周6–8（可选）：M5 Trail 回溯与持久化 Kernel A/B。

---

对比说明（与原两份文档差异）：
- 本文将 OPT 侧“订阅驱动、2D block、原子降压、位图化、禁调试回读”等快措施并入 REFACTOR 侧的 M0–M5 里程碑，并统一了里程碑命名与实施顺序。
- 去除了两文档中重复的背景叙述，保留与代码直接相关的“怎么改、在哪里改、如何验证”。
- 明确了接口不变前提下的改动边界，并将 Trail/持久化放到可选阶段，降低一次性风险。

