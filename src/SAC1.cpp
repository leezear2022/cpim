#include "Solver.h"

namespace cpim {
SAC1::SAC1(Network* n, const ACAlgorithm a) : n_(n), ac_algzm_(a) {
  switch (a) {
    case AC_3:
      ac_ = new AC3(n_);
      break;
    case AC_3bit:
      ac_ = new AC3bit(n_);
      break;
    case A_FC:
      ac_ = new FC(n_);
      break;
    case A_FC_bit:
      ac_ = new FCbit(n_);
      break;
      // case AC_3rm:
      //	ac_ = new AC3rm(nt_);
    default:
      break;
  }

  x_evt_ = n_->vars;
}
SAC1::~SAC1() {
  // TODO Auto-generated destructor stub
  delete ac_;
}

bool SAC1::one_pass() const {
  vector<IntVar*> x_evt;
  for (auto v : n_->vars) {
    for (auto i : v->values()) {
      if (v->have(i, 0)) {
        n_->CopyLevel(0, 1);
        v->ReduceTo(i, 1);
        v->assign(true, 1);
        x_evt.push_back(v);
        const auto res = ac_->enforce(x_evt, 1).state;
        if (!res) {
          // cout << "test:" << v->id() << "," << i << endl;
          // cout << "no pass" << endl;
          v->RemoveValue(i, 0);
        }
        x_evt.clear();
        n_->ClearLevel(1);
      }
      if (v->faild(0)) {
        return false;
      }
    }
  }
  return true;
}

bool SAC1::enforce(vector<IntVar*> x_evt, const int level) {
  ConsistencyState cs = ac_->enforce(n_->vars, level);
  bool result = cs.state;
  // cout << cs.num_delete << endl;
  del_ += cs.num_delete;
  x_evt_.clear();
  auto modified = false;

  if (!result) return false;

  do {
    modified = false;
    for (auto x : n_->vars) {
      for (auto a : x->values()) {
        if (x->have(a, level)) {
          // cout << "(" << x->id() << "," << a << ")" << endl;
          n_->CopyLevel(level, n_->tmp());
          x->ReduceTo(a, n_->tmp());
          x->assign(true, n_->tmp());
          x_evt_.push_back(x);
          result = ac_->enforce(x_evt_, n_->tmp()).state;
          x_evt_.clear();
          x->assign(false, level);
          n_->ClearLevel(n_->tmp());
          if (!result) {
            // cout << "delete: (" << x->id() << "," << a << ")" << endl;
            ++del_;

            x->RemoveValue(a, level);
            x_evt_.push_back(x);
            cs = ac_->enforce(x_evt_, level);
            result = cs.state;
            del_ += cs.num_delete;
            x_evt_.clear();

            if (!result) return false;

            modified = true;
          }
        }
      }
    }
  } while (modified);

  // cout << "delete:" << del_ << endl;
  return true;
}

}  // namespace cpim
