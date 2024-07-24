#include "Solver.h"

using namespace std;
namespace cpim {
using namespace std;

lMaxRPC::lMaxRPC(Network* m) : AC(m) {
  // 初始化队列
  q_var_.initial(m_->vars.size());
  // 初始化约束查找矩阵
  nei_.resize(m_->vars.size(), vector<Tabular*>(m_->vars.size(), nullptr));
  for (auto c : m_->tabs) {
    nei_[c->scope[0]->id()][c->scope[1]->id()] = c;
    nei_[c->scope[1]->id()][c->scope[0]->id()] = c;
  }

  // 初始化变量三角关系
  pc_nei_.resize(m_->vars.size(), vector<vector<IntVar*>>(m_->vars.size()));
  set<IntVar*> vars_map;
  vector<bool> vars_in(m_->vars.size(), false);
  for (auto x : m_->vars) {
    for (auto y : m_->vars) {
      for (auto z : m_->vars) {
        if (nei_[x->id()][z->id()] != nullptr && nei_[y->id()][z->id()]) {
          if (!vars_in[z->id()]) {
            vars_in[z->id()] = true;
            pc_nei_[x->id()][y->id()].push_back(z);
          }
        }
      }
      vars_in.assign(vars_in.size(), false);
    }
  }

  last_pc.resize(m_->tabs.size() * m_->max_domain_size() * m_->max_arity(),
                 Limits::INDEX_OVERFLOW);
  last_ac.resize(m_->tabs.size() * m_->max_domain_size() * m_->max_arity(),
                 Limits::INDEX_OVERFLOW);

  bitSup_.resize(m_->tabs.size() * m_->max_domain_size() * m_->max_arity(),
                 vector<bitset<BITSIZE>>(m_->max_domain_size(), 0));
  for (Tabular* c : m_->tabs) {
    for (auto t : c->tuples()) {
      const int index[] = {
          m_->GetIntConValIndex(IntConVal(c, c->scope[0], t[0])),
          m_->GetIntConValIndex(IntConVal(c, c->scope[1], t[1]))};
      auto idx0 = GetBitIdx(t[0]);
      auto idx1 = GetBitIdx(t[1]);
      bitSup_[index[0]][get<0>(idx1)].set(get<1>(idx1));
      bitSup_[index[1]][get<0>(idx0)].set(get<1>(idx0));
    }
  }

  neighborhood.resize(m_->vars.size());
  for (const auto x : m_->vars)
    for (const auto y : m_->vars)
      if ((x != y) && nei_[x->id()][y->id()] != nullptr)
        neighborhood[x->id()].push_back(y);
}

ConsistencyState lMaxRPC::enforce(vector<IntVar*>& x_evt, const int p) {
  level_ = p;
  q_var_.clear();
  for (auto v : x_evt) q_var_.push(v);
  while (!q_var_.empty()) {
    const auto j = q_var_.pop();
    for (auto i : neighborhood[j->id()]) {
      if (i->assigned(p)) {
        // cout << "assigned: " << i->id << endl;
        continue;
      }
      // cout << "neighborhood:" << i->id << endl;
      bool changed = false;
      const auto c = nei_[i->id()][j->id()];
      for (int a = i->head(p); a != Limits::INDEX_OVERFLOW;
           i->next_value(a, p)) {
        // cout << "have_pc_support(" << i->id << ", " << a << ", " << j->id <<
        // ", " << p << ")" << endl;
        if (!have_pc_support(*i, a, *j, p)) {
          i->RemoveValue(a, p);
          //++ps_.num_delete;
          // cout << "delete: (" << i->id << "," << a << ")" << endl;
          if (i->faild(p)) {
            // cout << "faild: " << i->id << endl;
            ++c->weight;
            cs.state = false;
            return cs;
          }
          changed = true;
        }
      }
      if (changed) {
        q_var_.push(i);
        // cout << "push:" << i->id << endl;
      }
    }
  }
  cs.state = true;
  return cs;
}

bool lMaxRPC::have_pc_support(IntVar& i, const int a, IntVar& j, const int p) {
  const auto c = nei_[i.id()][j.id()];
  const auto cia_idx = m_->GetIntConValIndex(c->id(), i.id(), a);
  const auto last_pc_iaj = last_pc[cia_idx];

  if (last_pc_iaj != Limits::INDEX_OVERFLOW && j.have(last_pc_iaj, p))
    return true;

  const int v = j.head(p);
  int b = next_support_bit(i, a, j, v, p);

  while (b != Limits::INDEX_OVERFLOW) {
    bool pc_witness = true;
    for (auto k : pc_nei_[i.id()][j.id()]) {
      if (!k->assigned(p)) {
        if (!have_pc_wit(i, a, j, b, *k, p)) {
          pc_witness = false;
          break;
        }
      }
    }

    if (pc_witness) {
      const auto cjb_idx = m_->GetIntConValIndex(c->id(), j.id(), b);
      last_pc[cia_idx] = b;
      last_pc[cjb_idx] = a;
      last_ac[cia_idx] = b / BITSIZE;
      return true;
    }
    b = next_support_bit(i, a, j, b + 1, p);
  }
  return false;
}

bool lMaxRPC::have_pc_wit(IntVar& i, const int a, const IntVar& j, int b,
                          IntVar& k, const int p) {
  const auto c_ik = nei_[i.id()][k.id()];
  const auto c_jk = nei_[j.id()][k.id()];
  const auto cva_ikia = m_->GetIntConValIndex(c_ik->id(), i.id(), a);
  const auto cva_jkjb = m_->GetIntConValIndex(c_jk->id(), j.id(), b);
  // k.
  if (last_ac[cva_ikia] != Limits::INDEX_OVERFLOW) {
    const auto d = last_ac[cva_ikia];
    if ((bitSup_[cva_ikia][d] & bitSup_[cva_jkjb][d] & k.bitDom(p)[d]).any()) {
      return true;
    }
  }

  if (last_ac[cva_jkjb] != Limits::INDEX_OVERFLOW) {
    const auto d = last_ac[cva_jkjb];
    if ((bitSup_[cva_ikia][d] & bitSup_[cva_jkjb][d] & k.bitDom(p)[d]).any()) {
      return true;
    }
  }
  // const int bitdom_size = k.bitDom(p).size();
  for (int c = 0; c < k.num_bit_; ++c)
    if ((bitSup_[cva_ikia][c] & bitSup_[cva_jkjb][c] & k.bitDom(p)[c]).any()) {
      last_ac[cva_ikia] = c;
      last_ac[cva_jkjb] = c;
      return true;
    }
  return false;
}

int lMaxRPC::next_support_bit(IntVar& i, const int a, IntVar& j, const int v,
                              const int p) {
  // 由于若传入的v已越界
  if (v > j.capacity() - 1 || v == Limits::INDEX_OVERFLOW)
    return Limits::INDEX_OVERFLOW;

  const auto c = nei_[i.id()][j.id()];
  const int idx_cia = m_->GetIntConValIndex(c->id(), i.id(), a);
  const auto index = GetBitIdx(v);
  const uint64_t b =
      ((bitSup_[idx_cia][get<0>(index)] & j.bitDom(p)[get<0>(index)]) >>
       get<1>(index))
          .to_ullong();

  if (b) return v + FirstOne(b);

  for (int u = get<0>(index) + 1; u < j.num_bit_; ++u) {
    const uint64_t mask = (bitSup_[idx_cia][u] & j.bitDom(p)[u]).to_ullong();
    if (mask) return GetValue(u, FirstOne(mask));
  }

  return Limits::INDEX_OVERFLOW;
}

bool lMaxRPC::have_no_PC_support(IntVar* i, const int a, IntVar* j) {
  const auto c = nei_[i->id()][j->id()];
  const auto idx1 = m_->GetIntConValIndex(c->id(), i->id(), a);

  for (int b = j->head(level_); b != Limits::INDEX_OVERFLOW;
       j->next_value(b, level_)) {
    int PCWitness = true;
    auto b_idx = GetBitIdx(b);
    if (bitSup_[idx1][get<0>(b_idx)].test(get<1>(b_idx))) {
      for (auto k : pc_nei_[i->id()][j->id()]) {
        if (!have_PC_wit(i, a, j, b, k)) {
          PCWitness = false;
          break;
        }
      }
      if (PCWitness != false) {
        const auto idx2 = m_->GetIntConValIndex(c->id(), j->id(), b);
        last_pc[idx1] = b;
        last_pc[idx2] = a;
        last_ac[idx1] = b / BITSIZE;
        return false;
      }
    }
  }

  return true;
}

bool lMaxRPC::have_PC_wit(IntVar* i, const int a, IntVar* j, const int b,
                          IntVar* k) {
  const auto c_ik = nei_[i->id()][k->id()];
  const auto c_jk = nei_[j->id()][k->id()];
  const auto cva_ikia = m_->GetIntConValIndex(c_ik->id(), i->id(), a);
  const auto cva_jkjb = m_->GetIntConValIndex(c_jk->id(), j->id(), b);
  // const auto cva_ikia = m_->GetIntConValIndex(c_ik->id(), i->id(), a);
  if (last_ac[cva_ikia] != Limits::INDEX_OVERFLOW) {
    const auto d = last_ac[cva_ikia];
    if ((bitSup_[cva_ikia][d] & bitSup_[cva_jkjb][d] & k->bitDom(level_)[d])
            .any()) {
      return true;
    }
    // const auto d = last_ac[cva_ikia];
    // const auto idx = GetBitIdx(d);
    // if (bitSup_[cva_ikia][get<0>(idx)].test(get<1>(idx))&
    //	bitSup_[cva_jkjb][get<0>(idx)].test(get<1>(idx))&
    //	k->bitDom(level_)[get<0>(idx)].test(get<1>(idx))) {
    //	return true;
    // }
  }

  if (last_ac[cva_jkjb] != Limits::INDEX_OVERFLOW) {
    const auto d = last_ac[cva_jkjb];
    if ((bitSup_[cva_ikia][d] & bitSup_[cva_jkjb][d] & k->bitDom(level_)[d])
            .any()) {
      return true;
    }
    // const auto idx = GetBitIdx(d);
    // if (bitSup_[cva_ikia][get<0>(idx)].test(get<1>(idx))&
    //	bitSup_[cva_jkjb][get<0>(idx)].test(get<1>(idx))&
    //	k->bitDom(level_)[get<0>(idx)].test(get<1>(idx))) {
    //	return true;
    // }
  }

  // const auto c_ik = nei_[i->id()][k->id()];
  // const auto c_jk = nei_[j->id()][k->id()];
  // const int cva_ikia = m_->GetIntConValIndex(c_ik->id(), i->id(), a);
  // const int cva_ikia = m_->GetIntConValIndex(c_jk->id(), j->id(), b);
  // const int bitdom_size = k->num_bit_;
  for (int c = 0; c < k->num_bit_; ++c)
    if ((bitSup_[cva_ikia][c] & bitSup_[cva_jkjb][c] & k->bitDom(level_)[c])
            .any()) {
      last_ac[cva_ikia] = c;
      last_ac[cva_jkjb] = c;
      return true;
    }
  return false;
}

// bool lMaxRPC::is_neibor(IntVar* x, IntVar* y) {
//	auto a = GetBitIdx(x->id());
	//	return neibor_[y][get<0>(a)].test(get<1>(a));
	//}
}
