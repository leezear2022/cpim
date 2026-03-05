# Batch AC GPU 设计（基于 GModel）

本文在 `docs/planning/Batch_AC.md` 的概念框架上，落地到当前
`src/solver/gpu/GModel.cu`/`src/solver/gpu/GModelSolver.cu` 的实现细节，
给出从 SAC-GPU 过渡到 Batch AC-GPU 的工程化设计方案与分阶段路径。

## 1. 目标与边界

目标：
- 在 GPU 上实现 **SAC-checking pass 的 Batch AC**，即一次性对一批
  singleton 赋值世界并行做 AC 到不动点，并返回哪些值失败（DWO）。
- 充分复用现有的 GModel 数据结构（`bitDom`/`bitSupData`/bitmap frontier）。
- 优先适配 Jetson Orin 的统一内存特性，减少 CPU/GPU 往返。

非目标（首版不做）：
- 完整 SAC-closure 的固定点循环（可在外层迭代完成）。
- 支持非二元 supports 约束类型（保持与现有 GModel 的边界一致）。

## 2. 现状速览（GModel / GModelSolver）

### 2.1 GModel（GPU 传播）
- 领域表示：`bitDom[var * bit_dom_int_size + word]`（统一内存）。
- 传播调度：bitmap frontier（`d_queue_bitmap_A/B`）。
- 核心 kernel：`ExecuteConstraintCheck_BpC` + `BitmapGACKernel`。
- 具备持久化版：`PersistentGACKernel`（Cooperative Groups）。

### 2.2 GModelSolver（搜索驱动）
- DFS 赋值 + `EnforceGAC(false)` 传播。
- 当前调用没有传入 `assigned_var`，未触发增量 frontier 初始化。
- 搜索过程以 CPU 递归为主，GPU 仅负责 GAC 传播。

## 3. AC-GPU 优化空间（基于现有实现）

1) **使用增量 frontier**  
`GModel::EnforceGAC` 已支持 `assigned_var`，但 `GModelSolver::Search`
仍调用 `EnforceGAC(false)`。传入刚赋值变量可显著减少初始 frontier。

2) **默认启用持久化 kernel**  
`EnforceGAC_Persistent` 已落地，建议在支持 cooperative launch 的设备上
作为默认路径，降低每轮传播的 CPU/GPU 同步开销。

3) **frontier 非空检测下沉到 GPU**  
当前 `EnforceGAC` 每轮在 CPU 扫描 `d_queue_bitmap_B` 判断是否为空。
可增加一个轻量 reduction kernel，或直接复用持久化 kernel 的内部检测逻辑。

4) **bitSup 访问路径与缓存策略**  
`bitSupData` 为统一内存全局数组，可尝试：
- 只读缓存（`__ldg` / `__restrict__`）；
- 复用纹理路径（`texObj_BitSup`）对高复用行进行缓存。

5) **线程粒度与 shared 复用**  
当前 BpC 采用“每线程=1 value”的 cuSAC 风格，适合小域。  
对大域实例可考虑 **word-level 并行**（线程处理 word），减小线程数，
提升访存密度，降低 warp divergence。

6) **域大小维护与回溯成本**  
GAC 内已在 GPU 更新 `d_cur_dom_size`，回溯时仍在 CPU 重新扫描域。
可通过记录“被修改变量集合 + 变更 delta”减少回溯时的全量 popcount。

## 4. Batch AC 设计概览

### 4.1 语义定义
Batch AC = 对一批 singleton 赋值世界并行执行 AC 到不动点，输出每个
世界是否产生 DWO。该操作只对应 **SAC-checking pass**，不是闭包。

### 4.2 数据结构与内存预算

基础数据（已存在）：
- `bitDom`/`bitSupData`/`constraint_scopes`/`subscription`。

Batch 数据：
- `bitDom_batch[B][num_vars][bit_dom_int_size]`
- `frontier_batch_A/B[B][bitmap_words]`
- `result_inconsistent[B]`、`result_deletions[B]`

内存估算（仅域位集）：
```
bytes ≈ B * num_vars * bit_dom_int_size * 4
```
在 Jetson Orin 上建议 `B` 从 16/32 起步，并以实际实例规模调优。

### 4.3 Kernel 方案

#### 方案 A：Batch-1（时间维 batch，落地快）
思路：一次性提交一批 probe 任务，GPU 端 **顺序** 处理每个 probe 的
GAC，减少 host<->device 往返和 launch 开销。

关键点：
- 设备端维护“probe 列表”（`(var, value)`）。
- 每个 probe 在 GPU 上完成：赋值 → GAC → 记录结果 → 恢复域。
- 可基于 `PersistentGACKernel` 做设备内循环，避免 CPU 驱动多次迭代。

优点：实现成本低、内存压力小。  
缺点：算子仍是“串行多次 GAC”，数据复用有限。

#### 方案 B：Batch-2（空间维 micro-batch，中期目标）
思路：同时维护 `B` 个世界并行传播，尽量复用 `bitSup` 行访问。

推荐初版配置：
- `B <= 32`，一 warp 处理一个 value，lane 对应 world。
- 以 `(constraint, value)` 为单元读取 `bitSup` 行，lane 并行计算
  `has_support`，避免重复加载 `bitSup`。

内核形态示意：
- Block 维度：`blockIdx.x` 绑定约束 `cid`，
  `blockIdx.y` 绑定 world tile。
- Thread 维度：`threadIdx.x` 处理 value 或 word。

注意点：
- 每个 world 有独立 frontier；bitmap 可以按 world 分块存储。
- 每轮传播后只输出 `inconsistent[B]`，不必同步完整域大小。

## 5. Batch AC 端到端流程

1) **候选生成**  
CPU 从当前域枚举 `(var, value)`（可参考 MSAC 的 SAC1/SAC3 选取策略）。

2) **分批调度**  
将候选列表切成大小为 `B` 的 micro-batches。

3) **批处理传播**  
每个 batch：  
`base bitDom` → 复制/初始化 batch buffer → 单例赋值 → Batch AC kernel。

4) **结果回写**  
读取 `inconsistent[B]`，对失败值执行 `RemoveValue`，并记录 Trail。
若需要 SAC-closure，则外层迭代该 pass 直到无删值。

## 6. 接口与模块建议

建议新增轻量封装，避免侵入 GModel 核心：
- `struct BatchACOptions { int micro_batch; int max_iters; bool use_persistent; }`
- `class GModelBatchRunner`
  - `RunSACPass(const std::vector<std::pair<int,int>>& probes, ...)`
  - `ApplyRemovals(...)`

同时保留 `GModel::EnforceGAC(_Persistent)` 作为单世界回退路径。

## 7. 验证与基准

正确性：
- 与 CPU `MSAC3bit`（SAC1）在小实例上对照删值集合。
- 先在 `dump_gmodel` 场景做单实例验证。

性能：
- 记录每次 batch 的 probe 数、平均传播轮数、GPU 时间。
- 评估 `B` 对吞吐与内存的影响曲线。

## 8. 风险与回退

- **内存压力**：`B` 过大时统一内存页迁移频繁，需自动降级为 Batch-1。
- **设备能力**：不支持 cooperative launch 时回退到普通 `BitmapGACKernel`。
- **实现复杂度**：若 Batch-2 延期，可先落地 Batch-1 验证功能正确性。
