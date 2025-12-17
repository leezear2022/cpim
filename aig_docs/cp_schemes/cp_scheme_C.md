# GPU并发约束传播优化方案总结

---

## 一、问题背景

### 当前瓶颈

```
Search()
  └─→ for each value:
        ├─→ AssignValue()
        ├─→ EnforceGAC()   ← 被反复调用，占用80%+时间
        └─→ Search(next)
```

`EnforceGAC()`内部是**串行迭代**所有约束，无法利用GPU并行能力。

### 本质特征：多生产者多消费者

```
域变化 → 约束入队（生产）
              ↓
         约束执行（消费）
              ↓
         可能产生新的域变化（又变成生产者）
```

---

## 二、核心优化思想

借鉴两篇论文的关键技术：

| 来源 | 借鉴技术 | 解决的问题 |
|------|---------|-----------|
| CSP论文 | 快照 + 原子提交 | 多约束并发修改同一变量域 |
| CSP论文 | 动态提交方案 | 消除轮次间同步屏障 |
| VeriSAT | 并发游标 | 多任务并行执行 |
| VeriSAT | 冲突广播 | 快速终止无效计算 |

**理论基础**：约束传播是**合流的**——无论执行顺序如何，最终结果相同。这是并行化的根本保障。

---

## 三、整体架构

```
┌─────────────────────────────────────────────────────────────────┐
│                    GPU 并发 GAC 传播引擎                          │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │              无锁工作队列 (Lock-Free Queue)                 │  │
│  │              [C₁, C₂, C₃, C₄, C₅, ...]                    │  │
│  └───────────────────────────────────────────────────────────┘  │
│         │                                                       │
│         │  多线程块并发取任务                                     │
│         ▼                                                       │
│  ┌──────────┬──────────┬──────────┬──────────┬─────────┐       │
│  │  Block0  │  Block1  │  Block2  │  Block3  │   ...   │       │
│  │          │          │          │          │         │       │
│  │ 取约束C  │ 取约束C' │ 取约束C''│ 取约束...│         │       │
│  │ 位矩阵乘 │ 位矩阵乘 │ 位矩阵乘 │ 位矩阵乘 │         │       │
│  │ 计算支持 │ 计算支持 │ 计算支持 │ 计算支持 │         │       │
│  └────┬─────┴────┬─────┴────┬─────┴────┬─────┴─────────┘       │
│       │          │          │          │                        │
│       └──────────┴────┬─────┴──────────┘                        │
│                       ▼                                         │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │           变量域 D[] (Atomic Bit Vectors)                  │  │
│  │                                                           │  │
│  │   atomicAnd 更新域                                         │  │
│  │        │                                                  │  │
│  │        ├─→ 若有变化 → 相关约束入队（动态提交）               │  │
│  │        └─→ 若域为空 → 设置 inconsistent = true             │  │
│  └───────────────────────────────────────────────────────────┘  │
│                                                                 │
│  终止条件：(队列空 && 活跃线程=0) || inconsistent               │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

---

## 四、关键数据结构

### 4.1 原子位向量（变量域）

```cpp
struct AtomicBitDomain {
    unsigned int* bits;
    int num_words;
    
    // 核心：原子AND实现并发安全的域裁剪
    __device__ bool atomicAndMask(const unsigned int* mask) {
        bool changed = false;
        for (int i = 0; i < num_words; ++i) {
            unsigned int old = atomicAnd(&bits[i], mask[i]);
            if (old != (old & mask[i])) changed = true;
        }
        return changed;
    }
    
    __device__ bool isEmpty();
};
```

**作用**：多个约束可同时裁剪同一变量，原子AND保证不丢失任何裁剪。

### 4.2 无锁工作队列

```cpp
struct LockFreeQueue {
    int* data;
    unsigned int* head;  // 原子头指针
    unsigned int* tail;  // 原子尾指针
    int capacity;
    
    __device__ bool enqueue(int cid);  // 生产者调用
    __device__ int dequeue();          // 消费者调用
};
```

**作用**：多线程块无锁并发存取任务。

### 4.3 约束入队标志

```cpp
struct ConstraintFlags {
    unsigned int* flags;  // 位图
    
