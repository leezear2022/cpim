---
status: archived
---

> 说明：本文为早期（Gemini）讨论稿/草案，部分内容（如 `CModel`、host-driven GAC 等）已被当前
> `GModel + Batch-2 Stage2(Persistent)` 主线实现替代，仅供回顾思路与记录历史，不作为现状依据。
>
> 现行主线请以以下文档为准：
> - `docs/gpu/BATCH_AC_DESIGN.md`
> - `docs/planning/SACGPU_DESIGN.md`

# GPU 求解器优化与 Batch AC-GPU 设计方案

本文档包含两部分内容：
1. **现有 AC-GPU 代码优化分析**：基于 `src/solver/gpu/` 下现有代码的分析与改进建议。
2. **SAC-GPU 转 Batch AC-GPU 设计**：基于 `Batch_AC.md` 理念的工程实现设计。

---

## 第一部分：AC-GPU (GAC) 现有代码优化分析

通过分析 `GModel.cu` (及 `cuSAC.cu` 中的 GAC 实现)，我们发现当前的 GPU 约束传播（Constraint Propagation）实现存在以下主要瓶颈，具有较大的优化空间。

### 1.1 现状瓶颈：Host-Driven 的传播循环

目前的 GAC 传播逻辑（如 `CModel::enforceGAC`）采用了 **Host-Driven（主机驱动）** 的迭代模式：

```cpp
// 伪代码表示现有逻辑
while (num_ConEvt != 0) { // 1. CPU 判断队列状态
    // 2. 启动 Kernel 进行一轮检测
    CsCheckMain<<<...>>>(...); 
    
    // 3. 队列压缩与更新 (涉及 D2H 同步或隐式同步)
    num_ConEvt = compress_Main(); 
}
```

**问题分析：**
*   **Kernel Launch Overhead**：每一轮传播迭代（特别是迭代后期，队列虽小但必须执行）都需要 CPU 介入启动 Kernel，Launch Latency 相比于极短的 GPU 执行时间（Microseconds 级别）占比过大。
*   **同步开销**：`compress_Main` 使用 `thrust::copy_if` 和 `thrust::distance`。虽然 Thrust 优化很好，但获取新的 `num_ConEvt` 大小本质上需要某种形式的同步（CPU 需要知道在这个 Stream 上下一轮发多少个线程，或者至少需要一次 Stream 间的同步），打断了 GPU 的流水线执行。

### 1.2 优化方案：Persistent Kernel / Device-Side Loop

**核心改进**：将整个不动点迭代（Fixed-Point Iteration）下沉到 GPU 内部。

**方案 A：Persistent Threads (推荐)**
*   启动一个足够填满 GPU SM 的 Persistent Kernel。
*   Kernel 内部是一个 `while(true)` 循环，从全局 Worklist (Queue) 中取任务执行。
*   产生的新任务（Domain Change 导致的约束入队）直接通过 `atomicAdd` 写入 Device 端的 Output Queue。
*   利用 `__syncthreads()` 或 Cooperative Groups 的 `this_grid().sync()` 进行全局同步或 Block 级同步，交换 Input/Output Queue 指针。
*   **收益**：完全消除传播过程中的 CPU Launch 开销。CPU 仅需 Launch 一次 Kernel 即可等待通过。

**方案 B：Device-Side Launch (CUDA Dynamic Parallelism)**
*   父 Kernel 负责检查队列状态，如果不为空则 Launch 子 Kernel。
*   **注意**：在 Jetson 或小规模任务上，CDP 的开销可能依然较大，通常不如 Persistent Threads 方案高效。

### 1.3 其他优化点
*   **Shared Memory 队列**：在 Block 内部维护小型 Shared Memory 队列，先在 Block 内聚合产生的事件，再批量写入 Global Memory 队列，减少对全局队列计数器的原子争用。
*   **Memory Coalescing**：当前的 `bitSup` 存取使用了 Texture Object，这是极其正确的选择，应予以保留。但需确认 `bitDom` 的读写在 Warp 内是否对齐。

---

## 第二部分：SAC-GPU 转换为 Batch AC-GPU 设计设计

本部分旨在落实 `docs/planning/Batch_AC.md` 中的理念，设计并实现基于 **Micro-batching (Space-Batching)** 的 SAC 求解器。

### 2.1 目标定义
将 **SAC (Singleton Arc Consistency)** 的计算过程（即：对所有 `(var, val)` 假设，分别运行 AC 检查一致性）转化为 **Batch AC** 过程，在 GPU 上并行执行多个 Singleton Check。

### 2.2 核心架构：Space-Batching (Micro-Batch)

鉴于 SAC 需要检查的假设数量巨大（总值数 `NumVars * DomSize`），无法一次性全部放入 GPU，我们需要采用 Micro-batching 策略。

*   **Batch Size (`B`)**：一次并行处理的世界（World）数量。建议值 `32`, `64` 或 `128` (取决于显存容量与 SM 资源)。每个 World 对应一个 Singleton Assumption。

### 2.3 数据结构设计 (BatchGModel)

我们需要一套支持“多世界”的数据结构。

#### 2.3.1 域存储 (Batch Domain)
为了支持 `B` 个世界并行，需要分配专属的 Global Memory 存储每个世界的域状态。

**推荐内存布局：Array of Structures (Block-Parallel Friendly)**
为了让每个 CUDA Block 独立负责一个 World 的计算，保持数据的局部性：

