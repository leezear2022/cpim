#include <cfloat>
#include <sstream>

#include "Solver.h"
#include "solver/common/propagation_engine.h"  // Phase 2.1
#include "solver/common/table_propagator.h"     // Phase 2.1
#include <glog/logging.h>

using namespace std;
namespace cpim {

MAC::MAC(Network* n, const ACAlgorithm ac_algzm, const Heuristic::Var varh,
         const Heuristic::Val valh, bool use_propagator_framework)
    : n_(n), ac_algzm_(ac_algzm), varh_(varh), valh_(valh),
      use_propagator_framework_(use_propagator_framework) {
  x_evt_.reserve(n_->vars.size());
  I.initial(n_);

  // Phase 2.1: 如果启用 Propagator 框架，初始化 PropagationEngine
  if (use_propagator_framework_) {
    VLOG(1) << "MAC: Using Propagator framework";
    InitializePropagationEngine();
  } else {
    VLOG(1) << "MAC: Using traditional AC algorithm";
  }

  // 传统 AC 算法初始化
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

void MAC::InitializePropagationEngine() {
  // 创建 PropagationEngine
  propagation_engine_ = new PropagationEngine();

  // 为每个表约束创建 TableConstraintPropagator
  for (Tabular* constraint : n_->tabs) {
    auto propagator = std::make_unique<TableConstraintPropagator>(constraint, n_);
    propagation_engine_->AddPropagator(std::move(propagator));
  }

  VLOG(1) << "MAC: Initialized PropagationEngine with "
          << propagation_engine_->GetPropagatorCount() << " propagators";
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
//		v_a.v()->ReduceTo(v_a.a());
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
//			v_a.v()->RemoveValue(v_a.a());
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

  // Phase 2.1: 初始传播（使用 PropagationEngine 或 AC）
  if (use_propagator_framework_) {
    auto result = propagation_engine_->Propagate(n_->vars, 0);
    consistent_ = (result.state != PropagationState::INCONSISTENT);
    VLOG(2) << "MAC: Initial propagation (PropagationEngine) - consistent=" << consistent_;
  } else {
    consistent_ = ac_->enforce(n_->vars, 0).state;
  }

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
    n_->trail()->NewLevel();
    I.push(v_a);
    ++statistics_.num_positive;
    v_a.v()->ReduceTo(v_a.a());
    x_evt_.push_back(v_a.v());

    // Phase 2.1: 赋值后传播（使用 PropagationEngine 或 AC）
    if (use_propagator_framework_) {
      auto result = propagation_engine_->Propagate(x_evt_, I.size());
      consistent_ = (result.state != PropagationState::INCONSISTENT);
      int num_delete = result.modified_vars.size();  // 近似
      cout << "  [Propagator] modified_vars=" << num_delete
           << ", inconsistent=" << (!consistent_ ? "true" : "false");
    } else {
      auto cs = ac_->enforce(x_evt_, I.size());
      consistent_ = cs.state;
      cout << "  [GAC] deletions=" << cs.num_delete
           << ", inconsistent=" << (!consistent_ ? "true" : "false");
    }
    x_evt_.clear();
    // I.update_model_assigned();
    if (consistent_ && I.full()) {
      cout << " → solution found!" << endl;
      finished_ = true;
      statistics_.num_sol = 1;  // FIX: Set num_sol when solution found
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
      v_a = I.pop();  // I.size() 减 1
      cout << "[Backtrack] Level " << I.size() << ": var[" << v_a.v()->id()
           << "] = " << v_a.a() << endl;
      // Phase 1.1: 回溯到 I.size() 对应的层级 (pop 后的深度)
      // Note: Trail level 从 -1 开始,第一次 NewLevel() 变为 0
      // 因此 I.size() == 0 时应回溯到 level -1 (初始状态)
      if (n_->trail()->CurrentLevel() >= 0) {
        n_->trail()->BacktrackTo(I.size() - 1);
      }
      v_a.v()->RemoveValue(v_a.a());
      ++statistics_.num_negative;
      x_evt_.push_back(v_a.v());

      // Phase 2.1: 回溯后传播（使用 PropagationEngine 或 AC）
      if (use_propagator_framework_) {
        if (v_a.v()->size()) {
          auto result = propagation_engine_->Propagate(x_evt_, I.size());
          consistent_ = (result.state != PropagationState::INCONSISTENT);
        } else {
          consistent_ = false;
        }
      } else {
        consistent_ =
            v_a.v()->size() && ac_->enforce(x_evt_, I.size()).state;
      }
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
//		v_a.v()->ReduceTo(v_a.a());
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
//			v_a.v()->RemoveValue(v_a.a());
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
  // Phase 2.1: 清理 PropagationEngine
  if (propagation_engine_ != nullptr) {
    delete propagation_engine_;
  }
  // delete I;
}

bool MAC::solution_check() const {
  // Phase 1.1: CopyLevel removed);
  bool res = false;
  for (int i = 0; i < I.size(); ++i) {
    auto v = I[i].v();
    const auto a = I[i].a();
    v->ReduceTo(a);
    res = ac_->enforce(n_->vars, n_->tmp()).state;
  }
  // Phase 1.1: ClearLevel removed);

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
  // FIX: Get solution from variable domains at current level, not from I[]
  int level = I.size();
  for (int i = 0; i < n_->vars.size(); ++i) {
    // Each variable should have exactly one value left in its domain
    solution[i] = n_->vars[i]->head();
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
      if (v->have(i)) {
        // bd[v->id()][i] = true;

        // Phase 1.1: CopyLevel removed;
        v->ReduceTo(i);
        v->assign(true);
        x_evt.push_back(v);
        const auto res = ac_->enforce(x_evt, 1).state;
        if (!res) {
          // cout << "test:" << v->id() << "," << i << endl;
          // cout << "no pass" << endl;
          v->RemoveValue(i);
        }
        x_evt.clear();
        // Phase 1.1: ClearLevel removed;
      }
      if (v->faild()) {
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
  //		if (!v->assigned())
  //			if (v->size() < min_size) {
  //				min_size = v->size();
  //				val.v(v);
  //			}
  //	val.a(val.v()->head());
  // }
  //		  break;
  // case DOM_WDEG: {
  //	float min_size = FLT_MAX;
  //	for (auto x : n_->vars) {
  //		if (!x->assigned()) {
  //			float x_w = 0.0;
  //			float x_dw = 0.0;
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
  //			if (x_dw < min_size) {
  //				min_size = x_dw;
  //				val.v(x);
  //			}
  //		}
  //	}
  //	val.a(val.v()->head());
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
        if (!v->assigned())
          if (v->size() < min_size) {
            min_size = v->size();
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
        if (!v->assigned()) {
          int dom_deg;
          if (n_->neighborhood[v].size() == 0)
            dom_deg = -1;
          else
            dom_deg = v->size() / n_->neighborhood[v].size();
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
        if (!x->assigned()) {
          double x_w = 0.0;
          double x_dw = 0.0;

          for (auto c : n_->subscription[x]) {
            int cnt = 0;
            for (auto y : c->scope)
              if (!y->assigned()) ++cnt;
            if (cnt > 1) x_w += c->weight;
          }

          if (x->size() == 1 || x_w == 0)
            x_dw = -1;
          else
            x_dw = x->size() / x_w;

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
      val = v->head();
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
