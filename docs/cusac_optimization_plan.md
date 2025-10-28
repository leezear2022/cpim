# cuSAC.cu 约束传播优化方案

## 当前架构分析

### 核心流程（enforceGAC）

```cuda
// cuSAC.cu:157-196
bool CModel::enforceGAC() {
  int num_ConEvt = compress_Main();  // 1. CPU 上压缩活跃约束
  while (num_ConEvt != 0) {
    // 2. 启动 GPU kernel 检查约束
    CsCheckMain<<<num_ConEvt, dim3(kBitDomIntSize * 32, 1, 1)>>>(...);
    
    // 3. CPU-GPU 同步点（隐式）
    num_ConEvt = compress_Main();  // 4. 再次压缩
  }
  return true;
}
```

### 现有问题

| 问题 | 影响 | 严重性 |
|------|------|--------|
| **频繁 CPU-GPU 同步** | 每次迭代都要同步 | 🔴 严重 |
| **compress_Main 在 CPU** | Thrust 复制数据到 CPU | 🔴 严重 |
| **kernel 启动开销** | 每次迭代 ~5-10 µs | 🟡 中等 |
| **未使用 shared memory** | 重复读取 bitDom | 🟡 中等 |
| **原子操作冲突** | atomicAnd 串行化 | 🟡 中等 |
| **没有 persistent kernel** | 频繁启动 kernel | 🟢 轻微 |

---

## 优化方案 1：减少 CPU-GPU 同步（P0）

### 问题根源

```cuda
// 当前实现：CPU 控制循环
while (num_ConEvt != 0) {
  CsCheckMain<<<...>>>();       // GPU 执行
  num_ConEvt = compress_Main(); // ← CPU 同步点！
}
```

**每次迭代的开销**：
- CPU-GPU 同步: ~10 µs
- Thrust copy: ~5 µs
- Kernel 启动: ~5 µs
- **总计**: ~20 µs/迭代

### 解决方案：GPU 端迭代控制

```cuda
// 优化后：GPU 控制循环
__global__ void PersistentGACKernel(
    int* d_num_active,        // 活跃约束数（设备端）
    int* mConPre,             // 约束前置标记
    const uint3* mCon,        // 约束列表
    uint32_t* bitDom,         // 域位集
    cudaTextureObject_t bitSup,
    int max_iterations
) {
  __shared__ int s_active_count;
  
  // 外层循环：在 GPU 上迭代
  for (int iter = 0; iter < max_iterations; ++iter) {
    // 1. 块内压缩活跃约束（GPU 端）
    if (threadIdx.x == 0) {
      s_active_count = 0;
    }
    __syncthreads();
    
    // 2. 检查约束
    int c_id = blockIdx.x;
    if (c_id < gridDim.x && mConPre[c_id] == 1) {
      // 约束检查逻辑...
      uint2 support = tex3D<uint2>(bitSup, ...);
      // ...
      
      // 如果有删除，标记约束为活跃
      if (has_deletion) {
        atomicAdd(&s_active_count, 1);
      }
    }
    __syncthreads();
    
    // 3. 检查收敛（GPU 端）
    if (s_active_count == 0) {
      if (threadIdx.x == 0 && blockIdx.x == 0) {
        *d_num_active = 0;  // 通知 CPU 收敛
      }
      break;  // GPU 端退出
    }
  }
}

// CPU 端简化为单次启动
bool CModel::enforceGAC() {
  int* d_num_active;
  cudaMallocManaged(&d_num_active, sizeof(int));
  *d_num_active = 1;  // 初始标记为活跃
  
  // 单次启动，GPU 内部迭代
  PersistentGACKernel<<<num_constraints, 256>>>(..., d_num_active, ...);
  
  cudaDeviceSynchronize();  // 仅最后同步一次
  
  return *d_num_active == 0;
}
```

**预期性能提升**: 5-10x（queens-12: 50 µs → 5-10 µs）

---

## 优化方案 2：动态 CPU/GPU 调度（P1）

### 核心思想：根据问题规模自适应选择执行设备

```cpp
// 调度决策
enum ExecutionDevice { CPU, GPU, HYBRID };

ExecutionDevice ChooseDevice(int num_constraints, int avg_dom_size) {
  if (num_constraints < 10 && avg_dom_size < 8) {
    return CPU;  // 小问题，CPU 更快（避免启动开销）
  } else if (num_constraints < 50 && avg_dom_size < 16) {
    return HYBRID;  // 中等问题，CPU/GPU 混合
  } else {
    return GPU;  // 大问题，GPU 并行优势明显
  }
}
```

