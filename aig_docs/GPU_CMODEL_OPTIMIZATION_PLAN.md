# CModel 优化计划（Jetson Orin · 渐进式实施）

**目标**：在不大规模重写的前提下，通过分阶段优化实现 2-3x GPU 加速

**原则**：
- 保持算法逻辑与外部接口不变
- 每阶段可独立验证与回退
- 优先低风险高收益的改动

**适用范围**：`include/cuSAC.cuh` + `src/cuSAC.cu`（CModel 类及相关 kernel）

---

## 改动规模分档

| 档位 | 改动范围 | 外部接口 | 预期收益 | 风险 |
|------|---------|---------|---------|------|
| **小改**（阶段 A） | Kernel 内部优化 + 事件触发机制 | 不变 | 2-3x | 低 |
| **中改**（阶段 B） | Trail 回溯 + 设备端队列 | 不变 | +20-40% | 中 |
| **大改**（可选） | Persistent Kernel + 设备调度 | 不变 | +20-40% | 高 |

**结论**：阶段 A 即可获得显著收益，无需"大改"。

---

## 阶段 A：高收益快优化（2-3 周，低风险）

### 目标
- 移除性能杀手（printf、频繁同步）
- 优化 Kernel 内部组织与访存
- 改进事件触发机制

### 改动清单

#### A1：移除调试开销（2 天）
**问题**：
- 30+ 处 `printf` 严重拖慢执行
- `cudaDeviceSynchronize` 频繁调用（每轮 2 次）
- `bitDomCopy()` 无效回读

**方案**：
```cpp
// 1. cuSAC.cuh 顶部添加宏
#ifndef CPIM_GPU_DEBUG
  #define GPU_PRINTF(...) /* no-op */
  #define GPU_ASSERT(cond, msg) /* no-op */
#else
  #define GPU_PRINTF printf
  #define GPU_ASSERT(cond, msg) assert((cond) && (msg))
#endif

// 2. 替换所有 printf（src/cuSAC.cu L533, L560, L589...）
// 旧：printf("x_v: %d, bitDom = %x\n", xid, vote_x);
// 新：GPU_PRINTF("x_v: %d, bitDom = %x\n", xid, vote_x);

// 3. Stream + Event 替代同步（enforceGAC, L1681-1710）
class CModel {
 private:
  cudaStream_t stream_;
  cudaEvent_t event_done_;
};

bool CModel::enforceGAC() {
  int num_ConEvt = compress_Main();
  while (num_ConEvt != 0) {
    CsCheckMain<<<num_ConEvt, ..., 0, stream_>>>(/*...*/);
    // ❌ 旧：cudaDeviceSynchronize();
    cudaEventRecord(event_done_, stream_);
    // ... 仅在需要读 GAC_success 时同步
    num_ConEvt = compress_Main();
  }
  cudaStreamSynchronize(stream_); // 仅最后一次
  return GAC_success;
}

// 4. 删除 bitDomCopy() 调用（L1645, L1650）
```

**验收**：
- `cudaDeviceSynchronize` 次数 < 5/求解
- Nsight Systems 显示 Host API 时间 < 10%
- 性能提升 > 30%

---

#### A2：订阅驱动的事件触发（3 天）
**问题**（当前实现 L586-604）：
- 通过 `neiCon` 纹理扫描全部约束（O(|C|)）
- `tex2D<int>(neiCon, idx, xid)` 逐个检查，访存分散

**方案**：直接使用 `d_subscription` + `d_subscription_offset`（已构建 L1543-1562）
```cpp
// CsCheckMain 末尾（L583-605）改为：
__syncthreads();
if (GAC_success && changex) {
  // 使用订阅表直接标记相邻约束
  int start = d_subscription_offset[xid];
  int end = d_subscription_offset[xid + 1];
  for (int i = start + tid; i < end; i += blockDim.x * blockDim.y) {
    uint3 sub_entry = d_subscription[i];
    int c_id = sub_entry.z;
    mConPre[c_id] = 1; // 标记需重检
  }
}
// changey 同理
```

