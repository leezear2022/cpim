#include "model/model_builder.h"

#include <algorithm>

#include "absl/strings/str_format.h"
#include "glog/logging.h"

namespace cpim::model {

// ============================================================================
// DomainBuilder 实现
// ============================================================================

DomainBuilder::DomainBuilder(ModelBuilder* parent, std::string name)
    : parent_(parent), name_(std::move(name)) {}

DomainBuilder& DomainBuilder::WithRange(int min_val, int max_val) {
  CHECK_LE(min_val, max_val) << "Invalid range: [" << min_val << ", "
                             << max_val << "]";
  values_ = RangeDomain{min_val, max_val};
  return *this;
}

DomainBuilder& DomainBuilder::WithValues(std::vector<int> values) {
  CHECK(!values.empty()) << "Domain values cannot be empty";
  // 排序并去重
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  values_ = EnumeratedDomain{std::move(values)};
  return *this;
}

DomainId DomainBuilder::Build() {
  CHECK(values_.has_value()) << "Domain values not set for domain: " << name_;
  return parent_->AddDomainInternal(std::move(name_), std::move(*values_));
}

// ============================================================================
// VariableBuilder 实现
// ============================================================================

VariableBuilder::VariableBuilder(ModelBuilder* parent, std::string name)
    : parent_(parent), name_(std::move(name)) {}

VariableBuilder& VariableBuilder::WithDomain(DomainId domain_id) {
  domain_id_ = domain_id;
  return *this;
}

VariableBuilder& VariableBuilder::WithDomainName(absl::string_view domain_name) {
  auto domain_id = parent_->GetDomainByName(domain_name);
  CHECK(domain_id.has_value()) << "Domain not found: " << domain_name;
  domain_id_ = *domain_id;
  return *this;
}

VariableBuilder& VariableBuilder::WithInitialValue(int value) {
  initial_value_ = value;
  return *this;
}

VariableId VariableBuilder::Build() {
  CHECK(domain_id_.has_value()) << "Domain not set for variable: " << name_;
  return parent_->AddVariableInternal(std::move(name_), *domain_id_,
                                      initial_value_);
}

// ============================================================================
// ConstraintBuilder 实现
// ============================================================================

ConstraintBuilder::ConstraintBuilder(ModelBuilder* parent, std::string name)
    : parent_(parent), name_(std::move(name)) {}

ConstraintBuilder& ConstraintBuilder::AsExtension(
    ExtensionConstraint::Semantics semantics, std::vector<VariableId> scope,
    std::vector<std::vector<int>> tuples) {
  ExtensionConstraint constraint;
  constraint.semantics = semantics;
  constraint.scope.assign(scope.begin(), scope.end());
  constraint.tuples = std::move(tuples);
  data_ = std::move(constraint);
  return *this;
}

ConstraintBuilder& ConstraintBuilder::AsIntension(
    std::vector<VariableId> scope, absl::string_view expression) {
  IntensionConstraint constraint;
  constraint.scope.assign(scope.begin(), scope.end());
  constraint.expression = std::string(expression);
  data_ = std::move(constraint);
  return *this;
}

ConstraintBuilder& ConstraintBuilder::AsAllDifferent(
    std::vector<VariableId> scope) {
  AllDifferentConstraint constraint;
  constraint.scope = std::move(scope);
  data_ = std::move(constraint);
  return *this;
}

ConstraintId ConstraintBuilder::Build() {
  CHECK(data_.has_value()) << "Constraint data not set for: " << name_;
  return parent_->AddConstraintInternal(std::move(name_), std::move(*data_));
}

// ============================================================================
// ModelBuilder 实现
// ============================================================================

ModelBuilder::ModelBuilder(std::string model_name)
    : model_name_(std::move(model_name)) {}

DomainBuilder ModelBuilder::AddDomain(absl::string_view name) {
  CHECK(!built_) << "Cannot add domain after model is built";
  return DomainBuilder(this, std::string(name));
}

VariableBuilder ModelBuilder::AddVariable(absl::string_view name) {
  CHECK(!built_) << "Cannot add variable after model is built";
  return VariableBuilder(this, std::string(name));
}

ConstraintBuilder ModelBuilder::AddConstraint(absl::string_view name) {
  CHECK(!built_) << "Cannot add constraint after model is built";

  std::string constraint_name;
  if (name.empty()) {
    // 自动生成名称
    constraint_name = absl::StrFormat("C%d", auto_constraint_counter_++);
  } else {
    constraint_name = std::string(name);
  }

  return ConstraintBuilder(this, std::move(constraint_name));
}

RelationId ModelBuilder::AddRelation(
    int arity, ExtensionConstraint::Semantics semantics,
    std::vector<std::vector<int>> tuples) {
  CHECK(!built_) << "Cannot add relation after model is built";

  RelationId id{static_cast<int>(relations_.size())};
  Relation relation;
  relation.id = id;
  relation.arity = arity;
  relation.semantics = semantics;
  relation.tuples = std::move(tuples);

  relations_.push_back(std::move(relation));
  return id;
}

const Relation& ModelBuilder::GetRelation(RelationId id) const {
  CHECK(HasRelation(id)) << "Invalid relation ID: " << id.value;
  return relations_[id.value];
}

bool ModelBuilder::HasRelation(RelationId id) const {
  return id.IsValid() &&
         id.value >= 0 &&
         static_cast<size_t>(id.value) < relations_.size();
}

std::optional<DomainId> ModelBuilder::GetDomainByName(
    absl::string_view name) const {
  auto it = domain_name_index_.find(name);
  if (it != domain_name_index_.end()) {
    return it->second;
  }
  return std::nullopt;
}

std::optional<VariableId> ModelBuilder::GetVariableByName(
    absl::string_view name) const {
  auto it = variable_name_index_.find(name);
  if (it != variable_name_index_.end()) {
    return it->second;
  }
  return std::nullopt;
}

DomainId ModelBuilder::AddDomainInternal(std::string name,
                                         DomainValues values) {
  // 检查名称是否已存在
  if (domain_name_index_.contains(name)) {
    LOG(WARNING) << "Domain name already exists: " << name;
  }

  DomainId id{static_cast<int>(domains_.size())};
  Domain domain;
  domain.id = id;
  domain.name = name;
  domain.values = std::move(values);

  domain_name_index_[domain.name] = id;
  domains_.push_back(std::move(domain));

  return id;
}

VariableId ModelBuilder::AddVariableInternal(
    std::string name, DomainId domain, std::optional<int> initial_value) {
  // 检查名称是否已存在
  if (variable_name_index_.contains(name)) {
    LOG(WARNING) << "Variable name already exists: " << name;
  }

  // 检查 domain 是否有效
  CHECK(domain.IsValid() && domain.value < domains_.size())
      << "Invalid domain ID: " << domain.value;

  // 如果设置了初始值，检查是否在域内
  if (initial_value.has_value()) {
    const auto& domain_obj = domains_[domain.value];
    CHECK(domain_obj.Contains(*initial_value))
        << "Initial value " << *initial_value << " not in domain " << domain_obj.name;
  }

  VariableId id{static_cast<int>(variables_.size())};
  Variable variable;
  variable.id = id;
  variable.name = name;
  variable.domain = domain;
  variable.initial_value = initial_value;

  variable_name_index_[variable.name] = id;
  variables_.push_back(std::move(variable));

  return id;
}

ConstraintId ModelBuilder::AddConstraintInternal(std::string name,
                                                 ConstraintData data) {
  // 验证 scope 中的变量ID
  auto scope = std::visit(
      [](const auto& c) -> absl::Span<const VariableId> {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, ExtensionConstraint>) {
          return absl::MakeConstSpan(c.scope);
        } else if constexpr (std::is_same_v<T, IntensionConstraint>) {
          return absl::MakeConstSpan(c.scope);
        } else if constexpr (std::is_same_v<T, AllDifferentConstraint>) {
          return absl::MakeConstSpan(c.scope);
        }
        static const std::vector<VariableId> empty;
        return absl::MakeConstSpan(empty);
      },
      data);