### 实现：混合执行策略

```cuda
bool CModel::enforceGAC_Adaptive() {
  auto device = ChooseDevice(kNumTabs, kMaxDomSize);
  
  switch (device) {
    case CPU:
      return enforceGAC_CPU();  // 纯 CPU 实现
    case GPU:
      return enforceGAC_GPU();  // 优化的 GPU 实现
    case HYBRID:
      return enforceGAC_Hybrid();  // CPU/GPU 协同
  }
}

// 纯 CPU 实现（小问题）
bool CModel::enforceGAC_CPU() {
  // 直接在 CPU 上操作统一内存
  std::vector<int> active_constraints;
  
  while (!active_constraints.empty()) {
    for (int c_id : active_constraints) {
      uint3 con = h_MCon[c_id];  // CPU 读取
      int x = con.x, y = con.y;
      
      // CPU 位运算
      uint32_t dom_x = h_bitDom[x * kBitDomIntSize + current_level_];
      uint32_t dom_y = h_bitDom[y * kBitDomIntSize + current_level_];
      
      // 检查支持（从统一内存读取）
      // ...
    }
    
    // 重新收集活跃约束
    active_constraints = CollectActiveConstraints();
  }
  return true;
}

// 混合实现（中等问题）
bool CModel::enforceGAC_Hybrid() {
  int num_ConEvt = compress_Main();
  
  while (num_ConEvt != 0) {
    if (num_ConEvt < 20) {
      // 约束数少，CPU 处理
      for (int i = 0; i < num_ConEvt; ++i) {
        ProcessConstraintCPU(d_MConEvt[i]);
      }
    } else {
      // 约束数多，GPU 批处理
      CsCheckMain<<<num_ConEvt, ...>>>(...);
    }
    
    num_ConEvt = compress_Main();
  }
  return true;
}
```

**预期性能提升**: 
- queens-4: 2-3x（CPU 避免 kernel 启动）
- queens-12: 1.2-1.5x（混合减少同步）
- queens-50: 5-10x（GPU 并行优势）

---

## 优化方案 3：GPU 端约束压缩（P0）

### 当前瓶颈：CPU 端 Thrust 压缩

```cpp
// cuSAC.cu:98-107
int CModel::compress_Main() {
  d_MConEvt.resize(d_MCon.size());
  auto end = thrust::copy_if(
      d_MCon.begin(), d_MCon.end(),
      d_ConPre.begin(),
      d_MConEvt.begin(),
      is_one());  // ← CPU-GPU 同步！
  // ...
}
```

### 优化：GPU 端流压缩（Stream Compaction）

```cuda
// GPU 端实现（CUB 库）
#include <cub/cub.cuh>

__global__ void MarkActiveConstraints(
    int* mConPre,
    int* d_flags,     // 输出：约束是否活跃
    int num_constraints
) {
  int c_id = blockIdx.x * blockDim.x + threadIdx.x;
  if (c_id < num_constraints) {
    d_flags[c_id] = (mConPre[c_id] == 1) ? 1 : 0;
  }
}

int CModel::compress_Main_GPU() {
  // 1. 标记活跃约束
  MarkActiveConstraints<<<...>>>(
      thrust::raw_pointer_cast(d_ConPre.data()),
      d_flags,
      kNumTabs);
  
  // 2. GPU 端流压缩（CUB）
  void* d_temp_storage = nullptr;
  size_t temp_storage_bytes = 0;
  
  // 计算临时存储大小
  cub::DeviceSelect::Flagged(
      d_temp_storage, temp_storage_bytes,
      thrust::raw_pointer_cast(d_MCon.data()),
      d_flags,
      thrust::raw_pointer_cast(d_MConEvt.data()),
      d_num_selected,
      kNumTabs);
  
  // 分配临时存储
  cudaMalloc(&d_temp_storage, temp_storage_bytes);
  
  // 执行压缩
  cub::DeviceSelect::Flagged(
      d_temp_storage, temp_storage_bytes,
      thrust::raw_pointer_cast(d_MCon.data()),
      d_flags,
      thrust::raw_pointer_cast(d_MConEvt.data()),
      d_num_selected,
      kNumTabs);
  
  // 读取活跃约束数
  int num_active;
  cudaMemcpy(&num_active, d_num_selected, sizeof(int), 
             cudaMemcpyDeviceToHost);
  
  cudaFree(d_temp_storage);
  return num_active;
}
```

