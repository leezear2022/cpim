#include "model/libxml2_parser.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <system_error>

#include "absl/strings/ascii.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_split.h"
#include "absl/strings/strip.h"
#include "glog/logging.h"

namespace cpim::model {

// ============================================================================
// XmlDocGuard 实现（RAII资源管理）
// ============================================================================

LibXml2Parser::XmlDocGuard::XmlDocGuard(const std::filesystem::path& path) {
  // 初始化 libxml2（线程安全）
  xmlInitParser();

  // 解析 XML 文件
  doc_ = xmlReadFile(path.c_str(), nullptr, 0);

  if (!doc_) {
    LOG(ERROR) << "Failed to parse XML file: " << path;
  }
}

LibXml2Parser::XmlDocGuard::~XmlDocGuard() {
  if (doc_) {
    xmlFreeDoc(doc_);
    doc_ = nullptr;
  }
  // 清理 libxml2
  xmlCleanupParser();
}

LibXml2Parser::XmlDocGuard::XmlDocGuard(XmlDocGuard&& other) noexcept
    : doc_(other.doc_) {
  other.doc_ = nullptr;
}

LibXml2Parser::XmlDocGuard& LibXml2Parser::XmlDocGuard::operator=(
    XmlDocGuard&& other) noexcept {
  if (this != &other) {
    if (doc_) {
      xmlFreeDoc(doc_);
    }
    doc_ = other.doc_;
    other.doc_ = nullptr;
  }
  return *this;
}

xmlNodePtr LibXml2Parser::XmlDocGuard::root() const {
  if (!doc_) return nullptr;
  return xmlDocGetRootElement(doc_);
}

// ============================================================================
// Parse 主函数
// ============================================================================

absl::StatusOr<IntermediateModel> LibXml2Parser::Parse(
    std::filesystem::path path) {
  LOG(INFO) << "Parsing XCSP file: " << path;

  // 检查文件是否存在
  if (!std::filesystem::exists(path)) {
    return absl::NotFoundError(
        absl::StrFormat("File not found: %s", path.string()));
  }

  // 解析 XML 文件（RAII管理）
  XmlDocGuard doc_guard(path);
  if (!doc_guard.IsValid()) {
    return absl::InvalidArgumentError(
        absl::StrFormat("Failed to parse XML file: %s", path.string()));
  }

  xmlNodePtr root = doc_guard.root();
  if (!root) {
    return absl::InvalidArgumentError("XML document has no root element");
  }

  // 检查是否是 bench manifest（例如 BMPath.xml）
  xmlNodePtr bmfile_node = FindChildNode(root, "BMFile");
  if (bmfile_node) {
    auto entries_or = LoadBenchManifest(path);
    if (!entries_or.ok()) {
      return entries_or.status();
    }
    if (entries_or->empty()) {
      return absl::InvalidArgumentError(
          "No BMFile entries found in manifest");
    }

    const BenchEntry* first_entry_with_file = nullptr;
    for (const auto& entry : *entries_or) {
      if (!entry.files.empty()) {
        first_entry_with_file = &entry;
        break;
      }
    }
    if (!first_entry_with_file) {
      return absl::InvalidArgumentError(
          "Manifest did not resolve to any XML files");
    }

    const auto& first_file = first_entry_with_file->files.front().path;
    LOG(INFO) << "Redirecting to actual benchmark file: " << first_file;

    return Parse(first_file);
  }

  // 创建 ModelBuilder
  std::string model_name = path.stem().string();
  ModelBuilder builder(model_name);

  // 解析各个部分
  auto status = ParseDomains(root, builder);
  if (!status.ok()) {
    return status;
  }

  status = ParseVariables(root, builder);
  if (!status.ok()) {
    return status;
  }

  status = ParseRelations(root, builder);
  if (!status.ok()) {
    return status;
  }

  status = ParseConstraints(root, builder);
  if (!status.ok()) {
    return status;
  }

  // 构建最终模型
  auto model_or = std::move(builder).Build();
  if (!model_or.ok()) {
    return model_or.status();
  }

  LOG(INFO) << absl::StrFormat(
      "Successfully parsed model: %d variables, %d constraints",
      model_or->num_variables(), model_or->num_constraints());

  return std::move(*model_or);
}

absl::StatusOr<BenchEntry> LibXml2Parser::DescribeBenchPath(
    std::filesystem::path path) {
  std::filesystem::path resolved = path;
  if (resolved.is_relative()) {
    resolved = std::filesystem::absolute(resolved);
  }
  return BuildBenchEntry(path, resolved);
}

absl::StatusOr<std::vector<BenchEntry>> LibXml2Parser::LoadBenchManifest(
    std::filesystem::path manifest_path) {
  std::filesystem::path resolved_manifest = manifest_path;
  std::vector<std::filesystem::path> candidates;

  if (resolved_manifest.is_absolute()) {
    candidates.push_back(resolved_manifest);
  } else {
    candidates.push_back(std::filesystem::absolute(resolved_manifest));
    std::filesystem::path parent_candidate =
        std::filesystem::current_path().parent_path() / resolved_manifest;
    candidates.push_back(std::filesystem::absolute(parent_candidate));
  }

  std::filesystem::path manifest_to_use;
  std::error_code ec;
  for (const auto& candidate : candidates) {
    ec.clear();
    if (candidate.empty()) {
      continue;
    }
    if (std::filesystem::exists(candidate, ec)) {
      manifest_to_use = candidate;
      break;
    }
    if (ec) {
      return absl::InternalError(absl::StrFormat(
          "Failed to check manifest existence %s: %s", candidate.string(),
          ec.message()));
    }
  }

  if (manifest_to_use.empty()) {
    return absl::NotFoundError(absl::StrFormat(
        "Bench manifest not found: %s",
        candidates.empty() ? manifest_path.string()
                            : candidates.front().string()));
  }

  resolved_manifest = manifest_to_use;

  XmlDocGuard doc_guard(resolved_manifest);
  if (!doc_guard.IsValid()) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "Failed to parse XML file: %s", resolved_manifest.string()));
  }

  xmlNodePtr root = doc_guard.root();
  if (!root) {
    return absl::InvalidArgumentError(
        "Bench manifest has no root element");
  }

  xmlNodePtr bmfiles_node = FindChildNode(root, "BMFiles");
  if (!bmfiles_node) {
    return absl::InvalidArgumentError(
        "Bench manifest missing <BMFiles> section");
  }

  auto bm_nodes = FindChildNodes(bmfiles_node, "BMFile");
  std::vector<BenchEntry> entries;
  entries.reserve(bm_nodes.size());

  for (xmlNodePtr node : bm_nodes) {
    auto content_or = GetNodeContent(node);
    if (!content_or.ok()) {
      return content_or.status();
    }

    std::string raw_path = std::string(absl::StripAsciiWhitespace(*content_or));
    if (raw_path.empty()) {
      continue;
    }

    std::filesystem::path original = raw_path;

    std::vector<std::filesystem::path> child_candidates;
    child_candidates.push_back(resolved_manifest.parent_path() / original);
    if (!original.is_absolute()) {
      child_candidates.push_back(std::filesystem::absolute(original));
      child_candidates.push_back(std::filesystem::absolute(
          resolved_manifest.parent_path().parent_path() / original));
    }

    absl::StatusOr<BenchEntry> entry_or;
    for (const auto& candidate : child_candidates) {
      entry_or = BuildBenchEntry(original, candidate);
      if (entry_or.ok()) {
        break;
      }
      if (!absl::IsNotFound(entry_or.status())) {
        return entry_or.status();
      }
    }

    if (!entry_or.ok()) {
      return entry_or.status();
    }

    auto type_attr_or = GetAttribute(node, "type");
    if (type_attr_or.ok()) {
      std::string type_lower = absl::AsciiStrToLower(*type_attr_or);
      BenchPathKind expected =
          (type_lower == "dir" || type_lower == "directory" ||
           type_lower == "folder")
              ? BenchPathKind::kDirectory
              : BenchPathKind::kFile;
      if (entry_or->kind != expected) {
        return absl::FailedPreconditionError(absl::StrFormat(
            "Manifest entry %s expected %s but resolved to %s",
            raw_path,
            expected == BenchPathKind::kDirectory ? "directory" : "file",
            entry_or->kind == BenchPathKind::kDirectory ? "directory"
                                                         : "file"));
      }
    }

    entries.push_back(std::move(*entry_or));
  }

  if (entries.empty()) {
    return absl::InvalidArgumentError(
        "Bench manifest did not contain usable BMFile entries");
  }

  return entries;
}

