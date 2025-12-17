#include "Solver.h"
namespace cpim {
AC3bit::AC3bit(Network* m) : AC3(m), max_bitDom_size_(m->max_bitDom_size()) {
  if (m_->max_arity() != 2) {
    std::cout << "error!" << std::endl;
  }
  // const vector<bitset<BITSIZE>> a(max_bitDom_size_, 0);
  bitSup_.resize(m_->tabs.size() * m_->max_domain_size() * m_->max_arity(),
                 vector<bitset<BITSIZE>>(max_bitDom_size_, 0));
  // bitSup_.resize(m_->tabs.size()*m_->max_domain_size()*m_->max_arity(), 0);
  for (Tabular* c : m_->tabs) {
    for (auto t : c->tuples()) {
      const int index[] = {
          m_->GetIntConValIndex(IntConVal(c, c->scope[0], t[0])),
          m_->GetIntConValIndex(IntConVal(c, c->scope[1], t[1]))};
      // const int idx0 = m_->GetIntConValIndex(IntConVal(c, c->scope[0],
      // t[0])); const int idx1 = m_->GetIntConValIndex(IntConVal(c,
      // c->scope[1], t[1]));
      auto idx0 = GetBitIdx(t[0]);
      auto idx1 = GetBitIdx(t[1]);
      // bitSup_[index[0]][t[0] / BITSIZE].set(t[1] % BITSIZE);
      // bitSup_[index[1]][t[1] / BITSIZE].set(t[0] % BITSIZE);
      bitSup_[index[0]][get<0>(idx1)].set(get<1>(idx1));
      bitSup_[index[1]][get<0>(idx0)].set(get<1>(idx0));
    }
  }
}

bool AC3bit::seek_support(const IntConVal& c_val, const int p) {
  const int idx = m_->GetIntConValIndex(c_val);

  for (IntVar* y : c_val.c()->scope)
    if (y->id() != c_val.v()->id())
      for (int i = 0; i < y->bitDom(p).size(); ++i)
        if ((bitSup_[idx][i] & y->bitDom(p)[i]).any()) return true;

  return false;
}
}  // namespace cpim
