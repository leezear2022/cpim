//
// Created by lee on 24-7-4.
//

#ifndef HMODEL_H
#define HMODEL_H
#include <algorithm>
#include <climits>
#include <cmath>
#include <functional>
#include <iostream>
#include <numeric>
#include <string>
#include <unordered_map>
#include <valarray>
#include <vector>

#include "object.h"

namespace cpim {
namespace common {
// 定义类型
using u64 = unsigned long;
using u32 = unsigned int;
using u16 = unsigned short;
using u8 = unsigned char;

using i64 = long;
using i32 = int;
using i16 = short;
using i8 = char;

// 定义统一编号范围
const int MAX_VALUE = INT_MAX - 4096;
const int MAX_OPT = INT_MIN + 4096;
const int MIN_USER_OPT = MAX_OPT - 1024;

// 定义符号类型
enum ExpType {
  ET_OP,
  ET_CONST,
  ET_VAR,
  ET_NONE = INT_MIN,
  ET_LPAR = INT_MIN + 1,
  ET_RPAR = INT_MIN + 2,
  ET_COMMA = INT_MIN + 3,
  ET_PARAMS = MAX_OPT,
  ET_MARK,
  ET_NULL
};

// 定义约束类型
enum ConType { CT_EXT, CT_INT };

// 隐藏约束转表约束
using func_map =
    std::unordered_map<int, std::function<int(std::vector<int> &)>>;
// typedef std::unordered_map<int, std::function<int(std::vector<int> &)>>
//     func_map;

namespace Funcs {
namespace ops {
static auto nullexp = [](std::vector<int> &a) { return INT_MIN; };
static auto neg = [](std::vector<int> &a) { return 0 - a[0]; };
static auto abs = [](std::vector<int> &a) { return std::abs(a[0]); };
static auto add = [](std::vector<int> &a) {
  return accumulate(a.begin(), a.end(), 0);
};
static auto sub = [](std::vector<int> &a) { return a[0] - a[1]; };
static auto mul = [](std::vector<int> &a) {
  return accumulate(a.begin(), a.end(), 1, std::multiplies<int>());
};
static auto div = [](std::vector<int> &a) { return a[0] / a[1]; };
static auto mod = [](std::vector<int> &a) { return a[0] % a[1]; };
static auto sqr = [](std::vector<int> &a) {
  return static_cast<int>(std::sqrt(a[0]));
};
static auto pow = [](std::vector<int> &a) { return std::pow(a[0], a[1]); };
static auto min = [](std::vector<int> &a) {
  return *min_element(a.begin(), a.end());
};
static auto max = [](std::vector<int> &a) {
  return *max_element(a.begin(), a.end());
};
static auto dist = [](std::vector<int> &a) { return std::abs(a[0] - a[1]); };

static auto le = [](std::vector<int> &a) { return a[0] <= a[1]; };
static auto lt = [](std::vector<int> &a) { return a[0] < a[1]; };
static auto ge = [](std::vector<int> &a) { return a[0] >= a[1]; };
static auto gt = [](std::vector<int> &a) { return a[0] > a[1]; };
static auto ne = [](std::vector<int> &a) { return a[0] != a[1]; };
static auto eq = [](std::vector<int> &a) {
  return std::all_of(a.begin(), a.end(), [&a](int n) { return n == a[0]; });
};

static auto op_not = [](std::vector<int> &a) { return !a[0]; };
static auto op_and = [](std::vector<int> &a) {
  return std::all_of(a.begin(), a.end(), [&a](int n) { return n && a[0]; });
};
static auto op_or = [](std::vector<int> &a) {
  return std::any_of(a.begin(), a.end(), [&a](int n) { return n || a[0]; });
};

// auto xor =[](std::vector<int>& a){return std::for_each(a.begin(),
// a.end(),[](int b))}
};  // namespace ops
static std::unordered_map<std::string, int> str_expr_map = {
    {"", INT_MIN},          {"(", INT_MIN + 1},    {")", INT_MIN + 2},
    {",", INT_MIN + 3},     {"sub", INT_MIN + 4},  {"mul", INT_MIN + 5},
    {"div", INT_MIN + 6},   {"mod", INT_MIN + 7},  {"sqr", INT_MIN + 8},
    {"pow", INT_MIN + 9},   {"min", INT_MIN + 10}, {"max", INT_MIN + 11},
    {"dist", INT_MIN + 12}, {"le", INT_MIN + 13},  {"lt", INT_MIN + 14},
    {"ge", INT_MIN + 15},   {"gt", INT_MIN + 16},  {"ne", INT_MIN + 17},
    {"eq", INT_MIN + 18},   {"not", INT_MIN + 19}, {"and", INT_MIN + 20},
    {"or", INT_MIN + 21},   {"abs", INT_MIN + 22}, {"add", INT_MIN + 23},
    {"neg", INT_MIN + 24},
};

// static const std::unordered_map<std::string, int> &get_str_expr_map(
//     std::string &str) {
//   return str_expr_map;
// };

// static const std::unordered_map<std::string, int> &get_str_expr()(
//     std::string &str) {
//   static std::unordered_map<std::string, int> str_expr_map = {
//       {"", INT_MIN},          {"(", INT_MIN + 1},    {")", INT_MIN + 2},
//       {",", INT_MIN + 3},     {"sub", INT_MIN + 4},  {"mul", INT_MIN + 5},
//       {"div", INT_MIN + 6},   {"mod", INT_MIN + 7},  {"sqr", INT_MIN + 8},
//       {"pow", INT_MIN + 9},   {"min", INT_MIN + 10}, {"max", INT_MIN + 11},
//       {"dist", INT_MIN + 12}, {"le", INT_MIN + 13},  {"lt", INT_MIN + 14},
//       {"ge", INT_MIN + 15},   {"gt", INT_MIN + 16},  {"ne", INT_MIN + 17},
//       {"eq", INT_MIN + 18},   {"not", INT_MIN + 19}, {"and", INT_MIN + 20},
//       {"or", INT_MIN + 21},   {"abs", INT_MIN + 22}, {"add", INT_MIN + 23},
//       {"neg", INT_MIN + 24},
//   };
//   return str_expr_map[str];
// };
static func_map int_expr_map = {
    {INT_MIN, ops::nullexp},     {INT_MIN + 1, ops::nullexp},
    {INT_MIN + 2, ops::nullexp}, {INT_MIN + 3, ops::nullexp},
    {INT_MIN + 4, ops::sub},     {INT_MIN + 5, ops::mul},
    {INT_MIN + 6, ops::div},     {INT_MIN + 7, ops::mod},
    {INT_MIN + 8, ops::sqr},     {INT_MIN + 9, ops::pow},
    {INT_MIN + 10, ops::min},    {INT_MIN + 11, ops::max},
    {INT_MIN + 12, ops::dist},   {INT_MIN + 13, ops::le},
    {INT_MIN + 14, ops::lt},     {INT_MIN + 15, ops::ge},
    {INT_MIN + 16, ops::gt},     {INT_MIN + 17, ops::ne},
    {INT_MIN + 18, ops::eq},     {INT_MIN + 19, ops::op_not},
    {INT_MIN + 20, ops::op_and}, {INT_MIN + 21, ops::op_or},
    {INT_MIN + 22, ops::abs},    {INT_MIN + 23, ops::add},
    {INT_MIN + 24, ops::neg},
};
// static const func_map &get_int_expr_map() {

//   return int_expr_map;
// };
}  // namespace Funcs

class HVar;
class HTab;
typedef std::vector<HVar> HVars;
typedef std::vector<HTab> HTabs;
class HModel;

class HVarNode : public Object {
 public:
  int id;
  int uid;
  std::string name;
  std::vector<int> vals;
  std::unordered_map<int, int> val_map;
  std::vector<int> anti_map;
  const int std_min = INT32_MIN;
  const int std_max = INT32_MAX;

