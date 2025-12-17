#include "GModel.cuh"

#include <algorithm>
#include <cstring>  // for memset (统一内存直接操作)
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

#include <cooperative_groups.h>

#include "model/gmodel_adapter.h"
#include "model/intermediate_model.h"

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
GModel::GModel(int num_vars, int num_constraints, int max_dom_size,
               int bit_dom_int_size, int bit_doms_int_size, int max_depth,
               int bitsup_per_constraint, u32* bitDom, int* d_cur_dom_size,
               int2* d_assigned, uint3* d_subscription,
               int* d_subscription_offset, int subscription_size,
               cudaTextureObject_t texObj_BitSup, cudaArray_t cuArray3D_BitSup,
               uint2* bitSupData, int2* constraint_scopes,
               std::vector<std::vector<int>> var_to_constraints,
               std::vector<int> initial_dom_sizes)
    : num_vars(num_vars),
      num_constraints(num_constraints),
      max_dom_size(max_dom_size),
      bit_dom_int_size(bit_dom_int_size),
      bit_doms_int_size(bit_doms_int_size),
      max_depth(max_depth),
      bitsup_per_constraint(bitsup_per_constraint),
      bitDom(bitDom),
      d_cur_dom_size(d_cur_dom_size),
      d_assigned(d_assigned),
      d_subscription(d_subscription),
      d_subscription_offset(d_subscription_offset),
      subscription_size(subscription_size),
      texObj_BitSup(texObj_BitSup),
      bitSupData(bitSupData),
      constraint_scopes(constraint_scopes),
      var_to_constraints(std::move(var_to_constraints)),
      initial_dom_sizes(std::move(initial_dom_sizes)),
      cuArray3D_BitSup(cuArray3D_BitSup) {
  std::cout << "[GModel] Constructed with multi-level support and device subscription"
            << std::endl;
  std::cout << "  num_vars=" << num_vars << std::endl;
  std::cout << "  num_constraints=" << num_constraints << std::endl;
  std::cout << "  max_dom_size=" << max_dom_size << std::endl;
  std::cout << "  bit_dom_int_size=" << bit_dom_int_size << std::endl;
  std::cout << "  bit_doms_int_size=" << bit_doms_int_size << std::endl;
  std::cout << "  max_depth=" << max_depth << std::endl;
  std::cout << "  subscription_size=" << subscription_size << std::endl;
  std::cout << "  texObj_BitSup=" << texObj_BitSup << std::endl;
}