  for (VariableId var_id : scope) {
    CHECK(var_id.IsValid() && var_id.value < variables_.size())
        << "Invalid variable ID in constraint scope: " << var_id.value;
  }

  ConstraintId id{static_cast<int>(constraints_.size())};
  Constraint constraint;
  constraint.id = id;
  constraint.name = std::move(name);
  constraint.data = std::move(data);

  constraints_.push_back(std::move(constraint));

  return id;
}

absl::Status ModelBuilder::Validate() const {
  // 检查是否有变量
  if (variables_.empty()) {
    return absl::InvalidArgumentError("Model has no variables");
  }

  // 检查是否有域
  if (domains_.empty()) {
    return absl::InvalidArgumentError("Model has no domains");
  }

  // 检查所有变量的domain是否有效
  for (const auto& var : variables_) {
    if (!var.domain.IsValid() || var.domain.value >= domains_.size()) {
      return absl::InvalidArgumentError(
          absl::StrFormat("Variable %s has invalid domain ID: %d", var.name,
                          var.domain.value));
    }
  }

  // 检查所有约束的scope是否有效
  for (const auto& constraint : constraints_) {
    auto scope = constraint.GetScope();
    for (VariableId var_id : scope) {
      if (!var_id.IsValid() || var_id.value >= variables_.size()) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "Constraint %s has invalid variable ID in scope: %d",
            constraint.name, var_id.value));
      }
    }
  }

  return absl::OkStatus();
}

absl::StatusOr<IntermediateModel> ModelBuilder::Build() && {
  CHECK(!built_) << "Model already built";
  built_ = true;

  // 验证模型
  auto status = Validate();
  if (!status.ok()) {
    return status;
  }

  LOG(INFO) << absl::StrFormat(
      "Building model '%s' with %d variables, %d constraints, %d domains",
      model_name_, variables_.size(), constraints_.size(), domains_.size());

  // 构建 IntermediateModel（使用私有构造函数）
  return IntermediateModel(std::move(model_name_), std::move(domains_),
                           std::move(variables_), std::move(constraints_),
                           std::move(relations_));
}

}  // namespace cpim::model
