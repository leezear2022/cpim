#ifndef CPIM_MODEL_GMODEL_VALIDATOR_H_
#define CPIM_MODEL_GMODEL_VALIDATOR_H_

#include <string>
#include <vector>

// Forward declaration
namespace cpim {
class GModel;
}

namespace cpim::model {

class IntermediateModel;

// ============================================================================
// ValidationResult - 验证结果
// ============================================================================
struct ValidationResult {
  bool success = true;
  std::vector<std::string> errors;
  std::vector<std::string> warnings;

  void AddError(const std::string& msg) {
    errors.push_back(msg);
    success = false;
  }

  void AddWarning(const std::string& msg) { warnings.push_back(msg); }

  std::string ToString() const {
    std::string result;
    if (!errors.empty()) {
      result += "Errors:\n";
      for (const auto& err : errors) {
        result += "  - " + err + "\n";
      }
    }
    if (!warnings.empty()) {
      result += "Warnings:\n";
      for (const auto& warn : warnings) {
        result += "  - " + warn + "\n";
      }
    }
    if (success && warnings.empty()) {
      result = "Validation passed with no errors or warnings.\n";
    }
    return result;
  }
};

// ============================================================================
// GModelValidator - 验证 GModel 的正确性
//
// 设计原则：
// 1. 静态方法：所有方法都是静态的，无状态
// 2. 详细报告：提供清晰的错误和警告信息
// 3. 可选验证：可以选择性地运行不同级别的验证
// ============================================================================
class GModelValidator {
 public:
  // 基础验证：检查内存指针、维度一致性
  static ValidationResult ValidateBasic(const cpim::GModel& gmodel);

  // 验证 bitDom 一致性（与 IntermediateModel 对比）
  static ValidationResult ValidateBitDom(const cpim::GModel& gmodel,
                                         const IntermediateModel& im_model);

  // 验证 bitSup 一致性（与 IntermediateModel 对比）
  static ValidationResult ValidateBitSup(const cpim::GModel& gmodel,
                                         const IntermediateModel& im_model);

  // GPU 内存可访问性验证（调用 CUDA kernel）
  static ValidationResult ValidateGPUMemory(const cpim::GModel& gmodel);
};

}  // namespace cpim::model

#endif  // CPIM_MODEL_GMODEL_VALIDATOR_H_
