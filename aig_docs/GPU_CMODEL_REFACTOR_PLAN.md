# CModel 渐进式重构计划（基于现有代码）

**目标**：在不大规模重写的前提下，通过渐进式重构实现 Jetson 适配优化方案（M0-M5）

**原则**：
- 保持现有算法逻辑不变
- 每个里程碑可独立验证与回退
- 优先低风险高收益的改动

---

## M0：基础清理与性能基线（1 周，低风险）

### 目标
- 移除性能杀手（printf、同步）
- 建立量化基准
- 清理代码债务

### 具体任务

#### Task 0.1：移除 Kernel Printf（2 天）
**问题**：当前代码中有 30+ 处 `printf`，严重拖慢 GPU 执行。

**方案**：
```cpp
// 1. 定义宏开关（cuSAC.cuh 顶部）
#ifndef CPIM_GPU_DEBUG
  #define GPU_PRINTF(...) /* no-op */
#else
  #define GPU_PRINTF(...) printf(__VA_ARGS__)
#endif

// 2. 替换所有 printf
// 旧代码（L533）：
printf("x_v: %d, bitDom = %x, ...", xid, vote_x, ...);
// 新代码：
GPU_PRINTF("x_v: %d, bitDom = %x, ...", xid, vote_x, ...);
```

**验收**：
- 默认编译无 printf 输出
- CMake 选项 `-DCPIM_GPU_DEBUG=ON` 可启用调试
- 性能提升 > 50%（queens-4 基准）

---

#### Task 0.2：减少 cudaDeviceSynchronize（3 天）
**问题**：`enforceGAC()` 中每轮迭代都有 2 次同步（L1684, L1693）。

**方案 A**：使用 CUDA Stream + Event
```cpp
// cuSAC.cuh 中添加
class CModel {
 private:
  cudaStream_t stream_;
  cudaEvent_t event_compress_done_;
  cudaEvent_t event_cscheck_done_;
};

// cuSAC.cu 构造函数中初始化
CModel::CModel(const HModel& xm) : ... {
  cudaStreamCreate(&stream_);
  cudaEventCreate(&event_compress_done_);
  cudaEventCreate(&event_cscheck_done_);
}

// enforceGAC() 改造
bool CModel::enforceGAC() {
  int num_ConEvt = compress_Main();
  // ❌ 旧代码：cudaDeviceSynchronize();

  while (num_ConEvt != 0) {
    CsCheckMain<<<num_ConEvt, ..., 0, stream_>>>(...);
    cudaEventRecord(event_cscheck_done_, stream_);

    if (!GAC_success) { // 需要同步读取
      cudaStreamSynchronize(stream_);
      return false;
    }

    num_ConEvt = compress_Main();
  }

  cudaStreamSynchronize(stream_); // 仅最后同步一次
  return true;
}
```

**方案 B**：UM 异步读取（更激进）
```cpp
// 使用 __managed__ 变量异步更新
extern __managed__ int d_num_ConEvt;

bool CModel::enforceGAC() {
  compress_Main_device<<<...>>>(&d_num_ConEvt); // 设备端更新计数

  while (true) {
    cudaStreamSynchronize(stream_); // 等待compress完成
    if (d_num_ConEvt == 0) break;

    CsCheckMain<<<d_num_ConEvt, ..., 0, stream_>>>(...);
    compress_Main_device<<<...>>>(&d_num_ConEvt);
  }
  return true;
}
```

**建议**：先实施方案 A（稳妥），M2 阶段再尝试方案 B。

**验收**：
- `cudaDeviceSynchronize` 次数减少 > 80%
- Nsight Systems 显示 Host API 时间占比 < 10%

---

#### Task 0.3：设备端统计替代 Printf（2 天）
**方案**：
```cpp
// cuSAC.cuh 新增
struct DeviceStatistics {
  uint64_t gac_iterations;
  uint64_t total_deletions;
  uint32_t max_queue_length;
};
extern __managed__ DeviceStatistics d_stats;

// CsCheckMain 中统计（L526）
if (delete_num_values > 0) {
  atomicAdd(&d_stats.total_deletions, delete_num_values);
}

// Host 侧读取
void CModel::PrintStatistics() {
  cudaDeviceSynchronize();
  LOG(INFO) << "GAC iterations: " << d_stats.gac_iterations;
  LOG(INFO) << "Total deletions: " << d_stats.total_deletions;
}
```

