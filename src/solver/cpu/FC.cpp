#include "Solver.h"
namespace cpim {
FC::FC(Network* n) : AC3(n) {}
ConsistencyState FC::enforce(vector<IntVar*>& x_evt, const int p) {
  level_ = p;
  delete_ = 0;
  cs.level = p;
  cs.num_delete = 0;
  if (p > 0 && x_evt[0]->assigned()) {
    IntVar* v = x_evt[0];
    for (auto c : m_->subscription[v])
      for (auto y : c->scope)
        if (!y->assigned() && y != v)
          if (revise(arc(c, y), p))
            if (y->faild()) {
              cs.tab = c;
              cs.var = y;
              cs.state = false;
              return cs;
            }
  }

  cs.state = true;
  return cs;
}

FCbit::FCbit(Network* n) : AC3bit(n) {}

ConsistencyState FCbit::enforce(vector<IntVar*>& x_evt, const int p) {
  level_ = p;
  delete_ = 0;
  cs.level = p;
  cs.num_delete = 0;
  if (p > 0 && x_evt[0]->assigned()) {
    IntVar* v = x_evt[0];
    for (auto c : m_->subscription[v])
      for (auto y : c->scope)
        if (!y->assigned() && y != v)
          if (revise(arc(c, y), p))
            if (y->faild()) {
              cs.state = false;
              return cs;
            }
  }

  cs.state = true;
  return cs;
}

}  // namespace cpim
