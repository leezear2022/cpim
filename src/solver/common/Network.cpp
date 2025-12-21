#include "Network.h"

#include <algorithm>
#include <cmath>
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

// Phase 1.1: 新构造函数使用 UnifiedTrail
IntVar::IntVar(int id, int domain_size, UnifiedTrail* trail)
    : id_(id),
      init_size_(domain_size),
      limit_(domain_size % BITSIZE),
      num_bit_(domain_size == 0
                   ? 0
                   : static_cast<int>(std::ceil(static_cast<float>(domain_size) /
                                               BITSIZE))),
      vals_(domain_size),
      trail_(trail) {
  if (num_bit_ == 0 && init_size_ > 0) {
    num_bit_ = 1;
  }

  // Phase 1.1: 单层域初始化 (vs 多级域)
  bit_tmp_.resize(num_bit_, ULLONG_MAX);
  if (!bit_tmp_.empty() && limit_ != BITSIZE && limit_ != 0) {
    bit_tmp_.back() >>= (BITSIZE - limit_);
  }

  // Phase 1.1: 单层域拷贝
  bit_doms_ = bit_tmp_;

  // Phase 1.1: 单一 assigned 状态
  assigned_ = false;

  for (int i = 0; i < init_size_; ++i) {
    vals_[i] = i;
  }
  top_ = 0;
  top_size = init_size_;
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

// Phase 1.1: 域修改前记录到 Trail
void IntVar::RemoveValue(const int a) {
  const auto index = GetBitIdx(a);
  const int word_idx = get<0>(index);

  // 记录旧值到 Trail
  if (trail_) {
    trail_->RecordDomainChange(id_, word_idx, bit_doms_[word_idx].to_ullong());
  }

  // 执行删除
  bit_doms_[word_idx].reset(get<1>(index));
  --top_size;
}

void IntVar::ReduceTo(const int a) {
  const auto index = GetBitIdx(a);
  const int target_word = get<0>(index);

  // 记录所有 word 的旧值到 Trail
  if (trail_) {
    for (int i = 0; i < static_cast<int>(bit_doms_.size()); ++i) {
      if (bit_doms_[i].any()) {  // 只记录非零 word
        trail_->RecordDomainChange(id_, i, bit_doms_[i].to_ullong());
      }
    }
  }

  // 执行 ReduceTo (清空所有,只设置目标值)
  for (auto& v : bit_doms_) v.reset();
  bit_doms_[target_word].set(get<1>(index));
  top_size = 1;  // Bug Fix: 域只包含一个值，应该是 1 而不是 0
  assigned_ = true;  // Phase 1.1: 单一 assigned 状态
}

void IntVar::AddValue(const int a) {
  const auto index = GetBitIdx(a);
  const int word_idx = get<0>(index);

  // 记录旧值到 Trail
  if (trail_) {
    trail_->RecordDomainChange(id_, word_idx, bit_doms_[word_idx].to_ullong());
  }

  // 执行添加
  bit_doms_[word_idx].set(get<1>(index));
  ++top_size;
}

// Phase 1.1: Trail 域恢复方法
void IntVar::RestoreBitWord(int word_idx, uint32_t bits) {
  bit_doms_[word_idx] = std::bitset<BITSIZE>(bits);

  // Bug Fix: 重新计算 top_size 和 assigned_（修复搜索节点数异常问题）
  // 原因：min-domain 启发式依赖 top_size，回溯后必须更新缓存值
  top_size = 0;
  for (const auto& w : bit_doms_) {
    top_size += w.count();
  }
  assigned_ = (top_size == 1);  // 动态判断是否已赋值
}

// Phase 1.1: 新的无参数版本 (访问单层域)
int IntVar::size() const {
  int size = 0;
  for (auto& a : bit_doms_) size += a.count();
  return size;
}

int IntVar::next(const int a) const {
  auto index = GetBitIdx(a);
  bitset<BITSIZE> b = (bit_doms_[get<0>(index)] >> get<1>(index)) >> 1;
  if (b.any()) return a + FirstOne(b) + 1;

  for (size_t i = get<0>(index) + 1; i < num_bit_; ++i)
    if (bit_doms_[i].any()) return GetValue(i, FirstOne(bit_doms_[i]));
  return Limits::INDEX_OVERFLOW;
}

void IntVar::next_value(int& a) {
  auto index = GetBitIdx(a++);
  bitset<BITSIZE> b = bit_doms_[get<0>(index)];
  b >>= get<1>(index);
  b >>= 1;

  if (b.any()) {
    a += FirstOne(b);
    return;
  }

  for (size_t i = get<0>(index) + 1; i < num_bit_; ++i)
    if (bit_doms_[i].any()) {
      a = GetValue(i, FirstOne(bit_doms_[i]));
      return;
    }
  a = Limits::INDEX_OVERFLOW;
}

bool IntVar::have(const int a) const {
  if (a == Limits::INDEX_OVERFLOW) return false;
  const auto index = GetBitIdx(a);
  return bit_doms_[get<0>(index)].test(get<1>(index));
}

int IntVar::head() const {
  for (size_t i = 0; i < num_bit_; ++i) {
    if (bit_doms_[i].any()) return GetValue(i, FirstOne(bit_doms_[i]));
  }
  return Limits::INDEX_OVERFLOW;
}

int IntVar::tail() const {
  for (int i = (num_bit_ - 1); i >= 0; --i)
    if (bit_doms_[i].any())
      return GetValue(i, BITSIZE - FirstOne(bitset<BITSIZE>(bit_doms_[i].to_ullong()).flip()) - 1);
  return Limits::INDEX_OVERFLOW;
}

// ========== 以下为旧的多级域版本(已废弃) ==========
// Phase 1.1: Deprecated - int IntVar::size(const int p) const {
// Phase 1.1: Deprecated -   int size = 0;
// Phase 1.1: Deprecated -   for (auto& a : bit_doms_[p]) size += a.count();
// Phase 1.1: Deprecated -   return size;
// Phase 1.1: Deprecated - }

// Phase 1.1: Deprecated - int IntVar::next(const int a, const int p) const {
// Phase 1.1: Deprecated -   // for (int i = (a + 1); i < init_size_; ++i) {
// Phase 1.1: Deprecated -   //	const auto index = GetBitIdx(i);
// Phase 1.1: Deprecated -   //	if (bit_doms_[p][get<0>(index)].test(get<1>(index)))
// Phase 1.1: Deprecated -   //		return i;
// Phase 1.1: Deprecated -   // }
// Phase 1.1: Deprecated -   auto index = GetBitIdx(a);
// Phase 1.1: Deprecated -   bitset<BITSIZE> b = (bit_doms_[p][get<0>(index)] >> get<1>(index)) >> 1;
// Phase 1.1: Deprecated -   if (b.any()) return a + FirstOne(b) + 1;
// Phase 1.1: Deprecated - 
// Phase 1.1: Deprecated -   for (size_t i = get<0>(index) + 1; i < num_bit_; ++i)
// Phase 1.1: Deprecated -     if (bit_doms_[p][i].any()) return GetValue(i, FirstOne(bit_doms_[p][i]));
// Phase 1.1: Deprecated -   return Limits::INDEX_OVERFLOW;
// Phase 1.1: Deprecated - }

// Phase 1.1: Deprecated - void IntVar::next_value(int& a, const int p) {
// Phase 1.1: Deprecated -   //++a;
// Phase 1.1: Deprecated -   // for (; a < init_size_; ++a) {
// Phase 1.1: Deprecated -   //	const auto index = GetBitIdx(a);
// Phase 1.1: Deprecated -   //	if (bit_doms_[p][get<0>(index)].test(get<1>(index)))
// Phase 1.1: Deprecated -   //		return;
// Phase 1.1: Deprecated -   //}
// Phase 1.1: Deprecated -   // a = Limits::INDEX_OVERFLOW;
// Phase 1.1: Deprecated - 
// Phase 1.1: Deprecated -   auto index = GetBitIdx(a++);
// Phase 1.1: Deprecated -   bitset<BITSIZE> b = bit_doms_[p][get<0>(index)];
// Phase 1.1: Deprecated -   b >>= get<1>(index);
// Phase 1.1: Deprecated -   b >>= 1;
// Phase 1.1: Deprecated - 
// Phase 1.1: Deprecated -   if (b.any()) {
// Phase 1.1: Deprecated -     a += FirstOne(b);
// Phase 1.1: Deprecated -     return;
// Phase 1.1: Deprecated -   }
// Phase 1.1: Deprecated - 
// Phase 1.1: Deprecated -   for (size_t i = get<0>(index) + 1; i < num_bit_; ++i)
// Phase 1.1: Deprecated -     if (bit_doms_[p][i].any()) {
// Phase 1.1: Deprecated -       a = GetValue(i, FirstOne(bit_doms_[p][i]));
// Phase 1.1: Deprecated -       return;
// Phase 1.1: Deprecated -     }
// Phase 1.1: Deprecated -   a = Limits::INDEX_OVERFLOW;
// Phase 1.1: Deprecated - }

// Phase 1.1: Deprecated - int IntVar::prev(const int a, const int p) const {
// Phase 1.1: Deprecated -   for (int i = (a - 1); i >= 0; --i) {
// Phase 1.1: Deprecated -     const auto index = GetBitIdx(i);
// Phase 1.1: Deprecated -     if (bit_doms_[p][get<0>(index)].test(get<1>(index))) return i;
// Phase 1.1: Deprecated -   }
// Phase 1.1: Deprecated -   return Limits::INDEX_OVERFLOW;
// Phase 1.1: Deprecated - }

// Phase 1.1: Deprecated - bool IntVar::have(const int a, const int p) const {
// Phase 1.1: Deprecated -   if (a == Limits::INDEX_OVERFLOW) return false;
// Phase 1.1: Deprecated -   const auto index = GetBitIdx(a);
// Phase 1.1: Deprecated -   return bit_doms_[p][get<0>(index)].test(get<1>(index));
// Phase 1.1: Deprecated - }

// Phase 1.1: Deprecated - int IntVar::head(const int p) const {
// Phase 1.1: Deprecated -   // for (int i = 0; i < num_bit_; ++i)
// Phase 1.1: Deprecated -   //	if (bit_doms_[p][i].any()) {
// Phase 1.1: Deprecated -   //		for (int j = 0; j < BITSIZE; ++j) {
// Phase 1.1: Deprecated -   //			if (bit_doms_[p][i].test(j))
// Phase 1.1: Deprecated -   //				return GetValue(i, j);
// Phase 1.1: Deprecated -   //		}
// Phase 1.1: Deprecated -   //	}
// Phase 1.1: Deprecated - 
// Phase 1.1: Deprecated -   for (size_t i = 0; i < num_bit_; ++i) {
// Phase 1.1: Deprecated -     if (bit_doms_[p][i].any()) return GetValue(i, FirstOne(bit_doms_[p][i]));
// Phase 1.1: Deprecated -   }
// Phase 1.1: Deprecated -   return Limits::INDEX_OVERFLOW;
// Phase 1.1: Deprecated - }

// Phase 1.1: Deprecated - int IntVar::tail(const int p) const {
// Phase 1.1: Deprecated -   for (int i = (num_bit_ - 1); i >= 0; --i)
// Phase 1.1: Deprecated -     if (bit_doms_[p][i].any())
// Phase 1.1: Deprecated -       for (int j = (BITSIZE - 1); j >= 0; --j)
// Phase 1.1: Deprecated -         if (bit_doms_[p][i].test(j)) return GetValue(i, j);
// Phase 1.1: Deprecated -   return Limits::INDEX_OVERFLOW;
// Phase 1.1: Deprecated - }

void IntVar::show() {
  cout << "id = " << id_ << ": ";
  for (auto a : vals_)
    if (have(a)) cout << a << " ";
  cout << "[" << assigned_ << "]";
  cout << endl;
}

// Phase 1.1: Deprecated - void IntVar::show(const int p) {
// Phase 1.1: Deprecated -   cout << "id = " << id_ << ": ";
// Phase 1.1: Deprecated -   for (auto a : vals_)
// Phase 1.1: Deprecated -     if (have(a, p)) cout << a << " ";
// Phase 1.1: Deprecated -   cout << "[" << assigned_[p] << "]";
// Phase 1.1: Deprecated -   cout << endl;
// Phase 1.1: Deprecated - }

// tuple<int, int> IntVar::get_bit_index(const int idx) const {
//	tuple<int, int> a;
//	get<0>(a) = idx / BITSIZE;
//	get<1>(a) = idx % BITSIZE;
//	return a;
// }

// Phase 1.1: Deprecated - int IntVar::GetDelete(const int src, const int dest, bitSetVector& del_vals) {
// Phase 1.1: Deprecated -   int size = 0;
// Phase 1.1: Deprecated -   for (int i = 0; i < num_bit_; ++i) {
// Phase 1.1: Deprecated -     del_vals[i] = bit_doms_[src][i] ^ bit_doms_[dest][i];
// Phase 1.1: Deprecated -     size = del_vals[i].count();
// Phase 1.1: Deprecated -   }
// Phase 1.1: Deprecated -   return size;
// Phase 1.1: Deprecated - }

// Phase 1.1: Deprecated - void IntVar::BackTo(const int dest) {
// Phase 1.1: Deprecated -   for (int i = dest; i <= top_; ++i) assigned_[i] = false;
// Phase 1.1: Deprecated -   top_ = dest;
// Phase 1.1: Deprecated -   top_size = size(top_);
// Phase 1.1: Deprecated - }

// Phase 1.1: Deprecated - void IntVar::ClearLevel(const int p) {
// Phase 1.1: Deprecated -   // bit_doms_[p].assign(num_bit_, 0);
// Phase 1.1: Deprecated -   assigned_[p] = false;
// Phase 1.1: Deprecated - }

// Phase 1.1: Deprecated - int IntVar::new_level(const int src) {
// Phase 1.1: Deprecated -   // bit_doms_[(src + 1)].assign(bit_doms_[src].begin(), bit_doms_[src].end());
// Phase 1.1: Deprecated -   copy(src, src + 1);
// Phase 1.1: Deprecated -   top_ = src;
// Phase 1.1: Deprecated -   return (src + 1);
// Phase 1.1: Deprecated - }

// Phase 1.1: Deprecated - void IntVar::copy(const int src, const int dest) {
// Phase 1.1: Deprecated -   bit_doms_[dest].assign(bit_doms_[src].begin(), bit_doms_[src].end());
// Phase 1.1: Deprecated -   assigned_[dest] = assigned_[src];
// Phase 1.1: Deprecated - }

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
  return IntVal(v_, v_->next(a_), true);
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

ostream& operator<<(ostream& os, const IntVal& v_val) {
  const string s = (v_val.aop_) ? " = " : " != ";
  os << "(" << v_val.vid() << s << v_val.a_ << ")";
  return os;
}
////////////////////////////////////////////////////////////////////////////

Tabular::Tabular(int id, std::vector<IntVar*> scp,
                         std::vector<std::vector<int>> tuples)
    : arity(scp.size()),
      scope(std::move(scp)),
      weight(1),
      id_(id),
      tuples_(std::move(tuples)),
      stamp_(0) {
  std::sort(tuples_.begin(), tuples_.end());
}

bool Tabular::sat(const vector<int>& t) const {
  return binary_search(tuples_.begin(), tuples_.end(), t);
}

void Tabular::GetFirstValidTuple(const IntVal& v_a, vector<int>& t,
                                 const int p) {
  for (int i = 0; i < arity; ++i)
    if (scope[i] != v_a.v())
      t[i] = scope[i]->head();
    else
      t[i] = v_a.a();
}

void Tabular::GetNextValidTuple(IntVal& v_a, vector<int>& t, const int p) {
  for (int i = arity - 1; i >= 0; --i) {
    if (scope[i] != v_a.v()) {
      if (scope[i]->next(t[i]) == Limits::INDEX_OVERFLOW) {
        t[i] = scope[i]->head();
      } else {
        t[i] = scope[i]->next(t[i]);
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
    if (!v->have(t[index(v)])) return false;
  return true;
}

// ostream & operator<<(ostream & os, IntVal & v_val) {
//	os << "(" << v_val.v->id() << ", " << v_val.a << ")";
//	return os;
// }
///////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////
Network::Network(const model::IntermediateModel& intermediate)
    : max_arity_(0),
      max_dom_size_(0),
      max_bitDom_size_(0),
      num_vars_(intermediate.num_variables()),
      num_tabs_(0) {
  // Phase 1.1: 创建 UnifiedTrail (估计最大 Trail 容量)
  const int estimated_trail_capacity = 1000000;  // 100 万条目 (~16MB)
  trail_ = new UnifiedTrail(estimated_trail_capacity, false);  // CPU-only
  // Phase 1.1: Trail 需要访问变量数组进行域恢复
  trail_->SetVariables(&vars);

  vars.reserve(num_vars_);
  tabs.reserve(intermediate.num_constraints());
  nei_.resize(num_vars_);

  for (const auto& var : intermediate.variables()) {
    const auto& domain = intermediate.GetDomain(var.domain);
    const int domain_size = domain.Size();
    max_dom_size_ = std::max(max_dom_size_, domain_size);

    // Phase 1.1: 传递 Trail 指针给 IntVar
    auto* v = new IntVar(var.id.value, domain_size, trail_);
    vars.push_back(v);
  }

  max_bitDom_size_ = max_dom_size_ == 0
                         ? 0
                         : static_cast<int>(std::ceil(static_cast<float>(max_dom_size_) /
                                                       BITSIZE));

  for (const auto& constraint : intermediate.constraints()) {
    const auto* ext = std::get_if<model::ExtensionConstraint>(&constraint.data);
    if (!ext) {
      continue;
    }
    if (ext->semantics != model::ExtensionConstraint::Semantics::kSupports) {
      continue;
    }
    std::vector<IntVar*> scope;
    scope.reserve(ext->scope.size());
    for (model::VariableId vid : ext->scope) {
      scope.push_back(vars[vid.value]);
    }
    max_arity_ = std::max<int>(max_arity_, scope.size());
    auto tuples = ext->tuples;
    auto* t = new Tabular(constraint.id.value, std::move(scope), std::move(tuples));
    tabs.push_back(t);
  }

  num_tabs_ = static_cast<int>(tabs.size());

  for (auto* t : tabs)
    for (auto* v : t->scope) subscription[v].push_back(t);

  for (auto* v : vars) {
    neighborhood[v] = get_neighbor(v);
    nei_[v->id()] = neighborhood[v];
  }

  tmp_ = static_cast<int>(vars.size()) + 2;
}

void Network::GetFirstValidTuple(const IntConVal& c_val, vector<int>& t,
                                 const int p) {
  IntVal v_a(c_val.v(), c_val.a());
  c_val.c()->GetFirstValidTuple(v_a, t, p);
}

void Network::GetNextValidTuple(const IntConVal& c_val, vector<int>& t, const int p) {
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

// Phase 1.1: Deprecated - int Network::NewLevel(const int src) {
// Phase 1.1: Deprecated -   top_ = src + 1;
// Phase 1.1: Deprecated -   for (auto v : vars) v->new_level(src);
// Phase 1.1: Deprecated -   return top_;
// Phase 1.1: Deprecated - }

// Phase 1.1: Deprecated - void Network::BackTo(const int p) {
// Phase 1.1: Deprecated -   for (auto v : vars) v->BackTo(p);
// Phase 1.1: Deprecated - }

// Phase 1.1: Deprecated - void Network::CopyLevel(const int src, const int dest) {
// Phase 1.1: Deprecated -   if (src == dest) return;
// Phase 1.1: Deprecated -   for (auto v : vars) v->copy(src, dest);
// Phase 1.1: Deprecated - }

// Phase 1.1: Deprecated - void Network::ClearLevel(const int p) {
// Phase 1.1: Deprecated -   for (auto v : vars) v->ClearLevel(p);
// Phase 1.1: Deprecated - }

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
  for (auto v : vars) v->show();
  // for (auto t : tabs)
  //	t->show();
}

Network::~Network() {
  // Phase 1.1: 删除 Trail
  if (trail_) {
    delete trail_;
    trail_ = nullptr;
  }

  for (const auto v : vars) delete v;
  for (const auto t : tabs) delete t;
  vars.clear();
  tabs.clear();
}

const IntConVal& IntConVal::operator=(const IntConVal& rhs) {
  c_ = rhs.c_;
  v_ = rhs.v_;
  a_ = rhs.a_;

  return *this;
}
}  // namespace cpim
