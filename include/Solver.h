#pragma once
#include <functional>
#include <queue>
#include <set>
// #include <limits>
#include <unordered_set>

#include "Network.h"
#include "Timer.h"

namespace cpim {

enum ACAlgorithm {
  CA_AC1,
  CA_AC2,
  AC_3,
  AC_4,
  AC_6,
  AC_7,
  AC_2001,
  AC_3bit,
  AC_3rm,
  STR_1,
  STR_2,
  STR_3,
  A_FC,
  A_FC_bit,
  A_NSAC,
  CA_LMRPC_BIT,
  CA_RPC3
};
enum Consistency {
  C_AC3,
  C_AC4,
  C_AC2001,
  C_AC3bit,
  C_AC3rm,
  C_STR1,
  C_STRC2,
  C_STR3,
  C_FC
};

enum LookBack {
  LB_SBT,
  LB_IBT,
  LB_DBT

};

enum LookAhead { LA_BC, LA_FC, LA_MAC };

enum VarHeu { DOM, DOM_WDEG };

namespace Heuristic {
enum Var {
  VRH_LEX,
  VRH_DOM_MIN,
  VRH_VWDEG,
  VRH_DEG_MIN,
  VRH_DOM_DEG_MIN,
  VRH_DOM_DDEG_MIN,
  VRH_DOM_WDEG_MIN
};

enum Val { VLH_MIN, VLH_MIN_DOM, VLH_MIN_INC, VLH_MAX_INC, VLH_VWDEG };

enum DecisionScheme { DS_BI, DS_NB };
};  // namespace Heuristic

struct SearchStatistics {
  uint64_t num_sol = 0;
  uint64_t num_positive = 0;
  uint64_t num_negative = 0;
  uint64_t nodes = 0;
  uint64_t build_time = 0;
  uint64_t solve_time = 0;
  uint64_t total_time = 0;
  bool time_out = false;
  bool pass = false;
  // int n_deep = 0;
};

struct ConsistencyState {
  // 是否执行失败
  bool state;
  bool seek_support_fail = true;
  bool revise_fail = true;
  // 删除变量值数
  int num_delete = 0;
  // 造成失败的约束
  Tabular* tab = nullptr;
  // 造成失败的变量
  IntVar* var = nullptr;
  IntVal v_a_fail;
  int level = 0;
};

struct SearchError {
  bool seek_support_fail = true;
  bool revise_fail = true;
  int num_delete = 0;
  Tabular* tab = nullptr;
  IntVar* v = nullptr;
  IntVal v_a_fail;
  int level = 0;
};
struct SearchScheme {
  Heuristic::Var vrh;
  Heuristic::Var vlh;
  Heuristic::DecisionScheme ds;

  string vrh_str;
  string vlh_str;
  string ds_str;
};

class VarEvt {
 public:
  VarEvt(Network* m);
  virtual ~VarEvt(){};

  IntVar* operator[](const int i) const;
  void push_back(IntVar* v);
  void clear();
  int size() const;
  IntVar* at(const int i) const;

 private:
  // IntVar** vars_;
  vector<IntVar*> vars;
  int size_;
  int cur_size_;
};

class arc_que {
 public:
  int& have(const arc a) {
    return vid_set_[a.c_id() * arity_ + a.c()->index(a.v())];
  };
  arc_que() {}
  arc_que(const int cons_size, const int max_arity);
  virtual ~arc_que(){};

  void MakeQue(const size_t cons_size, const size_t max_arity);
  // void DeleteQue();
  bool empty() const;
  bool full() const;
  bool push(arc& ele);
  arc pop();

 private:
  vector<arc> m_data_;
  vector<int> vid_set_;
  size_t arity_;
  size_t m_size_;
  int m_front_;
  int m_rear_;
};
struct cmp {
  bool operator()(const IntVar* a, const IntVar* b) const {
    return a->top_size > b->top_size;
  }
};
class var_pri_que {
 public:
  var_pri_que(){};
  ~var_pri_que(){};
  void initial(const int size);
  void push(IntVar* v);
  IntVar* pop();
  void clear();
  int size() const;
  bool empty() const;

 protected:
  priority_queue<IntVar*, vector<IntVar*>, cmp> q_;
  vector<bool> vid_set_;
};

class AssignedStack {
 public:
  AssignedStack() {}
  void update_model_assigned();
  ;
  AssignedStack(Network* m);
  void initial(Network* m);
  ~AssignedStack(){};
  void push(IntVal& v_a);
  IntVal pop();
  IntVal top() const;
  int size() const;
  int capacity() const;
  bool full() const;
  bool empty() const;
  IntVal operator[](const int i) const;
  IntVal at(const int i) const;
  void clear();
  void del(const IntVal val);
  bool assiged(const int v) const;
  bool assiged(const IntVar* v) const;
  vector<IntVal> vals() const;
  friend ostream& operator<<(ostream& os, AssignedStack& I);
  friend ostream& operator<<(ostream& os, AssignedStack* I);
  vector<int> solution();

