#include "model/device_layout.h"

#include <algorithm>
#include <cstddef>
#include <variant>

#include "absl/status/status.h"
#include "absl/strings/str_format.h"
#include "model/intermediate_model.h"
#include "model/types.h"

namespace cpim::model {
namespace {

constexpr int kBitsPerWord = 32;

int IntSize(int nbits) { return (nbits + kBitsPerWord - 1) / kBitsPerWord; }

uint32_t TailMask(int bits) {
  if (bits <= 0) return 0u;
  if (bits >= kBitsPerWord) return 0xFFFFFFFFu;
  return (1u << bits) - 1u;
}

bool IsInRange(int value, int upper) { return value >= 0 && value < upper; }

}  // namespace

absl::StatusOr<DeviceModelLayout> BuildDeviceLayoutFromIntermediate(
    const IntermediateModel& model) {
  if (!model.is_normalized()) {
    return absl::InvalidArgumentError(
        "DeviceModelLayout expects a normalized IntermediateModel");
  }

  DeviceModelLayout layout;
  layout.num_vars = model.num_variables();
  layout.num_constraints = model.num_constraints();
  if (layout.num_vars <= 0) {
    return absl::InvalidArgumentError("DeviceModelLayout requires variables");
  }

  layout.domain_sizes.assign(layout.num_vars, 0);
  for (const Variable& var : model.variables()) {
    if (!IsInRange(var.id.value, layout.num_vars)) {
      return absl::InvalidArgumentError(
          absl::StrFormat("Variable id out of range: %d", var.id.value));
    }
    const int dom_size = model.GetDomain(var.domain).Size();
    layout.domain_sizes[var.id.value] = dom_size;
    layout.max_dom_size = std::max(layout.max_dom_size, dom_size);
  }

  layout.bit_words = IntSize(layout.max_dom_size);
  if (layout.bit_words <= 0) {
    return absl::InvalidArgumentError("DeviceModelLayout requires non-empty domains");
  }

  layout.bit_dom.assign(
      static_cast<size_t>(layout.num_vars) * layout.bit_words, 0u);
  for (const Variable& var : model.variables()) {
    const int vid = var.id.value;
    const int dom_size = layout.domain_sizes[vid];
    const int base = vid * layout.bit_words;
    for (int word = 0; word < layout.bit_words; ++word) {
      const int remaining = dom_size - word * kBitsPerWord;
      layout.bit_dom[base + word] = TailMask(remaining);
    }
  }

  layout.constraint_scopes.assign(layout.num_constraints, DeviceInt2{});
  const int entries_per_constraint = layout.bitsup_entries_per_constraint();
  layout.bit_sup.assign(
      static_cast<size_t>(layout.num_constraints) * entries_per_constraint,
      DeviceUInt2{});
  layout.bit_sup_words.assign(
      static_cast<size_t>(layout.num_constraints) * entries_per_constraint, 0u);

  std::vector<bool> supported(layout.num_constraints, false);
  for (int index = 0; index < layout.num_constraints; ++index) {
    const Constraint& constraint = model.constraints()[index];
    const int cid = constraint.id.value;
    if (!IsInRange(cid, layout.num_constraints)) {
      return absl::InvalidArgumentError(
          absl::StrFormat("Constraint id out of range: %d", cid));
    }

    const auto* ext = std::get_if<ExtensionConstraint>(&constraint.data);
    if (ext == nullptr || ext->Arity() != 2) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "Metal v1 supports only binary extension constraints; cid=%d", cid));
    }
    if (ext->semantics != ExtensionConstraint::Semantics::kSupports) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "Metal v1 supports only supports semantics; cid=%d", cid));
    }

    const int x = ext->scope[0].value;
    const int y = ext->scope[1].value;
    if (!IsInRange(x, layout.num_vars) || !IsInRange(y, layout.num_vars)) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "Constraint %d has variable out of range: (%d, %d)", cid, x, y));
    }

    layout.constraint_scopes[cid] = DeviceInt2{x, y};
    supported[cid] = true;

    for (const auto& tuple : ext->tuples) {
      if (tuple.size() != 2) {
        return absl::InvalidArgumentError(
            absl::StrFormat("Constraint %d has non-binary tuple", cid));
      }
      const int xv = tuple[0];
      const int yv = tuple[1];
      if (!IsInRange(xv, layout.domain_sizes[x]) ||
          !IsInRange(yv, layout.domain_sizes[y])) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "Constraint %d has tuple value out of normalized domain: (%d, %d)",
            cid, xv, yv));
      }

      const int y_word = yv / kBitsPerWord;
      const int y_bit = yv % kBitsPerWord;
      const int x_word = xv / kBitsPerWord;
      const int x_bit = xv % kBitsPerWord;

      const int idx_x =
          ((cid * 2 + 0) * layout.max_dom_size + xv) * layout.bit_words +
          y_word;
      const int idx_y =
          ((cid * 2 + 1) * layout.max_dom_size + yv) * layout.bit_words +
          x_word;
      layout.bit_sup[idx_x].x |= (1u << y_bit);
      layout.bit_sup[idx_y].y |= (1u << x_bit);
      layout.bit_sup_words[idx_x] |= (1u << y_bit);
      layout.bit_sup_words[idx_y] |= (1u << x_bit);
    }
  }

  layout.subscriptions.offsets.assign(layout.num_vars + 1, 0);
  layout.subscriptions.entries.clear();
  layout.subscriptions.entries.reserve(static_cast<size_t>(layout.num_constraints) * 2);
  for (int vid = 0; vid < layout.num_vars; ++vid) {
    layout.subscriptions.offsets[vid] =
        static_cast<int32_t>(layout.subscriptions.entries.size());
    for (ConstraintId cid : model.GetConstraintsForVariable(VariableId{vid})) {
      if (!IsInRange(cid.value, layout.num_constraints) || !supported[cid.value]) {
        continue;
      }
      const DeviceInt2 scope = layout.constraint_scopes[cid.value];
      layout.subscriptions.entries.push_back(DeviceUInt3{
          static_cast<uint32_t>(scope.x),
          static_cast<uint32_t>(scope.y),
          static_cast<uint32_t>(cid.value)});
    }
  }
  layout.subscriptions.offsets[layout.num_vars] =
      static_cast<int32_t>(layout.subscriptions.entries.size());

  return layout;
}

}  // namespace cpim::model