**预期性能提升**: 2-3x（消除 CPU 同步）

---

## 优化方案 4：Shared Memory 优化（P1）

### 当前问题：重复读取 bitDom

```cuda
// 当前 CsCheckMain（简化）
__global__ void CsCheckMain(..., uint32_t* bitDom, ...) {
  int c_id = blockIdx.x;
  int val = threadIdx.x;
  
  // 每个线程都读取相同的 bitDom
  uint32_t dom_x = bitDom[x * kBitDomIntSize];  // 重复读取！
  uint32_t dom_y = bitDom[y * kBitDomIntSize];  // 重复读取！
  
  // ...
}
```

### 优化：Shared Memory 缓存

```cuda
__global__ void CsCheckMainOptimized(
    int* mConPre,
    const uint3* mCon,
    uint32_t* bitDom,
    const int* dom_size,
    cudaTextureObject_t bitSup,
    int num_ConEvt,
    int level
) {
  __shared__ uint32_t s_bitDom_x[8];  // 缓存变量 x 的域（假设 ≤8 words）
  __shared__ uint32_t s_bitDom_y[8];  // 缓存变量 y 的域
  __shared__ uint32_t s_deletion_mask[8];  // 删除标记
  
  int c_id = blockIdx.x;
  if (c_id >= num_ConEvt) return;
  
  uint3 con = mCon[c_id];
  int x = con.x, y = con.y;
  
  // 1. 协作加载到 shared memory（一次性）
  int tid = threadIdx.x;
  int warp_id = tid / 32;
  
  if (warp_id == 0 && tid < kBitDomIntSize) {
    s_bitDom_x[tid] = bitDom[x * kBitDomIntSize + tid + level * kBitDomsIntSize];
    s_bitDom_y[tid] = bitDom[y * kBitDomIntSize + tid + level * kBitDomsIntSize];
    s_deletion_mask[tid] = 0;  // 初始化删除标记
  }
  __syncthreads();
  
  // 2. 检查支持（从 shared memory 读取）
  int val = tid;
  if (val < dom_size[x]) {
    int word_idx = val / 32;
    int bit_idx = val % 32;
    
    // 检查 val 是否在域中
    if ((s_bitDom_x[word_idx] >> bit_idx) & 1) {
      // 读取支持（纹理内存）
      uint2 support = tex3D<uint2>(bitSup, val, word_idx, c_id);
      
      // 检查是否有支持
      uint32_t has_support = s_bitDom_y[word_idx] & support.x;
      
      if (__popc(has_support) == 0) {
        // 标记删除（在 shared memory）
        atomicOr(&s_deletion_mask[word_idx], 1u << bit_idx);
      }
    }
  }
  __syncthreads();
  
  // 3. 写回删除结果（批量写入）
  if (warp_id == 0 && tid < kBitDomIntSize) {
    if (s_deletion_mask[tid] != 0) {
      atomicAnd(&bitDom[x * kBitDomIntSize + tid + level * kBitDomsIntSize],
                ~s_deletion_mask[tid]);
      
      // 标记约束为活跃
      if (tid == 0) {
        mConPre[c_id] = 1;
      }
    }
  }
}
```

**优势**：
- ✅ 减少全局内存读取：从 N 次 → 1 次
- ✅ 减少原子操作冲突：先在 shared memory 累积
- ✅ 批量写回：减少内存事务

**预期性能提升**: 1.5-2x

---

## 优化方案 5：统一内存的异步预取（P2）

### 当前问题：按需页面迁移

```
统一内存默认行为：
- CPU 访问时，页面在 CPU 内存
- GPU kernel 访问时，触发页面故障 → 迁移到 GPU
- 每次迭代都可能触发迁移（延迟！）
```

### 优化：显式预取