absl::StatusOr<BenchEntry> LibXml2Parser::BuildBenchEntry(
    const std::filesystem::path& original,
    const std::filesystem::path& resolved) {
  BenchEntry entry;
  entry.original_path = original;
  std::filesystem::path resolved_abs = resolved;
  if (resolved_abs.is_relative()) {
    resolved_abs = std::filesystem::absolute(resolved_abs);
  }
  std::error_code norm_ec;
  auto normalized = std::filesystem::weakly_canonical(resolved_abs, norm_ec);
  if (!norm_ec) {
    resolved_abs = std::move(normalized);
  }
  entry.resolved_path = resolved_abs;

  std::error_code ec;
  if (!std::filesystem::exists(resolved_abs, ec)) {
    if (ec) {
      return absl::InternalError(absl::StrFormat(
          "Failed to check path existence %s: %s", resolved_abs.string(),
          ec.message()));
    }
    return absl::NotFoundError(absl::StrFormat(
        "Bench path not found: %s", resolved_abs.string()));
  }

  if (std::filesystem::is_directory(resolved_abs, ec)) {
    if (ec) {
      return absl::InternalError(absl::StrFormat(
          "Failed to inspect directory %s: %s", resolved_abs.string(),
          ec.message()));
    }
    entry.kind = BenchPathKind::kDirectory;
    if (auto status = CollectDirectoryFiles(resolved_abs, entry); !status.ok()) {
      return status;
    }
    return entry;
  }

  if (std::filesystem::is_regular_file(resolved_abs, ec)) {
    if (ec) {
      return absl::InternalError(absl::StrFormat(
          "Failed to inspect file %s: %s", resolved_abs.string(),
          ec.message()));
    }
    entry.kind = BenchPathKind::kFile;
    auto format_or = DetectFormatVersion(resolved_abs);
    if (!format_or.ok()) {
      return format_or.status();
    }
    entry.files.push_back({resolved_abs, *format_or});
    return entry;
  }

  return absl::InvalidArgumentError(absl::StrFormat(
      "Bench path is neither file nor directory: %s", resolved_abs.string()));
}

