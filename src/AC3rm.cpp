#pragma once

#include "Solver.h"
namespace cpim {
// residues::residues(Network* nt):nt_(nt)
//{
//	data_.resize(nt->tabs.size())
// }
AC3rm::AC3rm(Network* n) : AC3(n) {
  Exclude(tmp_tuple_);
  res_.resize(m_->tabs.size() * m_->max_domain_size() * m_->max_arity(),
              tmp_tuple_);
}
bool AC3rm::seek_support(IntConVal& c_val, const int p) {
  auto c = c_val.c();
  auto index = m_->GetIntConValIndex(c_val);
  tmp_tuple_ = res_[index];
  if (c->IsValidTuple(tmp_tuple_, p)) return true;

  m_->GetFirstValidTuple(c_val, tmp_tuple_, p);
  while (Existed(tmp_tuple_)) {
    if (c->sat(tmp_tuple_)) {
      for (auto v : c->scope) {
        index = m_->GetIntConValIndex(IntConVal(c, v, tmp_tuple_[c->index(v)]));
        res_[index] = tmp_tuple_;
      }
      return true;
    }
    m_->GetNextValidTuple(c_val, tmp_tuple_, p);
  }

  return false;
}
}  // namespace cpim
