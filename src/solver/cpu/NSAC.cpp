#include "Solver.h"
namespace cpim {

NSAC::NSAC(Network* m) : AC3bit(m) {
  const int vars_bit_length = ceil(float(m_->vars.size()) / BITSIZE);
  for (auto i : m_->vars) {
    neibor_[i].resize(vars_bit_length, 0);
    for (auto v : m_->neighborhood[i]) {
      auto a = GetBitIdx(v->id());
      neibor_[i][get<0>(a)].set(get<1>(a));
    }
  }
  q_var_.initial(m_->vars.size());
  q_nei_.initial(m_->vars.size());
}

ConsistencyState NSAC::enforce(vector<IntVar*>& x_evt, const int level) {
  q_var_.clear();
  level_ = level;

  if (level == 0)
    for (auto v : x_evt) q_var_.push(v);
  else
    for (auto v : m_->neighborhood[x_evt[0]])
      if (!v->assigned()) q_var_.push(v);

  while (!q_var_.empty()) {
    IntVar* x = q_var_.pop();
    deletion = false;
    const int removals = revise_NSAC(x, x_evt[0], level);

    if (removals > 0) {
      if (x->faild()) {
        cs.state = false;
        return cs;
      }

      for (auto v : m_->neighborhood[x]) {
        if (!v->assigned()) {
          q_var_.push(v);
        }
      }
    }
  }

  cs.state = true;
  return cs;
}
// 是否有删值
int NSAC::revise_NSAC(IntVar* v, IntVar* x, const int level) {
  int num = 0;
  for (auto a : v->values()) {
    if (v->have(a)) {
      // cout << "test :" << v->id() << ", " << a << endl;
      // Phase 1.1: 使用 Trail 替代 CopyLevel - 创建新层进行测试
      m_->trail()->NewLevel();
      const int test_level = m_->trail()->CurrentLevel();

      v->ReduceTo(a);  // Trail 自动记录修改
      v->assign(true);
      const bool res = full_NSAC(v, x, level);  // 传递原始 level

      // 恢复到测试前状态
      m_->trail()->BacktrackTo(test_level - 1);

      if (!res) {
        // cout << "fail" << endl;
        v->RemoveValue(a);
        ++num;
      }
    }
  }
  // cout << "delete: " << num << endl;
  return num;
}

bool NSAC::full_NSAC(IntVar* v, IntVar* x, const int level) {
  bool res;
  q_nei_.clear();
  for (auto c : m_->subscription[v]) {
    IntVar* y = (c->scope[0] == v) ? c->scope[1] : c->scope[0];
    res = revise(arc(c, y), level);
    q_nei_.push(y);
    // 可优化
    if (res) {
      if (y->faild()) {
        ++(c->weight);
        return false;
      }
    }
  }

  while (!q_nei_.empty()) {
    IntVar* y = q_nei_.pop();

    for (auto c : m_->subscription[y]) {
      IntVar* z = (c->scope[0] == y) ? c->scope[1] : c->scope[0];
      if ((is_neibor(v, z) == false) || (v == z)) continue;
      res = revise(arc(c, z), level);
      if (res) {
        if (z->faild()) {
          ++(c->weight);
          return false;
        }
      }
    }
  }

  return true;
}

bool NSAC::is_neibor(IntVar* x, IntVar* y) {
  auto a = GetBitIdx(x->id());
  return neibor_[y][get<0>(a)].test(get<1>(a));
}

// void NSAC::insert_(var_que& q, IntVar* v) {
//	q.push(v);
//	++t_;
//	stamp_var_[v->id()] = t_;
//}
}
