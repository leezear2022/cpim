// PropagationEngine 实现
// 基于优先级队列的增量传播调度器

#include "solver/common/propagation_engine.h"
#include "Network.h"  // 完整的 IntVar 定义
#include <glog/logging.h>
#include <algorithm>

namespace cpim {

void PropagationEngine::AddPropagator(std::unique_ptr<Propagator> propagator) {
  int idx = propagators_.size();

  // 建立变量 → Propagator 索引映射
  for (IntVar* var : propagator->GetScope()) {
    var_to_propagators_[var].push_back(idx);
  }

  VLOG(2) << "PropagationEngine: Registered Propagator #" << idx
          << " (" << propagator->Name() << ")"
          << " with priority " << propagator->Priority()
          << ", scope size = " << propagator->GetScope().size();

  propagators_.push_back(std::move(propagator));
  in_queue_.push_back(false);
}

void PropagationEngine::ScheduleAffectedPropagators(
    const std::vector<IntVar*>& vars) {
  for (IntVar* var : vars) {
    auto it = var_to_propagators_.find(var);
    if (it != var_to_propagators_.end()) {
      for (int idx : it->second) {
        if (!in_queue_[idx]) {
          int priority = propagators_[idx]->Priority();
          pending_queue_.push({idx, priority});
          in_queue_[idx] = true;

          VLOG(3) << "PropagationEngine: Scheduled Propagator #" << idx
                  << " (" << propagators_[idx]->Name() << ")"
                  << " due to change in var " << var->id();
        }
      }
    }
  }
}

PropagationResult PropagationEngine::Propagate(
    const std::vector<IntVar*>& modified_vars,
    int level) {

  VLOG(2) << "PropagationEngine::Propagate() called at level " << level
          << " with " << modified_vars.size() << " modified variables";

  // 1. 收集受影响的 Propagator，加入优先级队列
  ScheduleAffectedPropagators(modified_vars);

  // 2. 不动点传播循环
  std::vector<IntVar*> cumulative_modified;  // 累积所有被修改的变量
  int propagation_rounds = 0;

  while (!pending_queue_.empty()) {
    PropagatorQueueItem item = pending_queue_.top();
    pending_queue_.pop();
    in_queue_[item.propagator_idx] = false;

    Propagator* prop = propagators_[item.propagator_idx].get();

    VLOG(3) << "PropagationEngine: Executing Propagator #" << item.propagator_idx
            << " (" << prop->Name() << ")"
            << " with priority " << item.priority;

    // 执行传播
    PropagationResult result = prop->Propagate(cumulative_modified, level);

    ++propagation_rounds;

    // 处理传播结果
    if (result.state == PropagationState::INCONSISTENT) {
      // 不一致，清空队列并立即返回
      VLOG(2) << "PropagationEngine: Propagator " << prop->Name()
              << " detected inconsistency";

      while (!pending_queue_.empty()) {
        in_queue_[pending_queue_.top().propagator_idx] = false;
        pending_queue_.pop();
      }

      return {PropagationState::INCONSISTENT, {}};
    }

    // 如果有新的域变化，调度相关 Propagator
    if (result.state == PropagationState::CHANGED &&
        !result.modified_vars.empty()) {

      VLOG(3) << "PropagationEngine: Propagator " << prop->Name()
              << " modified " << result.modified_vars.size() << " variables";

      // 累积修改的变量（用于返回给调用者）
      cumulative_modified.insert(cumulative_modified.end(),
                                  result.modified_vars.begin(),
                                  result.modified_vars.end());

      // 调度受影响的 Propagator
      ScheduleAffectedPropagators(result.modified_vars);
    }
  }

  VLOG(2) << "PropagationEngine: Fixed point reached after "
          << propagation_rounds << " propagation rounds, "
          << cumulative_modified.size() << " variables modified";

  // 3. 返回最终状态
  // 注意: 如果 cumulative_modified 非空，说明有域变化，但最终是一致的
  // 我们统一返回 CONSISTENT，由调用者根据 cumulative_modified 判断是否有变化
  return {PropagationState::CONSISTENT, cumulative_modified};
}

}  // namespace cpim
