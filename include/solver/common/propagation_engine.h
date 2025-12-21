// PropagationEngine: 约束传播调度器
// 管理 Propagator 集合并实现不动点传播算法
//
// Phase 2.1 核心设计:
// 1. 优先级队列调度: 全局约束优先执行
// 2. 增量调度: 仅传播受影响的 Propagator
// 3. 重复检测: 避免同一 Propagator 重复入队
// 4. 无状态设计: 每次 Propagate() 调用独立，无需显式回溯

#ifndef CPIM_SOLVER_COMMON_PROPAGATION_ENGINE_H_
#define CPIM_SOLVER_COMMON_PROPAGATION_ENGINE_H_

#include "solver/common/propagator.h"
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <memory>

namespace cpim {

// PropagationEngine: 管理 Propagator 调度和执行
//
// 核心职责:
// 1. 维护 Propagator 集合
// 2. 根据域变化调度相关 Propagator
// 3. 处理不动点传播（循环直到无域变化）
//
// 使用示例:
//   PropagationEngine engine;
//
//   // 注册 Propagator
//   engine.AddPropagator(std::make_unique<TableConstraintPropagator>(...));
//   engine.AddPropagator(std::make_unique<AllDifferentPropagator>(...));
//
//   // 执行传播
//   std::vector<IntVar*> modified = {var1, var2};
//   PropagationResult result = engine.Propagate(modified, level);
//
//   if (result.state == PropagationState::INCONSISTENT) {
//     // 处理冲突
//   }
//
// 调度算法:
//   1. 收集受 modified_vars 影响的 Propagator，加入优先级队列
//   2. 循环:
//      a. 从队列中取出优先级最高的 Propagator
//      b. 执行 Propagator::Propagate()
//      c. 如果返回 INCONSISTENT，立即返回
//      d. 如果有域变化，收集相关 Propagator，加入队列
//      e. 队列为空时结束循环
//   3. 返回最终状态
class PropagationEngine {
 public:
  PropagationEngine() = default;

  // 注册 Propagator
  //
  // 必须在调用 Propagate() 前注册所有 Propagator
  // 注册后自动构建 var_to_propagators_ 映射
  //
  // 参数:
  //   propagator: Propagator 实例（转移所有权）
  void AddPropagator(std::unique_ptr<Propagator> propagator);

  // 执行传播（不动点算法）
  //
  // 参数:
  //   modified_vars: 初始被修改变量集合
  //                  通常来自搜索器的赋值操作
  //   level: 当前搜索深度（传递给 Propagator::Propagate）
  //
  // 返回:
  //   PropagationResult - 包含:
  //     - state: 最终一致性状态
  //       - CONSISTENT: 传播成功，无冲突
  //       - CHANGED: 不会返回（内部状态，不出现在最终结果）
  //       - INCONSISTENT: 检测到冲突
  //     - modified_vars: 累积的所有被修改变量
  //                      （用于搜索器更新统计信息）
  //
  // 实现细节:
  //   - 使用优先级队列（std::priority_queue）
  //   - 重复检测通过 in_queue_ 位向量实现
  //   - 冲突时立即清空队列并返回
  //   - 设计为无状态：每次调用独立，无需显式回溯
  PropagationResult Propagate(
      const std::vector<IntVar*>& modified_vars,
      int level);

  // 获取所有 Propagator（用于调试和测试）
  const std::vector<std::unique_ptr<Propagator>>& GetPropagators() const {
    return propagators_;
  }

  // 获取 Propagator 数量
  size_t GetPropagatorCount() const {
    return propagators_.size();
  }

 private:
  // 所有注册的 Propagator
  std::vector<std::unique_ptr<Propagator>> propagators_;

  // 变量 → 相关 Propagator 索引的映射
  // 用于快速查找受变量影响的 Propagator
  // 在 AddPropagator() 时构建
  std::unordered_map<IntVar*, std::vector<int>> var_to_propagators_;

  // 优先级队列元素
  struct PropagatorQueueItem {
    int propagator_idx;  // propagators_ 中的索引
    int priority;        // Propagator::Priority() 返回值

    // 优先级队列比较函数（小优先级先出队）
    bool operator>(const PropagatorQueueItem& other) const {
      return priority > other.priority;
    }
  };

  // 待执行的 Propagator 队列（按优先级排序）
  // 注意: priority_queue 默认是最大堆，使用 greater 反转为最小堆
  std::priority_queue<PropagatorQueueItem,
                      std::vector<PropagatorQueueItem>,
                      std::greater<PropagatorQueueItem>> pending_queue_;

  // 标记 Propagator 是否已在队列中（避免重复入队）
  // in_queue_[idx] = true 表示 propagators_[idx] 已在 pending_queue_ 中
  std::vector<bool> in_queue_;

  // 调度受变量影响的 Propagator
  // 将相关 Propagator 加入优先级队列
  //
  // 参数:
  //   vars: 发生域变化的变量
  //
  // 副作用:
  //   更新 pending_queue_ 和 in_queue_
  void ScheduleAffectedPropagators(const std::vector<IntVar*>& vars);
};

}  // namespace cpim

#endif  // CPIM_SOLVER_COMMON_PROPAGATION_ENGINE_H_
