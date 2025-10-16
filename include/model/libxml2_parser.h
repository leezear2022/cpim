#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <libxml/parser.h>
#include <libxml/tree.h>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "model/model_builder.h"
#include "model/xcsp_parser.h"

namespace cpim::model {

// ============================================================================
// LibXml2Parser - 使用 libxml2 解析 XCSP3 文件
// ============================================================================

class LibXml2Parser : public XcspParser {
 public:
  LibXml2Parser() = default;
  ~LibXml2Parser() override = default;

  // 实现 XcspParser 接口
  absl::StatusOr<IntermediateModel> Parse(
      std::filesystem::path path) override;

 private:
  // ==========================================================================
  // RAII 资源管理类
  // ==========================================================================

  // XML Document RAII Guard
  class XmlDocGuard {
   public:
    explicit XmlDocGuard(const std::filesystem::path& path);
    ~XmlDocGuard();

    // 禁止拷贝
    XmlDocGuard(const XmlDocGuard&) = delete;
    XmlDocGuard& operator=(const XmlDocGuard&) = delete;

    // 支持移动
    XmlDocGuard(XmlDocGuard&& other) noexcept;
    XmlDocGuard& operator=(XmlDocGuard&& other) noexcept;

    xmlDocPtr get() const { return doc_; }
    xmlNodePtr root() const;
    bool IsValid() const { return doc_ != nullptr; }

   private:
    xmlDocPtr doc_ = nullptr;
  };

  // ==========================================================================
  // 解析各个部分
  // ==========================================================================

  // 获取 BMPath.xml 中的实际 benchmark 文件路径
  absl::StatusOr<std::string> GetBenchmarkPath(xmlNodePtr root);

  // 解析 <domains> 部分
  absl::Status ParseDomains(xmlNodePtr root, ModelBuilder& builder);

  // 解析 <variables> 部分
  absl::Status ParseVariables(xmlNodePtr root, ModelBuilder& builder);

  // 解析 <relations> 部分
  absl::Status ParseRelations(xmlNodePtr root, ModelBuilder& builder);

  // 解析 <constraints> 部分
  absl::Status ParseConstraints(xmlNodePtr root, ModelBuilder& builder);

  // ==========================================================================
  // 辅助函数 - XML 操作
  // ==========================================================================

  // 查找子节点（按名称）
  xmlNodePtr FindChildNode(xmlNodePtr parent, absl::string_view name);

  // 查找所有子节点（按名称）
  std::vector<xmlNodePtr> FindChildNodes(xmlNodePtr parent,
                                         absl::string_view name);

  // 获取节点属性
  absl::StatusOr<std::string> GetAttribute(xmlNodePtr node,
                                           absl::string_view attr_name);

  // 获取节点属性（整数）
  absl::StatusOr<int> GetIntAttribute(xmlNodePtr node,
                                      absl::string_view attr_name);

  // 获取节点文本内容
  absl::StatusOr<std::string> GetNodeContent(xmlNodePtr node);

  // ==========================================================================
  // 辅助函数 - 数据解析
  // ==========================================================================

  // 解析 domain 值: "0..10" 或 "1 3 5 7"
  absl::StatusOr<DomainValues> ParseDomainValues(absl::string_view values_str);

  // 解析 scope 字符串: "V0 V1 V2" -> [VariableId{0}, VariableId{1}, VariableId{2}]
  absl::StatusOr<std::vector<VariableId>> ParseScope(
      absl::string_view scope_str, const ModelBuilder& builder);

  // 解析 tuple 字符串: "1 2 3 | 4 5 6 | ..." -> [[1,2,3], [4,5,6], ...]
  absl::StatusOr<std::vector<std::vector<int>>> ParseTuples(
      absl::string_view tuple_str, int arity, int num_tuples);

  // 从变量名提取数字: "V123" -> 123
  absl::StatusOr<int> ExtractNumberFromString(absl::string_view str);
};

}  // namespace cpim::model
