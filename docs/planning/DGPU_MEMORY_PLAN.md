---
status: active
updated: 2026-01-27
---

# dGPU（RTX 4060/4090）内存规划：从 Jetson UMA 迁移以减少 memcpy

> 目标：在不推翻现有 UMA（Jetson Orin）实现的前提下，为 PCIe dGPU（4060/4090/2080 等）增加一套
> **可消融、可回退、可观测**的内存后端，使求解/预处理主循环尽量做到 **GPU-resident**，
> Host 只承担“任务下发 + 小结果回收”，从而减少/隐藏 `cudaMemcpy`。

## 0. 背景：UMA vs dGPU 的根本差异

- **Jetson Orin（UMA）**：CPU/GPU 共享物理内存；Managed Memory 的 page migration 成本相对可控，但也受
  `concurrentManagedAccess==0` 等限制影响（prefetch/访问策略需要谨慎）。
- **RTX 4060/4090（dGPU, PCIe）**：GPU 有独立显存；任何 Host↔Device 数据交换都需要跨 PCIe。
  如果继续“Host 触碰 Managed 大数组”，会触发频繁 page migration，通常比显式 `cudaMemcpyAsync` 更难控。

因此，dGPU 的规划重点是：**把热数据常驻 GPU、把必要传输做小、并把传输隐藏到流水线里**。

## 1. 数据分层：决定哪些数据需要 memcpy

将内存分为三类（对应“是否频繁变化/是否必须被 Host 读取”）：

### A. 只读模型数据（一次 H2D，长期驻留）

典型：`bitSupData`、`constraint_scopes`、`d_subscription`/offset、`allowed_masks` 等。

- dGPU 目标：**模型构建后只拷贝一次**到 device memory（`cudaMalloc/cudaMallocAsync`）。
- 运行中：0 次 memcpy。

### B. 基模型可变状态（运行中频繁更新，但 Host 不应直接读写）

典型：Base `bitDom`/`dom_size`、epoch/version（用于 deferred recheck）。

推荐策略：
- Host 侧只维护 **“删值 delta 列表”**（`(var,value)` 或压缩版），每轮小批量 H2D；
- GPU 侧用 `ApplyDeletionsKernel` 应用 delta、同步更新 epoch/version；
- Host 不拉回整域/整 bitmap，只拉回统计与少量结果列表。

### C. 批处理临时数据（任务/结果/统计）

- **任务列表（H2D）**：`ProbeTask{var,value}`，体积小，可 pinned + async。
- **结果列表（D2H）**：只回传 `DWO/UNKNOWN` 的稀疏列表（而不是每个 task 的大状态）。
- **统计（D2H）**：聚合 counters（几十个标量）即可。

## 2. 内存后端参数化（必须：保留 UMA 版本用于消融）

新增运行时选择（示例命名，可按工程风格调整）：

- `--mem_backend=auto|uma_managed|dgpu_device|managed_prefetch`

语义：
- `uma_managed`：保持 Jetson 现状（Managed/UMA），不引入额外 memcpy。
- `dgpu_device`：dGPU 推荐默认：只读/热数据都在 device memory；Host 通过 pinned staging 发送任务/接收结果。
- `managed_prefetch`：实验后端：Managed + `cudaMemPrefetchAsync` 到 GPU + **Host 禁止触碰**热数组（否则会抖）。

消融要求：
- 任一后端失败（对齐/容量/设备属性不满足）必须回退到现有稳定路径（通常是 `uma_managed` 或 `dgpu_device+simt`）。
- 输出必须记录 `mem_backend`（CSV/日志），否则无法对照。

## 3. “少 memcpy”的核心：GPU-resident 执行流

以 preprocess（`sac1_preprocess / sac3_preprocess / full_sac`）为例，dGPU 目标数据流：

1) **模型加载阶段**（一次性）  
   Host 构建模型 → 只读数组一次 H2D → 常驻 GPU。

2) **每轮 preprocess**（反复执行）  
   - Host 生成 `tasks`（或从队列取 batch）→ H2D tasks（小）  
   - GPU 执行（Stage1/Stage2/SAC3 等）→ 在 GPU 侧 compact 出 `failed/unknown` 列表  
   - D2H 拉回 `failed/unknown`（小） + 统计  
   - Host 只做“是否删值”的决策/队列更新；删值 delta 再小批量 H2D 交给 GPU 应用

关键点：**避免 D2H 回传整域**，也避免 Host 触碰 Managed 的热数组。

## 4. “隐藏 memcpy”的核心：pinned + async + 双缓冲流水线

即使任务/结果很小，也建议做成标准流水线（便于未来 batch 变大）：

- Host 端分配两套 pinned buffer：`tasks_buf[2]`、`results_buf[2]`（`cudaHostAlloc` 或 `cudaMallocHost`）
- 使用两个 stream：一个 compute stream、一个 copy stream（或同 stream + events）
- 典型节奏：
  - 批 i：H2D tasks(i) 与 批 i-1 的 kernel 重叠
  - 批 i：kernel(i) 与 批 i-1 的 D2H results(i-1) 重叠

并在统计里输出：
- `h2d_bytes / d2h_bytes / h2d_ms / d2h_ms`（用于证明“传输可忽略或可隐藏”）。

## 5. 内存分配策略：一次分配、全程复用

建议引入（或封装现有）统一的 Buffer/Pool 概念：
- 优先使用 `cudaMallocAsync` + stream-ordered memory pool（减少频繁 `cudaMalloc/cudaFree`）
- 所有 workspace/queue/bitmap 在初始化时一次性分配到“最大 batch”容量：
  - Stage2 的 task arrays、status arrays
  - SAC3 queue/deferred queue（如果仍在 Host，至少其 GPU side 的 task/status 要预留）

消融要求：
- 允许通过 flag 限制最大预留（例如 `--gpu_max_tasks_cap`），避免显存被一次吃满。

## 6. 分阶段实施计划（支线任务拆解）

### Phase 0：文档 + 观测指标（不改算法）
- 增加 `mem_backend` 选项的设计落点、所需统计字段（bytes/ms）。

### Phase 1：dGPU device memory 后端（最小风险）
- 只读模型数据迁移到 device memory。
- tasks/results 使用 pinned + async + 双缓冲。
- delta 删除用 GPU kernel 应用，Host 不再触碰热数组。

验收：preprocess CSV 中 `h2d/d2h` 明显缩小；整体 time 不劣化。

### Phase 2：进一步减少 Host 往返（可选）
- 将更多队列结构下沉到 GPU（device-side queue），Host 只拿最终删值列表/统计。

验收：Host↔GPU 往返次数下降；对深传播 hard-cases 的吞吐更稳定。

## 7. 与后续 P2/P3 的关系（为什么这条支线值得先做）

- P2 Route‑B（SoA/packing）与 P3 BMMA 的前提之一是：**world/域矩阵载体稳定且可复用**。
  dGPU 后端把热数据“常驻 + 可复用”后，才能更真实评估 bitGEMM/BMMA 的 ROI，
  否则容易被“搬运/迁移成本”淹没。