  static HVar Make(int id, int uid, const std::string &name, int min_val,
                   int max_val);

  static HVar Make(int id, int uid, const std::string &name,
                   std::vector<int> &v);

  HVarNode(const HVarNode &other) = default;
  HVarNode &operator=(const HVarNode &other) = default;
  // 定义 != 运算符
  bool operator!=(const HVarNode &other) const { return this->id != other.id; }

  HVarNode(int id, int uid, const std::string &name, int min_val, int max_val);

  HVarNode(int id, int uid, const std::string &name, std::vector<int> &v);

  ~HVarNode() = default;

  void Show();

 private:
};

class HVar : public Shared<HVarNode> {
 public:
  HVar() = default;
  HVar(const HVar &other) : Shared(other.p_) {}
  explicit HVar(HVarNode *p) : Shared(p) {}
  void operator=(const HVar &other) {
    *static_cast<Shared<HVarNode> *>(this) =
        *static_cast<const Shared<HVarNode> *>(&other);
  }
  // 定义 != 运算符
  bool operator!=(const HVar &other) const {
    return this->get()->id != other.get()->id;
  }
  HVarNode *ptr() { return get(); }
  HVarNode *ptr() const { return get(); }
};

class HTabNode : public Object {
 public:
  int id;
  std::string name;
  bool semantics;
  std::vector<HVar> scope;
  std::vector<std::vector<int>> tuples;
  bool isSTD = false;
  static HTab Make(int id, bool sem, std::vector<std::vector<int>> &ts,
                   std::vector<HVar> &scp);
  static HTab Make(HTab &other, std::vector<HVar> &scp);
  // 需要添加拷贝构造函数和赋值运算符
  HTabNode(const HTabNode &other) = default;
  HTabNode(HTab &other, std::vector<HVar> &scp);
  HTabNode(int id, bool sem, std::vector<std::vector<int>> &ts,
           std::vector<HVar> &scp);
  HTabNode &operator=(const HTabNode &other) = default;

