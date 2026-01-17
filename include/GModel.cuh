#pragma once

#include <cstdint>
#include <vector>

#include <cuda_runtime.h>
#include <cooperative_groups.h>

#include "base/unified_trail.h"  // Phase 1.2: Trail 回溯系统

namespace cpim {
namespace model {
class IntermediateModel;
class GModelAdapter;
}  // namespace model

using u32 = uint32_t;

// Forward declarations for Batch AC-GPU (defined in solver/gpu/batch_probe_manager.h)
struct ProbeTask;
struct BatchProbeControl;

struct GacStats {
  int iterations = 0;
  int deletions = 0;
  bool inconsistent = false;
};

// Frontier 初始化策略
enum class FrontierInitStrategy {
  FULL_ACTIVATION,      // 激活所有约束（全局 GAC）
  NEIGHBOR_ACTIVATION   // 只激活邻接约束（增量 GAC）
};

// ============================================================================
// GAC GPU 控制结构（用于 Bitmap Frontier 方案）
// ============================================================================
struct GACControl {
  int inconsistent_flag;   // 0 or 1，全局不一致标志
  int scanner_index;       // word-based bitmap 扫描游标
  unsigned long long deletions;  // 累计删值数（使用 unsigned long long 以支持 atomicAdd）
  int iterations;          // 传播轮数（frontier 轮数）
};

// ============================================================================
// 持久化 Kernel 控制结构（用于 Cooperative Groups 持久化 GAC）
// ============================================================================
struct PersistentGACControl {
  // 基础控制字段（与 GACControl 兼容）
  int inconsistent_flag;         // 0 or 1，全局不一致标志
  int scanner_index;             // word-based bitmap 扫描游标
  unsigned long long deletions;  // 累计删值数
  int iterations;                // 传播轮数

  // 持久化 Kernel 专用字段
  int converged_flag;            // 0: 继续, 1: 收敛（达到不动点）
  int frontier_nonempty;         // 0: next frontier 为空, 1: 非空

  // 双缓冲指针（允许在 GPU 端 swap）
  u32* frontier_A;               // Current Frontier
  u32* frontier_B;               // Next Frontier
};

// GModel 数据视图（传入 kernel 使用）
struct GModelData {
  int num_vars;
  int num_constraints;
  int max_dom_size;
  int bit_dom_int_size;
  int bit_doms_int_size;
  int bitsup_per_constraint;

  u32*         bitDom;
  int*         d_cur_dom_size;
  const uint2* bitSupData;
  const int2*  constraint_scopes;
  const uint3* d_subscription;
  const int*   d_subscription_offset;

  // P0-2: NSAC allowed-constraints mask
  const u32*   allowed_masks;           // [num_vars * constraint_bitmap_words]
  int          constraint_bitmap_words; // (num_constraints + 31) / 32
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
  const int bit_doms_int_size;  // 所有变量域需要多少个 uint32 = num_vars * bit_dom_int_size
  const int bitsup_per_constraint = 0;

  // Phase 1.2: 单层域布局（统一内存 - CPU/GPU 都可访问，需要读写）
  // 单层布局：bitDom[var_id * bit_dom_int_size + word_idx]
  // 大小: bit_doms_int_size = num_vars * bit_dom_int_size
  u32* bitDom = nullptr;

  // 域大小追踪（统一内存，单层）
  // 布局：d_cur_dom_size[var_id]
  // 大小: num_vars
  int* d_cur_dom_size = nullptr;

  // Phase 1.2: Trail 回溯系统（统一内存）
  UnifiedTrail* trail_ = nullptr;

  // 赋值栈（统一内存）
  // 布局：d_assigned[level] = (var_id, value)
  int2* d_assigned = nullptr;

  // ========================================================================
  // Device 端订阅表（CSR 格式，统一内存）
  // 用途：高效的事件传播，GPU kernel 可以直接访问变量的邻接约束
  // ========================================================================

  // 订阅条目数组（统一内存）
  // 每个条目是 uint3(x, y, constraint_id)，表示约束 c 连接变量 x 和 y
  uint3* d_subscription = nullptr;

  // 订阅偏移数组（CSR 格式，统一内存）
  // 大小为 num_vars + 1
  // d_subscription_offset[var_id] 到 d_subscription_offset[var_id+1] 之间
  // 是变量 var_id 参与的所有约束
  int* d_subscription_offset = nullptr;