// ============================================================================
// Deprecated Public Constructor (for backward compatibility)
// ============================================================================
GModel::GModel(const model::IntermediateModel& im_model)
    : num_vars(0),  // Temporary, will be reassigned
      num_constraints(0),
      max_dom_size(0),
      bit_dom_int_size(0),
      bit_doms_int_size(0),
      max_depth(0) {
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
GModel::GModel(GModel&& other) noexcept
    : num_vars(other.num_vars),
      num_constraints(other.num_constraints),
      max_dom_size(other.max_dom_size),
      bit_dom_int_size(other.bit_dom_int_size),
      bit_doms_int_size(other.bit_doms_int_size),
      max_depth(other.max_depth),
      bitsup_per_constraint(other.bitsup_per_constraint),
      bitDom(other.bitDom),
      d_cur_dom_size(other.d_cur_dom_size),
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
      current_level_(other.current_level_),
      assigned_size_(other.assigned_size_) {
  // Take ownership of resources
  other.bitDom = nullptr;
  other.d_cur_dom_size = nullptr;
  other.d_assigned = nullptr;
  other.d_subscription = nullptr;
  other.d_subscription_offset = nullptr;
  other.subscription_size = 0;
  other.texObj_BitSup = 0;
  other.bitSupData = nullptr;
  other.constraint_scopes = nullptr;
  other.cuArray3D_BitSup = nullptr;
  other.current_level_ = 0;
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
    const_cast<int&>(num_vars) = other.num_vars;
    const_cast<int&>(num_constraints) = other.num_constraints;
    const_cast<int&>(max_dom_size) = other.max_dom_size;
    const_cast<int&>(bit_dom_int_size) = other.bit_dom_int_size;
    const_cast<int&>(bit_doms_int_size) = other.bit_doms_int_size;
    const_cast<int&>(max_depth) = other.max_depth;
    const_cast<int&>(bitsup_per_constraint) = other.bitsup_per_constraint;
    bitDom = other.bitDom;
    d_cur_dom_size = other.d_cur_dom_size;
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
    current_level_ = other.current_level_;
    assigned_size_ = other.assigned_size_;

    other.bitDom = nullptr;
    other.d_cur_dom_size = nullptr;
    other.d_assigned = nullptr;
    other.d_subscription = nullptr;
    other.d_subscription_offset = nullptr;
    other.subscription_size = 0;
    other.texObj_BitSup = 0;
    other.bitSupData = nullptr;
    other.constraint_scopes = nullptr;
    other.cuArray3D_BitSup = nullptr;
    other.current_level_ = 0;
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

__global__ void CsCheckMainKernel(const int* events, int num_events,
                                  u32* bitDom, const uint2* bitSup,
                                  const int2* scopes, int bit_dom_int_size,
                                  int max_dom_size, int bitsup_per_constraint,
                                  u32* removal,
                                  int current_level,
                                  int bit_doms_int_size) {
  const int event_idx = blockIdx.x;
  if (event_idx >= num_events) return;

  // Calculate level offset for multi-level bitDom access
  const int level_offset = current_level * bit_doms_int_size;

  const int cid = events[event_idx];
  const int2 scope = scopes[cid];
  if (scope.x < 0 || scope.y < 0) return;

  const int x = scope.x;
  const int y = scope.y;
  u32* dom_x = bitDom + level_offset + x * bit_dom_int_size;
  u32* dom_y = bitDom + level_offset + y * bit_dom_int_size;

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
      atomicAnd(reinterpret_cast<unsigned int*>(
                    &bitDom[level_offset + x * bit_dom_int_size + word]),
                ~removed_x);
    }

    const u32 old_word_y = s_dom_y[word];
    const u32 new_word_y = old_word_y & keep_mask_y;
    const u32 removed_y = old_word_y ^ new_word_y;
    if (removed_y) {
      s_dom_y[word] = new_word_y;
      atomicOr(&removal[y * bit_dom_int_size + word], removed_y);
      atomicAnd(reinterpret_cast<unsigned int*>(
                    &bitDom[level_offset + y * bit_dom_int_size + word]),
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

    CsCheckMainKernel<<<num_events, threads_per_block, shared_mem_bytes>>>(
        d_events, num_events, bitDom, bitSupData, constraint_scopes,
        bit_dom_int_size, max_dom_size, bitsup_per_constraint, removal,
        current_level_, bit_doms_int_size);
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
        const int level_base = current_level_ * bit_doms_int_size;
        int new_size = 0;
        for (int w = 0; w < bit_dom_int_size; ++w) {
          new_size += Popcount(bitDom[level_base + base + w]);
        }
        dom_size[var] = new_size;
        // Sync to d_cur_dom_size for multi-level support
        d_cur_dom_size[current_level_ * num_vars + var] = new_size;
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

// BpC 核心：执行单约束检查
__device__
PropagateResult ExecuteConstraintCheck_BpC(
    int cid,
    const GModelData& model,
    int current_level,
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

  const int level_offset = current_level * model.bit_doms_int_size;
  u32* dom_x = model.bitDom + level_offset + x * model.bit_dom_int_size;
  u32* dom_y = model.bitDom + level_offset + y * model.bit_dom_int_size;

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

  // 3) 写回 global + 统计删值 + DWO
  if (threadIdx.x == 0) {
    int new_size_x = 0, new_size_y = 0;

    for (int w = 0; w < model.bit_dom_int_size; ++w) {
      const u32 old_x = dom_x[w];
      const u32 new_x = s_dom_x[w];
      const u32 removed_x = old_x & ~new_x;
      if (removed_x) {
        atomicAnd(reinterpret_cast<unsigned int*>(&dom_x[w]), new_x);
        r.x_changed = true;
        r.deletions += __popc(removed_x);
      }

      const u32 old_y = dom_y[w];
      const u32 new_y = s_dom_y[w];
      const u32 removed_y = old_y & ~new_y;
      if (removed_y) {
        atomicAnd(reinterpret_cast<unsigned int*>(&dom_y[w]), new_y);
        r.y_changed = true;
        r.deletions += __popc(removed_y);
      }
    }

    // 重新计算域大小
    const int base_x = level_offset + x * model.bit_dom_int_size;
    const int base_y = level_offset + y * model.bit_dom_int_size;
    for (int w = 0; w < model.bit_dom_int_size; ++w) {
      new_size_x += __popc(model.bitDom[base_x + w]);
      new_size_y += __popc(model.bitDom[base_y + w]);
    }
    model.d_cur_dom_size[current_level * model.num_vars + x] = new_size_x;
    model.d_cur_dom_size[current_level * model.num_vars + y] = new_size_y;

    if (new_size_x == 0 || new_size_y == 0) {
      r.inconsistent = true;
    }
  }
  __syncthreads();

  return r;
}

// Bitmap GAC Kernel（单轮传播）
__global__
void BitmapGACKernel(
    GModelData model,
    GACControl* control,
    u32* frontier_cur,
    u32* frontier_next,
    int bitmap_size_words,
    int current_level) {

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

    // 传播
    PropagateResult r =
        ExecuteConstraintCheck_BpC(cid, model, current_level, shmem);

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
// ============================================================================
__global__
void PersistentGACKernel(
    GModelData model,
    PersistentGACControl* control,
    int bitmap_size_words,
    int current_level,
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

      // 执行约束传播
      PropagateResult r = ExecuteConstraintCheck_BpC(
          cid, model, current_level, shmem);

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

}  // namespace

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
    size_t shmem_bytes = 2 * bit_dom_int_size * sizeof(u32);

    BitmapGACKernel<<<blocks, threads, shmem_bytes>>>(
        md,
        d_gac_control,
        d_queue_bitmap_A,
        d_queue_bitmap_B,
        bitmap_size_words,
        current_level_);

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
  size_t sharedMemBytes = 2 * bit_dom_int_size * sizeof(u32);

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

  void* args[] = {
    &md,
    &d_persistent_control,
    &bitmap_size_words,
    &current_level_,
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

int GModel::CreateNewLevel() {
  if (current_level_ >= max_depth - 1) {
    throw std::runtime_error(
        "[GModel::CreateNewLevel] Max depth reached: " + std::to_string(max_depth));
  }

  ++current_level_;

  // 计算源和目标地址
  u32* src = bitDom + (current_level_ - 1) * bit_doms_int_size;
  u32* dst = bitDom + current_level_ * bit_doms_int_size;

  // 复制域（Device to Device，统一内存零拷贝）
  // 注意：在 Jetson Orin 上，这是内存内拷贝，不需要 PCIe 传输
  const size_t copy_size = bit_doms_int_size * sizeof(u32);
  cudaError_t status = cudaMemcpy(dst, src, copy_size, cudaMemcpyDeviceToDevice);
  if (status != cudaSuccess) {
    throw std::runtime_error(
        "[GModel::CreateNewLevel] cudaMemcpy failed: " +
        std::string(cudaGetErrorString(status)));
  }

  // 复制域大小
  std::memcpy(d_cur_dom_size + current_level_ * num_vars,
              d_cur_dom_size + (current_level_ - 1) * num_vars,
              num_vars * sizeof(int));

  return current_level_;
}

void GModel::BackToLevel(int level) {
  if (level < 0 || level >= max_depth) {
    throw std::runtime_error(
        "[GModel::BackToLevel] Invalid level: " + std::to_string(level));
  }

  if (level > current_level_) {
    throw std::runtime_error(
        "[GModel::BackToLevel] Cannot back to future level: " +
        std::to_string(level) + " (current=" + std::to_string(current_level_) + ")");
  }

  current_level_ = level;

  // 回溯赋值栈（如果需要）
  while (assigned_size_ > level) {
    --assigned_size_;
  }
}

int GModel::GetCurrentLevel() const {
  return current_level_;
}

bool GModel::AssignValue(int var, int value, int level) {
  if (var < 0 || var >= num_vars) {
    throw std::runtime_error(
        "[GModel::AssignValue] Invalid var: " + std::to_string(var));
  }

  if (level < 0 || level >= max_depth) {
    throw std::runtime_error(
        "[GModel::AssignValue] Invalid level: " + std::to_string(level));
  }

  if (value < 0 || value >= max_dom_size) {
    throw std::runtime_error(
        "[GModel::AssignValue] Invalid value: " + std::to_string(value));
  }

  // 清空该变量的域
  const int base_idx = GetBitDomIndex(var, 0, level);
  for (int word = 0; word < bit_dom_int_size; ++word) {
    bitDom[base_idx + word] = 0u;
  }

  // 设置唯一值
  const int word = value / kBitsPerWord;
  const int bit = value % kBitsPerWord;
  bitDom[base_idx + word] = (1u << bit);

  // 更新域大小
  d_cur_dom_size[level * num_vars + var] = 1;

  // 记录赋值
  if (level < max_depth) {
    d_assigned[level] = make_int2(var, value);
    assigned_size_ = std::max(assigned_size_, level + 1);
  }

  return true;
}

bool GModel::RemoveValue(int var, int value, int level) {
  if (var < 0 || var >= num_vars) {
    throw std::runtime_error(
        "[GModel::RemoveValue] Invalid var: " + std::to_string(var));
  }

  if (level < 0 || level >= max_depth) {
    throw std::runtime_error(
        "[GModel::RemoveValue] Invalid level: " + std::to_string(level));
  }

  if (value < 0 || value >= max_dom_size) {
    return false;  // Value out of range, nothing to remove
  }

  // 清除指定位
  const int word = value / kBitsPerWord;
  const int bit = value % kBitsPerWord;
  const int idx = GetBitDomIndex(var, word, level);

  const u32 old_value = bitDom[idx];
  const u32 new_value = old_value & ~(1u << bit);

  if (old_value == new_value) {
    return false;  // Value was already removed
  }

  bitDom[idx] = new_value;

  // 更新域大小（需要重新计算）
  int dom_size = 0;
  const int base_idx = GetBitDomIndex(var, 0, level);
  for (int w = 0; w < bit_dom_int_size; ++w) {
    dom_size += Popcount(bitDom[base_idx + w]);
  }
  d_cur_dom_size[level * num_vars + var] = dom_size;

  return dom_size > 0;  // Return false if domain becomes empty
}

int GModel::GetDomainSize(int var, int level) const {
  if (var < 0 || var >= num_vars) {
    throw std::runtime_error(
        "[GModel::GetDomainSize] Invalid var: " + std::to_string(var));
  }

  if (level < 0 || level >= max_depth) {
    throw std::runtime_error(
        "[GModel::GetDomainSize] Invalid level: " + std::to_string(level));
  }

  if (!d_cur_dom_size) {
    throw std::runtime_error(
        "[GModel::GetDomainSize] d_cur_dom_size is null!");
  }

  return d_cur_dom_size[level * num_vars + var];
}

// ============================================================================
// 求解器辅助方法实现
// ============================================================================

int GModel::GetMinDomainVar(int level) const {
  if (level < 0 || level >= max_depth) {
    throw std::runtime_error(
        "[GModel::GetMinDomainVar] Invalid level: " + std::to_string(level));
  }

  int min_var = -1;
  int min_size = max_dom_size + 1;

  for (int var = 0; var < num_vars; ++var) {
    const int size = d_cur_dom_size[level * num_vars + var];
    // 跳过已赋值的变量（size == 1）和空域（size == 0）
    if (size > 1 && size < min_size) {
      min_size = size;
      min_var = var;
    }
  }

  return min_var;  // -1 表示所有变量都已赋值或域为空
}

bool GModel::IsAssigned(int var, int level) const {
  if (var < 0 || var >= num_vars) {
    throw std::runtime_error(
        "[GModel::IsAssigned] Invalid var: " + std::to_string(var));
  }

  if (level < 0 || level >= max_depth) {
    throw std::runtime_error(
        "[GModel::IsAssigned] Invalid level: " + std::to_string(level));
  }

  return d_cur_dom_size[level * num_vars + var] == 1;
}

bool GModel::IsFullyAssigned(int level) const {
  if (level < 0 || level >= max_depth) {
    throw std::runtime_error(
        "[GModel::IsFullyAssigned] Invalid level: " + std::to_string(level));
  }

  for (int var = 0; var < num_vars; ++var) {
    if (d_cur_dom_size[level * num_vars + var] != 1) {
      return false;
    }
  }

  return true;
}

int GModel::GetAssignedValue(int var, int level) const {
  if (var < 0 || var >= num_vars) {
    throw std::runtime_error(
        "[GModel::GetAssignedValue] Invalid var: " + std::to_string(var));
  }

  if (level < 0 || level >= max_depth) {
    throw std::runtime_error(
        "[GModel::GetAssignedValue] Invalid level: " + std::to_string(level));
  }

  const int size = d_cur_dom_size[level * num_vars + var];
  if (size != 1) {
    return -1;  // 未赋值或域为空
  }

  // 查找域中唯一的值（使用 __builtin_ctz 优化）
  const int base_idx = GetBitDomIndex(var, 0, level);
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

int GModel::GetFirstValue(int var, int level) const {
  if (var < 0 || var >= num_vars) {
    throw std::runtime_error(
        "[GModel::GetFirstValue] Invalid var: " + std::to_string(var));
  }

  if (level < 0 || level >= max_depth) {
    throw std::runtime_error(
        "[GModel::GetFirstValue] Invalid level: " + std::to_string(level));
  }

  // 使用 __builtin_ctz 快速查找第一个值
  const int base_idx = GetBitDomIndex(var, 0, level);
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

int GModel::GetNextValue(int var, int value, int level) const {
  if (var < 0 || var >= num_vars) {
    throw std::runtime_error(
        "[GModel::GetNextValue] Invalid var: " + std::to_string(var));
  }

  if (level < 0 || level >= max_depth) {
    throw std::runtime_error(
        "[GModel::GetNextValue] Invalid level: " + std::to_string(level));
  }

  if (value < 0 || value >= max_dom_size - 1) {
    return -1;  // 没有更多值
  }

  // 使用 __builtin_ctz 快速查找下一个值
  const int base_idx = GetBitDomIndex(var, 0, level);
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

}  // namespace cpim
