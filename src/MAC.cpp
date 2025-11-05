#include <cfloat>
#include <sstream>

#include "Solver.h"
using namespace std;
namespace cpim {

MAC::MAC(Network* n, const ACAlgorithm ac_algzm, const Heuristic::Var varh,
         const Heuristic::Val valh)
    : n_(n), ac_algzm_(ac_algzm), varh_(varh), valh_(valh) {
  x_evt_.reserve(n_->vars.size());
  I.initial(n_);

  switch (ac_algzm) {
    case AC_3:
      ac_ = new AC3(n_);
      break;
    case AC_3bit:
      ac_ = new AC3bit(n_);
      break;
    case AC_3rm:
      ac_ = new AC3rm(n_);
      break;
    case A_FC:
      ac_ = new FC(n_);
      break;
    case A_FC_bit:
      ac_ = new FCbit(n_);
      break;
    case A_NSAC:
      ac_ = new NSAC(n_);
      break;
    case CA_LMRPC_BIT:
      ac_ = new lMaxRPC(n_);
    case CA_RPC3:
      ac_ = new RPC3(n_);
      break;
    default:
      break;
  }
}

// SearchStatistics MAC::enforce(const int time_limits) {
//	Timer t;
//	consistent_ = ac_->enforce_arc(n_->vars, 0);
//	x_evt_.clear();
//	if (!consistent_) {
//		statistics_.solve_time = t.elapsed();
//		return statistics_;
//	}
//
//	while (!finished_) {
//		if (t.elapsed() > time_limits) {
//			statistics_.solve_time = t.elapsed();
//			statistics_.time_out = true;
//			return statistics_;
//		}
//
//		IntVal v_a = select_v_value();
//		I.push(v_a);
//		cout << v_a << endl;
//		++statistics_.num_positive;
//		v_a.v()->ReduceTo(v_a.a(), I.size());
//		x_evt_.push_back(v_a.v());
//		consistent_ = ac_->enforce_arc(x_evt_, I.size());
//		cout << ac_->del() << endl;
//		x_evt_.clear();
//
//		if (consistent_&&I.full()) {
//			cout << I << endl;
//			finished_ = true;
//			//++sol_count_;
//			//consistent_ = false;
//		}
//
//		while (!consistent_ && !I.empty()) {
//			v_a = I.pop();
//
//			for (IntVar* v : n_->vars)
//				if (!v->assigned())
//					v->RestoreUpTo(I.size() + 1);
//
//			v_a.v()->RemoveValue(v_a.a(), I.size());
//			cout << "!=" << v_a << endl;
//			++statistics_.num_negative;
//			x_evt_.push_back(v_a.v());
//			consistent_ = v_a.v()->size() &&
//ac_->enforce_arc(x_evt_, I.size()); 			cout << ac_->del() << endl;
//			x_evt_.clear();
//		}
//
//		if (!consistent_)
//			finished_ = true;
//	}
//
//	statistics_.solve_time = t.elapsed();
//	return statistics_;
// }

SearchStatistics MAC::enforce(const int time_limits) {
  Timer t;
  x_evt_.clear();
  consistent_ = ac_->enforce(n_->vars, 0).state;
  // consistent_ = one_pass_sac();
  if (!consistent_) {
    statistics_.solve_time = t.elapsed();
    return statistics_;
  }

  while (!finished_) {
    if (t.elapsed() > time_limits) {
      statistics_.solve_time = t.elapsed();
      statistics_.time_out = true;
      return statistics_;
    }

    IntVal v_a = select_v_value(I.size());
    cout << "[Try] Level " << I.size() << ": var[" << v_a.v()->id()
         << "] = " << v_a.a() << endl;
    n_->NewLevel(I.size());
    I.push(v_a);
    ++statistics_.num_positive;
    v_a.v()->ReduceTo(v_a.a(), I.size());
    x_evt_.push_back(v_a.v());
    auto cs = ac_->enforce(x_evt_, I.size());
    consistent_ = cs.state;
    cout << "  [GAC] deletions=" << cs.num_delete
         << ", inconsistent=" << (!consistent_ ? "true" : "false");
    x_evt_.clear();
    // I.update_model_assigned();
    if (consistent_ && I.full()) {
      cout << " → solution found!" << endl;
      finished_ = true;
      statistics_.solve_time = t.elapsed();
      get_solution();
      return statistics_;
      //++sol_count_;
      // consistent_ = false;
    } else if (consistent_) {
      cout << " → continue search" << endl;
    } else {
      cout << " → prune" << endl;
    }

    while (!consistent_ && !I.empty()) {
      v_a = I.pop();
      cout << "[Backtrack] Level " << I.size() << ": var[" << v_a.v()->id()
           << "] = " << v_a.a() << endl;
      n_->BackTo(I.size());
      v_a.v()->RemoveValue(v_a.a(), I.size());
      ++statistics_.num_negative;
      x_evt_.push_back(v_a.v());
      consistent_ =
          v_a.v()->size(I.size()) && ac_->enforce(x_evt_, I.size()).state;
      x_evt_.clear();
      I.update_model_assigned();
    }

    if (!consistent_) finished_ = true;
  }

  statistics_.solve_time = t.elapsed();
  return statistics_;
}

// SearchStatistics MAC::enforce_fc(const int time_limits) {
//	Timer t;
//	//consistent_ = ac_->enforce(n_->vars, 0).state;
//	x_evt_.clear();
//	//if (!consistent_) {
//	//	statistics_.solve_time = t.elapsed();
//	//	return statistics_;
//	//}
//
//	while (!finished_) {
//		if (t.elapsed() > time_limits) {
//			statistics_.solve_time = t.elapsed();
//			statistics_.time_out = true;
//			return statistics_;
//		}
//
//		IntVal v_a = select_v_value();
//		I.push(v_a);
//		//cout << v_a << " I.size() = " << I.size() << endl;
//		++statistics_.num_positive;
//		v_a.v()->ReduceTo(v_a.a(), I.size());
//		x_evt_.push_back(v_a.v());
//		consistent_ = ac_->enforce(x_evt_, I.size()).state;
//		x_evt_.clear();
//
//		if (consistent_&&I.full()) {
//			//cout << I << endl;
//			finished_ = true;
//			//++sol_count_;
//			//consistent_ = false;
//		}
//
//		while (!consistent_ && !I.empty()) {
//			v_a = I.pop();
//			//cout << "!=" << v_a << " I.size() = " << I.size() <<
//endl; 			for (IntVar* v : n_->vars) { 				if (!v->assigned()) {
//					v->RestoreUpTo(I.size() + 1);
//				}
//			}
//
//			v_a.v()->RemoveValue(v_a.a(), I.size());
//			++statistics_.num_negative;
//			consistent_ = v_a.v()->size();
//			//x_evt_.push_back(v_a.v());
//			//consistent_ = v_a.v()->size() && ac_->enforce(x_evt_,
//I.size()).state;
//			//x_evt_.clear();
//		}
//
//		if (!consistent_)
//			finished_ = true;
//	}
//
//	statistics_.solve_time = t.elapsed();
//	return statistics_;
// }

MAC::~MAC() {
  delete ac_;
  // delete I;
}

bool MAC::solution_check() const {
  n_->CopyLevel(0, n_->tmp());
  bool res = false;
  for (int i = 0; i < I.size(); ++i) {
    auto v = I[i].v();
    const auto a = I[i].a();
    v->ReduceTo(a, n_->tmp());
    res = ac_->enforce(n_->vars, n_->tmp()).state;
  }
  n_->ClearLevel(n_->tmp());

  return res;
  // if (solutions.empty())
  //	return false;

  // vector<int> tuple(max_arity);
  // tuple.clear();
  // for (auto c : tabs) {
  //	for (auto v : c->scope)
  //		tuple.push_back(sol_std[v->id]);
  //	if (!c->sat(tuple))
  //		return false;
  //	tuple.clear();
  // }
  // return true;
}

void MAC::get_solution() {
  solution.resize(n_->vars.size());
  for (int i = 0; i < n_->vars.size(); ++i) {
    solution[i] = I[i].a();
  }

  stringstream strs;
  for (int a : solution) {
    strs << a << " ";
  }
  sol_str = strs.str();
  sol_str.pop_back();
}

bool MAC::one_pass_sac() const {
  vector<IntVar*> x_evt;
  for (auto v : n_->vars) {
    for (auto i : v->values()) {
      if (v->have(i, 0)) {
        // bd[v->id()][i] = true;

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

IntVal MAC::select_v_value(const int p) const {
  // IntVar* v = n_->vars[I->size()];
  // return IntVal(v, v->head());
  // IntVal val(nullptr, -1);
  // switch (h_) {
  // case DOM: {
  //	float min_size = INT_MAX;
  //	for (auto v : n_->vars)
  //		if (!v->assigned(p))
  //			if (v->size(p) < min_size) {
  //				min_size = v->size(p);
  //				val.v(v);
  //			}
  //	val.a(val.v()->head(p));
  // }
  //		  break;
  // case DOM_WDEG: {
  //	float min_size = FLT_MAX;
  //	for (auto x : n_->vars) {
  //		if (!x->assigned(p)) {
  //			float x_w = 0.0;
  //			float x_dw = 0.0;
  //			for (auto c : n_->subscription[x]) {
  //				int cnt = 0;
  //				for (auto y : c->scope)
  //					if (!y->assigned(p))
  //						++cnt;
  //				if (cnt > 1)
  //					x_w += c->weight;
  //			}

  //			if (x->size(p) == 1 || x_w == 0)
  //				x_dw = -1;
  //			else
  //				x_dw = x->size(p) / x_w;
  //			if (x_dw < min_size) {
  //				min_size = x_dw;
  //				val.v(x);
  //			}
  //		}
  //	}
  //	val.a(val.v()->head(p));
  //}
  //			   break;
  // default:;
  //}
  IntVar* v = select_var(p);
  const int a = select_val(v, p);
  IntVal val(v, a);
  return val;
}

IntVar* MAC::select_var(const int p) const {
  IntVar* var = nullptr;
  double min_size = DBL_MAX;
  switch (varh_) {
    case Heuristic::VRH_DOM_MIN: {
      for (auto v : n_->vars)
        if (!v->assigned(p))
          if (v->size(p) < min_size) {
            min_size = v->size(p);
            var = v;
          }
    }
      return var;
    case Heuristic::VRH_LEX:
      var = n_->vars[I.size() + 1];
      break;
    case Heuristic::VRH_VWDEG:
      break;
    case Heuristic::VRH_DOM_DEG_MIN: {
      for (auto v : n_->vars)
        if (!v->assigned(p)) {
          int dom_deg;
          if (n_->neighborhood[v].size() == 0)
            dom_deg = -1;
          else
            dom_deg = v->size(p) / n_->neighborhood[v].size();
          if (dom_deg < min_size) {
            min_size = dom_deg;
            var = v;
          }
        }
    }
      return var;
    case Heuristic::VRH_DOM_WDEG_MIN: {
      // cout << "wdeg" << endl;kx
      for (auto x : n_->vars) {
        if (!x->assigned(p)) {
          double x_w = 0.0;
          double x_dw = 0.0;

          for (auto c : n_->subscription[x]) {
            int cnt = 0;
            for (auto y : c->scope)
              if (!y->assigned(p)) ++cnt;
            if (cnt > 1) x_w += c->weight;
          }

          if (x->size(p) == 1 || x_w == 0)
            x_dw = -1;
          else
            x_dw = x->size(p) / x_w;

          if (x_dw < min_size) {
            min_size = x_dw;
            var = x;
          }
        }
      }
    }
      return var;
    default:
      var = nullptr;
      break;
  }
  return var;
}

int MAC::select_val(const IntVar* v, const int p) const {
  int val = -1;
  switch (valh_) {
    case Heuristic::VLH_MIN:
      val = v->head(p);
      break;
    case Heuristic::VLH_MIN_DOM:
      break;
    case Heuristic::VLH_MIN_INC:
      break;
    case Heuristic::VLH_MAX_INC:
      break;
    case Heuristic::VLH_VWDEG:
      val = -1;
      break;
    default:;
  }
  return val;
}

}  // namespace cpim
