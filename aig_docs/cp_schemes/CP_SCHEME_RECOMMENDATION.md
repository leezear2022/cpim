# Jetson Orin 约束传播方案推荐：Scheme F (Bitmap) + Hybrid S

## 1. 核心推荐 (Core Recommendation)

基于对 Jetson Orin 平台特性（统一内存、Ampere 架构、有限 SM 资源）的分析，我们推荐采用 **"Bitmap Frontier (Scheme F)"** 作为基础数据结构，并融合 **"Hybrid Adaptive Scheduling (Scheme S)"** 的执行策略。

### 方案概览

*   **数据结构：** 使用 **位图 (Bitmap)** 代替显式队列来管理待传播的约束集合。
    *   `Queue_Current`: 当前轮次需要处理的约束位图。
    *   `Queue_Next`: 下一轮次产生的约束位图。
*   **执行模型：** **持久化内核 (Persistent Kernel)** + **混合调度 (Hybrid Scheduling)**。
    *   **大负载：** 启动 GPU 持久化内核，在设备端循环直到不动点。
    *   **小负载：** 直接在 CPU 端利用统一内存进行位图扫描和传播，避免 Kernel 启动开销。

---

## 2. 方案优势 (Why This Combination?)

### 2.1 针对 Jetson Orin 的极致优化
*   **统一内存利用：** Orin 的 CPU 和 GPU 共享物理内存。方案 F 的位图结构和 `GModel` 的 `bitDom` 都存储在统一内存中。这意味着我们可以零拷贝地在 CPU 和 GPU 之间切换执行引擎。
*   **减少原子争用：** 相比于 Scheme C/M 的显式队列（Ring Buffer），Bitmap 方案使用 `atomicOr` 分散在整个位图中，几乎消除了高并发下的原子操作争用。
*   **天然去重：** 约束传播是幂等的（重复检查无副作用）。Bitmap 天然保证了同一个约束在同一轮次只会被标记一次，无需额外的 `in_queue` 检查逻辑。

### 2.2 解决 "小任务" 痛点
*   **问题：** 在约束传播的尾声或小规模子问题中，活跃约束可能只有几十个。此时启动一个 GPU Kernel（耗时 ~5-10us）可能比实际计算还慢。
*   **对策 (Hybrid S)：** 利用 Scheme S 的思想，在传播入口检查活跃约束数量（或上一轮变化量）。如果低于阈值（例如 4 * SM数），直接用 CPU 的 NEON 指令集快速处理 Bitmap，完全跳过 GPU 启动。

---

## 3. 实施路线图 (Implementation Roadmap)

建议分三个阶段落地，逐步逼近最优性能。

### 阶段 1: 基础版 (Scheme F - Host Loop)
**目标：** 验证 Bitmap 数据结构和传播逻辑的正确性。
**工作：**
1.  在 `GModel` 中增加 `GACControl` 结构和两个 Bitmap (`d_queue_bitmap_A`, `d_queue_bitmap_B`)。
2.  实现 `BitmapGACKernel`：接收当前 Bitmap，执行一轮传播，将新任务标记到下一轮 Bitmap。
3.  在 Host 端 (`EnforceGAC`) 编写 `while` 循环，控制 Kernel 的反复启动和 Bitmap 的交换 (Swap)。

### 阶段 2: 持久化版 (Scheme F - Persistent)
**目标：** 消除 Kernel 启动开销，提升中大规模算例性能。
**工作：**
1.  将 Host 端的 `while` 循环移入 GPU Kernel 内部，形成 `PersistentGACKernel`。
2.  使用 `grid.sync()` (Cooperative Groups) 在设备端实现全局同步和 Bitmap 交换。
3.  引入 `GACControl` 中的 `active_count` 或 `inconsistent` 标志作为设备端终止条件。

### 阶段 3: 混合自适应版 (Scheme F + Hybrid S)
**目标：** 全场景性能覆盖，适配 Jetson 嵌入式特性。
**工作：**
1.  在 `EnforceGAC` 入口引入启发式判断：
    ```cpp
    if (estimated_workload < CPU_THRESHOLD) {
        RunBitmapPropagateOnCPU(); // 利用统一内存，零拷贝
    } else {
        LaunchPersistentGPUKernel();
    }
    ```
2.  调优 `CPU_THRESHOLD`，找到 Orin 平台上的最佳切换点。

---

## 4. 关键数据结构 (Key Data Structures)

```cpp
struct GACControl {
    int inconsistent_flag;   // 全局不一致标志
    int scanner_index;       // Bitmap 扫描游标 (用于 Block 间负载均衡)
    long long deletions;     // 累计删值数
    int iterations;          // 传播轮数
};

class GModel {
    // ...
    GACControl* d_gac_control;
    u32* d_queue_bitmap_A; // Current Frontier
    u32* d_queue_bitmap_B; // Next Frontier
    int bitmap_size_words;
    // ...
};
```

## 5. 结论

该方案结合了 Scheme F 的高效数据结构和 Scheme S 的灵活调度策略，是目前针对 Jetson Orin 平台最稳健、最高效的约束传播实现路径。
