#include "model/gmodel_adapter.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "GModel.cuh"
#include "base/unified_trail.h"  // Phase 1.2: Trail 回溯系统
#include "model/intermediate_model.h"
#include "model/types.h"

namespace cpim::model {
namespace {

constexpr int kBitsPerWord = 32;

inline int IntSize(int nbits) {
  return (nbits + kBitsPerWord - 1) / kBitsPerWord;
}

inline uint32_t TailMask(int bits) {
  if (bits <= 0) return 0u;
  if (bits >= kBitsPerWord) return 0xFFFFFFFFu;
  return (1u << bits) - 1u;
}

#define CUDA_CHECK(call)                                                      \
  do {                                                                        \
    cudaError_t status = (call);                                              \
    if (status != cudaSuccess) {                                              \
      throw std::runtime_error(std::string("CUDA error at ") +               \
                               std::string(__FILE__) + ":" +                  \
                               std::to_string(__LINE__) + ": " +              \
                               cudaGetErrorString(status));                   \
    }                                                                         \
  } while (0)

}  // namespace

// ============================================================================
// Public API: Build GModel from IntermediateModel
// ============================================================================
GModel GModelAdapter::Build(const IntermediateModel& im_model,
                            const GModelOptions& options) {
  std::cout << "[GModelAdapter] Building GModel from IntermediateModel..."
            << std::endl;

  // 计算模型维度
  const int num_vars = im_model.num_variables();
  const int num_constraints = im_model.num_constraints();

  int max_dom_size = 0;
  for (const auto& var : im_model.variables()) {
    const auto& dom = im_model.GetDomain(var.domain);
    max_dom_size = std::max(max_dom_size, dom.Size());
  }

  const int bit_dom_int_size = IntSize(max_dom_size);
  const int bit_doms_int_size = num_vars * bit_dom_int_size;
  // Phase 1.2: 移除 max_depth，使用 UnifiedTrail 管理层级

  std::cout << "[GModelAdapter] Model dimensions:" << std::endl;
  std::cout << "  num_vars=" << num_vars << std::endl;
  std::cout << "  num_constraints=" << num_constraints << std::endl;
  std::cout << "  max_dom_size=" << max_dom_size << std::endl;
  std::cout << "  bit_dom_int_size=" << bit_dom_int_size << std::endl;
  std::cout << "  bit_doms_int_size=" << bit_doms_int_size << std::endl;

  // ========================================================================
  // 1. Phase 1.2: 分配单层 bitDom (使用统一内存)
  // ========================================================================
  const size_t bitdom_size = bit_doms_int_size * sizeof(u32);
  std::cout << "[GModelAdapter] Allocating single-layer bitDom: " << bitdom_size
            << " bytes (single level)" << std::endl;

  u32* bitDom = nullptr;
  CUDA_CHECK(cudaMallocManaged(&bitDom, bitdom_size));

  // 初始化为 0
  std::fill(bitDom, bitDom + bit_doms_int_size, 0u);

  // 初始化域
  BuildBitDom(im_model, bitDom, num_vars, bit_dom_int_size);
  std::cout << "[GModelAdapter] bitDom initialized" << std::endl;

  // ========================================================================
  // 2. 检查纹理内存限制
  // ========================================================================
  constexpr int kMaxTexture3DDepth = 16384;
  if (num_constraints > kMaxTexture3DDepth) {
    throw std::runtime_error(
        "[GModelAdapter] Error: num_constraints (" +
        std::to_string(num_constraints) + ") exceeds 3D texture depth limit (" +
        std::to_string(kMaxTexture3DDepth) + ")");
  }

  // ========================================================================
  // 3. 构建 bitSup 临时数据（CPU 端）
  // ========================================================================
  const size_t bitsup_per_constraint = 2 * max_dom_size * bit_dom_int_size;
  const size_t bitsup_size =
      num_constraints * bitsup_per_constraint * sizeof(uint2);

  std::cout << "[GModelAdapter] Building bitSup temp data: " << bitsup_size
            << " bytes " << "(" << num_constraints << " × "
            << bitsup_per_constraint << " uint2)" << std::endl;

  // 使用 std::vector 在 CPU 上临时存储
  std::vector<uint2> temp_bitSup(num_constraints * bitsup_per_constraint);
  std::fill(temp_bitSup.begin(), temp_bitSup.end(), make_uint2(0, 0));

  BuildBitSup(im_model, temp_bitSup.data(), num_constraints, max_dom_size,
              bit_dom_int_size, options.skip_non_binary);
  std::cout << "[GModelAdapter] bitSup temp data initialized" << std::endl;

  uint2* bitSup_managed = nullptr;
  CUDA_CHECK(cudaMallocManaged(&bitSup_managed, bitsup_size));
  std::memcpy(bitSup_managed, temp_bitSup.data(), bitsup_size);

  // ========================================================================
  // 4. 创建 3D 纹理对象
  // ========================================================================
  std::cout << "[GModelAdapter] Creating 3D texture for bitSup..." << std::endl;
  std::cout << "[GModelAdapter] Texture dimensions: " << max_dom_size << " × "
            << bit_dom_int_size << " × " << num_constraints
            << " (value × word × constraint)" << std::endl;

  cudaArray_t cuArray3D = nullptr;
  cudaTextureObject_t texObj_BitSup = 0;

  // 4.1 创建 3D CUDA 数组
  cudaChannelFormatDesc channelDesc = cudaCreateChannelDesc<uint2>();
  cudaExtent extent =
      make_cudaExtent(max_dom_size, bit_dom_int_size, num_constraints);
  CUDA_CHECK(cudaMalloc3DArray(&cuArray3D, &channelDesc, extent));

  // 4.2 拷贝数据到 CUDA 数组
  cudaMemcpy3DParms copyParams;
  memset(&copyParams, 0, sizeof(copyParams));
  copyParams.srcPtr = make_cudaPitchedPtr(temp_bitSup.data(),
                                          max_dom_size * sizeof(uint2),
                                          max_dom_size, bit_dom_int_size);
  copyParams.dstArray = cuArray3D;
  copyParams.extent = extent;
  copyParams.kind = cudaMemcpyHostToDevice;  // Jetson 上实际是零拷贝重映射
  CUDA_CHECK(cudaMemcpy3D(&copyParams));
  std::cout << "[GModelAdapter] bitSup data copied to CUDA array" << std::endl;

  // 4.3 创建纹理对象
  cudaResourceDesc resDesc;
  memset(&resDesc, 0, sizeof(resDesc));
  resDesc.resType = cudaResourceTypeArray;
  resDesc.res.array.array = cuArray3D;

  cudaTextureDesc texDesc;
  memset(&texDesc, 0, sizeof(texDesc));
  texDesc.addressMode[0] = cudaAddressModeClamp;
  texDesc.addressMode[1] = cudaAddressModeClamp;
  texDesc.addressMode[2] = cudaAddressModeClamp;
  texDesc.filterMode = cudaFilterModePoint;
  texDesc.readMode = cudaReadModeElementType;
  texDesc.normalizedCoords = 0;

  CUDA_CHECK(cudaCreateTextureObject(&texObj_BitSup, &resDesc, &texDesc, nullptr));
  std::cout << "[GModelAdapter] Texture object created successfully" << std::endl;

  // ========================================================================
  // 5. 可选：预取 bitDom 到 GPU
  // ========================================================================
  if (options.enable_prefetch) {
    PrefetchBitDomToGPU(bitDom, bitdom_size, options.device_id);
  }

  // ========================================================================
  // 6. 构建额外元数据（约束作用域、变量邻接、初始域大小）
  // ========================================================================
  std::vector<int2> constraint_scopes(num_constraints, make_int2(-1, -1));
  std::vector<std::vector<int>> var_to_constraints(num_vars);
  std::vector<int> initial_dom_sizes(num_vars, 0);

  // Phase 1.5: CPU 友好的约束作用域（用于 DOM/DDEG 启发式）
  std::vector<std::vector<int>> constraint_scopes_cpu(num_constraints);

  for (const auto& var : im_model.variables()) {
    initial_dom_sizes[var.id.value] = im_model.GetDomain(var.domain).Size();
  }

  for (int idx = 0; idx < num_constraints; ++idx) {
    const auto& constraint = im_model.constraints()[idx];
    const auto* ext = std::get_if<ExtensionConstraint>(&constraint.data);
    if (!ext || ext->Arity() != 2) continue;
    if (ext->semantics != ExtensionConstraint::Semantics::kSupports) continue;

    const int x = ext->scope[0].value;
    const int y = ext->scope[1].value;
    constraint_scopes[idx] = make_int2(x, y);
    var_to_constraints[x].push_back(idx);
    var_to_constraints[y].push_back(idx);

    // Phase 1.5: 填充 CPU 友好格式
    constraint_scopes_cpu[idx] = {x, y};
  }

  int2* scopes_managed = nullptr;
  CUDA_CHECK(cudaMallocManaged(&scopes_managed, num_constraints * sizeof(int2)));
  std::memcpy(scopes_managed, constraint_scopes.data(),
              num_constraints * sizeof(int2));

  // ========================================================================
  // 7. Phase 1.2: 分配单层辅助数据（统一内存）
  // ========================================================================
  std::cout << "[GModelAdapter] Allocating single-layer auxiliary data..." << std::endl;

  // 7.1 域大小追踪（单层）
  const size_t dom_size_array_size = num_vars * sizeof(int);
  int* d_cur_dom_size = nullptr;
  CUDA_CHECK(cudaMallocManaged(&d_cur_dom_size, dom_size_array_size));
  std::cout << "[GModelAdapter]   d_cur_dom_size: " << dom_size_array_size
            << " bytes (" << num_vars << " vars)" << std::endl;

  // 初始化域大小
  for (int var = 0; var < num_vars; ++var) {
    d_cur_dom_size[var] = initial_dom_sizes[var];
  }

  // 7.2 赋值栈（保留，用于记录赋值历史）
  const int max_assignments = num_vars;  // 最多 num_vars 个赋值
  const size_t assigned_size = max_assignments * sizeof(int2);
  int2* d_assigned = nullptr;
  CUDA_CHECK(cudaMallocManaged(&d_assigned, assigned_size));
  std::fill_n(reinterpret_cast<int*>(d_assigned), max_assignments * 2, -1);
  std::cout << "[GModelAdapter]   d_assigned: " << assigned_size
            << " bytes (" << max_assignments << " max assignments)" << std::endl;

  // 7.3 Device 端订阅表（CSR 格式）
  std::cout << "[GModelAdapter] Building device-side subscription table (CSR format)..."
            << std::endl;

  // 统计每个变量的度数
  std::vector<int> degrees(num_vars, 0);
  for (int idx = 0; idx < num_constraints; ++idx) {
    const auto& constraint = im_model.constraints()[idx];
    const auto* ext = std::get_if<ExtensionConstraint>(&constraint.data);
    if (!ext || ext->Arity() != 2) continue;
    if (ext->semantics != ExtensionConstraint::Semantics::kSupports) continue;

    const int x = ext->scope[0].value;
    const int y = ext->scope[1].value;
    ++degrees[x];
    ++degrees[y];
  }

  // 构建 CSR 偏移数组（前缀和）
  std::vector<int> offsets(num_vars + 1, 0);
  for (int i = 0; i < num_vars; ++i) {
    offsets[i + 1] = offsets[i] + degrees[i];
  }
  const int subscription_size = offsets[num_vars];

  std::cout << "[GModelAdapter]   subscription_size: " << subscription_size
            << " entries" << std::endl;

  // 填充订阅条目
  std::vector<uint3> entries(subscription_size);
  std::vector<int> current_pos = offsets;  // 复制偏移作为当前写入位置

  for (int idx = 0; idx < num_constraints; ++idx) {
    const auto& constraint = im_model.constraints()[idx];
    const auto* ext = std::get_if<ExtensionConstraint>(&constraint.data);
    if (!ext || ext->Arity() != 2) continue;
    if (ext->semantics != ExtensionConstraint::Semantics::kSupports) continue;

    const int cid = constraint.id.value;
    const int x = ext->scope[0].value;
    const int y = ext->scope[1].value;

    // 为变量 x 添加订阅条目
    entries[current_pos[x]++] = make_uint3(x, y, cid);
    // 为变量 y 添加订阅条目
    entries[current_pos[y]++] = make_uint3(x, y, cid);
  }

  // 分配统一内存并拷贝
  uint3* d_subscription = nullptr;
  int* d_subscription_offset = nullptr;

  const size_t entries_size = subscription_size * sizeof(uint3);
  const size_t offsets_size = (num_vars + 1) * sizeof(int);

  CUDA_CHECK(cudaMallocManaged(&d_subscription, entries_size));
  CUDA_CHECK(cudaMallocManaged(&d_subscription_offset, offsets_size));

  std::memcpy(d_subscription, entries.data(), entries_size);
  std::memcpy(d_subscription_offset, offsets.data(), offsets_size);

  std::cout << "[GModelAdapter]   d_subscription: " << entries_size << " bytes"
            << std::endl;
  std::cout << "[GModelAdapter]   d_subscription_offset: " << offsets_size
            << " bytes" << std::endl;

  // ========================================================================
  // 7.4 Phase 1.2: 创建 UnifiedTrail（统一回溯系统）
  // ========================================================================
  // Trail 容量估算：
  // - 最大搜索深度：num_vars（每个变量一次赋值）
  // - 每次传播可能删除多个值：num_vars * max_dom_size
  // - 保守估计：num_vars * max_dom_size * 20（足够大的缓冲）
  const int trail_capacity = num_vars * max_dom_size * 20;
  std::cout << "[GModelAdapter] Creating UnifiedTrail: capacity=" << trail_capacity
            << std::endl;

  // 注意：UnifiedTrail 使用 new 分配，所有权转移给 GModel
  UnifiedTrail* trail = new UnifiedTrail(trail_capacity, true /* enable_gpu */);
  std::cout << "[GModelAdapter]   Trail created successfully" << std::endl;

  // ========================================================================
  // 8. Phase 1.2: 构造 GModel（转移所有权）
  // ========================================================================
  std::cout << "[GModelAdapter] Building GModel object..." << std::endl;
  GModel gmodel(num_vars, num_constraints, max_dom_size, bit_dom_int_size,
                bit_doms_int_size, bitsup_per_constraint, bitDom,
                d_cur_dom_size, d_assigned, d_subscription, d_subscription_offset,
                subscription_size, texObj_BitSup, cuArray3D, bitSup_managed,
                scopes_managed, std::move(var_to_constraints),
                std::move(initial_dom_sizes), std::move(degrees),
                std::move(constraint_scopes_cpu), trail);

  // [Phase 4.3] 只读数据优化
  OptimizeReadOnlyMemoryAdvice(&gmodel, options.device_id);

  std::cout << "[GModelAdapter] GModel built successfully!" << std::endl;
  return gmodel;
}

// ============================================================================
// Private Helper: Build bitDom
// ============================================================================
void GModelAdapter::BuildBitDom(const IntermediateModel& im, u32* bitDom,
                                int num_vars, int bit_dom_int_size) {
  for (const auto& var : im.variables()) {
    const int vid = var.id.value;
    const auto& dom = im.GetDomain(var.domain);
    const int dom_size = dom.Size();

    // 为每个 word 设置位
    for (int word = 0; word < bit_dom_int_size; ++word) {
      const int base = word * kBitsPerWord;
      const int remaining = dom_size - base;
      bitDom[vid * bit_dom_int_size + word] = TailMask(remaining);
    }
  }
}

// ============================================================================
// Private Helper: Build bitSup
// ============================================================================
void GModelAdapter::BuildBitSup(const IntermediateModel& im, uint2* bitSup,
                                int num_constraints, int max_dom_size,
                                int bit_dom_int_size, bool skip_non_binary) {
  const size_t bitsup_per_constraint = 2 * max_dom_size * bit_dom_int_size;

  for (int idx = 0; idx < num_constraints; ++idx) {
    const auto& constraint = im.constraints()[idx];
    const auto* ext = std::get_if<ExtensionConstraint>(&constraint.data);

    if (!ext || ext->Arity() != 2) {
      if (skip_non_binary) {
        std::cout << "[GModelAdapter] Warning: Skipping non-binary constraint "
                  << idx << std::endl;
        continue;
      } else {
        throw std::invalid_argument(
            "GModelAdapter: only binary extension constraints are supported");
      }
    }

    if (ext->semantics != ExtensionConstraint::Semantics::kSupports) {
      std::cout << "[GModelAdapter] Warning: Skipping non-support constraint "
                << idx << std::endl;
      continue;
    }

    const int cid = constraint.id.value;
    const int x_var = ext->scope[0].value;
    const int y_var = ext->scope[1].value;

    // 处理每个支持元组
    for (const auto& tuple : ext->tuples) {
      if (tuple.size() != 2) continue;

      const int x_val = tuple[0];
      const int y_val = tuple[1];

      // 计算索引
      // 对于 x=x_val，需要记录 y 域中 y_val 的支持
      const int y_word = y_val / kBitsPerWord;
      const int y_bit = y_val % kBitsPerWord;
      const int idx_x = cid * bitsup_per_constraint +
                        (0 * max_dom_size + x_val) * bit_dom_int_size + y_word;

      // 对于 y=y_val，需要记录 x 域中 x_val 的支持
      const int x_word = x_val / kBitsPerWord;
      const int x_bit = x_val % kBitsPerWord;
      const int idx_y = cid * bitsup_per_constraint +
                        (1 * max_dom_size + y_val) * bit_dom_int_size + x_word;

      // 设置位
      bitSup[idx_x].x |= (1u << y_bit);
      bitSup[idx_y].y |= (1u << x_bit);
    }
  }
}

// ============================================================================
// Private Helper: Prefetch bitDom to GPU
// ============================================================================
void GModelAdapter::PrefetchBitDomToGPU(u32* bitDom, size_t bitdom_size,
                                        int device_id) {
  // 先设置设备以初始化 CUDA 上下文
  cudaError_t status = cudaSetDevice(device_id);
  if (status != cudaSuccess) {
    std::cout << "[GModelAdapter] Warning: cudaSetDevice failed: "
              << cudaGetErrorString(status) << std::endl;
    std::cout << "[GModelAdapter] Using unified memory without prefetch"
              << std::endl;
    return;
  }

  // 获取设备信息
  cudaDeviceProp prop;
  status = cudaGetDeviceProperties(&prop, device_id);
  if (status == cudaSuccess) {
    std::cout << "[GModelAdapter] GPU device " << device_id << ": " << prop.name
              << " (compute " << prop.major << "." << prop.minor << ")"
              << std::endl;
    std::cout << "[GModelAdapter] Managed memory: "
              << (prop.managedMemory ? "Yes" : "No") << std::endl;
    std::cout << "[GModelAdapter] Concurrent managed access: "
              << (prop.concurrentManagedAccess ? "Yes" : "No") << std::endl;

    // Jetson 设备的统一内存架构不支持也不需要 cudaMemPrefetchAsync
    if (prop.concurrentManagedAccess == 0) {
      std::cout << "[GModelAdapter] Device uses integrated unified memory (no "
                   "prefetch needed)"
                << std::endl;
      std::cout
          << "[GModelAdapter] Memory is already accessible by both CPU and GPU"
          << std::endl;
      return;
    }
  }

  // 预取 bitDom 到 GPU（仅对支持的设备）
  std::cout << "[GModelAdapter] Prefetching bitDom to GPU..." << std::endl;
  status = cudaMemPrefetchAsync(bitDom, bitdom_size, device_id);
  if (status != cudaSuccess) {
    std::cout << "[GModelAdapter] Warning: bitDom prefetch failed: "
              << cudaGetErrorString(status) << std::endl;
    return;
  }

  cudaDeviceSynchronize();
  std::cout << "[GModelAdapter] bitDom successfully prefetched to GPU"
            << std::endl;
}

// ============================================================================
// [Phase 4.3] 只读数据优化：cudaMemAdviseSetReadMostly
// ============================================================================
void GModelAdapter::OptimizeReadOnlyMemoryAdvice(GModel* model, int device_id) {
  if (model == nullptr) {
    std::cout << "[GModelAdapter] Warning: model is nullptr, skip read-only advice"
              << std::endl;
    return;
  }

  cudaError_t status = cudaSetDevice(device_id);
  if (status != cudaSuccess) {
    std::cout << "[GModelAdapter] Warning: cudaSetDevice failed, skip read-only advice"
              << std::endl;
    return;
  }

  std::cout << "[GModelAdapter] Applying cudaMemAdviseSetReadMostly for read-only data..."
            << std::endl;

  int success_count = 0;
  int skip_count = 0;
  int fail_count = 0;

  // 辅助 lambda：安全地调用 cudaMemAdvise
  auto apply_advice = [&](void* ptr, size_t size, const char* name) {
    if (ptr == nullptr || size == 0) {
      std::cout << "[GModelAdapter]   " << name << ": skipped (nullptr or size=0)" << std::endl;
      skip_count++;
      return;
    }
    cudaError_t err = cudaMemAdvise(ptr, size, cudaMemAdviseSetReadMostly, device_id);
    if (err != cudaSuccess) {
      std::cout << "[GModelAdapter]   " << name << ": failed ("
                << cudaGetErrorString(err) << ")" << std::endl;
      fail_count++;
    } else {
      std::cout << "[GModelAdapter]   " << name << ": " << size << " bytes" << std::endl;
      success_count++;
    }
  };

  // 1. bitSupData（最大的只读数据）
  const size_t bitsup_size = static_cast<size_t>(model->num_constraints) *
                             model->bitsup_per_constraint * sizeof(uint2);
  apply_advice(model->bitSupData, bitsup_size, "bitSupData");

  // 2. d_subscription（变量订阅表）
  const size_t sub_size = static_cast<size_t>(model->subscription_size) * sizeof(uint3);
  apply_advice(model->d_subscription, sub_size, "d_subscription");

  // 3. d_subscription_offset（CSR 偏移索引）
  const size_t offset_size = static_cast<size_t>(model->num_vars + 1) * sizeof(int);
  apply_advice(model->d_subscription_offset, offset_size, "d_subscription_offset");

  // 4. constraint_scopes（约束作用域）
  const size_t scopes_size = static_cast<size_t>(model->num_constraints) * sizeof(int2);
  apply_advice(model->constraint_scopes, scopes_size, "constraint_scopes");

  // 输出统计结果
  if (fail_count == 0 && skip_count == 0) {
    std::cout << "[GModelAdapter] Read-only memory advice applied successfully ("
              << success_count << "/4)" << std::endl;
  } else if (fail_count == 0) {
    std::cout << "[GModelAdapter] Read-only memory advice applied with skips ("
              << success_count << " success, " << skip_count << " skipped)" << std::endl;
  } else {
    std::cout << "[GModelAdapter] Read-only memory advice applied with warnings ("
              << success_count << " success, " << fail_count << " failed, "
              << skip_count << " skipped)" << std::endl;
  }
}

}  // namespace cpim::model