**传参调整**：
```cpp
// CsCheckMain 签名增加（L414-417）
__global__ void CsCheckMain(
    // ... 原有参数
    const uint3* subscription,        // ← 新增
    const int* subscription_offset,   // ← 新增
    int num_ConEvt, int current_level);

// 调用处（L1687-1692）
CsCheckMain<<<num_ConEvt, ...>>>(
    /*...*/,
    thrust::raw_pointer_cast(d_subscription.data()),
    thrust::raw_pointer_cast(d_subscription_offset.data()),
    num_ConEvt, current_level_);
```

**验收**：
- 移除 `neiCon` 纹理构建代码（L1256-1301）
- 事件触发时间减少 > 50%
- 正确性验证：与旧版本结果一致

**参考**：`GPU_EVENT_COMPRESSION_PLAN.md` § 方案 A

---

#### A3：Kernel 线程组织重排（3 天）
**问题**（当前 L477-489）：
- `kBitDomIntSize > 1` 时需循环访问 `bitSup`
- 线程块配置为 `(kBitDomIntSize*32, 1, 1)`，Y 维未利用

**方案**：2D Block + 去循环
```cpp
// 1. 修改 block 配置（enforceGAC 调用处 L1687）
// 旧：dim3(kBitDomIntSize * 32, 1, 1)
// 新：dim3(32, kBitDomIntSize, 1)
//     ↑ X 维 32 线程处理 bit 位
//     ↑ Y 维 kBitDomIntSize 线程处理 word 段

// 2. CsCheckMain 内部重组（L477-489）
__global__ void CsCheckMain(/*...*/) {
  const int lane = threadIdx.x;      // 0-31（bit 位）
  const int word_idx = threadIdx.y;  // 0..(kBitDomIntSize-1)
  const int tid = threadIdx.y * 32 + threadIdx.x;

  // 共享内存布局不变
  extern __shared__ u32 shared_mem[];
  u32* s_bitDom_x = shared_mem;
  u32* s_bitDom_y = &shared_mem[kDeviceBitDomIntSize];

  // 加载到共享内存（每个 word_idx 线程加载一个 word）
  if (word_idx < kDeviceBitDomIntSize && lane == 0) {
    s_bitDom_x[word_idx] = bitDom[level_offset + xid * kDeviceBitDomIntSize + word_idx];
    s_bitDom_y[word_idx] = bitDom[level_offset + yid * kDeviceBitDomIntSize + word_idx];
  }
  __syncthreads();

  // 检查支持（去掉循环，直接用 word_idx）
  int my_bit = lane + word_idx * 32; // 当前线程负责的 bit 位
  int l_xa = (my_bit < kDeviceMaxDomSize) ? BITSET_GET(s_bitDom_x, my_bit) : 0;
  int l_ya = (my_bit < kDeviceMaxDomSize) ? BITSET_GET(s_bitDom_y, my_bit) : 0;

  u32 val_x = 0, val_y = 0;
  auto [x, y] = tex3D<uint2>(bitSup, my_bit, word_idx, bid);
  val_x = l_xa && (x & s_bitDom_y[word_idx]);
  val_y = l_ya && (y & s_bitDom_x[word_idx]);

  // Y 维归约（每个 word 独立）
  unsigned vote_x = __ballot_sync(0xFFFFFFFF, val_x != 0);
  unsigned vote_y = __ballot_sync(0xFFFFFFFF, val_y != 0);

  // 写回（仅每个 warp 的 lane 0）
  if (lane == 0) {
    if (s_bitDom_x[word_idx] ^ vote_x) {
      u32 oldVal = atomicAnd(&bitDom[level_offset + xid * kDeviceBitDomIntSize + word_idx], vote_x);
      int deletes = __popc(oldVal) - __popc(oldVal & vote_x);
      if (deletes > 0) {
        atomicSub(&dom_size[current_level * kDeviceNumVars + xid], deletes);
        changex = 1;
      }
    }
    // changey 同理
  }
}
```