---

#### Task 0.4：性能基线采集（1 天）
**目标**：记录当前版本的性能数据，供后续对比。

```bash
# 创建基准脚本
cat > scripts/benchmark_baseline.sh << 'EOF'
#!/bin/bash
set -e

BENCHMARKS=(
  "../samples/bench/queens-4_ext.xml"
  "../samples/bench/queens-12_ext.xml"
  "../samples/bench/haystacks-11_ext.xml"
)

mkdir -p baseline_results
for bench in "${BENCHMARKS[@]}"; do
  echo "Running $bench..."
  /usr/bin/time -v ./cpim "$bench" 2>&1 | tee "baseline_results/$(basename $bench .xml).log"
done

# 提取关键指标
grep -E "(Elapsed time|GAC iterations)" baseline_results/*.log > baseline_summary.txt
EOF

chmod +x scripts/benchmark_baseline.sh
cd build && ../scripts/benchmark_baseline.sh
```

**产出**：
- `baseline_results/` 目录存放原始日志
- `baseline_summary.txt` 汇总数据
- 记录到 Git：`git add baseline_results && git commit -m "feat: add M0 baseline"`

---

#### Task 0.5：代码清理（1 天）
**清理内容**：
1. 删除注释掉的代码（L990-1144, L1782-1800等）
2. 整理 `#pragma region` 折叠（部分编辑器不支持）
3. 添加 `cudaError_t` 检查宏：
```cpp
// cuSAC.cuh
#define CUDA_CHECK(call) \
  do { \
    cudaError_t err = call; \
    if (err != cudaSuccess) { \
      LOG(FATAL) << "CUDA error at " << __FILE__ << ":" << __LINE__ \
                 << " - " << cudaGetErrorString(err); \
    } \
  } while(0)

// 使用示例（L1257）
CUDA_CHECK(cudaMalloc(&d_ConNeighbor, sizeof(int) * kNumTabs * kNumVars));
```

---

### M0 验收标准
- [x] printf 全部移除（或宏控制）
- [x] cudaDeviceSynchronize 次数 < 5/求解
- [x] 基线数据采集完成
- [x] 所有 CUDA API 有错误检查
- [x] 注释代码清理 > 500 行

### M0 预期收益
- 性能提升：30-50%（主要来自去 printf）
- 代码行数减少：~600 行
- 调试效率提升：统计数据 + 宏开关

---

## M1：IntermediateModel 适配器（1.5 周，低风险）

### 目标
- 解耦 HModel 依赖
- 为后续优化提供干净的数据源

### 具体任务

#### Task 1.1：设计适配器接口（2 天）
```cpp
// include/model/cmodel_adapter.h
#pragma once
#include "model/intermediate_model.h"
#include "absl/status/statusor.h"
#include <vector>
#include <cstdint>

namespace cpim::model {

struct BitDomainLayout {
  int num_vars;
  int max_dom_size;
  int bit_dom_int_size;        // 每个变量域的 uint32 数量
  int bit_doms_int_size;       // 所有变量域的 uint32 总数
  std::vector<uint32_t> data;  // 初始域位图
};

struct BitSupportLayout {
  int num_constraints;
  int bit_sup_int_size;        // 每个约束支持的 uint32 数量
  std::vector<uint32_t> data;  // 支持位图（3D 展平）
};

struct SubscriptionCSR {
  std::vector<int> offsets;    // [num_vars + 1]
  std::vector<uint3> entries;  // [(c.x, c.y, c.id)]
};

class CModelAdapter {
 public:
  // 从 IntermediateModel 构建
  static absl::StatusOr<CModelAdapter> FromIntermediate(
      const IntermediateModel& model);

  // 从旧 HModel 构建（过渡期）
  static absl::StatusOr<CModelAdapter> FromHModel(const HModel& hmodel);

  // 访问器
  const BitDomainLayout& Domains() const { return domains_; }
  const BitSupportLayout& Supports() const { return supports_; }
  const SubscriptionCSR& Subscriptions() const { return subscriptions_; }
  const std::vector<uint3>& Constraints() const { return constraints_; }
  const std::vector<int>& DomainSizes() const { return domain_sizes_; }
  const std::vector<int>& Degrees() const { return degrees_; }

 private:
  BitDomainLayout domains_;
  BitSupportLayout supports_;
  SubscriptionCSR subscriptions_;
  std::vector<uint3> constraints_;  // [(v1_id, v2_id, c_id)]
  std::vector<int> domain_sizes_;
  std::vector<int> degrees_;
};

}  // namespace cpim::model
```

