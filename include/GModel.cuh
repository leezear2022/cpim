#pragma once

#include <cstdint>
#include <vector>

#include <cuda_runtime.h>

namespace cpim {
namespace model {
class IntermediateModel;
class GModelAdapter;
}  // namespace model

using u32 = uint32_t;

struct GacStats {
  int iterations = 0;
  int deletions = 0;
  bool inconsistent = false;
};

// ============================================================================
// GModel - 简化的GPU模型，只包含核心位集结构
//
// 设计原则：
// 1. 统一内存 (Unified Memory)：所有数据使用 cudaMallocManaged
// 2. 最小化：只包含 bitDom 和 bitSup
// 3. 零拷贝：利用 Jetson 的统一内存特性，避免显式拷贝
// 4. 单一职责：只负责 GPU 内存管理，不负责数据转换
// ============================================================================
class GModel {
 public:
  // 友元类，允许 GModelAdapter 访问私有构造函数
  friend class model::GModelAdapter;

  // 模型维度（const 成员，构造时初始化）
  const int num_vars;
  const int num_constraints;
  const int max_dom_size;
  const int bit_dom_int_size;  // 每个变量域需要多少个 uint32
  const int bitsup_per_constraint = 0;

  // 位域表示（统一内存 - CPU/GPU 都可访问，需要读写）
  // bitDom[var_id * bit_dom_int_size + word_idx]
  u32* bitDom = nullptr;

  // 位支持表示（纹理内存 - GPU 专用，只读优化）
  // 3D 纹理索引: tex3D<uint2>(texObj_BitSup, value, word_idx, constraint_id)
  // 返回的 uint2:
  //   - .x: 变量 0 → 变量 1 的支持位集
  //   - .y: 变量 1 → 变量 0 的支持位集
  //
  // 优势：
  //   1. 硬件纹理缓存加速（适合 2D/3D 访问模式）
  //   2. 自动边界检查（clamp 模式）
  //   3. Jetson Orin 统一内存架构下零拷贝
  //   4. 只读数据，无需 CPU 访问，节省内存
  cudaTextureObject_t texObj_BitSup = 0;

  // 位支持表示（统一内存 - GPU kernel 直接访问）
  uint2* bitSupData = nullptr;

  // 约束作用域（统一内存）
  int2* constraint_scopes = nullptr;

  // 变量 → 约束 邻接表（CPU 侧使用）
  std::vector<std::vector<int>> var_to_constraints;

  // 初始域大小（CPU 侧基线）
  std::vector<int> initial_dom_sizes;

  // 从 IntermediateModel 构造（已弃用，建议使用 GModelAdapter::Build）
  // 保留此构造函数以保持向后兼容性
  [[deprecated("Use GModelAdapter::Build() instead")]] explicit GModel(
      const model::IntermediateModel& im_model);

  // 移动构造和赋值
  GModel(GModel&& other) noexcept;
  GModel& operator=(GModel&& other) noexcept;

  ~GModel();

  // 禁止拷贝
  GModel(const GModel&) = delete;
  GModel& operator=(const GModel&) = delete;

  // 打印模型信息
  void Print(int max_print = 8) const;

  // GPU 验证：调用 GPU kernel 读取数据以验证统一内存访问
  // 已弃用，建议使用 GModelValidator::ValidateGPUMemory()
  [[deprecated("Use GModelValidator::ValidateGPUMemory() instead")]] void
  VerifyOnGPU() const;

  // 基线 GAC 传播：CPU 队列 + GPU CsCheckMain
  GacStats EnforceGAC(bool verbose = true, bool use_thrust_queue = false);

 private:
  // 纹理后端存储（CUDA Array，对用户不可见）
  cudaArray_t cuArray3D_BitSup = nullptr;

  // 私有构造函数，由 GModelAdapter 调用
  // 接收预分配的内存指针和纹理对象（所有权转移给 GModel）
  GModel(int num_vars, int num_constraints, int max_dom_size,
         int bit_dom_int_size, int bitsup_per_constraint, u32* bitDom,
         cudaTextureObject_t texObj_BitSup, cudaArray_t cuArray3D_BitSup,
         uint2* bitSupData, int2* constraint_scopes,
         std::vector<std::vector<int>> var_to_constraints,
         std::vector<int> initial_dom_sizes);

  // 旧的转换逻辑（已移到 GModelAdapter）
  void BuildFromIntermediate(const model::IntermediateModel& im_model);
};

}  // namespace cpim