absl::Status LibXml2Parser::CollectDirectoryFiles(
    const std::filesystem::path& dir, BenchEntry& entry) {
  std::vector<BenchFileInfo> files;
  std::error_code ec;
  for (std::filesystem::recursive_directory_iterator it(dir, ec), end;
       it != end && !ec; it.increment(ec)) {
    if (!it->is_regular_file(ec)) {
      continue;
    }
    if (it->path().extension() != ".xml") {
      continue;
    }
    auto format_or = DetectFormatVersion(it->path());
    if (!format_or.ok()) {
      return format_or.status();
    }
    files.push_back({std::filesystem::absolute(it->path()), *format_or});
  }

  if (ec) {
    return absl::InternalError(absl::StrFormat(
        "Failed to iterate directory %s: %s", dir.string(), ec.message()));
  }

  if (files.empty()) {
    return absl::NotFoundError(absl::StrFormat(
        "No XML benchmarks found under %s", dir.string()));
  }

  entry.files = std::move(files);
  return absl::OkStatus();
}

absl::StatusOr<std::string> LibXml2Parser::DetectFormatVersion(
    const std::filesystem::path& file) {
  XmlDocGuard doc_guard(file);
  if (!doc_guard.IsValid()) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "Failed to parse XML file: %s", file.string()));
  }

  xmlNodePtr root = doc_guard.root();
  if (!root) {
    return absl::InvalidArgumentError(
        "XML document has no root element");
  }

  xmlNodePtr presentation = FindChildNode(root, "presentation");
  if (!presentation) {
    return std::string();
  }

  auto format_attr = GetAttribute(presentation, "format");
  if (!format_attr.ok()) {
    return std::string();
  }

  return *format_attr;
}

