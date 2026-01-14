# enforceGAC 流水线一体化规划（事件压缩 × CsCheck 协同）

目标：不强行合并为单一 kernel，而是通过“流水线与调度策略”优化 `enforceGAC` 的整体吞吐，降低往返与无效工作，提升 Jetson 平台下的稳定性与可扩展性。

## 1. 流水线架构
- 设备常驻结构：
  - 订阅 CSR：`var_offsets` + `var2cons`；可选反向 `cons_offsets` + `cons2vars`。a
  - 事件队列：`events_A`, `events_B` 双缓冲（或环形队列），以 32/64 bit key 编码 `(c<<16)|x`。
  - 戳记去重：`seen[c]`（或 `(c,x)` 粒度）；使用自增 `cur_stamp` 避免清空数组。
- 流水阶段：
  1) seed/seed_small：根据“新增赋值/删值”的变量集合播种事件；
  2) compress_dev：基于 CSR 扩展为受影响的约束集合（去重）写入 `events_A`；
  3) cscheck_dev：消化 `events_A`，对 `(c,x)` 按域值做支持检查和删值，统计删值与新影响变量；
  4) reduce/next：若删值>0，构造下一轮变量 change 标记或直接填充 `events_B`；
  5) swap：交换 A/B 进入下一轮；直到 `events` 为空或失败。

## 2. 调度策略
- 固定网格 + 线程内边界检查：便于 **CUDA Graph** 捕获与回放；减少每轮 Host 改参。
- 批量与短路：
  - `warp-per-event`：一个 warp 消化一个 `(c,x)`，lane 处理不同 word 段；`__any_sync` 早停；
  - scope 排序（按域长/度升序），先用“便宜维度”筛除；
  - 小元数（2/3 元）特化内核，移除循环与分支；
- 事件分桶：按 arity/度/约束块区分，CTA 只处理相近数据，提升 L2 命中与合并写入概率。
- 原子降压：per‑warp 本地缓冲 + 末尾聚合写回；局部队列 + 合并，减少全局热点。

## 3. 过渡与变体
- 小工作量快路径（可选）：当 `num_events < T_small` 时使用轻量 kernel 或 Host 直构（UM 写入），避免大网格空转。
- 残留见证（Residue）：为 `(c,x,a)` 缓存上轮见证索引，下一轮从 residue 附近开始，有效降低平均位运算量。
- 稳定顺序：对 `events` 做稳定排序（如按 `c` 升序），提升同约束相邻的访存局部性。

## 4. 与执行模型的耦合
- Graph‑replay：将 `compress → cscheck → reduce → swap` 捕获为一轮子图，设备端写 `d_should_continue`；Host 回放直到终止。
- Persistent Kernel：设备端维护 `events` 双缓冲与戳记，`grid.sync()` 轮次同步；Host 只负责播种与外层决策。
- 阈值切换：Graph 与 Persistent 预留宏/选项，方便 A/B 测试与回退。

## 5. 内存与 UM 策略
- 预分配并复用 `events`/`seen`/`change_flags`；`__managed__` + `cudaMemAdvisePreferredLocation(GPU)` + 关键点 `cudaMemPrefetchAsync`；
- 数据对齐：bitDom/bitSup 统一 `uint4` 对齐矢量加载；CSR 数组 128‑bit 对齐；
- 统计写回：记录事件数、删值数、迭代次数、早停命中等指标，禁用内核 `printf`。

## 6. 正确性与增量
- 不变式：删值后，所有受影响约束必须被重新检查（重入队）；避免丢失传播；
- 事件幂等：重复入队通过戳记过滤；层间 `cur_stamp` 自增；
- 逐步替换：先上设备端压缩 + 现有 CsCheck；再引入双缓冲与稳定排序；最后切 Graph/Persistent。

## 7. 里程碑（对齐 Jetson 规划）
- P0：设备端压缩 + 戳记去重（与现内核对接），固定网格；
- P1：双缓冲 event + 事件分桶 + per‑warp 聚合写回；
- P2：scope 排序 + 小元数专用内核；Residue 见证缓存；
- P3：Graph‑replay 捕获整轮；
- P4：Persistent Kernel 版本 A/B 测试；
- P5：收敛阈值策略与 Nsight 驱动的细粒度调优。

## 8. KPI
- 单轮构建/检查耗时、`events/ms`、删值/事件比、早停命中；
- L2 命中、全局带宽、SM 占用；Host API 时间占比；
- 端到端 enforceGAC 时长与 CPU 版基线对比。

