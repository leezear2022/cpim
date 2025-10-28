# cuSAC 架构对比：当前 vs 优化

## 架构对比图

### 当前架构（Baseline）

```
┌─────────────────────────────────────────────────────────────┐
│                      CPU (Host)                             │
├─────────────────────────────────────────────────────────────┤
│  enforceGAC() {                                             │
│    while (num_active != 0) {                                │
│      ┌──────────────────────────┐                           │
│      │ compress_Main()          │ ← Thrust CPU 同步         │
│      │ (CPU 端压缩)             │   (~15 µs/次)             │
│      └──────────────────────────┘                           │
│             ↓                                                │
│      ┌──────────────────────────┐                           │
│      │ Kernel 启动              │ ← Overhead                │
│      └──────────────────────────┘   (~5 µs/次)              │
│             ↓                                                │
├─────────────────────────────────────────────────────────────┤
│                      GPU (Device)                           │
├─────────────────────────────────────────────────────────────┤
│      ┌──────────────────────────┐                           │
│      │ CsCheckMain<<<...>>>()   │                           │
│      │  - 读取 bitDom (全局)    │ ← 重复读取                │
│      │  - 读取 bitSup (纹理)    │   (~每线程 2 次)          │
│      │  - 原子操作 (atomicAnd)  │ ← 冲突                    │
│      └──────────────────────────┘   (~10 µs/次)             │
│             ↓                                                │
│      ┌──────────────────────────┐                           │
│      │ cudaDeviceSynchronize()  │ ← 阻塞 CPU                │
│      └──────────────────────────┘   (~10 µs/次)             │
│    }                                                         │
│  }                                                           │
└─────────────────────────────────────────────────────────────┘

总延迟/迭代: ~40 µs
queens-12 (10 迭代): ~400 µs
瓶颈: CPU-GPU 同步 (60%)
```

---

### 优化后架构（Phase 1: Shared Memory + Warp）

```
┌─────────────────────────────────────────────────────────────┐
│                      CPU (Host)                             │
├─────────────────────────────────────────────────────────────┤
│  enforceGAC() {                                             │
│    while (num_active != 0) {                                │
│      compress_Main();  // 仍在 CPU，但频率降低              │
│      ↓                                                       │
├─────────────────────────────────────────────────────────────┤
│                      GPU (Device)                           │
├─────────────────────────────────────────────────────────────┤
│      ┌──────────────────────────────────────┐               │
│      │ CsCheckMainOptimized<<<...>>>()      │               │
│      │  ┌────────────────────────────────┐  │               │
│      │  │ Shared Memory                  │  │               │
│      │  │  - s_bitDom_x[8]  ← 一次加载   │  │               │
│      │  │  - s_bitDom_y[8]  ← 一次加载   │  │               │
│      │  │  - s_deletion[8]  ← 累积删除   │  │               │
│      │  └────────────────────────────────┘  │               │
│      │  ↓                                    │               │
│      │  Warp-Level Voting                    │               │
│      │  - __ballot_sync() ← 避免分支        │               │
│      │  - 批量原子操作    ← 减少冲突        │               │
│      └──────────────────────────────────────┘               │
│    }                                                         │
│  }                                                           │
└─────────────────────────────────────────────────────────────┘

总延迟/迭代: ~15 µs (2.7x 加速)
queens-12 (10 迭代): ~150 µs
瓶颈: Kernel 启动 + 同步 (40%)
```

---

### 优化后架构（Phase 2: Persistent Kernel）

