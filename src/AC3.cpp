#include "Solver.h"
namespace cpim {
AC3::AC3(Network* m) : AC(m) {
  q_.initial(m_->vars.size());
  stamp_var_.resize(m_->vars.size(), 0);
  stamp_tab_.resize(m_->tabs.size(), 0);
  // inital_q_arc();
  // q_.reserve(m->vars.size());
}
void AC3::insert(IntVar* v) {
  q_.push(v);
  ++t_;
  stamp_var_[v->id()] = t_;
}
// bool AC3::enforce(VarEvt* x_evt, const int level) {
//	level_ = level;
//	q_.clear();
//
//	for (int i = 0; i < x_evt->size(); ++i)
//		insert(x_evt->at(i));
//
//	while (!q_.empty()) {
//		IntVar* x = q_.back();
//		for (Tabular* c : m_->subscription[x]) {
//			if (stamp_var_[x->id()] > stamp_tab_[c->id()]) {
//				for (auto y : c->scope) {
//					if (!y->assigned()) {
//						bool aa = false;
//						for (auto z : c->scope)
//							if ((z != x) &&
// stamp_var_[z->id()] > stamp_tab_[c->id()])
// aa = true;
//
//						if ((y != x) || aa) {
//							if (revise(arc(c, y))) {
//								if (y->size() ==
// 0) { return false;
//								}
//								insert(y);
//							}
//						}
//
//					}
//				}
//				++t_;
//				stamp_tab_[c->id()] = t_;
//			}
//		}
//	}
//	return true;
// }

// ConsistencyState AC3::enforce(vector<IntVar*>& x_evt, const int level) {
//	level_ = level;
//	q_.clear();
//	cs.level = level;
//	cs.num_delete = 0;
//
//	for (auto v : x_evt)
//		insert(v);
//	while (!q_.empty()) {
//		IntVar* x = q_.back();
//		q_.pop_back();
//		for (Tabular* c : m_->subscription[x]) {
//			if (stamp_var_[x->id()] > stamp_tab_[c->id()]) {
//				for (auto y : c->scope) {
//					if (!y->assigned()) {
//						bool aa = false;
//						for (auto z : c->scope)
//							if ((z != x) &&
// stamp_var_[z->id()] > stamp_tab_[c->id()])
// aa = true;
//
//						if ((y != x) || aa)
//							if (revise(arc(c, y))) {
//								if (y->faild())
//{ 									cs.tab =
//c; 									cs.var =
//y;
//									++(c->weight);
//									//cout
//<< c->id()<<": weight = "<<c->weight << endl;
//cs.state = false;
//return cs;
//								}
//								insert(y);
//							}
//					}
//				}
//				++t_;
//				stamp_tab_[c->id()] = t_;
//			}
//		}
//	}
//	cs.state = true;
//	return cs;
// }

ConsistencyState AC3::enforce(vector<IntVar*>& x_evt, const int level) {
  stamp_var_.assign(stamp_var_.size(), 0);
  stamp_tab_.assign(stamp_tab_.size(), 0);
  t_ = 0;
  level_ = level;
  q_.clear();
  cs.level = level;
  cs.num_delete = 0;

  for (auto v : x_evt) insert(v);
  while (!q_.empty()) {
    IntVar* x = q_.pop();
    for (Tabular* c : m_->subscription[x]) {
      if (stamp_var_[x->id()] > stamp_tab_[c->id()]) {
        for (auto y : c->scope) {
          if (!y->assigned(level)) {
            bool aa = false;
            for (auto z : c->scope)
              if ((z != x) && stamp_var_[z->id()] > stamp_tab_[c->id()])
                aa = true;

            if ((y != x) || aa)
              if (revise(arc(c, y), level)) {
                if (y->faild(level)) {
                  cs.tab = c;
                  cs.var = y;
                  ++(c->weight);
                  // cout << c->id()<<": weight = "<<c->weight << endl;
                  cs.state = false;
                  return cs;
                }
                insert(y);
              }
          }
        }
        ++t_;
        stamp_tab_[c->id()] = t_;
      }
    }
  }
  cs.state = true;
  return cs;
}

// ConsistencyState AC3::enforce_arc(vector<IntVar*>& x_evt, const int level) {
//	level_ = level;
//	delete_ = 0;
//	cs.level = level;
//	cs.num_delete = 0;
//	for (int i = 0; i < x_evt.size(); ++i)
//		for (Tabular* c : m_->subscription[x_evt[i]])
//			for (IntVar* x : c->scope)
//				if (x != x_evt[i])
//					Q.push(arc(c, x));
//
//	while (!Q.empty()) {
//		arc c_x = Q.pop();
//		//cout << "--" << c_x << endl;
//
//		if (revise(c_x)) {
//			if (c_x.v()->faild()) {
//				cs.tab = c_x.c();
//				cs.var = c_x.v();
//				cs.state = false;
//				++(c_x.c()->weight);
//				//cout << c_x.c()->id() << ": weight = " <<
// c_x.c()->weight << endl; 				return cs;
//			}
//
//			for (Tabular* c : m_->subscription[c_x.v()])
//				if (c != c_x.c())
//					for (IntVar* v : c->scope)
//						if ((v != c_x.v()) &&
//(!v->assigned())) Q.push(arc(c, v));
//		}
//	}
//	cs.state = true;
//	return cs;
// }

bool AC3::revise(const arc& c_x, const int p) {
  const int num_elements = c_x.v()->size(p);
  int a = c_x.v()->head(p);

  while (a != Limits::INDEX_OVERFLOW) {
    if (!seek_support(IntConVal(c_x, a), p)) {
      c_x.v()->RemoveValue(a, p);
      ++cs.num_delete;
      ++delete_;
    }
    a = c_x.v()->next(a, p);
  }

  return num_elements != c_x.v()->size(p);
}

bool AC3::seek_support(const IntConVal& c_val, const int p) {
  m_->GetFirstValidTuple(c_val, tmp_tuple_, p);
  // if (c_val.c()->id() == 200 && c_val.v()->id() == 25) {
  //	cout << 123 << endl;
  // }
  // cout << "c-value" << c_val << endl;
  while (Existed(tmp_tuple_)) {
    // cout << "tuple: " << tmp_tuple_[0] << "," << tmp_tuple_[1] << endl;
    if (c_val.c()->sat(tmp_tuple_))
      return true;
    else
      m_->GetNextValidTuple(c_val, tmp_tuple_, p);
  }
  return false;
}

// void AC3::inital_q_arc() {
//	Q.MakeQue(m_->tabs.size(), m_->max_arity());
// }
}  // namespace cpim
