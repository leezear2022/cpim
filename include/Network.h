#pragma once
#include <bitset>
#include <climits>
#include <vector>

#include "NaiveBitSet.h"
#include "xcsp3model/XBuilder.h"

using namespace std;
namespace cpim {
using namespace std;
using namespace common;

static bool Existed(vector<int>& tuple) { return tuple[0] != INT_MAX; }

static void Exclude(vector<int>& tuple) { tuple[0] = INT_MAX; }

const uint64_t MASK1_64[64] = {
    0x8000000000000000, 0x4000000000000000, 0x2000000000000000,
    0x1000000000000000, 0x0800000000000000, 0x0400000000000000,
    0x0200000000000000, 0x0100000000000000, 0x0080000000000000,
    0x0040000000000000, 0x0020000000000000, 0x0010000000000000,
    0x0008000000000000, 0x0004000000000000, 0x0002000000000000,
    0x0001000000000000, 0x0000800000000000, 0x0000400000000000,
    0x0000200000000000, 0x0000100000000000, 0x0000080000000000,
    0x0000040000000000, 0x0000020000000000, 0x0000010000000000,
    0x0000008000000000, 0x0000004000000000, 0x0000002000000000,
    0x0000001000000000, 0x0000000800000000, 0x0000000400000000,
    0x0000000200000000, 0x0000000100000000, 0x0000000080000000,
    0x0000000040000000, 0x0000000020000000, 0x0000000010000000,
    0x0000000008000000, 0x0000000004000000, 0x0000000002000000,
    0x0000000001000000, 0x0000000000800000, 0x0000000000400000,
    0x0000000000200000, 0x0000000000100000, 0x0000000000080000,
    0x0000000000040000, 0x0000000000020000, 0x0000000000010000,
    0x0000000000008000, 0x0000000000004000, 0x0000000000002000,
    0x0000000000001000, 0x0000000000000800, 0x0000000000000400,
    0x0000000000000200, 0x0000000000000100, 0x0000000000000080,
    0x0000000000000040, 0x0000000000000020, 0x0000000000000010,
    0x0000000000000008, 0x0000000000000004, 0x0000000000000002,
    0x0000000000000001,
};

const uint64_t MASK0_64[64] = {
    0x7FFFFFFFFFFFFFFF, 0xBFFFFFFFFFFFFFFF, 0xDFFFFFFFFFFFFFFF,
    0xEFFFFFFFFFFFFFFF, 0xF7FFFFFFFFFFFFFF, 0xFBFFFFFFFFFFFFFF,
    0xFDFFFFFFFFFFFFFF, 0xFEFFFFFFFFFFFFFF, 0xFF7FFFFFFFFFFFFF,
    0xFFBFFFFFFFFFFFFF, 0xFFDFFFFFFFFFFFFF, 0xFFEFFFFFFFFFFFFF,
    0xFFF7FFFFFFFFFFFF, 0xFFFBFFFFFFFFFFFF, 0xFFFDFFFFFFFFFFFF,
    0xFFFEFFFFFFFFFFFF, 0xFFFF7FFFFFFFFFFF, 0xFFFFBFFFFFFFFFFF,
    0xFFFFDFFFFFFFFFFF, 0xFFFFEFFFFFFFFFFF, 0xFFFFF7FFFFFFFFFF,
    0xFFFFFBFFFFFFFFFF, 0xFFFFFDFFFFFFFFFF, 0xFFFFFEFFFFFFFFFF,
    0xFFFFFF7FFFFFFFFF, 0xFFFFFFBFFFFFFFFF, 0xFFFFFFDFFFFFFFFF,
    0xFFFFFFEFFFFFFFFF, 0xFFFFFFF7FFFFFFFF, 0xFFFFFFFBFFFFFFFF,
    0xFFFFFFFDFFFFFFFF, 0xFFFFFFFEFFFFFFFF, 0xFFFFFFFF7FFFFFFF,
    0xFFFFFFFFBFFFFFFF, 0xFFFFFFFFDFFFFFFF, 0xFFFFFFFFEFFFFFFF,
    0xFFFFFFFFF7FFFFFF, 0xFFFFFFFFFBFFFFFF, 0xFFFFFFFFFDFFFFFF,
    0xFFFFFFFFFEFFFFFF, 0xFFFFFFFFFF7FFFFF, 0xFFFFFFFFFFBFFFFF,
    0xFFFFFFFFFFDFFFFF, 0xFFFFFFFFFFEFFFFF, 0xFFFFFFFFFFF7FFFF,
    0xFFFFFFFFFFFBFFFF, 0xFFFFFFFFFFFDFFFF, 0xFFFFFFFFFFFEFFFF,
    0xFFFFFFFFFFFF7FFF, 0xFFFFFFFFFFFFBFFF, 0xFFFFFFFFFFFFDFFF,
    0xFFFFFFFFFFFFEFFF, 0xFFFFFFFFFFFFF7FF, 0xFFFFFFFFFFFFFBFF,
    0xFFFFFFFFFFFFFDFF, 0xFFFFFFFFFFFFFEFF, 0xFFFFFFFFFFFFFF7F,
    0xFFFFFFFFFFFFFFBF, 0xFFFFFFFFFFFFFFDF, 0xFFFFFFFFFFFFFFEF,
    0xFFFFFFFFFFFFFFF7, 0xFFFFFFFFFFFFFFFB, 0xFFFFFFFFFFFFFFFD,
    0xFFFFFFFFFFFFFFFE,
};

namespace Limits {
/**
 * \brief 取值范围
 */
const int MIN_INTVAR_ID = 0x7fff7000;
const int MAX_INTVAR_ID = INT_MAX - 1;
const int MAX_OPT = INT_MIN & 0xffff7000 - 1;
const int MIN_OPT = INT_MIN + 1;
const int UNSIGNED_VAL = INT_MIN & 0xffff7000;
const int MIN_VAL = UNSIGNED_VAL + 1;
const int MAX_VAL = MIN_INTVAR_ID - 1;
const int INDEX_OVERFLOW = -1;
const int PRESENT = -1;
const int ABSENT = 0;
}  // namespace Limits

const int BITSIZE = 64;
const int DIV_BIT = 6;
const int MOD_MASK = 0x3f;

// class dynamic_bitset {
// public:
//	dynamic_bitset() {};
//	void resize(const int size, bool a = false);
//	auto& operator[](const int i);
//	bool any();
//	bool test(const int idx);
//	int count();
//	dynamic_bitset operator|(const dynamic_bitset &&lhs, const
// dynamic_bitset &&rhs); 	dynamic_bitset& operator|=(const dynamic_bitset
// &t) const; protected: 	tuple<int, int> get_index(const int i) const;
//	vector<bitset<BITSIZE>> data_;
//	int size_;
//	int limit_;
// };

// using bitSetVector = NaiveBitSet;

inline tuple<int, int> GetBitIdx(const int idx) {
  tuple<int, int> a;
  get<0>(a) = idx >> DIV_BIT;
  get<1>(a) = idx & MOD_MASK;
  return a;
}
inline int GetValue(const int i, const int j) { return (i << DIV_BIT) + j; }
// typedef vector<bitset<BITSIZE>> bitSetVector;

inline uint64_t FirstOne(const bitset<BITSIZE>& UseMask) {
  uint64_t index = UseMask.to_ullong();
  // 将第一个为1位的低位都置1，其它位都置0
  index = (index - 1) & ~index;
  // 得到有多少为1的位
  index = (index & 0x5555555555555555) + ((index >> 1) & 0x5555555555555555);
  index = (index & 0x3333333333333333) + ((index >> 2) & 0x3333333333333333);
  index = (index & 0x0F0F0F0F0F0F0F0F) + ((index >> 4) & 0x0F0F0F0F0F0F0F0F);
  index = (index & 0x00FF00FF00FF00FF) + ((index >> 8) & 0x00FF00FF00FF00FF);
  index = (index & 0x0000ffff0000ffff) + ((index >> 16) & 0x0000ffff0000ffff);
  index = (index & 0xFFFFFFFF) + ((index & 0xFFFFFFFF00000000) >> 32);
  // 得到位数,如果为32则表示全0
  return index;
}

inline uint64_t FirstOne(const uint64_t UseMask) {
  uint64_t index = UseMask;
  // 将第一个为1位的低位都置1，其它位都置0
  index = (index - 1) & ~index;
  // 得到有多少为1的位
  index = (index & 0x5555555555555555) + ((index >> 1) & 0x5555555555555555);
  index = (index & 0x3333333333333333) + ((index >> 2) & 0x3333333333333333);
  index = (index & 0x0F0F0F0F0F0F0F0F) + ((index >> 4) & 0x0F0F0F0F0F0F0F0F);
  index = (index & 0x00FF00FF00FF00FF) + ((index >> 8) & 0x00FF00FF00FF00FF);
  index = (index & 0x0000ffff0000ffff) + ((index >> 16) & 0x0000ffff0000ffff);
  index = (index & 0xFFFFFFFF) + ((index & 0xFFFFFFFF00000000) >> 32);
  // 得到位数,如果为32则表示全0
  return index;
}

class IntVar {
 public:
  IntVar(const HVar& v, int vs_size);
  IntVar(){};
  // IntVar(const int id, vector<int>& v);
  ~IntVar() {}
  bool operator==(const IntVar& int_var) const;
  ;
  void RemoveValue(int a, int p = 0);
  void ReduceTo(int a, int p = 0);
  // void AddValue(const int a, const int p = 0);
  // void RestoreUpTo(const int p);
  int value(const int idx) const { return vals_[idx]; }
  int size(const int p) const;
  int capacity() const { return init_size_; }
  bool assigned(const int p) const { return assigned_[p]; }
  void assign(const bool a, const int p) { assigned_[p] = a; }
  int next(const int a, const int p) const;
  // void next_value(int& a, const int p);
  int prev(const int a, const int p) const;
  bool have(const int a, const int p) const;
  int head(const int p) const;
  int tail(const int p) const;
  bool faild(const int p) const { return size(p) == 0; };
  int stamp() const { return stamp_; }
  void stamp(const int s) { stamp_ = s; }
  NaiveBitSet& bitDom(const int p) { return bit_doms_[p]; }
  int id() const { return id_; }
  void show(const int p);
  // inline tuple<int, int> get_bit_index(const int idx) const;
  vector<int>& values() { return vals_; }
  int GetDelete(const int src, const int dest, NaiveBitSet& del_vals);
  void BackTo(const int dest);
  void ClearLevel(const int p);
  int new_level(int src);
  void copy(const int src, const int dest);
  int top_size = 0;
  int num_bit_;

