#ifndef CPIM_MODEL_GMODEL_ADAPTER_H_
#define CPIM_MODEL_GMODEL_ADAPTER_H_

#include <cstdint>

// Forward declarations
namespace cpim {
class GModel;
using u32 = uint32_t;
}  // namespace cpim

namespace cpim::model {

class IntermediateModel;

// ============================================================================
// GModelOptions - 配置 GModel 构建过程
// ============================================================================
struct GModelOptions {
  int device_id = 0;
  bool enable_prefetch = false;  // Jetson 默认不需要
  bool skip_non_binary = true;   // 跳过非二元约束
  bool skip_conflicts = false;   // 是否跳过冲突约束（暂未实现）

  // 未来扩展：
  // bool use_texture_memory = false;
  // size_t max_memory_mb = 0;  // 0 = 无限制
};

// ============================================================================
// GModelAdapter - 负责 IntermediateModel → GModel 的数据转换
//
// 设计原则：
// 1. 单一职责：只负责数据转换，不管理 GPU 内存生命周期
// 2. 静态方法：所有方法都是静态的，无状态
// 3. 参考 CModelAdapter：保持一致的 API 风格
// ============================================================================
class GModelAdapter {
 public:
  // 从 IntermediateModel 构建 GModel
  // 这是唯一的公共 API
  static GModel Build(const IntermediateModel& im_model,
                      const GModelOptions& options = {});

 private:
  // 辅助方法：构建 bitDom
  static void BuildBitDom(const IntermediateModel& im, u32* bitDom,
                          int num_vars, int bit_dom_int_size);

  // 辅助方法：构建 bitSup 临时数据（CPU 端）
  static void BuildBitSup(const IntermediateModel& im, uint2* bitSup,
                          int num_constraints, int max_dom_size,
                          int bit_dom_int_size, bool skip_non_binary);

  // 辅助方法：预取 bitDom 到 GPU（可选，Jetson 默认不需要）
  // 注意：bitSup 使用纹理内存，不需要预取
  static void PrefetchBitDomToGPU(u32* bitDom, size_t bitdom_size,
                                  int device_id);
};

}  // namespace cpim::model

#endif  // CPIM_MODEL_GMODEL_ADAPTER_H_