  //
  // HTabNode(HTabNode *t, std::vector<HVar *> &scp);
  //
  // int GetAllSize() const;
  //
  void GetSTDTuple(std::vector<int> &src_tuple, std::vector<int> &std_tuple);

  void GetORITuple(std::vector<int> &std_tuple, std::vector<int> &ori_tuple);

  bool SAT(std::vector<int> &t);

  bool SAT_STD(std::vector<int> &t);

  //
  // void Show();
  //
  void GetTuple(int idx, std::vector<int> &t, std::vector<int> &t_idx);

  ~HTabNode() = default;

 private:
  // 临时变量
  std::vector<int> tmp_t_;

  friend std::ostream &operator<<(std::ostream &os,
                                  const std::vector<HVar *> &a);
};

class HTab : public Shared<HTabNode> {
 public:
  HTab() = default;
  HTab(const HTab &other) : Shared(other.p_) {}
  explicit HTab(HTabNode *p) : Shared(p) {}

  void operator=(const HTab &other) {
    *static_cast<Shared<HTabNode> *>(this) =
        *static_cast<const Shared<HTabNode> *>(&other);
  }

  HTabNode *ptr() { return get(); }
  HTabNode *ptr() const { return get(); }
};

// Expr hash functor, presents how to hash an Expr
struct HVarHash {
  size_t operator()(const HVar &e) const {
    return std::hash<HVarNode *>()(e.ptr());
  }
};
// Expr equal functor, presents whether a Expr pair is equal
struct HVarEqual {
  bool operator()(const HVar &lhs, const HVar &rhs) const {
    return lhs.get() == rhs.get();
  }
};

// Expr hash functor, presents how to hash an Expr
struct HTabHash {
  size_t operator()(const HTab &e) const {
    return std::hash<HTabNode *>()(e.ptr());
  }
};
// Expr equal functor, presents whether a Expr pair is equal
struct HTabEqual {
  bool operator()(const HTab &lhs, const HTab &rhs) const {
    return lhs.get() == rhs.get();
  }
};

class HModelNode : public Object {
 public:
  // std::unordered_map<std::string, HVar> var_n_;
  friend class HModel;
  HModelNode() {}
  HModelNode(std::string &name);
  static HModel Make();
  static HModel Make(std::string &name);
  HVars &Vars() { return vars; }
  HTabs &Tabs() { return tabs; }

