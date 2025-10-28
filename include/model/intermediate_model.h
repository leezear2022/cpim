#pragma once

#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "model/types.h"

namespace cpim::model {

class ModelNormalizer;

// ============================================================================
// IntermediateModel - 求解器无关的中间模型表示
//
// 设计原则:
// 1. 不可变性: 构建后不可修改（线程安全）
// 2. 高效查询: 预构建索引结构
// 3. 零拷贝视图: 使用 absl::Span
// 4. 求解器无关: 纯数据表示，不含求解逻辑
// ============================================================================

class IntermediateModel {
 public:
  // 禁止拷贝，只允许移动
  IntermediateModel(const IntermediateModel&) = delete;
  IntermediateModel& operator=(const IntermediateModel&) = delete;

  IntermediateModel(IntermediateModel&&) noexcept = default;
  IntermediateModel& operator=(IntermediateModel&&) noexcept = default;

  ~IntermediateModel() = default;

  // ==========================================================================

  bool is_normalized() const { return normalized_; }

  // 基本查询接口
  // ==========================================================================

  absl::string_view name() const { return model_name_; }

  // 获取全部数据（只读视图，零拷贝）
  absl::Span<const Variable> variables() const { return variables_; }
  absl::Span<const Domain> domains() const { return domains_; }
  absl::Span<const Constraint> constraints() const { return constraints_; }
  absl::Span<const Relation> relations() const { return relations_; }

  // 数量统计
  int num_variables() const { return static_cast<int>(variables_.size()); }
  int num_constraints() const {
    return static_cast<int>(constraints_.size());
  }
  int num_domains() const { return static_cast<int>(domains_.size()); }
  int num_relations() const { return static_cast<int>(relations_.size()); }

  // ==========================================================================
  // 按 ID 查询
  // ==========================================================================

  const Variable& GetVariable(VariableId id) const;
  const Domain& GetDomain(DomainId id) const;
  const Constraint& GetConstraint(ConstraintId id) const;
  const Relation& GetRelation(RelationId id) const;

  // ==========================================================================
  // 按名称查询
  // ==========================================================================

  std::optional<VariableId> GetVariableByName(absl::string_view name) const;
  std::optional<DomainId> GetDomainByName(absl::string_view name) const;

  // ==========================================================================
  // 统计信息
  // ==========================================================================

  ModelStatistics GetStatistics() const;

  // ==========================================================================
  // 拓扑查询（变量-约束关系）
  // ==========================================================================

  // 获取变量参与的所有约束
  absl::Span<const ConstraintId> GetConstraintsForVariable(
      VariableId var) const;

  // 获取两变量之间的共同约束
  std::vector<ConstraintId> GetConstraintsBetween(VariableId var1,
                                                   VariableId var2) const;

  // 检查两约束是否相邻（是否共享变量）
  bool AreConstraintsNeighbors(ConstraintId c1, ConstraintId c2) const;

  // 获取约束的邻居约束（共享至少一个变量的约束）
  absl::Span<const ConstraintId> GetNeighborConstraints(
      ConstraintId c) const;

  // ==========================================================================
  // 输出接口
  // ==========================================================================

  // 格式化输出模型信息
  std::string ToString() const;

  // 打印模型信息到日志
  void Print() const;

  // 打印详细的模型结构（用于调试）
  void PrintDetailed() const;

 private:
  friend class ModelBuilder;
  friend class ModelNormalizer;

  bool normalized_ = false;

  // 私有构造函数，只能由 ModelBuilder 调用
  explicit IntermediateModel(std::string name, std::vector<Domain> domains,
                             std::vector<Variable> variables,
                             std::vector<Constraint> constraints,
                             std::vector<Relation> relations);

  // 构建索引结构（在构造时调用）
  void BuildIndices();

  // ==========================================================================
  // 数据成员
  // ==========================================================================

  std::string model_name_;

  // 主要数据
  std::vector<Domain> domains_;
  std::vector<Variable> variables_;
  std::vector<Constraint> constraints_;
  std::vector<Relation> relations_;

  // 名称索引 (使用 absl::flat_hash_map，比 std::unordered_map 快 30-50%)
  absl::flat_hash_map<std::string, VariableId> var_name_index_;
  absl::flat_hash_map<std::string, DomainId> domain_name_index_;

  // 变量-约束订阅关系（邻接表）
  // var_to_constraints_[i] = 变量 i 参与的约束列表
  // 使用 InlinedVector 优化小规模邻居（大多数变量涉及 < 8 个约束）
  std::vector<absl::InlinedVector<ConstraintId, 8>> var_to_constraints_;

  // 约束邻接关系（稀疏表示）
  // constraint_neighbors_[c] = 与约束 c 相邻的约束集合
  std::vector<absl::InlinedVector<ConstraintId, 16>> constraint_neighbors_;
};

}  // namespace cpim::model