---

#### Task 1.2：实现 FromHModel（3 天）
**目标**：将现有 `BuildBitModel(HModel)` 逻辑迁移到适配器。

```cpp
// src/model/cmodel_adapter.cpp
#include "model/cmodel_adapter.h"
#include "xcsp3model/HModel.h"

namespace cpim::model {

absl::StatusOr<CModelAdapter> CModelAdapter::FromHModel(const HModel& xm) {
  CModelAdapter adapter;

  // 1. 构建域布局（复用现有逻辑）
  adapter.domains_.num_vars = xm->Vars().size();
  adapter.domains_.max_dom_size = xm->max_domain_size();
  adapter.domains_.bit_dom_int_size = intsizeof(adapter.domains_.max_dom_size);
  adapter.domains_.bit_doms_int_size =
      adapter.domains_.bit_dom_int_size * adapter.domains_.num_vars;

  // 2. 初始化域位图
  adapter.domains_.data.resize(adapter.domains_.bit_doms_int_size, 0);
  for (int v = 0; v < adapter.domains_.num_vars; ++v) {
    const auto& var = xm->Vars(v);
    for (int a : var->vals) {
      int word_idx = v * adapter.domains_.bit_dom_int_size + (a >> 5);
      int bit_offset = a & 31;
      adapter.domains_.data[word_idx] |= (1U << bit_offset);
    }
  }

  // 3. 构建支持位图（bitSup）
  // ... 复用 BuildBitModel 中的 bitSup 构建逻辑

  // 4. 构建订阅 CSR
  adapter.subscriptions_.offsets.resize(adapter.domains_.num_vars + 1, 0);
  int offset = 0;
  for (int v = 0; v < adapter.domains_.num_vars; ++v) {
    adapter.subscriptions_.offsets[v] = offset;
    const auto& var = xm->Vars(v);
    for (const auto& c : xm->subscriptions[var]) {
      adapter.subscriptions_.entries.push_back(
          make_uint3(c->scope[0]->id, c->scope[1]->id, c->id));
      offset++;
    }
  }
  adapter.subscriptions_.offsets[adapter.domains_.num_vars] = offset;

  // 5. 提取约束、域大小、度
  // ...

  return adapter;
}

}  // namespace cpim::model
```

---

#### Task 1.3：改造 CModel 构造函数（2 天）
```cpp
// cuSAC.cuh 新增
class CModel {
 public:
  // 新构造函数（推荐）
  explicit CModel(const cpim::model::CModelAdapter& adapter);

  // 旧构造函数（过渡期保留）
  explicit CModel(const HModel& xm);

 private:
  void BuildFromAdapter(const cpim::model::CModelAdapter& adapter);
};

// cuSAC.cu 实现
CModel::CModel(const cpim::model::CModelAdapter& adapter)
    : kNumVars(adapter.Domains().num_vars),
      kNumTabs(adapter.Constraints().size()),
      kMaxDomSize(adapter.Domains().max_dom_size),
      ... {
  BuildFromAdapter(adapter);
}

void CModel::BuildFromAdapter(const cpim::model::CModelAdapter& adapter) {
  // 1. 直接使用适配器数据，无需重新计算
  const auto& domains = adapter.Domains();

  cudaMallocManaged(&d_bitDom, domains.data.size() * kDepth * sizeof(uint32_t));
  memcpy(d_bitDom, domains.data.data(), domains.data.size() * sizeof(uint32_t));

  // 2. 复制订阅结构
  h_subscription_offset = adapter.Subscriptions().offsets;
  h_subscription.assign(adapter.Subscriptions().entries.begin(),
                        adapter.Subscriptions().entries.end());
  d_subscription = h_subscription;
  d_subscription_offset = h_subscription_offset;

  // 3. 其他初始化...
}

// 旧构造函数兼容
CModel::CModel(const HModel& xm)
    : CModel(*cpim::model::CModelAdapter::FromHModel(xm)) {
  // 委托给新构造函数
}
```