 protected:
  int id_;
  int init_size_;
  int value_ = -1;
  uint64_t stamp_ = 0;
  vector<bool> assigned_;
  int limit_;
  vector<int> vals_;
  int top_;
  // unordered_map<int, int> val_map;
  // vector<int> anti_map;
  // vector<bitSetVector> bit_doms_;
  // bitSetVector bit_tmp_;
  // static inline int get_value(const int i, const int j);
  // vector<uint64_t> tmp_;

  vector<NaiveBitSet> bit_doms_;
  NaiveBitSet bit_tmp_;
};

using Val_A = tuple<weak_ptr<IntVar>, int, bool>;

class IntVal {
 public:
  IntVal() : a_(-2) {}
  IntVal(const std::shared_ptr<IntVar>& v, const int a, const bool aop = true)
      : v_(v), a_(a), aop_(aop) {}
  const IntVal& operator=(const IntVal& rhs);
  [[nodiscard]] std::shared_ptr<IntVar> v() const { return v_; }
  void v(const std::shared_ptr<IntVar>& v) { v_ = v; }
  void a(int a) { a_ = a; }
  int vid() const { return v_ ? v_->id() : -1; }
  int a() const { return a_; }
  bool op() const { return aop_; }
  void flip() { aop_ = !aop_; }
  IntVal next(int p) const;
  bool operator==(const IntVal& rhs) const;
  bool operator!=(const IntVal& rhs) const;
  friend std::ostream& operator<<(std::ostream& os, const IntVal& v_val);

