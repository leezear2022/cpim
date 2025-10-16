#include "model/types.h"

#include <algorithm>

#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"

namespace cpim::model {

// ============================================================================
// Domain 实现
// ============================================================================

std::vector<int> Domain::GetAllValues() const {
  return std::visit(
      [](const auto& d) -> std::vector<int> {
        using T = std::decay_t<decltype(d)>;
        if constexpr (std::is_same_v<T, RangeDomain>) {
          std::vector<int> result;
          result.reserve(d.Size());
          for (int v = d.min_value; v <= d.max_value; ++v) {
            result.push_back(v);
          }
          return result;
        } else {  // EnumeratedDomain
          return d.values;
        }
      },
      values);
}

// ============================================================================
// Constraint 实现
// ============================================================================

absl::Span<const VariableId> Constraint::GetScope() const {
  return std::visit(
      [](const auto& c) -> absl::Span<const VariableId> {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, ExtensionConstraint>) {
          return absl::MakeConstSpan(c.scope);
        } else if constexpr (std::is_same_v<T, IntensionConstraint>) {
          return absl::MakeConstSpan(c.scope);
        } else if constexpr (std::is_same_v<T, AllDifferentConstraint>) {
          return absl::MakeConstSpan(c.scope);
        }
        // 不应该到达这里
        static const std::vector<VariableId> empty;
        return absl::MakeConstSpan(empty);
      },
      data);
}

// ============================================================================
// ModelStatistics 实现
// ============================================================================

std::string ModelStatistics::ToString() const {
  std::string result;
  result += "ModelStatistics {\n";
  result += absl::StrFormat("  variables: %d\n", num_variables);
  result += absl::StrFormat("  constraints: %d\n", num_constraints);
  result += absl::StrFormat("  domains: %d\n", num_domains);
  result += absl::StrFormat("  relations: %d\n", num_relations);
  result += absl::StrFormat("  domain_size: [%d, %d]\n", min_domain_size,
                            max_domain_size);
  result += absl::StrFormat("  arity: [%d, %d]\n", min_arity, max_arity);

  if (!constraint_type_counts.empty()) {
    result += "  constraint_types: {\n";
    for (const auto& [type, count] : constraint_type_counts) {
      result += absl::StrFormat("    %s: %d\n", type, count);
    }
    result += "  }\n";
  }

  result += "}";
  return result;
}

// ============================================================================
// 辅助函数
// ============================================================================

std::string GetConstraintTypeName(const ConstraintData& constraint) {
  return std::visit(
      [](const auto& c) -> std::string {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, ExtensionConstraint>) {
          return "Extension";
        } else if constexpr (std::is_same_v<T, IntensionConstraint>) {
          return "Intension";
        } else if constexpr (std::is_same_v<T, AllDifferentConstraint>) {
          return "AllDifferent";
        }
        return "Unknown";
      },
      constraint);
}

}  // namespace cpim::model
