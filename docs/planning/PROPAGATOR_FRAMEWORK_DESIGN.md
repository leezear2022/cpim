# Phase 2: Propagator 框架重构设计

## 概述

**目标**: 参考 OR-Tools 的设计模式，将 CPIM 的约束传播系统重构为模块化、可扩展的 Propagator 框架。

**参考**: Google OR-Tools CP-SAT 的 Propagator 架构
- `ortools/sat/integer.h` - IntegerVariable 和 IntegerTrail
- `ortools/sat/sat_solver.h` - Propagator 接口
- `ortools/sat/constraint.cc` - 约束传播器实现

---

## 当前架构的问题

### 1. 约束与传播器耦合

**现状** (src/solver/cpu/AC3bit.cpp):
```cpp
class AC3bit : public ConsistencyAlgorithm {
 public:
  ConsistencyState enforce(const std::vector<IntVar*>& vars, int level) {
    // 传播逻辑直接写在这里
    for (auto c : n_->tabs) {  // ← 直接访问全局约束表
      for (auto x : c->scope) {
        // 硬编码的传播逻辑
      }
    }
  }
};
```

**问题**：
- ❌ 约束类型硬编码（只支持 table constraints）
- ❌ 传播逻辑无法复用
- ❌ 难以添加新的约束类型
- ❌ CPU/GPU 代码重复

### 2. 域操作分散

**现状**:
```cpp
// IntVar.cpp - CPU 域操作
class IntVar {
  void RemoveValue(int value);
  void ReduceTo(int value);
  int size(int level);
};

// GModel.cu - GPU 域操作
__global__ void RemoveValueKernel(...);
__global__ void AssignValueKernel(...);
```

**问题**：
- ❌ CPU/GPU 接口不统一
- ❌ 没有统一的域抽象
- ❌ Trail 记录逻辑分散

### 3. 传播触发机制缺失

**现状**:
```cpp
// MAC.cpp - 手动管理传播队列
std::vector<IntVar*> x_evt_;
x_evt_.push_back(v_a.v());
consistent_ = ac_->enforce(x_evt_, I.size());
x_evt_.clear();
```

**问题**：
- ❌ 没有事件驱动机制
- ❌ 手动管理修改事件
- ❌ 无法区分不同类型的域修改（RemoveValue vs ReduceTo）

---

## 新框架设计

### 架构总览

```
┌─────────────────────────────────────────────────────────────┐
│                    Solver（搜索引擎）                        │
│  - 变量选择                                                  │
│  - 值选择                                                    │
│  - 回溯管理                                                  │
└────────────────┬────────────────────────────────────────────┘
                 │ 赋值事件
                 ▼
┌─────────────────────────────────────────────────────────────┐
│              PropagationEngine（传播引擎）                   │
│  - 事件队列管理                                              │
│  - Propagator 调度                                           │
│  - 不动点检测                                                │
└────────────────┬────────────────────────────────────────────┘
                 │ 通知相关 Propagators
                 ▼
┌─────────────────────────────────────────────────────────────┐
│                  Propagator（传播器）                        │
│  - AllDifferent, Table, Linear, etc.                        │
│  - 增量传播逻辑                                              │
│  - 冲突检测                                                  │
└────────────────┬────────────────────────────────────────────┘
                 │ 域修改
                 ▼
┌─────────────────────────────────────────────────────────────┐
│               DomainManager（域管理器）                      │
│  - IntegerVariable 抽象                                      │
│  - 域修改 + Trail 记录                                       │
│  - CPU/GPU 统一接口                                          │
└─────────────────────────────────────────────────────────────┘
```

---

## 核心组件设计

### 1. IntegerVariable（域抽象）

参考 OR-Tools 的 `IntegerVariable`，统一 CPU/GPU 的域操作。

#### 1.1 接口定义