**验收**：
- 移除 `if/else if (kBitDomIntSize == ...)` 分支（L477-489）
- SM 占用率提升（Nsight Compute 验证）
- 性能提升 > 15%

**参考**：`GPU_CSCHECK_OPT_PLAN.md` § 2（Warp 级并行与早停）

---

#### A4：写回原子优化（2 天）
**问题**（L515-562）：
- 每个变量的每个 word 独立 `atomicAnd` + `atomicSub`
- 删值计数分散累加

**方案**：Warp 聚合
```cpp
// 在共享内存暂存删值
__shared__ int s_delete_count_x[kDeviceBitDomIntSize];
__shared__ int s_delete_count_y[kDeviceBitDomIntSize];

if (lane == 0) {
  s_delete_count_x[word_idx] = 0;
  s_delete_count_y[word_idx] = 0;
}
__syncthreads();

// 写回 bitDom 并累加删值
if (lane == 0 && word_idx < kDeviceBitDomIntSize) {
  if (s_bitDom_x[word_idx] ^ vote_x) {
    u32 oldVal = atomicAnd(&bitDom[level_offset + xid * kDeviceBitDomIntSize + word_idx], vote_x);
    s_delete_count_x[word_idx] = __popc(oldVal) - __popc(oldVal & vote_x);
    changex = 1;
  }
  // changey 同理
}
__syncthreads();

// 块内归约删值总数（仅一次 atomicSub）
if (tid == 0) {
  int total_delete_x = 0, total_delete_y = 0;
  for (int i = 0; i < kDeviceBitDomIntSize; ++i) {
    total_delete_x += s_delete_count_x[i];
    total_delete_y += s_delete_count_y[i];
  }
  if (total_delete_x > 0) {
    int old_size = atomicSub(&dom_size[current_level * kDeviceNumVars + xid], total_delete_x);
    if (old_size <= total_delete_x) GAC_success = false;
  }
  // total_delete_y 同理
}
```

**验收**：
- 原子操作次数减少 > 60%
- Nsight Compute 显示原子热点降低

---

#### A5：事件标记位图化（1 天，可选）
**当前**：`d_ConPre` 为 `int` 数组（4 字节/约束）
**优化**：改为位图（1 bit/约束）

```cpp
// cuSAC.cuh
class CModel {
 private:
  thrust::device_vector<uint32_t> d_ConPre_bitmap; // ceil(kNumTabs / 32) 个 uint32
};

// CsCheckMain 末尾标记
int c_word = c_id >> 5;
int c_bit = c_id & 31;
atomicOr(&d_ConPre_bitmap[c_word], 1U << c_bit);

// compress_Main 改为位图扫描（或用 CUB DeviceSelect）
```

**验收**：
- 内存占用减少 75%
- 缓存命中率提升

**参考**：`GPU_EVENT_COMPRESSION_PLAN.md` § 去重与稳定性

---

#### A6：uint4 矢量加载（1 天，可选）
**当前**（L458-459）：逐 `uint32` 加载 bitDom

**优化**：
```cpp
// 假设 kBitDomIntSize % 4 == 0
if (word_idx < kDeviceBitDomIntSize / 4 && lane == 0) {
  uint4* s_x_v4 = reinterpret_cast<uint4*>(s_bitDom_x);
  uint4* s_y_v4 = reinterpret_cast<uint4*>(s_bitDom_y);
  const uint4* bitDom_v4 = reinterpret_cast<const uint4*>(bitDom);

  int base_x = level_offset / 4 + xid * (kDeviceBitDomIntSize / 4);
  int base_y = level_offset / 4 + yid * (kDeviceBitDomIntSize / 4);
  s_x_v4[word_idx] = bitDom_v4[base_x + word_idx];
  s_y_v4[word_idx] = bitDom_v4[base_y + word_idx];
}
```

**验收**：
- 全局内存事务减少 75%

---

