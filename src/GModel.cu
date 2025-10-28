#include "GModel.cuh"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

#include "model/gmodel_adapter.h"
#include "model/intermediate_model.h"

namespace cpim {

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
               int bit_dom_int_size, int bitsup_per_constraint, u32* bitDom,
               cudaTextureObject_t texObj_BitSup, cudaArray_t cuArray3D_BitSup,
               uint2* bitSupData, int2* constraint_scopes,
               std::vector<std::vector<int>> var_to_constraints,
               std::vector<int> initial_dom_sizes)
    : num_vars(num_vars),
      num_constraints(num_constraints),
      max_dom_size(max_dom_size),
      bit_dom_int_size(bit_dom_int_size),
      bitsup_per_constraint(bitsup_per_constraint),
      bitDom(bitDom),
      texObj_BitSup(texObj_BitSup),
      bitSupData(bitSupData),
      constraint_scopes(constraint_scopes),
      var_to_constraints(std::move(var_to_constraints)),
      initial_dom_sizes(std::move(initial_dom_sizes)),
      cuArray3D_BitSup(cuArray3D_BitSup) {
  std::cout << "[GModel] Constructed from pre-allocated memory and texture"
            << std::endl;
  std::cout << "  num_vars=" << num_vars << std::endl;
  std::cout << "  num_constraints=" << num_constraints << std::endl;
  std::cout << "  max_dom_size=" << max_dom_size << std::endl;
  std::cout << "  bit_dom_int_size=" << bit_dom_int_size << std::endl;
  std::cout << "  texObj_BitSup=" << texObj_BitSup << std::endl;
}