```
┌─────────────────────────────────────────────────────────────┐
│                      CPU (Host)                             │
├─────────────────────────────────────────────────────────────┤
│  enforceGAC_Persistent() {                                  │
│    *d_converged = 1;                                        │
│    ┌────────────────────────────┐                           │
│    │ 单次 Kernel 启动           │ ← 仅一次！                │
│    │ PersistentGACKernel<<<>>>  │                           │
│    └────────────────────────────┘                           │
│             ↓                                                │
├─────────────────────────────────────────────────────────────┤
│                      GPU (Device)                           │
├─────────────────────────────────────────────────────────────┤
│    ┌────────────────────────────────────────────┐           │
│    │ Persistent Kernel (GPU 内部循环)          │           │
│    │  for (iter = 0; iter < max_iter; ++iter) {│           │
│    │    ┌────────────────────────────────────┐ │           │
│    │    │ GPU 端压缩 (CUB/Warp)              │ │           │
│    │    │  - 流压缩在 GPU                    │ │           │
│    │    │  - 无需 CPU 同步                   │ │           │
│    │    └────────────────────────────────────┘ │           │
│    │    ↓                                       │           │
│    │    ┌────────────────────────────────────┐ │           │
│    │    │ 约束检查 (Shared Mem + Warp)      │ │           │
│    │    │  - 所有优化生效                    │ │           │
│    │    └────────────────────────────────────┘ │           │
│    │    ↓                                       │           │
│    │    if (*d_converged) break; // GPU 端退出 │           │
│    │  }                                         │           │
│    └────────────────────────────────────────────┘           │
│             ↓                                                │
│    cudaDeviceSynchronize();  // 仅最后一次                  │
│  }                                                           │
└─────────────────────────────────────────────────────────────┘

总延迟: ~40 µs (所有迭代)
queens-12 (10 迭代): ~40 µs (10x 加速!)
瓶颈: 计算本身 (80%)
```

---

### 优化后架构（Phase 3: CPU/GPU 自适应）

```
┌─────────────────────────────────────────────────────────────┐
│                      CPU (Host)                             │
├─────────────────────────────────────────────────────────────┤
│  enforceGAC_Adaptive() {                                    │
│    auto device = ChooseDevice(num_constraints, dom_size);   │
│    ↓                                                         │
│    if (device == CPU) {  // 小问题                          │
│      ┌─────────────────────────────────┐                    │
│      │ CPU 直接处理                    │                    │
│      │  - 统一内存零拷贝               │                    │
│      │  - 避免 kernel 启动             │                    │
│      │  - 约束数 < 20 时最快           │                    │
│      └─────────────────────────────────┘                    │
│    }                                                         │
│    else if (device == HYBRID) {  // 中等问题                │
│      ┌─────────────────────────────────┐                    │
│      │ CPU/GPU 协同                    │                    │
│      │  - CPU 处理少量约束             │                    │
│      │  - GPU 批处理大量约束           │                    │
│      │  - 动态切换                     │                    │
│      └─────────────────────────────────┘                    │
│    }                                                         │
│    else {  // 大问题                                        │
├─────────────────────────────────────────────────────────────┤
│                      GPU (Device)                           │
├─────────────────────────────────────────────────────────────┤
│      ┌─────────────────────────────────┐                    │
│      │ Persistent Kernel (最优)        │                    │
│      │  - GPU 内部迭代                 │                    │
│      │  - Shared Memory                │                    │
│      │  - Warp Primitives              │                    │
│      │  - 异步预取                     │                    │
│      └─────────────────────────────────┘                    │
│    }                                                         │
│  }                                                           │
└─────────────────────────────────────────────────────────────┘

总延迟:
  queens-4:  ~10 µs  (CPU 直接处理)
  queens-12: ~40 µs  (GPU Persistent)
  queens-50: ~200 µs (GPU Persistent)
平均加速: 10-20x (问题依赖)
```

---

## 性能对比表

| 架构 | queens-4 | queens-12 | queens-50 | 适用场景 |
|------|----------|-----------|-----------|---------|
| **Baseline** | 80 µs | 400 µs | 2000 µs | 无 |
| **Phase 1** | 60 µs | 150 µs | 800 µs | 通用改进 |
| **Phase 2** | 50 µs | 40 µs | 200 µs | 中大问题 |
| **Phase 3** | **10 µs** | **40 µs** | **200 µs** | **所有问题** |
| **加速比** | **8x** | **10x** | **10x** | - |

---

## 内存访问模式对比

### Baseline

```
每次迭代（66 个约束，queens-12）:
  
  bitDom 读取:  66 × 2 vars × 1 word × 4 bytes = 528 bytes
  bitSup 读取:  66 × 12 vals × 8 bytes = 6.3 KB (纹理缓存)
  
  原子操作:     ~100 次 atomicAnd (冲突严重)
  CPU-GPU 同步: 2 次/迭代 (compress + sync)
  
  总带宽: ~7 KB/迭代
  有效带宽利用率: ~5%
```

### Optimized (Phase 2)