// ============================================================================
// 解析 Domains
// ============================================================================

absl::Status LibXml2Parser::ParseDomains(xmlNodePtr root,
                                         ModelBuilder& builder) {
  xmlNodePtr domains_node = FindChildNode(root, "domains");
  if (!domains_node) {
    return absl::NotFoundError("No <domains> element found");
  }

  auto num_domains_or = GetIntAttribute(domains_node, "nbDomains");
  if (!num_domains_or.ok()) {
    LOG(WARNING) << "nbDomains attribute not found, counting manually";
  }

  // 查找所有 <domain> 节点
  auto domain_nodes = FindChildNodes(domains_node, "domain");
  LOG(INFO) << absl::StrFormat("Parsing %d domains", domain_nodes.size());

  for (size_t idx = 0; idx < domain_nodes.size(); ++idx) {
    xmlNodePtr domain_node = domain_nodes[idx];

    // 获取 domain 的 name（如果没有，使用 DX）
    auto name_or = GetAttribute(domain_node, "name");
    std::string domain_name;
    if (name_or.ok()) {
      domain_name = *name_or;
    } else {
      domain_name = absl::StrFormat("D%d", idx);
    }

    // 获取 domain 的值
    auto values_str_or = GetNodeContent(domain_node);
    if (!values_str_or.ok()) {
      return absl::InvalidArgumentError(
          absl::StrFormat("Failed to get domain values for %s", domain_name));
    }

    auto domain_values_or = ParseDomainValues(*values_str_or);
    if (!domain_values_or.ok()) {
      return domain_values_or.status();
    }

    // 添加 domain
    builder.AddDomain(domain_name)
        .WithValues(std::visit(
            [](const auto& d) -> std::vector<int> {
              using T = std::decay_t<decltype(d)>;
              if constexpr (std::is_same_v<T, RangeDomain>) {
                std::vector<int> result;
                for (int i = d.min_value; i <= d.max_value; ++i) {
                  result.push_back(i);
                }
                return result;
              } else {
                return d.values;
              }
            },
            *domain_values_or))
        .Build();
  }

  return absl::OkStatus();
}

// ============================================================================
// 解析 Variables
// ============================================================================

absl::Status LibXml2Parser::ParseVariables(xmlNodePtr root,
                                           ModelBuilder& builder) {
  xmlNodePtr variables_node = FindChildNode(root, "variables");
  if (!variables_node) {
    return absl::NotFoundError("No <variables> element found");
  }

  auto var_nodes = FindChildNodes(variables_node, "variable");
  LOG(INFO) << absl::StrFormat("Parsing %d variables", var_nodes.size());

  for (xmlNodePtr var_node : var_nodes) {
    // 获取变量名
    auto name_or = GetAttribute(var_node, "name");
    if (!name_or.ok()) {
      return absl::InvalidArgumentError("Variable has no name attribute");
    }
    std::string var_name = *name_or;

    // 获取 domain 引用
    auto domain_ref_or = GetAttribute(var_node, "domain");
    if (!domain_ref_or.ok()) {
      return absl::InvalidArgumentError(
          absl::StrFormat("Variable %s has no domain attribute", var_name));
    }

    // domain 引用可能是 "D0" 或 "0"，提取数字
    auto domain_idx_or = ExtractNumberFromString(*domain_ref_or);
    if (!domain_idx_or.ok()) {
      return domain_idx_or.status();
    }

    std::string domain_name = absl::StrFormat("D%d", *domain_idx_or);

    // 添加变量
    builder.AddVariable(var_name).WithDomainName(domain_name).Build();
  }

  return absl::OkStatus();
}

// ============================================================================
// 解析 Relations
// ============================================================================

