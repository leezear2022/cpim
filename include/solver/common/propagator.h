// Propagator 抽象接口
// 支持可插拔的约束传播策略
//
// Phase 2.1 设计决策:
// 1. PropagationState 枚举: 区分一致、变化、不一致三种状态
// 2. PropagationResult 结构: 包含状态和被修改变量列表
// 3. Propagator 接口: 轻量级（仅 4 个虚函数）
// 4. 优先级支持: 全局约束可以优先执行

#ifndef CPIM_SOLVER_COMMON_PROPAGATOR_H_
#define CPIM_SOLVER_COMMON_PROPAGATOR_H_

#include <vector>
#include <string>

namespace cpim {

// 前向声明
class IntVar;

// 传播状态枚举
enum class PropagationState {
  CONSISTENT,    // 一致，无域变化
  CHANGED,       // 一致，但有域变化
  INCONSISTENT   // 不一致，域清空或冲突
};

// 传播结果
// 包含一致性状态和被修改的变量列表
struct PropagationResult {
  PropagationState state;
  std::vector<IntVar*> modified_vars;

  // 便利构造函数
  PropagationResult(PropagationState s, const std::vector<IntVar*>& vars)
      : state(s), modified_vars(vars) {}

  PropagationResult(PropagationState s) : state(s) {}
};

// Propagator 抽象基类
// 封装约束传播逻辑，支持可插拔的传播算法
//
// 使用示例:
//   class TableConstraintPropagator : public Propagator {
//     PropagationResult Propagate(...) override {
//       // 实现 AC3bit 风格的传播逻辑
//     }
//   };
class Propagator {
 public:
  virtual ~Propagator() = default;

  // 执行传播
  //
  // 参数:
  //   modified_vars: 自上次传播以来发生域变化的变量
  //                  PropagationEngine 根据这个列表调度相关 Propagator
  //   level: 当前搜索深度（用于回溯，传递给 IntVar::RemoveValue）
  //
  // 返回:
  //   PropagationResult - 包含:
  //     - state: 传播后的一致性状态
  //       - CONSISTENT: 无域变化
  //       - CHANGED: 有域变化（返回被修改的变量）
  //       - INCONSISTENT: 检测到冲突（域清空）
  //     - modified_vars: 被此次传播修改的变量列表
  //                      用于 PropagationEngine 调度后续传播
  //
  // 实现注意事项:
  //   - 应检查 modified_vars 中是否包含本 Propagator 的 scope 变量
  //   - 如果 scope 未受影响，应快速返回 CONSISTENT（避免冗余计算）
  //   - 域修改通过 IntVar::RemoveValue() 完成（自动记录到 Trail）
  //   - 不应捕获异常，失败通过返回 INCONSISTENT 表示
  virtual PropagationResult Propagate(
      const std::vector<IntVar*>& modified_vars,
      int level) = 0;

  // 返回 Propagator 的作用域变量
  //
  // 用于 PropagationEngine 构建 var_to_propagators_ 映射
  // 当这些变量的域发生变化时，PropagationEngine 会调度此 Propagator
  //
  // 返回值应稳定（不随传播过程变化）
  virtual std::vector<IntVar*> GetScope() const = 0;

  // 返回 Propagator 的优先级（数值越小越先执行）
  //
  // 默认值: 10（普通优先级）
  // 推荐值:
  //   - AllDifferent, Element 等全局约束: 5（高优先级）
  //   - 表约束（TableConstraintPropagator）: 10（普通优先级）
  //   - 松弛约束: 15（低优先级）
  //
  // 设计理由:
  //   全局约束通常能删除更多值，优先执行可减少后续冗余传播
  virtual int Priority() const { return 10; }

  // 返回 Propagator 的名称（用于调试和日志）
  //
  // 建议格式: "ConstraintType_ID"
  // 例如: "TableConstraint_42", "AllDifferent_Global"
  virtual std::string Name() const = 0;

  // 可选: 通知赋值事件（用于动态数据结构更新）
  // Phase 2.1 暂不使用，Phase 2.2 添加事件驱动传播时启用
  virtual void OnAssignment(IntVar* var, int value, int level) {}

  // 可选: 通知回溯事件（用于状态恢复）
  // 注意: 大多数 Propagator 无需实现此方法
  // IntVar 的域回溯由 UnifiedTrail 自动处理
  virtual void OnBacktrack(int level) {}
};

}  // namespace cpim

#endif  // CPIM_SOLVER_COMMON_PROPAGATOR_H_
