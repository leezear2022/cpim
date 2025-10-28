#include "model/gmodel_validator.h"

#include <algorithm>
#include <iostream>

#include "GModel.cuh"
#include "model/intermediate_model.h"
#include "model/types.h"

namespace cpim::model {

// ============================================================================
// GPU kernel for memory validation (with texture memory for bitSup)
// ============================================================================
__global__ void ValidateGPUMemoryKernel(const u32* bitDom,
                                        cudaTextureObject_t bitSup_tex,
                                        int num_vars, int num_constraints,
                                        int bit_dom_int_size, int max_dom_size,
                                        int* error_count) {
  int tid = blockIdx.x * blockDim.x + threadIdx.x;

  // Validate bitDom access (unified memory)
  if (tid < num_vars) {
    u32 dom_word = bitDom[tid * bit_dom_int_size];
    if (tid < 8) {  // Print first few for verification
      printf("[GPU Validator] var[%d] bitDom[0] = 0x%x\n", tid, dom_word);
    }
  }

  // Validate bitSup texture access
  if (tid < num_constraints) {
    // 3D 纹理访问：(value=0, word=0, constraint=tid)
    uint2 sup_word = tex3D<uint2>(bitSup_tex, 0, 0, tid);
    if (tid < 8) {  // Print first few for verification
      printf("[GPU Validator] constraint[%d] bitSup tex3D(0,0,%d) = (0x%x, "
             "0x%x)\n",
             tid, tid, sup_word.x, sup_word.y);
    }
  }
}

// ============================================================================
// Basic Validation (CPU-side)
// ============================================================================
ValidationResult GModelValidator::ValidateBasic(const cpim::GModel& gmodel) {
  ValidationResult result;

  // Check null pointers
  if (gmodel.bitDom == nullptr) {
    result.AddError("bitDom is null");
  }

  if (gmodel.texObj_BitSup == 0) {
    result.AddError("texObj_BitSup is invalid (zero)");
  }

  // Check dimensions
  if (gmodel.num_vars <= 0) {
    result.AddError("num_vars must be positive, got " +
                    std::to_string(gmodel.num_vars));
  }

  if (gmodel.num_constraints < 0) {
    result.AddError("num_constraints must be non-negative, got " +
                    std::to_string(gmodel.num_constraints));
  }

  if (gmodel.max_dom_size <= 0) {
    result.AddError("max_dom_size must be positive, got " +
                    std::to_string(gmodel.max_dom_size));
  }

  if (gmodel.bit_dom_int_size <= 0) {
    result.AddError("bit_dom_int_size must be positive, got " +
                    std::to_string(gmodel.bit_dom_int_size));
  }

  // Check consistency
  const int expected_bit_dom_int_size =
      (gmodel.max_dom_size + 31) / 32;  // Ceiling division
  if (gmodel.bit_dom_int_size != expected_bit_dom_int_size) {
    result.AddWarning("bit_dom_int_size mismatch: expected " +
                      std::to_string(expected_bit_dom_int_size) + ", got " +
                      std::to_string(gmodel.bit_dom_int_size));
  }

  return result;
}

// ============================================================================
// Validate bitDom (compare with IntermediateModel)
// ============================================================================
ValidationResult GModelValidator::ValidateBitDom(
    const cpim::GModel& gmodel, const IntermediateModel& im_model) {
  ValidationResult result;

  // First run basic validation
  auto basic = ValidateBasic(gmodel);
  if (!basic.success) {
    result.AddError("Basic validation failed, skipping bitDom validation");
    return result;
  }

  // Check number of variables
  if (gmodel.num_vars != im_model.num_variables()) {
    result.AddError("Variable count mismatch: GModel has " +
                    std::to_string(gmodel.num_vars) +
                    ", IntermediateModel has " +
                    std::to_string(im_model.num_variables()));
    return result;
  }

  // Validate each variable's domain
  constexpr int kBitsPerWord = 32;
  int errors = 0;
  for (const auto& var : im_model.variables()) {
    const int vid = var.id.value;
    const auto& dom = im_model.GetDomain(var.domain);
    const int dom_size = dom.Size();

    // Check each word
    for (int word = 0; word < gmodel.bit_dom_int_size; ++word) {
      const int base = word * kBitsPerWord;
      const int remaining = dom_size - base;

      u32 expected_mask;
      if (remaining <= 0) {
        expected_mask = 0u;
      } else if (remaining >= kBitsPerWord) {
        expected_mask = 0xFFFFFFFFu;
      } else {
        expected_mask = (1u << remaining) - 1u;
      }

      const u32 actual_mask =
          gmodel.bitDom[vid * gmodel.bit_dom_int_size + word];

      if (actual_mask != expected_mask) {
        result.AddError("bitDom mismatch for var " + std::to_string(vid) +
                        " word " + std::to_string(word) + ": expected 0x" +
                        std::to_string(expected_mask) + ", got 0x" +
                        std::to_string(actual_mask));
        ++errors;
        if (errors >= 10) {
          result.AddWarning("Too many bitDom errors, stopping validation");
          return result;
        }
      }
    }
  }

  if (errors == 0) {
    result.AddWarning("bitDom validation passed for all " +
                      std::to_string(gmodel.num_vars) + " variables");
  }

  return result;
}

// ============================================================================
// Validate bitSup (compare with IntermediateModel)
// ============================================================================
ValidationResult GModelValidator::ValidateBitSup(
    const cpim::GModel& gmodel, const IntermediateModel& im_model) {
  ValidationResult result;

  // First run basic validation
  auto basic = ValidateBasic(gmodel);
  if (!basic.success) {
    result.AddError("Basic validation failed, skipping bitSup validation");
    return result;
  }

  // Check number of constraints
  if (gmodel.num_constraints != im_model.num_constraints()) {
    result.AddError("Constraint count mismatch: GModel has " +
                    std::to_string(gmodel.num_constraints) +
                    ", IntermediateModel has " +
                    std::to_string(im_model.num_constraints()));
    return result;
  }

  // Note: Detailed bitSup validation is complex and would require
  // iterating through all tuples. For now, we just check basic structure.
  result.AddWarning(
      "bitSup validation is limited to basic structure checks. Full tuple "
      "validation not yet implemented.");

  return result;
}

// ============================================================================
// Validate GPU Memory Access
// ============================================================================
ValidationResult GModelValidator::ValidateGPUMemory(
    const cpim::GModel& gmodel) {
  ValidationResult result;

  std::cout << "\n[GModelValidator] Validating GPU memory access..."
            << std::endl;

  // First run basic validation
  auto basic = ValidateBasic(gmodel);
  if (!basic.success) {
    result.AddError("Basic validation failed, skipping GPU validation");
    result.errors.insert(result.errors.end(), basic.errors.begin(),
                         basic.errors.end());
    return result;
  }

  // Check data pointers
  if (!gmodel.bitDom || !gmodel.texObj_BitSup) {
    result.AddError("Null pointers or invalid texture object in GModel");
    return result;
  }

  // Allocate error counter on device
  int* d_error_count = nullptr;
  cudaError_t status = cudaMallocManaged(&d_error_count, sizeof(int));
  if (status != cudaSuccess) {
    result.AddError("Failed to allocate error counter: " +
                    std::string(cudaGetErrorString(status)));
    return result;
  }
  *d_error_count = 0;

  // Launch validation kernel
  const int num_threads = std::max(gmodel.num_vars, gmodel.num_constraints);
  const int block_size = 256;
  const int num_blocks = (num_threads + block_size - 1) / block_size;

  std::cout << "[GModelValidator] Launching GPU validation kernel with "
            << num_blocks << " blocks, " << block_size << " threads per block"
            << std::endl;

  ValidateGPUMemoryKernel<<<num_blocks, block_size>>>(
      gmodel.bitDom, gmodel.texObj_BitSup, gmodel.num_vars,
      gmodel.num_constraints, gmodel.bit_dom_int_size, gmodel.max_dom_size,
      d_error_count);

  // Synchronize and check errors
  status = cudaDeviceSynchronize();
  if (status != cudaSuccess) {
    result.AddError("GPU kernel execution failed: " +
                    std::string(cudaGetErrorString(status)));
    cudaFree(d_error_count);
    return result;
  }

  if (*d_error_count > 0) {
    result.AddError("GPU kernel reported " + std::to_string(*d_error_count) +
                    " errors");
  } else {
    std::cout << "[GModelValidator] GPU validation kernel executed successfully"
              << std::endl;
  }

  cudaFree(d_error_count);

  return result;
}

}  // namespace cpim::model