  HVar Vars(const int index) { return vars[index]; }
  HTab Tabs(const int index) { return tabs[index]; }
  const std::string Name() { return name_; }

  int max_domain_size() const { return mds_; }
  int max_arity() const { return mas_; };
  void show();
  int regist(std::string expr_name,
             std::function<int(std::vector<int> &)> expr);
  static int calculate(std::vector<int> &stack, std::vector<int> &params_len);
  std::vector<HTab> solution_check(std::vector<int> &sol);
  bool have_same_scope() const { return have_same_scope_; }

  int AddVar(int id, std::string name, std::vector<int> &v);
  int AddTab(const bool sem, std::vector<std::vector<int>> &ts,
             std::vector<int> &scp);
  int AddTab(const bool sem, std::vector<std::vector<int>> &ts,
             std::vector<std::string> &scp);
  int AddTab(const bool sem, std::vector<std::vector<int>> &ts,
             std::vector<HVar> &scp, const bool STD = false);
  int AddTab(const std::string expr);
  int AddTabAsPrevious(HTab &t, std::vector<std::string> &scp);

  ~HModelNode() = default;

 private:
  std::vector<HVar> vars;
  std::vector<HTab> tabs;
  std::unordered_map<HVar, std::vector<HTab>, HVarHash, HVarEqual>
      subscriptions;
  // // 两个矩阵组成的矩阵,矩阵内容为，作用在两个变量之间约束的个数
  // //.empty()表示无约束作用在该两个变量之间
  std::vector<std::vector<std::vector<int>>> neighborhoods;
  std::string name_;
  void get_postfix(const std::string expr, std::vector<int> &data,
                   std::vector<int> &params, std::vector<int> &num_op_params,
                   std::vector<HVar> &scp);
  std::tuple<ExpType, int> get_type_tuple(std::string &expr);
  static ExpType get_type(const int expr);
  void subscript(HTab tab);
  void neighbor(HTab tab);
  void get_scope(std::vector<std::string> &scp_str, std::vector<HVar> &scp);
  int get_var_id(const int id) const { return var_uid_ - MAX_VALUE - 1; }
  int generate_expr_uid() { return ++expr_id_; }
  int generate_var_uid() { return ++var_uid_; }

  static void get_STD_tuple(std::vector<int> &src_tuple,
                            std::vector<int> &std_tuple,
                            std::vector<HVar> &scp);
  static void get_ORI_Tuple(std::vector<int> &std_tuple,
                            std::vector<int> &ori_tuple,
                            std::vector<HVar> &scp);
  static void get_ori_tuple_by_index(int idx, std::vector<int> &t,
                                     const std::vector<HVar> &scp);
  static void result(int op, std::vector<int> &result, const int len);
  std::unordered_map<std::string, HVar> str_var_map_;
  std::unordered_map<int, HVar> int_var_map_;
  size_t mds_ = 0;
  size_t mas_ = 0;
  int expr_id_ = MIN_USER_OPT;
  int var_uid_ = MAX_VALUE;
  bool have_same_scope_ = false;
};

class HModel : public Shared<HModelNode> {
 public:
  HModel() = default;

  explicit HModel(HModelNode *p) : Shared(p) {}
};

}  // namespace common
}  // namespace cpim

#endif  // HMODEL_H