---

#### Task 1.4：集成测试（2 天）
**验证**：
1. 新旧构造函数结果一致
2. 与 CPU 版本（MAC + AC3bit）结果对比
3. 性能无回归

```cpp
// test/gpu/cmodel_adapter_test.cu
TEST(CModelAdapterTest, FromHModelEquivalence) {
  HModel hm = LoadTestModel("queens-4_ext.xml");

  // 旧路径
  CModel old_model(hm);

  // 新路径
  auto adapter = CModelAdapter::FromHModel(hm);
  ASSERT_TRUE(adapter.ok());
  CModel new_model(*adapter);

  // 验证域一致
  EXPECT_EQ(old_model.GetDomain(0), new_model.GetDomain(0));

  // 验证求解结果一致
  old_model.solve(1000);
  new_model.solve(1000);
  EXPECT_EQ(old_model.statistics_.nodes, new_model.statistics_.nodes);
}
```

---

### M1 验收标准
- [x] `CModelAdapter` 接口完整
- [x] `FromHModel` 实现正确
- [x] 新旧路径求解结果一致
- [x] 性能无回归（±5%）
- [x] 单元测试覆盖 > 80%

### M1 预期收益
- 代码解耦：GPU 侧不再依赖 HModel
- 为 IntermediateModel 铺路
- 数据预处理前置，减少 GPU 构建时间

---

## M2：设备端事件压缩（1 周，中风险）

### 目标
- 将 `compress_Main()` 从 Host 搬到 Device
- 消除 Thrust `copy_if` 的 Host 往返

### 具体任务

#### Task 2.1：设备端压缩 Kernel（3 天）
**当前瓶颈**（L1151-1164）：
```cpp
// Host 侧 Thrust 操作，有隐式同步
int CModel::compress_Main() {
  auto end = thrust::copy_if(d_MCon.begin(), d_MCon.end(),
                             d_ConPre.begin(),
                             d_MConEvt.begin(),
                             is_one());
  thrust::fill(d_ConPre.begin(), d_ConPre.end(), 0);
  d_MConEvt.resize(thrust::distance(d_MConEvt.begin(), end));
  return d_MConEvt.size();
}
```

**新方案**：设备端 Compact
```cpp
// cuSAC.cu 新增
__global__ void CompressEventsKernel(
    const uint3* all_constraints,
    const int* constraint_flags,
    uint3* output_events,
    int* output_count,
    int num_constraints) {

  extern __shared__ int s_local_count[];
  int tid = threadIdx.x;
  int lane = tid & 31;
  int warp_id = tid >> 5;

  // 每个 warp 处理一段约束
  int base = (blockIdx.x * blockDim.x + threadIdx.x) & ~31;
  int local_count = 0;
  uint3 local_buffer[32];

  for (int i = base; i < num_constraints; i += gridDim.x * blockDim.x) {
    int idx = base + lane;
    int flag = (idx < num_constraints) ? constraint_flags[idx] : 0;

    // Warp 内投票与压缩
    unsigned mask = __ballot_sync(0xFFFFFFFF, flag == 1);
    int rank = __popc(mask & ((1U << lane) - 1));
    int warp_total = __popc(mask);

    if (flag == 1) {
      local_buffer[local_count + rank] = all_constraints[idx];
    }
    local_count += warp_total;

    // 缓冲满则写回
    if (local_count >= 32) {
      int base_out = atomicAdd(output_count, local_count);
      for (int j = lane; j < local_count; j += 32) {
        output_events[base_out + j] = local_buffer[j];
      }
      local_count = 0;
    }
  }

  // 剩余元素写回
  if (local_count > 0 && lane == 0) {
    int base_out = atomicAdd(output_count, local_count);
    for (int j = 0; j < local_count; ++j) {
      output_events[base_out + j] = local_buffer[j];
    }
  }
}

// Host 侧调用
int CModel::compress_Main_device() {
  int h_count = 0;
  cudaMemcpyAsync(&h_count, d_num_events, sizeof(int), cudaMemcpyDeviceToHost, stream_);
  cudaMemsetAsync(d_num_events, 0, sizeof(int), stream_);

  int threads = 256;
  int blocks = (kNumTabs + threads - 1) / threads;
  CompressEventsKernel<<<blocks, threads, 0, stream_>>>(
      thrust::raw_pointer_cast(d_MCon.data()),
      thrust::raw_pointer_cast(d_ConPre.data()),
      thrust::raw_pointer_cast(d_MConEvt.data()),
      d_num_events,
      kNumTabs);

  cudaStreamSynchronize(stream_);
  cudaMemcpy(&h_count, d_num_events, sizeof(int), cudaMemcpyDeviceToHost);
  return h_count;
}
```