```cpp
// cpim/model/integer_variable.h
namespace cpim {

// 域修改事件类型
enum class DomainEvent {
  kValueRemoved,      // 移除单个值
  kBoundsChanged,     // 边界变化
  kDomainReduced,     // 域大小变化
  kAssigned           // 变量被赋值
};

// IntegerVariable 抽象接口
class IntegerVariable {
 public:
  virtual ~IntegerVariable() = default;

  // 基本查询
  virtual int id() const = 0;
  virtual int min() const = 0;
  virtual int max() const = 0;
  virtual int size() const = 0;
  virtual bool is_assigned() const = 0;
  virtual bool contains(int value) const = 0;

  // 域修改（触发事件）
  virtual bool RemoveValue(int value) = 0;
  virtual bool SetMin(int new_min) = 0;
  virtual bool SetMax(int new_max) = 0;
  virtual bool Assign(int value) = 0;

  // Trail 支持
  virtual void SaveState() = 0;
  virtual void RestoreState() = 0;

  // 迭代器（支持范围遍历）
  virtual absl::Span<const int> values() const = 0;
};

// CPU 实现
class CPUIntegerVariable : public IntegerVariable {
 public:
  explicit CPUIntegerVariable(int id, absl::Span<const int> domain,
                              UnifiedTrail* trail);

  bool RemoveValue(int value) override;
  bool Assign(int value) override;
  // ... 其他接口实现

 private:
  int id_;
  std::vector<uint32_t> bitset_;  // 位集合表示域
  UnifiedTrail* trail_;           // Trail 回溯
  EventNotifier* notifier_;       // 事件通知器
};

// GPU 实现
class GPUIntegerVariable : public IntegerVariable {
 public:
  explicit GPUIntegerVariable(int id, GModel* gmodel, int var_offset);

  bool RemoveValue(int value) override;
  bool Assign(int value) override;
  // ... 通过 GModel 代理到 GPU

 private:
  int id_;
  GModel* gmodel_;
  int var_offset_;
};

}  // namespace cpim
```

#### 1.2 事件通知机制

```cpp
// cpim/model/event_notifier.h
namespace cpim {

class EventNotifier {
 public:
  // 注册监听器
  void RegisterPropagator(IntegerVariable* var, DomainEvent event,
                          Propagator* propagator);

  // 触发事件
  void NotifyDomainChange(IntegerVariable* var, DomainEvent event);

  // 获取待传播的 propagators
  absl::Span<Propagator*> GetAffectedPropagators();

 private:
  // var_id → (event → propagators)
  absl::flat_hash_map<int,
                      absl::flat_hash_map<DomainEvent,
                                          std::vector<Propagator*>>> watchers_;
};

}  // namespace cpim
```

---

### 2. Propagator（传播器接口）

参考 OR-Tools 的 `PropagatorInterface`。

#### 2.1 基础接口

```cpp
// cpim/solver/propagator.h
namespace cpim {

// 传播结果
enum class PropagationStatus {
  kNoChange,      // 域未修改
  kDomainChanged, // 域已修改，可能需要继续传播
  kInfeasible     // 检测到冲突
};

// Propagator 抽象接口
class Propagator {
 public:
  virtual ~Propagator() = default;

  // 增量传播（只处理修改的变量）
  virtual PropagationStatus Propagate() = 0;

  // 初始传播（处理所有变量）
  virtual PropagationStatus InitialPropagate() = 0;

  // 注册监听的变量和事件
  virtual void RegisterWatchers(EventNotifier* notifier) = 0;

  // 优先级（用于调度）
  virtual int Priority() const { return 0; }

  // 调试信息
  virtual std::string DebugString() const = 0;
};

}  // namespace cpim
```

#### 2.2 示例实现：AllDifferent

