#pragma once

#include <filesystem>
#include <memory>

#include "absl/status/statusor.h"

namespace cpim::model {

// 前向声明
class IntermediateModel;

// ============================================================================
// 解析器类型枚举
// ============================================================================

enum class ParserType {
  kLibXml2,     // libxml2 实现（默认）
  kPugiXml,     // pugixml 实现（可选，未来扩展）
  kTinyXml2,    // tinyxml2 实现（可选，未来扩展）
};

// ============================================================================
// XCSP3 解析器抽象接口 (Strategy Pattern)
// ============================================================================

class XcspParser {
 public:
  virtual ~XcspParser() = default;

  // 解析XCSP3文件，返回中间模型
  // 如果解析失败，返回错误状态
  virtual absl::StatusOr<IntermediateModel> Parse(
      std::filesystem::path path) = 0;

  // 工厂方法：根据类型创建解析器
  static std::unique_ptr<XcspParser> Create(
      ParserType type = ParserType::kLibXml2);

 protected:
  XcspParser() = default;

  // 禁止拷贝和移动
  XcspParser(const XcspParser&) = delete;
  XcspParser& operator=(const XcspParser&) = delete;
  XcspParser(XcspParser&&) = delete;
  XcspParser& operator=(XcspParser&&) = delete;
};

}  // namespace cpim::model
