# GModel 传播队列优化分析与改进方案

**日期**: 2025-01-04
**对比对象**: GModel.cu (Jetson 统一内存版) vs cuSAC copy.cu (传统独显版)
**分析重点**: 传播队列的设计、优化策略和性能瓶颈

---

## 目录

1. [传播队列原理](#传播队列原理)
2. [CModel 的队列设计（Thrust 方案）](#cmodel-的队列设计)
3. [GModel 的队列设计（CPU 方案）](#gmodel-的队列设计)
4. [对比分析](#对比分析)
5. [性能瓶颈识别](#性能瓶颈识别)
6. [优化方案](#优化方案)
7. [实现建议](#实现建议)

---

## 传播队列原理

### GAC 传播的基本流程

```
初始状态：所有约束都在队列中

循环直到队列为空：
  1. 从队列中取出所有约束事件
  2. 并行检查这些约束，删除不一致的域值
  3. 收集被修改的变量
  4. 将这些变量相关的约束重新加入队列
```

### 关键问题

**Q1: 如何高效维护队列？**
- 添加新事件（触发的约束）
- 去重（同一约束只处理一次）
- 批量处理（一次处理多个事件）

**Q2: 如何在 CPU/GPU 之间传递队列？**
- CPU 维护 → 每次迭代需要 CPU-GPU 同步
- GPU 维护 → 需要原子操作或 Thrust 库

---

## CModel 的队列设计（Thrust 方案）

### 数据结构

```cpp
// src/cuSAC copy.cu

// 全局变量（托管内存）
__managed__ int* S_ConPre;      // 约束前驱标记（是否在队列中）
__managed__ int3* S_ConEvt;     // 约束事件队列
__managed__ int3* S_Con;        // 约束主队列

// 成员变量（Thrust 向量）
thrust::device_vector<uint3> d_MCon;      // 所有约束（x, y, cid）
thrust::device_vector<uint3> d_MConEvt;   // 当前事件约束
thrust::device_vector<int> d_ConPre;      // 前驱标记（1=在队列，0=不在）
```

### 核心机制：Thrust Stream Compaction

```cpp
// 压缩队列：从 d_MCon 中筛选出 d_ConPre[i]=1 的约束
int CModel::compress_Main() {
    d_MConEvt.resize(d_MCon.size());

    // 使用 Thrust 的 copy_if 进行流压缩
    auto end = thrust::copy_if(
        d_MCon.begin(), d_MCon.end(),     // 输入：所有约束
        d_ConPre.begin(),                 // 判断条件数组
        d_MConEvt.begin(),                // 输出：活跃约束
        is_one()                          // 谓词：d_ConPre[i]==1
    );

    // 清空标记
    thrust::fill(d_ConPre.begin(), d_ConPre.end(), 0);

    // 返回事件数量
    return end - d_MConEvt.begin();
}

// 谓词函数
struct is_one {
    __host__ __device__ bool operator()(const int x) const {
        return x == 1;
    }
};
```

### 传播循环

```cpp
bool CModel::enforceGAC() {
    printf("-----------enforceGAC-----------\n");

    // 初始压缩：获取所有待处理的约束
    int num_ConEvt = compress_Main();
    cudaDeviceSynchronize();

    while (num_ConEvt != 0) {
        printf("-----------iteration-----------\n");

        // 启动内核检查约束
        CsCheckMain<<<num_ConEvt, dim3(kBitDomIntSize * 32, 1, 1),
                      kSharedMemSize>>>(
            thrust::raw_pointer_cast(d_ConPre.data()),
            thrust::raw_pointer_cast(d_MCon.data()),
            d_bitDom,
            thrust::raw_pointer_cast(d_cur_dom_size.data()),
            texObj_BitSup,
            texObj_MCon,
            num_ConEvt,
            current_level_
        );
        cudaDeviceSynchronize();

        // 检查失败标志
        if (!GAC_success) {
            return false;
        }

        printf("-----------end iteration-----------\n");

        // 再次压缩，获取新触发的约束
        num_ConEvt = compress_Main();
    }

    return true;
}
```

### 优点

✅ **GPU 原生队列管理**
- Thrust 库在 GPU 上高效实现流压缩
- 无需 CPU-GPU 数据传输

✅ **并行压缩**
- `thrust::copy_if` 利用 GPU 并行性
- 适合大规模约束网络

✅ **简洁代码**
- Thrust 封装了复杂的并行算法
- 易于维护

### 缺点

❌ **Thrust 库开销**
- 每次 `compress_Main` 调用都要启动 Thrust 内核
- 小规模问题（<100 约束）开销明显

❌ **内存分配**
- `d_MConEvt.resize()` 可能触发重新分配
- 频繁调用影响性能

❌ **同步点**
- 每次 `cudaDeviceSynchronize()` 阻塞 CPU
- 限制 CPU-GPU 流水线

❌ **调试困难**
- Thrust 内部实现复杂
- 出错时难以定位

---

## GModel 的队列设计（CPU 方案）

### 数据结构

```cpp
// src/GModel.cu (lines 457-611)

GacStats GModel::EnforceGAC(bool verbose, bool /*use_thrust_queue*/) {
    // CPU 端的队列（STL 容器）
    std::vector<int> queue;                     // 当前队列
    std::vector<int> next_queue;                // 下一轮队列
    std::vector<int> touched_vars;              // 被修改的变量
    std::vector<char> in_queue(num_constraints, 0);  // 去重标记
    std::vector<int> reset_list;                // 需要重置的标记

    // GPU 端的事件数组（统一内存）
    int* d_events = nullptr;
    cudaMallocManaged(&d_events, num_constraints * sizeof(int));
}
```

### 核心机制：CPU 维护 + GPU 批处理

```cpp
// 初始队列：所有有效约束
std::vector<int> queue;
queue.reserve(num_constraints);
for (int cid = 0; cid < num_constraints; ++cid) {
    if (constraint_scopes[cid].x >= 0) {
        queue.push_back(cid);  // ✅ CPU 端直接操作 STL
    }
}

// 传播循环
while (!queue.empty()) {
    ++stats.iterations;

    // 拷贝队列到 GPU
    const int num_events = static_cast<int>(queue.size());
    for (int i = 0; i < num_events; ++i) {
        d_events[i] = queue[i];  // ✅ 统一内存，CPU 直接写入
    }

    // GPU 检查约束
    CsCheckMainKernel<<<num_events, threads_per_block, shared_mem_bytes>>>(
        d_events, num_events, bitDom, bitSupData, constraint_scopes,
        bit_dom_int_size, max_dom_size, bitsup_per_constraint, removal,
        current_level_, bit_doms_int_size
    );
    cudaDeviceSynchronize();

    // CPU 端收集被修改的变量
    std::vector<int> touched_vars;
    for (int var = 0; var < num_vars; ++var) {
        bool changed = false;
        for (int w = 0; w < bit_dom_int_size; ++w) {
            if (removal[var * bit_dom_int_size + w] != 0) {
                changed = true;
                break;
            }
        }
        if (changed) {
            touched_vars.push_back(var);
        }
    }

    // CPU 端构建下一轮队列（去重）
    std::vector<int> next_queue;
    for (int var : touched_vars) {
        const auto& neighbors = var_to_constraints[var];  // CSR 邻接表
        for (int cid : neighbors) {
            if (in_queue[cid]) continue;  // ✅ O(1) 去重
            in_queue[cid] = 1;
            reset_list.push_back(cid);
            next_queue.push_back(cid);
        }
    }

    // 重置标记
    for (int cid : reset_list) {
        in_queue[cid] = 0;
    }
    reset_list.clear();

    // 交换队列
    queue.swap(next_queue);
}
```

### 优点

✅ **零 Thrust 依赖**
- 纯 STL + CUDA，无额外库
- 编译快，依赖少

✅ **统一内存友好**
- Jetson Orin 的零拷贝特性
- CPU 写 `d_events`，GPU 直接读取

✅ **灵活控制**
- CPU 端易于调试和修改
- 可以添加自定义启发式

✅ **小问题高效**
- 避免 Thrust 启动开销
- 适合 CSP 问题（通常 <1000 约束）

### 缺点

❌ **CPU-GPU 同步**
- 每次迭代都有 `cudaDeviceSynchronize()`
- CPU 等待 GPU 完成

❌ **队列拷贝**
- `for (int i = 0; i < num_events; ++i) d_events[i] = queue[i];`
- 虽然是统一内存，但仍然是顺序拷贝

❌ **变量遍历**
- `for (int var = 0; var < num_vars; ++var)` 检查所有变量
- 大规模问题（1000+ 变量）可能慢

❌ **未利用 GPU 并行构建队列**
- 下一轮队列构建完全在 CPU 端
- GPU 空闲时间

---

## 对比分析

### 表格对比

| 维度 | CModel (Thrust) | GModel (CPU) | 优劣 |
|------|----------------|--------------|------|
| **队列维护位置** | GPU (Thrust) | CPU (STL) | CModel 更纯粹 |
| **压缩算法** | `thrust::copy_if` | CPU 遍历 | CModel 并行，GModel 串行 |
| **去重机制** | GPU 标记数组 + Thrust 压缩 | CPU `std::vector<char>` | GModel 更简单 |
| **内存模型** | 设备内存 + Thrust 向量 | 统一内存 + STL | GModel 利用 Jetson 优势 |
| **同步开销** | 每次 `compress_Main` | 每次迭代 | 相当 |
| **库依赖** | Thrust (大) | 仅 STL | GModel 更轻量 |
| **调试难度** | 高（Thrust 黑盒） | 低（CPU 代码） | GModel 更易调试 |
| **小问题性能** | 一般（Thrust 开销） | 好 | GModel 胜 |
| **大问题性能** | 好（GPU 并行） | 一般 | CModel 胜 |
| **扩展性** | 受 Thrust API 限制 | 灵活 | GModel 胜 |

### 性能估算

#### 小问题（N-Queens-12: 12 变量，66 约束）

```
CModel (Thrust):
  compress_Main: ~0.1ms (Thrust 启动开销)
  CsCheckMain: ~0.05ms
  每次迭代: ~0.15ms

GModel (CPU):
  队列拷贝: ~0.001ms (66 个 int)
  CsCheckMainKernel: ~0.05ms
  变量遍历: ~0.01ms (12 个变量)
  构建队列: ~0.02ms
  每次迭代: ~0.08ms

GModel 快 ~2x ✅
```

#### 大问题（假设 1000 变量，50000 约束）

```
CModel (Thrust):
  compress_Main: ~0.2ms (GPU 并行压缩)
  CsCheckMain: ~5ms
  每次迭代: ~5.2ms

GModel (CPU):
  队列拷贝: ~0.5ms (50000 个 int)
  CsCheckMainKernel: ~5ms
  变量遍历: ~1ms (1000 个变量)
  构建队列: ~2ms (CPU 串行)
  每次迭代: ~8.5ms

CModel 快 ~1.6x ✅
```

---

## 性能瓶颈识别

### GModel 的主要瓶颈

#### 瓶颈 1: 队列拷贝（CPU → GPU）

**位置**: `src/GModel.cu:505-507`

```cpp
for (int i = 0; i < num_events; ++i) {
    d_events[i] = queue[i];  // ⚠️ 顺序拷贝
}
```

**问题**:
- 虽然是统一内存，但 CPU 顺序写入效率低
- 无法利用内存带宽

**优化**: 使用 `memcpy` 或向量化

```cpp
// 优化前
for (int i = 0; i < num_events; ++i) {
    d_events[i] = queue[i];
}

// 优化后
std::memcpy(d_events, queue.data(), num_events * sizeof(int));
// 或者
cudaMemcpy(d_events, queue.data(), num_events * sizeof(int),
           cudaMemcpyHostToDevice);
```

#### 瓶颈 2: 全变量遍历

**位置**: `src/GModel.cu:537-572`

```cpp
for (int var = 0; var < num_vars; ++var) {  // ⚠️ 遍历所有变量
    bool changed = false;
    const int base = var * bit_dom_int_size;
    for (int w = 0; w < bit_dom_int_size; ++w) {
        const u32 mask = removal[base + w];
        if (mask == 0u) continue;
        // ...
        changed = true;
    }
    if (changed) {
        touched_vars.push_back(var);
    }
}
```

**问题**:
- 即使只有少数变量被修改，仍然遍历所有变量
- O(num_vars) 复杂度

**优化**: GPU 并行收集 + 原子计数

```cpp
// GPU kernel 直接记录被修改的变量
__global__ void CollectTouchedVarsKernel(const u32* removal,
                                          int num_vars,
                                          int bit_dom_int_size,
                                          int* touched_vars,
                                          int* touched_count) {
    int var = blockIdx.x * blockDim.x + threadIdx.x;
    if (var >= num_vars) return;

    // 检查该变量是否被修改
    bool changed = false;
    const int base = var * bit_dom_int_size;
    for (int w = 0; w < bit_dom_int_size; ++w) {
        if (removal[base + w] != 0) {
            changed = true;
            break;
        }
    }

    // 如果修改，记录到输出数组
    if (changed) {
        int idx = atomicAdd(touched_count, 1);
        touched_vars[idx] = var;
    }
}
```

#### 瓶颈 3: 队列构建（CPU 串行）

**位置**: `src/GModel.cu:579-587`

```cpp
for (int var : touched_vars) {  // ⚠️ CPU 串行循环
    const auto& neighbors = var_to_constraints[var];
    for (int cid : neighbors) {
        if (in_queue[cid]) continue;
        in_queue[cid] = 1;
        reset_list.push_back(cid);
        next_queue.push_back(cid);
    }
}
```

**问题**:
- 虽然使用 CSR 邻接表，但串行遍历
- 大量变量时成为瓶颈

**优化**: GPU 并行构建队列

```cpp
// GPU kernel 并行添加邻居约束
__global__ void BuildNextQueueKernel(const int* touched_vars,
                                      int num_touched,
                                      const uint3* subscription,
                                      const int* subscription_offset,
                                      char* in_queue,
                                      int* next_queue,
                                      int* next_queue_size) {
    int tid = blockIdx.x * blockDim.x + threadIdx.x;
    if (tid >= num_touched) return;

    int var = touched_vars[tid];
    int start = subscription_offset[var];
    int end = subscription_offset[var + 1];

    for (int i = start; i < end; ++i) {
        int cid = subscription[i].z;

        // 原子 CAS 去重
        char old = atomicCAS(&in_queue[cid], 0, 1);
        if (old == 0) {  // 首次添加
            int idx = atomicAdd(next_queue_size, 1);
            next_queue[idx] = cid;
        }
    }
}
```

#### 瓶颈 4: 同步开销

**位置**: `src/GModel.cu:527, 534, 601`

```cpp
cudaDeviceSynchronize();  // ⚠️ 阻塞 CPU
```

**问题**:
- 每次迭代都同步
- CPU 空闲等待 GPU

**优化**: 使用 CUDA Stream 流水线

```cpp
cudaStream_t stream;
cudaStreamCreate(&stream);

// 启动内核（异步）
CsCheckMainKernel<<<..., stream>>>(...);

// CPU 继续工作（不等待）
// ...准备下一轮数据...

// 最后才同步
cudaStreamSynchronize(stream);
```

---

## 优化方案

### 方案 1: 混合队列管理（推荐）⭐

**核心思想**: 保留 CPU 队列简洁性，但用 GPU 加速瓶颈部分

#### 实现步骤

```cpp
GacStats GModel::EnforceGAC_Hybrid(bool verbose) {
    GacStats stats;

    // CPU 端队列（保留）
    std::vector<int> queue;
    std::vector<int> next_queue;

    // GPU 端辅助数组（统一内存）
    int* d_events;
    int* d_touched_vars;
    int* d_touched_count;
    int* d_next_queue;
    int* d_next_queue_size;
    char* d_in_queue;

    cudaMallocManaged(&d_events, num_constraints * sizeof(int));
    cudaMallocManaged(&d_touched_vars, num_vars * sizeof(int));
    cudaMallocManaged(&d_touched_count, sizeof(int));
    cudaMallocManaged(&d_next_queue, num_constraints * sizeof(int));
    cudaMallocManaged(&d_next_queue_size, sizeof(int));
    cudaMallocManaged(&d_in_queue, num_constraints * sizeof(char));

    // 初始队列
    for (int cid = 0; cid < num_constraints; ++cid) {
        if (constraint_scopes[cid].x >= 0) queue.push_back(cid);
    }

    while (!queue.empty()) {
        ++stats.iterations;

        // ✅ 优化1: 用 memcpy 拷贝队列
        std::memcpy(d_events, queue.data(), queue.size() * sizeof(int));

        // GPU 检查约束
        CsCheckMainKernel<<<queue.size(), threads_per_block, ...>>>(
            d_events, queue.size(), ...
        );

        // ✅ 优化2: GPU 并行收集被修改的变量
        *d_touched_count = 0;
        int blocks = (num_vars + 255) / 256;
        CollectTouchedVarsKernel<<<blocks, 256>>>(
            removal, num_vars, bit_dom_int_size,
            d_touched_vars, d_touched_count
        );
        cudaDeviceSynchronize();

        int num_touched = *d_touched_count;
        if (num_touched == 0) break;

        // ✅ 优化3: GPU 并行构建下一轮队列
        *d_next_queue_size = 0;
        memset(d_in_queue, 0, num_constraints);

        blocks = (num_touched + 255) / 256;
        BuildNextQueueKernel<<<blocks, 256>>>(
            d_touched_vars, num_touched,
            d_subscription, d_subscription_offset,
            d_in_queue, d_next_queue, d_next_queue_size
        );
        cudaDeviceSynchronize();

        // 拷贝回 CPU 队列（可选：也可以保持在 GPU）
        int next_size = *d_next_queue_size;
        queue.assign(d_next_queue, d_next_queue + next_size);
    }

    // 释放
    cudaFree(d_events);
    cudaFree(d_touched_vars);
    // ...

    return stats;
}
```

**预期收益**:
- ✅ 队列拷贝：5-10x 加速（memcpy vs 循环）
- ✅ 变量收集：10-50x 加速（GPU 并行 vs CPU 串行）
- ✅ 队列构建：5-20x 加速（GPU 并行 vs CPU 串行）
- **总体预期**: 20-40% 端到端性能提升

---

### 方案 2: 纯 GPU 队列（激进）🚀

**核心思想**: 完全在 GPU 端维护队列，避免 CPU-GPU 传输

#### 使用 Thrust（类似 CModel）

```cpp
GacStats GModel::EnforceGAC_Thrust(bool verbose) {
    // 使用 Thrust 向量
    thrust::device_vector<int> d_queue;
    thrust::device_vector<int> d_next_queue;
    thrust::device_vector<char> d_in_queue(num_constraints, 0);

    // 初始化队列
    for (int cid = 0; cid < num_constraints; ++cid) {
        if (constraint_scopes[cid].x >= 0) {
            d_queue.push_back(cid);
        }
    }

    while (!d_queue.empty()) {
        // GPU 检查约束
        CsCheckMainKernel<<<d_queue.size(), ...>>>(
            thrust::raw_pointer_cast(d_queue.data()),
            d_queue.size(), ...
        );

        // GPU 收集被修改的变量
        thrust::device_vector<int> d_touched_vars;
        // ... (使用 Thrust 算法)

        // GPU 构建下一轮队列
        d_next_queue.clear();
        // ... (使用 Thrust 算法)

        d_queue.swap(d_next_queue);
    }
}
```

**优点**:
- ✅ 完全 GPU 端，无 CPU-GPU 传输
- ✅ Thrust 优化的并行算法

**缺点**:
- ❌ 引入 Thrust 依赖（与 GModel 简洁性矛盾）
- ❌ 小问题可能变慢（Thrust 启动开销）

---

### 方案 3: 流水线优化（高级）🔄

**核心思想**: 使用 CUDA Stream 重叠计算和数据传输

```cpp
GacStats GModel::EnforceGAC_Pipelined(bool verbose) {
    cudaStream_t stream1, stream2;
    cudaStreamCreate(&stream1);
    cudaStreamCreate(&stream2);

    // 双缓冲队列
    int* d_events[2];
    cudaMallocManaged(&d_events[0], ...);
    cudaMallocManaged(&d_events[1], ...);

    int buffer = 0;

    while (!queue.empty()) {
        // Stream1: 启动当前批次的 kernel
        CsCheckMainKernel<<<..., stream1>>>(
            d_events[buffer], ...
        );

        // Stream2: 同时准备下一批次的数据（如果有）
        // ...

        // 等待当前批次完成
        cudaStreamSynchronize(stream1);

        // 交换缓冲
        buffer = 1 - buffer;
    }
}
```

**预期收益**:
- ✅ CPU-GPU 重叠，减少空闲时间
- ✅ 适合多迭代的传播

---

## 实现建议

### 短期优化（1-2天实现）

#### 优化 1: 使用 memcpy 拷贝队列

**文件**: `src/GModel.cu:505-507`

```cpp
// 修改前
for (int i = 0; i < num_events; ++i) {
    d_events[i] = queue[i];
}

// 修改后
if (num_events > 0) {
    std::memcpy(d_events, queue.data(), num_events * sizeof(int));
}
```

**预期收益**: 5-10% 整体加速

#### 优化 2: Early Exit 优化

**文件**: `src/GModel.cu:537-572`

```cpp
// 添加计数器，提前退出
int num_changed = 0;
for (int var = 0; var < num_vars; ++var) {
    bool changed = false;
    const int base = var * bit_dom_int_size;
    for (int w = 0; w < bit_dom_int_size; ++w) {
        const u32 mask = removal[base + w];
        if (mask == 0u) continue;
        changed = true;
        ++num_changed;
        break;  // ✅ 提前退出内层循环
    }
    if (changed) {
        touched_vars.push_back(var);
    }
}

// ✅ 如果没有变化，提前退出
if (num_changed == 0) break;
```

**预期收益**: 2-5% 整体加速

---

### 中期优化（1周实现）

#### 实现方案 1: 混合队列管理

见上文"方案 1"详细实现。

**工作量**:
- 实现 `CollectTouchedVarsKernel`: 0.5 天
- 实现 `BuildNextQueueKernel`: 1 天
- 集成和测试: 2 天
- 性能调优: 2 天

**预期收益**: 20-40% 整体加速

---

### 长期优化（2-4周实现）

#### 1. 自适应队列策略

**思想**: 根据问题规模动态选择 CPU 或 GPU 队列

```cpp
GacStats GModel::EnforceGAC_Adaptive(bool verbose) {
    if (num_constraints < 100 && num_vars < 50) {
        return EnforceGAC_CPU(verbose);  // 小问题用 CPU
    } else if (num_constraints > 10000) {
        return EnforceGAC_Thrust(verbose);  // 大问题用 Thrust
    } else {
        return EnforceGAC_Hybrid(verbose);  // 中等问题用混合
    }
}
```

#### 2. 多流并发

**思想**: 分批处理约束，不同批次用不同 Stream

```cpp
const int num_streams = 4;
cudaStream_t streams[num_streams];

// 分批处理队列
for (int batch = 0; batch < num_batches; ++batch) {
    int stream_id = batch % num_streams;
    CsCheckMainKernel<<<..., streams[stream_id]>>>(...);
}
cudaDeviceSynchronize();
```

#### 3. 持久化 kernel

**思想**: Kernel 持续运行，从队列中取任务（类似线程池）

```cpp
__global__ void PersistentGACKernel(int* global_queue,
                                     int* queue_size,
                                     /* ... */) {
    while (true) {
        // 原子获取任务
        int task_id = atomicAdd(queue_size, -1);
        if (task_id < 0) break;  // 队列空

        int cid = global_queue[task_id];
        // 处理约束 cid
        // ...
    }
}
```

**预期收益**: 减少 kernel 启动开销，适合迭代多的问题

---

## 实验评估计划

### 基准测试

| 测试用例 | 变量数 | 约束数 | 域大小 | 预期迭代数 |
|---------|--------|--------|--------|-----------|
| queens-4 | 4 | 6 | 4 | 3-5 |
| queens-8 | 8 | 28 | 8 | 5-10 |
| queens-12 | 12 | 66 | 12 | 10-15 |
| queens-20 | 20 | 190 | 20 | 20-30 |
| queens-50 | 50 | 1225 | 50 | 50-100 |
| queens-100 | 100 | 4950 | 100 | 100-200 |

### 性能指标

```cpp
struct GacStats {
    int iterations;       // 迭代次数
    int deletions;        // 删除的域值数量
    bool inconsistent;    // 是否不一致

    // 新增性能指标
    double total_time_ms;         // 总时间（毫秒）
    double kernel_time_ms;        // Kernel 执行时间
    double queue_build_time_ms;   // 队列构建时间
    double queue_copy_time_ms;    // 队列拷贝时间
    int max_queue_size;           // 最大队列长度
    double avg_queue_size;        // 平均队列长度
};
```

### 测试脚本

```bash
#!/bin/bash
# benchmark_gac.sh

for size in 4 8 12 20 50 100; do
    echo "Testing queens-$size..."

    # 原始版本
    ./gmodel_solver queens-${size}_ext.xml > results_baseline_$size.txt 2>&1

    # 优化版本1 (memcpy)
    ./gmodel_solver_opt1 queens-${size}_ext.xml > results_opt1_$size.txt 2>&1

    # 优化版本2 (混合队列)
    ./gmodel_solver_opt2 queens-${size}_ext.xml > results_opt2_$size.txt 2>&1

    # 提取性能数据
    python3 parse_results.py results_*.txt
done
```

---

## 总结

### 当前状态

| 方面 | GModel (CPU 队列) | CModel (Thrust 队列) |
|------|------------------|---------------------|
| **代码复杂度** | 低 ✅ | 中等 |
| **小问题性能** | 好 ✅ | 中等 |
| **大问题性能** | 中等 | 好 ✅ |
| **Jetson 优化** | 好 ✅ | 中等 |
| **可维护性** | 高 ✅ | 中等 |
| **扩展性** | 高 ✅ | 受限 |

### 推荐路线

1. **第一步**（立即实施）:
   - ✅ 使用 `memcpy` 拷贝队列
   - ✅ 添加 early exit 优化
   - 预期收益: 5-15%

2. **第二步**（1周内）:
   - ✅ 实现混合队列管理（方案 1）
   - GPU 并行收集变量 + 构建队列
   - 预期收益: 20-40%

3. **第三步**（可选，2-4周）:
   - 自适应策略
   - 多流并发
   - 持久化 kernel
   - 预期收益: 10-20% (在第二步基础上)

### 关键原则

- ✅ **保持简洁**: 优先考虑代码可读性
- ✅ **利用统一内存**: 充分发挥 Jetson 优势
- ✅ **增量优化**: 逐步改进，每步都测量
- ✅ **适合问题规模**: CSP 问题通常不大，避免过度设计

---

**文档结束**