```cpp
// cpim/solver/propagators/all_different.h
namespace cpim {

class AllDifferentPropagator : public Propagator {
 public:
  explicit AllDifferentPropagator(absl::Span<IntegerVariable*> vars)
      : vars_(vars.begin(), vars.end()) {}

  PropagationStatus Propagate() override {
    // AC 传播逻辑
    bool changed = false;

    // 对于已赋值的变量，从其他变量的域中移除该值
    for (auto* var : vars_) {
      if (var->is_assigned()) {
        int assigned_value = var->min();
        for (auto* other : vars_) {
          if (other != var && other->contains(assigned_value)) {
            if (!other->RemoveValue(assigned_value)) {
              return PropagationStatus::kInfeasible;
            }
            changed = true;
          }
        }
      }
    }

    return changed ? PropagationStatus::kDomainChanged
                   : PropagationStatus::kNoChange;
  }

  PropagationStatus InitialPropagate() override {
    return Propagate();
  }

  void RegisterWatchers(EventNotifier* notifier) override {
    // 监听所有变量的赋值事件
    for (auto* var : vars_) {
      notifier->RegisterPropagator(var, DomainEvent::kAssigned, this);
    }
  }

  std::string DebugString() const override {
    return absl::StrCat("AllDifferent(", vars_.size(), " vars)");
  }

 private:
  std::vector<IntegerVariable*> vars_;
};

}  // namespace cpim
```

#### 2.3 示例实现：TableConstraint

```cpp
// cpim/solver/propagators/table_constraint.h
namespace cpim {

class TableConstraintPropagator : public Propagator {
 public:
  TableConstraintPropagator(absl::Span<IntegerVariable*> scope,
                            absl::Span<const std::vector<int>> tuples)
      : scope_(scope.begin(), scope.end()),
        tuples_(tuples.begin(), tuples.end()) {
    // 构建支持数据结构（bitSup）
    BuildBitSup();
  }

  PropagationStatus Propagate() override {
    // GAC 传播（CT/STR2/MDD 等算法）
    bool changed = false;

    for (size_t i = 0; i < scope_.size(); ++i) {
      auto* var = scope_[i];
      for (int value = var->min(); value <= var->max(); ++value) {
        if (!var->contains(value)) continue;

        // 检查是否有支持
        if (!HasSupport(i, value)) {
          if (!var->RemoveValue(value)) {
            return PropagationStatus::kInfeasible;
          }
          changed = true;
        }
      }
    }

    return changed ? PropagationStatus::kDomainChanged
                   : PropagationStatus::kNoChange;
  }

  void RegisterWatchers(EventNotifier* notifier) override {
    // 监听所有变量的域修改事件
    for (auto* var : scope_) {
      notifier->RegisterPropagator(var, DomainEvent::kValueRemoved, this);
      notifier->RegisterPropagator(var, DomainEvent::kAssigned, this);
    }
  }

 private:
  void BuildBitSup();
  bool HasSupport(int var_idx, int value) const;

  std::vector<IntegerVariable*> scope_;
  std::vector<std::vector<int>> tuples_;
  // bitSup 数据结构
};

}  // namespace cpim
```

---

### 3. PropagationEngine（传播引擎）

参考 OR-Tools 的 `SatPropagator`。

#### 3.1 接口设计

```cpp
// cpim/solver/propagation_engine.h
namespace cpim {

class PropagationEngine {
 public:
  explicit PropagationEngine(EventNotifier* notifier);

  // 添加 Propagator
  void AddPropagator(std::unique_ptr<Propagator> propagator);

  // 执行传播（不动点计算）
  PropagationStatus Propagate();

  // 初始传播（在搜索开始前）
  PropagationStatus InitialPropagate();

  // 统计信息
  struct Stats {
    int64_t num_propagations = 0;
    int64_t num_conflicts = 0;
    int64_t num_domain_changes = 0;
  };
  const Stats& GetStats() const { return stats_; }

 private:
  EventNotifier* notifier_;
  std::vector<std::unique_ptr<Propagator>> propagators_;

  // 优先级队列（高优先级 propagator 先执行）
  std::priority_queue<Propagator*> propagation_queue_;

  Stats stats_;
};

}  // namespace cpim
```