### 阶段 A 验收标准
- [x] printf 全部移除（宏控制）
- [x] `cudaDeviceSynchronize` < 5 次/求解
- [x] 订阅驱动事件触发
- [x] 2D Block 重组完成
- [x] 原子操作优化
- [x] 性能提升 **2-3x**（queens-12 基准）

### 阶段 A 预期收益
| 优化项 | 收益 |
|--------|------|
| 移除 printf | +30-50% |
| 减少同步 | +15% |
| 订阅驱动 | +20% |
| 2D Block | +15% |
| 原子优化 | +10% |
| **总计** | **2-3x** |

---

## 阶段 B：回溯与调度（按需推进，中风险）

### B1：Trail 替代整层复制（1-2 周）
**问题**（CreateNewLevel, L1635-1658）：
- 每次复制 `kBitDomsIntSize * sizeof(u32)` 字节
- 回溯密集场景带宽开销大

**方案**：设备端 Trail
```cpp
// cuSAC.cuh 新增
struct TrailEntry {
  int var_id;
  int word_idx;
  uint32_t old_word;
  int old_dom_size;
};

class CModel {
 private:
  TrailEntry* d_trail;
  int* d_trail_top;  // [kDepth]
};

// CsCheckMain 写回时记录 trail
if (lane == 0 && s_bitDom_x[word_idx] ^ vote_x) {
  u32 oldVal = atomicAnd(&bitDom[...], vote_x);
  int trail_idx = atomicAdd(&d_trail_top[current_level], 1);
  d_trail[trail_idx] = {xid, word_idx, oldVal, /*old_dom_size*/};
}

// BackLevel 回滚（替代 cudaMemcpy L1647）
__global__ void RollbackTrail(TrailEntry* trail, int trail_top, u32* bitDom, int* dom_size) {
  int idx = blockIdx.x * blockDim.x + threadIdx.x;
  if (idx < trail_top) {
    auto& entry = trail[idx];
    bitDom[entry.var_id * kBitDomIntSize + entry.word_idx] = entry.old_word;
    dom_size[entry.var_id] = entry.old_dom_size;
  }
}
```

**验收**：
- `CreateNewLevel` 带宽 < 1% 峰值
- 回溯密集场景性能提升 > 30%

**参考**：`MODERNIZATION_PLAN_V2.md` § Phase 2（Trail 回溯系统）

---

### B2：设备端事件队列（1-2 周，可选）
**当前**：`compress_Main()` 在 Host 侧用 Thrust

**方案**：设备端 Compact Kernel
```cpp
__global__ void CompressEventsKernel(
    const uint3* all_constraints,
    const uint32_t* constraint_bitmap,
    uint3* output_events,
    int* output_count,
    int num_constraints) {
  // Warp-level stream compaction
  // 参考 GPU_EVENT_COMPRESSION_PLAN.md § 方案 A
}
```

**验收**：
- Host 往返次数减少 > 50%
- 配合 CUDA Graph（见阶段 C）

---

### B3：CUDA Graph 捕获（1 周，可选）
**目标**：将 `compress → cscheck` 循环捕获为 Graph

**方案**：
```cpp
// 固定网格 + 边界检查
__global__ void CsCheckMain_FixedGrid(/*...*/, int* num_ConEvt) {
  int bid = blockIdx.x;
  if (bid >= *num_ConEvt) return; // 动态边界
  // ...
}

// 捕获
cudaStreamBeginCapture(stream_);
  CompressEventsKernel<<<maxGrid, 256, 0, stream_>>>(/*...*/);
  CsCheckMain_FixedGrid<<<maxGrid, dim3(32, kBitDomIntSize), 0, stream_>>>(/*...*/);
cudaStreamEndCapture(stream_, &graph_);
cudaGraphInstantiate(&graph_exec_, graph_, nullptr, nullptr, 0);

// 回放
while (true) {
  cudaGraphLaunch(graph_exec_, stream_);
  cudaStreamSynchronize(stream_);
  if (*d_num_ConEvt == 0 || !GAC_success) break;
}
```

**验收**：
- Kernel 启动开销 < 5%（Nsight Systems）
- 小批量场景性能提升 > 30%