  // 订阅表总条目数（方便 kernel 使用）
  int subscription_size = 0;

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

  // Phase 1.5: 启发式支持
  // 变量度数（静态）- 用于 DOM/DEG 启发式
  std::vector<int> var_degrees;

  // 约束作用域（CPU 友好格式）- 用于 DOM/DDEG 启发式
  // constraint_scopes_cpu[cid] = 约束 cid 的变量列表
  std::vector<std::vector<int>> constraint_scopes_cpu;

  // ========================================================================
  // GAC GPU 控制资源（Bitmap Frontier 方案）
  // ========================================================================
  GACControl* d_gac_control = nullptr;      // GPU 控制块
  u32* d_queue_bitmap_A = nullptr;          // Current Frontier
  u32* d_queue_bitmap_B = nullptr;          // Next Frontier
  int bitmap_size_words = 0;                // Bitmap 字数 = (num_constraints + 31) / 32

  // ========================================================================
  // P0-2: NSAC allowed-constraints mask（统一内存）
  // 用途：限制 singleton test 的传播范围到 Xi + N(Xi) 诱导子图
  // 布局：d_allowed_masks[focal_var * constraint_bitmap_words + cid/32]
  //       位 (cid % 32) = 1 表示约束 cid 对 focal_var 的 probe 是 allowed
  // ========================================================================
  u32* d_allowed_masks = nullptr;           // [num_vars * constraint_bitmap_words]
  int constraint_bitmap_words = 0;          // = (num_constraints + 31) / 32
  bool nsac_mask_enabled = false;           // 运行时开关

  // ========================================================================
  // 持久化 Kernel 控制资源（Cooperative Groups）
  // ========================================================================
  PersistentGACControl* d_persistent_control = nullptr;  // 持久化控制块

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

  // ========================================================================
  // GAC 传播接口
  // ========================================================================

  // Bitmap GAC 传播（推荐，高效）
  // assigned_var: 如果 >= 0，表示增量 GAC（只激活该变量邻接约束）
  //               如果 < 0，表示初始 GAC（激活所有约束）
  GacStats EnforceGAC(bool verbose = true, int assigned_var = -1);

  // 持久化 Kernel GAC 传播（使用 Cooperative Groups，消除多轮启动开销）
  // 仅在支持 Cooperative Launch 的设备上可用
  // assigned_var: 如果 >= 0，表示增量 GAC（只激活该变量邻接约束）
  //               如果 < 0，表示初始 GAC（激活所有约束）
  GacStats EnforceGAC_Persistent(bool verbose = true, int assigned_var = -1);

  // 旧版基线 GAC 传播：CPU 队列 + GPU CsCheckMain（已弃用）
  [[deprecated("Use EnforceGAC(verbose, assigned_var) instead")]]
  GacStats EnforceGAC_Legacy(bool verbose = true);

  // GPU 资源初始化/释放
  void InitializeGPUResources();
  void FreeGPUResources();

  // 持久化 Kernel 资源初始化/释放
  void InitializeGPUResources_Persistent();
  void FreeGPUResources_Persistent();

  // 获取 GModel 数据视图（传入 kernel 使用）
  GModelData GetGModelDataView() const;

  // ========================================================================
  // P0-2: NSAC allowed-constraints mask
  // ========================================================================

  // 构建 NSAC allowed-constraints mask（一次性预计算）
  // 对每个变量 Xi，计算其邻域 S_i = {Xi} ∪ neighbors(Xi)
  // 约束 cid(u,v) 在 Xi 的 allowed_mask 中置位当且仅当 u,v ∈ S_i
  void BuildAllowedMasks();

  // 检查 NSAC mask 是否已构建
  bool IsAllowedMasksBuilt() const { return d_allowed_masks != nullptr; }

  // 设置 NSAC mask 启用状态
  void SetNSACMaskEnabled(bool enabled) { nsac_mask_enabled = enabled; }
  bool IsNSACMaskEnabled() const { return nsac_mask_enabled; }

  // ========================================================================
  // Phase 1.2: Trail 回溯支持（单层域 + Trail）
  // ========================================================================

  // 层级管理（委托给 Trail）
  void NewLevel();                           // 进入新层级（O(1)）
  void BacktrackTo(int target_level);        // 回溯到指定层级（O(1)）
  int GetCurrentLevel() const;               // 获取当前层级