#### 3.2 实现逻辑

```cpp
// cpim/solver/propagation_engine.cpp
namespace cpim {

PropagationStatus PropagationEngine::Propagate() {
  while (!propagation_queue_.empty()) {
    Propagator* prop = propagation_queue_.top();
    propagation_queue_.pop();

    ++stats_.num_propagations;

    PropagationStatus status = prop->Propagate();

    if (status == PropagationStatus::kInfeasible) {
      ++stats_.num_conflicts;
      return PropagationStatus::kInfeasible;
    }

    if (status == PropagationStatus::kDomainChanged) {
      ++stats_.num_domain_changes;

      // 获取受影响的 propagators 并加入队列
      for (Propagator* affected : notifier_->GetAffectedPropagators()) {
        propagation_queue_.push(affected);
      }
    }
  }

  return PropagationStatus::kNoChange;
}

}  // namespace cpim
```

---

### 4. CPU/GPU 统一抽象

#### 4.1 DomainManager（统一域管理）

```cpp
// cpim/model/domain_manager.h
namespace cpim {

// 域管理器抽象接口
class DomainManager {
 public:
  virtual ~DomainManager() = default;

  // 创建变量
  virtual IntegerVariable* NewIntVar(int id, absl::Span<const int> domain) = 0;

  // 批量操作（GPU 优化）
  virtual PropagationStatus BatchRemoveValues(
      absl::Span<const std::pair<int, int>> var_value_pairs) = 0;

  // Trail 管理
  virtual void NewLevel() = 0;
  virtual void BacktrackTo(int level) = 0;

  // 获取所有变量
  virtual absl::Span<IntegerVariable*> variables() = 0;
};

// CPU 实现
class CPUDomainManager : public DomainManager {
 public:
  explicit CPUDomainManager(UnifiedTrail* trail);

  IntegerVariable* NewIntVar(int id, absl::Span<const int> domain) override;

  PropagationStatus BatchRemoveValues(...) override {
    // 串行处理
    for (auto [var_id, value] : var_value_pairs) {
      if (!variables_[var_id]->RemoveValue(value)) {
        return PropagationStatus::kInfeasible;
      }
    }
    return PropagationStatus::kDomainChanged;
  }

 private:
  UnifiedTrail* trail_;
  std::vector<std::unique_ptr<CPUIntegerVariable>> variables_;
};

// GPU 实现
class GPUDomainManager : public DomainManager {
 public:
  explicit GPUDomainManager(GModel* gmodel);

  IntegerVariable* NewIntVar(int id, absl::Span<const int> domain) override;

  PropagationStatus BatchRemoveValues(...) override {
    // 批量提交到 GPU
    cudaMemcpy(d_operations_, var_value_pairs.data(), ...);
    BatchRemoveValuesKernel<<<...>>>(d_bitDom_, d_operations_, ...);
    cudaDeviceSynchronize();
    // 检查冲突
  }

 private:
  GModel* gmodel_;
  std::vector<std::unique_ptr<GPUIntegerVariable>> variables_;
};

}  // namespace cpim
```

#### 4.2 GPU Propagator 适配器

```cpp
// cpim/solver/gpu/gpu_propagator_adapter.h
namespace cpim {

// 将 CPU Propagator 包装为 GPU 批量操作
class GPUPropagatorAdapter : public Propagator {
 public:
  explicit GPUPropagatorAdapter(std::unique_ptr<Propagator> cpu_propagator,
                                GPUDomainManager* gpu_manager)
      : cpu_propagator_(std::move(cpu_propagator)),
        gpu_manager_(gpu_manager) {}

  PropagationStatus Propagate() override {
    // 1. 拷贝当前域状态到 CPU
    SyncGPUToCPU();

    // 2. 在 CPU 上执行传播逻辑
    PropagationStatus status = cpu_propagator_->Propagate();

    if (status == PropagationStatus::kDomainChanged) {
      // 3. 批量提交修改到 GPU
      BatchSyncCPUToGPU();
    }

    return status;
  }

 private:
  void SyncGPUToCPU();
  void BatchSyncCPUToGPU();

  std::unique_ptr<Propagator> cpu_propagator_;
  GPUDomainManager* gpu_manager_;
};

}  // namespace cpim
```