absl::Status LibXml2Parser::ParseRelations(xmlNodePtr root,
                                           ModelBuilder& builder) {
  xmlNodePtr relations_node = FindChildNode(root, "relations");
  if (!relations_node) {
    // Relations 是可选的
    LOG(INFO) << "No <relations> element found (optional)";
    return absl::OkStatus();
  }

  auto relation_nodes = FindChildNodes(relations_node, "relation");
  LOG(INFO) << absl::StrFormat("Parsing %d relations", relation_nodes.size());

  for (xmlNodePtr rel_node : relation_nodes) {
    // 获取 arity
    auto arity_or = GetIntAttribute(rel_node, "arity");
    if (!arity_or.ok()) {
      return absl::InvalidArgumentError("Relation has no arity attribute");
    }
    int arity = *arity_or;

    // 获取 semantics
    auto semantics_str_or = GetAttribute(rel_node, "semantics");
    if (!semantics_str_or.ok()) {
      return absl::InvalidArgumentError("Relation has no semantics attribute");
    }

    ExtensionConstraint::Semantics semantics;
    if (*semantics_str_or == "supports") {
      semantics = ExtensionConstraint::Semantics::kSupports;
    } else if (*semantics_str_or == "conflicts") {
      semantics = ExtensionConstraint::Semantics::kConflicts;
    } else {
      return absl::InvalidArgumentError(
          absl::StrFormat("Unknown semantics: %s", *semantics_str_or));
    }

    // 获取 nbTuples
    auto num_tuples_or = GetIntAttribute(rel_node, "nbTuples");
    if (!num_tuples_or.ok()) {
      return absl::InvalidArgumentError("Relation has no nbTuples attribute");
    }
    int num_tuples = *num_tuples_or;

    // 获取 tuples 内容
    std::vector<std::vector<int>> tuples;
    if (num_tuples > 0) {
      auto tuples_str_or = GetNodeContent(rel_node);
      if (!tuples_str_or.ok()) {
        return absl::InvalidArgumentError("Failed to get relation tuples");
      }

      auto tuples_or = ParseTuples(*tuples_str_or, arity, num_tuples);
      if (!tuples_or.ok()) {
        return tuples_or.status();
      }
      tuples = std::move(*tuples_or);
    }

    // 添加 relation
    builder.AddRelation(arity, semantics, std::move(tuples));
  }

  return absl::OkStatus();
}

// ============================================================================
// 解析 Constraints
// ============================================================================

absl::Status LibXml2Parser::ParseConstraints(xmlNodePtr root,
                                             ModelBuilder& builder) {
  xmlNodePtr constraints_node = FindChildNode(root, "constraints");
  if (!constraints_node) {
    // 允许没有约束（虽然这很少见）
    LOG(WARNING) << "No <constraints> element found";
    return absl::OkStatus();
  }

  auto constraint_nodes = FindChildNodes(constraints_node, "constraint");
  LOG(INFO) << absl::StrFormat("Parsing %d constraints",
                               constraint_nodes.size());

  for (size_t i = 0; i < constraint_nodes.size(); ++i) {
    xmlNodePtr con_node = constraint_nodes[i];

    // 获取 arity
    auto arity_or = GetIntAttribute(con_node, "arity");
    if (!arity_or.ok()) {
      return absl::InvalidArgumentError(
          absl::StrFormat("Constraint %d has no arity attribute", i));
    }

    // 获取 scope
    auto scope_str_or = GetAttribute(con_node, "scope");
    if (!scope_str_or.ok()) {
      return absl::InvalidArgumentError(
          absl::StrFormat("Constraint %d has no scope attribute", i));
    }

    auto scope_or = ParseScope(*scope_str_or, builder);
    if (!scope_or.ok()) {
      return scope_or.status();
    }

    // 获取 reference（指向 relation）
    auto ref_str_or = GetAttribute(con_node, "reference");
    if (!ref_str_or.ok()) {
      return absl::InvalidArgumentError(
          absl::StrFormat("Constraint %d has no reference attribute", i));
    }
    const std::string ref = *ref_str_or;

    if (ref == "global:allDifferent") {
      return absl::UnimplementedError(absl::StrFormat(
          "global:allDifferent is not supported by Metal GAC v1: "
          "constraint %d",
          i));
    }

    if (!ref.empty() && ref[0] == 'P') {
      return absl::UnimplementedError(absl::StrFormat(
          "Predicate/intension references are not supported yet: "
          "constraint %d reference=%s",
          i, ref));
    }

    // 提取 relation ID
    auto rel_idx_or = ExtractNumberFromString(ref);
    if (!rel_idx_or.ok()) {
      return rel_idx_or.status();
    }

    RelationId rel_id{*rel_idx_or};
    if (!builder.HasRelation(rel_id)) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "Constraint %d references invalid relation ID: %d", i, rel_id.value));
    }
    const auto& relation = builder.GetRelation(rel_id);

    // 创建约束（constraint name 自动生成）
    builder.AddConstraint()
        .AsExtension(relation.semantics, std::move(*scope_or),
                     relation.tuples)
        .Build();
  }

  return absl::OkStatus();
}