 protected:
  Network* gm_;
  vector<IntVal> vals_;
  vector<bool> asnd_;
  // int top_ = 0;
  int max_size_;
};

class DeleteExplanation {
 public:
  DeleteExplanation(){};
  DeleteExplanation(Network* m);
  ~DeleteExplanation() {}
  void initial(Network* m);
  vector<IntVal>& operator[](const IntVal val);

 protected:
  vector<vector<vector<IntVal>>> val_array_;
  Network* m_;
};

class var_que {
 public:
  // int& have(const IntVar* v) { return vid_set_[v->id()]; };
  var_que() {}
  // var_que(Network* n);
  virtual ~var_que(){};

  // void initial(Network* n);
  // void DeleteQue();
  // bool have(IntVar* v);
  bool empty() const;
  void initial(const int size);
  bool full() const;
  void push(IntVar* v);
  IntVar* pop();
  void clear();
  int max_size() const;
  int size() const;

 private:
  vector<IntVar*> m_data_;
  vector<bool> vid_set_;
  size_t max_size_;
  int m_front_;
  int m_rear_;
  int size_;
};

struct variable_pair {
  IntVar* x;
  IntVar* y;
};

class vars_pair_cir_que {
 public:
  vars_pair_cir_que(){};
  virtual ~vars_pair_cir_que(){};

  bool empty() const;
  void initial(const int size);
  bool full() const;
  void push(variable_pair vv);
  variable_pair pop();
  void clear();
  int max_size() const;
  int size() const;

 private:
  vector<variable_pair> m_data_;
  vector<vector<int>> id_set_;
  size_t max_size_;
  int m_front_;
  int m_rear_;
  int size_;
  int num_vars_;
};

// class var_priority_queue {
// public:
//	int& have(const IntVar* v) { return vid_set_[v->id()]; };
//	var_priority_queue(){};
//	//var_que(Network* n);
//	virtual ~var_priority_queue() {};
//
//	//void initial(Network* n);
//	//void DeleteQue();
//	bool have(IntVar* v);
//	bool empty() const;
//	void initial(const int size);
//	//bool full() const;
//	void push(IntVar* v);
//	IntVar* pop();
//	void clear();
//	int max_size() const;
//	int size() const;
//
// private:
//	priority_queue<IntVar*> m_data_;
//	vector<int> vid_set_;
// };

class AC {
 public:
  AC(Network* m);
  // AC(Network *m, const LookAhead look_ahead, const LookBack look_back);
  virtual ~AC(){};
  // virtual bool enforce(VarEvt* x_evt, const int level = 0) = 0;
  virtual ConsistencyState enforce(vector<IntVar*>& x_evt, int level) = 0;
  // virtual ConsistencyState enforce_arc(vector<IntVar*>& x_evt, const int
  // level) = 0;
  ConsistencyState cs;
  // void q_insert(IntVar* v);
  int del() const { return delete_; }

  // virtual bool revise(const arc& c_x, int level) = 0;
  // virtual bool seek_support(const IntConVal& c_val, int level) = 0;

 protected:
  // vector<IntVar*> q_;
  vector<int> tmp_tuple_;
  Network* m_;
  int delete_ = 0;
  int level_ = 0;
};

class AC3 : public AC {
 public:
  AC3(Network* m);
  virtual ~AC3(){};
  // bool enforce(VarEvt* x_evt, const int level = 0) override;
  ConsistencyState enforce(vector<IntVar*>& x_evt, int level) override;
  // ConsistencyState enforce_arc(vector<IntVar*>& x_evt, const int level = 0)
  // override; SearchError se;
  virtual bool revise(const arc& c_x, int level);
  virtual bool seek_support(const IntConVal& c_val, int level);

 protected:
  // pro_que<T> q;
  // arc_que Q;
  var_que q_;
  vector<uint64_t> stamp_var_;
  vector<uint64_t> stamp_tab_;
  uint64_t t_ = 0;

  LookAhead la_;
  LookBack lb_;
  int level_ = 0;

  void insert(IntVar* v);
  // void inital_q_arc();
  // private:
  //	void inital_Q_arc();
};

class FC : public AC3 {
 public:
  FC(Network* n);
  ConsistencyState enforce(vector<IntVar*>& x_evt, int level) override;

 private:
  // int max_bitDom_size_;
  // vector<vector<bitset<BITSIZE>>> bitSup_;
};

class AC3bit : public AC3 {
 public:
  AC3bit(Network* m);
  virtual ~AC3bit(){};

