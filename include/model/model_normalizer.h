#pragma once

#include <cstdint>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "model/intermediate_model.h"

namespace cpim::model {

struct DomainRemap {
  DomainId original_id;
  DomainId normalized_id;
  absl::flat_hash_map<int, int> value_to_canonical;
  std::vector<int> canonical_to_original;
};

struct NormalizationOptions {
  bool canonicalize_domains = true;
  bool force_support_semantics = true;
  std::int64_t max_expanded_tuples = 1'000'000;
};

class ModelNormalizer {
 public:
  explicit ModelNormalizer(NormalizationOptions options = {});

  absl::StatusOr<IntermediateModel> Normalize(
      const IntermediateModel& input_model);

  absl::Span<const DomainRemap> domain_remaps() const {
    return domain_remaps_;
  }

 private:
  absl::StatusOr<DomainRemap> BuildDomainRemap(const Domain& domain) const;
  absl::StatusOr<int> MapValue(const DomainRemap& remap, int original) const;
  absl::StatusOr<std::vector<std::vector<int>>> NormalizeTuples(
      const ExtensionConstraint& ext, absl::Span<const Variable> variables,
      absl::Span<const DomainRemap> remaps) const;

  NormalizationOptions options_;
  std::vector<DomainRemap> domain_remaps_;
};

}  // namespace cpim::model