```
所有迭代（10 次，queens-12）:
  
  bitDom 读取:  10 × 66 × 1 load = 660 loads (Shared Memory 缓存)
  bitSup 读取:  10 × 66 × 12 vals × 8 bytes = 63 KB (纹理缓存)
  
  原子操作:     ~50 次/迭代 (Warp 批量)
  CPU-GPU 同步: 1 次 (仅最后)
  
  总带宽: ~65 KB (所有迭代)
  有效带宽利用率: ~40%
```

---

## 统一内存优化策略

### 1. 零拷贝访问（Jetson Orin 优势）

```cpp
// CPU 直接访问 GPU 内存（小问题）
bool CModel::enforceGAC_CPU() {
  // bitDom 是统一内存，CPU 可以直接访问
  uint32_t* cpu_bitDom = d_bitDom;  // 零拷贝！
  
  for (int c_id : active_constraints) {
    uint3 con = h_MCon[c_id];
    uint32_t dom_x = cpu_bitDom[con.x * kBitDomIntSize];
    uint32_t dom_y = cpu_bitDom[con.y * kBitDomIntSize];
    
    // CPU 直接操作，无需 kernel 启动
    // ...
  }
}
```

### 2. 显式预取（大问题）

```cpp
// GPU 计算前预取
cudaMemPrefetchAsync(d_bitDom, size, 0, stream);  // → GPU

// GPU 计算
PersistentGACKernel<<<...>>>(...);

// CPU 访问前预取回
cudaMemPrefetchAsync(d_bitDom, size, cudaCpuDeviceId, stream);  // → CPU
```

### 3. 页面锁定（中等问题）

```cpp
// 锁定页面防止迁移
cudaMemAdvise(d_bitDom, size, cudaMemAdviseSetPreferredLocation, 0);  // GPU
cudaMemAdvise(d_bitDom, size, cudaMemAdviseSetAccessedBy, cudaCpuDeviceId);  // CPU 也可访问
```

---

## 实施检查清单

### Phase 1（2-3 天）

- [ ] 修改 `CsCheckMain` 添加 Shared Memory
  - [ ] `s_bitDom_x`, `s_bitDom_y`
  - [ ] `s_deletion_mask`
  - [ ] 协作加载逻辑

- [ ] 添加 Warp-Level Primitives
  - [ ] `__ballot_sync()`
  - [ ] 批量原子操作

- [ ] 测试验证
  - [ ] queens-4, queens-12
  - [ ] 性能对比

### Phase 2（5-7 天）

- [ ] 实现 `PersistentGACKernel`
  - [ ] GPU 内部循环
  - [ ] 收敛检测
  - [ ] 约束活跃标记

- [ ] GPU 端压缩（CUB）
  - [ ] `cub::DeviceSelect::Flagged`
  - [ ] 流压缩逻辑

- [ ] 测试验证
  - [ ] 多个基准
  - [ ] 迭代次数验证

### Phase 3（5-10 天）

- [ ] 实现自适应调度
  - [ ] `ChooseDevice()` 启发式
  - [ ] CPU 实现
  - [ ] Hybrid 实现

- [ ] 统一内存优化
  - [ ] 预取策略
  - [ ] 页面锁定

- [ ] 全面测试
  - [ ] 所有基准
  - [ ] 性能曲线

---

## 关键代码片段

### Shared Memory 加载模式

```cuda
// 协作加载（Coalesced Access）
__shared__ uint32_t s_data[MAX_SIZE];

int tid = threadIdx.x;
int stride = blockDim.x;

// 所有线程协作加载（高效）
for (int i = tid; i < data_size; i += stride) {
  s_data[i] = global_data[i];
}
__syncthreads();
```

### Warp 投票模式

```cuda
// 检测 warp 内的模式
uint32_t predicate = (value == target);
uint32_t mask = __ballot_sync(0xFFFFFFFF, predicate);

// 只有第一个匹配的线程执行
if (__popc(mask) > 0) {
  int first_lane = __ffs(mask) - 1;
  if (threadIdx.x == first_lane) {
    // 代表 warp 执行操作
    do_work();
  }
}
```

### GPU 端收敛检测

```cuda
__shared__ int s_local_active;

// 块内检测
if (threadIdx.x == 0) s_local_active = 0;
__syncthreads();

if (has_work) {
  atomicAdd(&s_local_active, 1);
}
__syncthreads();

// 全局更新
if (threadIdx.x == 0 && s_local_active > 0) {
  atomicAdd(d_global_active, s_local_active);
}
```

---

**下一步**: 立即开始 Phase 1 实现（Shared Memory + Warp）？