  ~IntVal() = default;

 private:
  std::shared_ptr<IntVar> v_;
  int a_;
  bool aop_;
};

static const IntVal Nil_Val(nullptr, -1);

class Tabular {
 public:
  Tabular(const HTab& t, const vector<shared_ptr<IntVar>>& scp);
  // Tabular(const int id, const std::vector<IntVar *>& scope,
  // vector<vector<int>>& ts, const int len);
  bool sat(vector<int>& t) const;
  ~Tabular() {}
  void GetFirstValidTuple(IntVal& v_a, vector<int>& t, int p);
  void GetNextValidTuple(IntVal& v_a, vector<int>& t, int p);
  int index(const shared_ptr<IntVar>& v) const;
  bool IsValidTuple(vector<int>& t, const int p);
  int id() const { return id_; }
  void stamp(const int s) { stamp_ = s; }
  int stamp() const { return stamp_; }
  size_t arity;
  vector<shared_ptr<IntVar>> scope;
  const vector<vector<int>>& tuples() const { return tuples_; }
  float weight;
  int id_;
  vector<vector<int>>& tuples_;
  uint64_t stamp_ = 0;

 private:
};

class arc {
 public:
  arc() = default;
  arc(const std::shared_ptr<Tabular>& c, const std::shared_ptr<IntVar>& v)
      : c_(c), v_(v) {}
  virtual ~arc() = default;

  void c(const std::shared_ptr<Tabular>& val) { c_ = val; }
  std::shared_ptr<Tabular> c() { return c_; }
  std::shared_ptr<IntVar> v() { return v_; }
  [[nodiscard]] int c_id() const { return c_ ? c_->id() : -1; }
  [[nodiscard]] int v_id() const { return v_ ? v_->id() : -1; }

  const arc& operator=(const arc& rhs) {
    if (this != &rhs) {
      c_ = rhs.c_;
      v_ = rhs.v_;
    }
    return *this;
  }

