// TableConstraintPropagator 实现
// 迁移 AC3/AC3bit 核心逻辑到 Propagator 框架

#include "solver/common/table_propagator.h"
#include <glog/logging.h>
#include <sstream>
#include <algorithm>

namespace cpim {

TableConstraintPropagator::TableConstraintPropagator(
    Tabular* constraint,
    Network* network)
    : constraint_(constraint),
      network_(network),
      max_bitDom_size_(network->max_bitDom_size()),
      is_binary_(constraint->arity == 2) {

  // 构建 scope 变量列表
  for (IntVar* var : constraint_->scope) {
    scope_vars_.push_back(var);
  }

  // 如果是二元约束，初始化 AC3bit 位向量
  if (is_binary_) {
    InitBitSupport();
  }

  // 初始化临时元组（用于通用 AC3 算法）
  tmp_tuple_.resize(constraint_->arity);

  VLOG(2) << "TableConstraintPropagator: Created for constraint "
          << constraint_->id() << " (arity=" << constraint_->arity
          << ", scope_size=" << scope_vars_.size()
          << ", is_binary=" << is_binary_ << ")";
}

void TableConstraintPropagator::InitBitSupport() {
  // 从 AC3bit 构造函数迁移
  // bitSup_ 大小 = tabs.size() * max_domain_size * max_arity
  const size_t size = network_->tabs.size() *
                      network_->max_domain_size() *
                      network_->max_arity();
  bitSup_.resize(size, std::vector<std::bitset<BITSIZE>>(max_bitDom_size_, 0));

  // 遍历所有元组，构建支持位向量
  for (const auto& t : constraint_->tuples()) {
    // 计算两个 IntConValIndex
    const int index0 = network_->GetIntConValIndex(
        IntConVal(constraint_, constraint_->scope[0], t[0]));
    const int index1 = network_->GetIntConValIndex(
        IntConVal(constraint_, constraint_->scope[1], t[1]));

    // 获取位索引
    auto idx0 = GetBitIdx(t[0]);
    auto idx1 = GetBitIdx(t[1]);

    // 设置支持位
    // bitSup_[index0][idx1] 表示: constraint_ + scope[0] + value=t[0] 被 t[1] 支持
    bitSup_[index0][std::get<0>(idx1)].set(std::get<1>(idx1));
    // bitSup_[index1][idx0] 表示: constraint_ + scope[1] + value=t[1] 被 t[0] 支持
    bitSup_[index1][std::get<0>(idx0)].set(std::get<1>(idx0));
  }

  VLOG(3) << "TableConstraintPropagator: Initialized AC3bit bitSup_ for "
          << constraint_->tuples().size() << " tuples";
}

std::string TableConstraintPropagator::Name() const {
  std::ostringstream oss;
  oss << "TableConstraint_" << constraint_->id();
  return oss.str();
}

PropagationResult TableConstraintPropagator::Propagate(
    const std::vector<IntVar*>& modified_vars,
    int level) {

  // 1. 快速检查: 是否有 scope 变量被修改
  bool scope_modified = false;
  for (IntVar* var : scope_vars_) {
    if (std::find(modified_vars.begin(), modified_vars.end(), var) !=
        modified_vars.end()) {
      scope_modified = true;
      break;
    }
  }

  if (!scope_modified) {
    VLOG(4) << "TableConstraintPropagator " << Name()
            << ": No scope variable modified, skipping";
    return {PropagationState::CONSISTENT, {}};
  }

  VLOG(3) << "TableConstraintPropagator " << Name()
          << ": Propagating at level " << level;

  // 2. 对每个变量执行 Revise
  std::vector<IntVar*> modified;
  for (size_t i = 0; i < scope_vars_.size(); ++i) {
    IntVar* var = scope_vars_[i];

    // 跳过已赋值的变量
    if (var->assigned()) {
      continue;
    }

    if (Revise(var, i, level)) {
      // 域发生变化
      if (var->faild()) {
        // 域清空，不一致
        VLOG(3) << "TableConstraintPropagator " << Name()
                << ": Variable " << var->id() << " domain wiped out";
        return {PropagationState::INCONSISTENT, {}};
      }
      modified.push_back(var);
    }
  }

  // 3. 返回结果
  if (modified.empty()) {
    return {PropagationState::CONSISTENT, {}};
  } else {
    VLOG(3) << "TableConstraintPropagator " << Name()
            << ": Modified " << modified.size() << " variables";
    return {PropagationState::CHANGED, modified};
  }
}

bool TableConstraintPropagator::Revise(IntVar* var, int var_idx_in_scope, int level) {
  // 从 AC3::revise 迁移
  const int num_elements = var->size();
  int a = var->head();

  int num_removed = 0;

  // 遍历变量的所有值
  while (a != Limits::INDEX_OVERFLOW) {
    // 检查是否有支持
    IntConVal c_val(constraint_, var, a);
    if (!SeekSupport(c_val, level)) {
      // 无支持，删除该值
      var->RemoveValue(a);
      ++num_removed;

      VLOG(4) << "TableConstraintPropagator " << Name()
              << ": Removed value " << a << " from variable " << var->id()
              << " (no support)";
    }
    a = var->next(a);
  }

  // 返回是否有域变化
  return num_elements != var->size();
}

bool TableConstraintPropagator::SeekSupport(const IntConVal& c_val, int level) {
  if (is_binary_) {
    // 二元约束: 使用 AC3bit 位向量优化
    // 从 AC3bit::seek_support 迁移
    const int idx = network_->GetIntConValIndex(c_val);

    // 遍历另一个变量的域
    for (IntVar* y : c_val.c()->scope) {
      if (y->id() != c_val.v()->id()) {
        // y 是另一个变量
        for (int i = 0; i < static_cast<int>(y->bitDom().size()); ++i) {
          // 检查位向量交集
          if ((bitSup_[idx][i] & y->bitDom()[i]).any()) {
            return true;  // 找到支持
          }
        }
      }
    }
    return false;  // 无支持
  } else {
    // 高元约束: 使用通用 AC3 元组遍历
    // 从 AC3::seek_support 迁移
    network_->GetFirstValidTuple(c_val, tmp_tuple_, level);

    while (Existed(tmp_tuple_)) {
      if (c_val.c()->sat(tmp_tuple_)) {
        return true;  // 找到支持
      } else {
        network_->GetNextValidTuple(c_val, tmp_tuple_, level);
      }
    }
    return false;  // 无支持
  }
}

}  // namespace cpim
