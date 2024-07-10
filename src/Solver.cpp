#include "Solver.h"
namespace cpim {

VarEvt::VarEvt(Network* m) : size_(m->vars.size()), cur_size_(0) {
  vars = m->vars;
}

IntVar* VarEvt::operator[](const int i) const { return vars[i]; }

int VarEvt::size() const { return cur_size_; }

IntVar* VarEvt::at(const int i) const { return vars[i]; }

void VarEvt::push_back(IntVar* v) {
  vars[cur_size_] = v;
  ++cur_size_;
}

void VarEvt::clear() { cur_size_ = 0; }

AssignedStack::AssignedStack(Network* m) : gm_(m) {
  max_size_ = m->vars.size();
  // vals_.resize(m->vars.size());
  vals_.reserve(m->vars.size());
  asnd_.resize(m->vars.size(), false);
}

void AssignedStack::initial(Network* m) {
  gm_ = m;
  max_size_ = m->vars.size();
  // vals_.resize(m->vars.size());
  vals_.reserve(m->vars.size());
  asnd_.resize(m->vars.size(), false);
};

void AssignedStack::push(IntVal& v_a) {
  // vals_[top_] = v_a;
  // asnd_[v_a.vid()] = v_a.op();
  // v_a.v()->assign(v_a.op());
  //++top_;
  vals_.push_back(v_a);
  asnd_[v_a.vid()] = v_a.op();
  v_a.v()->assign(v_a.op(), vals_.size());
};

IntVal AssignedStack::pop() {
  //--top_;
  // asnd_[vals_[top_].vid()] = false;
  // vals_[top_].v()->assign(false);
  // return vals_[top_];
  auto val = vals_.back();
  vals_.pop_back();
  asnd_[val.vid()] = false;
  val.v()->assign(false, vals_.size());
  return val;
}

// IntVal AssignedStack::top() const { return vals_[top_]; };
// int AssignedStack::size() const { return top_; }
// int AssignedStack::capacity() const { return max_size_; }
// bool AssignedStack::full() const { return top_ == max_size_; }
// bool AssignedStack::empty() const { return top_ == 0; }
// IntVal AssignedStack::operator[](const int i) const { return vals_[i]; };
// IntVal AssignedStack::at(const int i) const { return vals_[i]; };
// void AssignedStack::clear() { top_ = 0; };
// bool AssignedStack::assiged(const int v) const { return asnd_[v]; }
// bool AssignedStack::assiged(const IntVar* v) const { return assiged(v->id());
// };
IntVal AssignedStack::top() const { return vals_.back(); }
int AssignedStack::size() const { return vals_.size(); }
int AssignedStack::capacity() const { return vals_.capacity(); }
bool AssignedStack::full() const { return size() == max_size_; }
bool AssignedStack::empty() const { return vals_.empty(); }
IntVal AssignedStack::operator[](const int i) const { return vals_[i]; }
IntVal AssignedStack::at(const int i) const { return vals_[i]; }
void AssignedStack::clear() {
  vals_.clear();
  asnd_.assign(asnd_.size(), false);
}

void AssignedStack::del(const IntVal val) {
  remove(vals_.begin(), vals_.end(), val);
  asnd_[val.vid()] = false;
}

bool AssignedStack::assiged(const int v) const { return asnd_[v]; }
bool AssignedStack::assiged(const IntVar* v) const { return assiged(v->id()); }
vector<IntVal> AssignedStack::vals() const { return vals_; }

vector<int> AssignedStack::solution() {
  if (size() == 0) {
    return vector<int>();
  }
  vector<int> sol(size());
  sol.reserve(max_size_);
  for (int i = 0; i < size(); ++i) {
    sol[vals_[i].vid()] = vals_[i].a();
  }
  return sol;
}

// ostream & operator<<(ostream & os, AssignedStack & I) {
// 	for (int i = 0; i < I.size(); ++i)
// 		os << I[i] << " ";
// 	return os;
// }
//
// ostream & operator<<(ostream & os, AssignedStack * I) {
// 	for (int i = 0; i < I->size(); ++i)
// 		os << I->at(i) << " ";
// 	return os;
// }

void AssignedStack::update_model_assigned() {
  for (auto val : vals_) val.v()->assign(true, vals().size());
}

///////////////////////////////////////////////////////////////////////
DeleteExplanation::DeleteExplanation(Network* m) : m_(m) {
  val_array_.resize(m_->vars.size(),
                    vector<vector<IntVal>>(m_->max_domain_size()));
}

void DeleteExplanation::initial(Network* m) {
  m_ = m;
  val_array_.resize(m_->vars.size(),
                    vector<vector<IntVal>>(m_->max_domain_size()));
}

vector<IntVal>& DeleteExplanation::operator[](const IntVal val) {
  return val_array_[val.vid()][val.a()];
}
///////////////////////////////////////////////////////////////////////
arc_que::arc_que(const int cons_size, const int max_arity)
    : arity_(max_arity),
      m_size_(max_arity * cons_size + 1),
      m_front_(0),
      m_rear_(0) {
  m_data_.resize(m_size_);
  vid_set_.resize(m_size_);
}

void arc_que::MakeQue(const size_t cons_size, const size_t max_arity) {
  arity_ = max_arity;
  m_size_ = max_arity * cons_size + 1;
  m_front_ = 0;
  m_rear_ = 0;

  m_data_.resize(m_size_);
  vid_set_.resize(m_size_);
}

bool arc_que::empty() const { return m_front_ == m_rear_; }

bool arc_que::full() const { return m_front_ == (m_rear_ + 1) % m_size_; }

bool arc_que::push(arc& ele) {
  if (full()) throw std::bad_exception();

  if (have(ele)) return false;

  m_data_[m_rear_] = ele;
  m_rear_ = (m_rear_ + 1) % m_size_;
  have(ele) = 1;

  return true;
}

arc arc_que::pop() {
  if (empty()) throw std::bad_exception();

  arc tmp = m_data_[m_front_];
  m_front_ = (m_front_ + 1) % m_size_;
  have(tmp) = 0;

  return tmp;
}

/////////////////////////////////////////////////////////////////////////
bool var_que::empty() const { return m_front_ == m_rear_; }

void var_que::initial(const int size) {
  max_size_ = size + 1;
  m_data_.resize(max_size_, nullptr);
  vid_set_.resize(max_size_, 0);
  m_front_ = 0;
  m_rear_ = 0;
  size_ = 0;
}

bool var_que::full() const { return m_front_ == (m_rear_ + 1) % max_size_; }

void var_que::push(IntVar* v) {
  if (vid_set_[v->id()]) return;
  m_data_[m_rear_] = v;
  m_rear_ = (m_rear_ + 1) % max_size_;
  vid_set_[v->id()] = true;
  ++size_;
}

IntVar* var_que::pop() {
  IntVar* tmp = m_data_[m_front_];
  m_front_ = (m_front_ + 1) % max_size_;
  vid_set_[tmp->id()] = false;
  --size_;
  return tmp;
}

void var_que::clear() {
  m_front_ = 0;
  m_rear_ = 0;
  size_ = 0;
  vid_set_.assign(vid_set_.size(), false);
}

int var_que::max_size() const { return max_size_; }

int var_que::size() const { return size_; }

void var_pri_que::initial(const int size) { vid_set_.resize(size, false); }

void var_pri_que::push(IntVar* v) {
  if (!vid_set_[v->id()])
    q_.push(v);
  else
    vid_set_[v->id()] = true;
}

IntVar* var_pri_que::pop() {
  IntVar* v = q_.top();
  vid_set_[v->id()] = false;
  q_.pop();
  return v;
}

void var_pri_que::clear() {
  q_ = priority_queue<IntVar*, vector<IntVar*>, cmp>();
  vid_set_.assign(vid_set_.size(), false);
}

int var_pri_que::size() const { return q_.size(); }

bool var_pri_que::empty() const { return q_.empty(); }

///////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////
// bool var_priority_queue::have(IntVar* v) {
//	return vid_set_[v->id()];
// }
//
// bool var_priority_queue::empty() const {
//	return m_data_.empty();
// }
//
// void var_priority_queue::initial(const int size) {
//	vid_set_.resize(size, 0);
//
// }
//
// void var_priority_queue::push(IntVar* v) {
//	m_data_.push(v);
//	vid_set_[v->id()] = 1;
// }
/////////////////////////////

bool vars_pair_cir_que::empty() const { return m_front_ == m_rear_; }

void vars_pair_cir_que::initial(const int size) {
  max_size_ = size * size + 1;
  m_data_.resize(max_size_);
  id_set_.resize(size, vector<int>(size, false));
  m_front_ = 0;
  m_rear_ = 0;
  size_ = 0;
  num_vars_ = size;
}

bool vars_pair_cir_que::full() const {
  return m_front_ == (m_rear_ + 1) % max_size_;
}

void vars_pair_cir_que::push(const variable_pair vv) {
  if (id_set_[vv.x->id()][vv.y->id()]) return;
  m_data_[m_rear_] = vv;
  m_rear_ = (m_rear_ + 1) % max_size_;
  id_set_[vv.x->id()][vv.y->id()] = true;
  ++size_;
}

variable_pair vars_pair_cir_que::pop() {
  const variable_pair tmp = m_data_[m_front_];
  m_front_ = (m_front_ + 1) % max_size_;
  id_set_[tmp.x->id()][tmp.y->id()] = false;
  --size_;
  return tmp;
}

void vars_pair_cir_que::clear() {
  m_front_ = 0;
  m_rear_ = 0;
  size_ = 0;
  for (auto& v : id_set_) {
    v.assign(num_vars_, false);
  }
}

int vars_pair_cir_que::max_size() const { return max_size_; }

int vars_pair_cir_que::size() const {
  return size_;
}  ///////////////////////////////
}  // namespace cpim