// ============================================================================
// XML 操作辅助函数
// ============================================================================

xmlNodePtr LibXml2Parser::FindChildNode(xmlNodePtr parent,
                                        absl::string_view name) {
  if (!parent) return nullptr;

  for (xmlNodePtr child = parent->children; child; child = child->next) {
    if (child->type == XML_ELEMENT_NODE) {
      if (std::string(reinterpret_cast<const char*>(child->name)) ==
          std::string(name)) {
        return child;
      }
    }
  }
  return nullptr;
}

std::vector<xmlNodePtr> LibXml2Parser::FindChildNodes(xmlNodePtr parent,
                                                      absl::string_view name) {
  std::vector<xmlNodePtr> result;
  if (!parent) return result;

  for (xmlNodePtr child = parent->children; child; child = child->next) {
    if (child->type == XML_ELEMENT_NODE) {
      if (std::string(reinterpret_cast<const char*>(child->name)) ==
          std::string(name)) {
        result.push_back(child);
      }
    }
  }
  return result;
}

absl::StatusOr<std::string> LibXml2Parser::GetAttribute(
    xmlNodePtr node, absl::string_view attr_name) {
  if (!node) {
    return absl::InvalidArgumentError("Node is null");
  }

  xmlChar* attr = xmlGetProp(node, BAD_CAST attr_name.data());
  if (!attr) {
    return absl::NotFoundError(
        absl::StrFormat("Attribute '%s' not found", attr_name));
  }

  std::string result(reinterpret_cast<const char*>(attr));
  xmlFree(attr);
  return result;
}

absl::StatusOr<int> LibXml2Parser::GetIntAttribute(xmlNodePtr node,
                                                   absl::string_view attr_name) {
  auto str_or = GetAttribute(node, attr_name);
  if (!str_or.ok()) {
    return str_or.status();
  }

  int result;
  if (!absl::SimpleAtoi(*str_or, &result)) {
    return absl::InvalidArgumentError(
        absl::StrFormat("Failed to parse int from attribute '%s': %s",
                        attr_name, *str_or));
  }
  return result;
}

absl::StatusOr<std::string> LibXml2Parser::GetNodeContent(xmlNodePtr node) {
  if (!node) {
    return absl::InvalidArgumentError("Node is null");
  }

  xmlChar* content = xmlNodeGetContent(node);
  if (!content) {
    return absl::NotFoundError("Node has no content");
  }

  std::string result(reinterpret_cast<const char*>(content));
  xmlFree(content);
  return result;
}

// ============================================================================
// 数据解析辅助函数
// ============================================================================

absl::StatusOr<DomainValues> LibXml2Parser::ParseDomainValues(
    absl::string_view values_str) {
  std::vector<int> values;
  std::vector<absl::string_view> parts =
      absl::StrSplit(values_str, absl::ByAnyChar(" ,\t\n"), absl::SkipEmpty());

  for (absl::string_view part : parts) {
    auto expanded_or = ExpandDomainToken(part);
    if (!expanded_or.ok()) {
      return expanded_or.status();
    }
    values.insert(values.end(), expanded_or->begin(), expanded_or->end());
  }

  if (values.empty()) {
    return absl::InvalidArgumentError("Domain values are empty");
  }

  // 排序并去重
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());

  return EnumeratedDomain{std::move(values)};
}