  friend std::ostream& operator<<(std::ostream& os, const arc& c_x) {
    os << "(" << (c_x.c_ ? c_x.c_->id() : -1) << ", "
       << (c_x.v_ ? c_x.v_->id() : -1) << ")";
    return os;
  }

  // [[nodiscard]] std::weak_ptr<IntVar> v() const { return v_; }
  void v(const std::shared_ptr<IntVar>& val) { v_ = val; }

 private:
  std::shared_ptr<Tabular> c_;
  std::shared_ptr<IntVar> v_;
};

class IntConVal {
 public:
  IntConVal() : a_(-1) {}
  IntConVal(shared_ptr<Tabular> c, shared_ptr<IntVar> v, const int a)
      : c_(c), v_(v), a_(a) {}
  IntConVal(shared_ptr<Tabular> c, IntVal& va)
      : c_(c), v_(va.v()), a_(va.a()) {}
  IntConVal(arc& rc, const int a) : c_(rc.c()), v_(rc.v()), a_(a) {}

  virtual ~IntConVal() {}

  shared_ptr<Tabular> c() const { return c_; }
  void c(shared_ptr<Tabular> c) { c_ = c; }

  shared_ptr<IntVar> v() const { return v_; }
  void v(const shared_ptr<IntVar>& val) { v_ = val; }

  int a() const { return a_; }
  void a(const int val) { a_ = val; }

  arc get_arc() const { return arc(c_, v_); }
  IntVal get_v_value() const { return IntVal(v_, a_); }

  int get_var_index() const { return c_->index(v_); }

  const IntConVal& operator=(const IntConVal& rhs);

  int GetVarIndex() const { return c_->index(v_); };

  friend std::ostream& operator<<(std::ostream& os, const IntConVal& c_val) {
    os << "(" << c_val.c_->id() << ", " << c_val.v_->id() << ", " << c_val.a_
       << ")";
    return os;
  }

 private:
  shared_ptr<Tabular> c_;
  shared_ptr<IntVar> v_;
  int a_;
};

class Network {
 public:
  vector<shared_ptr<IntVar>> vars;
  vector<shared_ptr<Tabular>> tabs;
  unordered_map<shared_ptr<IntVar>, vector<shared_ptr<Tabular>>> subscription;
  unordered_map<shared_ptr<IntVar>, vector<shared_ptr<IntVar>>> neighborhood;
  vector<vector<shared_ptr<IntVar>>> nei_;
  // unordered_map<IntVar*, vector<IntVar*>> neighborhood;
  explicit Network(const HModel& h);
  static void GetFirstValidTuple(IntConVal& c_val, vector<int>& t, int p);
  static void GetNextValidTuple(IntConVal& c_val, vector<int>& t, int p);

  //  由于所有变量的域长度不一定相同 所以这里的c-value值不一定真实存在
  inline int GetIntConValIndex(IntConVal& c_val) const {
    return c_val.c()->id() * max_arity_ * max_dom_size_ +
           c_val.c()->index(c_val.v()) * max_dom_size_ + c_val.a();
  }
  inline int GetIntConValIndex(const int c_id, const int v_id, const int a) {
    const auto tid = tabs[c_id]->index(vars[v_id]);
    return c_id * max_arity_ * max_dom_size_ + tid * max_dom_size_ + a;
  }

  // IntConVal GetIntConVal(int index);
  // int Network::GetIntConValIndex(const int c_id, const int v_id, const int a)
  inline IntConVal GetIntConVal(const int index) {
    const int c_id = index / tabs.size();
    const int v_id = index % tabs.size() / max_dom_size_;
    const int a = index % tabs.size() % max_dom_size_;
    IntConVal c(tabs[c_id], tabs[c_id]->scope[v_id], a);
    return c;
  }

  int top() const { return top_; }
  int tmp() const { return tmp_; }
  // void RestoreUpto(const int level);
  int NewLevel(int src);
  void BackTo(int dest);
  void CopyLevel(int src, int dest);
  void ClearLevel(int p);
  // int NewTmpLevel();
  int max_arity() const { return max_arity_; }
  int max_domain_size() const { return max_dom_size_; }
  int max_bitDom_size() const { return max_bitDom_size_; }
  vector<shared_ptr<IntVar>> get_neighbor(const shared_ptr<IntVar> &v);
  void show(int p);
  ~Network();

 private:
  HModel hm_;
  const int max_arity_;
  const int max_dom_size_;
  const int max_bitDom_size_;
  const int num_vars_;
  const int num_tabs_;
  int top_ = 0;
  int tmp_ = 0;
  vector<shared_ptr<IntVar>> get_scope(const HTab& t) const;
  void get_scope(const HTab &t, vector<shared_ptr<IntVar>> scp);

};

}