```cpp
bool CModel::enforceGAC_Prefetch() {
  // 1. 预取到 GPU（异步）
  cudaMemPrefetchAsync(d_bitDom, 
                       sizeof(u32) * kBitDomsIntSize,
                       0,  // GPU device 0
                       cudaStreamDefault);
  
  // 2. GPU 计算（与预取重叠）
  int num_ConEvt = compress_Main();
  
  while (num_ConEvt != 0) {
    CsCheckMain<<<...>>>(...);
    
    // 3. 异步检查结果（不阻塞）
    num_ConEvt = compress_Main();
  }
  
  // 4. 最后同步
  cudaDeviceSynchronize();
  
  // 5. 预取回 CPU（如果需要）
  if (need_cpu_access) {
    cudaMemPrefetchAsync(d_bitDom, 
                         sizeof(u32) * kBitDomsIntSize,
                         cudaCpuDeviceId,
                         cudaStreamDefault);
  }
  
  return true;
}
```

**预期性能提升**: 1.2-1.5x（Jetson 上可能更高）

---

## 优化方案 6：Warp-Level Primitives（P1）

### 当前问题：分支发散

```cuda
// 当前实现
if (__popc(has_support) == 0) {
  atomicAnd(&bitDom[...], ~(1u << val));  // ← 分支发散！
}
```

### 优化：Warp 投票

```cuda
__global__ void CsCheckMainWarpOptimized(...) {
  // ...
  
  // 检查支持
  uint32_t has_support = s_bitDom_y[word_idx] & support.x;
  int no_support = (__popc(has_support) == 0);
  
  // Warp-level 投票（避免分支）
  uint32_t warp_mask = __ballot_sync(0xFFFFFFFF, no_support);
  
  if (warp_mask != 0) {
    // 只有第一个线程执行原子操作
    int first_lane = __ffs(warp_mask) - 1;
    if (threadIdx.x % 32 == first_lane) {
      // 批量删除（一次原子操作）
      atomicAnd(&bitDom[x * kBitDomIntSize + word_idx],
                ~warp_mask);
    }
  }
}
```

**预期性能提升**: 1.3-1.8x（减少原子操作冲突）

---

## 实施优先级

### Phase 1: 快速优化（1 周）

1. ✅ **纹理内存**（已完成）
2. 🎯 **Shared Memory 优化**（方案 4）
   - 投入: 2-3 天
   - 收益: 1.5-2x
   - 风险: 低

3. 🎯 **Warp Primitives**（方案 6）
   - 投入: 1-2 天
   - 收益: 1.3-1.8x
   - 风险: 低

**累计加速**: 2-3.6x

---

### Phase 2: 架构优化（2-3 周）

4. 🚀 **Persistent Kernel**（方案 1）
   - 投入: 5-7 天
   - 收益: 5-10x
   - 风险: 中等

5. 🚀 **GPU 端压缩**（方案 3）
   - 投入: 3-5 天
   - 收益: 2-3x
   - 风险: 低

**累计加速**: 10-30x

---

### Phase 3: 自适应优化（1-2 周）

6. 🧠 **动态 CPU/GPU 调度**（方案 2）
   - 投入: 5-10 天
   - 收益: 问题依赖（小问题 2-3x，大问题持平）
   - 风险: 中等

7. ⚡ **异步预取**（方案 5）
   - 投入: 2-3 天
   - 收益: 1.2-1.5x
   - 风险: 低

**累计加速**: 12-45x（问题依赖）

---

## 性能预估（queens-12）

| 阶段 | 延迟 (ms) | 加速比 | 说明 |
|------|----------|--------|------|
| Baseline (当前) | 50 | 1.0x | 频繁 CPU-GPU 同步 |
| Phase 1 | 17 | 3.0x | Shared mem + warp |
| Phase 2 | 3.3 | 15x | Persistent kernel + GPU compress |
| Phase 3 | 2.5 | 20x | CPU/GPU 混合 + 预取 |

---

## 代码示例：完整 Persistent Kernel

