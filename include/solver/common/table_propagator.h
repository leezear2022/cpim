// TableConstraintPropagator: 表约束传播器
// 基于 AC3/AC3bit 算法实现 GAC 传播
//
// Phase 2.1 实现策略:
// 1. 支持任意元数（arity）的表约束
// 2. 二元约束使用 AC3bit 位运算优化（保持性能）
// 3. 高元约束使用通用 AC3 算法（正确性优先）
//
// 设计决策:
// - 复用 Network::GetFirstValidTuple / GetNextValidTuple 遍历逻辑
// - 复用 Tabular::sat(tuple) 约束检查
// - 保持与 AC3bit 完全一致的位向量数据结构

#ifndef CPIM_SOLVER_COMMON_TABLE_PROPAGATOR_H_
#define CPIM_SOLVER_COMMON_TABLE_PROPAGATOR_H_

#include "solver/common/propagator.h"
#include "Network.h"  // IntVar, Constraint, Tabular
#include "Solver.h"   // BITSIZE, bitset

#include <vector>
#include <bitset>

namespace cpim {

// TableConstraintPropagator: 表约束的 Propagator 实现
//
// 核心逻辑:
//   1. 对 scope 中的每个变量执行 Revise()
//   2. Revise: 对变量的每个值检查是否有支持（support）
//   3. 无支持的值被删除（调用 IntVar::RemoveValue）
//   4. 如果域变空，返回 INCONSISTENT
//
// 优化:
//   - 二元约束: 使用 AC3bit 位向量快速检查支持
//   - 高元约束: 使用通用 AC3 元组遍历
class TableConstraintPropagator : public Propagator {
 public:
  // 构造函数
  //
  // 参数:
  //   constraint: 表约束指针（Tabular 类型）
  //   network: 变量网络（用于元组遍历）
  //
  // 注意:
  //   - constraint 和 network 的生命周期必须长于 Propagator
  //   - 构造时会初始化位向量（对于二元约束）
  TableConstraintPropagator(Tabular* constraint, Network* network);

  // 执行传播
  //
  // 算法:
  //   1. 检查 modified_vars 是否包含本约束的 scope 变量
  //   2. 如果不包含，快速返回 CONSISTENT（无需传播）
  //   3. 对 scope 中的每个变量调用 Revise()
  //   4. 如果任何变量域变空，返回 INCONSISTENT
  //   5. 返回 CHANGED（如果有域变化）或 CONSISTENT
  PropagationResult Propagate(
      const std::vector<IntVar*>& modified_vars,
      int level) override;

  std::vector<IntVar*> GetScope() const override {
    return scope_vars_;
  }

  int Priority() const override {
    return 10;  // 普通优先级（表约束）
  }

  std::string Name() const override;

 private:
  Tabular* constraint_;             // 约束指针
  Network* network_;                // 网络指针（用于元组遍历）
  std::vector<IntVar*> scope_vars_; // scope 变量列表

  // AC3bit 位向量数据（仅用于二元约束）
  // bitSup_[IntConValIndex][bitDom_idx] 表示支持的值集合
  std::vector<std::vector<std::bitset<BITSIZE>>> bitSup_;
  int max_bitDom_size_;  // 位域大小
  bool is_binary_;       // 是否为二元约束

  // 临时元组（用于通用 AC3 算法）
  std::vector<int> tmp_tuple_;

  // Revise 方法: 对变量的所有值检查支持，删除无支持的值
  //
  // 参数:
  //   var: 要传播的变量
  //   var_idx_in_scope: 变量在 scope 中的索引（0-based）
  //   level: 当前搜索深度
  //
  // 返回:
  //   true - 域发生变化
  //   false - 域未变化
  bool Revise(IntVar* var, int var_idx_in_scope, int level);

  // 检查支持: 检查约束-值对是否有支持
  //
  // 参数:
  //   c_val: 约束-值对（IntConVal）
  //   level: 当前搜索深度
  //
  // 返回:
  //   true - 有支持
  //   false - 无支持（应删除该值）
  //
  // 实现:
  //   - 二元约束: 使用 bitSup_ 位向量快速检查
  //   - 高元约束: 遍历所有有效元组，检查 constraint_->sat(tuple)
  bool SeekSupport(const IntConVal& c_val, int level);

  // AC3bit 位向量索引计算
  // 返回值: (bitDom_idx, bit_pos)
  std::tuple<int, int> GetBitIdx(int value) const {
    return std::make_tuple(value / BITSIZE, value % BITSIZE);
  }

  // 初始化 AC3bit 位向量（仅用于二元约束）
  void InitBitSupport();
};

}  // namespace cpim

#endif  // CPIM_SOLVER_COMMON_TABLE_PROPAGATOR_H_
