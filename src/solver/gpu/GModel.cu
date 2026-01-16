#include "GModel.cuh"

#include <algorithm>
#include <cstring>  // for memset (统一内存直接操作)
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

#include <cooperative_groups.h>
#include <glog/logging.h>

#include "model/gmodel_adapter.h"
#include "model/intermediate_model.h"
#include "solver/gpu/batch_probe_manager.h"

namespace cpim {

namespace cg = cooperative_groups;

// GPU kernel to verify data access (deprecated, moved to GModelValidator)
__global__ void VerifyGModelKernel(const u32* bitDom,
                                   cudaTextureObject_t bitSup_tex, int num_vars,
                                   int num_constraints, int bit_dom_int_size,
                                   int max_dom_size) {
  int tid = blockIdx.x * blockDim.x + threadIdx.x;

  // Each thread verifies one variable's bitDom
  if (tid < num_vars) {
    u32 dom_word = bitDom[tid * bit_dom_int_size];
    printf("[GPU Validator] var[%d] bitDom[0] = 0x%x\n", tid, dom_word);
  }

  // First few threads verify constraint bitSup (via texture)
  if (tid < num_constraints) {
    // 读取纹理坐标 (value=0, word=0, constraint=tid)
    uint2 sup_word = tex3D<uint2>(bitSup_tex, 0, 0, tid);
    printf("[GPU Validator] constraint[%d] bitSup tex3D(0,0,%d) = (0x%x, 0x%x)\n",
           tid, tid, sup_word.x, sup_word.y);
  }
}

// ============================================================================
namespace {

constexpr int kBitsPerWord = 32;

inline int Popcount(u32 value) {
  return __builtin_popcount(value);
}

}  // namespace

// ============================================================================
// Private Constructor (called by GModelAdapter)
// ============================================================================
// Phase 1.2: 私有构造函数 - 移除 max_depth，添加 trail
GModel::GModel(int num_vars, int num_constraints, int max_dom_size,
               int bit_dom_int_size, int bit_doms_int_size,
               int bitsup_per_constraint, u32* bitDom, int* d_cur_dom_size,
               int2* d_assigned, uint3* d_subscription,
               int* d_subscription_offset, int subscription_size,
               cudaTextureObject_t texObj_BitSup, cudaArray_t cuArray3D_BitSup,
               uint2* bitSupData, int2* constraint_scopes,
               std::vector<std::vector<int>> var_to_constraints,
               std::vector<int> initial_dom_sizes,
               std::vector<int> var_degrees,
               std::vector<std::vector<int>> constraint_scopes_cpu,
               UnifiedTrail* trail)
    : num_vars(num_vars),
      num_constraints(num_constraints),
      max_dom_size(max_dom_size),
      bit_dom_int_size(bit_dom_int_size),
      bit_doms_int_size(bit_doms_int_size),
      bitsup_per_constraint(bitsup_per_constraint),
      bitDom(bitDom),
      d_cur_dom_size(d_cur_dom_size),
      trail_(trail),
      d_assigned(d_assigned),
      d_subscription(d_subscription),
      d_subscription_offset(d_subscription_offset),
      subscription_size(subscription_size),
      texObj_BitSup(texObj_BitSup),
      bitSupData(bitSupData),
      constraint_scopes(constraint_scopes),
      var_to_constraints(std::move(var_to_constraints)),
      initial_dom_sizes(std::move(initial_dom_sizes)),
      var_degrees(std::move(var_degrees)),
      constraint_scopes_cpu(std::move(constraint_scopes_cpu)),
      cuArray3D_BitSup(cuArray3D_BitSup) {
  std::cout << "[GModel] Constructed with single-layer domain and Trail backtracking"
            << std::endl;
  std::cout << "  num_vars=" << num_vars << std::endl;
  std::cout << "  num_constraints=" << num_constraints << std::endl;
  std::cout << "  max_dom_size=" << max_dom_size << std::endl;
  std::cout << "  bit_dom_int_size=" << bit_dom_int_size << std::endl;
  std::cout << "  bit_doms_int_size=" << bit_doms_int_size << std::endl;
  std::cout << "  subscription_size=" << subscription_size << std::endl;
  std::cout << "  texObj_BitSup=" << texObj_BitSup << std::endl;
}

// ============================================================================
// Deprecated Public Constructor (for backward compatibility)
// ============================================================================
// Phase 1.2: 移除 max_depth 初始化
GModel::GModel(const model::IntermediateModel& im_model)
    : num_vars(0),  // Temporary, will be reassigned
      num_constraints(0),
      max_dom_size(0),
      bit_dom_int_size(0),
      bit_doms_int_size(0) {
  std::cout << "[GModel] WARNING: Using deprecated constructor. Please use "
               "GModelAdapter::Build() instead."
            << std::endl;

  // Delegate to GModelAdapter
  model::GModelOptions options;
  options.enable_prefetch = false;  // Default for Jetson

  GModel temp = model::GModelAdapter::Build(im_model, options);

  // Move data from temp to this
  *this = std::move(temp);
}

// ============================================================================
// Move Constructor and Assignment
// ============================================================================
// Phase 1.2: 移除 max_depth 和 trail_->CurrentLevel()，添加 trail_
GModel::GModel(GModel&& other) noexcept
    : num_vars(other.num_vars),
      num_constraints(other.num_constraints),
      max_dom_size(other.max_dom_size),
      bit_dom_int_size(other.bit_dom_int_size),
      bit_doms_int_size(other.bit_doms_int_size),
      bitsup_per_constraint(other.bitsup_per_constraint),
      bitDom(other.bitDom),
      d_cur_dom_size(other.d_cur_dom_size),
      trail_(other.trail_),
      d_assigned(other.d_assigned),
      d_subscription(other.d_subscription),
      d_subscription_offset(other.d_subscription_offset),
      subscription_size(other.subscription_size),
      texObj_BitSup(other.texObj_BitSup),
      bitSupData(other.bitSupData),
      constraint_scopes(other.constraint_scopes),
      var_to_constraints(std::move(other.var_to_constraints)),
      initial_dom_sizes(std::move(other.initial_dom_sizes)),
      cuArray3D_BitSup(other.cuArray3D_BitSup),
      assigned_size_(other.assigned_size_) {
  // Take ownership of resources
  other.bitDom = nullptr;
  other.d_cur_dom_size = nullptr;
  other.trail_ = nullptr;
  other.d_assigned = nullptr;
  other.d_subscription = nullptr;
  other.d_subscription_offset = nullptr;
  other.subscription_size = 0;
  other.texObj_BitSup = 0;
  other.bitSupData = nullptr;
  other.constraint_scopes = nullptr;
  other.cuArray3D_BitSup = nullptr;
  other.assigned_size_ = 0;
}

GModel& GModel::operator=(GModel&& other) noexcept {
  if (this != &other) {
    // Free existing resources
    if (bitDom) cudaFree(bitDom);
    if (d_cur_dom_size) cudaFree(d_cur_dom_size);
    if (d_assigned) cudaFree(d_assigned);
    if (d_subscription) cudaFree(d_subscription);
    if (d_subscription_offset) cudaFree(d_subscription_offset);
    if (texObj_BitSup) cudaDestroyTextureObject(texObj_BitSup);
    if (cuArray3D_BitSup) cudaFreeArray(cuArray3D_BitSup);
    if (bitSupData) cudaFree(bitSupData);
    if (constraint_scopes) cudaFree(constraint_scopes);

    // Transfer ownership
    // Phase 1.2: 移除 max_depth 和 trail_->CurrentLevel()
    const_cast<int&>(num_vars) = other.num_vars;
    const_cast<int&>(num_constraints) = other.num_constraints;
    const_cast<int&>(max_dom_size) = other.max_dom_size;
    const_cast<int&>(bit_dom_int_size) = other.bit_dom_int_size;
    const_cast<int&>(bit_doms_int_size) = other.bit_doms_int_size;
    const_cast<int&>(bitsup_per_constraint) = other.bitsup_per_constraint;
    bitDom = other.bitDom;
    d_cur_dom_size = other.d_cur_dom_size;
    trail_ = other.trail_;
    d_assigned = other.d_assigned;
    d_subscription = other.d_subscription;
    d_subscription_offset = other.d_subscription_offset;
    subscription_size = other.subscription_size;
    texObj_BitSup = other.texObj_BitSup;
    bitSupData = other.bitSupData;
    constraint_scopes = other.constraint_scopes;
    var_to_constraints = std::move(other.var_to_constraints);
    initial_dom_sizes = std::move(other.initial_dom_sizes);
    cuArray3D_BitSup = other.cuArray3D_BitSup;
    assigned_size_ = other.assigned_size_;

    other.bitDom = nullptr;
    other.d_cur_dom_size = nullptr;
    other.trail_ = nullptr;
    other.d_assigned = nullptr;
    other.d_subscription = nullptr;
    other.d_subscription_offset = nullptr;
    other.subscription_size = 0;
    other.texObj_BitSup = 0;
    other.bitSupData = nullptr;
    other.constraint_scopes = nullptr;
    other.cuArray3D_BitSup = nullptr;
    other.assigned_size_ = 0;
  }
  return *this;
}

// ============================================================================
// Destructor
// ============================================================================
GModel::~GModel() {
  // 释放 Bitmap GAC 资源
  FreeGPUResources();

  if (bitDom || d_cur_dom_size || d_assigned || d_subscription ||
      d_subscription_offset || texObj_BitSup || cuArray3D_BitSup) {
    std::cout << "[GModel] Destructor: freeing GPU memory and textures"
              << std::endl;
  }

  if (bitDom) {
    cudaFree(bitDom);
    bitDom = nullptr;
  }

  if (d_cur_dom_size) {
    cudaFree(d_cur_dom_size);
    d_cur_dom_size = nullptr;
  }

  if (d_assigned) {
    cudaFree(d_assigned);
    d_assigned = nullptr;
  }

  if (d_subscription) {
    cudaFree(d_subscription);
    d_subscription = nullptr;
  }

  if (d_subscription_offset) {
    cudaFree(d_subscription_offset);
    d_subscription_offset = nullptr;
  }

  if (texObj_BitSup) {
    cudaDestroyTextureObject(texObj_BitSup);
    texObj_BitSup = 0;
  }

  if (bitSupData) {
    cudaFree(bitSupData);
    bitSupData = nullptr;
  }

  if (constraint_scopes) {
    cudaFree(constraint_scopes);
    constraint_scopes = nullptr;
  }

  if (cuArray3D_BitSup) {
    cudaFreeArray(cuArray3D_BitSup);
    cuArray3D_BitSup = nullptr;
  }
}

// ============================================================================
// Deprecated BuildFromIntermediate (kept for compatibility)
// ============================================================================
void GModel::BuildFromIntermediate(
    const model::IntermediateModel& im_model) {
  // This method is no longer used, but kept for compilation compatibility
  // All logic has been moved to GModelAdapter
  throw std::runtime_error(
      "GModel::BuildFromIntermediate is deprecated. Use GModelAdapter::Build() "
      "instead.");
}

// ============================================================================
// Print Method
// ============================================================================
void GModel::Print(int max_print) const {
  std::cout << "\n=== GModel Summary ===" << std::endl;
  std::cout << "Variables: " << num_vars << std::endl;
  std::cout << "Constraints: " << num_constraints << std::endl;
  std::cout << "Max domain size: " << max_dom_size << std::endl;
  std::cout << "Bit words per domain: " << bit_dom_int_size << std::endl;

  // 打印部分 bitDom
  std::cout << "\n--- bitDom (first " << std::min(max_print, num_vars)
            << " variables) ---" << std::endl;
  for (int var = 0; var < num_vars && var < max_print; ++var) {
    std::cout << "var[" << var << "]: ";
    for (int word = 0; word < bit_dom_int_size; ++word) {
      const u32 value = bitDom[var * bit_dom_int_size + word];
      std::cout << "0x" << std::hex << value << std::dec;
      if (word + 1 < bit_dom_int_size) std::cout << " ";
    }
    std::cout << std::endl;
  }

  // 注意：bitSup 使用纹理内存，在 CPU 端不可访问
  std::cout << "\n--- bitSup ---" << std::endl;
  std::cout << "bitSup stored in 3D texture memory (GPU-only, not accessible from "
               "CPU)"
            << std::endl;
  std::cout << "  Texture object ID: " << texObj_BitSup << std::endl;
  std::cout << "  Texture dimensions: " << max_dom_size << " × "
            << bit_dom_int_size << " × " << num_constraints
            << " (value × word × constraint)" << std::endl;
  std::cout << "  Use GPU kernel to verify texture content" << std::endl;

  std::cout << "\n=== End of GModel dump ===" << std::endl;
}

// ============================================================================
// Deprecated VerifyOnGPU Method (kept for backward compatibility)
// ============================================================================
void GModel::VerifyOnGPU() const {
  std::cout << "\n=== Verifying GModel on GPU (deprecated) ===" << std::endl;
  std::cout
      << "WARNING: This method is deprecated. Use "
         "GModelValidator::ValidateGPUMemory() instead."
      << std::endl;

  // 检查数据指针
  if (!bitDom || !texObj_BitSup) {
    std::cout << "[VerifyOnGPU] Error: null pointers or invalid texture object"
              << std::endl;
    return;
  }

  // 启动 kernel: 使用足够的线程来覆盖 max(num_vars, num_constraints)
  const int num_threads = std::max(num_vars, num_constraints);
  const int block_size = 256;
  const int num_blocks = (num_threads + block_size - 1) / block_size;

  std::cout << "[VerifyOnGPU] Launching kernel with " << num_blocks
            << " blocks, " << block_size << " threads per block" << std::endl;
  std::cout << "[VerifyOnGPU] Total threads: " << num_threads << std::endl;

  // 调用 kernel
  VerifyGModelKernel<<<num_blocks, block_size>>>(
      bitDom, texObj_BitSup, num_vars, num_constraints, bit_dom_int_size,
      max_dom_size);

  // 同步并检查错误
  cudaError_t status = cudaDeviceSynchronize();
  if (status != cudaSuccess) {
    std::cout << "[VerifyOnGPU] Kernel execution failed: "
              << cudaGetErrorString(status) << std::endl;
    return;
  }

  std::cout << "[VerifyOnGPU] Kernel execution successful!" << std::endl;
  std::cout << "=== GPU verification complete ===" << std::endl;
}

namespace {

// Phase 1.2: 移除 current_level 和 bit_doms_int_size 参数（单层域）
__global__ void CsCheckMainKernel(const int* events, int num_events,
                                  u32* bitDom, const uint2* bitSup,
                                  const int2* scopes, int bit_dom_int_size,
                                  int max_dom_size, int bitsup_per_constraint,
                                  u32* removal) {
  const int event_idx = blockIdx.x;
  if (event_idx >= num_events) return;

  const int cid = events[event_idx];
  const int2 scope = scopes[cid];
  if (scope.x < 0 || scope.y < 0) return;

  const int x = scope.x;
  const int y = scope.y;
  // Phase 1.2: 单层域，移除 level_offset
  u32* dom_x = bitDom + x * bit_dom_int_size;
  u32* dom_y = bitDom + y * bit_dom_int_size;

  extern __shared__ u32 shared[];
  u32* s_dom_x = shared;
  u32* s_dom_y = shared + bit_dom_int_size;

  // Load shared memory (only first bit_dom_int_size threads needed)
  if (threadIdx.x < bit_dom_int_size) {
    s_dom_x[threadIdx.x] = dom_x[threadIdx.x];
    s_dom_y[threadIdx.x] = dom_y[threadIdx.x];
  }
  __syncthreads();

  // Each thread handles exactly one domain value (cuSAC style)
  const int val = threadIdx.x;
  const int word = val / kBitsPerWord;
  const int bit = val % kBitsPerWord;
  const u32 mask = 1u << bit;
  const int lane = threadIdx.x & 31;

  bool keep_x = false;
  bool keep_y = false;

  // Check if this value is in the domain
  if (word < bit_dom_int_size) {
    const bool active_x = (s_dom_x[word] & mask) != 0u;
    const bool active_y = (s_dom_y[word] & mask) != 0u;

    // Find support for x=val (branch-free version to avoid warp divergence)
    const int sup_idx_base_x = cid * bitsup_per_constraint +
                               (0 * max_dom_size + val) * bit_dom_int_size;
    bool has_support_x = false;
    #pragma unroll
    for (int w = 0; w < bit_dom_int_size; ++w) {
      has_support_x |= (bitSup[sup_idx_base_x + w].x & s_dom_y[w]) != 0;
    }
    keep_x = active_x && has_support_x;

    // Find support for y=val (branch-free version to avoid warp divergence)
    const int sup_idx_base_y = cid * bitsup_per_constraint +
                               (1 * max_dom_size + val) * bit_dom_int_size;
    bool has_support_y = false;
    #pragma unroll
    for (int w = 0; w < bit_dom_int_size; ++w) {
      has_support_y |= (bitSup[sup_idx_base_y + w].y & s_dom_x[w]) != 0;
    }
    keep_y = active_y && has_support_y;
  }

  // Warp-level voting (all threads participate)
  const unsigned keep_mask_x = __ballot_sync(0xFFFFFFFF, keep_x);
  const unsigned keep_mask_y = __ballot_sync(0xFFFFFFFF, keep_y);

  // First thread in each warp writes the result
  if (lane == 0 && word < bit_dom_int_size) {
    const u32 old_word_x = s_dom_x[word];
    const u32 new_word_x = old_word_x & keep_mask_x;
    const u32 removed_x = old_word_x ^ new_word_x;
    if (removed_x) {
      s_dom_x[word] = new_word_x;
      atomicOr(&removal[x * bit_dom_int_size + word], removed_x);
      // Phase 1.2: 单层域，移除 level_offset
      atomicAnd(reinterpret_cast<unsigned int*>(
                    &bitDom[x * bit_dom_int_size + word]),
                ~removed_x);
    }

    const u32 old_word_y = s_dom_y[word];
    const u32 new_word_y = old_word_y & keep_mask_y;
    const u32 removed_y = old_word_y ^ new_word_y;
    if (removed_y) {
      s_dom_y[word] = new_word_y;
      atomicOr(&removal[y * bit_dom_int_size + word], removed_y);
      // Phase 1.2: 单层域，移除 level_offset
      atomicAnd(reinterpret_cast<unsigned int*>(
                    &bitDom[y * bit_dom_int_size + word]),
                ~removed_y);
    }
  }
}

}  // namespace

GacStats GModel::EnforceGAC_Legacy(bool verbose) {
  GacStats stats;
  if (!bitDom || !bitSupData || !constraint_scopes) {
    std::cerr << "[GModel::EnforceGAC] Missing GPU data structures" << std::endl;
    return stats;
  }

  // 初始事件队列：所有有效二元 supports 约束
  std::vector<int> queue;
  queue.reserve(num_constraints);
  for (int cid = 0; cid < num_constraints; ++cid) {
    if (constraint_scopes[cid].x >= 0) queue.push_back(cid);
  }
  if (queue.empty()) {
    if (verbose) {
      std::cout << "[GAC] No binary support constraints to process" << std::endl;
    }
    return stats;
  }

  const int total_words = num_vars * bit_dom_int_size;
  u32* removal = nullptr;
  if (cudaMallocManaged(&removal, total_words * sizeof(u32)) != cudaSuccess) {
    std::cerr << "[GAC] Failed to allocate removal buffer" << std::endl;
    return stats;
  }

  int* d_events = nullptr;
  if (cudaMallocManaged(&d_events, num_constraints * sizeof(int)) !=
      cudaSuccess) {
    std::cerr << "[GAC] Failed to allocate events buffer" << std::endl;
    cudaFree(removal);
    return stats;
  }

  std::vector<int> dom_size = initial_dom_sizes;
  if (dom_size.size() != static_cast<size_t>(num_vars)) {
    dom_size.assign(num_vars, max_dom_size);
  }

  std::vector<char> in_queue(num_constraints, 0);
  std::vector<int> reset_list;
  reset_list.reserve(num_constraints);

  while (!queue.empty()) {
    ++stats.iterations;

    const int num_events = static_cast<int>(queue.size());
    for (int i = 0; i < num_events; ++i) {
      d_events[i] = queue[i];
    }

    cudaMemset(removal, 0, total_words * sizeof(u32));

    // cuSAC style: each thread handles exactly one domain value
    // threads_per_block = max_dom_size (must be <= 1024)
    int threads_per_block = max_dom_size;
    if (threads_per_block > 1024) {
      std::cerr << "[GAC] ERROR: max_dom_size=" << max_dom_size
                << " exceeds CUDA limit of 1024 threads per block!" << std::endl;
      threads_per_block = 1024;  // Fallback (will not work correctly)
    }

    size_t shared_mem_bytes =
        static_cast<size_t>(2 * bit_dom_int_size) * sizeof(u32);

    // Phase 1.2: 移除 current_level 和 bit_doms_int_size 参数
    CsCheckMainKernel<<<num_events, threads_per_block, shared_mem_bytes>>>(
        d_events, num_events, bitDom, bitSupData, constraint_scopes,
        bit_dom_int_size, max_dom_size, bitsup_per_constraint, removal);
    cudaError_t err = cudaDeviceSynchronize();
    if (err != cudaSuccess) {
      std::cerr << "[GAC] Kernel failed: " << cudaGetErrorString(err)
                << std::endl;
      break;
    }

    std::vector<int> touched_vars;
    touched_vars.reserve(64);

    for (int var = 0; var < num_vars; ++var) {
      bool changed = false;
      const int base = var * bit_dom_int_size;
      for (int w = 0; w < bit_dom_int_size; ++w) {
        const u32 mask = removal[base + w];
        if (mask == 0u) continue;
        if (verbose) {
          for (int b = 0; b < kBitsPerWord; ++b) {
            if ((mask >> b) & 1u) {
              const int value = w * kBitsPerWord + b;
              if (value < max_dom_size) {
                std::cout << "[GAC] remove var " << var << " value " << value
                          << " (iteration " << stats.iterations << ")\n";
              }
            }
          }
        }
        stats.deletions += Popcount(mask);
        changed = true;
      }
      if (changed) {
        const int level_base = trail_->CurrentLevel() * bit_doms_int_size;
        int new_size = 0;
        for (int w = 0; w < bit_dom_int_size; ++w) {
          new_size += Popcount(bitDom[level_base + base + w]);
        }
        dom_size[var] = new_size;
        // Sync to d_cur_dom_size for multi-level support
        d_cur_dom_size[trail_->CurrentLevel() * num_vars + var] = new_size;
        touched_vars.push_back(var);
        if (new_size == 0) {
          stats.inconsistent = true;
        }
      }
      if (stats.inconsistent) break;
    }

    if (stats.inconsistent) break;

    std::vector<int> next_queue;
    next_queue.reserve(queue.size());

    for (int var : touched_vars) {
      const auto& neighbors = var_to_constraints[var];
      for (int cid : neighbors) {
        if (in_queue[cid]) continue;
        in_queue[cid] = 1;
        reset_list.push_back(cid);
        next_queue.push_back(cid);
      }
    }

    for (int cid : reset_list) {
      in_queue[cid] = 0;
    }
    reset_list.clear();

    queue.swap(next_queue);
  }

  cudaFree(d_events);
  cudaFree(removal);

  // 确保所有内存写入完成（统一内存同步）
  cudaDeviceSynchronize();

  if (verbose) {
    std::cout << "\n[GAC] iterations=" << stats.iterations
              << " deletions=" << stats.deletions
              << " inconsistent=" << (stats.inconsistent ? "true" : "false")
              << std::endl;
  }

  return stats;
}

// ============================================================================
// Bitmap GAC 传播实现（Scheme F: Bitmap Frontier）
// ============================================================================

namespace {

// 传播结果结构
struct PropagateResult {
  bool x_changed;
  bool y_changed;
  bool inconsistent;
  int deletions;
};

// 变量 → 邻接约束的传播（Producer）
__device__ __forceinline__
void PropagateVarToNextBitmap(
    int var,
    const GModelData& model,
    u32* next_bitmap) {

  const int start = model.d_subscription_offset[var];
  const int end = model.d_subscription_offset[var + 1];

  for (int i = start; i < end; ++i) {
    int cid = model.d_subscription[i].z;  // uint3 中 cid 在 z 分量
    int w = cid / 32;
    int b = cid % 32;
    atomicOr(&next_bitmap[w], 1u << b);
  }
}

// 从 bitmap 取任务（word 级调度）
__device__ __forceinline__
int FetchNextCidFromBitmap(
    u32* frontier_cur,
    int bitmap_size_words,
    int* scanner_index,
    int& local_word,
    int& local_offset) {

  while (true) {
    if (local_word != 0) {
      int bit = __ffs(local_word) - 1;
      local_word &= ~(1u << bit);
      return local_offset + bit;
    }
    int w = atomicAdd(scanner_index, 1);
    if (w >= bitmap_size_words) {
      return -1;  // 本轮没有任务了
    }
    u32 word = atomicExch(&frontier_cur[w], 0u);
    if (word == 0u) {
      continue;  // 这个 word 没有任务，继续抢下一 word
    }
    local_word = word;
    local_offset = w * 32;
  }
}

// BpC 核心：执行单约束检查（Legacy 版本 - 条纹分配）
// 适用于 bit_dom_int_size == 1 的小域场景
__device__
PropagateResult ExecuteConstraintCheck_BpC_Legacy(
    int cid,
    const GModelData& model,
    u32* shared_mem) {

  PropagateResult r{};
  r.x_changed = r.y_changed = r.inconsistent = false;
  r.deletions = 0;

  const int2 scope = model.constraint_scopes[cid];
  const int x = scope.x;
  const int y = scope.y;
  if (x < 0 || y < 0) {
    return r;
  }

  // Phase 1.2: 单层域，移除 level_offset
  u32* dom_x = model.bitDom + x * model.bit_dom_int_size;
  u32* dom_y = model.bitDom + y * model.bit_dom_int_size;

  u32* s_dom_x = shared_mem;
  u32* s_dom_y = shared_mem + model.bit_dom_int_size;

  // 1) load 到 shared memory
  for (int w = threadIdx.x; w < model.bit_dom_int_size; w += blockDim.x) {
    s_dom_x[w] = dom_x[w];
    s_dom_y[w] = dom_y[w];
  }
  __syncthreads();

  // 2) 每个线程处理若干 value
  for (int val = threadIdx.x; val < model.max_dom_size; val += blockDim.x) {
    const int word = val / 32;
    const int bit = val % 32;
    if (word >= model.bit_dom_int_size) break;
    const u32 mask = 1u << bit;

    const bool active_x = (s_dom_x[word] & mask) != 0u;
    const bool active_y = (s_dom_y[word] & mask) != 0u;

    bool keep_x = true, keep_y = true;

    if (active_x) {
      const int sup_idx_base_x =
          cid * model.bitsup_per_constraint +
          (0 * model.max_dom_size + val) * model.bit_dom_int_size;
      bool has_sup = false;
      #pragma unroll
      for (int w = 0; w < model.bit_dom_int_size; ++w) {
        has_sup |= (model.bitSupData[sup_idx_base_x + w].x & s_dom_y[w]) != 0;
      }
      keep_x = has_sup;
    }

    if (active_y) {
      const int sup_idx_base_y =
          cid * model.bitsup_per_constraint +
          (1 * model.max_dom_size + val) * model.bit_dom_int_size;
      bool has_sup = false;
      #pragma unroll
      for (int w = 0; w < model.bit_dom_int_size; ++w) {
        has_sup |= (model.bitSupData[sup_idx_base_y + w].y & s_dom_x[w]) != 0;
      }
      keep_y = has_sup;
    }

    if (active_x && !keep_x) {
      atomicAnd(&s_dom_x[word], ~mask);
    }
    if (active_y && !keep_y) {
      atomicAnd(&s_dom_y[word], ~mask);
    }
  }
  __syncthreads();

  // 3) 写回 global + 统计删值 + DWO（增量更新优化：避免全量 __popc 遍历）
  if (threadIdx.x == 0) {
    int del_x = 0, del_y = 0;

    // 单次遍历：删值 + 统计删除数
    // 使用 atomicAnd 返回值避免竞态：确保只统计本 block 实际删除的位
    for (int w = 0; w < model.bit_dom_int_size; ++w) {
      const u32 new_x = s_dom_x[w];
      // atomicAnd 返回操作前的旧值，保证我们只统计自己删除的位
      const u32 old_x = atomicAnd(
          reinterpret_cast<unsigned int*>(&dom_x[w]), new_x);
      const u32 removed_x = old_x & ~new_x;
      if (removed_x) {
        r.x_changed = true;
        const int del_count = __popc(removed_x);
        r.deletions += del_count;
        del_x += del_count;
      }

      const u32 new_y = s_dom_y[w];
      const u32 old_y = atomicAnd(
          reinterpret_cast<unsigned int*>(&dom_y[w]), new_y);
      const u32 removed_y = old_y & ~new_y;
      if (removed_y) {
        r.y_changed = true;
        const int del_count = __popc(removed_y);
        r.deletions += del_count;
        del_y += del_count;
      }
    }

    // 增量更新域大小（仅在有删值时，使用原子操作保证多 block 并发安全）
    if (del_x > 0) {
      atomicSub(reinterpret_cast<unsigned int*>(&model.d_cur_dom_size[x]),
                static_cast<unsigned int>(del_x));
    }
    if (del_y > 0) {
      atomicSub(reinterpret_cast<unsigned int*>(&model.d_cur_dom_size[y]),
                static_cast<unsigned int>(del_y));
    }

    // DWO 检测：使用原子读获取最新值（其他 block 可能已修改）
    const int final_size_x = static_cast<int>(atomicAdd(
        reinterpret_cast<unsigned int*>(&model.d_cur_dom_size[x]), 0u));
    const int final_size_y = static_cast<int>(atomicAdd(
        reinterpret_cast<unsigned int*>(&model.d_cur_dom_size[y]), 0u));
    if (final_size_x <= 0 || final_size_y <= 0) {
      r.inconsistent = true;
    }
  }
  __syncthreads();

  return r;
}

// ExecuteConstraintCheck_BpC_WarpPerWord - Warp-per-Word 优化版本
// 每个 warp (32 线程) 处理一个 domain word（32 个连续值），使用 __ballot_sync 替代 shared memory atomicAnd
// 适用于 bit_dom_int_size > 1 的大域场景
// 注意：global memory 写回仍需 atomicAnd（多 block 并发安全）
__device__
PropagateResult ExecuteConstraintCheck_BpC_WarpPerWord(
    int cid,
    const GModelData& model,
    u32* shared_mem) {

  PropagateResult r{};
  r.x_changed = r.y_changed = r.inconsistent = false;
  r.deletions = 0;

  const int2 scope = model.constraint_scopes[cid];
  const int x = scope.x;
  const int y = scope.y;
  if (x < 0 || y < 0) {
    return r;
  }

  u32* dom_x = model.bitDom + x * model.bit_dom_int_size;
  u32* dom_y = model.bitDom + y * model.bit_dom_int_size;

  // Warp 配置
  const int warp_id = threadIdx.x / 32;
  const int lane_id = threadIdx.x % 32;
  const int num_warps = blockDim.x / 32;  // 256/32 = 8

  // Shared memory 布局：
  // [0, bit_dom_int_size)          : new_dom_x
  // [bit_dom_int_size, 2*...)      : new_dom_y
  // [2*bit_dom_int_size, +8)       : warp_del_x (每 warp 1 个 int)
  // [2*bit_dom_int_size+8, +8)     : warp_del_y
  u32* new_dom_x = shared_mem;
  u32* new_dom_y = shared_mem + model.bit_dom_int_size;
  int* warp_del_x = reinterpret_cast<int*>(shared_mem + 2 * model.bit_dom_int_size);
  int* warp_del_y = warp_del_x + 8;

  // 初始化 warp 删值计数
  if (lane_id == 0 && warp_id < 8) {
    warp_del_x[warp_id] = 0;
    warp_del_y[warp_id] = 0;
  }
  __syncthreads();

  // 每个 warp 处理一个 word（条纹循环处理多个 word）
  for (int word = warp_id; word < model.bit_dom_int_size; word += num_warps) {
    const int value = word * 32 + lane_id;
    const u32 lane_mask = 1u << lane_id;

    // 读取当前域（所有 lane 读同一 word，合并访问）
    const u32 old_x = dom_x[word];
    const u32 old_y = dom_y[word];

    // 检查该 lane 对应的值是否活跃
    const bool valid_value = (value < model.max_dom_size);
    const bool active_x = valid_value && ((old_x & lane_mask) != 0);
    const bool active_y = valid_value && ((old_y & lane_mask) != 0);

    bool keep_x = true, keep_y = true;

    // X→Y 支持检查：value 在 X 中是否在 Y 的域中有支持
    if (active_x) {
      const int sup_idx_base =
          cid * model.bitsup_per_constraint +
          (0 * model.max_dom_size + value) * model.bit_dom_int_size;
      bool has_sup = false;
      for (int w = 0; w < model.bit_dom_int_size; ++w) {
        has_sup |= (model.bitSupData[sup_idx_base + w].x & dom_y[w]) != 0;
      }
      keep_x = has_sup;
    }

    // Y→X 支持检查
    if (active_y) {
      const int sup_idx_base =
          cid * model.bitsup_per_constraint +
          (1 * model.max_dom_size + value) * model.bit_dom_int_size;
      bool has_sup = false;
      for (int w = 0; w < model.bit_dom_int_size; ++w) {
        has_sup |= (model.bitSupData[sup_idx_base + w].y & dom_x[w]) != 0;
      }
      keep_y = has_sup;
    }

    // Warp 内收集 32 个决策
    // keep_mask 中：bit i = 1 表示 lane i 的值应保留（keep 或 inactive）
    u32 keep_mask_x = __ballot_sync(0xFFFFFFFF, keep_x || !active_x);
    u32 keep_mask_y = __ballot_sync(0xFFFFFFFF, keep_y || !active_y);

    // Lane 0 计算新域并统计删值
    if (lane_id == 0) {
      u32 result_x = old_x & keep_mask_x;
      u32 result_y = old_y & keep_mask_y;

      new_dom_x[word] = result_x;
      new_dom_y[word] = result_y;

      u32 removed_x = old_x & ~result_x;
      u32 removed_y = old_y & ~result_y;

      if (removed_x) warp_del_x[warp_id] += __popc(removed_x);
      if (removed_y) warp_del_y[warp_id] += __popc(removed_y);
    }
  }
  __syncthreads();

  // Thread 0 汇总结果并原子写回 global（多 block 并发安全）
  if (threadIdx.x == 0) {
    int total_del_x = 0, total_del_y = 0;
    for (int w = 0; w < num_warps && w < 8; ++w) {
      total_del_x += warp_del_x[w];
      total_del_y += warp_del_y[w];
    }

    // 原子写回域并检测变化（使用 atomicAnd 返回旧值精确统计本 block 删除的位）
    int del_x = 0, del_y = 0;
    for (int w = 0; w < model.bit_dom_int_size; ++w) {
      const u32 new_x = new_dom_x[w];
      const u32 old_x = atomicAnd(
          reinterpret_cast<unsigned int*>(&dom_x[w]), new_x);
      const u32 removed_x = old_x & ~new_x;
      if (removed_x) {
        r.x_changed = true;
        const int del_count = __popc(removed_x);
        r.deletions += del_count;
        del_x += del_count;
      }

      const u32 new_y = new_dom_y[w];
      const u32 old_y = atomicAnd(
          reinterpret_cast<unsigned int*>(&dom_y[w]), new_y);
      const u32 removed_y = old_y & ~new_y;
      if (removed_y) {
        r.y_changed = true;
        const int del_count = __popc(removed_y);
        r.deletions += del_count;
        del_y += del_count;
      }
    }

    // 增量更新域大小
    if (del_x > 0) {
      atomicSub(reinterpret_cast<unsigned int*>(&model.d_cur_dom_size[x]),
                static_cast<unsigned int>(del_x));
    }
    if (del_y > 0) {
      atomicSub(reinterpret_cast<unsigned int*>(&model.d_cur_dom_size[y]),
                static_cast<unsigned int>(del_y));
    }

    // DWO 检测
    const int final_size_x = static_cast<int>(atomicAdd(
        reinterpret_cast<unsigned int*>(&model.d_cur_dom_size[x]), 0u));
    const int final_size_y = static_cast<int>(atomicAdd(
        reinterpret_cast<unsigned int*>(&model.d_cur_dom_size[y]), 0u));
    if (final_size_x <= 0 || final_size_y <= 0) {
      r.inconsistent = true;
    }
  }
  __syncthreads();

  return r;
}

// ExecuteConstraintCheck_BpC - 调度函数
// 根据域大小选择最优实现：小域用 Legacy，大域用 Warp-per-Word
__device__
PropagateResult ExecuteConstraintCheck_BpC(
    int cid,
    const GModelData& model,
    u32* shared_mem) {

  // 小域（bit_dom_int_size == 1）走旧路径
  // 原因：只有 1 个 word，warp 内大部分 lane 处理无效值，收益不大
  if (model.bit_dom_int_size == 1) {
    return ExecuteConstraintCheck_BpC_Legacy(cid, model, shared_mem);
  }

  // 大域走 Warp-per-Word 路径
  return ExecuteConstraintCheck_BpC_WarpPerWord(cid, model, shared_mem);
}

// Bitmap GAC Kernel（单轮传播）
// Phase 1.2: 移除 current_level 参数（单层域）
__global__
void BitmapGACKernel(
    GModelData model,
    GACControl* control,
    u32* frontier_cur,
    u32* frontier_next,
    int bitmap_size_words) {

  extern __shared__ u32 shmem[];
  int local_word = 0;
  int local_offset = 0;

  while (true) {
    __shared__ int cid_shared;
    if (threadIdx.x == 0) {
      int cid = FetchNextCidFromBitmap(
          frontier_cur,
          bitmap_size_words,
          &control->scanner_index,
          local_word,
          local_offset);
      cid_shared = cid;
    }
    __syncthreads();

    int cid = cid_shared;
    if (cid < 0) {
      break;  // 当前轮 frontier 用完
    }

    // 检查约束 ID 有效性
    if (cid >= model.num_constraints) {
      break;
    }

    // Phase 1.2: 传播（移除 current_level 参数）
    PropagateResult r =
        ExecuteConstraintCheck_BpC(cid, model, shmem);

    if (threadIdx.x == 0) {
      if (r.deletions > 0) {
        atomicAdd(&control->deletions, (unsigned long long)r.deletions);
        const int2 scope = model.constraint_scopes[cid];
        if (r.x_changed) {
          PropagateVarToNextBitmap(scope.x, model, frontier_next);
        }
        if (r.y_changed) {
          PropagateVarToNextBitmap(scope.y, model, frontier_next);
        }
      }
      if (r.inconsistent) {
        atomicExch(&control->inconsistent_flag, 1);
      }
    }
    __syncthreads();

    if (control->inconsistent_flag) {
      break;  // 快速逃出
    }
  }
}

// ============================================================================
// 持久化 GAC Kernel（使用 Cooperative Groups 实现 GPU 内部多轮迭代）
// Phase 1.2: 移除 current_level 参数（单层域）
// ============================================================================
__global__
void PersistentGACKernel(
    GModelData model,
    PersistentGACControl* control,
    int bitmap_size_words,
    int max_iterations) {

  // 初始化 Cooperative Groups
  cg::grid_group grid = cg::this_grid();

  extern __shared__ u32 shmem[];
  int local_word = 0;
  int local_offset = 0;

  // 主循环：直到收敛或发现不一致
  for (int iter = 0; iter < max_iterations; ++iter) {

    // === 阶段 1：重置扫描索引 ===
    if (blockIdx.x == 0 && threadIdx.x == 0) {
      control->scanner_index = 0;
      control->iterations++;
    }
    grid.sync();

    // === 阶段 2：处理当前 frontier ===
    while (true) {
      __shared__ int cid_shared;
      if (threadIdx.x == 0) {
        int cid = FetchNextCidFromBitmap(
            control->frontier_A,
            bitmap_size_words,
            &control->scanner_index,
            local_word,
            local_offset);
        cid_shared = cid;
      }
      __syncthreads();

      int cid = cid_shared;
      if (cid < 0 || cid >= model.num_constraints) {
        break;  // 当前 block 没有更多任务
      }

      // Phase 1.2: 执行约束传播（移除 current_level 参数）
      PropagateResult r = ExecuteConstraintCheck_BpC(
          cid, model, shmem);

      if (threadIdx.x == 0) {
        if (r.deletions > 0) {
          atomicAdd(&control->deletions, (unsigned long long)r.deletions);
          const int2 scope = model.constraint_scopes[cid];
          if (r.x_changed) {
            PropagateVarToNextBitmap(scope.x, model, control->frontier_B);
          }
          if (r.y_changed) {
            PropagateVarToNextBitmap(scope.y, model, control->frontier_B);
          }
        }
        if (r.inconsistent) {
          atomicExch(&control->inconsistent_flag, 1);
        }
      }
      __syncthreads();

      if (control->inconsistent_flag) {
        break;
      }
    }

    // === 阶段 3：全局同步 + 检查不一致 ===
    grid.sync();

    if (control->inconsistent_flag) {
      break;  // 退出主循环
    }

    // === 阶段 4：检查 next frontier 是否为空 ===
    __shared__ int block_nonempty;
    if (threadIdx.x == 0) {
      block_nonempty = 0;
    }
    __syncthreads();

    // 每个 block 检查部分 words
    int words_per_block = (bitmap_size_words + gridDim.x - 1) / gridDim.x;
    int start = blockIdx.x * words_per_block;
    int end = min(start + words_per_block, bitmap_size_words);
    for (int w = start + (int)threadIdx.x; w < end; w += (int)blockDim.x) {
      if (control->frontier_B[w] != 0u) {
        atomicExch(&block_nonempty, 1);
        break;
      }
    }
    __syncthreads();

    if (threadIdx.x == 0) {
      atomicOr(&control->frontier_nonempty, block_nonempty);
    }

    grid.sync();

    // 检查是否收敛
    if (control->frontier_nonempty == 0) {
      if (blockIdx.x == 0 && threadIdx.x == 0) {
        control->converged_flag = 1;
      }
      break;  // 退出主循环
    }

    // === 阶段 5：Swap 双缓冲 ===
    if (blockIdx.x == 0 && threadIdx.x == 0) {
      u32* temp = control->frontier_A;
      control->frontier_A = control->frontier_B;
      control->frontier_B = temp;
      control->frontier_nonempty = 0;
    }

    grid.sync();

    // === 阶段 6：清空新的 next frontier ===
    int tid = blockIdx.x * blockDim.x + threadIdx.x;
    int total_threads = gridDim.x * blockDim.x;
    for (int w = tid; w < bitmap_size_words; w += total_threads) {
      control->frontier_B[w] = 0u;
    }

    grid.sync();
  }
}

// ============================================================================
// Batch AC-GPU: 辅助函数
// ============================================================================

// InitializeFrontierForVariable - 初始化单个变量的前沿
// strategy: 0 = FULL_ACTIVATION (全局 GAC), 1 = NEIGHBOR_ACTIVATION (增量 GAC)
__device__
void InitializeFrontierForVariable(
    const GModelData& model,
    int var_id,
    u32* frontier_bitmap,
    int bitmap_size_words,
    cg::grid_group& grid,
    int strategy = 0) {

  int tid = blockIdx.x * blockDim.x + threadIdx.x;
  int total_threads = gridDim.x * blockDim.x;

  // 参数校验
  if (var_id < 0 || var_id >= model.num_vars) {
    if (tid == 0) {
      printf("[InitializeFrontierForVariable] ERROR: Invalid var_id=%d (num_vars=%d)\n",
             var_id, model.num_vars);
    }
    return;
  }

  // 清空 frontier
  for (int w = tid; w < bitmap_size_words; w += total_threads) {
    frontier_bitmap[w] = 0u;
  }
  grid.sync();

  // 策略分支
  if (strategy == 1) {
    // NEIGHBOR_ACTIVATION: 只激活邻接约束（增量 GAC）
    int start = model.d_subscription_offset[var_id];
    int end = model.d_subscription_offset[var_id + 1];

    for (int i = start + tid; i < end; i += total_threads) {
      int cid = model.d_subscription[i].z;  // uint3 中 cid 在 z 分量
      int w = cid / 32;
      int b = cid % 32;
      atomicOr(&frontier_bitmap[w], 1u << b);
    }
  } else {
    // FULL_ACTIVATION: 激活所有约束（全局 GAC）
    for (int w = tid; w < bitmap_size_words; w += total_threads) {
      frontier_bitmap[w] = 0xFFFFFFFFu;
    }

    // 处理最后一个 word 的越界位
    if (tid == 0) {
      int num_constraints = model.num_constraints;
      int last_word = (num_constraints - 1) / 32;
      int last_bit = num_constraints % 32;
      if (last_bit != 0) {
        u32 mask = (1u << last_bit) - 1;
        frontier_bitmap[last_word] &= mask;
      }
    }
  }
  grid.sync();
}

// RunGACToFixpoint - 运行 GAC 到不动点
// 复用现有的 GAC 传播逻辑（ExecuteConstraintCheck_BpC 等）
__device__
void RunGACToFixpoint(
    const GModelData& model,
    BatchProbeControl* control,
    int bitmap_size_words,
    int max_iterations,
    u32* shared_mem,
    cg::grid_group& grid) {

  // 重置控制标志
  if (blockIdx.x == 0 && threadIdx.x == 0) {
    control->inconsistent_flag = 0;
    control->scanner_index = 0;
    control->deletions = 0;
    control->iterations = 0;
    control->converged_flag = 0;
    control->frontier_nonempty = 1;  // 假设 frontier 非空（由 InitializeFrontierForVariable 保证）
  }
  grid.sync();

  // 每个 block 的 local state（用于 FetchNextCidFromBitmap）
  int local_word = 0;
  int local_offset = 0;

  // GAC 主循环（与 PersistentGACKernel 相同）
  while (control->iterations < max_iterations) {
    // 调试：打印每轮迭代的 frontier 状态
    if (false && control->iterations < 5 && blockIdx.x == 0 && threadIdx.x == 0) {
      int num_frontier = 0;
      for (int w = 0; w < bitmap_size_words; ++w) {
        num_frontier += __popc(control->frontier_A[w]);
      }
      printf("    [Iter %d] frontier_A has %d constraints\\n",
             control->iterations, num_frontier);
    }
    grid.sync();

    // 检查收敛条件
    if (control->inconsistent_flag == 1) {
      break;  // DWO，提前退出
    }

    // 检查 frontier 是否为空
    if (control->frontier_nonempty == 0) {
      if (blockIdx.x == 0 && threadIdx.x == 0) {
        control->converged_flag = 1;
      }
      break;  // 收敛，退出
    }

    // 处理当前 frontier（与 PersistentGACKernel 相同）
    while (true) {
      __shared__ int cid_shared;
      if (threadIdx.x == 0) {
        int cid = FetchNextCidFromBitmap(
            control->frontier_A,
            bitmap_size_words,
            &control->scanner_index,
            local_word,
            local_offset);
        cid_shared = cid;
      }
      __syncthreads();

      int cid = cid_shared;
      if (cid < 0) {
        break;  // 当前 frontier 处理完毕
      }

      if (cid >= model.num_constraints) {
        break;
      }

      // 执行约束检查
      PropagateResult r = ExecuteConstraintCheck_BpC(cid, model, shared_mem);

      if (threadIdx.x == 0) {
        if (r.deletions > 0) {
          atomicAdd(&control->deletions, (unsigned long long)r.deletions);
          const int2 scope = model.constraint_scopes[cid];
          if (r.x_changed) {
            PropagateVarToNextBitmap(scope.x, model, control->frontier_B);
          }
          if (r.y_changed) {
            PropagateVarToNextBitmap(scope.y, model, control->frontier_B);
          }
        }
        if (r.inconsistent) {
          control->inconsistent_flag = 1;
        }
      }
      __syncthreads();

      if (control->inconsistent_flag == 1) {
        break;
      }
    }

    grid.sync();

    // 检查 next frontier 是否为空，并交换 frontier（只用一个线程）
    if (blockIdx.x == 0 && threadIdx.x == 0) {
      control->frontier_nonempty = 0;
      for (int w = 0; w < bitmap_size_words; ++w) {
        if (control->frontier_B[w] != 0) {
          control->frontier_nonempty = 1;
          break;
        }
      }
      control->scanner_index = 0;
      control->iterations++;

      // Swap frontiers（修复 bug：只用一个线程交换指针）
      u32* tmp = control->frontier_A;
      control->frontier_A = control->frontier_B;
      control->frontier_B = tmp;
    }
    grid.sync();

    // 清空 next frontier
    int tid = blockIdx.x * blockDim.x + threadIdx.x;
    int total_threads = gridDim.x * blockDim.x;
    for (int w = tid; w < bitmap_size_words; w += total_threads) {
      control->frontier_B[w] = 0u;
    }

    grid.sync();
  }
}

// CheckValueSupportBitSup - Cheap Precheck（安全的“早失败”版本）
// 仅用于快速检测必然 DWO 的情况：
//   - 对 singleton 赋值 (X=a)，若存在邻接约束 c(X,Y) 使得 a 在 Y 当前域中无任何支持
//     则 AC 会立刻删除 a，导致 X 域为空（DWO），可跳过完整 GAC。
//
// 重要：若所有邻接约束都“当前有支持”，仍不能跳过 GAC（支持可能在传播中被删掉）。
// 返回：true = 需要完整 GAC；false = 已确定 DWO（可短路）
__device__
bool CheckValueSupportBitSup(
    const GModelData& model,
    int assigned_var,
    int assigned_value,
    BatchProbeControl* control,
    cg::grid_group& grid) {

  int tid = blockIdx.x * blockDim.x + threadIdx.x;

  // 使用全局内存标志（避免 shared memory 跨 block 问题）
  if (tid == 0) {
    control->need_gac_flag = 1;  // 默认：需要完整 GAC
  }
  grid.sync();

  // 获取赋值变量的邻接约束
  int start = model.d_subscription_offset[assigned_var];
  int end = model.d_subscription_offset[assigned_var + 1];

  // 单线程检查（避免复杂的并行同步）
  if (tid == 0) {
    // 统计：Precheck 被评估一次（仅在启用时由调用点加计数）
    if (assigned_value < 0 || assigned_value >= model.max_dom_size) {
      control->need_gac_flag = 0;  // 非法输入，按 DWO 处理（防御性）
    } else {
      for (int idx = start; idx < end; ++idx) {
        const int cid = model.d_subscription[idx].z;
        if (cid < 0 || cid >= model.num_constraints) {
          continue;
        }

        const int2 scope = model.constraint_scopes[cid];
        int neighbor_var = -1;
        int var_pos = -1;  // 0=scope.x, 1=scope.y
        if (scope.x == assigned_var) {
          var_pos = 0;
          neighbor_var = scope.y;
        } else if (scope.y == assigned_var) {
          var_pos = 1;
          neighbor_var = scope.x;
        } else {
          // 防御性：subscription 异常（assigned_var 不在 scope 中）
          continue;
        }

        if (neighbor_var < 0 || neighbor_var >= model.num_vars) {
          continue;
        }

        // bitSupData 索引对齐 ExecuteConstraintCheck_BpC：
        //   base = cid * bitsup_per_constraint
        //   row  = (var_pos * max_dom_size + assigned_value) * bit_dom_int_size
        const int sup_idx_base =
            cid * model.bitsup_per_constraint +
            (var_pos * model.max_dom_size + assigned_value) * model.bit_dom_int_size;

        const u32* neighbor_dom =
            model.bitDom + neighbor_var * model.bit_dom_int_size;

        bool has_support = false;
        for (int w = 0; w < model.bit_dom_int_size; ++w) {
          const uint2 sup = model.bitSupData[sup_idx_base + w];
          const u32 support_word = (var_pos == 0) ? sup.x : sup.y;
          if ((support_word & neighbor_dom[w]) != 0u) {
            has_support = true;
            break;
          }
        }

        if (!has_support) {
          // 必然 DWO：assigned_var 的唯一值在该约束上无支持
          control->need_gac_flag = 0;
          break;
        }
      }
    }

  }

  grid.sync();

  return control->need_gac_flag == 1;
}

// ============================================================================
// Batch AC-GPU: 主 Kernel
// ============================================================================

__global__
void PersistentBatchProbeKernel(
    GModelData model,
    BatchProbeControl* control,
    int max_iterations_per_probe) {

  cg::grid_group grid = cg::this_grid();
  extern __shared__ u32 shmem[];

  const int tid = blockIdx.x * blockDim.x + threadIdx.x;
  const int total_threads = gridDim.x * blockDim.x;
  const int bitmap_size_words = control->bitmap_size_words;

  // 添加一个临时字段用于任务索引广播
  // 注意：这需要在 BatchProbeControl 中添加 current_processing_task 字段
  // 或者我们可以复用 iterations 字段（因为它会在 RunGACToFixpoint 中重置）

  // 主循环：处理所有任务
  while (true) {
    // [1] 获取任务（grid 级原子操作）
    // 只让 tid==0 的线程获取任务索引，然后通过 control 广播给所有线程
    if (tid == 0) {
      int idx = atomicAdd(&control->current_task_index, 1);
      control->iterations = idx;  // 临时存储任务索引（会在 RunGACToFixpoint 中重置）
    }
    grid.sync();

    int task_idx = control->iterations;

    if (task_idx >= control->num_tasks) {
      break;  // 所有任务处理完毕
    }

    // [2] 获取任务信息
    const ProbeTask& task = control->tasks[task_idx];
    const int var_id = task.var_id;
    const int value = task.value;

    // [3] 并行恢复快照（所有线程协作）
    // 3a. 恢复 bitDom
    const int total_dom_words = model.num_vars * model.bit_dom_int_size;
    for (int idx = tid; idx < total_dom_words; idx += total_threads) {
      model.bitDom[idx] = control->domain_snapshot[idx];
    }

    // 3b. 恢复 d_cur_dom_size（修复中优先级bug：probe 间一致性）
    for (int var = tid; var < model.num_vars; var += total_threads) {
      model.d_cur_dom_size[var] = control->dom_size_snapshot[var];
    }
    grid.sync();

    // [3.1] 调试：验证快照域状态（已禁用）
    if (false && task_idx < 4 && blockIdx.x == 0 && threadIdx.x == 0) {
      printf("[Debug] Task %d snapshot domain sizes: ", task_idx);
      for (int v = 0; v < model.num_vars; ++v) {
        printf("v%d=%d ", v, model.d_cur_dom_size[v]);
      }
      printf("\\n");
    }
    grid.sync();

    // [4] 单例赋值（只有一个线程执行）
    if (tid == 0) {
      // 找到值所在的 word 和 bit 位置
      const int word_idx = value / 32;
      const int bit_idx = value % 32;
      const int dom_base = var_id * model.bit_dom_int_size;

      // 清空该变量的所有 word
      for (int w = 0; w < model.bit_dom_int_size; ++w) {
        model.bitDom[dom_base + w] = 0u;
      }

      // 设置单个值
      model.bitDom[dom_base + word_idx] = (1u << bit_idx);

      // 更新域大小
      model.d_cur_dom_size[var_id] = 1;
    }
    grid.sync();

    // [5] 初始化 frontier（根据策略选择）
    InitializeFrontierForVariable(model, var_id, control->frontier_A,
                                   bitmap_size_words, grid,
                                   control->activation_strategy);  // 传入策略

    // [5.1] 调试：统计激活的约束数和域状态（已禁用）
    if (false && task_idx < 4 && blockIdx.x == 0 && threadIdx.x == 0) {
      int num_active = 0;
      for (int w = 0; w < bitmap_size_words; ++w) {
        num_active += __popc(control->frontier_A[w]);
      }
      printf("[Debug] Task %d (var=%d, val=%d): Activated %d constraints (strategy=%d)\\n",
             task_idx, var_id, value, num_active, control->activation_strategy);

      // 打印所有变量的域大小（赋值后）
      printf("  Domain sizes after assignment: ");
      for (int v = 0; v < model.num_vars; ++v) {
        printf("v%d=%d ", v, model.d_cur_dom_size[v]);
      }
      printf("\\n");
    }
    grid.sync();

    // [5.5] P0-2: Cheap Precheck（安全的“早失败”）
    // 若 (X=a) 在任一邻接约束上无支持，则必然 DWO，可跳过完整 GAC。
    if (tid == 0) {
      atomicAdd(&control->precheck_count, 1ULL);
    }
    grid.sync();

    bool need_gac = CheckValueSupportBitSup(model, var_id, value, control, grid);
    if (!need_gac) {
      if (tid == 0) {
        atomicAdd(&control->short_circuit_count, 1ULL);
        control->results[task_idx] = false;  // DWO
      }
      grid.sync();
      continue;  // 跳过 GAC，处理下一个任务
    }

    // [6] GAC 到不动点（只在需要时执行）
    RunGACToFixpoint(model, control, bitmap_size_words,
                     max_iterations_per_probe, shmem, grid);

    // [7] 记录结果
    if (tid == 0) {
      // results[i] = true 表示一致（该值有效）
      // results[i] = false 表示 DWO（该值应删除）
      control->results[task_idx] = (control->inconsistent_flag == 0);

      // 调试：输出 GAC 结果（已禁用）
      if (false && task_idx < 4) {
        printf("[Debug] Task %d result: %s (deletions=%llu, iterations=%d)\\n",
               task_idx,
               control->inconsistent_flag == 0 ? "CONSISTENT" : "DWO",
               control->deletions,
               control->iterations);

        // 打印最终域大小
        printf("  Final domain sizes: ");
        for (int v = 0; v < model.num_vars; ++v) {
          printf("v%d=%d ", v, model.d_cur_dom_size[v]);
        }
        printf("\\n");
      }
    }

    grid.sync();
  }
}

// ============================================================================
// Batch-2 (Micro-Batch): 单 block / 多 probe 并行
// ============================================================================

// InitializeFrontierForVariable_BlockSync - 单 block 版本 frontier 初始化
// strategy: 0 = FULL_ACTIVATION, 1 = NEIGHBOR_ACTIVATION
__device__
void InitializeFrontierForVariable_BlockSync(
    const GModelData& model,
    int var_id,
    u32* frontier_bitmap,
    int bitmap_size_words,
    int strategy) {

  const int tid = threadIdx.x;
  const int total_threads = blockDim.x;

  // 清空 frontier
  for (int w = tid; w < bitmap_size_words; w += total_threads) {
    frontier_bitmap[w] = 0u;
  }
  __syncthreads();

  if (var_id < 0 || var_id >= model.num_vars) {
    return;
  }

  if (strategy == 1) {
    // NEIGHBOR_ACTIVATION: 只激活邻接约束
    const int start = model.d_subscription_offset[var_id];
    const int end = model.d_subscription_offset[var_id + 1];

    for (int i = start + tid; i < end; i += total_threads) {
      const int cid = model.d_subscription[i].z;
      const int w = cid / 32;
      const int b = cid % 32;
      atomicOr(&frontier_bitmap[w], 1u << b);
    }
  } else {
    // FULL_ACTIVATION: 激活所有约束
    for (int w = tid; w < bitmap_size_words; w += total_threads) {
      frontier_bitmap[w] = 0xFFFFFFFFu;
    }

    // 处理最后一个 word 的越界位
    if (tid == 0) {
      const int num_constraints = model.num_constraints;
      const int last_word = (num_constraints - 1) / 32;
      const int last_bit = num_constraints % 32;
      if (last_bit != 0) {
        const u32 mask = (1u << last_bit) - 1;
        frontier_bitmap[last_word] &= mask;
      }
    }
  }

  __syncthreads();
}

// ExecuteConstraintCheck_BpC_Workspace_Legacy - 旧版本（条纹分配，atomicAnd）
// 用于 bit_dom_int_size == 1 的小域场景
__device__
PropagateResult ExecuteConstraintCheck_BpC_Workspace_Legacy(
    int cid,
    const GModelData& model,
    WorldWorkspace* ws,
    u32* shared_mem) {

  PropagateResult r{};
  r.x_changed = r.y_changed = r.inconsistent = false;
  r.deletions = 0;

  const int2 scope = model.constraint_scopes[cid];
  const int x = scope.x;
  const int y = scope.y;
  if (x < 0 || y < 0) {
    return r;
  }

  u32* dom_x = ws->bitDom + x * model.bit_dom_int_size;
  u32* dom_y = ws->bitDom + y * model.bit_dom_int_size;

  u32* s_dom_x = shared_mem;
  u32* s_dom_y = shared_mem + model.bit_dom_int_size;

  // 1) load 到 shared memory
  for (int w = threadIdx.x; w < model.bit_dom_int_size; w += blockDim.x) {
    s_dom_x[w] = dom_x[w];
    s_dom_y[w] = dom_y[w];
  }
  __syncthreads();

  // 2) 每个线程处理若干 value
  for (int val = threadIdx.x; val < model.max_dom_size; val += blockDim.x) {
    const int word = val / 32;
    const int bit = val % 32;
    if (word >= model.bit_dom_int_size) break;
    const u32 mask = 1u << bit;

    const bool active_x = (s_dom_x[word] & mask) != 0u;
    const bool active_y = (s_dom_y[word] & mask) != 0u;

    bool keep_x = true, keep_y = true;

    if (active_x) {
      const int sup_idx_base_x =
          cid * model.bitsup_per_constraint +
          (0 * model.max_dom_size + val) * model.bit_dom_int_size;
      bool has_sup = false;
      #pragma unroll
      for (int w = 0; w < model.bit_dom_int_size; ++w) {
        has_sup |= (model.bitSupData[sup_idx_base_x + w].x & s_dom_y[w]) != 0;
      }
      keep_x = has_sup;
    }

    if (active_y) {
      const int sup_idx_base_y =
          cid * model.bitsup_per_constraint +
          (1 * model.max_dom_size + val) * model.bit_dom_int_size;
      bool has_sup = false;
      #pragma unroll
      for (int w = 0; w < model.bit_dom_int_size; ++w) {
        has_sup |= (model.bitSupData[sup_idx_base_y + w].y & s_dom_x[w]) != 0;
      }
      keep_y = has_sup;
    }

    if (active_x && !keep_x) {
      atomicAnd(&s_dom_x[word], ~mask);
    }
    if (active_y && !keep_y) {
      atomicAnd(&s_dom_y[word], ~mask);
    }
  }
  __syncthreads();

  // 3) 写回私有域 + 统计删值 + DWO（增量更新优化：避免每个 word 都 __popc(new_*)）
  if (threadIdx.x == 0) {
    // 增量更新：从现有 size 开始，减去删除的位数
    int size_x = ws->d_cur_dom_size[x];
    int size_y = ws->d_cur_dom_size[y];

    for (int w = 0; w < model.bit_dom_int_size; ++w) {
      const u32 old_x = dom_x[w];
      const u32 new_x = s_dom_x[w];
      const u32 removed_x = old_x & ~new_x;
      if (removed_x) {
        dom_x[w] = new_x;
        r.x_changed = true;
        const int del_count = __popc(removed_x);
        r.deletions += del_count;
        size_x -= del_count;  // 增量更新
      }

      const u32 old_y = dom_y[w];
      const u32 new_y = s_dom_y[w];
      const u32 removed_y = old_y & ~new_y;
      if (removed_y) {
        dom_y[w] = new_y;
        r.y_changed = true;
        const int del_count = __popc(removed_y);
        r.deletions += del_count;
        size_y -= del_count;  // 增量更新
      }
    }

    // 只在有变化时写回
    if (r.x_changed) {
      ws->d_cur_dom_size[x] = size_x;
    }
    if (r.y_changed) {
      ws->d_cur_dom_size[y] = size_y;
    }

    if (size_x == 0 || size_y == 0) {
      r.inconsistent = true;
    }
  }
  __syncthreads();

  return r;
}

// ExecuteConstraintCheck_BpC_Workspace_WarpPerWord - Warp-per-Word 优化版本
// 每个 warp 处理一个 domain word（32 个连续值），使用 __ballot_sync 替代 atomicAnd
// 适用于 bit_dom_int_size > 1 的大域场景
__device__
PropagateResult ExecuteConstraintCheck_BpC_Workspace_WarpPerWord(
    int cid,
    const GModelData& model,
    WorldWorkspace* ws,
    u32* shared_mem) {

  PropagateResult r{};
  r.x_changed = r.y_changed = r.inconsistent = false;
  r.deletions = 0;

  const int2 scope = model.constraint_scopes[cid];
  const int x = scope.x;
  const int y = scope.y;
  if (x < 0 || y < 0) {
    return r;
  }

  u32* dom_x = ws->bitDom + x * model.bit_dom_int_size;
  u32* dom_y = ws->bitDom + y * model.bit_dom_int_size;

  // Warp 配置
  const int warp_id = threadIdx.x / 32;
  const int lane_id = threadIdx.x % 32;
  const int num_warps = blockDim.x / 32;  // 256/32 = 8

  // Shared memory 布局：
  // [0, bit_dom_int_size)          : new_dom_x
  // [bit_dom_int_size, 2*...)      : new_dom_y
  // [2*bit_dom_int_size, +8)       : warp_del_x (每 warp 1 个 int)
  // [2*bit_dom_int_size+8, +8)     : warp_del_y
  u32* new_dom_x = shared_mem;
  u32* new_dom_y = shared_mem + model.bit_dom_int_size;
  int* warp_del_x = reinterpret_cast<int*>(shared_mem + 2 * model.bit_dom_int_size);
  int* warp_del_y = warp_del_x + 8;

  // 初始化 warp 删值计数
  if (lane_id == 0 && warp_id < 8) {
    warp_del_x[warp_id] = 0;
    warp_del_y[warp_id] = 0;
  }
  __syncthreads();

  // 每个 warp 处理一个 word（条纹循环处理多个 word）
  for (int word = warp_id; word < model.bit_dom_int_size; word += num_warps) {
    const int value = word * 32 + lane_id;
    const u32 lane_mask = 1u << lane_id;

    // 读取当前域（所有 lane 读同一 word，合并访问）
    const u32 old_x = dom_x[word];
    const u32 old_y = dom_y[word];

    // 检查该 lane 对应的值是否活跃
    const bool valid_value = (value < model.max_dom_size);
    const bool active_x = valid_value && ((old_x & lane_mask) != 0);
    const bool active_y = valid_value && ((old_y & lane_mask) != 0);

    bool keep_x = true, keep_y = true;

    // X→Y 支持检查：value 在 X 中是否在 Y 的域中有支持
    if (active_x) {
      const int sup_idx_base =
          cid * model.bitsup_per_constraint +
          (0 * model.max_dom_size + value) * model.bit_dom_int_size;
      bool has_sup = false;
      for (int w = 0; w < model.bit_dom_int_size; ++w) {
        has_sup |= (model.bitSupData[sup_idx_base + w].x & dom_y[w]) != 0;
      }
      keep_x = has_sup;
    }

    // Y→X 支持检查
    if (active_y) {
      const int sup_idx_base =
          cid * model.bitsup_per_constraint +
          (1 * model.max_dom_size + value) * model.bit_dom_int_size;
      bool has_sup = false;
      for (int w = 0; w < model.bit_dom_int_size; ++w) {
        has_sup |= (model.bitSupData[sup_idx_base + w].y & dom_x[w]) != 0;
      }
      keep_y = has_sup;
    }

    // Warp 内收集 32 个决策
    // keep_mask 中：bit i = 1 表示 lane i 的值应保留（keep 或 inactive）
    u32 keep_mask_x = __ballot_sync(0xFFFFFFFF, keep_x || !active_x);
    u32 keep_mask_y = __ballot_sync(0xFFFFFFFF, keep_y || !active_y);

    // Lane 0 计算新域并统计删值
    if (lane_id == 0) {
      u32 result_x = old_x & keep_mask_x;
      u32 result_y = old_y & keep_mask_y;

      new_dom_x[word] = result_x;
      new_dom_y[word] = result_y;

      u32 removed_x = old_x & ~result_x;
      u32 removed_y = old_y & ~result_y;

      if (removed_x) warp_del_x[warp_id] += __popc(removed_x);
      if (removed_y) warp_del_y[warp_id] += __popc(removed_y);
    }
  }
  __syncthreads();

  // Thread 0 汇总结果并写回
  if (threadIdx.x == 0) {
    int total_del_x = 0, total_del_y = 0;
    for (int w = 0; w < num_warps && w < 8; ++w) {
      total_del_x += warp_del_x[w];
      total_del_y += warp_del_y[w];
    }

    // 写回域并检测变化
    int size_x = ws->d_cur_dom_size[x];
    int size_y = ws->d_cur_dom_size[y];

    for (int w = 0; w < model.bit_dom_int_size; ++w) {
      if (dom_x[w] != new_dom_x[w]) {
        dom_x[w] = new_dom_x[w];
        r.x_changed = true;
      }
      if (dom_y[w] != new_dom_y[w]) {
        dom_y[w] = new_dom_y[w];
        r.y_changed = true;
      }
    }

    if (total_del_x > 0) {
      size_x -= total_del_x;
      ws->d_cur_dom_size[x] = size_x;
      r.deletions += total_del_x;
    }
    if (total_del_y > 0) {
      size_y -= total_del_y;
      ws->d_cur_dom_size[y] = size_y;
      r.deletions += total_del_y;
    }

    if (size_x == 0 || size_y == 0) {
      r.inconsistent = true;
    }
  }
  __syncthreads();

  return r;
}

// ExecuteConstraintCheck_BpC_Workspace - 调度函数
// 根据域大小选择最优实现：小域用 Legacy，大域用 Warp-per-Word
__device__
PropagateResult ExecuteConstraintCheck_BpC_Workspace(
    int cid,
    const GModelData& model,
    WorldWorkspace* ws,
    u32* shared_mem) {

  // 小域（bit_dom_int_size == 1）走旧路径
  // 原因：只有 1 个 word，warp 内大部分 lane 处理无效值，收益不大
  if (model.bit_dom_int_size == 1) {
    return ExecuteConstraintCheck_BpC_Workspace_Legacy(cid, model, ws, shared_mem);
  }

  // 大域走 Warp-per-Word 路径
  return ExecuteConstraintCheck_BpC_Workspace_WarpPerWord(cid, model, ws, shared_mem);
}

// CheckValueSupportBitSup_BlockSync - Cheap Precheck（安全"早失败"）
// 返回：true=需要完整 GAC；false=已确定 DWO（可短路）
__device__
bool CheckValueSupportBitSup_BlockSync(
    const GModelData& model,
    const WorldWorkspace* ws,
    int assigned_var,
    int assigned_value) {

  __shared__ int need_gac;
  if (threadIdx.x == 0) {
    need_gac = 1;
    if (assigned_var < 0 || assigned_var >= model.num_vars ||
        assigned_value < 0 || assigned_value >= model.max_dom_size) {
      need_gac = 0;  // 防御性：非法输入视为 DWO
    }
  }
  __syncthreads();

  if (threadIdx.x == 0 && need_gac) {
    const int start = model.d_subscription_offset[assigned_var];
    const int end = model.d_subscription_offset[assigned_var + 1];

    for (int idx = start; idx < end; ++idx) {
      const int cid = model.d_subscription[idx].z;
      if (cid < 0 || cid >= model.num_constraints) {
        continue;
      }

      const int2 scope = model.constraint_scopes[cid];
      int neighbor_var = -1;
      int var_pos = -1;  // 0=scope.x, 1=scope.y
      if (scope.x == assigned_var) {
        var_pos = 0;
        neighbor_var = scope.y;
      } else if (scope.y == assigned_var) {
        var_pos = 1;
        neighbor_var = scope.x;
      } else {
        continue;
      }

      if (neighbor_var < 0 || neighbor_var >= model.num_vars) {
        continue;
      }

      const int sup_idx_base =
          cid * model.bitsup_per_constraint +
          (var_pos * model.max_dom_size + assigned_value) * model.bit_dom_int_size;

      const u32* neighbor_dom =
          ws->bitDom + neighbor_var * model.bit_dom_int_size;

      bool has_support = false;
      for (int w = 0; w < model.bit_dom_int_size; ++w) {
        const uint2 sup = model.bitSupData[sup_idx_base + w];
        const u32 support_word = (var_pos == 0) ? sup.x : sup.y;
        if ((support_word & neighbor_dom[w]) != 0u) {
          has_support = true;
          break;
        }
      }

      if (!has_support) {
        need_gac = 0;
        break;
      }
    }
  }

  __syncthreads();
  return need_gac != 0;
}

// RunGACToFixpoint_BlockSync - 单 block 版本 GAC 到不动点
__device__
void RunGACToFixpoint_BlockSync(
    const GModelData& model,
    WorldWorkspace* ws,
    int bitmap_size_words,
    int max_iterations,
    u32* shared_mem) {

  __shared__ u32* frontier_cur;
  __shared__ u32* frontier_next;

  if (threadIdx.x == 0) {
    ws->inconsistent_flag = 0;
    ws->scanner_index = 0;
    ws->deletions = 0;
    ws->iterations = 0;
    ws->frontier_nonempty = 1;
    frontier_cur = ws->frontier_A;
    frontier_next = ws->frontier_B;
  }
  __syncthreads();

  int local_word = 0;
  int local_offset = 0;

  while (ws->iterations < max_iterations) {
    if (ws->inconsistent_flag == 1) {
      break;
    }

    if (ws->frontier_nonempty == 0) {
      break;
    }

    while (true) {
      __shared__ int cid_shared;
      if (threadIdx.x == 0) {
        const int cid = FetchNextCidFromBitmap(
            frontier_cur, bitmap_size_words, &ws->scanner_index,
            local_word, local_offset);
        cid_shared = cid;
      }
      __syncthreads();

      const int cid = cid_shared;
      if (cid < 0 || cid >= model.num_constraints) {
        break;
      }

      PropagateResult r = ExecuteConstraintCheck_BpC_Workspace(
          cid, model, ws, shared_mem);

      if (threadIdx.x == 0) {
        if (r.deletions > 0) {
          ws->deletions += static_cast<unsigned long long>(r.deletions);
          const int2 scope = model.constraint_scopes[cid];
          if (r.x_changed) {
            PropagateVarToNextBitmap(scope.x, model, frontier_next);
          }
          if (r.y_changed) {
            PropagateVarToNextBitmap(scope.y, model, frontier_next);
          }
        }
        if (r.inconsistent) {
          ws->inconsistent_flag = 1;
        }
      }
      __syncthreads();

      if (ws->inconsistent_flag == 1) {
        break;
      }
    }

    __syncthreads();

    if (threadIdx.x == 0) {
      ws->frontier_nonempty = 0;
      for (int w = 0; w < bitmap_size_words; ++w) {
        if (frontier_next[w] != 0u) {
          ws->frontier_nonempty = 1;
          break;
        }
      }
      ws->scanner_index = 0;
      ws->iterations++;

      // Swap frontiers（单线程）
      u32* tmp = frontier_cur;
      frontier_cur = frontier_next;
      frontier_next = tmp;
    }
    __syncthreads();

    // 清空 next frontier
    for (int w = threadIdx.x; w < bitmap_size_words; w += blockDim.x) {
      frontier_next[w] = 0u;
    }
    __syncthreads();
  }
}

// Batch2ProbeKernel_MicroBatch - 每个 block 处理一个 probe（batch_size 个 block）
__global__
void Batch2ProbeKernel_MicroBatch(
    const GModelData model,
    const u32* domain_snapshot,
    const int* dom_size_snapshot,
    const ProbeTask* tasks,
    bool* results,
    WorldWorkspace* workspaces,
    int batch_size,
    int max_iterations_per_probe,
    int activation_strategy,
    int enable_precheck) {

  const int local_task_id = blockIdx.x;
  if (local_task_id >= batch_size) return;

  WorldWorkspace* ws = &workspaces[local_task_id];
  const ProbeTask task = tasks[local_task_id];

  // 重置 workspace 标量状态（避免 precheck/早退路径残留上一次的值）
  if (threadIdx.x == 0) {
    ws->inconsistent_flag = 0;
    ws->scanner_index = 0;
    ws->deletions = 0;
    ws->iterations = 0;
    ws->frontier_nonempty = 0;
  }
  __syncthreads();

  // [1] 恢复快照到 workspace
  const int total_dom_words = model.num_vars * model.bit_dom_int_size;
  for (int idx = threadIdx.x; idx < total_dom_words; idx += blockDim.x) {
    ws->bitDom[idx] = domain_snapshot[idx];
  }
  for (int v = threadIdx.x; v < model.num_vars; v += blockDim.x) {
    ws->d_cur_dom_size[v] = dom_size_snapshot[v];
  }

  // 清空 frontiers
  const int bitmap_size_words = (model.num_constraints + 31) / 32;
  for (int w = threadIdx.x; w < bitmap_size_words; w += blockDim.x) {
    ws->frontier_A[w] = 0u;
    ws->frontier_B[w] = 0u;
  }
  __syncthreads();

  // [2] 单例赋值
  if (threadIdx.x == 0) {
    const int var_id = task.var_id;
    const int value = task.value;
    if (var_id < 0 || var_id >= model.num_vars ||
        value < 0 || value >= model.max_dom_size) {
      ws->inconsistent_flag = 1;
    } else {
      const int word_idx = value / 32;
      const int bit_idx = value % 32;
      const int dom_base = var_id * model.bit_dom_int_size;
      for (int w = 0; w < model.bit_dom_int_size; ++w) {
        ws->bitDom[dom_base + w] = 0u;
      }
      if (word_idx < model.bit_dom_int_size) {
        ws->bitDom[dom_base + word_idx] = (1u << bit_idx);
      }
      ws->d_cur_dom_size[var_id] = 1;
      ws->inconsistent_flag = 0;
    }
  }
  __syncthreads();

  if (ws->inconsistent_flag == 1) {
    if (threadIdx.x == 0) {
      results[local_task_id] = false;
    }
    return;
  }

  // [3] 初始化 frontier（Block-sync）
  InitializeFrontierForVariable_BlockSync(
      model, task.var_id, ws->frontier_A, bitmap_size_words, activation_strategy);

  // [4] Cheap Precheck（可选）
  if (enable_precheck) {
    const bool need_gac = CheckValueSupportBitSup_BlockSync(
        model, ws, task.var_id, task.value);
    if (!need_gac) {
      if (threadIdx.x == 0) {
        results[local_task_id] = false;
      }
      return;
    }
  }
  __syncthreads();

  // [5] RunGACToFixpoint（Block-sync）
  extern __shared__ u32 shmem[];
  RunGACToFixpoint_BlockSync(
      model, ws, bitmap_size_words, max_iterations_per_probe, shmem);

  // [6] 记录结果
  if (threadIdx.x == 0) {
    results[local_task_id] = (ws->inconsistent_flag == 0);
  }
}

}  // namespace

// ============================================================================
// Batch AC-GPU: Kernel Wrapper（Host 端调用）
// ============================================================================

void LaunchPersistentBatchProbeKernelWrapper(
    GModelData model_data,
    BatchProbeControl* control,
    int max_iterations_per_probe) {

  // 检查设备是否支持 Cooperative Launch
  int device;
  cudaGetDevice(&device);

  int supportsCoopLaunch = 0;
  cudaDeviceGetAttribute(&supportsCoopLaunch,
                          cudaDevAttrCooperativeLaunch,
                          device);

  if (!supportsCoopLaunch) {
    LOG(ERROR) << "Device does not support Cooperative Launch!";
    LOG(ERROR) << "Batch Probe Kernel requires Cooperative Groups support.";
    throw std::runtime_error(
        "Device does not support Cooperative Launch (required for Batch Probe)");
  }

  // 计算 shared memory 大小
  // Legacy: 2 * bit_dom_int_size (s_dom_x, s_dom_y)
  // Warp-per-Word: 2 * bit_dom_int_size (new_dom_x, new_dom_y) + 64 (warp_del_x/y)
  int shared_mem_bytes = 2 * model_data.bit_dom_int_size * sizeof(u32) + 64;

  // 计算 grid 和 block 大小（基于设备能力）
  const int block_size = 256;  // 固定 block size

  // 查询最大可驻留 blocks
  int max_blocks_per_sm = 0;
  cudaError_t err = cudaOccupancyMaxActiveBlocksPerMultiprocessor(
      &max_blocks_per_sm,
      PersistentBatchProbeKernel,
      block_size,
      shared_mem_bytes);

  if (err != cudaSuccess) {
    LOG(ERROR) << "Failed to query occupancy: " << cudaGetErrorString(err);
    throw std::runtime_error(
        std::string("Failed to query occupancy: ") + cudaGetErrorString(err));
  }

  cudaDeviceProp prop;
  cudaGetDeviceProperties(&prop, device);

  int max_grid_size = max_blocks_per_sm * prop.multiProcessorCount;
  int desired_blocks = 128;  // 期望的 blocks 数
  int grid_size = std::min(desired_blocks, max_grid_size);

  LOG(INFO) << "Cooperative Launch config:";
  LOG(INFO) << "  max_blocks_per_sm: " << max_blocks_per_sm;
  LOG(INFO) << "  multiProcessorCount: " << prop.multiProcessorCount;
  LOG(INFO) << "  max_grid_size: " << max_grid_size;
  LOG(INFO) << "  grid_size (actual): " << grid_size;
  LOG(INFO) << "  block_size: " << block_size;

  // 准备 kernel 参数
  void* args[] = {
    &model_data,
    &control,
    &max_iterations_per_probe
  };

  // 启动 Cooperative Kernel
  err = cudaLaunchCooperativeKernel(
      (void*)PersistentBatchProbeKernel,
      dim3(grid_size),
      dim3(block_size),
      args,
      shared_mem_bytes);

  if (err != cudaSuccess) {
    LOG(ERROR) << "Failed to launch PersistentBatchProbeKernel: "
               << cudaGetErrorString(err);
    throw std::runtime_error(
        std::string("Failed to launch PersistentBatchProbeKernel: ") +
        cudaGetErrorString(err));
  }

  VLOG(2) << "PersistentBatchProbeKernel launched successfully "
          << "(grid=" << grid_size << ", block=" << block_size
          << ", shmem=" << shared_mem_bytes << " bytes)";
}

// ============================================================================
// Batch-2 (Micro-Batch): Kernel Wrapper（Host 端调用）
// ============================================================================

void LaunchBatch2MicroBatchKernelWrapper(
    GModelData model_data,
    const u32* domain_snapshot,
    const int* dom_size_snapshot,
    const ProbeTask* tasks,
    bool* results,
    WorldWorkspace* workspaces,
    int batch_size,
    int max_iterations_per_probe,
    int activation_strategy,
    int enable_precheck) {

  const int block_size = 256;
  // Warp-per-Word 需要额外 64 bytes (warp_del_x/y)
  const int shared_mem_bytes = 2 * model_data.bit_dom_int_size * sizeof(u32) + 64;

  Batch2ProbeKernel_MicroBatch<<<dim3(batch_size), dim3(block_size), shared_mem_bytes>>>(
      model_data,
      domain_snapshot,
      dom_size_snapshot,
      tasks,
      results,
      workspaces,
      batch_size,
      max_iterations_per_probe,
      activation_strategy,
      enable_precheck);

  cudaError_t err = cudaGetLastError();
  if (err != cudaSuccess) {
    LOG(ERROR) << "Failed to launch Batch2ProbeKernel_MicroBatch: "
               << cudaGetErrorString(err);
    throw std::runtime_error(
        std::string("Failed to launch Batch2ProbeKernel_MicroBatch: ") +
        cudaGetErrorString(err));
  }
}

// ============================================================================
// Batch-2 Stage 2: Persistent Blocks Kernel
// - 每个 block 持续从全局任务队列拉取任务（atomicAdd）
// - 直到所有任务完成才退出
// - 减少 kernel launch 开销
// ============================================================================

__global__
void Batch2ProbeKernel_PersistentBlocks(
    const GModelData model,
    Batch2PersistentControl* control) {

  // Shared memory for constraint check（与 Batch-2 Micro-Batch 一致）
  extern __shared__ u32 shared_mem[];

  const int block_id = blockIdx.x;
  WorldWorkspace* ws = &control->workspaces[block_id];

  const int num_vars = model.num_vars;
  const int bit_dom_int_size = model.bit_dom_int_size;
  const int total_dom_words = num_vars * bit_dom_int_size;
  const int bitmap_size_words = (model.num_constraints + 31) / 32;

  const int chunk_size = control->chunk_size;

  // 持久循环：不断从全局队列批量拉取任务
  while (true) {
    // [1] 批量获取任务（只有 thread 0 做原子操作）
    __shared__ int shared_base_task_id;
    if (threadIdx.x == 0) {
      shared_base_task_id = atomicAdd(control->task_cursor, chunk_size);
    }
    __syncthreads();

    const int base_task_id = shared_base_task_id;

    // 退出条件：没有更多任务
    if (base_task_id >= control->num_tasks) {
      break;
    }

    // 处理 chunk 中的每个任务
    const int chunk_end = min(base_task_id + chunk_size, control->num_tasks);
    for (int task_id = base_task_id; task_id < chunk_end; ++task_id) {
    const ProbeTask task = control->tasks[task_id];

    // [2] 重置 workspace 标量状态
    if (threadIdx.x == 0) {
      ws->inconsistent_flag = 0;
      ws->scanner_index = 0;
      ws->deletions = 0;
      ws->iterations = 0;
      ws->frontier_nonempty = 0;
    }
    __syncthreads();

    // [3] 恢复快照到 workspace
    for (int idx = threadIdx.x; idx < total_dom_words; idx += blockDim.x) {
      ws->bitDom[idx] = control->domain_snapshot[idx];
    }
    for (int v = threadIdx.x; v < num_vars; v += blockDim.x) {
      ws->d_cur_dom_size[v] = control->dom_size_snapshot[v];
    }

    // 清空 frontiers
    for (int w = threadIdx.x; w < bitmap_size_words; w += blockDim.x) {
      ws->frontier_A[w] = 0u;
      ws->frontier_B[w] = 0u;
    }
    __syncthreads();

    // [4] 单例赋值
    if (threadIdx.x == 0) {
      const int var_id = task.var_id;
      const int value = task.value;
      if (var_id < 0 || var_id >= num_vars ||
          value < 0 || value >= model.max_dom_size) {
        ws->inconsistent_flag = 1;
      } else {
        // 清空域
        for (int w = 0; w < bit_dom_int_size; ++w) {
          ws->bitDom[var_id * bit_dom_int_size + w] = 0u;
        }
        // 设置单例值
        const int word_idx = value / 32;
        const int bit_idx = value % 32;
        ws->bitDom[var_id * bit_dom_int_size + word_idx] = (1u << bit_idx);
        ws->d_cur_dom_size[var_id] = 1;
      }
    }
    __syncthreads();

    // 如果单例赋值失败，跳过 GAC
    if (ws->inconsistent_flag == 1) {
      if (threadIdx.x == 0) {
        control->results[task_id] = false;
        // P0-1 NEW: 设置三态状态
        if (control->task_status != nullptr) {
          control->task_status[task_id] = 1;  // kDWO
        }
      }
      __syncthreads();
      continue;  // 处理下一个任务
    }

    // [4.5] Cheap Precheck（与 Stage 1 一致）
    if (control->enable_precheck) {
      const bool need_gac = CheckValueSupportBitSup_BlockSync(
          model, ws, task.var_id, task.value);
      if (!need_gac) {
        if (threadIdx.x == 0) {
          control->results[task_id] = false;
          // P0-1 NEW: 设置三态状态
          if (control->task_status != nullptr) {
            control->task_status[task_id] = 1;  // kDWO
          }
          if (control->precheck_short_circuit_count != nullptr) {
            atomicAdd(control->precheck_short_circuit_count, 1ULL);
          }
        }
        __syncthreads();
        continue;  // 跳过 GAC，处理下一个任务
      }
    }
    __syncthreads();

    // [5] 初始化 Frontier（并行版本）
    if (control->activation_strategy == 0) {
      // FULL_ACTIVATION: 所有线程并行设置
      for (int w = threadIdx.x; w < bitmap_size_words; w += blockDim.x) {
        ws->frontier_A[w] = 0xFFFFFFFFu;
      }
      if (threadIdx.x == 0) {
        const int last_bit = model.num_constraints % 32;
        if (last_bit != 0) {
          ws->frontier_A[bitmap_size_words - 1] &= ((1u << last_bit) - 1);
        }
        ws->frontier_nonempty = 1;
      }
    } else {
      // NEIGHBOR_ACTIVATION: 所有线程并行激活邻接约束（使用 atomicOr）
      const int var_id = task.var_id;
      const int start = model.d_subscription_offset[var_id];
      const int end = model.d_subscription_offset[var_id + 1];

      // 所有线程并行处理邻接约束
      for (int i = start + threadIdx.x; i < end; i += blockDim.x) {
        const int cid = model.d_subscription[i].z;
        atomicOr(&ws->frontier_A[cid / 32], 1u << (cid % 32));
      }

      // thread 0 设置 frontier_nonempty 标志
      if (threadIdx.x == 0) {
        ws->frontier_nonempty = (end > start) ? 1 : 0;
      }
    }
    __syncthreads();

    // [6] GAC 传播（直接调用 RunGACToFixpoint_BlockSync）
    RunGACToFixpoint_BlockSync(
        model, ws, bitmap_size_words,
        control->max_iterations_per_probe, shared_mem);

    // [7] 写回结果和统计
    if (threadIdx.x == 0) {
      const bool is_dwo = (ws->inconsistent_flag == 1);
      control->results[task_id] = !is_dwo;

      // P0-1 NEW: 设置三态状态
      if (control->task_status != nullptr) {
        if (is_dwo) {
          control->task_status[task_id] = 1;  // kDWO: 可删值
        } else if (control->max_iterations_per_probe > 0 &&
                   ws->iterations >= control->max_iterations_per_probe) {
          // 预算超限但没有 DWO：UNKNOWN（不删值，保守处理）
          control->task_status[task_id] = 2;  // kUNKNOWN
        } else {
          control->task_status[task_id] = 0;  // kOK: 正常收敛
        }
      }

      // 写入 per-task 统计（如果启用）
      if (control->task_iterations != nullptr) {
        control->task_iterations[task_id] = ws->iterations;
      }
      if (control->task_deletions != nullptr) {
        control->task_deletions[task_id] = ws->deletions;
      }
    }
    __syncthreads();

    }  // end for (chunk)
  }  // end while (true)
}

// ============================================================================
// Batch-2 Stage 2: Persistent Blocks Wrapper（Host 端调用）
// ============================================================================

void LaunchBatch2PersistentBlocksKernelWrapper(
    GModelData model_data,
    Batch2PersistentControl* control) {

  const int block_size = 256;
  const int num_blocks = control->num_blocks;
  // Warp-per-Word 需要额外 64 bytes (warp_del_x/y)
  const int shared_mem_bytes = 2 * model_data.bit_dom_int_size * sizeof(u32) + 64;

  Batch2ProbeKernel_PersistentBlocks<<<dim3(num_blocks), dim3(block_size), shared_mem_bytes>>>(
      model_data,
      control);

  cudaError_t err = cudaGetLastError();
  if (err != cudaSuccess) {
    LOG(ERROR) << "Failed to launch Batch2ProbeKernel_PersistentBlocks: "
               << cudaGetErrorString(err);
    throw std::runtime_error(
        std::string("Failed to launch Batch2ProbeKernel_PersistentBlocks: ") +
        cudaGetErrorString(err));
  }
}

// ============================================================================
// GPU 资源管理
// ============================================================================

void GModel::InitializeGPUResources() {
  if (d_gac_control) return;  // 已经初始化

  bitmap_size_words = (num_constraints + 31) / 32;

  cudaError_t err;
  // 使用统一内存，Jetson 上 CPU/GPU 零拷贝访问
  err = cudaMallocManaged(&d_gac_control, sizeof(GACControl));
  if (err != cudaSuccess) {
    throw std::runtime_error(
        "[GModel::InitializeGPUResources] Failed to allocate d_gac_control: " +
        std::string(cudaGetErrorString(err)));
  }

  err = cudaMallocManaged(&d_queue_bitmap_A, bitmap_size_words * sizeof(u32));
  if (err != cudaSuccess) {
    cudaFree(d_gac_control);
    d_gac_control = nullptr;
    throw std::runtime_error(
        "[GModel::InitializeGPUResources] Failed to allocate d_queue_bitmap_A: " +
        std::string(cudaGetErrorString(err)));
  }

  err = cudaMallocManaged(&d_queue_bitmap_B, bitmap_size_words * sizeof(u32));
  if (err != cudaSuccess) {
    cudaFree(d_gac_control);
    cudaFree(d_queue_bitmap_A);
    d_gac_control = nullptr;
    d_queue_bitmap_A = nullptr;
    throw std::runtime_error(
        "[GModel::InitializeGPUResources] Failed to allocate d_queue_bitmap_B: " +
        std::string(cudaGetErrorString(err)));
  }
}

void GModel::FreeGPUResources() {
  if (d_gac_control) {
    cudaFree(d_gac_control);
    d_gac_control = nullptr;
  }
  if (d_queue_bitmap_A) {
    cudaFree(d_queue_bitmap_A);
    d_queue_bitmap_A = nullptr;
  }
  if (d_queue_bitmap_B) {
    cudaFree(d_queue_bitmap_B);
    d_queue_bitmap_B = nullptr;
  }
}

// ============================================================================
// 持久化 Kernel 资源管理
// ============================================================================

void GModel::InitializeGPUResources_Persistent() {
  if (d_persistent_control) return;  // 已经初始化

  // 先初始化基础资源（bitmap 等）
  InitializeGPUResources();

  // 分配持久化控制结构（统一内存）
  cudaError_t err = cudaMallocManaged(&d_persistent_control,
                                       sizeof(PersistentGACControl));
  if (err != cudaSuccess) {
    throw std::runtime_error(
        "[GModel::InitializeGPUResources_Persistent] "
        "Failed to allocate d_persistent_control: " +
        std::string(cudaGetErrorString(err)));
  }
}

void GModel::FreeGPUResources_Persistent() {
  // 释放持久化控制结构
  if (d_persistent_control) {
    cudaFree(d_persistent_control);
    d_persistent_control = nullptr;
  }

  // 释放基础资源
  FreeGPUResources();
}

GModelData GModel::GetGModelDataView() const {
  GModelData md;
  md.num_vars = num_vars;
  md.num_constraints = num_constraints;
  md.max_dom_size = max_dom_size;
  md.bit_dom_int_size = bit_dom_int_size;
  md.bit_doms_int_size = bit_doms_int_size;
  md.bitsup_per_constraint = bitsup_per_constraint;

  md.bitDom = bitDom;
  md.d_cur_dom_size = d_cur_dom_size;
  md.bitSupData = bitSupData;
  md.constraint_scopes = constraint_scopes;
  md.d_subscription = d_subscription;
  md.d_subscription_offset = d_subscription_offset;
  return md;
}

// ============================================================================
// 新版 Bitmap GAC 传播（EnforceGAC）
// ============================================================================

GacStats GModel::EnforceGAC(bool verbose, int assigned_var) {
  GacStats stats;

  if (!bitDom || !bitSupData || !constraint_scopes) {
    std::cerr << "[GModel::EnforceGAC] Missing GPU data structures" << std::endl;
    return stats;
  }

  // 初始化 GPU 资源（首次调用时分配）
  InitializeGPUResources();

  // Phase 1.2: 保存域快照（用于 Trail 记录）
  std::vector<u32> domain_snapshot;
  if (trail_) {
    const int total_words = num_vars * bit_dom_int_size;
    domain_snapshot.resize(total_words);
    std::memcpy(domain_snapshot.data(), bitDom, total_words * sizeof(u32));
  }

  // 1. 初始化 GAC 控制块（统一内存，直接写入）
  d_gac_control->inconsistent_flag = 0;
  d_gac_control->scanner_index = 0;
  d_gac_control->deletions = 0;
  d_gac_control->iterations = 0;

  // 2. 清空 next frontier（统一内存，用 memset）
  memset(d_queue_bitmap_B, 0, bitmap_size_words * sizeof(u32));

  // 3. 初始化 current frontier（统一内存，直接写入）
  memset(d_queue_bitmap_A, 0, bitmap_size_words * sizeof(u32));

  if (assigned_var < 0) {
    // 初始 GAC：所有约束激活
    for (int cid = 0; cid < num_constraints; ++cid) {
      if (constraint_scopes[cid].x < 0) continue;
      int w = cid / 32;
      int b = cid % 32;
      d_queue_bitmap_A[w] |= (1u << b);
    }
  } else {
    // 增量 GAC：只激活 assigned_var 邻接约束
    const int start = d_subscription_offset[assigned_var];
    const int end = d_subscription_offset[assigned_var + 1];
    for (int i = start; i < end; ++i) {
      int cid = d_subscription[i].z;
      int w = cid / 32;
      int b = cid % 32;
      d_queue_bitmap_A[w] |= (1u << b);
    }
  }

  GModelData md = GetGModelDataView();

  bool done = false;
  while (!done) {
    // 每轮迭代（统一内存，直接写入）
    ++d_gac_control->iterations;
    d_gac_control->scanner_index = 0;

    // 启动 Kernel
    int blocks = std::min(bitmap_size_words, 128);
    int threads = std::min(max_dom_size, 256);
    // Warp-per-Word 实现要求 blockDim.x 为 32 的倍数（避免 partial warp）
    if (bit_dom_int_size > 1) {
      threads = ((threads + 31) / 32) * 32;
      threads = std::min(threads, 256);
    }
    // Warp-per-Word 需要额外 64 bytes (warp_del_x/y, 8+8 ints)
    size_t shmem_bytes = 2 * bit_dom_int_size * sizeof(u32) + 64;

    // Phase 1.2: 移除 current_level 参数
    BitmapGACKernel<<<blocks, threads, shmem_bytes>>>(
        md,
        d_gac_control,
        d_queue_bitmap_A,
        d_queue_bitmap_B,
        bitmap_size_words);

    cudaError_t err = cudaDeviceSynchronize();
    if (err != cudaSuccess) {
      std::cerr << "[GAC-Bitmap] Kernel failed: " << cudaGetErrorString(err)
                << std::endl;
      break;
    }

    // 4. 检查 inconsistent / 下一轮是否为空（统一内存，直接读取）
    if (d_gac_control->inconsistent_flag) {
      stats.inconsistent = true;
      done = true;
    } else {
      // 检查 d_queue_bitmap_B 是否全 0（统一内存，直接读取）
      bool non_empty = false;
      for (int i = 0; i < bitmap_size_words; ++i) {
        if (d_queue_bitmap_B[i] != 0u) {
          non_empty = true;
          break;
        }
      }

      if (!non_empty) {
        done = true;  // 达到不动点
      } else {
        // swap A/B，清空新的 B（统一内存，用 memset）
        std::swap(d_queue_bitmap_A, d_queue_bitmap_B);
        memset(d_queue_bitmap_B, 0, bitmap_size_words * sizeof(u32));
      }
    }
  }

  stats.deletions = static_cast<int>(d_gac_control->deletions);
  stats.iterations = d_gac_control->iterations;

  // Phase 1.2: 记录域修改到 Trail（比较快照和当前域）
  if (trail_ && !domain_snapshot.empty()) {
    for (int var = 0; var < num_vars; ++var) {
      for (int word = 0; word < bit_dom_int_size; ++word) {
        const int idx = GetBitDomIndex(var, word);
        const u32 old_bits = domain_snapshot[idx];
        const u32 new_bits = bitDom[idx];
        if (old_bits != new_bits) {
          // 域发生了变化，记录旧值
          trail_->RecordDomainChange(var, word, old_bits);
        }
      }
    }
  }

  if (verbose) {
    std::cout << "[GAC-Bitmap] iterations=" << stats.iterations
              << " deletions=" << stats.deletions
              << " inconsistent=" << (stats.inconsistent ? "true" : "false")
              << std::endl;
  }

  return stats;
}

// ============================================================================
// 持久化 Kernel GAC 传播（Cooperative Groups）
// ============================================================================

GacStats GModel::EnforceGAC_Persistent(bool verbose, int assigned_var) {
  GacStats stats;

  if (!bitDom || !bitSupData || !constraint_scopes) {
    std::cerr << "[GModel::EnforceGAC_Persistent] Missing GPU data structures"
              << std::endl;
    return stats;
  }

  // 初始化持久化 GPU 资源（首次调用时分配）
  InitializeGPUResources_Persistent();

  // Phase 1.2: 保存域快照（用于 Trail 记录）
  std::vector<u32> domain_snapshot;
  if (trail_) {
    const int total_words = num_vars * bit_dom_int_size;
    domain_snapshot.resize(total_words);
    std::memcpy(domain_snapshot.data(), bitDom, total_words * sizeof(u32));
  }

  // ========================================================================
  // 1. 检查设备是否支持 Cooperative Launch
  // ========================================================================
  int deviceId = 0;
  int supportsCoopLaunch = 0;
  cudaDeviceGetAttribute(&supportsCoopLaunch,
                         cudaDevAttrCooperativeLaunch, deviceId);

  if (!supportsCoopLaunch) {
    if (verbose) {
      std::cerr << "[GModel::EnforceGAC_Persistent] "
                << "Device does not support cooperative launch, "
                << "falling back to EnforceGAC" << std::endl;
    }
    return EnforceGAC(verbose, assigned_var);
  }

  // ========================================================================
  // 2. 初始化持久化控制块（统一内存，直接写入）
  // ========================================================================
  d_persistent_control->inconsistent_flag = 0;
  d_persistent_control->scanner_index = 0;
  d_persistent_control->deletions = 0;
  d_persistent_control->iterations = 0;
  d_persistent_control->converged_flag = 0;
  d_persistent_control->frontier_nonempty = 0;
  d_persistent_control->frontier_A = d_queue_bitmap_A;
  d_persistent_control->frontier_B = d_queue_bitmap_B;

  // ========================================================================
  // 3. 初始化 bitmap frontier
  // ========================================================================
  memset(d_queue_bitmap_A, 0, bitmap_size_words * sizeof(u32));
  memset(d_queue_bitmap_B, 0, bitmap_size_words * sizeof(u32));

  if (assigned_var < 0) {
    // 初始 GAC：所有约束激活
    for (int cid = 0; cid < num_constraints; ++cid) {
      if (constraint_scopes[cid].x < 0) continue;
      int w = cid / 32;
      int b = cid % 32;
      d_queue_bitmap_A[w] |= (1u << b);
    }
  } else {
    // 增量 GAC：只激活 assigned_var 邻接约束
    const int start = d_subscription_offset[assigned_var];
    const int end = d_subscription_offset[assigned_var + 1];
    for (int i = start; i < end; ++i) {
      int cid = d_subscription[i].z;
      int w = cid / 32;
      int b = cid % 32;
      d_queue_bitmap_A[w] |= (1u << b);
    }
  }

  // ========================================================================
  // 4. 计算 Cooperative Launch 参数
  // ========================================================================
  int threadsPerBlock = std::min(max_dom_size, 256);
  // Warp-per-Word 实现要求 blockDim.x 为 32 的倍数（避免 partial warp）
  if (bit_dom_int_size > 1) {
    threadsPerBlock = ((threadsPerBlock + 31) / 32) * 32;
    threadsPerBlock = std::min(threadsPerBlock, 256);
  }
  // Warp-per-Word 需要额外 64 bytes (warp_del_x/y, 8+8 ints)
  size_t sharedMemBytes = 2 * bit_dom_int_size * sizeof(u32) + 64;

  // 查询最大可驻留 blocks
  int maxBlocksPerSM = 0;
  cudaOccupancyMaxActiveBlocksPerMultiprocessor(
      &maxBlocksPerSM,
      PersistentGACKernel,
      threadsPerBlock,
      sharedMemBytes);

  cudaDeviceProp prop;
  cudaGetDeviceProperties(&prop, deviceId);

  int maxGridSize = maxBlocksPerSM * prop.multiProcessorCount;
  int desiredBlocks = std::min(bitmap_size_words, 128);
  int actualGridSize = std::min(desiredBlocks, maxGridSize);

  if (verbose) {
    std::cout << "[GAC-Persistent] Cooperative Launch config:" << std::endl;
    std::cout << "  Blocks: " << actualGridSize
              << " (max: " << maxGridSize << ")" << std::endl;
    std::cout << "  Threads per block: " << threadsPerBlock << std::endl;
    std::cout << "  Shared memory: " << sharedMemBytes << " bytes" << std::endl;
  }

  // ========================================================================
  // 5. 使用 Cooperative Launch 启动 Kernel
  // ========================================================================
  GModelData md = GetGModelDataView();
  int maxIterations = num_vars * 10;  // 安全限制

  // Phase 1.2: 移除 current_level 参数
  void* args[] = {
    &md,
    &d_persistent_control,
    &bitmap_size_words,
    &maxIterations
  };

  cudaError_t err = cudaLaunchCooperativeKernel(
      (void*)PersistentGACKernel,
      dim3(actualGridSize),
      dim3(threadsPerBlock),
      args,
      sharedMemBytes);

  if (err != cudaSuccess) {
    std::cerr << "[GAC-Persistent] Cooperative launch failed: "
              << cudaGetErrorString(err) << std::endl;

    // 如果是 grid 太大导致失败，尝试缩小
    if (err == cudaErrorCooperativeLaunchTooLarge) {
      std::cerr << "  Grid size too large, reducing and retrying..."
                << std::endl;
      actualGridSize = maxGridSize / 2;
      if (actualGridSize > 0) {
        err = cudaLaunchCooperativeKernel(
            (void*)PersistentGACKernel,
            dim3(actualGridSize),
            dim3(threadsPerBlock),
            args,
            sharedMemBytes);
      }
    }

    // 如果仍然失败，回退到非持久化版本
    if (err != cudaSuccess) {
      std::cerr << "  Falling back to EnforceGAC" << std::endl;
      return EnforceGAC(verbose, assigned_var);
    }
  }

  // ========================================================================
  // 6. 等待 Kernel 完成
  // ========================================================================
  err = cudaDeviceSynchronize();
  if (err != cudaSuccess) {
    std::cerr << "[GAC-Persistent] Kernel execution failed: "
              << cudaGetErrorString(err) << std::endl;
    return stats;
  }

  // ========================================================================
  // 7. 读取结果（统一内存，直接访问）
  // ========================================================================
  stats.deletions = static_cast<int>(d_persistent_control->deletions);
  stats.iterations = d_persistent_control->iterations;
  stats.inconsistent = (d_persistent_control->inconsistent_flag != 0);

  // Phase 1.2: 记录域修改到 Trail（比较快照和当前域）
  if (trail_ && !domain_snapshot.empty()) {
    for (int var = 0; var < num_vars; ++var) {
      for (int word = 0; word < bit_dom_int_size; ++word) {
        const int idx = GetBitDomIndex(var, word);
        const u32 old_bits = domain_snapshot[idx];
        const u32 new_bits = bitDom[idx];
        if (old_bits != new_bits) {
          // 域发生了变化，记录旧值
          trail_->RecordDomainChange(var, word, old_bits);
        }
      }
    }
  }

  if (verbose) {
    std::cout << "[GAC-Persistent] iterations=" << stats.iterations
              << " deletions=" << stats.deletions
              << " inconsistent=" << (stats.inconsistent ? "true" : "false")
              << " converged=" << (d_persistent_control->converged_flag ? "true" : "false")
              << std::endl;
  }

  return stats;
}

// ============================================================================
// Multi-Level Search Support Implementation
// ============================================================================
// Phase 1.2: Trail 层级管理（替换旧的多级域拷贝）
// ============================================================================

void GModel::NewLevel() {
  if (!trail_) {
    throw std::runtime_error("[GModel::NewLevel] Trail is null!");
  }
  trail_->NewLevel();  // O(1) 操作
}

void GModel::BacktrackTo(int target_level) {
  if (!trail_) {
    throw std::runtime_error("[GModel::BacktrackTo] Trail is null!");
  }

  // Phase 1.2: 手动恢复域（从 Trail 读取条目）
  // 计算需要撤销的 trail 范围
  int current_level = trail_->CurrentLevel();
  if (target_level >= current_level) {
    return;  // 无需回溯
  }

  // 获取 trail entries 并恢复
  const auto* entries = trail_->GetGPUTrailPointer();
  const auto* level_markers = trail_->GetGPULevelMarkersPointer();
  int trail_size = trail_->Size();

  int target_trail_size;
  if (target_level == -1) {
    target_trail_size = 0;
  } else if (target_level + 1 < current_level + 1) {
    target_trail_size = level_markers[target_level + 1];
  } else {
    return;
  }

  // Phase 1.2: 从后向前恢复域，并收集被修改的变量
  std::vector<bool> modified_vars(num_vars, false);

  for (int i = trail_size - 1; i >= target_trail_size; --i) {
    const auto& entry = entries[i];
    if (entry.type == TrailEntry::DOMAIN_CHANGE) {
      int idx = GetBitDomIndex(entry.var_id, entry.word_index);
      bitDom[idx] = entry.old_bits;
      modified_vars[entry.var_id] = true;
    }
  }

  // 重新计算所有被修改变量的域大小（批量处理，避免重复计算）
  for (int var = 0; var < num_vars; ++var) {
    if (modified_vars[var]) {
      int dom_size = 0;
      for (int w = 0; w < bit_dom_int_size; ++w) {
        int word_idx = GetBitDomIndex(var, w);
        dom_size += __builtin_popcount(bitDom[word_idx]);
      }
      d_cur_dom_size[var] = dom_size;
    }
  }

  // 委托给 Trail 更新层级指针
  trail_->BacktrackTo(target_level);

  // 回溯赋值栈（如果需要）
  while (assigned_size_ > target_level) {
    --assigned_size_;
  }
}

int GModel::GetCurrentLevel() const {
  if (!trail_) {
    throw std::runtime_error("[GModel::GetCurrentLevel] Trail is null!");
  }
  return trail_->CurrentLevel();
}

bool GModel::AssignValue(int var, int value) {
  if (var < 0 || var >= num_vars) {
    throw std::runtime_error(
        "[GModel::AssignValue] Invalid var: " + std::to_string(var));
  }


  if (value < 0 || value >= max_dom_size) {
    throw std::runtime_error(
        "[GModel::AssignValue] Invalid value: " + std::to_string(value));
  }

  // Phase 1.2: 记录域修改到 Trail（在修改前）
  const int base_idx = GetBitDomIndex(var, 0);
  if (trail_) {
    for (int word = 0; word < bit_dom_int_size; ++word) {
      u32 old_bits = bitDom[base_idx + word];
      if (old_bits != 0) {  // 只记录非空的 word
        trail_->RecordDomainChange(var, word, old_bits);
      }
    }
  }

  // 清空该变量的域
  for (int word = 0; word < bit_dom_int_size; ++word) {
    bitDom[base_idx + word] = 0u;
  }

  // 设置唯一值
  const int word = value / kBitsPerWord;
  const int bit = value % kBitsPerWord;
  bitDom[base_idx + word] = (1u << bit);

  // 更新域大小
  d_cur_dom_size[var] = 1;

  return true;
}

bool GModel::RemoveValue(int var, int value) {
  if (var < 0 || var >= num_vars) {
    throw std::runtime_error(
        "[GModel::RemoveValue] Invalid var: " + std::to_string(var));
  }


  if (value < 0 || value >= max_dom_size) {
    return false;  // Value out of range, nothing to remove
  }

  // 清除指定位
  const int word = value / kBitsPerWord;
  const int bit = value % kBitsPerWord;
  const int idx = GetBitDomIndex(var, word);

  const u32 old_value = bitDom[idx];
  const u32 new_value = old_value & ~(1u << bit);

  if (old_value == new_value) {
    return false;  // Value was already removed
  }

  // Phase 1.2: 记录域修改到 Trail
  if (trail_) {
    trail_->RecordDomainChange(var, word, old_value);
  }

  bitDom[idx] = new_value;

  // 更新域大小（需要重新计算）
  int dom_size = 0;
  const int base_idx = GetBitDomIndex(var, 0);
  for (int w = 0; w < bit_dom_int_size; ++w) {
    dom_size += Popcount(bitDom[base_idx + w]);
  }
  d_cur_dom_size[var] = dom_size;

  return dom_size > 0;  // Return false if domain becomes empty
}

int GModel::GetDomainSize(int var) const {
  if (var < 0 || var >= num_vars) {
    throw std::runtime_error(
        "[GModel::GetDomainSize] Invalid var: " + std::to_string(var));
  }


  if (!d_cur_dom_size) {
    throw std::runtime_error(
        "[GModel::GetDomainSize] d_cur_dom_size is null!");
  }

  return d_cur_dom_size[var];
}

// 批量恢复域大小（用于 Batch AC 状态恢复）
void GModel::RestoreDomainSizes(const int* sizes, int count) {
  if (count != num_vars) {
    throw std::runtime_error(
        "[GModel::RestoreDomainSizes] Count mismatch: expected " +
        std::to_string(num_vars) + ", got " + std::to_string(count));
  }

  if (!d_cur_dom_size) {
    throw std::runtime_error(
        "[GModel::RestoreDomainSizes] d_cur_dom_size is null!");
  }

  // 拷贝域大小数组（使用 Default 以支持 managed 内存）
  cudaError_t err = cudaMemcpy(d_cur_dom_size, sizes,
                                count * sizeof(int),
                                cudaMemcpyDefault);
  if (err != cudaSuccess) {
    throw std::runtime_error(
        "[GModel::RestoreDomainSizes] cudaMemcpy failed: " +
        std::string(cudaGetErrorString(err)));
  }

  VLOG(3) << "Restored domain sizes for " << count << " variables";
}

// ============================================================================
// 求解器辅助方法实现
// ============================================================================

int GModel::GetMinDomainVar() const {

  int min_var = -1;
  int min_size = max_dom_size + 1;

  for (int var = 0; var < num_vars; ++var) {
    const int size = d_cur_dom_size[var];
    // 跳过已赋值的变量（size == 1）和空域（size == 0）
    if (size > 1 && size < min_size) {
      min_size = size;
      min_var = var;
    }
  }

  return min_var;  // -1 表示所有变量都已赋值或域为空
}

bool GModel::IsAssigned(int var) const {
  if (var < 0 || var >= num_vars) {
    throw std::runtime_error(
        "[GModel::IsAssigned] Invalid var: " + std::to_string(var));
  }


  return d_cur_dom_size[var] == 1;
}

bool GModel::IsFullyAssigned() const {

  for (int var = 0; var < num_vars; ++var) {
    if (d_cur_dom_size[var] != 1) {
      return false;
    }
  }

  return true;
}

int GModel::GetAssignedValue(int var) const {
  if (var < 0 || var >= num_vars) {
    throw std::runtime_error(
        "[GModel::GetAssignedValue] Invalid var: " + std::to_string(var));
  }


  const int size = d_cur_dom_size[var];
  if (size != 1) {
    return -1;  // 未赋值或域为空
  }

  // 查找域中唯一的值（使用 __builtin_ctz 优化）
  const int base_idx = GetBitDomIndex(var, 0);
  for (int word = 0; word < bit_dom_int_size; ++word) {
    const u32 bits = bitDom[base_idx + word];
    if (bits != 0u) {
      const int bit = __builtin_ctz(bits);  // Count Trailing Zeros，O(1)
      const int value = word * kBitsPerWord + bit;
      return (value < max_dom_size) ? value : -1;
    }
  }

  return -1;  // 不应该到达这里
}

int GModel::GetFirstValue(int var) const {
  if (var < 0 || var >= num_vars) {
    throw std::runtime_error(
        "[GModel::GetFirstValue] Invalid var: " + std::to_string(var));
  }


  // 使用 __builtin_ctz 快速查找第一个值
  const int base_idx = GetBitDomIndex(var, 0);
  for (int word = 0; word < bit_dom_int_size; ++word) {
    const u32 bits = bitDom[base_idx + word];
    if (bits != 0u) {
      const int bit = __builtin_ctz(bits);  // Count Trailing Zeros，O(1)
      const int value = word * kBitsPerWord + bit;
      return (value < max_dom_size) ? value : -1;
    }
  }

  return -1;  // 域为空
}

int GModel::GetNextValue(int var, int value) const {
  if (var < 0 || var >= num_vars) {
    throw std::runtime_error(
        "[GModel::GetNextValue] Invalid var: " + std::to_string(var));
  }


  if (value < 0 || value >= max_dom_size - 1) {
    return -1;  // 没有更多值
  }

  // 使用 __builtin_ctz 快速查找下一个值
  const int base_idx = GetBitDomIndex(var, 0);
  const int start_value = value + 1;
  const int start_word = start_value / kBitsPerWord;
  const int start_bit = start_value % kBitsPerWord;

  // 检查起始 word 的剩余位
  if (start_word < bit_dom_int_size) {
    // 屏蔽掉 start_bit 之前的位
    u32 bits = bitDom[base_idx + start_word] >> start_bit;
    if (bits != 0u) {
      const int bit = __builtin_ctz(bits);
      const int v = start_word * kBitsPerWord + start_bit + bit;
      return (v < max_dom_size) ? v : -1;
    }
  }

  // 检查后续 word
  for (int word = start_word + 1; word < bit_dom_int_size; ++word) {
    const u32 bits = bitDom[base_idx + word];
    if (bits != 0u) {
      const int bit = __builtin_ctz(bits);
      const int v = word * kBitsPerWord + bit;
      return (v < max_dom_size) ? v : -1;
    }
  }

  return -1;  // 没有更多值
}

// ============================================================================
// Batch-3A: 约束聚合 Kernel 实现
// ============================================================================

// ExecuteConstraintCheck_Aggregated - 对多个 world 检查同一约束
// bitSup 已加载到 shared memory，对 world_mask 中的每个活跃 world 执行检查
// ============================================================================
// Warp-per-World 版本：G 个 warp 并行处理 G 个 world（支持多 Block）
// shared_del 布局：[num_warps * 2 * MAX_BIT_DOM_INT_SIZE]
// 每个 warp 有独立的删除缓冲区
// ============================================================================
__device__
void ExecuteConstraintCheck_Aggregated(
    int cid,
    u32 local_world_mask,       // 局部 world 掩码（0 ~ G-1 位）
    int block_world_start,      // 本 block 的 world 起始索引
    int block_world_count,      // 本 block 的 world 数量
    const GModelData& model,
    Batch3AControl* control,
    const uint2* shmem_bitsup,  // bitSup 在 shared memory
    u32* shared_del) {          // 布局: [num_warps * 2 * bit_dom_int_size]

  const int warp_id = threadIdx.x / 32;
  const int lane_id = threadIdx.x % 32;
  const int num_warps = blockDim.x / 32;  // G warps for G*32 threads

  const int2 scope = model.constraint_scopes[cid];
  const int x = scope.x;
  const int y = scope.y;
  const int bit_dom_int_size = model.bit_dom_int_size;
  const int max_dom_size = model.max_dom_size;

  // 每个 warp 处理不同的 world（按 warp_id 分配）
  // 本 block 只处理 block_world_count 个 world
  for (int local_w = warp_id; local_w < block_world_count; local_w += num_warps) {
    // 检查该 world 是否需要处理（局部掩码）
    if ((local_world_mask & (1u << local_w)) == 0) continue;

    // 转换为全局 world 索引
    const int global_w = block_world_start + local_w;
    WorldWorkspace* ws = &control->workspaces[global_w];

    // 跳过已经不一致的 world
    if (ws->inconsistent_flag == 1) {
      continue;
    }

    // 获取该 world 的私有域
    u32* bitDom = ws->bitDom;
    int* dom_size = ws->d_cur_dom_size;

    const int x_base = x * bit_dom_int_size;
    const int y_base = y * bit_dom_int_size;

    // 该 warp 的删除缓冲区偏移
    u32* warp_del = shared_del + warp_id * 2 * bit_dom_int_size;

    // Warp 内线程并行清空删除缓冲区（支持 bit_dom_int_size > 16）
    for (int i = lane_id; i < 2 * bit_dom_int_size; i += 32) {
      warp_del[i] = 0u;
    }
    __syncwarp();

    // Warp 内线程分工：每个 lane 处理一个 word（最多 32 words）
    // 对于 bit_dom_int_size <= 32 的情况，每个 lane 最多处理一个 word
    for (int word = lane_id; word < bit_dom_int_size; word += 32) {
      const u32 x_word = bitDom[x_base + word];
      const u32 y_word = bitDom[y_base + word];

      u32 del_x = 0u;
      u32 del_y = 0u;

      // 检查 x 域中每个值的支持
      u32 x_bits = x_word;
      while (x_bits != 0u) {
        const int bit = __ffs(x_bits) - 1;
        x_bits &= ~(1u << bit);
        const int a = word * 32 + bit;

        // 检查 y 是否有支持 a 的值
        bool has_support = false;
        for (int w2 = 0; w2 < bit_dom_int_size && !has_support; ++w2) {
          const u32 y_dom = bitDom[y_base + w2];
          if (y_dom == 0u) continue;

          const int bitsup_idx = a * bit_dom_int_size + w2;
          const uint2 sup = shmem_bitsup[bitsup_idx];

          if ((sup.x & y_dom) != 0u) {
            has_support = true;
          }
        }

        if (!has_support) {
          del_x |= (1u << bit);
        }
      }

      // 检查 y 域中每个值的支持
      u32 y_bits = y_word;
      while (y_bits != 0u) {
        const int bit = __ffs(y_bits) - 1;
        y_bits &= ~(1u << bit);
        const int b = word * 32 + bit;

        bool has_support = false;
        for (int w2 = 0; w2 < bit_dom_int_size && !has_support; ++w2) {
          const u32 x_dom = bitDom[x_base + w2];
          if (x_dom == 0u) continue;

          const int bitsup_idx = (max_dom_size + b) * bit_dom_int_size + w2;
          const uint2 sup = shmem_bitsup[bitsup_idx];

          if ((sup.y & x_dom) != 0u) {
            has_support = true;
          }
        }

        if (!has_support) {
          del_y |= (1u << bit);
        }
      }

      // 写入该 warp 的删除缓冲区（直接赋值，每个 lane 写不同位置，无冲突）
      warp_del[word] = del_x;
      warp_del[bit_dom_int_size + word] = del_y;
    }
    __syncwarp();

    // Lane 0 应用删除（每个 warp 独立处理自己的 world）
    if (lane_id == 0) {
      int x_deletions = 0;
      int y_deletions = 0;

      // 应用删除到该 world 的域
      for (int w = 0; w < bit_dom_int_size; ++w) {
        const u32 del_x_word = warp_del[w];
        const u32 del_y_word = warp_del[bit_dom_int_size + w];

        if (del_x_word != 0u) {
          const u32 old_x = bitDom[x_base + w];
          const u32 new_x = old_x & ~del_x_word;
          if (new_x != old_x) {
            bitDom[x_base + w] = new_x;
            x_deletions += __popc(old_x) - __popc(new_x);
          }
        }

        if (del_y_word != 0u) {
          const u32 old_y = bitDom[y_base + w];
          const u32 new_y = old_y & ~del_y_word;
          if (new_y != old_y) {
            bitDom[y_base + w] = new_y;
            y_deletions += __popc(old_y) - __popc(new_y);
          }
        }
      }

      // 增量更新域大小（复用删值计数，避免重复 __popc）
      const int x_size = dom_size[x] - x_deletions;
      const int y_size = dom_size[y] - y_deletions;
      dom_size[x] = x_size;
      dom_size[y] = y_size;

      // 检查不一致性
      if (x_size == 0 || y_size == 0) {
        ws->inconsistent_flag = 1;
        control->results[global_w] = false;
      } else {
        // 更新 frontier（邻域激活）
        if (x_deletions > 0) {
          PropagateVarToNextBitmap(x, model, ws->frontier_B);
        }
        if (y_deletions > 0) {
          PropagateVarToNextBitmap(y, model, ws->frontier_B);
        }
      }

      ws->deletions += x_deletions + y_deletions;
    }
    __syncwarp();
  }

  // 最后同步所有 warp
  __syncthreads();
}

// Batch3AKernel - 约束聚合单 block kernel（MVP 版本，避免跨 block 同步问题）
__global__
void Batch3AKernel(
    const GModelData model,
    Batch3AControl* control) {

  // MVP: 只使用单个 block，避免跨 block 同步问题
  // 后续可以扩展为多 block 版本（需要 cooperative groups）

  // Shared memory 布局:
  // - shmem_bitsup: bitSup 数据 [2 * max_dom_size * bit_dom_int_size * sizeof(uint2)]
  // - shared_del: 删除缓冲区 [2 * bit_dom_int_size * sizeof(u32)]
  extern __shared__ char shmem_raw[];

  const int num_worlds = control->num_worlds;
  const int num_cons = model.num_constraints;
  const int bitmap_size_words = (num_cons + 31) / 32;
  const int bit_dom_int_size = model.bit_dom_int_size;
  const int max_dom_size = model.max_dom_size;

  // 计算 shared memory 布局
  const int bitsup_size_bytes = 2 * max_dom_size * bit_dom_int_size * sizeof(uint2);
  uint2* shmem_bitsup = reinterpret_cast<uint2*>(shmem_raw);
  u32* shared_del = reinterpret_cast<u32*>(shmem_raw + bitsup_size_bytes);

  // Phase 0: 初始化所有 world
  for (int world_id = threadIdx.x; world_id < num_worlds; world_id += blockDim.x) {
    WorldWorkspace* ws = &control->workspaces[world_id];
    const ProbeTask& task = control->world_probes[world_id];

    // 单例赋值
    const int var_id = task.var_id;
    const int value = task.value;

    if (var_id >= 0 && var_id < model.num_vars &&
        value >= 0 && value < model.max_dom_size) {
      const int dom_base = var_id * bit_dom_int_size;
      const int word_idx = value / 32;
      const int bit_idx = value % 32;

      for (int w = 0; w < bit_dom_int_size; ++w) {
        ws->bitDom[dom_base + w] = 0u;
      }
      if (word_idx < bit_dom_int_size) {
        ws->bitDom[dom_base + word_idx] = (1u << bit_idx);
      }
      ws->d_cur_dom_size[var_id] = 1;

      // 初始化 frontier（邻域激活）
      const int start = model.d_subscription_offset[var_id];
      const int end = model.d_subscription_offset[var_id + 1];
      for (int i = start; i < end; ++i) {
        const int cid = model.d_subscription[i].z;
        atomicOr(&ws->frontier_A[cid / 32], 1u << (cid % 32));
      }
      ws->frontier_nonempty = 1;
    } else {
      ws->inconsistent_flag = 1;
      control->results[world_id] = false;
    }
  }
  __syncthreads();

  // 主传播循环
  for (int iter = 0; iter < control->max_iterations; ++iter) {
    // thread 0 读取 active_mask
    __shared__ u32 active_mask_shared;
    if (threadIdx.x == 0) {
      active_mask_shared = *control->active_world_mask;
    }
    __syncthreads();

    u32 active_mask = active_mask_shared;
    if (active_mask == 0) {
      break;
    }

    // Step 1: thread 0 构建稀疏约束任务列表（只包含活跃约束）
    __shared__ int num_tasks_shared;
    if (threadIdx.x == 0) {
      int task_count = 0;
      for (int cid = 0; cid < num_cons; ++cid) {
        u32 mask = 0;
        for (int w = 0; w < num_worlds; ++w) {
          if ((active_mask & (1u << w)) == 0) continue;
          WorldWorkspace* ws = &control->workspaces[w];
          if (ws->inconsistent_flag == 1) continue;

          const u32 frontier_word = ws->frontier_A[cid / 32];
          if (frontier_word & (1u << (cid % 32))) {
            mask |= (1u << w);
          }
        }
        if (mask != 0) {
          control->constraint_tasks[task_count].cid = cid;
          control->constraint_tasks[task_count].world_mask = mask;
          task_count++;
        }
      }
      control->num_constraint_tasks = task_count;
      num_tasks_shared = task_count;
    }
    __syncthreads();

    const int num_tasks = num_tasks_shared;

    // Step 2: 只遍历活跃约束（O(K) 而非 O(C)）
    for (int task_idx = 0; task_idx < num_tasks; ++task_idx) {
      const int cid = control->constraint_tasks[task_idx].cid;
      const u32 world_mask = control->constraint_tasks[task_idx].world_mask;

      // 加载 bitSup 到 shared memory（一次加载，多 world 共享）
      const int bitsup_words = model.bitsup_per_constraint;  // uint2 数量
      const uint2* src = model.bitSupData + cid * bitsup_words;

      for (int i = threadIdx.x; i < bitsup_words; i += blockDim.x) {
        shmem_bitsup[i] = src[i];
      }
      __syncthreads();

      // 对所有相关 world 执行约束检查（单 block 版本：block_world_start=0）
      ExecuteConstraintCheck_Aggregated(
          cid, world_mask, 0, num_worlds, model, control, shmem_bitsup, shared_del);

      __syncthreads();
    }

    // 迭代结束，更新全局状态（thread 0）
    if (threadIdx.x == 0) {
      // 检查每个 world 的收敛状态
      u32 new_active_mask = 0;
      for (int w = 0; w < num_worlds; ++w) {
        WorldWorkspace* ws = &control->workspaces[w];
        if (ws->inconsistent_flag == 1) continue;

        // 检查 frontier_B 是否非空
        bool has_work = false;
        for (int word = 0; word < bitmap_size_words; ++word) {
          if (ws->frontier_B[word] != 0u) {
            has_work = true;
            break;
          }
        }

        if (has_work) {
          new_active_mask |= (1u << w);
          // Swap frontiers
          u32* tmp = ws->frontier_A;
          ws->frontier_A = ws->frontier_B;
          ws->frontier_B = tmp;
          // 清空新的 frontier_B
          for (int word = 0; word < bitmap_size_words; ++word) {
            ws->frontier_B[word] = 0u;
          }
          ws->iterations++;
        }
      }
      *control->active_world_mask = new_active_mask;
      (*control->global_iteration)++;
    }
    __syncthreads();
  }
}

// ============================================================================
// Batch3AKernel_MultiBlock - 多 Block World 分片版本
// 每个 block 独立处理 G 个 world，无需跨 block 同步
// ============================================================================
__global__
void Batch3AKernel_MultiBlock(
    const GModelData model,
    Batch3AControl* control) {

  // 获取本 block 的 world 分片信息
  const int block_world_start = control->block_world_start[blockIdx.x];
  const int block_world_count = control->block_world_count[blockIdx.x];

  // 如果本 block 没有分配到 world，直接返回
  if (block_world_count == 0) return;

  // Shared memory 布局
  extern __shared__ char shmem_raw[];
  const int bit_dom_int_size = model.bit_dom_int_size;
  const int max_dom_size = model.max_dom_size;
  const int num_cons = model.num_constraints;
  const int bitmap_size_words = (num_cons + 31) / 32;

  const int bitsup_size_bytes = 2 * max_dom_size * bit_dom_int_size * sizeof(uint2);
  uint2* shmem_bitsup = reinterpret_cast<uint2*>(shmem_raw);
  u32* shared_del = reinterpret_cast<u32*>(shmem_raw + bitsup_size_bytes);

  // Phase 0: 初始化本 block 的 world 子集
  for (int local_w = threadIdx.x; local_w < block_world_count; local_w += blockDim.x) {
    const int global_w = block_world_start + local_w;
    WorldWorkspace* ws = &control->workspaces[global_w];
    const ProbeTask& task = control->world_probes[global_w];

    // 单例赋值
    const int var_id = task.var_id;
    const int value = task.value;

    if (var_id >= 0 && var_id < model.num_vars &&
        value >= 0 && value < model.max_dom_size) {
      const int dom_base = var_id * bit_dom_int_size;
      const int word_idx = value / 32;
      const int bit_idx = value % 32;

      for (int w = 0; w < bit_dom_int_size; ++w) {
        ws->bitDom[dom_base + w] = 0u;
      }
      if (word_idx < bit_dom_int_size) {
        ws->bitDom[dom_base + word_idx] = (1u << bit_idx);
      }
      ws->d_cur_dom_size[var_id] = 1;

      // 初始化 frontier（邻域激活）
      const int start = model.d_subscription_offset[var_id];
      const int end = model.d_subscription_offset[var_id + 1];
      for (int i = start; i < end; ++i) {
        const int cid = model.d_subscription[i].z;
        atomicOr(&ws->frontier_A[cid / 32], 1u << (cid % 32));
      }
      ws->frontier_nonempty = 1;
    } else {
      ws->inconsistent_flag = 1;
      control->results[global_w] = false;
    }
  }
  __syncthreads();

  // 主传播循环
  for (int iter = 0; iter < control->max_iterations; ++iter) {
    // 读取全局 active_mask，提取本 block 的局部掩码
    __shared__ u32 local_active_mask;
    if (threadIdx.x == 0) {
      const u32 global_mask = *control->active_world_mask;
      u32 mask = 0;
      for (int i = 0; i < block_world_count; ++i) {
        const int global_w = block_world_start + i;
        if (global_mask & (1u << global_w)) {
          mask |= (1u << i);  // 局部索引
        }
      }
      local_active_mask = mask;
    }
    __syncthreads();

    if (local_active_mask == 0) break;

    // Step 1: thread 0 构建本 block 的稀疏约束任务列表
    // 注意：使用 control->constraint_tasks 的分片（每个 block 用不同区域）
    // 为简化实现，这里使用 shared memory 存储局部任务
    __shared__ int num_local_tasks;
    __shared__ int local_task_cids[512];     // 最多 512 个约束
    __shared__ u32 local_task_masks[512];

    if (threadIdx.x == 0) {
      int task_count = 0;
      for (int cid = 0; cid < num_cons && task_count < 512; ++cid) {
        u32 mask = 0;
        for (int i = 0; i < block_world_count; ++i) {
          if ((local_active_mask & (1u << i)) == 0) continue;
          const int global_w = block_world_start + i;
          WorldWorkspace* ws = &control->workspaces[global_w];
          if (ws->inconsistent_flag == 1) continue;

          const u32 frontier_word = ws->frontier_A[cid / 32];
          if (frontier_word & (1u << (cid % 32))) {
            mask |= (1u << i);  // 局部索引
          }
        }
        if (mask != 0) {
          local_task_cids[task_count] = cid;
          local_task_masks[task_count] = mask;
          task_count++;
        }
      }
      num_local_tasks = task_count;
    }
    __syncthreads();

    // Step 2: 只遍历活跃约束
    for (int task_idx = 0; task_idx < num_local_tasks; ++task_idx) {
      const int cid = local_task_cids[task_idx];
      const u32 local_world_mask = local_task_masks[task_idx];

      // 加载 bitSup 到 shared memory
      const int bitsup_words = model.bitsup_per_constraint;
      const uint2* src = model.bitSupData + cid * bitsup_words;

      for (int i = threadIdx.x; i < bitsup_words; i += blockDim.x) {
        shmem_bitsup[i] = src[i];
      }
      __syncthreads();

      // 对本 block 的相关 world 执行约束检查
      ExecuteConstraintCheck_Aggregated(
          cid, local_world_mask, block_world_start, block_world_count,
          model, control, shmem_bitsup, shared_del);

      __syncthreads();
    }

    // 迭代结束，更新本 block 管理的 world 状态
    if (threadIdx.x == 0) {
      u32 done_mask = 0;  // 已完成的 world（全局索引）
      u32 still_active_mask = 0;  // 仍需继续的 world（全局索引）

      for (int i = 0; i < block_world_count; ++i) {
        const int global_w = block_world_start + i;
        WorldWorkspace* ws = &control->workspaces[global_w];

        if (ws->inconsistent_flag == 1) {
          done_mask |= (1u << global_w);
          continue;
        }

        // 检查 frontier_B 是否非空
        bool has_work = false;
        for (int word = 0; word < bitmap_size_words; ++word) {
          if (ws->frontier_B[word] != 0u) {
            has_work = true;
            break;
          }
        }

        if (has_work) {
          still_active_mask |= (1u << global_w);
          // Swap frontiers
          u32* tmp = ws->frontier_A;
          ws->frontier_A = ws->frontier_B;
          ws->frontier_B = tmp;
          // 清空新的 frontier_B
          for (int word = 0; word < bitmap_size_words; ++word) {
            ws->frontier_B[word] = 0u;
          }
          ws->iterations++;
        } else {
          done_mask |= (1u << global_w);
        }
      }

      // 原子更新全局 active_mask
      // 清除已完成的 world，保持仍活跃的
      if (done_mask != 0) {
        atomicAnd(control->active_world_mask, ~done_mask);
      }
    }
    __syncthreads();
  }
}

// LaunchBatch3AKernelWrapper - Host 端调用入口
void LaunchBatch3AKernelWrapper(
    GModelData model_data,
    Batch3AControl* control) {

  const int num_worlds = control->num_worlds;

  // 选择分片策略：G=4（每 block 4 个 world）
  // 当 num_worlds <= 4 时使用单 block
  const int G = 4;  // worlds per block
  const int block_size = G * 32;  // 128 threads = 4 warps
  const int num_blocks = (num_worlds + G - 1) / G;  // 最多 8 blocks（32/4）

  // 初始化 block 分片信息
  control->worlds_per_block = G;
  for (int b = 0; b < num_blocks; ++b) {
    control->block_world_start[b] = b * G;
    control->block_world_count[b] = (b == num_blocks - 1)
        ? (num_worlds - b * G)  // 最后一个 block 可能不满
        : G;
  }
  // 清空未使用的 block 信息
  for (int b = num_blocks; b < 16; ++b) {
    control->block_world_start[b] = 0;
    control->block_world_count[b] = 0;
  }

  // 计算 shared memory 需求
  // bitSup: 2 * max_dom_size * bit_dom_int_size * sizeof(uint2)
  // del_buffer: G * 2 * bit_dom_int_size * sizeof(u32) (每个 warp 独立)
  // local_tasks: 512 * (sizeof(int) + sizeof(u32)) = 4KB（约束任务列表）
  const int bitsup_bytes = 2 * model_data.max_dom_size *
                           model_data.bit_dom_int_size * sizeof(uint2);
  const int del_buffer_bytes = G * 2 * model_data.bit_dom_int_size * sizeof(u32);
  const int local_tasks_bytes = 512 * (sizeof(int) + sizeof(u32));
  const int shared_mem_bytes = bitsup_bytes + del_buffer_bytes + local_tasks_bytes;

  LOG(INFO) << "Batch3A kernel launch (multi-block, G=" << G << "):";
  LOG(INFO) << "  num_blocks: " << num_blocks << ", block_size: " << block_size;
  LOG(INFO) << "  num_worlds: " << num_worlds << ", worlds_per_block: " << G;
  LOG(INFO) << "  shared_mem_bytes: " << shared_mem_bytes
            << " (bitsup=" << bitsup_bytes
            << ", del=" << del_buffer_bytes
            << ", tasks=" << local_tasks_bytes << ")";

  Batch3AKernel_MultiBlock<<<dim3(num_blocks), dim3(block_size), shared_mem_bytes>>>(
      model_data,
      control);

  cudaError_t err = cudaGetLastError();
  if (err != cudaSuccess) {
    LOG(ERROR) << "Failed to launch Batch3AKernel_MultiBlock: "
               << cudaGetErrorString(err);
    throw std::runtime_error(
        std::string("Failed to launch Batch3AKernel_MultiBlock: ") +
        cudaGetErrorString(err));
  }
}

}  // namespace cpim