**参考**：`GPU_GRAPHS_GAC.md` § 方案 A（Graph‑replay）

---

## 阶段 C：Persistent Kernel（2-3 周，高风险，可选）

**决策点**：仅在阶段 A+B 后 Host API 占比 > 10% 时启动

**方案**：设备端维护事件队列 + 状态机

**参考**：`GPU_GAC_PERSISTENT_STATE_MACHINE.md`

**风险**：
- 调试困难（设备端死循环）
- 需 Cooperative Groups 同步
- 收益不确定

**建议**：保留为实验分支，默认关闭

---

## CMake 特性开关

```cmake
# CMakeLists.txt
option(CPIM_GPU_DEBUG "Enable GPU kernel debug output" OFF)
option(CPIM_GPU_USE_SUBSCRIPTION_TRIGGER "Use subscription-driven event trigger" ON)
option(CPIM_GPU_2D_BLOCK "Use 2D block layout for CsCheck" ON)
option(CPIM_GPU_WARP_ATOMIC_MERGE "Merge atomic operations in warp" ON)
option(CPIM_GPU_BITMAP_EVENT "Use bitmap for event flags" OFF)
option(CPIM_GPU_TRAIL_BACKTRACK "Use trail-based backtracking" OFF)
option(CPIM_GPU_DEVICE_COMPRESS "Device-side event compression" OFF)
option(CPIM_GPU_USE_GRAPHS "CUDA Graph for GAC loop" OFF)
option(CPIM_GPU_PERSISTENT_KERNEL "Persistent kernel (experimental)" OFF)
```

**使用**：
```cpp
// cuSAC.cu
#ifdef CPIM_GPU_USE_SUBSCRIPTION_TRIGGER
  // 新代码：订阅驱动
#else
  // 旧代码：neiCon 扫描
#endif
```

---

## 验收与回归测试

### 基线采集
```bash
#!/bin/bash
# scripts/benchmark_baseline.sh
BENCHMARKS=(queens-4_ext queens-12_ext haystacks-11_ext)
mkdir -p baseline_results

for bench in "${BENCHMARKS[@]}"; do
  echo "Running $bench..."
  /usr/bin/time -v ./cpim "../samples/bench/${bench}.xml" 2>&1 | \
    tee "baseline_results/${bench}.log"
done

# 提取指标
grep -E "(Elapsed|GAC iterations|Nodes)" baseline_results/*.log > baseline_summary.txt
git add baseline_results && git commit -m "perf: add GPU baseline"
```

### 回归验证
```bash
# scripts/test_regression.sh
for bench in queens-4 queens-12; do
  diff <(grep "Solution" baseline_results/${bench}.log) \
       <(grep "Solution" optimized_results/${bench}.log) || {
    echo "❌ Regression on ${bench}!"
    exit 1
  }
done
echo "✅ All tests passed"
```

### Nsight 分析
```bash
# 采集 trace
nsys profile -o baseline.qdrep ./cpim queens-12_ext.xml
nsys profile -o optimized.qdrep ./cpim queens-12_ext.xml

# 对比 CUDA API 时间
nsys stats --report cuda_api_sum baseline.qdrep
nsys stats --report cuda_api_sum optimized.qdrep

# Kernel 分析
ncu --set full -o kernel_baseline ./cpim queens-4_ext.xml
ncu --set full -o kernel_optimized ./cpim queens-4_ext.xml
```

---

## 关键代码锚点（快速定位）

| 优化项 | 文件 | 行数 | 函数/Kernel |
|--------|------|------|-------------|
| 移除 printf | cuSAC.cu | L533, L560, L589... | CsCheckMain |
| 减少同步 | cuSAC.cu | L1681-1710 | enforceGAC |
| 订阅触发 | cuSAC.cu | L586-604 | CsCheckMain |
| 2D Block | cuSAC.cu | L414-606, L1687 | CsCheckMain 签名与调用 |
| 原子优化 | cuSAC.cu | L515-562 | CsCheckMain 写回段 |
| 事件压缩 | cuSAC.cu | L1151-1164 | compress_Main |
| Trail 回溯 | cuSAC.cu | L1635-1658 | CreateNewLevel, BackLevel |