```cuda
// 完整实现
__global__ void PersistentGACKernel(
    int* d_converged,         // 全局收敛标志
    int* mConPre,             // 约束前置标记
    const uint3* mCon,        // 约束列表
    uint32_t* bitDom,         // 域位集
    const int* dom_size,      // 域大小
    cudaTextureObject_t bitSup,
    int num_constraints,
    int level,
    int max_iterations
) {
  __shared__ uint32_t s_bitDom_x[8];
  __shared__ uint32_t s_bitDom_y[8];
  __shared__ uint32_t s_deletion_mask[8];
  __shared__ int s_has_deletion;
  
  int c_id = blockIdx.x;
  int tid = threadIdx.x;
  
  if (c_id >= num_constraints) return;
  
  uint3 con = mCon[c_id];
  int x = con.x, y = con.y;
  
  // GPU 内部迭代
  for (int iter = 0; iter < max_iterations; ++iter) {
    // 检查全局收敛
    if (*d_converged == 1) break;
    
    // 检查约束是否活跃
    if (mConPre[c_id] == 0) continue;
    
    // 重置标志
    if (tid == 0) {
      s_has_deletion = 0;
      mConPre[c_id] = 0;  // 假设不活跃
    }
    __syncthreads();
    
    // 加载域到 shared memory
    if (tid < kBitDomIntSize) {
      s_bitDom_x[tid] = bitDom[x * kBitDomIntSize + tid + level * kBitDomsIntSize];
      s_bitDom_y[tid] = bitDom[y * kBitDomIntSize + tid + level * kBitDomsIntSize];
      s_deletion_mask[tid] = 0;
    }
    __syncthreads();
    
    // 检查支持
    int val = tid;
    if (val < dom_size[x]) {
      int word_idx = val / 32;
      int bit_idx = val % 32;
      
      if ((s_bitDom_x[word_idx] >> bit_idx) & 1) {
        uint2 support = tex3D<uint2>(bitSup, val, word_idx, c_id);
        uint32_t has_support = s_bitDom_y[word_idx] & support.x;
        
        if (__popc(has_support) == 0) {
          atomicOr(&s_deletion_mask[word_idx], 1u << bit_idx);
          s_has_deletion = 1;
        }
      }
    }
    __syncthreads();
    
    // 写回删除
    if (tid < kBitDomIntSize && s_deletion_mask[tid] != 0) {
      atomicAnd(&bitDom[x * kBitDomIntSize + tid + level * kBitDomsIntSize],
                ~s_deletion_mask[tid]);
    }
    
    // 标记邻居约束为活跃
    if (s_has_deletion) {
      if (tid == 0) {
        mConPre[c_id] = 1;
        *d_converged = 0;  // 未收敛
      }
      // TODO: 标记邻居约束
    }
    __syncthreads();
  }
}

// CPU 端调用
bool CModel::enforceGAC_Persistent() {
  int* d_converged;
  cudaMallocManaged(&d_converged, sizeof(int));
  
  int max_iterations = 100;  // 安全限制
  
  for (int iter = 0; iter < max_iterations; ++iter) {
    *d_converged = 1;  // 假设收敛
    
    PersistentGACKernel<<<kNumTabs, 256>>>(
        d_converged,
        thrust::raw_pointer_cast(d_ConPre.data()),
        thrust::raw_pointer_cast(d_MCon.data()),
        d_bitDom,
        thrust::raw_pointer_cast(d_cur_dom_size.data()),
        texObj_BitSup,
        kNumTabs,
        current_level_,
        10  // 内部最大迭代
    );
    
    cudaDeviceSynchronize();
    
    if (*d_converged == 1) {
      break;  // 收敛
    }
  }
  
  cudaFree(d_converged);
  return true;
}
```

---

## 总结

### 关键优化点

1. **消除 CPU-GPU 同步** - 5-10x 加速
2. **Shared Memory 缓存** - 1.5-2x 加速
3. **GPU 端压缩** - 2-3x 加速
4. **Warp Primitives** - 1.3-1.8x 加速
5. **自适应调度** - 小问题 2-3x

### 推荐路线

```
Week 1: Shared Memory + Warp (3x 加速)
Week 2-3: Persistent Kernel (10x 加速)
Week 4-5: CPU/GPU 混合（进一步优化）
```

### 实验验证

```bash
# Baseline
./cpim --benchmark=queens-12 --iterations=1000

# Phase 1
./cpim --benchmark=queens-12 --opt=shared_warp --iterations=1000

# Phase 2
./cpim --benchmark=queens-12 --opt=persistent --iterations=1000

# Phase 3
./cpim --benchmark=queens-12 --opt=adaptive --iterations=1000
```

---

**文档版本**: v1.0  
**作者**: Claude (based on cuSAC.cu analysis)  
**日期**: 2025-10-21  
**状态**: 待实施
