#pragma once

#include <compare>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/container/inlined_vector.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"

namespace cpim::model {

// ============================================================================
// 强类型ID定义 (类型安全，避免int混淆)
// ============================================================================

struct DomainId {
  int value = -1;

  bool operator==(const DomainId& other) const { return value == other.value; }
  bool operator!=(const DomainId& other) const { return value != other.value; }
  bool operator<(const DomainId& other) const { return value < other.value; }

  bool IsValid() const { return value >= 0; }
};

struct VariableId {
  int value = -1;

  bool operator==(const VariableId& other) const { return value == other.value; }
  bool operator!=(const VariableId& other) const { return value != other.value; }
  bool operator<(const VariableId& other) const { return value < other.value; }

  bool IsValid() const { return value >= 0; }
};

struct ConstraintId {
  int value = -1;

  bool operator==(const ConstraintId& other) const { return value == other.value; }
  bool operator!=(const ConstraintId& other) const { return value != other.value; }
  bool operator<(const ConstraintId& other) const { return value < other.value; }

  bool IsValid() const { return value >= 0; }
};

struct RelationId {
  int value = -1;

  bool operator==(const RelationId& other) const { return value == other.value; }
  bool operator!=(const RelationId& other) const { return value != other.value; }
  bool operator<(const RelationId& other) const { return value < other.value; }

  bool IsValid() const { return value >= 0; }
};

// ============================================================================
// Abseil Hash 支持 (用于 flat_hash_map)
// ============================================================================

template <typename H>
H AbslHashValue(H h, const DomainId& id) {
  return H::combine(std::move(h), id.value);
}

template <typename H>
H AbslHashValue(H h, const VariableId& id) {
  return H::combine(std::move(h), id.value);
}

template <typename H>
H AbslHashValue(H h, const ConstraintId& id) {
  return H::combine(std::move(h), id.value);
}

template <typename H>
H AbslHashValue(H h, const RelationId& id) {
  return H::combine(std::move(h), id.value);
}

// ============================================================================
// Domain 定义
// ============================================================================

// 范围域: [min, max]
struct RangeDomain {
  int min_value;
  int max_value;

  int Size() const { return max_value - min_value + 1; }

  bool Contains(int value) const {
    return value >= min_value && value <= max_value;
  }
};

// 枚举域: {1, 3, 5, 7, ...}
struct EnumeratedDomain {
  std::vector<int> values;  // 已排序

  int Size() const { return static_cast<int>(values.size()); }

  bool Contains(int value) const {
    return std::binary_search(values.begin(), values.end(), value);
  }
};

using DomainValues = std::variant<RangeDomain, EnumeratedDomain>;

struct Domain {
  DomainId id;
  std::string name;
  DomainValues values;

  int Size() const {
    return std::visit([](const auto& d) { return d.Size(); }, values);
  }

  bool Contains(int value) const {
    return std::visit([value](const auto& d) { return d.Contains(value); },
                      values);
  }

  // 获取所有值（统一接口）
  std::vector<int> GetAllValues() const;
};

// ============================================================================
// Variable 定义
// ============================================================================

struct Variable {
  VariableId id;
  std::string name;
  DomainId domain;
  std::optional<int> initial_value;
};

// ============================================================================
// Constraint 定义
// ============================================================================

// Extension约束（表格约束）
struct ExtensionConstraint {
  enum class Semantics {
    kSupports,   // 支持语义（元组在表中表示允许的赋值）
    kConflicts   // 冲突语义（元组在表中表示禁止的赋值）
  };

  Semantics semantics;
  // 使用 InlinedVector 优化小规模 scope（大多数约束 arity ≤ 4）
  absl::InlinedVector<VariableId, 4> scope;
  std::vector<std::vector<int>> tuples;  // 标准化的元组（使用标准值索引）

  int Arity() const { return static_cast<int>(scope.size()); }
  int NumTuples() const { return static_cast<int>(tuples.size()); }
};

// Intension约束（表达式约束）
struct IntensionConstraint {
  absl::InlinedVector<VariableId, 4> scope;
  std::string expression;

  int Arity() const { return static_cast<int>(scope.size()); }
};

// AllDifferent约束
struct AllDifferentConstraint {
  std::vector<VariableId> scope;

  int Arity() const { return static_cast<int>(scope.size()); }
};

// 约束数据的variant
using ConstraintData =
    std::variant<ExtensionConstraint, IntensionConstraint,
                 AllDifferentConstraint>;

struct Constraint {
  ConstraintId id;
  std::string name;
  ConstraintData data;

  int Arity() const {
    return std::visit([](const auto& c) { return c.Arity(); }, data);
  }

  // 获取scope（需要根据不同约束类型提取）
  absl::Span<const VariableId> GetScope() const;
};

// ============================================================================
// Relation 定义（XCSP3特有，relation表可以被多个constraint引用）
// ============================================================================

struct Relation {
  RelationId id;
  int arity;
  ExtensionConstraint::Semantics semantics;
  std::vector<std::vector<int>> tuples;

  int NumTuples() const { return static_cast<int>(tuples.size()); }
};

// ============================================================================
// 模型统计信息
// ============================================================================

struct ModelStatistics {
  int num_variables = 0;
  int num_constraints = 0;
  int num_domains = 0;
  int num_relations = 0;

  int max_domain_size = 0;
  int min_domain_size = 0;
  int max_arity = 0;
  int min_arity = 0;

  // 约束类型分布
  absl::flat_hash_map<std::string, int> constraint_type_counts;

  // 格式化输出
  std::string ToString() const;
};

// ============================================================================
// 辅助函数
// ============================================================================

// 获取约束类型名称
std::string GetConstraintTypeName(const ConstraintData& constraint);

}  // namespace cpim::model
