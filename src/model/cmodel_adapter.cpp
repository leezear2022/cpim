#include "model/cmodel_adapter.h"

#include <algorithm>
#include <stdexcept>

#include "model/intermediate_model.h"
#include "xcsp3model/HModel.h"

namespace cpim::model {
namespace {
constexpr int kBitsPerWord = 32;
inline int IntSize(int nbits) {
  return (nbits + kBitsPerWord - 1) / kBitsPerWord;
}

inline uint32_t TailMask(int bits) {
  if (bits <= 0) return 0u;
  if (bits >= kBitsPerWord) return 0xFFFFFFFFu;
  return (1u << bits) - 1u;
}
}  // namespace

CModelAdapter CModelAdapter::FromHModel(const HModel& model) {
  CModelAdapter adapter;

  const int num_vars = static_cast<int>(model->Vars().size());
  const int num_tabs = static_cast<int>(model->Tabs().size());
  const int max_dom_size = model->max_domain_size();

  adapter.domains_.num_vars = num_vars;
  adapter.domains_.max_dom_size = max_dom_size;
  adapter.domains_.bit_dom_int_size = IntSize(max_dom_size);
  const int bit_dom_int_size = adapter.domains_.bit_dom_int_size;
  adapter.domains_.data.assign(num_vars * bit_dom_int_size, 0u);

  adapter.domain_sizes_.resize(num_vars, 0);
  adapter.degrees_.resize(num_vars, 0);

  for (int i = 0; i < num_vars; ++i) {
    const HVar var = model->Vars(i);
    const int dom_size = static_cast<int>(var->vals.size());
    adapter.domain_sizes_[i] = dom_size;
    adapter.degrees_[i] = static_cast<int>(model->subscriptions[var].size());

    for (int word = 0; word < bit_dom_int_size; ++word) {
      const int base = word * kBitsPerWord;
      const int remaining = dom_size - base;
      adapter.domains_.data[i * bit_dom_int_size + word] = TailMask(remaining);
    }
  }

  adapter.constraints_.reserve(num_tabs);
  for (int i = 0; i < num_tabs; ++i) {
    const HTab tab = model->Tabs(i);
    adapter.constraints_.push_back(
        make_uint3(tab->scope[0]->id, tab->scope[1]->id, tab->id));
  }

  adapter.subscriptions_.offsets.assign(num_vars + 1, 0);
  adapter.subscriptions_.entries.clear();
  int offset = 0;
  for (int i = 0; i < num_vars; ++i) {
    adapter.subscriptions_.offsets[i] = offset;
    const HVar var = model->Vars(i);
    for (const auto& tab : model->subscriptions[var]) {
      adapter.subscriptions_.entries.push_back(
          make_uint3(tab->scope[0]->id, tab->scope[1]->id, tab->id));
      ++offset;
    }
  }
  adapter.subscriptions_.offsets[num_vars] = offset;

  adapter.supports_.num_constraints = num_tabs;
  adapter.supports_.bit_sup_int_size = max_dom_size * bit_dom_int_size;
  adapter.supports_.data.assign(adapter.supports_.num_constraints *
                                    adapter.supports_.bit_sup_int_size,
                                make_uint2(0u, 0u));

  const int bit_sup_int_size = adapter.supports_.bit_sup_int_size;
  const int bit_index_mask = kBitsPerWord - 1;

  for (int cid = 0; cid < num_tabs; ++cid) {
    const HTab tab = model->Tabs(cid);
    if (tab->Arity() != 2) {
      throw std::invalid_argument(
          "Only binary constraints are supported by the GPU backend.");
    }
    if (!tab->semantics) {
      throw std::invalid_argument("Only support semantics constraints are supported.");
    }

    for (const auto& tuple : tab->tuples) {
      const int x_val = tuple[0];
      const int y_val = tuple[1];

      const int y_word = y_val >> 5;
      const int x_word = x_val >> 5;
      const int idx_x = cid * bit_sup_int_size + y_word * max_dom_size + x_val;
      const int idx_y = cid * bit_sup_int_size + x_word * max_dom_size + y_val;

      const uint32_t mask_y = 1u << (y_val & bit_index_mask);
      const uint32_t mask_x = 1u << (x_val & bit_index_mask);

      adapter.supports_.data[idx_x].x |= mask_y;
      adapter.supports_.data[idx_y].y |= mask_x;
    }
  }

  return adapter;
}

CModelAdapter CModelAdapter::FromIntermediate(const IntermediateModel& model) {
  if (!model.is_normalized()) {
    throw std::invalid_argument(
        "CModelAdapter expects a normalized IntermediateModel. Please run ModelNormalizer first.");
  }

  CModelAdapter adapter;

  const int num_vars = model.num_variables();
  const int num_constraints = model.num_constraints();

  adapter.domains_.num_vars = num_vars;
  adapter.domain_sizes_.resize(num_vars, 0);
  adapter.degrees_.resize(num_vars, 0);

  int max_dom_size = 0;
  for (const Variable& var : model.variables()) {
    const Domain& dom = model.GetDomain(var.domain);
    const int size = dom.Size();
    adapter.domain_sizes_[var.id.value] = size;
    max_dom_size = std::max(max_dom_size, size);
  }

  adapter.domains_.max_dom_size = max_dom_size;
  adapter.domains_.bit_dom_int_size = IntSize(max_dom_size);
  const int bit_dom_int_size = adapter.domains_.bit_dom_int_size;
  adapter.domains_.data.assign(num_vars * bit_dom_int_size, 0u);

  for (const Variable& var : model.variables()) {
    const int vid = var.id.value;
    const int dom_size = adapter.domain_sizes_[vid];
    for (int word = 0; word < bit_dom_int_size; ++word) {
      const int base = word * kBitsPerWord;
      const int remaining = dom_size - base;
      adapter.domains_.data[vid * bit_dom_int_size + word] = TailMask(remaining);
    }
  }

  for (const Variable& var : model.variables()) {
    auto constraints_span = model.GetConstraintsForVariable(var.id);
    adapter.degrees_[var.id.value] = static_cast<int>(constraints_span.size());
  }

  adapter.constraints_.clear();
  adapter.constraints_.reserve(num_constraints);
  adapter.supports_.num_constraints = num_constraints;
  adapter.supports_.bit_sup_int_size =
      adapter.domains_.bit_dom_int_size * adapter.domains_.max_dom_size;
  const int bit_sup_int_size = adapter.supports_.bit_sup_int_size;
  adapter.supports_.data.assign(num_constraints * bit_sup_int_size,
                                make_uint2(0u, 0u));

  std::vector<bool> supported(num_constraints, false);

  for (int idx = 0; idx < num_constraints; ++idx) {
    const Constraint& constraint = model.constraints()[idx];
    const auto* ext = std::get_if<ExtensionConstraint>(&constraint.data);
    if (ext == nullptr || ext->Arity() != 2) {
      throw std::invalid_argument(
          "CModelAdapter: only binary extension constraints are supported");
    }
    if (ext->semantics != ExtensionConstraint::Semantics::kSupports) {
      throw std::invalid_argument(
          "CModelAdapter: only support semantics constraints are supported");
    }

    const int cid = constraint.id.value;
    if (cid < 0 || cid >= num_constraints) {
      throw std::invalid_argument("Constraint id out of range in IntermediateModel");
    }
    supported[cid] = true;

    const int x = ext->scope[0].value;
    const int y = ext->scope[1].value;
    adapter.constraints_.push_back(make_uint3(x, y, cid));

    for (const auto& tuple : ext->tuples) {
      if (tuple.size() != 2) continue;
      const int x_val = tuple[0];
      const int y_val = tuple[1];
      const int y_word = y_val >> 5;
      const int x_word = x_val >> 5;
      const int idx_x = cid * bit_sup_int_size + y_word * max_dom_size + x_val;
      const int idx_y = cid * bit_sup_int_size + x_word * max_dom_size + y_val;
      const uint32_t mask_y = 1u << (y_val & (kBitsPerWord - 1));
      const uint32_t mask_x = 1u << (x_val & (kBitsPerWord - 1));
      adapter.supports_.data[idx_x].x |= mask_y;
      adapter.supports_.data[idx_y].y |= mask_x;
    }
  }

  adapter.subscriptions_.offsets.assign(num_vars + 1, 0);
  adapter.subscriptions_.entries.clear();
  adapter.subscriptions_.entries.reserve(num_constraints * 2);
  for (int i = 0; i < num_vars; ++i) {
    adapter.subscriptions_.offsets[i] =
        static_cast<int>(adapter.subscriptions_.entries.size());
    for (ConstraintId cid : model.GetConstraintsForVariable(VariableId{i})) {
      if (!supported[cid.value]) {
        continue;
      }
      const Constraint& constraint = model.GetConstraint(cid);
      const auto& ext = std::get<ExtensionConstraint>(constraint.data);
      adapter.subscriptions_.entries.push_back(
          make_uint3(ext.scope[0].value, ext.scope[1].value, cid.value));
    }
  }
  adapter.subscriptions_.offsets[num_vars] =
      static_cast<int>(adapter.subscriptions_.entries.size());

  return adapter;
}

}  // namespace cpim::model