// ============================================================================
// Deprecated Public Constructor (for backward compatibility)
// ============================================================================
GModel::GModel(const model::IntermediateModel& im_model)
    : num_vars(0),  // Temporary, will be reassigned
      num_constraints(0),
      max_dom_size(0),
      bit_dom_int_size(0) {
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
      bitsup_per_constraint(other.bitsup_per_constraint),
      bitDom(other.bitDom),
      texObj_BitSup(other.texObj_BitSup),
      bitSupData(other.bitSupData),
      constraint_scopes(other.constraint_scopes),
      var_to_constraints(std::move(other.var_to_constraints)),
      initial_dom_sizes(std::move(other.initial_dom_sizes)),
      cuArray3D_BitSup(other.cuArray3D_BitSup) {
  // Take ownership of resources
  other.bitDom = nullptr;
  other.texObj_BitSup = 0;
  other.bitSupData = nullptr;
  other.constraint_scopes = nullptr;
  other.cuArray3D_BitSup = nullptr;
}

GModel& GModel::operator=(GModel&& other) noexcept {
  if (this != &other) {
    // Free existing resources
    if (bitDom) cudaFree(bitDom);
    if (texObj_BitSup) cudaDestroyTextureObject(texObj_BitSup);
    if (cuArray3D_BitSup) cudaFreeArray(cuArray3D_BitSup);
    if (bitSupData) cudaFree(bitSupData);
    if (constraint_scopes) cudaFree(constraint_scopes);

    // Transfer ownership
    const_cast<int&>(num_vars) = other.num_vars;
    const_cast<int&>(num_constraints) = other.num_constraints;
    const_cast<int&>(max_dom_size) = other.max_dom_size;
    const_cast<int&>(bit_dom_int_size) = other.bit_dom_int_size;
    const_cast<int&>(bitsup_per_constraint) = other.bitsup_per_constraint;
    bitDom = other.bitDom;
    texObj_BitSup = other.texObj_BitSup;
    bitSupData = other.bitSupData;
    constraint_scopes = other.constraint_scopes;
    var_to_constraints = std::move(other.var_to_constraints);
    initial_dom_sizes = std::move(other.initial_dom_sizes);
    cuArray3D_BitSup = other.cuArray3D_BitSup;

    other.bitDom = nullptr;
    other.texObj_BitSup = 0;
    other.bitSupData = nullptr;
    other.constraint_scopes = nullptr;
    other.cuArray3D_BitSup = nullptr;
  }
  return *this;
}

// ============================================================================
// Destructor
// ============================================================================
GModel::~GModel() {
  if (bitDom || texObj_BitSup || cuArray3D_BitSup) {
    std::cout << "[GModel] Destructor: freeing GPU memory and textures"
              << std::endl;
  }

  if (bitDom) {
    cudaFree(bitDom);
    bitDom = nullptr;
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
                                  const u32* bitDom, const uint2* bitSup,
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
  const u32* dom_x = bitDom + x * bit_dom_int_size;
  const u32* dom_y = bitDom + y * bit_dom_int_size;

  // 传播 x <- y
  for (int val = threadIdx.x; val < max_dom_size; val += blockDim.x) {
    const int word = val / kBitsPerWord;
    const int bit = val % kBitsPerWord;
    if (word >= bit_dom_int_size) continue;
    const u32 dom_word = dom_x[word];
    if (((dom_word >> bit) & 1u) == 0u) continue;

    const int sup_idx_base = cid * bitsup_per_constraint +
                             (0 * max_dom_size + val) * bit_dom_int_size;
    bool supported = false;
    for (int w = 0; w < bit_dom_int_size; ++w) {
      const u32 sup_word = bitSup[sup_idx_base + w].x;
      if (sup_word & dom_y[w]) {
        supported = true;
        break;
      }
    }
    if (!supported) {
      atomicOr(&removal[x * bit_dom_int_size + word], 1u << bit);
    }
  }

  // 传播 y <- x
  for (int val = threadIdx.x; val < max_dom_size; val += blockDim.x) {
    const int word = val / kBitsPerWord;
    const int bit = val % kBitsPerWord;
    if (word >= bit_dom_int_size) continue;
    const u32 dom_word = dom_y[word];
    if (((dom_word >> bit) & 1u) == 0u) continue;

    const int sup_idx_base = cid * bitsup_per_constraint +
                             (1 * max_dom_size + val) * bit_dom_int_size;
    bool supported = false;
    for (int w = 0; w < bit_dom_int_size; ++w) {
      const u32 sup_word = bitSup[sup_idx_base + w].y;
      if (sup_word & dom_x[w]) {
        supported = true;
        break;
      }
    }
    if (!supported) {
      atomicOr(&removal[y * bit_dom_int_size + word], 1u << bit);
    }
  }
}

}  // namespace

GacStats GModel::EnforceGAC(bool verbose, bool /*use_thrust_queue*/) {
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

    constexpr int kBlockSize = 128;
    CsCheckMainKernel<<<num_events, kBlockSize>>>(
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
        const u32 before = bitDom[base + w];
        const u32 remove_bits = before & mask;
        if (remove_bits == 0u) continue;
        if (verbose) {
          for (int b = 0; b < kBitsPerWord; ++b) {
            if ((remove_bits >> b) & 1u) {
              const int value = w * kBitsPerWord + b;
              if (value < max_dom_size) {
                std::cout << "[GAC] remove var " << var << " value " << value
                          << " (iteration " << stats.iterations << ")\n";
              }
            }
          }
        }
        bitDom[base + w] = before & ~remove_bits;
        stats.deletions += Popcount(remove_bits);
        changed = true;
      }
      if (changed) {
        int new_size = 0;
        for (int w = 0; w < bit_dom_int_size; ++w) {
          new_size += Popcount(bitDom[base + w]);
        }
        dom_size[var] = new_size;
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

  if (verbose) {
    std::cout << "\n[GAC] iterations=" << stats.iterations
              << " deletions=" << stats.deletions
              << " inconsistent=" << (stats.inconsistent ? "true" : "false")
              << std::endl;
  }

  return stats;
}

}  // namespace cpim