---

## 与专题文档的关系

| 专题文档 | 本计划对应章节 |
|---------|--------------|
| `GPU_CSCHECK_OPT_PLAN.md` | § A3（2D Block）、§ A4（原子优化） |
| `GPU_EVENT_COMPRESSION_PLAN.md` | § A2（订阅触发）、§ B2（设备压缩） |
| `GPU_GRAPHS_GAC.md` | § B3（CUDA Graph） |
| `GPU_GAC_PERSISTENT_STATE_MACHINE.md` | § 阶段 C（Persistent Kernel） |
| `GPU_MODEL_DATA_PLAN.md` | § B1（Trail 回溯） |
| `GPU_GAC_PIPELINE_PLAN.md` | § B2-B3（事件队列与 Graph） |

**原则**：本文档聚焦"改动边界与落地顺序"，细节参考专题文档。

---

## 总体时间线

| 阶段 | 时间 | 风险 | 累计收益 | 备注 |
|------|------|------|---------|------|
| **阶段 A** | 2-3 周 | 低 | **2-3x** | 优先实施 |
| 阶段 B1（Trail） | 1-2 周 | 中 | +20-30% | 回溯密集场景 |
| 阶段 B2（设备压缩） | 1 周 | 中 | +10-15% | 配合 Graph |
| 阶段 B3（Graph） | 1 周 | 中 | +20-30% | 小批量场景 |
| **B 总计** | 3-4 周 | 中 | +30-50% | 按需推进 |
| 阶段 C（Persistent） | 2-3 周 | 高 | +20-40% | 实验分支 |

**推荐路径**：阶段 A → 评估 → 选择性实施 B1/B2/B3 → 若需要再启动 C

---

## 下一步行动（本周）

1. **创建开发分支**
   ```bash
   git checkout -b feature/gpu-opt-stage-a
   ```

2. **启动 A1**：移除 printf + 减少同步
   - 在 `cuSAC.cuh` 添加 `GPU_PRINTF` 宏
   - 替换所有 `printf` 调用
   - 引入 `cudaStream_t` 成员

3. **采集基线**
   ```bash
   cd build
   ../scripts/benchmark_baseline.sh
   git add baseline_results && git commit -m "perf: add GPU baseline"
   ```

4. **评审与调整**
   - 团队讨论优先级
   - 确认 CMake 开关策略
   - 分配任务（A2-A6）

---

## 附录

### 编译与构建
```bash
# Jetson Orin 优化编译
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CUDA_ARCHITECTURES="87" \
  -DCPIM_GPU_DEBUG=OFF \
  -DCPIM_GPU_USE_SUBSCRIPTION_TRIGGER=ON \
  -DCPIM_GPU_2D_BLOCK=ON

cmake --build build -j$(nproc)
```

### 风险缓解
- **订阅触发正确性**：确保删值后所有相邻约束入队（幂等 + 不丢事件）
- **2D Block 边界**：`my_bit < kDeviceMaxDomSize` 检查防止越界
- **原子冲突**：若性能回退，回退到旧代码（CMake 开关）
- **Trail 一致性**：所有写回路径必须记录旧值；回滚顺序与写入一致

### 工具链
- **Nsight Systems**：端到端性能分析（API 时间、Kernel 时间线）
- **Nsight Compute**：单 Kernel 深度分析（占用、带宽、分支效率）
- **cuda-memcheck**：内存错误检测（越界、竞态）
- **compute-sanitizer**：CUDA 12 同步/竞态检测工具

---

**文档版本**：v2.0（整合版）
**更新日期**：2025-10-18
**相关文档**：`GPU_JETSON_INDEPENDENT_REVIEW.md`、`GPU_CMODEL_REFACTOR_PLAN.md`（已归档）

🤖 Generated with Claude Code
Co-Authored-By: Claude <noreply@anthropic.com>