  // 域操作（单层域 + Trail 记录）
  // 注意：域修改会自动记录到 Trail，回溯时恢复
  bool AssignValue(int var, int value);      // 赋值变量
  bool RemoveValue(int var, int value);      // 删除域值
  int GetDomainSize(int var) const;          // 获取域大小

  // ========================================================================
  // 求解器辅助方法
  // ========================================================================

  // 选择最小域变量（启发式）
  // 返回 -1 表示所有变量都已赋值
  int GetMinDomainVar() const;

  // 检查变量是否已赋值（域大小 = 1）
  bool IsAssigned(int var) const;

  // 检查所有变量是否都已赋值
  bool IsFullyAssigned() const;

  // 获取已赋值变量的唯一值
  // 前提：IsAssigned(var) == true
  // 返回 -1 表示变量未赋值或域为空
  int GetAssignedValue(int var) const;

  // 获取变量域中的第一个值
  // 返回 -1 表示域为空
  int GetFirstValue(int var) const;

  // 获取变量域中的下一个值
  // 返回 -1 表示没有更多值
  int GetNextValue(int var, int value) const;

  // Phase 1.2: 辅助方法：获取位域索引（单层域，去掉 level 参数）
  inline int GetBitDomIndex(int var, int word) const {
    return var * bit_dom_int_size + word;
  }

  // ========================================================================
  // Phase 2.x: Batch AC-GPU 辅助方法
  // ========================================================================

  // 获取 GModelData 视图（用于 Batch Probe Kernel）
  GModelData GetModelData() const { return GetGModelDataView(); }

  // 获取当前域指针（用于快照保存）
  const u32* GetBitDom() const { return bitDom; }
  u32* GetBitDomMutable() { return bitDom; }  // 可修改版本（避免 const_cast）

  // 获取基础参数（用于 BatchProbeManager）
  int GetNumVars() const { return num_vars; }
  int GetNumCons() const { return num_constraints; }
  int GetBitDomIntSize() const { return bit_dom_int_size; }

  // 批量恢复域大小（用于 Batch AC 状态恢复）
  void RestoreDomainSizes(const int* sizes, int count);

  // 获取域大小数组指针（用于快照保存）
  int* GetDomainSizesPtr() { return d_cur_dom_size; }
  const int* GetDomainSizesPtr() const { return d_cur_dom_size; }

 private:
  // 纹理后端存储（CUDA Array，对用户不可见）
  cudaArray_t cuArray3D_BitSup = nullptr;

  // Phase 1.2: 移除 current_level_（由 Trail 管理）
  // int current_level_ = 0;  // 已删除，使用 trail_->CurrentLevel()

  // 赋值栈大小（暂时保留，可能还有用）
  int assigned_size_ = 0;

  // 私有构造函数，由 GModelAdapter 调用
  // 接收预分配的内存指针和纹理对象（所有权转移给 GModel）
  // Phase 1.2: 移除 max_depth 参数，添加 trail 参数
  // Phase 1.5: 添加 var_degrees 和 constraint_scopes_cpu 参数
  GModel(int num_vars, int num_constraints, int max_dom_size,
         int bit_dom_int_size, int bit_doms_int_size,
         int bitsup_per_constraint, u32* bitDom, int* d_cur_dom_size,
         int2* d_assigned, uint3* d_subscription, int* d_subscription_offset,
         int subscription_size, cudaTextureObject_t texObj_BitSup,
         cudaArray_t cuArray3D_BitSup, uint2* bitSupData, int2* constraint_scopes,
         std::vector<std::vector<int>> var_to_constraints,
         std::vector<int> initial_dom_sizes,
         std::vector<int> var_degrees,
         std::vector<std::vector<int>> constraint_scopes_cpu,
         UnifiedTrail* trail);

  // 旧的转换逻辑（已移到 GModelAdapter）
  void BuildFromIntermediate(const model::IntermediateModel& im_model);
};

// ============================================================================
// Batch AC-GPU Kernel Wrapper（实现在 GModel.cu）
// ============================================================================
// 启动 PersistentBatchProbeKernel 的 wrapper 函数
// 由 BatchProbeManager 调用，实际 kernel 实现在 GModel.cu
// grid_size 和 block_size 会在 wrapper 内部根据设备能力自动计算
void LaunchPersistentBatchProbeKernelWrapper(
    GModelData model_data,
    BatchProbeControl* control,
    int max_iterations_per_probe);

}  // namespace cpim