  virtual bool seek_support(const IntConVal& c_val, int p) override;

 protected:
  int max_bitDom_size_;
  vector<vector<bitset<BITSIZE>>> bitSup_;
};

class FCbit : public AC3bit {
 public:
  FCbit(Network* n);
  ConsistencyState enforce(vector<IntVar*>& x_evt, int level);

 private:
  // int max_bitDom_size_;
  // vector<vector<bitset<BITSIZE>>> bitSup_;
};

// class residues {
// public:
//	residues(Network *nt);
//	~residues();
//
//	IntTuple& operator[](const IntConVal &c_val) const {
//		return *data_[c_val.c()->id()][c_val.GetVarIndex()][c_val.a()];
//	}
//
//	IntTuple& at(const IntConVal &c_val);
//
// private:
//	Network *nt_;
//	IntTuple ****data_;
//	vector<vector<vector<vector<int>>>> data_
// };
//
class AC3rm : public AC3 {
 public:
  AC3rm(Network* nt);
  virtual ~AC3rm(){};

  bool seek_support(const IntConVal& c_val, int p) override;

 protected:
  vector<vector<int>> res_;
};

class SAC1 {
 public:
  SAC1(Network* n, ACAlgorithm a);
  virtual bool enforce(vector<IntVar*> x_evt, const int level);
  virtual ~SAC1();
  bool one_pass() const;
  int del() const { return del_; }
  AC* ac_;

 protected:
  int del_ = 0;
  int level_;
  Network* n_;
  ACAlgorithm ac_algzm_;
  vector<IntVar*> x_evt_;
};

class Qsac {
 public:
  Qsac(){};
  Qsac(Network* n);
  ~Qsac(){};
  void create(Network* n);
  void initial(const int p);
  void push(IntVal val);
  IntVal pop(const Heuristic::Var varh, const Heuristic::Val valh, const int p);
  bool empty() const;
  int size(const IntVar* v) const;
  void update(const int p);
  // bool vars_assigned();
  // void reset();
  void show();
  bool all_assigned(AssignedStack& I) const;
  bool have(IntVal v);
  bool have(IntVar* var, int a);
  // void delete_vals();
 protected:
  int head(const IntVar* v) const;
  IntVal select_IntVal(const Heuristic::Var varh, const Heuristic::Val valh,
                       const int p);
  IntVar* select_var(const Heuristic::Var varh, const int p) const;
  int select_val(const Heuristic::Val valh, IntVar* v, const int p);
  vector<bitSetVector> bitDoms_;
  Network* n_;
  VarHeu h_;
  int num_bit_vars_;
  // vector<bitset<BITSIZE>> vars_assigned_;
  // vector<bitset<BITSIZE>> vars_assigned_old_;
  // vector<bitset<BITSIZE>> tmp_empty_;
};

class SAC3 : public SAC1 {
 public:
  SAC3(Network* n, ACAlgorithm a, const Heuristic::Var varh,
       const Heuristic::Val valh);
  bool enforce(vector<IntVar*> x_evt, const int level) override;

 protected:
  IntVal BuildBranch();
  Qsac q_;
  Heuristic::Var varh_;
  Heuristic::Val valh_;
  AssignedStack I_;
};

// class RNSQ :public AC3bit {
// public:
//	RNSQ(Network *m);
//	ConsistencyState conditionFC(IntVar* v, const int level = 0);
//	ConsistencyState neiborAC(vector<IntVar*>& x_evt, IntVar* x, const int
// level = 0); 	ConsistencyState enforce(vector<IntVar*>& x_evt, const int level
// = 0) override; protected: 	unordered_map<IntVar*, bitSetVector> neibor_;
// bool is_neibor(IntVar* x, IntVar* v); 	var_que q_nei_; 	var_que
// q_var_; 	void insert_(var_que& q, IntVar* v); 	bool
// in_neibor_exp(Tabular* t, IntVar* x); 	bool in_neibor(Tabular* t,
// IntVar* x);
//	bool has_sigleton_domain_neibor(IntVar* x) const;
// };

class NSAC : public AC3bit {
 public:
  NSAC(Network* m);
  ConsistencyState enforce(vector<IntVar*>& x_evt,
                           const int level = 0) override;
  int revise_NSAC(IntVar* v, IntVar* x, const int level);
  bool full_NSAC(IntVar* v, IntVar* x, const int level);

 protected:
  unordered_map<IntVar*, bitSetVector> neibor_;
  bool is_neibor(IntVar* x, IntVar* v);
  var_que q_nei_;
  var_que q_var_;
  // void insert_(var_que& q, IntVar* v);
  bool deletion = false;
};

class lMaxRPC : public AC {
 public:
  lMaxRPC(Network* m);
  ConsistencyState enforce(vector<IntVar*>& x_evt,
                           const int level = 0) override;
  bool have_pc_support(IntVar& i, const int a, IntVar& j, const int p);
  bool have_pc_wit(IntVar& i, const int a, const IntVar& j, int b, IntVar& k,
                   const int p);
  int next_support_bit(IntVar& i, const int a, IntVar& j, const int v,
                       const int p);

