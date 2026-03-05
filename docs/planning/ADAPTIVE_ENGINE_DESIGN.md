# CPIM 自适应 CPU/GPU 切换引擎设计

## 概述

自适应引擎根据问题特征和运行时状态，动态选择 CPU 或 GPU 执行策略，以最大化求解性能。

---

## 方案 1：基于问题特征的静态选择（Phase 1.3）

### 核心思想
在求解开始前，分析问题特征，选择最优执行策略。

### 决策指标

#### 1.1 问题规模指标
```cpp
struct ProblemProfile {
  int num_vars;           // 变量数量
  int num_constraints;    // 约束数量
  int max_domain_size;    // 最大域大小
  int avg_arity;          // 平均约束元数
  double tightness;       // 约束紧度估计

  // 复杂度估计
  int64_t search_space_estimate() const {
    return std::pow(max_domain_size, num_vars);
  }

  int64_t propagation_complexity() const {
    return num_constraints * avg_arity * max_domain_size;
  }
};
```

#### 1.2 决策规则（启发式）

| 指标 | CPU 适用场景 | GPU 适用场景 |
|------|-------------|-------------|
| **变量数** | < 50 | > 100 |
| **约束数** | < 100 | > 500 |
| **域大小** | < 20 | > 50 |
| **约束紧度** | > 0.8（稀疏） | < 0.3（稠密） |
| **传播复杂度** | < 10,000 | > 100,000 |

**决策公式**：
```cpp
enum class ExecutionMode {
  CPU_ONLY,
  GPU_ONLY,
  HYBRID
};

ExecutionMode SelectMode(const ProblemProfile& profile) {
  // 小问题 → CPU（避免 GPU 启动开销）
  if (profile.num_vars < 50 && profile.num_constraints < 100) {
    return ExecutionMode::CPU_ONLY;
  }

  // 大问题 + 高传播复杂度 → GPU
  if (profile.num_vars > 100 &&
      profile.propagation_complexity() > 100000) {
    return ExecutionMode::GPU_ONLY;
  }

  // 中等规模 → 混合模式
  return ExecutionMode::HYBRID;
}
```

### 实现步骤

#### Step 1: 添加问题分析器
```cpp
// src/solver/common/problem_profiler.h
namespace cpim {

class ProblemProfiler {
 public:
  static ProblemProfile Analyze(const IntermediateModel& model) {
    ProblemProfile profile;
    profile.num_vars = model.num_variables();
    profile.num_constraints = model.num_constraints();
    profile.max_domain_size = ComputeMaxDomainSize(model);
    profile.avg_arity = ComputeAvgArity(model);
    profile.tightness = EstimateTightness(model);
    return profile;
  }

 private:
  static int ComputeMaxDomainSize(const IntermediateModel& model);
  static double ComputeAvgArity(const IntermediateModel& model);
  static double EstimateTightness(const IntermediateModel& model);
};

}  // namespace cpim
```

#### Step 2: 添加执行策略选择器
```cpp
// src/solver/common/execution_strategy.h
namespace cpim {

class ExecutionStrategy {
 public:
  static ExecutionMode SelectMode(const ProblemProfile& profile) {
    // 实现决策逻辑
    if (ShouldUseCPU(profile)) return ExecutionMode::CPU_ONLY;
    if (ShouldUseGPU(profile)) return ExecutionMode::GPU_ONLY;
    return ExecutionMode::HYBRID;
  }

 private:
  static bool ShouldUseCPU(const ProblemProfile& profile);
  static bool ShouldUseGPU(const ProblemProfile& profile);
};

}  // namespace cpim
```

#### Step 3: 修改主求解器入口
```cpp
// apps/cpim_adaptive_solver.cpp
int main(int argc, char* argv[]) {
  // 解析输入
  auto model = ParseXCSP(argv[1]);

  // 分析问题特征
  auto profile = ProblemProfiler::Analyze(model);
  LOG(INFO) << "Problem profile: vars=" << profile.num_vars
            << ", constraints=" << profile.num_constraints;

  // 选择执行模式
  auto mode = ExecutionStrategy::SelectMode(profile);
  LOG(INFO) << "Selected execution mode: " << ModeToString(mode);

  // 根据模式创建求解器
  std::unique_ptr<Solver> solver;
  switch (mode) {
    case ExecutionMode::CPU_ONLY:
      solver = std::make_unique<CPUSolver>(model);
      break;
    case ExecutionMode::GPU_ONLY:
      solver = std::make_unique<GPUSolver>(model);
      break;
    case ExecutionMode::HYBRID:
      solver = std::make_unique<HybridSolver>(model);
      break;
  }

  // 求解
  auto result = solver->Solve();
  return result.status == SolverStatus::SAT ? 0 : 1;
}
```

---

## 方案 2：混合执行模式（Phase 2.x）

### 核心思想
搜索在 CPU，传播在 GPU，充分利用两者优势。

### 架构设计

```
┌─────────────────────────────────────────┐
│         主搜索循环（CPU）                │
│  - 变量选择（启发式）                    │
│  - 值选择                                │
│  - 回溯决策                              │
└──────────┬──────────────────────────────┘
           │ 赋值事件
           ▼
┌─────────────────────────────────────────┐
│      约束传播（GPU）                     │
│  - 并行 GAC 传播                         │
│  - 域更新                                │
│  - 冲突检测                              │
└──────────┬──────────────────────────────┘
           │ 传播结果
           ▼
┌─────────────────────────────────────────┐
│         CPU 继续搜索                     │
└─────────────────────────────────────────┘
```

### 关键技术