---

#### Task 2.2：清零标志优化（1 天）
**问题**：`thrust::fill(d_ConPre, 0)` 也有开销。

**方案**：使用戳记去重（参考 `GPU_EVENT_COMPRESSION_PLAN.md`）
```cpp
// cuSAC.cuh 新增
class CModel {
 private:
  int* d_constraint_stamp;  // [kNumTabs]
  int current_stamp_ = 1;
};

// 压缩时检查戳记而非清零
__global__ void CompressWithStamp(
    const uint3* all_constraints,
    const int* constraint_stamp,
    int current_stamp,
    uint3* output_events,
    int* output_count,
    int num_constraints) {
  // ...
  if (constraint_stamp[idx] == current_stamp) {
    // 入队
  }
  // 无需清零
}

// Host 侧
int CModel::compress_Main_device() {
  current_stamp_++;
  CompressWithStamp<<<...>>>(d_MCon, d_constraint_stamp, current_stamp_, ...);
  // ...
}
```

---

#### Task 2.3：内存池引入（2 天）
**目标**：避免频繁分配 `d_MConEvt`。

```cpp
// cuSAC.cu 构造函数
CModel::CModel(...) {
  // 创建内存池
  cudaMemPoolProps poolProps = {};
  poolProps.allocType = cudaMemAllocationTypePinned;
  poolProps.location.type = cudaMemLocationTypeDevice;
  poolProps.location.id = 0;
  cudaMemPoolCreate(&mem_pool_, &poolProps);

  // 设置释放阈值
  uint64_t threshold = 128 * 1024 * 1024; // 128MB
  cudaMemPoolSetAttribute(mem_pool_, cudaMemPoolAttrReleaseThreshold, &threshold);
}

// 使用
void* temp_buffer;
cudaMallocAsync(&temp_buffer, size, stream_);
// ... 使用
cudaFreeAsync(temp_buffer, stream_); // 立即返回池
```

---

### M2 验收标准
- [x] `compress_Main` 在设备端完成
- [x] Host 往返次数减少 > 50%
- [x] 戳记去重工作正常
- [x] 性能提升 > 20%

### M2 预期收益
- Host↔Device 带宽减少 60%
- 同步点减少 > 50%
- 为 CUDA Graph 铺路

---

## M3：CUDA Graph 捕获（1 周，中风险）

### 目标
- 将 `enforceGAC` 循环捕获为 Graph
- 减少 kernel 启动开销

### 具体任务

#### Task 3.1：固定网格设计（2 天）
**问题**：Graph 节点参数不能变化，但 `num_ConEvt` 每轮不同。

**方案**：固定网格 + 线程内边界检查
```cpp
// cuSAC.cu 修改 CsCheckMain 签名
__global__ void CsCheckMain_FixedGrid(
    i32* mConPre,
    const u32x3* mCon,
    u32* bitDom,
    i32* dom_size,
    cudaTextureObject_t bitSup,
    cudaTextureObject_t neiCon,
    int* num_ConEvt,  // ← 改为指针，动态读取
    int current_level) {

  int bid = blockIdx.x;
  if (bid >= *num_ConEvt) return; // ← 边界检查

  // 其余逻辑不变
  const int a_0 = threadIdx.x;
  const int a_1 = threadIdx.y;
  // ...
}
```

---

