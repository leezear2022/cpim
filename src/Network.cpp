#include "Network.h"

#include <unordered_set>

namespace cpim {
// void dynamic_bitset::resize(const int size, const bool a) :
//	size_(size) {
//	const int s = ceil(float(size) / BITSIZE);
//	limit_ = s / BITSIZE;
//	if (a) {
//		data_.resize(s, ULLONG_MAX);
//		if (BITSIZE - limit_)
//			data_.back() <<= BITSIZE - limit_;
//	}
//	else
//		data_.resize(s, 0);
// }
//
// auto& dynamic_bitset::operator[](const int i) {
//	const auto index = get_index(i);
//	return data_[get<0>(index)][get<1>(index)];
// }
//
// bool dynamic_bitset::any() {
//	for (auto& a : data_)
//		if (a.any())
//			return true;
//	return false;
// }
//
// bool dynamic_bitset::test(const int idx) {
//	auto a = get_index(idx);
//	return data_[get<0>(a)][get<1>(a)];
// }
//
// int dynamic_bitset::count() {
//	int cnt = 0;
//	for (auto a : data_)
//		cnt += a.count();
//	return cnt;
// }
//
// dynamic_bitset dynamic_bitset::operator|(const dynamic_bitset && lhs, const
// dynamic_bitset && rhs) { 	data_
// }
//
////dynamic_bitset dynamic_bitset::operator|(const dynamic_bitset & t) const {
////	return dynamic_bitset();
////}
//
// dynamic_bitset& dynamic_bitset::operator|=(const dynamic_bitset& t) const {}
//
// tuple<int, int> dynamic_bitset::get_index(const int i) const {
//	tuple<int, int> a;
//	get<0>(a) = i / BITSIZE;
//	get<1>(a) = i % BITSIZE;
//	return a;
//}

IntVar::IntVar(const HVar& v, const int num_vars)
    : id_(v->id),
      init_size_(v->vals.size()),
      limit_(v->vals.size() % BITSIZE),
      num_bit_(ceil(static_cast<float>(v->vals.size()) / BITSIZE)),
      vals_(v->vals) {
  bit_tmp_.resize(num_bit_, ULLONG_MAX);
  if (limit_ != BITSIZE) bit_tmp_.back() >>= BITSIZE - limit_;
  bit_doms_.resize(num_vars + 3, bit_tmp_);
  assigned_.resize(num_vars + 3, false);
}

// IntVar::IntVar(const int id, vector<int>& v) :
//	id_(id),
//	init_size_(v.size()),
//	limit_(v.size() % BITSIZE),
//	num_bit_(ceil(static_cast<float>(v.size()) / BITSIZE))) {
//	//vals_.resize(init_size_);
//	//bit_tmp_.resize(num_bit_, ULLONG_MAX);
//	//if (limit_)
//	//	bit_tmp_.back() >>= BITSIZE - limit_;
//	////for (int i = 0; i < init_size_; ++i) {
//	////	auto idx = get_bit_index(i);
//	////	bit_tmp_[get<0>(idx)].set(get<1>(idx));
//	////}
//	//for (size_t i = 0; i < vals_.size(); ++i) {
//	//	val_map[v[i]] = i;
//	//	vals_[i] = i;
//	//}
// }

void IntVar::RemoveValue(const int a, const int p) {
  const auto index = GetBitIdx(a);
  bit_doms_[p][get<0>(index)].reset(get<1>(index));
  --top_size;
}

void IntVar::ReduceTo(const int a, const int p) {
  const auto index = GetBitIdx(a);
  for (auto& v : bit_doms_[p]) v.reset();
  bit_doms_[p][get<0>(index)].set(get<1>(index));
  top_size = 0;
}

void IntVar::AddValue(const int a, const int p) {
  const auto index = GetBitIdx(a);
  bit_doms_[p][get<0>(index)].set(get<1>(index));
  ++top_size;
}

int IntVar::size(const int p) const {
  int size = 0;
  for (auto& a : bit_doms_[p]) size += a.count();
  return size;
}

int IntVar::next(const int a, const int p) const {
  // for (int i = (a + 1); i < init_size_; ++i) {
  //	const auto index = GetBitIdx(i);
  //	if (bit_doms_[p][get<0>(index)].test(get<1>(index)))
  //		return i;
  // }
  auto index = GetBitIdx(a);
  bitset<BITSIZE> b = (bit_doms_[p][get<0>(index)] >> get<1>(index)) >> 1;
  if (b.any()) return a + FirstOne(b) + 1;

  for (size_t i = get<0>(index) + 1; i < num_bit_; ++i)
    if (bit_doms_[p][i].any()) return GetValue(i, FirstOne(bit_doms_[p][i]));
  return Limits::INDEX_OVERFLOW;
}

void IntVar::next_value(int& a, const int p) {
  //++a;
  // for (; a < init_size_; ++a) {
  //	const auto index = GetBitIdx(a);
  //	if (bit_doms_[p][get<0>(index)].test(get<1>(index)))
  //		return;
  //}
  // a = Limits::INDEX_OVERFLOW;

  auto index = GetBitIdx(a++);
  bitset<BITSIZE> b = bit_doms_[p][get<0>(index)];
  b >>= get<1>(index);
  b >>= 1;

  if (b.any()) {
    a += FirstOne(b);
    return;
  }

  for (size_t i = get<0>(index) + 1; i < num_bit_; ++i)
    if (bit_doms_[p][i].any()) {
      a = GetValue(i, FirstOne(bit_doms_[p][i]));
      return;
    }
  a = Limits::INDEX_OVERFLOW;
}

int IntVar::prev(const int a, const int p) const {
  for (int i = (a - 1); i >= 0; --i) {
    const auto index = GetBitIdx(i);
    if (bit_doms_[p][get<0>(index)].test(get<1>(index))) return i;
  }
  return Limits::INDEX_OVERFLOW;
}

bool IntVar::have(const int a, const int p) const {
  if (a == Limits::INDEX_OVERFLOW) return false;
  const auto index = GetBitIdx(a);
  return bit_doms_[p][get<0>(index)].test(get<1>(index));
}

int IntVar::head(const int p) const {
  // for (int i = 0; i < num_bit_; ++i)
  //	if (bit_doms_[p][i].any()) {
  //		for (int j = 0; j < BITSIZE; ++j) {
  //			if (bit_doms_[p][i].test(j))
  //				return GetValue(i, j);
  //		}
  //	}

  for (size_t i = 0; i < num_bit_; ++i) {
    if (bit_doms_[p][i].any()) return GetValue(i, FirstOne(bit_doms_[p][i]));
  }
  return Limits::INDEX_OVERFLOW;
}

int IntVar::tail(const int p) const {
  for (int i = (num_bit_ - 1); i >= 0; --i)
    if (bit_doms_[p][i].any())
      for (int j = (BITSIZE - 1); j >= 0; --j)
        if (bit_doms_[p][i].test(j)) return GetValue(i, j);
  return Limits::INDEX_OVERFLOW;
}

void IntVar::show(const int p) {
  cout << "id = " << id_ << ": ";
  for (auto a : vals_)
    if (have(a, p)) cout << a << " ";
  cout << "[" << assigned_[p] << "]";
  cout << endl;
}

// tuple<int, int> IntVar::get_bit_index(const int idx) const {
//	tuple<int, int> a;
//	get<0>(a) = idx / BITSIZE;
//	get<1>(a) = idx % BITSIZE;
//	return a;
// }

int IntVar::GetDelete(const int src, const int dest, bitSetVector& del_vals) {
  int size = 0;
  for (int i = 0; i < num_bit_; ++i) {
    del_vals[i] = bit_doms_[src][i] ^ bit_doms_[dest][i];
    size = del_vals[i].count();
  }
  return size;
}

void IntVar::BackTo(const int dest) {
  for (int i = dest; i <= top_; ++i) assigned_[i] = false;
  top_ = dest;
  top_size = size(top_);
}

void IntVar::ClearLevel(const int p) {
  // bit_doms_[p].assign(num_bit_, 0);
  assigned_[p] = false;
}

int IntVar::new_level(const int src) {
  // bit_doms_[(src + 1)].assign(bit_doms_[src].begin(), bit_doms_[src].end());
  copy(src, src + 1);
  top_ = src;
  return (src + 1);
}

void IntVar::copy(const int src, const int dest) {
  bit_doms_[dest].assign(bit_doms_[src].begin(), bit_doms_[src].end());
  assigned_[dest] = assigned_[src];
}

// int IntVar::get_value(const int i, const int j) {
//	return i*BITSIZE + j;
// }

///////////////////////////////////////////////////////////////
const IntVal& IntVal::operator=(const IntVal& rhs) {
  v_ = rhs.v_;
  a_ = rhs.a_;
  aop_ = rhs.aop_;
  return *this;
}

void IntVal::flip() { aop_ = !aop_; }

IntVal IntVal::next(const int p) const {
  return IntVal(v_, v_->next(a_, p), true);
}

bool IntVal::operator==(const IntVal& rhs) {
  return (this == &rhs) || (v_ == rhs.v_ && a_ == rhs.a_ && aop_ == rhs.aop_);
}

bool IntVal::operator!=(const IntVal& rhs) {
  return !((this == &rhs) ||
           (v_ == rhs.v_ && a_ == rhs.a_ && aop_ == rhs.aop_));
}

// tuple<int, int> IntVal::get_bit_index() const {
//	tuple<int, int> a;
//	get<0>(a) = a_ / BITSIZE;
//	get<1>(a) = a_ % BITSIZE;
//	return a;
// }

ostream& operator<<(ostream& os, IntVal& v_val) {
  const string s = (v_val.aop_) ? " = " : " != ";
  os << "(" << v_val.vid() << s << v_val.a_ << ")";
  return os;
}
////////////////////////////////////////////////////////////////////////////

Tabular::Tabular(const HTab& t, const vector<IntVar*>& scp)
    : arity(scp.size()),
      scope(scp),
      weight(1),
      id_(t->id),
      tuples_(t->tuples),
      stamp_(0) {}

bool Tabular::sat(const vector<int>& t) const {
  return binary_search(tuples_.begin(), tuples_.end(), t);
}

void Tabular::GetFirstValidTuple(const IntVal& v_a, vector<int>& t,
                                 const int p) {
  for (int i = 0; i < arity; ++i)
    if (scope[i] != v_a.v())
      t[i] = scope[i]->head(p);
    else
      t[i] = v_a.a();
}

void Tabular::GetNextValidTuple(IntVal& v_a, vector<int>& t, const int p) {
  for (int i = arity - 1; i >= 0; --i) {
    if (scope[i] != v_a.v()) {
      if (scope[i]->next(t[i], p) == Limits::INDEX_OVERFLOW) {
        t[i] = scope[i]->head(p);
      } else {
        t[i] = scope[i]->next(t[i], p);
        return;
      }
    }
  }
  Exclude(t);
}

int Tabular::index(IntVar* v) const {
  for (int i = scope.size() - 1; i >= 0; --i)
    if (scope[i] == v) return i;
  return -1;
}

bool Tabular::IsValidTuple(vector<int>& t, const int p) {
  if (!Existed(t)) return false;

  for (IntVar* v : scope)
    if (!v->have(t[index(v)], p)) return false;
  return true;
}

// ostream & operator<<(ostream & os, IntVal & v_val) {
//	os << "(" << v_val.v->id() << ", " << v_val.a << ")";
//	return os;
// }
///////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////
Network::Network(const HModel& h)
    : hm_(h),
      max_arity_(h->max_arity()),
      max_dom_size_(h->max_domain_size()),
      max_bitDom_size_(ceil(float(h->max_domain_size()) / BITSIZE)),
      num_vars_(h->Vars().size()),
      num_tabs_(h->Tabs().size()) {
  vars.reserve(num_vars_);
  tabs.reserve(num_tabs_);
  nei_.resize(num_vars_);
  for (const auto& hv : hm_->Vars()) {
    IntVar* v = new IntVar(hv, num_vars_);
    vars.push_back(v);
  }

  for (auto ht : hm_->Tabs()) {
    Tabular* t = new Tabular(ht, get_scope(ht));
    tabs.push_back(t);
  }

  for (auto t : tabs)
    for (auto v : t->scope) subscription[v].push_back(t);

  for (auto v : vars) {
    neighborhood[v] = get_neighbor(v);
    nei_[v->id()] = neighborhood[v];
  }

  tmp_ = vars.size() + 2;
}

void Network::GetFirstValidTuple(IntConVal& c_val, vector<int>& t,
                                 const int p) {
  IntVal v_a(c_val.v(), c_val.a());
  c_val.c()->GetFirstValidTuple(v_a, t, p);
}

void Network::GetNextValidTuple(IntConVal& c_val, vector<int>& t, const int p) {
  IntVal v_a(c_val.v(), c_val.a());
  c_val.c()->GetNextValidTuple(v_a, t, p);
}

// int Network::GetIntConValIndex(IntConVal& c_val) const {
//	return  c_val.c()->id() * max_arity_ * max_dom_size_ +
// c_val.c()->index(c_val.v()) * max_dom_size_ + c_val.a();
// }

// int Network::GetIntConValIndex(const int c_id, const int v_id, const int a)
// IntConVal Network::GetIntConVal(const int index) {
//	const int c_id = index / tabs.size();
//	const int v_id = index % tabs.size() / max_dom_size_;
//	const int a = index % tabs.size() % max_dom_size_;
//	IntConVal c(tabs[c_id], tabs[c_id]->scope[v_id], a);
//	return c;
// }

int Network::NewLevel(const int src) {
  top_ = src + 1;
  for (auto v : vars) v->new_level(src);
  return top_;
}

void Network::BackTo(const int p) {
  for (auto v : vars) v->BackTo(p);
}

void Network::CopyLevel(const int src, const int dest) {
  if (src == dest) return;
  for (auto v : vars) v->copy(src, dest);
}

void Network::ClearLevel(const int p) {
  for (auto v : vars) v->ClearLevel(p);
}

// int Network::NewTmpLevel()
//{
// }

// void Network::RestoreUpto(const int level) {
//	for (IntVar* v : vars)
//		//if (!v->assigned())
//		v->RestoreUpTo(level);
// }

vector<IntVar*> Network::get_neighbor(IntVar* v) {
  unordered_set<IntVar*> vs;
  for (auto c : subscription[v])
    for (auto x : c->scope)
      if (x != v) vs.insert(x);

  return vector<IntVar*>(vs.begin(), vs.end());
}

void Network::show(const int p) {
  for (auto v : vars) v->show(p);
  // for (auto t : tabs)
  //	t->show(p);
}

Network::~Network() {
  for (auto v : vars) delete v;
  for (auto t : tabs) delete t;
  vars.clear();
  tabs.clear();
}

vector<IntVar*> Network::get_scope(const HTab& t) {
  vector<IntVar*> tt(t->scope.size());
  for (int i = 0; i < t->scope.size(); ++i) tt[i] = vars[t->scope[i]->id];
  return tt;
}

void Network::get_scope(const HTab& t, vector<IntVar*> scp) {
  for (int i = 0; i < t->scope.size(); ++i) scp[i] = vars[t->scope[i]->id];
}

const IntConVal& IntConVal::operator=(const IntConVal& rhs) {
  c_ = rhs.c_;
  v_ = rhs.v_;
  a_ = rhs.a_;

  return *this;
}
}  // namespace cp
