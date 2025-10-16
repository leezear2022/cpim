#include "model/model_normalizer.h"

#include <utility>

#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/types/span.h"
#include "model/model_builder.h"

namespace cpim::model {

namespace {
constexpr std::int64_t kDefaultMaxTuples = 1'000'000;

std::string EncodeTuple(absl::Span<const int> tuple) {
  return absl::StrJoin(tuple, ",");
}

std::vector<int> CanonicalDomainValues(const Domain& domain) {
  return domain.GetAllValues();
}

}  // namespace

ModelNormalizer::ModelNormalizer(NormalizationOptions options)
    : options_(options) {
  if (options_.max_expanded_tuples <= 0) {
    options_.max_expanded_tuples = kDefaultMaxTuples;
  }
}

absl::StatusOr<DomainRemap> ModelNormalizer::BuildDomainRemap(
    const Domain& domain) const {
  DomainRemap remap;
  remap.original_id = domain.id;
  remap.canonical_to_original = CanonicalDomainValues(domain);
  remap.value_to_canonical.reserve(remap.canonical_to_original.size());
  for (int idx = 0; idx < static_cast<int>(remap.canonical_to_original.size()); ++idx) {
    remap.value_to_canonical.emplace(remap.canonical_to_original[idx], idx);
  }
  return remap;
}

absl::StatusOr<int> ModelNormalizer::MapValue(const DomainRemap& remap,
                                              int original) const {
  auto it = remap.value_to_canonical.find(original);
  if (it == remap.value_to_canonical.end()) {
    return absl::InvalidArgumentError(
        absl::StrCat("Value ", original,
                     " not present in original domain during normalization"));
  }
  return it->second;
}

absl::StatusOr<std::vector<std::vector<int>>> ModelNormalizer::NormalizeTuples(
    const ExtensionConstraint& ext, absl::Span<const Variable> variables,
    absl::Span<const DomainRemap> remaps) const {
  std::vector<const DomainRemap*> scope_remaps;
  scope_remaps.reserve(ext.scope.size());
  for (VariableId vid : ext.scope) {
    if (vid.value < 0 || vid.value >= static_cast<int>(variables.size())) {
      return absl::InvalidArgumentError(
          "Variable id out of range while normalizing tuples");
    }
    const Variable& var = variables[vid.value];
    if (var.domain.value < 0 ||
        var.domain.value >= static_cast<int>(remaps.size())) {
      return absl::InvalidArgumentError(
          "Domain id out of range while normalizing tuples");
    }
    scope_remaps.push_back(&remaps[var.domain.value]);
  }

  std::vector<std::vector<int>> normalized_tuples;
  normalized_tuples.reserve(ext.tuples.size());

  if (ext.semantics == ExtensionConstraint::Semantics::kSupports) {
    for (const auto& tuple : ext.tuples) {
      if (tuple.size() != scope_remaps.size()) {
        return absl::InvalidArgumentError(
            "Tuple arity mismatch while normalizing supports");
      }
      std::vector<int> mapped(tuple.size());
      for (size_t i = 0; i < tuple.size(); ++i) {
        auto mapped_or = MapValue(*scope_remaps[i], tuple[i]);
        if (!mapped_or.ok()) {
          return mapped_or.status();
        }
        mapped[i] = *mapped_or;
      }
      normalized_tuples.push_back(std::move(mapped));
    }
    return normalized_tuples;
  }

  // conflicts -> compute complement set in canonical space
  std::vector<std::vector<int>> forbidden;
  forbidden.reserve(ext.tuples.size());
  for (const auto& tuple : ext.tuples) {
    if (tuple.size() != scope_remaps.size()) {
      return absl::InvalidArgumentError(
          "Tuple arity mismatch while normalizing conflicts");
    }
    std::vector<int> mapped(tuple.size());
    for (size_t i = 0; i < tuple.size(); ++i) {
      auto mapped_or = MapValue(*scope_remaps[i], tuple[i]);
      if (!mapped_or.ok()) {
        return mapped_or.status();
      }
      mapped[i] = *mapped_or;
    }
    forbidden.push_back(std::move(mapped));
  }

  absl::flat_hash_set<std::string> forbidden_set;
  forbidden_set.reserve(forbidden.size());
  for (const auto& tuple : forbidden) {
    forbidden_set.insert(EncodeTuple(tuple));
  }

  std::vector<int> domain_sizes;
  domain_sizes.reserve(scope_remaps.size());
  for (const DomainRemap* remap : scope_remaps) {
    const int size = static_cast<int>(remap->canonical_to_original.size());
    if (size <= 0) {
      return absl::InvalidArgumentError(
          "Domain has no values while normalizing conflicts");
    }
    domain_sizes.push_back(size);
  }

  std::int64_t total = 1;
  for (int size : domain_sizes) {
    total *= size;
    if (total > options_.max_expanded_tuples) {
      return absl::ResourceExhaustedError(
          absl::StrCat("Expanded tuple space exceeds limit (", total,
                       ") while normalizing conflicts."));
    }
  }

  normalized_tuples.clear();
  normalized_tuples.reserve(static_cast<size_t>(total) - forbidden_set.size());
  if (scope_remaps.empty()) {
    return normalized_tuples;
  }

  std::vector<int> current(scope_remaps.size(), 0);
  while (true) {
    const std::string key = EncodeTuple(current);
    if (!forbidden_set.contains(key)) {
      normalized_tuples.push_back(current);
    }
    int idx = static_cast<int>(current.size()) - 1;
    while (idx >= 0) {
      ++current[idx];
      if (current[idx] < domain_sizes[idx]) {
        break;
      }
      current[idx] = 0;
      --idx;
    }
    if (idx < 0) {
      break;
    }
  }

  return normalized_tuples;
}

absl::StatusOr<IntermediateModel> ModelNormalizer::Normalize(
    const IntermediateModel& input_model) {
  domain_remaps_.clear();
  domain_remaps_.reserve(input_model.num_domains());

  ModelBuilder builder(std::string(input_model.name()));

  std::vector<DomainId> new_domain_ids(input_model.num_domains(), DomainId{});
  if (options_.canonicalize_domains) {
    for (const Domain& domain : input_model.domains()) {
      auto remap_or = BuildDomainRemap(domain);
      if (!remap_or.ok()) {
        return remap_or.status();
      }
      DomainRemap remap = std::move(*remap_or);
      int canonical_size = remap.canonical_to_original.size();
      if (canonical_size <= 0) {
        return absl::InvalidArgumentError(
            absl::StrCat("Domain ", domain.name, " is empty"));
      }
      auto domain_builder = builder.AddDomain(domain.name);
      DomainId new_id =
          domain_builder.WithRange(0, canonical_size - 1).Build();
      remap.normalized_id = new_id;
      new_domain_ids[domain.id.value] = new_id;
      domain_remaps_.push_back(std::move(remap));
    }
  } else {
    for (const Domain& domain : input_model.domains()) {
      auto domain_builder = builder.AddDomain(domain.name);
      DomainId new_id;
      if (auto ptr = std::get_if<RangeDomain>(&domain.values)) {
        new_id = domain_builder
                     .WithRange(ptr->min_value, ptr->max_value)
                     .Build();
      } else if (auto ptr = std::get_if<EnumeratedDomain>(&domain.values)) {
        new_id = domain_builder.WithValues(ptr->values).Build();
      } else {
        return absl::InvalidArgumentError("Unsupported domain type");
      }
      DomainRemap remap;
      remap.original_id = domain.id;
      remap.normalized_id = new_id;
      remap.canonical_to_original = domain.GetAllValues();
      remap.value_to_canonical.reserve(remap.canonical_to_original.size());
      for (int idx = 0; idx < static_cast<int>(remap.canonical_to_original.size()); ++idx) {
        remap.value_to_canonical.emplace(remap.canonical_to_original[idx],
                                         remap.canonical_to_original[idx]);
      }
      new_domain_ids[domain.id.value] = new_id;
      domain_remaps_.push_back(std::move(remap));
    }
  }

  std::vector<VariableId> new_variable_ids(input_model.num_variables(),
                                           VariableId{});
  for (const Variable& var : input_model.variables()) {
    auto builder_var = builder.AddVariable(var.name);
    if (var.domain.value < 0 ||
        var.domain.value >= static_cast<int>(new_domain_ids.size())) {
      return absl::InvalidArgumentError(
          "Variable domain out of range during normalization");
    }
    builder_var.WithDomain(new_domain_ids[var.domain.value]);
    if (var.initial_value.has_value()) {
      auto mapped_or =
          MapValue(domain_remaps_[var.domain.value], *var.initial_value);
      if (!mapped_or.ok()) {
        return mapped_or.status();
      }
      builder_var.WithInitialValue(*mapped_or);
    }
    VariableId new_id = builder_var.Build();
    new_variable_ids[var.id.value] = new_id;
  }

  // Relations are copied verbatim for now; tuple normalization could be added
  // when relation -> domain mapping metadata becomes available.
  for (const Relation& relation : input_model.relations()) {
    auto tuples = relation.tuples;
    ExtensionConstraint::Semantics semantics = relation.semantics;
    if (options_.force_support_semantics &&
        semantics == ExtensionConstraint::Semantics::kConflicts) {
      semantics = ExtensionConstraint::Semantics::kSupports;
      // We cannot safely compute the complement without knowing the domain per
      // coordinate, so we leave the tuples empty and rely on constraints that
      // already host normalized tables.
      tuples.clear();
    }
    builder.AddRelation(relation.arity, semantics, std::move(tuples));
  }

  for (const Constraint& constraint : input_model.constraints()) {
    if (const auto* ext = std::get_if<ExtensionConstraint>(&constraint.data)) {
      auto normalized_or =
          NormalizeTuples(*ext, input_model.variables(), domain_remaps_);
      if (!normalized_or.ok()) {
        return normalized_or.status();
      }
      auto normalized_tuples = std::move(*normalized_or);
      ExtensionConstraint::Semantics semantics =
          ExtensionConstraint::Semantics::kSupports;
      builder.AddConstraint(constraint.name)
          .AsExtension(semantics,
                       [&]() {
                         std::vector<VariableId> scope;
                         scope.reserve(ext->scope.size());
                         for (VariableId vid : ext->scope) {
                           scope.push_back(new_variable_ids[vid.value]);
                         }
                         return scope;
                       }(),
                       std::move(normalized_tuples))
          .Build();
    } else if (const auto* all_diff =
                   std::get_if<AllDifferentConstraint>(&constraint.data)) {
      std::vector<VariableId> scope;
      scope.reserve(all_diff->scope.size());
      for (VariableId vid : all_diff->scope) {
        scope.push_back(new_variable_ids[vid.value]);
      }
      builder.AddConstraint(constraint.name).AsAllDifferent(scope).Build();
    } else if (const auto* intension =
                   std::get_if<IntensionConstraint>(&constraint.data)) {
      std::vector<VariableId> scope;
      scope.reserve(intension->scope.size());
      for (VariableId vid : intension->scope) {
        scope.push_back(new_variable_ids[vid.value]);
      }
      builder.AddConstraint(constraint.name)
          .AsIntension(scope, intension->expression)
          .Build();
    }
  }

  return std::move(builder).Build();
}

}  // namespace cpim::model