    __device__ bool tryMark(int cid);  // 原子标记，防止重复入队
    __device__ void clear(int cid);    // 取出后清除标记
};
```

**作用**：避免同一约束被重复加入队列。

---

## 五、核心算法流程

### 5.1 主Kernel

```cpp
__global__ void ConcurrentPropagateKernel(...) {
    while (true) {
        // 1. 检查全局冲突标志
        if (*inconsistent) return;
        
        // 2. 从队列取任务
        int cid = queue->dequeue();
        if (cid < 0) {
            if (*active_workers == 0) return;  // 真正结束
            continue;  // 等待新任务
        }
        
        // 3. 执行位矩阵乘（约束传播）
        bool changed = PropagateConstraint(cid, domains);
        
        // 4. 检查DWO
        if (HasEmptyDomain()) {
            *inconsistent = true;
            return;
        }
        
        // 5. 动态提交新任务
        if (changed) {
            for (相关约束 c') {
                if (flags->tryMark(c')) {
                    queue->enqueue(c');
                }
            }
        }
    }
}
```

### 5.2 位矩阵乘（表约束传播）

```cpp
__device__ void ComputeSupport(Constraint& c, int var_idx, ...) {
    // 1. 共享内存缓存相关变量域
    __shared__ unsigned int cached[MAX_ARITY][MAX_WORDS];
    // 协作加载...
    
    // 2. 检查每个元组是否有效
    for (int t = 0; t < num_tuples; ++t) {
        bool valid = CheckTupleValidity(t, cached);
        if (valid) {
            // 将该元组对var_idx的值加入支持集
            AddToSupport(c.tuples[t][var_idx]);
        }
    }
    
    // 3. 返回支持集用于原子AND
}
```

### 5.3 主机端调用

```cpp
GacStats GModel::EnforceGAC_GPU() {
    // 1. 初始化：所有约束入队
    InitQueue<<<...>>>(d_queue, d_flags, num_constraints);
    
    // 2. 清空状态
    cudaMemset(d_inconsistent, 0, sizeof(bool));
    cudaMemset(d_active_workers, 0, sizeof(int));
    
    // 3. 启动并发传播（持久Kernel）
    ConcurrentPropagateKernel<<<num_blocks, 256>>>(...);
    cudaDeviceSynchronize();
    
    // 4. 读取结果
    cudaMemcpy(&stats.inconsistent, d_inconsistent, ...);
    return stats;
}
```

---

## 六、SM 8.0+ 特定优化

针对A100/RTX 30系列/RTX 40系列的增强：

| 优化项 | 技术 | 效果 |
|--------|------|------|
| 异步内存加载 | `cuda::memcpy_async` | 隐藏内存延迟 |
| 硬件归约 | `__reduce_or_sync` | 快速合并支持集 |
| L2缓存驻留 | `cudaAccessPropertyPersisting` | 热点数据常驻缓存 |
| 更大共享内存 | 164KB | 缓存更多约束数据 |
| 更快原子操作 | 硬件优化 | 无锁队列吞吐提升 |

```cpp
// 异步加载示例
cuda::memcpy_async(&shared[i], &global[i], sizeof(int), pipe);

// 硬件归约示例
unsigned int result = __reduce_or_sync(0xFFFFFFFF, my_support);
```

---

## 七、与串行方案的对比

| 维度 | 原串行方案 | GPU并发方案 |
|------|-----------|-------------|
| **约束执行** | 一次一个 | 多个线程块同时执行 |
| **域更新** | 直接写 | 原子AND，并发安全 |
| **任务调度** | 固定迭代 | 无锁队列动态取任务 |
| **同步方式** | 每轮结束等待 | 无屏障，动态提交 |
| **冲突检测** | 函数返回值 | 全局标志广播 |
| **终止判断** | 队列空 | 队列空 + 活跃线程=0 |

---

## 八、预期收益

| 优化点 | 机制 | 预期效果 |
|--------|------|----------|
| 多约束并行 | 多线程块同时处理 | 吞吐量提升10-100× |
| 消除同步屏障 | 动态提交 | 减少空闲等待 |
| 原子域更新 | atomicAnd | 正确性 + 无锁 |
| 快速冲突终止 | 全局标志 | 避免无效计算 |
| 共享内存缓存 | 约束数据缓存 | 减少全局访存延迟 |
| SM 8.0+特性 | 异步拷贝/硬件归约 | 进一步隐藏延迟 |

---

## 九、编译与运行

```bash
# SM 8.0 (A100)
nvcc -arch=sm_80 -O3 --use_fast_math solver.cu -o solver

# SM 8.6 (RTX 3080/3090)
nvcc -arch=sm_86 -O3 --use_fast_math solver.cu -o solver

# SM 8.9 (RTX 4090)
nvcc -arch=sm_89 -O3 --use_fast_math solver.cu -o solver
```

---

## 十、总结

本方案的核心是将**串行GAC传播**改造为**GPU多生产者多消费者并发模型**：

1. **理论基础**：约束传播的合流性保证并行正确性
2. **并发执行**：多线程块同时处理不同约束
3. **原子更新**：`atomicAnd`实现并发安全的域裁剪
4. **动态调度**：无锁队列 + 动态提交消除同步屏障
5. **快速终止**：全局冲突标志广播
6. **硬件适配**：利用SM 8.0+的异步拷贝、硬件归约等特性

整个方案**不需要修改GPU驱动**，完全基于标准CUDA API实现。