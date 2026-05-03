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
// LibXml2Parser - 使用 libxml2 解析 XCSP 文件
// ============================================================================

class LibXml2Parser : public XcspParser {
 public:
  LibXml2Parser() = default;
  ~LibXml2Parser() override = default;

  absl::StatusOr<IntermediateModel> Parse(
      std::filesystem::path path) override;

  absl::StatusOr<BenchEntry> DescribeBenchPath(
      std::filesystem::path path) override;

  absl::StatusOr<std::vector<BenchEntry>> LoadBenchManifest(
      std::filesystem::path manifest_path) override;

 private:
  class XmlDocGuard {
   public:
    explicit XmlDocGuard(const std::filesystem::path& path);
    ~XmlDocGuard();

    XmlDocGuard(const XmlDocGuard&) = delete;
    XmlDocGuard& operator=(const XmlDocGuard&) = delete;

    XmlDocGuard(XmlDocGuard&& other) noexcept;
    XmlDocGuard& operator=(XmlDocGuard&& other) noexcept;

    xmlDocPtr get() const { return doc_; }
    xmlNodePtr root() const;
    bool IsValid() const { return doc_ != nullptr; }

   private:
    xmlDocPtr doc_ = nullptr;
  };

  absl::Status ParseDomains(xmlNodePtr root, ModelBuilder& builder);
  absl::Status ParseVariables(xmlNodePtr root, ModelBuilder& builder);
  absl::Status ParseRelations(xmlNodePtr root, ModelBuilder& builder);
  absl::Status ParseConstraints(xmlNodePtr root, ModelBuilder& builder);

  xmlNodePtr FindChildNode(xmlNodePtr parent, absl::string_view name);
  std::vector<xmlNodePtr> FindChildNodes(xmlNodePtr parent,
                                         absl::string_view name);
  absl::StatusOr<std::string> GetAttribute(xmlNodePtr node,
                                           absl::string_view attr_name);
  absl::StatusOr<int> GetIntAttribute(xmlNodePtr node,
                                      absl::string_view attr_name);
  absl::StatusOr<std::string> GetNodeContent(xmlNodePtr node);

  absl::StatusOr<DomainValues> ParseDomainValues(absl::string_view values_str);
  absl::StatusOr<std::vector<int>> ExpandDomainToken(
      absl::string_view token);
  absl::StatusOr<std::vector<VariableId>> ParseScope(
      absl::string_view scope_str, const ModelBuilder& builder);
  absl::StatusOr<std::vector<std::vector<int>>> ParseTuples(
      absl::string_view tuple_str, int arity, int num_tuples);
  absl::StatusOr<int> ExtractNumberFromString(absl::string_view str);

  // Bench 元数据相关助手
  absl::StatusOr<BenchEntry> BuildBenchEntry(const std::filesystem::path& original,
                                             const std::filesystem::path& resolved);
  absl::Status CollectDirectoryFiles(const std::filesystem::path& dir,
                                     BenchEntry& entry);
  absl::StatusOr<std::string> DetectFormatVersion(
      const std::filesystem::path& file);
};

}  // namespace cpim::model