#### Task 3.2：Graph 捕获实现（3 天）
```cpp
// cuSAC.cuh 新增
class CModel {
 private:
  cudaGraph_t gac_graph_ = nullptr;
  cudaGraphExec_t gac_graph_exec_ = nullptr;
  bool use_graph_ = true; // CMake 选项控制
};

// cuSAC.cu 实现
void CModel::BuildGACGraph() {
  if (!use_graph_) return;

  // 开始捕获
  cudaStreamBeginCapture(stream_, cudaStreamCaptureModeGlobal);

  // 捕获一轮迭代
  int max_grid = kNumTabs; // 上限
  CompressEventsKernel<<<max_grid, 256, 0, stream_>>>(
      thrust::raw_pointer_cast(d_MCon.data()),
      d_constraint_stamp,
      current_stamp_,
      thrust::raw_pointer_cast(d_MConEvt.data()),
      d_num_events,
      kNumTabs);

  CsCheckMain_FixedGrid<<<max_grid, dim3(kBitDomIntSize * 32, 1, 1),
                          kSharedMemSize, stream_>>>(
      thrust::raw_pointer_cast(d_ConPre.data()),
      thrust::raw_pointer_cast(d_MCon.data()),
      d_bitDom,
      thrust::raw_pointer_cast(d_cur_dom_size.data()),
      texObj_BitSup,
      texObj_MCon,
      d_num_events,
      current_level_);

  // 结束捕获
  cudaStreamEndCapture(stream_, &gac_graph_);

  // 实例化
  cudaGraphInstantiate(&gac_graph_exec_, gac_graph_, nullptr, nullptr, 0);
}

bool CModel::enforceGAC() {
  if (!use_graph_) {
    return enforceGAC_legacy(); // 回退路径
  }

  int num_ConEvt = compress_Main_device();
  cudaMemcpy(d_num_events, &num_ConEvt, sizeof(int), cudaMemcpyHostToDevice);

  while (true) {
    // 回放 Graph
    cudaGraphLaunch(gac_graph_exec_, stream_);
    cudaStreamSynchronize(stream_);

    // 检查收敛
    cudaMemcpy(&num_ConEvt, d_num_events, sizeof(int), cudaMemcpyDeviceToHost);
    if (num_ConEvt == 0 || !GAC_success) break;

    current_stamp_++;
  }

  return GAC_success;
}
```

---

#### Task 3.3：CMake 特性开关（1 天）
```cmake
# CMakeLists.txt
option(CPIM_GPU_USE_GRAPHS "Use CUDA Graphs for GAC propagation" OFF)

if(CPIM_GPU_USE_GRAPHS)
  target_compile_definitions(cpim PRIVATE CPIM_GPU_USE_GRAPHS)
endif()
```

```cpp
// cuSAC.cu
#ifdef CPIM_GPU_USE_GRAPHS
  use_graph_ = true;
#else
  use_graph_ = false;
#endif
```

---

### M3 验收标准
- [x] Graph 捕获成功
- [x] 固定网格正确处理边界
- [x] CMake 开关可切换新旧路径
- [x] Kernel 启动开销减少 > 80%

### M3 预期收益
- Nsight Systems 显示 CUDA API 时间 < 5%
- 小批量场景（events < 10）性能提升 > 30%

---

## M4：Warp 级优化与对齐加载（1 周，中风险）

### 目标
- 优化 `CsCheckMain` 内核的访存与计算

### 具体任务

#### Task 4.1：uint4 矢量加载（2 天）
**当前问题**（L458-459）：逐字加载 `bitDom`。

```cpp
// 旧代码
if (a_0 < kDeviceBitDomIntSize && a_1 == 0) {
  s_bitDom_x[a_0] = bitDom[level_offset + DeviceGetBitDomByIndex(xid, a_0)];
  s_bitDom_y[a_0] = bitDom[level_offset + DeviceGetBitDomByIndex(yid, a_0)];
}

// 新代码（假设 kBitDomIntSize 是 4 的倍数）
if (a_0 < kDeviceBitDomIntSize / 4 && a_1 == 0) {
  uint4* s_bitDom_x_v4 = reinterpret_cast<uint4*>(s_bitDom_x);
  uint4* s_bitDom_y_v4 = reinterpret_cast<uint4*>(s_bitDom_y);
  const uint4* bitDom_v4 = reinterpret_cast<const uint4*>(bitDom);

  s_bitDom_x_v4[a_0] = bitDom_v4[level_offset / 4 + xid * (kDeviceBitDomIntSize / 4) + a_0];
  s_bitDom_y_v4[a_0] = bitDom_v4[level_offset / 4 + yid * (kDeviceBitDomIntSize / 4) + a_0];
}
```