  bool have_no_PC_support(IntVar* x, const int a, IntVar* y);
  bool have_PC_wit(IntVar* x, const int a, IntVar* y, const int b, IntVar* z);

 protected:
  var_que q_var_;
  // var_pri_que q_var_;
  // unordered_map<IntVar*, bitSetVector> neibor_;
  vector<vector<Tabular*>> nei_;
  // vector<vector<unordered_set<IntVar*>>> pc_nei_;
  vector<vector<vector<IntVar*>>> pc_nei_;
  vector<int> last_pc;
  vector<int> last_ac;
  vector<vector<bitset<BITSIZE>>> bitSup_;
  vector<vector<IntVar*>> neighborhood;
};

class RPC3 : public AC {
 public:
  RPC3(Network* m);
  ~RPC3(){};

  ConsistencyState enforce(vector<IntVar*>& x_evt,
                           const int level = 0) override;
  bool is_consistent(const IntVar& x, const int a, const IntVar& y,
                     const int b);
  bool is_consistent(const IntVar& c, const IntVar& x, const int a,
                     const IntVar& y, const int b);
  int find_two_support(const IntVar& i, const int a, const IntVar& y,
                       const int r, const int p);

 protected:
  int num_vars;
  int max_dom_size;
  // u64 * * bitSup_;
  // vars_heap q_nei_;
  // vector<vector<QTab*>> N;
  // vector<int> var_mark_;
  vars_pair_cir_que con_que_;
  vector<vector<vector<int>>> r_1_, r_2_;
  // vector<vector<vector<int>>> rel_;
  vector<vector<vector<vector<int>>>> rel_;
  vector<vector<vector<IntVar*>>> common_neibor_;
  vector<vector<vector<Tabular*>>> neibor_matrix;
  vector<vector<IntVar*>> neighborhood;
};

// Forward declaration for Phase 2.1
class PropagationEngine;

class MAC {
 public:
  MAC(Network* n, ACAlgorithm ac_algzm, const Heuristic::Var varh,
      const Heuristic::Val valh, bool use_propagator_framework = false);
  SearchStatistics enforce(const int time_limits);
  // SearchStatistics enforce_fc(const int time_limits);
  virtual ~MAC();
  int sol_count() const { return sol_count_; }
  void sol_count(const int val) { sol_count_ = val; }
  bool solution_check() const;
  void get_solution();
  AssignedStack I;
  vector<int> solution;
  string sol_str;
  bool one_pass_sac() const;

 private:
  int sol_count_ = 0;
  Network* n_;
  AC* ac_;
  vector<IntVar*> x_evt_;
  // VarEvt* x_evt_;
  ACAlgorithm ac_algzm_;
  IntVal select_v_value(const int p) const;
  int select_val(const IntVar* v, const int p) const;
  IntVar* select_var(const int p) const;
  bool consistent_;
  bool finished_ = false;
  SearchStatistics statistics_;
  Heuristic::Var varh_;
  Heuristic::Val valh_;

  // Phase 2.1: Propagator 框架支持
  bool use_propagator_framework_;
  PropagationEngine* propagation_engine_ = nullptr;

  // Phase 2.1: 初始化 PropagationEngine（注册 TableConstraintPropagators）
  void InitializePropagationEngine();
};

// class Search {
// public:
//	Search(Network *n, const LookAhead look_ahead, const LookBack look_back,
// const Consistency consistency);
//	//SearchStatistics enforce(const int time_limits);
//	virtual ~Search();
//	int sol_count() const { return sol_count_; }
//	void sol_count(const int val) { sol_count_ = val; }
//	virtual vector<IntVal> HandleEmptyDomain(IntVar* v);
//	//virtual vector<IntVal> CheckConsistencyAfterAssignment(IntVar *v);
//	//virtual vector<IntVal> CheckConsistencyAfterRefutati(IntVar *v);
//	virtual void UndoAssignment(IntVal v_a);
//
// private:
//	int sol_count_ = 0;
//	Network *n_;
//	AC* ac_;
//	vector<IntVar*> x_evt_;
//	//VarEvt* x_evt_;
//	Consistency c_type_;
//	vector<IntVal> nogood;
//	AssignedStack I;
//	IntVal select_v_value() const;
//	bool consistent_;
//	bool finished_ = false;
//	SearchStatistics statistics_;
//	LookAhead la_;
//	LookBack lb_;
//	DeleteExplanation expl;
//};

}