#### 2.1 异步传播管线
```cpp
class HybridSolver {
 public:
  SearchResult Solve() {
    // CPU 搜索线程
    std::thread search_thread([this]() {
      while (!finished_) {
        // 选择变量和值
        auto assignment = SelectAssignment();

        // 提交到 GPU 传播队列
        propagation_queue_.Push(assignment);

        // 等待传播结果
        auto result = propagation_queue_.Pop();

        if (result.consistent) {
          // 继续搜索
        } else {
          // 回溯
        }
      }
    });

    // GPU 传播线程
    std::thread propagation_thread([this]() {
      while (!finished_) {
        auto assignment = propagation_queue_.Pop();

        // 在 GPU 上执行传播
        auto result = gpu_propagator_->Propagate(assignment);

        // 返回结果
        propagation_queue_.Push(result);
      }
    });

    search_thread.join();
    propagation_thread.join();
  }

 private:
  ThreadSafeQueue<Assignment> propagation_queue_;
  std::unique_ptr<GPUPropagator> gpu_propagator_;
};
```

#### 2.2 GPU 传播器接口
```cpp
class GPUPropagator {
 public:
  PropagationResult Propagate(const Assignment& assignment) {
    // 拷贝赋值到 GPU
    cudaMemcpy(d_assignment_, &assignment, ...);

    // 启动 GAC kernel
    EnforceGAC_Kernel<<<blocks, threads>>>(
        d_bitDom_, d_constraints_, d_assignment_);

    // 同步并拷贝结果
    cudaDeviceSynchronize();
    cudaMemcpy(&result, d_result_, ...);

    return result;
  }
};
```

---

## 方案 3：运行时自适应切换（Phase 3.x）

### 核心思想
在求解过程中，根据运行时性能动态切换执行模式。

### 监控指标
```cpp
struct RuntimeMetrics {
  double cpu_propagation_time_ms;
  double gpu_propagation_time_ms;
  int search_depth;
  int num_backtracks;
  int propagations_per_second;

  // 决策：是否应该切换到 GPU？
  bool ShouldSwitchToGPU() const {
    // 如果 CPU 传播变慢，切换到 GPU
    return cpu_propagation_time_ms > 100.0 &&
           search_depth > 10;
  }

  // 决策：是否应该切换回 CPU？
  bool ShouldSwitchToCPU() const {
    // 如果搜索空间收缩，切换回 CPU
    return propagations_per_second < 1000 &&
           num_backtracks > 100;
  }
};
```

### 动态切换逻辑
```cpp
class AdaptiveSolver {
 public:
  SearchResult Solve() {
    ExecutionMode current_mode = initial_mode_;
    RuntimeMetrics metrics;

    while (!finished_) {
      // 执行一轮迭代
      PerformIteration(current_mode, &metrics);

      // 定期评估是否切换模式（每 N 次迭代）
      if (++iteration_count_ % EVALUATION_INTERVAL == 0) {
        ExecutionMode new_mode = EvaluateMode(metrics);
        if (new_mode != current_mode) {
          LOG(INFO) << "Switching from " << current_mode
                    << " to " << new_mode;
          SwitchMode(current_mode, new_mode);
          current_mode = new_mode;
        }
      }
    }
  }

 private:
  void SwitchMode(ExecutionMode from, ExecutionMode to) {
    // 同步状态（domains, trail, etc.）
    if (to == ExecutionMode::GPU_ONLY) {
      SyncCPUToGPU();
    } else if (from == ExecutionMode::GPU_ONLY) {
      SyncGPUToCPU();
    }
  }
};
```

---

## 实施优先级建议

### Phase 1.3：基础自适应（1-2周）
- ✅ 实现 ProblemProfiler
- ✅ 实现静态决策规则
- ✅ 创建 cpim_adaptive_solver 应用
- ✅ 在 TIER 0/1 测试集上验证

### Phase 2.x：混合执行（3-4周）
- ⏸ 实现异步传播管线
- ⏸ 优化 CPU-GPU 数据传输
- ⏸ 性能对比测试

### Phase 3.x：运行时自适应（4-6周）
- ⏸ 添加运行时监控
- ⏸ 实现动态切换机制
- ⏸ 机器学习辅助决策（可选）

---

## 测试与验证

### 基准测试
```bash
# 对比三种模式的性能
./scripts/benchmark_adaptive.sh --tier=1

# 输出格式：
# Instance              CPU      GPU    Hybrid   Best
# queens-12_ext        0.5s     2.1s    0.8s     CPU
# langford-3-11       15.2s     3.4s    4.1s     GPU
# composed-25-1-2     TIMEOUT  12.3s   11.8s    Hybrid
```

### 决策日志
```
[Profiler] queens-12_ext: vars=12, constraints=22, max_dom=12
[Strategy] Propagation complexity: 5,280 → CPU_ONLY
[Solver] Using CPU solver (MAC + AC3bit)
[Result] SAT in 0.53s (P=1023, N=981)

[Profiler] langford-3-11: vars=33, constraints=72, max_dom=33
[Strategy] Propagation complexity: 78,408 → GPU_ONLY
[Solver] Using GPU solver (GModel + cuGAC)
[Result] SAT in 3.41s (P=18234, N=17102)
```

---

## 下一步行动

**立即可行**（基于当前代码）：
1. 创建 `src/solver/common/problem_profiler.cpp`
2. 实现基本的启发式决策规则
3. 编写 `apps/cpim_adaptive_solver.cpp`
4. 在 TIER 0/1 测试集上验证正确性

**需要进一步开发**：
- 混合模式需要重构 GPU 传播器接口
- 运行时切换需要实现状态同步机制

你想先实现哪个方案？我建议从**方案 1（静态选择）**开始，它最容易实现且能带来立竿见影的效果。