absl::StatusOr<std::vector<int>> LibXml2Parser::ExpandDomainToken(
    absl::string_view token) {
  size_t range_pos = token.find("..");
  if (range_pos == absl::string_view::npos) {
    int value;
    if (!absl::SimpleAtoi(token, &value)) {
      return absl::InvalidArgumentError(
          absl::StrFormat("Failed to parse domain value: %s", token));
    }
    return std::vector<int>{value};
  }

  int min_val;
  int max_val;
  if (!absl::SimpleAtoi(token.substr(0, range_pos), &min_val)) {
    return absl::InvalidArgumentError(
        absl::StrFormat("Failed to parse range min: %s", token));
  }
  if (!absl::SimpleAtoi(token.substr(range_pos + 2), &max_val)) {
    return absl::InvalidArgumentError(
        absl::StrFormat("Failed to parse range max: %s", token));
  }
  if (min_val > max_val) {
    return absl::InvalidArgumentError(
        absl::StrFormat("Invalid domain range: %s", token));
  }

  std::vector<int> values;
  values.reserve(max_val - min_val + 1);
  for (int value = min_val; value <= max_val; ++value) {
    values.push_back(value);
  }
  return values;
}

absl::StatusOr<std::vector<VariableId>> LibXml2Parser::ParseScope(
    absl::string_view scope_str, const ModelBuilder& builder) {
  std::vector<VariableId> scope;
  std::vector<absl::string_view> parts =
      absl::StrSplit(scope_str, absl::ByAnyChar(" ,\t\n"), absl::SkipEmpty());

  for (absl::string_view part : parts) {
    auto var_id_opt = builder.GetVariableByName(part);
    if (!var_id_opt.has_value()) {
      return absl::NotFoundError(
          absl::StrFormat("Variable not found in scope: %s", part));
    }
    scope.push_back(*var_id_opt);
  }

  return scope;
}

absl::StatusOr<std::vector<std::vector<int>>> LibXml2Parser::ParseTuples(
    absl::string_view tuple_str, int arity, int num_tuples) {
  std::vector<std::vector<int>> tuples;
  tuples.reserve(num_tuples);

  // 按 '|' 分割元组
  std::vector<absl::string_view> tuple_parts =
      absl::StrSplit(tuple_str, '|', absl::SkipEmpty());

  for (absl::string_view tuple_part : tuple_parts) {
    std::vector<int> tuple;
    tuple.reserve(arity);

    std::vector<absl::string_view> value_parts = absl::StrSplit(
        tuple_part, absl::ByAnyChar(" ,\t\n"), absl::SkipEmpty());

    for (absl::string_view value_str : value_parts) {
      int value;
      if (!absl::SimpleAtoi(value_str, &value)) {
        return absl::InvalidArgumentError(
            absl::StrFormat("Failed to parse tuple value: %s", value_str));
      }
      tuple.push_back(value);
    }

    if (static_cast<int>(tuple.size()) != arity) {
      return absl::InvalidArgumentError(
          absl::StrFormat("Tuple size mismatch: expected %d, got %d", arity,
                          tuple.size()));
    }

    tuples.push_back(std::move(tuple));
  }

  return tuples;
}

absl::StatusOr<int> LibXml2Parser::ExtractNumberFromString(
    absl::string_view str) {
  // 查找第一个数字
  size_t digit_pos = 0;
  while (digit_pos < str.size() && !std::isdigit(str[digit_pos])) {
    ++digit_pos;
  }

  if (digit_pos == str.size()) {
    return absl::InvalidArgumentError(
        absl::StrFormat("No number found in string: %s", str));
  }

  int result;
  if (!absl::SimpleAtoi(str.substr(digit_pos), &result)) {
    return absl::InvalidArgumentError(
        absl::StrFormat("Failed to parse number from: %s", str));
  }

  return result;
}

}  // namespace cpim::model