```cpp
// 逻辑维度: [BatchSize][NumVars][BitDomIntSize]
// 物理大小: BatchSize * NumVars * BitDomIntSize * sizeof(u32)
u32* d_batch_bitDom; 
```

*   **访问模式**：Block `k` 处理 World `k`。线程 `t` 读取 `d_batch_bitDom[k * ...]`。
*   **优势**：现有的 `CsCheckMain` kernel 逻辑几乎可以直接复用，只需加上 Base Offset。

*(注：如果追求由 Batch_AC.md 提及的 BitGEMM 极致性能，可采用 Structure of Arrays `[NumVars][BitDomIntSize][BatchSize]` 并结合 Bit-Packing，但这会显著增加代码复杂度，建议作为二期优化目标)*

#### 2.3.2 队列与状态
*   `d_batch_queue`: 大小 `BatchSize * MaxQueueSize`。每个 World 拥有独立的传播队列。
*   `d_batch_status`: 大小 `BatchSize`。记录每个 World 是否发生 DWO (Domain Wipe Out)。

### 2.4 核心 Kernel 设计：`BatchSAC_Kernel`

**Grid & Block 配置**
*   `GridDim.x`: `BatchSize` (每个 Block 处理一个 World)。
*   `BlockDim.x`: 与现有 `CsCheckMain` 类似，建议 `32` 或 `BitDomIntSize * 32`，足以覆盖一个变量的 bitDom 操作。

**Kernel 伪代码逻辑**

```cpp
__global__ void BatchSAC_Kernel(
    u32* d_batch_bitDom,     // [Batch][Vars]...
    int* d_batch_queue,      // [Batch][QSize]
    int* d_batch_q_counts,   // [Batch]
    int* d_batch_status,     // [Batch]
    int2* d_assumptions,     // [Batch] 输入的假设列表 (var, val)
    // ... 其他只读模型数据 (BitSup, Constraints) ...
) {
    // 1. 确定当前 Block 负责的 World ID
    int world_idx = blockIdx.x;
    if (world_idx >= num_assumptions) return;
    
    // 2. World 初始化 (Copy from Base Model)
    // 每个线程协作复制 Base BitDom 到 d_batch_bitDom[world_idx]
    CopyBaseModelToWorld(world_idx);
    __syncthreads();
    
    // 3. 应用 Singleton Assumption
    int2 assumption = d_assumptions[world_idx];
    ApplyAssumption(world_idx, assumption.x, assumption.y);
    // 将受影响的约束加入该 World 的局部队列
    InitQueue(world_idx);
    __syncthreads();
    
    // 4. Device-Side Propagation Loop (Persistent Logic)
    while(true) {
        __syncthreads(); // 确保上一轮写完
        
        int q_size = d_batch_q_counts[world_idx];
        if (q_size == 0) break; // 不动点到达
        if (d_batch_status[world_idx] == INCONSISTENT) break; // 提前剪枝
        
        // 取出任务
        // 执行 Propagate (逻辑复用 CsCheckMain, 但操作的是 d_batch_bitDom)
        // 此处需要将 CsCheckMain 的逻辑内联或以此为蓝本重写为 Device Function
        
        __syncthreads();
    }
}
```

### 2.5 Host 端执行流 (Controller)

设计一个 `BatchACSolver` 类来管理流程：

1.  **Generate Tasks**: 生成所有待检查的 `(var, val)` 列表（过滤掉本来就不在域中的值）。
2.  **Loop over Batches**:
    *   填充 `d_assumptions` 缓冲（大小 `B`）。
    *   Launch `BatchSAC_Kernel<<<B, Threads>>>`。
    *   （可选）在 Stream 中异步拷贝结果 `d_batch_status` 回 Host。
3.  **Process Results**:
    *   如果某 `(var, val)` 的 World 状态为 `INCONSISTENT`，则在 Base Model 中永久删除该值 `val` from `var`。
4.  **Recycle**: 如果 Base Model 发生了删值，可能需要重新做一轮 GAC，然后继续下一轮 Batch SAC (因为 Base 变了)。

### 2.6 内存估算 (Jetson Orin Friendly)

假设：
*   Vars: 2000
*   DomSize: 1000 (32 ints)
*   BitDom Size per World: 2000 * 128 bytes ≈ 256 KB.
*   若 Batch Size = 128，则 `BatchBitDom` 总大小 ≈ 32 MB。
*   Jetson Orin 显存充足，完全可行。甚至可以开启更大的 Batch (`512` 或 `1024`) 以充分利用 CUDA Cores。

---

## 总结与建议

1.  **优先实施 GAC 优化**：将 `GModel` 的传播改为 **Persistent Kernel** 模式。这不仅提升现有求解器性能，也是实现高效 Batch SAC Kernel 的基础（因为 Batch SAC 必须在 Kernel 内部完成传播循环）。
2.  **复用代码**：将 `CsCheckMain` 的核心逻辑提取为 `__device__` 函数，使其既能被单世界的 Persistent GAC 调用，也能被多世界的 Batch SAC 调用。
3.  **分步实现**：
    *   Step 1: 重构 GAC 为 Persistent Kernel。
    *   Step 2: 实现 Host 端 Batch 调度框架 + 简单的 Block-Parallel Batch Kernel。
