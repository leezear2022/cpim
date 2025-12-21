#include <cfloat>

#include "Solver.h"
namespace cpim {
Qsac::Qsac(Network* n) : n_(n) {
  bitDoms_.resize(n_->vars.size(), bitSetVector(n_->max_bitDom_size()));
  for (int i = 0; i < n_->vars.size(); ++i)
    bitDoms_[i].assign(n_->vars[i]->bitDom().begin(),
                       n_->vars[i]->bitDom().end());
}

void Qsac::create(Network* n) {
  n_ = n;
  num_bit_vars_ = ceil(static_cast<float>(n_->vars.size()) / BITSIZE);
  // vars_assigned_.resize(num_bit_vars_);

  // for (int i = 0; i < n_->vars.size(); ++i) {
  //	auto idx = GetBitIdx(i);
  //	vars_assigned_[get<0>(idx)].set(get<1>(idx));
  // }
  // vars_assigned_old_ = vars_assigned_;
  // tmp_empty_ = vars_assigned_;
  bitDoms_.resize(n_->vars.size(), bitSetVector(n_->max_bitDom_size()));
  for (int i = 0; i < n_->vars.size(); ++i)
    bitDoms_[i].assign(n_->vars[i]->bitDom().begin(),
                       n_->vars[i]->bitDom().end());
}

void Qsac::initial(const int p) {
  bitDoms_.resize(n_->vars.size(), bitSetVector(n_->max_bitDom_size()));
  for (int i = 0; i < n_->vars.size(); ++i)
    bitDoms_[i].assign(n_->vars[i]->bitDom().begin(),
                       n_->vars[i]->bitDom().end());
}

void Qsac::push(IntVal val) {
  auto a = GetBitIdx(val.a_);
  bitDoms_[val.vid()][get<0>(a)].set(get<1>(a));
  // auto b = GetBitIdx(val.vid());
  // vars_assigned_[get<0>(b)].reset(get<1>(b));
}

IntVal Qsac::pop(const Heuristic::Var varh, const Heuristic::Val valh,
                 const int p) {
  IntVal val = select_IntVal(varh, valh, p);
  auto a = GetBitIdx(val.a_);
  bitDoms_[val.vid()][get<0>(a)].reset(get<1>(a));
  // auto b = GetBitIdx(val.vid());
  // vars_assigned_[get<0>(b)].set(get<1>(b));
  return val;
}

bool Qsac::empty() const {
  int s = 0;
  for (auto& v : n_->vars) s += size(v);
  return s;
}

int Qsac::size(const IntVar* v) const {
  int size = 0;
  for (auto& a : bitDoms_[v->id()]) size += a.count();
  return size;
}

void Qsac::update(const int p) {
  for (int i = 0; i < n_->vars.size(); ++i)
    for (int j = 0; j < n_->max_bitDom_size(); ++j)
      bitDoms_[i][j] &= n_->vars[i]->bitDom()[j];
}

// std::ostream& operator<<(ostream& os, const IntVal& v_val) {
//   const string s = (v_val.aop_) ? " = " : " != ";
//   os << "(" << v_val.vid() << s << v_val.a_ << ")";
//   return os;
// }

void Qsac::show() {
  for (auto v : n_->vars)
    for (auto a : v->values())
      if (have(v, a)) cout << IntVal(v, a) << " ";
  cout << endl;
}

//
// bool Qsac::vars_assigned() {
//	for (int i = 0; i < n_->vars.size(); ++i) {
//		IntVar* v = n_->vars[i];
//		auto a = GetBitIdx(v->id());
//		if (vars_assigned_[get<0>(a)].test(get<1>(a)) && size(v) != 0)
//			return false;
//	}
//	return true;
//}

// void Qsac::reset() {
//	for (auto&a : vars_assigned_)
//		a.reset();
// }

bool Qsac::all_assigned(AssignedStack& I) const {
  for (int i = 0; i < n_->vars.size(); ++i) {
    const IntVar* v = n_->vars[i];
    if (size(v) != 0 && !I.assiged(v)) {
      return false;
    }
  }

  return true;
}

bool Qsac::have(IntVal val) {
  auto a = GetBitIdx(val.a_);
  return bitDoms_[val.vid()][get<0>(a)].test(get<1>(a));
}
bool Qsac::have(IntVar* var, const int a) {
  auto idx = GetBitIdx(a);
  return bitDoms_[var->id()][get<0>(idx)].test(get<1>(idx));
}
int Qsac::head(const IntVar* v) const {
  for (int i = 0; i < n_->max_bitDom_size(); ++i)
    if (bitDoms_[v->id()][i].any())
      for (int j = 0; j < BITSIZE; ++j)
        if (bitDoms_[v->id()][i].test(j)) return GetValue(i, j);
  return Limits::INDEX_OVERFLOW;
}

IntVal Qsac::select_IntVal(const Heuristic::Var varh, const Heuristic::Val valh,
                           const int p) {
  IntVar* v = select_var(varh, p);
  const int a = select_val(valh, v, p);
  return IntVal(v, a);
}

int Qsac::select_val(const Heuristic::Val valh, IntVar* v, const int p) {
  switch (valh) {
    case Heuristic::VLH_MIN: {
      for (auto i : v->values())
        if (have(v, i) && v->have(i)) return i;
    } break;
    case Heuristic::VLH_MIN_DOM:
      break;
    case Heuristic::VLH_MIN_INC:
      break;
    case Heuristic::VLH_MAX_INC:
      break;
    case Heuristic::VLH_VWDEG:
      break;
    default:;
  }
  return INT_MIN;
}

IntVar* Qsac::select_var(const Heuristic::Var varh, const int p) const {
  IntVar* v = nullptr;
  auto min_size = DBL_MAX;
  auto max_size = DBL_MIN;
  switch (varh) {
      // case DOM: {
      //	double min_size = DBL_MAX;
      //	for (auto v : n_->vars)
      //		if (!v->assigned())
      //			if (v->size() < min_size&&size(v) > 0) {
      //				min_size = v->size();
      //				val.v(v);
      //			}
      //	val.a(head(val.v()));
      // }
      //		  break;
      // case DOM_WDEG: {
      //	double min_size = DBL_MAX;
      //	for (auto x : n_->vars) {
      //		if (!x->assigned()) {
      //			float x_w = 0.0;
      //			float x_dw;
      //			for (auto c : n_->subscription[x]) {
      //				int cnt = 0;
      //				for (auto y : c->scope)
      //					if (!y->assigned())
      //						++cnt;
      //				if (cnt > 1)
      //					x_w += c->weight;
      //			}

      //			if (x->size() == 1 || x_w == 0)
      //				x_dw = -1;
      //			else
      //				x_dw = x->size() / x_w;
      //			if (x_dw < min_size&&size(x) > 0) {
      //				min_size = x_dw;
      //				val.v(x);
      //			}
      //		}
      //	}
      //	val.a(head(val.v()));
      //}
      //			   break;
      // default:;
    case Heuristic::VRH_LEX:
      break;
    case Heuristic::VRH_DOM_MIN: {
      // for (auto x : n_->vars)
      //	if (!x->assigned())
      //		if (x->size() < min_size&&size(x) > 0) {
      //			min_size = x->size();
      //			v = x;
      //		}

      for (auto x : n_->vars)
        if (!x->assigned())
          if (x->size() < max_size && size(x) > 0) {
            max_size = x->size();
            v = x;
          }
    }
      return v;
    case Heuristic::VRH_VWDEG:
      break;
    case Heuristic::VRH_DOM_DEG_MIN:
      break;
    case Heuristic::VRH_DOM_WDEG_MIN: /* {
             cout << "122" << endl;
             for (auto x : n_->vars) {
                     if (!x->assigned()) {
                             double x_w = 0.0;
                             double x_dw = 0.0;

                             for (auto c : n_->subscription[x]) {
                                     int cnt = 0;
                                     for (auto y : c->scope)
                                             if (!y->assigned())
                                                     ++cnt;
                                     if (cnt > 1)
                                             x_w += c->weight;
                             }

                             if (x->size() == 1 || x_w == 0)
                                     x_dw = -1;
                             else
                                     x_dw = x->size() / x_w;

                             if (x_dw < min_size) {
                                     min_size = x_dw;
                                     v = x;
                             }
                     }
             }
     } return v;*/
      break;
    default:;
  }
  return v;
}

SAC3::SAC3(Network* n, const ACAlgorithm a, const Heuristic::Var varh,
           const Heuristic::Val valh)
    : SAC1(n, a), varh_(varh), valh_(valh) {
  q_.create(n_);
  I_.initial(n_);
}

bool SAC3::enforce(vector<IntVar*> x_evt, const int level) {
  level_ = level;
  ConsistencyState cs = ac_->enforce(n_->vars, level);
  del_ += cs.num_delete;
  bool result = cs.state;
  auto modified = false;
  x_evt_.clear();
  if (!result) return false;

  do {
    modified = false;
    q_.initial(level_);
    while (q_.empty()) {
      // q_.show();
      IntVal val = BuildBranch();
      // 此处assigned 要处下理下
      if (val != Nil_Val) {
        val.v()->RemoveValue(val.a());
        del_++;
        x_evt_.push_back(val.v());
        cs = ac_->enforce(x_evt_, level_);
        result = cs.state;
        del_ += cs.num_delete;
        x_evt_.clear();

        if (!result) return false;

        q_.update(level_);
        modified = true;
      }
    }
  } while (modified);

  return true;
}

IntVal SAC3::BuildBranch() {
  int length = 0;
  IntVal val = Nil_Val;
  bool result;
  // q_.reset();
  // Phase 1.1: 使用 Trail 替代 CopyLevel
  n_->trail()->NewLevel();
  const int test_level = n_->trail()->CurrentLevel();

  do {
    val = q_.pop(varh_, valh_, test_level);
    cout << val << endl;
    val.v()->ReduceTo(val.a());
    val.v()->assign(true);
    I_.push(val);
    x_evt_.push_back(val.v());
    result = ac_->enforce(x_evt_, test_level).state;
    x_evt_.clear();

    if (result)
      ++length;
    else if (length > 0)
      q_.push(val);
  } while (!(result == false || q_.all_assigned(I_)));
  I_.clear();

  // 恢复到测试前
  n_->trail()->BacktrackTo(test_level - 1);

  if (length == 0)
    return val;
  else
    return Nil_Val;
}
}
