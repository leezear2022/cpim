#pragma once

#include <optional>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "model/intermediate_model.h"
#include "model/types.h"

namespace cpim::model {

// ============================================================================
// Builder 子类 - 用于流式API
// ============================================================================

class ModelBuilder;  // 前向声明

// Domain Builder
class DomainBuilder {
 public:
  DomainBuilder& WithRange(int min_val, int max_val);
  DomainBuilder& WithValues(std::vector<int> values);

  DomainId Build();

 private:
  friend class ModelBuilder;
  explicit DomainBuilder(ModelBuilder* parent, std::string name);

  ModelBuilder* parent_;
  std::string name_;
  std::optional<DomainValues> values_;
};

// Variable Builder
class VariableBuilder {
 public:
  VariableBuilder& WithDomain(DomainId domain_id);
  VariableBuilder& WithDomainName(absl::string_view domain_name);
  VariableBuilder& WithInitialValue(int value);

  VariableId Build();

 private:
  friend class ModelBuilder;
  explicit VariableBuilder(ModelBuilder* parent, std::string name);

  ModelBuilder* parent_;
  std::string name_;
  std::optional<DomainId> domain_id_;
  std::optional<int> initial_value_;
};

// Constraint Builder
class ConstraintBuilder {
 public:
  // Extension 约束
  ConstraintBuilder& AsExtension(ExtensionConstraint::Semantics semantics,
                                 std::vector<VariableId> scope,
                                 std::vector<std::vector<int>> tuples);

  // Intension 约束
  ConstraintBuilder& AsIntension(std::vector<VariableId> scope,
                                 absl::string_view expression);

  // AllDifferent 约束
  ConstraintBuilder& AsAllDifferent(std::vector<VariableId> scope);

  ConstraintId Build();

 private:
  friend class ModelBuilder;
  explicit ConstraintBuilder(ModelBuilder* parent, std::string name);

  ModelBuilder* parent_;
  std::string name_;
  std::optional<ConstraintData> data_;
};

// ============================================================================
// ModelBuilder - 流式API主类
//
// 使用示例:
//   ModelBuilder builder("MyModel");
//
//   auto d0 = builder.AddDomain("D0").WithRange(0, 10).Build();
//   auto x = builder.AddVariable("x").WithDomain(d0).Build();
//   auto y = builder.AddVariable("y").WithDomain(d0).Build();
//
//   builder.AddConstraint("c1")
//       .AsExtension(Semantics::kSupports, {x, y}, tuples)
//       .Build();
//
//   auto model = std::move(builder).Build();
// ============================================================================

class ModelBuilder {
 public:
  ModelBuilder() = default;
  explicit ModelBuilder(std::string model_name);

  // ==========================================================================
  // 流式API - 添加组件
  // ==========================================================================

  // 添加 Domain
  DomainBuilder AddDomain(absl::string_view name);

  // 添加 Variable
  VariableBuilder AddVariable(absl::string_view name);

  // 添加 Constraint（name可选，会自动生成）
  ConstraintBuilder AddConstraint(absl::string_view name = "");

  // ==========================================================================
  // Relation 管理（XCSP3特有，一个relation可以被多个constraint引用）
  // ==========================================================================

  RelationId AddRelation(int arity, ExtensionConstraint::Semantics semantics,
                         std::vector<std::vector<int>> tuples);

  const Relation& GetRelation(RelationId id) const;

  // ==========================================================================
  // 查询接口
  // ==========================================================================

  std::optional<DomainId> GetDomainByName(absl::string_view name) const;
  std::optional<VariableId> GetVariableByName(absl::string_view name) const;

  // ==========================================================================
  // 构建最终模型（消耗 builder，只能调用一次）
  // ==========================================================================

  absl::StatusOr<IntermediateModel> Build() &&;

 private:
  friend class DomainBuilder;
  friend class VariableBuilder;
  friend class ConstraintBuilder;

  // 内部添加方法（由 Builder 子类调用）
  DomainId AddDomainInternal(std::string name, DomainValues values);
  VariableId AddVariableInternal(std::string name, DomainId domain,
                                 std::optional<int> initial_value);
  ConstraintId AddConstraintInternal(std::string name, ConstraintData data);

  // 验证方法
  absl::Status Validate() const;

  // 数据成员
  std::string model_name_ = "UnnamedModel";

  std::vector<Domain> domains_;
  std::vector<Variable> variables_;
  std::vector<Constraint> constraints_;
  std::vector<Relation> relations_;

  // 名称索引（使用 absl::flat_hash_map）
  absl::flat_hash_map<std::string, DomainId> domain_name_index_;
  absl::flat_hash_map<std::string, VariableId> variable_name_index_;

  // 自动命名计数器
  int auto_constraint_counter_ = 0;

  // 标记是否已经构建（防止重复构建）
  bool built_ = false;
};

}  // namespace cpim::model