---

## 迁移计划

### Phase 2.1: 基础框架（2-3周）

#### 任务清单

- [ ] **2.1.1 定义核心接口**
  - IntegerVariable 接口
  - Propagator 接口
  - DomainManager 接口
  - 时间：3-4 天

- [ ] **2.1.2 实现 CPUIntegerVariable**
  - 基于位集合的域表示
  - Trail 集成
  - 事件通知
  - 时间：3-4 天

- [ ] **2.1.3 实现 EventNotifier**
  - Watcher 注册
  - 事件触发
  - 队列管理
  - 时间：2-3 天

- [ ] **2.1.4 实现 PropagationEngine**
  - 不动点计算
  - 优先级队列
  - 统计信息
  - 时间：3-4 天

**验收标准**：
- 核心接口编译通过
- CPUIntegerVariable 单元测试通过
- 简单的 AllDifferent propagator 可以工作

---

### Phase 2.2: Propagator 实现（3-4周）

#### 任务清单

- [ ] **2.2.1 AllDifferent Propagator**
  - AC 传播
  - 事件驱动优化
  - 时间：2-3 天

- [ ] **2.2.2 TableConstraint Propagator**
  - GAC 算法（STR2/CT）
  - bitSup 数据结构
  - 时间：5-7 天

- [ ] **2.2.3 Linear Propagator**
  - 线性约束传播
  - 边界传播
  - 时间：3-4 天

- [ ] **2.2.4 Global Constraints**
  - Element
  - Cumulative（可选）
  - 时间：每个 2-3 天

**验收标准**：
- 每个 propagator 有单元测试
- 在小型问题上验证正确性
- 性能不低于现有实现

---

### Phase 2.3: 现有代码迁移（2-3周）

#### 任务清单

- [ ] **2.3.1 重构 MAC 搜索器**
  - 使用新的 IntegerVariable 接口
  - 使用 PropagationEngine
  - 时间：3-4 天

- [ ] **2.3.2 迁移 AC3bit**
  - 转换为 Propagator 实现
  - 保持性能
  - 时间：2-3 天

- [ ] **2.3.3 迁移约束解析**
  - XcspParser → 新框架
  - 创建对应的 Propagators
  - 时间：4-5 天

**验收标准**：
- 所有现有测试通过
- 性能无显著下降（< 10%）
- TIER 0/1 测试全部通过

---

### Phase 2.4: GPU 集成（3-4周）

#### 任务清单

- [ ] **2.4.1 GPUIntegerVariable 实现**
  - 通过 GModel 代理
  - 批量操作接口
  - 时间：3-4 天

- [ ] **2.4.2 GPUDomainManager 实现**
  - 批量域修改
  - GPU-CPU 同步
  - 时间：4-5 天

- [ ] **2.4.3 GPUPropagatorAdapter**
  - CPU Propagator → GPU 批量操作
  - 性能优化
  - 时间：5-7 天

**验收标准**：
- GPU 路径所有测试通过
- CPU/GPU 结果完全一致
- GPU 性能优于 CPU（对大问题）

---

## 预期收益

### 1. 可扩展性
- ✅ 添加新约束只需实现 Propagator 接口
- ✅ 支持用户自定义约束
- ✅ 易于集成第三方算法

### 2. 可维护性
- ✅ 清晰的模块边界
- ✅ 接口与实现分离
- ✅ 易于测试和调试

### 3. 性能
- ✅ 事件驱动减少无效传播
- ✅ 优先级队列优化传播顺序
- ✅ 批量操作优化 GPU 性能