**收益**：全局内存事务减少 75%

---

#### Task 4.2：Warp 级聚合写回（2 天）
**当前问题**（L515-562）：每个 lane 独立 `atomicAnd`。

**方案**：Warp 内合并
```cpp
// 旧代码（L515-538）
if (a_0 % warpSize == 0) {
  int bitIdx = a_0 / 32;
  if (s_bitDom_x[bitIdx] ^ vote_x) {
    changex = true;
    u32 oldVal = atomicAnd(&bitDom[...], vote_x);
    // ...
  }
}

// 新代码：warp 聚合
if (a_1 == 0) {
  unsigned vote_x = __ballot_sync(0xFFFFFFFF, val_x != 0);

  // lane 0 统一写回
  if (lane == 0) {
    int bitIdx = warp_id;
    if (bitIdx < kDeviceBitDomIntSize) {
      if (s_bitDom_x[bitIdx] ^ vote_x) {
        u32 oldVal = atomicAnd(&bitDom[level_offset + xid * kDeviceBitDomIntSize + bitIdx], vote_x);
        int deletes = __popc(oldVal) - __popc(oldVal & vote_x);
        if (deletes > 0) {
          atomicSub(&dom_size[current_level * kDeviceNumVars + xid], deletes);
          changex = 1;
        }
      }
    }
  }
}
```

**收益**：原子冲突减少 > 50%

---

#### Task 4.3：Residue 见证缓存（3 天，可选）
**思路**：为 `(c, x, a)` 缓存上次找到的支持位置。

```cpp
// cuSAC.cuh 新增
class CModel {
 private:
  int* d_residue; // [kNumTabs * kMaxDomSize * 2] 存 (x的residue, y的residue)
};

// CsCheckMain 中使用
__global__ void CsCheckMain_WithResidue(..., int* residue) {
  // ...
  int res_idx = cid * kDeviceMaxDomSize * 2 + a_0 * 2;
  int last_support_x = residue[res_idx];
  int last_support_y = residue[res_idx + 1];

  // 优先检查 residue 位置
  if (last_support_x < kDeviceBitDomIntSize) {
    auto [x, y] = tex3D<uint2>(bitSup, a_0, last_support_x, bid);
    if (x & s_bitDom_y[last_support_x]) {
      val_x = 1; // 快速命中
      // 更新 residue
      residue[res_idx] = last_support_x;
    }
  }

  // 未命中则全量搜索
  if (!val_x) {
    for (int i = 0; i < kDeviceBitDomIntSize; ++i) {
      auto [x, y] = tex3D<uint2>(bitSup, a_0, i, bid);
      if (x & s_bitDom_y[i]) {
        val_x = 1;
        residue[res_idx] = i; // 记录新 residue
        break;
      }
    }
  }
}
```

**预期**：平均支持检查次数减少 40-60%（实际效果取决于问题特性）

---

### M4 验收标准
- [x] uint4 矢量加载实现
- [x] Warp 聚合写回实现
- [x] Residue 缓存可选启用
- [x] 全局带宽提升 > 30%
- [x] L2 命中率提升 > 20%

### M4 预期收益
- 内核执行时间减少 30-40%
- Nsight Compute 显示访存效率提升

---

## M5：Persistent Kernel（2-3 周，高风险，可选）

### 目标
- 将整个 GAC 循环搬到设备端
- 彻底消除 Host 参与

### ⚠️ 风险评估
- 复杂度高（状态机 + 队列管理）
- 调试困难（设备端死循环难排查）
- 收益不确定（取决于问题规模）

### 决策点
**在 M4 完成后评估**：
- 若 Host API 时间占比 < 5% → M5 可推迟
- 若小批量场景 SM 占用 < 20% → M5 值得投入

### 实施方案
参考 `GPU_GAC_PERSISTENT_STATE_MACHINE.md`，核心要点：
1. 设备端维护事件队列（环形缓冲或双缓冲）
2. 约束状态机（Idle / InQueue / Processing）
3. Cooperative Groups 同步

