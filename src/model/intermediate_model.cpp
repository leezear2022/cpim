#include "model/intermediate_model.h"

#include <algorithm>
#include <limits>

#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "glog/logging.h"

namespace cpim::model {

// ============================================================================
// 构造函数和索引构建
// ============================================================================

IntermediateModel::IntermediateModel(std::string name,
                                     std::vector<Domain> domains,
                                     std::vector<Variable> variables,
                                     std::vector<Constraint> constraints,
                                     std::vector<Relation> relations)
    : model_name_(std::move(name)),
      domains_(std::move(domains)),
      variables_(std::move(variables)),
      constraints_(std::move(constraints)),
      relations_(std::move(relations)) {
  BuildIndices();
}

void IntermediateModel::BuildIndices() {
  // 构建变量名称索引
  var_name_index_.reserve(variables_.size());
  for (const auto& var : variables_) {
    var_name_index_[var.name] = var.id;
  }

  // 构建域名称索引
  domain_name_index_.reserve(domains_.size());
  for (const auto& domain : domains_) {
    domain_name_index_[domain.name] = domain.id;
  }

  // 构建变量-约束订阅关系
  var_to_constraints_.resize(variables_.size());
  for (const auto& constraint : constraints_) {
    auto scope = constraint.GetScope();
    for (VariableId var_id : scope) {
      if (var_id.IsValid() && var_id.value < var_to_constraints_.size()) {
        var_to_constraints_[var_id.value].push_back(constraint.id);
      }
    }
  }

  // 构建约束邻接关系
  constraint_neighbors_.resize(constraints_.size());
  for (size_t i = 0; i < constraints_.size(); ++i) {
    absl::flat_hash_set<ConstraintId> neighbors;
    auto scope_i = constraints_[i].GetScope();

    // 遍历scope中的每个变量，找出该变量参与的其他约束
    for (VariableId var : scope_i) {
      if (!var.IsValid() || var.value >= var_to_constraints_.size()) continue;

      for (ConstraintId other_c : var_to_constraints_[var.value]) {
        if (other_c.value != static_cast<int>(i)) {
          neighbors.insert(other_c);
        }
      }
    }

    constraint_neighbors_[i].assign(neighbors.begin(), neighbors.end());
  }
}

// ============================================================================
// 按 ID 查询
// ============================================================================

const Variable& IntermediateModel::GetVariable(VariableId id) const {
  CHECK(id.IsValid() && id.value < variables_.size())
      << "Invalid variable ID: " << id.value;
  return variables_[id.value];
}

const Domain& IntermediateModel::GetDomain(DomainId id) const {
  CHECK(id.IsValid() && id.value < domains_.size())
      << "Invalid domain ID: " << id.value;
  return domains_[id.value];
}

const Constraint& IntermediateModel::GetConstraint(ConstraintId id) const {
  CHECK(id.IsValid() && id.value < constraints_.size())
      << "Invalid constraint ID: " << id.value;
  return constraints_[id.value];
}

const Relation& IntermediateModel::GetRelation(RelationId id) const {
  CHECK(id.IsValid() && id.value < relations_.size())
      << "Invalid relation ID: " << id.value;
  return relations_[id.value];
}

// ============================================================================
// 按名称查询
// ============================================================================

std::optional<VariableId> IntermediateModel::GetVariableByName(
    absl::string_view name) const {
  auto it = var_name_index_.find(name);
  if (it != var_name_index_.end()) {
    return it->second;
  }
  return std::nullopt;
}

std::optional<DomainId> IntermediateModel::GetDomainByName(
    absl::string_view name) const {
  auto it = domain_name_index_.find(name);
  if (it != domain_name_index_.end()) {
    return it->second;
  }
  return std::nullopt;
}

// ============================================================================
// 统计信息
// ============================================================================

ModelStatistics IntermediateModel::GetStatistics() const {
  ModelStatistics stats;

  stats.num_variables = num_variables();
  stats.num_constraints = num_constraints();
  stats.num_domains = num_domains();
  stats.num_relations = num_relations();

  // 统计 domain 大小
  if (!domains_.empty()) {
    stats.max_domain_size = 0;
    stats.min_domain_size = std::numeric_limits<int>::max();

    for (const auto& domain : domains_) {
      int size = domain.Size();
      stats.max_domain_size = std::max(stats.max_domain_size, size);
      stats.min_domain_size = std::min(stats.min_domain_size, size);
    }
  }

  // 统计约束 arity 和类型
  if (!constraints_.empty()) {
    stats.max_arity = 0;
    stats.min_arity = std::numeric_limits<int>::max();

    for (const auto& constraint : constraints_) {
      int arity = constraint.Arity();
      stats.max_arity = std::max(stats.max_arity, arity);
      stats.min_arity = std::min(stats.min_arity, arity);

      // 统计约束类型
      std::string type = GetConstraintTypeName(constraint.data);
      stats.constraint_type_counts[type]++;
    }
  }

  return stats;
}

// ============================================================================
// 拓扑查询
// ============================================================================

absl::Span<const ConstraintId> IntermediateModel::GetConstraintsForVariable(
    VariableId var) const {
  if (!var.IsValid() || var.value >= var_to_constraints_.size()) {
    static const std::vector<ConstraintId> empty;
    return absl::MakeConstSpan(empty);
  }
  return absl::MakeConstSpan(var_to_constraints_[var.value]);
}

std::vector<ConstraintId> IntermediateModel::GetConstraintsBetween(
    VariableId var1, VariableId var2) const {
  if (!var1.IsValid() || !var2.IsValid() ||
      var1.value >= var_to_constraints_.size() ||
      var2.value >= var_to_constraints_.size()) {
    return {};
  }

  // 取两个变量的约束集合的交集
  const auto& constraints1 = var_to_constraints_[var1.value];
  const auto& constraints2 = var_to_constraints_[var2.value];

  std::vector<ConstraintId> result;
  for (ConstraintId c : constraints1) {
    if (std::find(constraints2.begin(), constraints2.end(), c) !=
        constraints2.end()) {
      result.push_back(c);
    }
  }

  return result;
}

bool IntermediateModel::AreConstraintsNeighbors(ConstraintId c1,
                                                ConstraintId c2) const {
  if (!c1.IsValid() || !c2.IsValid() ||
      c1.value >= constraint_neighbors_.size() ||
      c2.value >= constraint_neighbors_.size()) {
    return false;
  }

  const auto& neighbors = constraint_neighbors_[c1.value];
  return std::find(neighbors.begin(), neighbors.end(), c2) != neighbors.end();
}

absl::Span<const ConstraintId> IntermediateModel::GetNeighborConstraints(
    ConstraintId c) const {
  if (!c.IsValid() || c.value >= constraint_neighbors_.size()) {
    static const std::vector<ConstraintId> empty;
    return absl::MakeConstSpan(empty);
  }
  return absl::MakeConstSpan(constraint_neighbors_[c.value]);
}

// ============================================================================
// 输出接口
// ============================================================================

std::string IntermediateModel::ToString() const {
  std::string result;

  result += absl::StrFormat("Model: %s\n", model_name_);
  result += absl::StrFormat("Variables: %d\n", num_variables());
  result += absl::StrFormat("Constraints: %d\n", num_constraints());
  result += absl::StrFormat("Domains: %d\n", num_domains());
  result += absl::StrFormat("Relations: %d\n", num_relations());

  auto stats = GetStatistics();
  result += absl::StrFormat("Domain size: [%d, %d]\n", stats.min_domain_size,
                            stats.max_domain_size);
  result += absl::StrFormat("Arity: [%d, %d]\n", stats.min_arity,
                            stats.max_arity);

  // 打印约束类型分布
  if (!stats.constraint_type_counts.empty()) {
    result += "Constraint types:\n";
    for (const auto& [type, count] : stats.constraint_type_counts) {
      result += absl::StrFormat("  %s: %d\n", type, count);
    }
  }

  return result;
}

void IntermediateModel::Print() const {
  LOG(INFO) << "\n" << ToString();
}

void IntermediateModel::PrintDetailed() const {
  std::string result;

  result += "========== Detailed Model Information ==========\n";
  result += absl::StrFormat("Model: %s\n\n", model_name_);

  // 打印所有域
  result += absl::StrFormat("========== Domains (%d) ==========\n",
                            num_domains());
  int domain_count = 0;
  for (const auto& domain : domains_) {
    result +=
        absl::StrFormat("  [%d] %s: size=%d\n", domain.id.value, domain.name,
                        domain.Size());
    if (++domain_count >= 10 && domains_.size() > 15) {
      result += absl::StrFormat("  ... (%d more domains)\n",
                                static_cast<int>(domains_.size()) - 10);
      break;
    }
  }

  // 打印所有变量
  result += absl::StrFormat("\n========== Variables (%d) ==========\n",
                            num_variables());
  int var_count = 0;
  for (const auto& var : variables_) {
    const auto& domain = GetDomain(var.domain);
    result += absl::StrFormat("  [%d] %s: domain=%s (size=%d)", var.id.value,
                              var.name, domain.name, domain.Size());
    if (var.initial_value.has_value()) {
      result += absl::StrFormat(" init=%d", *var.initial_value);
    }
    result += "\n";

    if (++var_count >= 10 && variables_.size() > 15) {
      result += absl::StrFormat("  ... (%d more variables)\n",
                                static_cast<int>(variables_.size()) - 10);
      break;
    }
  }

  // 打印所有约束
  result += absl::StrFormat("\n========== Constraints (%d) ==========\n",
                            num_constraints());
  int constraint_count = 0;
  for (const auto& constraint : constraints_) {
    auto scope = constraint.GetScope();
    std::vector<std::string> scope_names;
    for (VariableId vid : scope) {
      scope_names.push_back(GetVariable(vid).name);
    }

    std::string type = GetConstraintTypeName(constraint.data);
    result += absl::StrFormat("  [%d] %s: type=%s arity=%d scope={%s}\n",
                              constraint.id.value, constraint.name, type,
                              constraint.Arity(),
                              absl::StrJoin(scope_names, ", "));

    if (++constraint_count >= 10 && constraints_.size() > 15) {
      result += absl::StrFormat("  ... (%d more constraints)\n",
                                static_cast<int>(constraints_.size()) - 10);
      break;
    }
  }

  // 打印统计信息
  result += "\n========== Statistics ==========\n";
  result += GetStatistics().ToString();
  result += "\n";

  LOG(INFO) << "\n" << result;
}

}  // namespace cpim::model
