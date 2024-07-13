//
// Created by lee on 24-7-4.
//
#include "xcsp3model/HModel.h"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <utility>

namespace cpim {

////////// HVar //////////
HVar HVarNode::Make(int id, int uid, const std::string &name, int min_val,
                    int max_val) {
  auto node = make_shared<HVarNode>(id, uid, name, min_val, max_val);
  return HVar(node);
}

HVar HVarNode::Make(int id, int uid, const std::string &name,
                    std::vector<int> &v) {
  auto node = make_shared<HVarNode>(id, uid, name, v);
  return HVar(node);
}
HVarNode::HVarNode(int id, int uid, const std::string &name, int min_val,
                   int max_val)
    : id(id), uid(uid), name(std::move(name)), std_max(max_val - min_val) {
  int j = 0;
  const int size = max_val - min_val + 1;
  vals.resize(size);
  anti_map.resize(size);
  for (int i = min_val; i <= max_val; ++i) {
    val_map[i] = j;
    vals[j] = j;
    anti_map[j] = i;
    ++j;
  }
}

HVarNode::HVarNode(int id, int uid, const std::string &name,
                   std::vector<int> &v)
    : id(id), uid(uid), name(std::move(name)), std_max(v.size() - 1) {
  std::sort(v.begin(), v.end());
  vals.resize(v.size());
  anti_map = v;
  for (size_t i = 0; i < vals.size(); ++i) {
    val_map[v[i]] = i;
    vals[i] = i;
  }
}

void HVarNode::show() {
  std::cout << id << "| " << name << ": ";
  for (size_t i = 0; i < vals.size(); ++i)
    std::cout << vals[i] << "[" << val_map[i] << "] ";
  std::cout << std::endl;
}

HTab HTabNode::Make(int id, bool sem, std::vector<std::vector<int>> &ts,
                    std::vector<HVar> &scp) {
  auto node = make_shared<HTabNode>(id, sem, ts, scp);
  return HTab(node);
}

HTab HTabNode::Make(HTab &other, std::vector<HVar> &scp) {
  auto node = make_shared<HTabNode>(other, scp);
  return HTab(node);
}

HTabNode::HTabNode(int id, bool sem, std::vector<std::vector<int>> &ts,
                   std::vector<HVar> &scp)
    : id(id), semantics(sem), scope(scp) {
  unsigned long all_size = 1;
  for (auto i : scp) all_size *= i->vals.size();
  unsigned long sup_size;

  if (!sem)
    sup_size = all_size - ts.size();
  else
    sup_size = ts.size();
  std::vector<int> ori_t_(scope.size());
  std::vector<int> std_t_(scope.size());
  tmp_t_.resize(scope.size());
  tuples.resize(sup_size, std::vector<int>(scope.size()));

  if (sem) {
    for (size_t i = 0; i < sup_size; i++) {
      GetSTDTuple(ts[i], std_t_);
      tuples[i] = std_t_;
    }
  } else {
    int j = 0;
    for (int i = 0; (i < all_size) && (j <= sup_size); ++i) {
      GetTuple(i, ori_t_, std_t_);
      if (std::find(ts.begin(), ts.end(), ori_t_) == ts.end())
        tuples[j++] = std_t_;
    }
  }

  semantics = true;
  isSTD = true;
}

HTabNode::HTabNode(HTab &t, std::vector<HVar> &scp)
    : scope(scp), semantics(t->semantics) {
  id = t->id + 1;
  isSTD = true;
  tuples = t->tuples;
}

void HTabNode::GetSTDTuple(std::vector<int> &src_tuple,
                           std::vector<int> &std_tuple) {
  for (size_t i = 0; i < src_tuple.size(); ++i)
    std_tuple[i] = scope[i]->val_map[src_tuple[i]];
}

void HTabNode::GetORITuple(std::vector<int> &std_tuple,
                           std::vector<int> &ori_tuple) {
  for (size_t i = 0; i < std_tuple.size(); ++i)
    ori_tuple[i] = scope[i]->anti_map[std_tuple[i]];
}

// std::ostream operator<<(const std::ostream& lhs,
// std::vector<std::vector<int>>::const_reference rhs);
void HTabNode::show() {
  const std::string sem = semantics ? "supports" : "conflicts";
  std::vector<int> scope_int;
  for (auto &v : scope) {
    scope_int.push_back(v->id);
  }
  std::cout << "id: " << id << " semantics: " << sem
            << " size: " << tuples.size() << " arity:" << scope.size()
            << " scope = {" << scope_int << std::endl;
  std::cout << tuples << std::endl;
  std::cout << std::endl;
}

void HTabNode::GetTuple(int idx, std::vector<int> &src_t,
                        std::vector<int> &std_t) {
  for (int i = (scope.size() - 1); i >= 0; --i) {
    HVar v = scope[i];
    std_t[i] = idx % v->vals.size();
    src_t[i] = v->anti_map[std_t[i]];
    idx /= v->vals.size();
  }
}

bool HTabNode::SAT(std::vector<int> &t) {
  return binary_search(tuples.begin(), tuples.end(), t);
}

// bool HTab::sat(std::vector<int> &t) {
//   return binary_search(p_->tuples.begin(), p_->tuples.end(), t);
// }

////////// HModel //////////
HModelNode::HModelNode(std::string &name) : name_(name) {}

HModel HModelNode::Make() {
  auto node = make_shared<HModelNode>();
  return HModel(node);
}

HModel HModelNode::Make(std::string &name) {
  auto node = make_shared<HModelNode>(name);
  return HModel(node);
}

void HModelNode::show() {
  std::cout << "--------------Variables--------------" << std::endl;
  std::cout << "size: " << vars.size() << "\tmax domain size :" << mds_
            << std::endl;
  for (auto v : vars) v->show();
  std::cout << "-------------Constraints-------------" << std::endl;
  std::cout << "size: " << tabs.size() << "\tmax arity size :" << mas_
            << std::endl;
  for (const auto &t : tabs) t->show();
}

int HModelNode::regist(const std::string &exp_name,
                       std::function<int(std::vector<int> &)> exp) {
  const int id = generate_expr_uid();
  if (Funcs::str_expr_map.find(exp_name) != Funcs::str_expr_map.end()) {
    Funcs::str_expr_map[exp_name] = id;
    Funcs::int_expr_map[id] = exp;
    return id;
  } else {
    std::cout << "existing" << std::endl;
    return INT_MIN;
  }
}

int HModelNode::calculate(std::vector<int> &expr,
                          std::vector<int> &params_len) {
  std::vector<int> res_stack;
  // res_stack.reserve(10);
  int j = -1;

  for (int i = 0; i < expr.size(); ++i) {
    const int op = expr[i];

    // op为参数
    if (op > MAX_OPT)
      res_stack.push_back(op);
    else
      result(op, res_stack, params_len[++j]);
  }

  return res_stack[0];
}

std::vector<HTab> HModelNode::solution_check(std::vector<int> &sol) {
  if (sol.empty()) {
    std::cout << "no solution" << std::endl;
    return std::vector<HTab>();
  }
  std::vector<int> a(sol.size());
  for (int i = 0; i < sol.size(); ++i) {
    a[i] = vars[i]->anti_map[sol[i]];
  }

  for (auto c : a) std::cout << c << " ";

  std::cout << std::endl;
  std::vector<int> tuple(max_arity());
  std::vector<HTab> conflict_constraints(tabs.size());
  conflict_constraints.clear();
  tuple.clear();
  for (auto c : tabs) {
    for (auto v : c->scope) {
      tuple.push_back(sol[v->id]);
    }
    if (!c->SAT(tuple)) {
      std::cout << "conflict constraint id = " << c->id << std::endl;
      conflict_constraints.push_back(c);
    }
    tuple.clear();
  }

  if (conflict_constraints.empty()) std::cout << "pass!" << std::endl;

  return conflict_constraints;
}

void HModelNode::get_postfix(const std::string expr, std::vector<int> &data,
                             std::vector<int> &params,
                             std::vector<int> &num_op_params,
                             std::vector<HVar> &scp) {
  // 转换表达式
  std::string s = expr;
  std::string tmp;
  int startpos = 0;
  std::tuple<ExpType, int> t;
  for (int i = 0; i < s.length(); ++i) {
    switch (s[i]) {
      case '(':
        tmp = s.substr(startpos, i - startpos);
        t = get_type_tuple(tmp);
        if (std::get<0>(t) != ET_NONE) {
          data.push_back(std::get<1>(t));

          if (std::get<0>(t) == ET_VAR) {
            params.push_back(std::get<1>(t));
            if (std::find(scp.begin(), scp.end(), str_var_map_[tmp]) ==
                scp.end())
              scp.push_back(str_var_map_[tmp]);
          }

          if (std::get<0>(t) == ET_CONST) params.push_back(std::get<1>(t));
        }
        data.push_back(Funcs::str_expr_map["("]);
        startpos = i + 1;
        break;
      case ')':
        tmp = s.substr(startpos, i - startpos);
        t = get_type_tuple(tmp);
        if (std::get<0>(t) != ET_NONE) {
          data.push_back(std::get<1>(t));

          if (std::get<0>(t) == ET_VAR) {
            params.push_back(std::get<1>(t));
            if (std::find(scp.begin(), scp.end(), str_var_map_[tmp]) ==
                scp.end())
              scp.push_back(str_var_map_[tmp]);
          }

          if (std::get<0>(t) == ET_CONST) params.push_back(std::get<1>(t));
        }
        data.push_back(Funcs::str_expr_map[")"]);
        startpos = i + 1;
        break;
      case ',':
        //","不被推入栈
        tmp = s.substr(startpos, i - startpos);

        t = get_type_tuple(tmp);
        if (std::get<0>(t) != ET_NONE) {
          data.push_back(std::get<1>(t));

          if (std::get<0>(t) == ET_VAR) {
            params.push_back(std::get<1>(t));
            if (std::find(scp.begin(), scp.end(), str_var_map_[tmp]) ==
                scp.end())
              scp.push_back(str_var_map_[tmp]);
          }

          if (std::get<0>(t) == ET_CONST) params.push_back(std::get<1>(t));
        }
        // data.push_back(Funcs::str_expr_map[","]);
        startpos = i + 1;
        break;
      case ' ':
        startpos = i + 1;
        break;
      default:
        break;
    }
  }

  // 统计每个参数个数
  std::vector<int> postfix_stack(data);
  int last_lpar_idx = 0;
  for (int i = 0; i < postfix_stack.size(); ++i) {
    int op = postfix_stack[i];

    // 找到左括号
    if (op == ET_LPAR) last_lpar_idx = i;
    // 找右括号，后寻找左括号
    else if (op == ET_RPAR) {
      int num = 0;
      for (int j = last_lpar_idx; j < i; ++j) {
        if (postfix_stack[j] > MAX_OPT) {
          postfix_stack[j] = ET_NONE;
          ++num;
        } else if (postfix_stack[j] < ET_PARAMS &&
                   postfix_stack[j] > ET_COMMA) {
          postfix_stack[j] = ET_NONE;
          ++num;
        }
      }
      num_op_params.push_back(num);

      postfix_stack[last_lpar_idx] = ET_NONE;
      const int idx = last_lpar_idx - 1;
      op = postfix_stack[idx];

      // 再找下一个左括号
      while (op != ET_LPAR && last_lpar_idx > 0) {
        --last_lpar_idx;
        op = postfix_stack[last_lpar_idx];
      }
    }
  }

  postfix_stack.clear();
  // 生成后缀表达式
  last_lpar_idx = 0;
  for (int i = 0; i < data.size(); ++i) {
    int op = data[i];

    // 参数与常量压入栈
    if (op > MAX_OPT) postfix_stack.push_back(op);

    // 找到左括号
    if (op == ET_LPAR) last_lpar_idx = i;
    // 找右括号，后寻找左括号
    else if (op == ET_RPAR) {
      postfix_stack.push_back(data[last_lpar_idx - 1]);

      data[last_lpar_idx] = ET_NONE;
      const int idx = last_lpar_idx - 1;
      op = data[idx];

      // 再找下一个左括号
      while (op != ET_LPAR && last_lpar_idx > 0) {
        --last_lpar_idx;
        op = data[last_lpar_idx];
      }
    }
  }

  data = postfix_stack;
  std::vector<int>().swap(postfix_stack);
}

std::tuple<ExpType, int> HModelNode::get_type_tuple(std::string &expr) {
  if (expr == "") return std::make_tuple(ET_NONE, Funcs::str_expr_map[expr]);
  if (expr == "(") return std::make_tuple(ET_LPAR, Funcs::str_expr_map[expr]);
  if (expr == ")") return std::make_tuple(ET_RPAR, Funcs::str_expr_map[expr]);
  if (expr == ",") return std::make_tuple(ET_COMMA, Funcs::str_expr_map[expr]);
  if (Funcs::str_expr_map.find(expr) != Funcs::str_expr_map.end())
    return std::make_tuple(ET_OP, Funcs::str_expr_map[expr]);
  if (expr[0] >= '0' && expr[0] <= '9')
    return std::make_tuple(ET_CONST, atoi(expr.c_str()));
  if (str_var_map_.find(expr) != str_var_map_.end())
    return std::make_tuple(ET_VAR, str_var_map_[expr]->uid);

  std::cout << "undefined" << std::endl;
  return std::make_tuple(ET_NULL, INT_MIN);
}

ExpType HModelNode::get_type(const int expr) {
  if (expr <= INT_MIN + 3) return static_cast<ExpType>(expr);
  if (expr < MAX_OPT && expr > INT_MIN + 3) return ET_OP;
  if (expr > MAX_OPT && expr < MAX_VALUE) return ET_CONST;
  if (expr > MAX_VALUE) return ET_VAR;
  return ET_NULL;
}

void HModelNode::subscript(const HTab &tab) {
  for (const auto &v : tab->scope) subscriptions[v].push_back(tab);
  neighbor(tab);
}

void HModelNode::neighbor(const HTab &tab) {
  if (neighborhoods.empty())
    neighborhoods.resize(vars.size(),
                         std::vector<std::vector<int>>(vars.size()));

  for (const auto &x : tab->scope) {
    for (const auto &y : tab->scope) {
      if (x != y) {
        neighborhoods[x->id][y->id].push_back(tab->id);
        if (neighborhoods[x->id][y->id].size() > 1) {
          have_same_scope_ = true;
        }
      }
    }
  }
}

void HModelNode::get_scope(std::vector<std::string> &scp_str,
                           std::vector<HVar> &scp) {
  scp.resize(scp_str.size());
  for (int i = 0; i < scp_str.size(); ++i) scp[i] = str_var_map_[scp_str[i]];
}

std::vector<HVar> HModelNode::get_scope(const std::string &scp_str) const {
  // 使用 stringstream 和 istringstream 来解析 scope_str
  std::istringstream iss(scp_str);
  std::string token;
  // scope.reserve(arity);
  std::vector<HVar> scope;
  while (iss >> token) {
    if (token[0] == 'V') {
      scope.push_back(vars[std::stoi(token.substr(1))]);
    }
  }
  return scope;
}
void HModelNode::generate_tuples(const std::string &ts_str_, int size,
                                 int arity,
                                 std::vector<std::vector<int>> &tuples) {
  std::istringstream iss(ts_str_);
  std::string token;
  tuples.resize(size, std::vector<int>(arity));

  for (int i = 0; i < size; ++i) {
    for (int j = 0; j < arity; ++j) {
      if (iss >> token) {
        if (token == "|") {
          --j;  // Skip separator
          continue;
        }
        tuples[i][j] = std::stoi(token);
      }
    }
  }
}

void HModelNode::get_STD_tuple(std::vector<int> &src_tuple,
                               std::vector<int> &std_tuple,
                               std::vector<HVar> &scp) {
  for (size_t i = 0; i < src_tuple.size(); ++i)
    std_tuple[i] = scp[i]->val_map[src_tuple[i]];
}

void HModelNode::get_ORI_Tuple(std::vector<int> &std_tuple,
                               std::vector<int> &ori_tuple,
                               std::vector<HVar> &scp) {
  for (size_t i = 0; i < std_tuple.size(); ++i)
    ori_tuple[i] = scp[i]->anti_map[std_tuple[i]];
}

void HModelNode::get_ori_tuple_by_index(int idx, std::vector<int> &t,
                                        const std::vector<HVar> &scp) {
  for (int i = scp.size() - 1; i >= 0; --i) {
    const int size = scp[i]->vals.size();
    t[i] = idx % size;
    idx /= size;
  }
}

void HModelNode::result(const int op, std::vector<int> &result, const int len) {
  std::vector<int> a(len);
  for (int i = len - 1; i >= 0; --i) {
    a[i] = result.back();
    result.pop_back();
  }
  result.push_back(Funcs::int_expr_map[op](a));
}

int HModelNode::AddVar(const int id, const std::string &name,
                       std::vector<int> &v) {
  auto newId = vars.size();
  auto uid = generate_var_uid();
  HVar var = HVarNode::Make(id, uid, name, v);
  str_var_map_[name] = var;
  int_var_map_[uid] = var;
  vars.push_back(var);
  mds_ = std::max(mds_, var->vals.size());
  return id;
}

int HModelNode::AddTab(const bool sem, std::vector<std::vector<int>> &ts,
                       std::vector<HVar> &scp, const bool STD) {
  const int id = tabs.size();
  HTab t = HTabNode::Make(id, sem, ts, scp);
  tabs.push_back(t);
  mas_ = std::max(mas_, t->scope.size());
  subscript(t);
  return id;
}

int HModelNode::AddTab(const bool sem, std::vector<std::vector<int>> &ts,
                       std::vector<int> &scp) {
  std::vector<HVar> scope(scp.size());
  for (int i = 0; i < scp.size(); ++i) scope[i] = vars[scp[i]];
  return AddTab(sem, ts, scope);
}

int HModelNode::AddTab(const bool sem, std::vector<std::vector<int>> &ts,
                       std::vector<std::string> &scp) {
  std::vector<HVar> scope;
  get_scope(scp, scope);
  return AddTab(sem, ts, scope);
}

int HModelNode::AddTabAsPrevious(HTab &t, std::vector<std::string> &scp) {
  std::vector<HVar> scope;
  get_scope(scp, scope);
  HTab nt = HTabNode::Make(t, scope);
  tabs.push_back(nt);
  mas_ = std::max(mas_, nt->scope.size());
  subscript(t);
  return tabs.size() - 1;
}

int HModelNode::AddTab(const std::string expr) {
  // cout << expr << endl;
  // 表达式栈
  std::vector<int> expr_stack;
  std::vector<int> params;
  std::vector<HVar> scp;
  std::vector<int> num_op_params;
  get_postfix(expr, expr_stack, params, num_op_params, scp);
  std::vector<int> expr_tmp(expr_stack);
  std::vector<std::vector<int>> ts;
  std::vector<int> ori_t(scp.size());
  std::vector<int> std_t(scp.size());
  std::unordered_map<HVar, int, HVarHash, HVarEqual> t;
  int num_total_tuples = 1;

  for (auto i : scp) num_total_tuples *= i->vals.size();

  for (int i = 0; i < num_total_tuples; ++i) {
    get_ori_tuple_by_index(i, std_t, scp);
    get_ORI_Tuple(std_t, ori_t, scp);

    for (int j = 0; j < scp.size(); ++j) t[scp[j]] = ori_t[j];

    expr_tmp = expr_stack;

    for (size_t j = 0; j < expr_tmp.size(); j++) {
      // 将变量替换为值
      if (get_type(expr_tmp[j]) == ET_VAR)
        // 通过uid拿到HVar* 再通过HVar 拿到赋值
        expr_tmp[j] = t[int_var_map_[expr_tmp[j]]];
    }

    t.clear();
    const int result = calculate(expr_tmp, num_op_params);

    if (result) ts.push_back(std_t);
  }

  return AddTab(true, ts, scp, true);
}

}  // namespace cpim