**建议**：
- 先完成 M0-M4 验证收益
- 若需要 M5，单独开分支实验
- 保留 CMake 开关 `-DCPIM_GPU_USE_PERSISTENT=OFF`

---

## 总体时间线

| 里程碑 | 时间 | 风险 | 累计收益（预估） |
|--------|------|------|-----------------|
| M0 | 1 周 | 低 | 30-50% |
| M1 | 1.5 周 | 低 | +10% |
| M2 | 1 周 | 中 | +20% |
| M3 | 1 周 | 中 | +30% |
| M4 | 1 周 | 中 | +30% |
| **总计 M0-M4** | **5.5 周** | - | **2-3x 加速** |
| M5（可选） | 2-3 周 | 高 | +20-40%（不确定） |

---

## 回退策略

每个里程碑都保留 CMake 开关：
```cmake
option(CPIM_GPU_M0_OPTIMIZATIONS "Enable M0 optimizations" ON)
option(CPIM_GPU_USE_ADAPTER "Use CModelAdapter instead of HModel" ON)
option(CPIM_GPU_DEVICE_COMPRESS "Use device-side event compression" ON)
option(CPIM_GPU_USE_GRAPHS "Use CUDA Graphs" ON)
option(CPIM_GPU_WARP_OPTIMIZATIONS "Enable warp-level optimizations" ON)
option(CPIM_GPU_USE_PERSISTENT "Use persistent kernel (experimental)" OFF)
```

代码中使用宏：
```cpp
#ifdef CPIM_GPU_USE_GRAPHS
  return enforceGAC_graph();
#else
  return enforceGAC_legacy();
#endif
```

---

## 验收与回归测试

### 自动化测试框架
```bash
# scripts/test_regression.sh
#!/bin/bash
BENCHMARKS=(queens-4 queens-12 haystacks-11)

for bench in "${BENCHMARKS[@]}"; do
  for config in baseline M0 M1 M2 M3 M4; do
    ./cpim_${config} "../samples/bench/${bench}_ext.xml" > "results/${config}_${bench}.log"

    # 验证结果一致性
    diff <(grep "Solution" "results/baseline_${bench}.log") \
         <(grep "Solution" "results/${config}_${bench}.log") || {
      echo "❌ ${config} regression on ${bench}"
      exit 1
    }
  done
done

echo "✅ All regression tests passed"
```

### 性能对比脚本
```python
# scripts/compare_milestones.py
import json, sys, matplotlib.pyplot as plt

configs = ['baseline', 'M0', 'M1', 'M2', 'M3', 'M4']
data = {c: json.load(open(f'results/{c}_summary.json')) for c in configs}

# 绘制加速比
benchmarks = ['queens-4', 'queens-12', 'haystacks-11']
for bench in benchmarks:
    speedups = [data['baseline'][bench]['time'] / data[c][bench]['time'] for c in configs]
    plt.plot(configs, speedups, marker='o', label=bench)

plt.ylabel('Speedup')
plt.legend()
plt.savefig('results/speedup_comparison.png')
```

---

## 总结

### 核心观点
✅ **不需要大规模重写 CModel**
- 通过渐进式重构实现所有优化
- 每个里程碑独立验证与回退
- 算法逻辑保持不变

### 关键改动点
1. **M0**：清理债务（printf、同步、错误检查）
2. **M1**：引入适配器层（解耦 HModel）
3. **M2**：设备端压缩（消除 Host 往返）
4. **M3**：CUDA Graph（减少启动开销）
5. **M4**：Warp 优化（提升访存效率）

### 预期收益
- M0-M4 总计：**2-3x 加速**
- 代码质量：**大幅提升**（清理、解耦、测试）
- 可维护性：**显著改善**（模块化、开关控制）

### 下一步行动
1. **立即开始 M0**（本周）：清理 printf + 采集基线
2. **评审本计划**：团队讨论优先级与时间分配
3. **创建分支**：`git checkout -b feature/gpu-jetson-m0`

---

**附录**：
- 参考文档：`GPU_JETSON_INDEPENDENT_REVIEW.md`
- 相关规划：`GPU_GAC_PIPELINE_PLAN.md`, `GPU_MODEL_DATA_PLAN.md`
- 联系反馈：在 `aig_docs/REVIEW_RESPONSES.md` 回复