### 4. CPU/GPU 统一
- ✅ 相同的 Propagator 逻辑
- ✅ 自动化的 GPU 适配
- ✅ 降低维护成本

---

## 示例：使用新框架求解 N-Queens

```cpp
#include "cpim/model/domain_manager.h"
#include "cpim/solver/propagation_engine.h"
#include "cpim/solver/propagators/all_different.h"
#include "cpim/solver/search.h"

using namespace cpim;

void SolveNQueens(int n) {
  // 1. 创建域管理器
  UnifiedTrail trail(1000);
  CPUDomainManager domain_manager(&trail);

  // 2. 创建变量
  std::vector<IntegerVariable*> queens;
  for (int i = 0; i < n; ++i) {
    queens.push_back(domain_manager.NewIntVar(i, {0, 1, 2, ..., n-1}));
  }

  // 3. 创建传播引擎
  EventNotifier notifier;
  PropagationEngine engine(&notifier);

  // 4. 添加 AllDifferent 约束（行不同）
  engine.AddPropagator(std::make_unique<AllDifferentPropagator>(queens));

  // 5. 添加对角线约束
  // queens[i] + i != queens[j] + j
  // queens[i] - i != queens[j] - j
  // ... (通过 Linear Propagators 实现)

  // 6. 初始传播
  if (engine.InitialPropagate() == PropagationStatus::kInfeasible) {
    std::cout << "UNSAT" << std::endl;
    return;
  }

  // 7. 搜索
  MACSearcher searcher(&domain_manager, &engine);
  if (searcher.Search()) {
    std::cout << "Solution: ";
    for (auto* var : queens) {
      std::cout << var->min() << " ";
    }
    std::cout << std::endl;
  }
}
```

---

## 与现有代码对比

### 现有方式（AC3bit + MAC）

```cpp
// 硬编码的约束类型
Network* n = new Network(...);
AC3bit* ac = new AC3bit(n);
MAC* solver = new MAC(n, AC_3bit, ...);

// 搜索
solver->enforce(time_limit);
```

**问题**：
- 只支持 table constraints
- 无法添加 AllDifferent、Linear 等约束
- CPU/GPU 代码重复

### 新方式（Propagator 框架）

```cpp
// 灵活的约束组合
DomainManager* dm = new CPUDomainManager(...);
PropagationEngine* engine = new PropagationEngine(...);

// 添加不同类型的约束
engine->AddPropagator(new AllDifferentPropagator(...));
engine->AddPropagator(new TableConstraintPropagator(...));
engine->AddPropagator(new LinearPropagator(...));

// 统一的求解接口
Searcher* searcher = new MACSearcher(dm, engine);
searcher->Search();
```

**优势**：
- 支持多种约束类型
- 用户可自定义约束
- CPU/GPU 自动适配

---

## 风险与挑战

### 1. 性能开销
**风险**: 抽象层可能带来性能损失
**缓解**:
- 使用 inline 和模板优化
- 批量操作减少虚函数调用
- 性能对比测试

### 2. 迁移成本
**风险**: 重构现有代码工作量大
**缓解**:
- 渐进式迁移
- 保留旧代码作为对比基准
- 每个阶段都有验收标准

### 3. GPU 同步开销
**风险**: CPU-GPU 数据传输成为瓶颈
**缓解**:
- 批量操作减少传输次数
- 异步传输 + 双缓冲
- 只在必要时同步

---

## 下一步行动

**立即可行**（基于当前代码）：
1. 创建 `cpim/model/integer_variable.h` 接口定义
2. 实现 `CPUIntegerVariable` 原型
3. 编写 N-Queens 示例验证设计

**需要进一步讨论**：
- Propagator 优先级策略
- GPU 批量操作的粒度
- 是否支持 Lazy Propagation

你想先从哪个部分开始？我建议从 **IntegerVariable 接口定义**开始，这是整个框架的基础。
