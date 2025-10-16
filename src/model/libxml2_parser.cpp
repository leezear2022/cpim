#include "model/libxml2_parser.h"

#include <sstream>

#include "absl/strings/numbers.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_split.h"
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
  LOG(INFO) << "Parsing XCSP3 file: " << path;

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

  // 检查是否是 BMPath.xml（间接引用文件）
  xmlNodePtr bmfile_node = FindChildNode(root, "BMFile");
  if (bmfile_node) {
    // 这是一个 BMPath.xml，需要读取实际的 benchmark 文件
    auto bm_path_or = GetBenchmarkPath(root);
    if (!bm_path_or.ok()) {
      return bm_path_or.status();
    }

    std::filesystem::path actual_path = path.parent_path() / *bm_path_or;
    LOG(INFO) << "Redirecting to actual benchmark file: " << actual_path;

    // 递归解析实际文件
    return Parse(actual_path);
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

// ============================================================================
// 获取 BMPath
// ============================================================================

absl::StatusOr<std::string> LibXml2Parser::GetBenchmarkPath(xmlNodePtr root) {
  xmlNodePtr bmfile_node = FindChildNode(root, "BMFile");
  if (!bmfile_node) {
    return absl::NotFoundError("BMFile node not found");
  }

  auto content_or = GetNodeContent(bmfile_node);
  if (!content_or.ok()) {
    return content_or.status();
  }

  return *content_or;
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

    // 提取 relation ID
    auto rel_idx_or = ExtractNumberFromString(*ref_str_or);
    if (!rel_idx_or.ok()) {
      return rel_idx_or.status();
    }

    RelationId rel_id{*rel_idx_or};
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
  // 检查是否是范围格式: "min..max"
  size_t range_pos = values_str.find("..");
  if (range_pos != absl::string_view::npos) {
    // 范围格式
    int min_val, max_val;
    if (!absl::SimpleAtoi(values_str.substr(0, range_pos), &min_val)) {
      return absl::InvalidArgumentError(
          absl::StrFormat("Failed to parse range min: %s", values_str));
    }
    if (!absl::SimpleAtoi(values_str.substr(range_pos + 2), &max_val)) {
      return absl::InvalidArgumentError(
          absl::StrFormat("Failed to parse range max: %s", values_str));
    }
    return RangeDomain{min_val, max_val};
  }

  // 枚举格式: "1 3 5 7" 或 "1,3,5,7"
  std::vector<int> values;
  std::vector<absl::string_view> parts =
      absl::StrSplit(values_str, absl::ByAnyChar(" ,\t\n"), absl::SkipEmpty());

  for (absl::string_view part : parts) {
    int value;
    if (!absl::SimpleAtoi(part, &value)) {
      return absl::InvalidArgumentError(
          absl::StrFormat("Failed to parse domain value: %s", part));
    }
    values.push_back(value);
  }

  if (values.empty()) {
    return absl::InvalidArgumentError("Domain values are empty");
  }

  // 排序并去重
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());

  return EnumeratedDomain{std::move(values)};
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
