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
      if (v->have(i)) {
        // Phase 1.1: 使用 Trail 替代 CopyLevel
        n_->trail()->NewLevel();
        const int test_level = n_->trail()->CurrentLevel();

        v->ReduceTo(i);
        v->assign(true);
        x_evt.push_back(v);
        const auto res = ac_->enforce(x_evt, test_level).state;
        if (!res) {
          // cout << "test:" << v->id() << "," << i << endl;
          // cout << "no pass" << endl;
          v->RemoveValue(i);
        }
        x_evt.clear();

        // 恢复到测试前
        n_->trail()->BacktrackTo(test_level - 1);
      }
      if (v->faild()) {
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
        if (x->have(a)) {
          // cout << "(" << x->id() << "," << a << ")" << endl;
          // Phase 1.1: 使用 Trail 替代 CopyLevel
          n_->trail()->NewLevel();
          const int test_level = n_->trail()->CurrentLevel();

          x->ReduceTo(a);
          x->assign(true);
          x_evt_.push_back(x);
          result = ac_->enforce(x_evt_, test_level).state;
          x_evt_.clear();
          x->assign(false);

          // 恢复到测试前
          n_->trail()->BacktrackTo(test_level - 1);

          if (!result) {
            // cout << "delete: (" << x->id() << "," << a << ")" << endl;
            ++del_;

            x->RemoveValue(a);
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
