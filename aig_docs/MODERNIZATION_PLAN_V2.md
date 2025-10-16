# CPIM 项目现代化实施计划 V2.0
## 基于 Google OR-Tools 最佳实践修订版

> 本计划参考了 Google OR-Tools 的架构设计、代码组织和现代 C++ 实践

---

## 参考基准：Google OR-Tools 核心实践

### OR-Tools 的优秀设计模式

1. **模块化架构**
   - Protocol Buffers 定义模型接口（`cp_model.proto`）
   - 依赖注入框架（`model.h`）实现松耦合
   - SAT/CP/LP 各自独立但可组合

2. **现代 C++ 生态**
   - 深度集成 Abseil 库（`absl::Span`, `absl::Status`, `absl::flat_hash_map`）
   - C++17/20 特性全面应用
   - 智能指针和 RAII 无处不在

3. **流式 API 设计**
   ```cpp
   // OR-Tools 风格
   CpModelBuilder model;
   IntVar x = model.NewIntVar(Domain(0, 10)).WithName("x");
   model.AddEquality(x + y, 10);
   ```

4. **构建系统**
   - CMake + Bazel 双构建系统支持
   - 模块化依赖管理
   - 可选择性编译特性

5. **测试文化**
   - 每个模块对应 `*_test.cc`
   - GTest 框架
   - 持续集成

---

## CPIM 现代化路线图（修订版）

### 🎯 总体目标

将 CPIM 改造为类似 OR-Tools 的**现代化约束求解器库**：
- **库优先**：核心功能作为库，可被其他程序调用
- **API 优雅**：流式接口，类型安全，易于使用
- **模块化**：清晰的模块边界，可独立测试
- **高性能**：零开销抽象，内存安全
- **可扩展**：支持插件式算法和启发式

---

## 阶段 0：技术栈升级（1周，预备工作）

**目标**：引入 Abseil 库，建立现代 C++ 基础设施

### 任务清单

- [ ] **0.1 集成 Abseil 库**
  ```cmake
  # CMakeLists.txt
  find_package(absl REQUIRED)

  target_link_libraries(cpim_core PUBLIC
      absl::flat_hash_map
      absl::flat_hash_set
      absl::span
      absl::status
      absl::statusor
      absl::strings
      absl::time
  )
  ```
  - 通过 vcpkg 或系统包管理器安装 Abseil
  - 验证编译通过
  - 时间：2-3小时

- [ ] **0.2 升级到 C++20**
  ```cmake
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  ```
  - 确保编译器支持（GCC 10+, Clang 13+, MSVC 19.29+）
  - 时间：1小时

- [ ] **0.3 引入 GoogleTest**
  ```cmake
  find_package(GTest REQUIRED)
  enable_testing()
  ```
  - 建立 `tests/` 目录结构
  - 编写第一个测试验证环境
  - 时间：2-3小时

**验收标准**：
- Abseil 库可用，示例代码编译通过
- GoogleTest 框架就位

---

## 阶段 1：模块化重构（3-4周）

**目标**：将 CPIM 拆分为清晰的模块，参考 OR-Tools 的目录结构

### 新的目录结构

```
cpim/
├── cpim/                   # 核心库（参考 ortools/sat/）
│   ├── base/               # 基础设施
│   │   ├── types.h         # 基础类型定义
│   │   ├── bitset.h        # 位集合操作
│   │   ├── trail.h         # 回溯系统
│   │   └── logging.h       # 日志抽象
│   ├── model/              # 模型层
│   │   ├── variable.h      # IntVar, BoolVar
│   │   ├── constraint.h    # 约束接口
│   │   ├── model.h         # Problem/Model 定义
│   │   └── model_builder.h # 流式 API
│   ├── solver/             # 求解器层
│   │   ├── solver.h        # Solver 接口
│   │   ├── propagator.h    # 传播器接口
│   │   ├── search.h        # 搜索引擎
│   │   └── heuristics/     # 启发式策略
│   │       ├── var_selector.h
│   │       └── val_selector.h
│   ├── algorithms/         # 算法实现
│   │   ├── ac3.h
│   │   ├── ac3_bit.h
│   │   ├── fc.h
│   │   ├── sac.h
│   │   └── rpc.h
│   ├── util/               # 工具类
│   │   ├── stats.h         # 统计信息
│   │   └── timer.h         # 计时器
│   └── proto/              # Protocol Buffers（可选）
│       └── cpim_model.proto
├── tests/                  # 单元测试
│   ├── base/
│   ├── model/
│   ├── solver/
│   └── algorithms/
├── examples/               # 示例程序
│   ├── n_queens.cc
│   ├── sudoku.cc
│   └── benchmarks/
├── parsers/                # 解析器（独立模块）
│   └── xcsp3/
└── tools/                  # 工具程序
    └── cpim_solver.cc      # 命令行求解器
```

### 任务清单

- [ ] **1.1 创建模块化目录结构**
  - 按上述结构创建目录和骨架文件
  - 时间：2-3小时

- [ ] **1.2 定义核心接口**
  ```cpp
  // cpim/model/variable.h
  namespace cpim {

  class IntVar {
  public:
      explicit IntVar(int id, absl::Span<const int> domain);

      int id() const { return id_; }
      int size(int level) const;
      bool has_value(int value, int level) const;

      absl::Span<const int> values() const;
      std::optional<int> min(int level) const;
      std::optional<int> max(int level) const;

  private:
      int id_;
      std::vector<int> domain_;
      // ... 内部实现
  };

  }  // namespace cpim
  ```
  - 时间：4-5小时

- [ ] **1.3 实现 Model 构建器（流式 API）**
  ```cpp
  // cpim/model/model_builder.h
  namespace cpim {

  class ModelBuilder {
  public:
      // 变量创建
      IntVar NewIntVar(int min, int max, absl::string_view name = "");
      IntVar NewIntVar(absl::Span<const int> values, absl::string_view name = "");

      // 约束添加
      void AddAllDifferent(absl::Span<const IntVar> vars);
      void AddEquality(IntVar var, int value);
      void AddTableConstraint(absl::Span<const IntVar> vars,
                              absl::Span<const std::vector<int>> tuples);

      // 构建模型
      std::unique_ptr<Model> Build();

  private:
      std::vector<std::unique_ptr<IntVarImpl>> vars_;
      std::vector<std::unique_ptr<ConstraintImpl>> constraints_;
  };

  }  // namespace cpim
  ```
  - 时间：6-8小时

- [ ] **1.4 重构现有代码到新结构**
  - 将 `src/Network.cpp` 拆分到 `cpim/model/` 和 `cpim/base/`
  - 将 `src/Solver.cpp` 拆分到 `cpim/solver/` 和 `cpim/algorithms/`
  - 时间：10-12小时

- [ ] **1.5 编写模块测试**
  ```cpp
  // tests/model/variable_test.cc
  #include "cpim/model/variable.h"
  #include <gtest/gtest.h>

  namespace cpim {
  namespace {

  TEST(IntVarTest, BasicOperations) {
      std::vector<int> domain = {1, 2, 3, 4, 5};
      IntVar var(0, domain);

      EXPECT_EQ(var.size(0), 5);
      EXPECT_TRUE(var.has_value(3, 0));
      EXPECT_FALSE(var.has_value(6, 0));
  }

  }  // namespace
  }  // namespace cpim
  ```
  - 每个核心类至少 3 个测试用例
  - 时间：每天 1-2小时，持续进行

**验收标准**：
- 目录结构清晰，模块边界明确
- 每个模块可以独立测试
- 流式 API 可以构建简单模型

---

## 阶段 2：依赖注入与传播器框架（2-3周）

**目标**：参考 OR-Tools 的依赖注入模式，实现可插拔的传播器系统

### 核心设计

```cpp
// cpim/solver/propagator.h
namespace cpim {

class PropagatorInterface {
public:
    virtual ~PropagatorInterface() = default;

    // 初始传播
    virtual absl::Status Propagate() = 0;

    // 增量传播
    virtual absl::Status IncrementalPropagate(absl::Span<const IntVar* const> changed_vars) = 0;

    // 优先级（数字越小越先执行）
    virtual int priority() const = 0;
};

// cpim/solver/model.h
class Model {
public:
    // 注册传播器
    void AddPropagator(std::unique_ptr<PropagatorInterface> propagator);

    // 获取服务（依赖注入）
    template<typename T>
    T* GetOrCreate() {
        // 类似 OR-Tools 的 GetOrCreate 模式
    }

private:
    std::vector<std::unique_ptr<PropagatorInterface>> propagators_;
    absl::flat_hash_map<std::type_index, std::unique_ptr<void>> services_;
};

}  // namespace cpim
```

### 任务清单

- [ ] **2.1 实现依赖注入容器**
  ```cpp
  // cpim/solver/model.h
  class Model {
  public:
      template<typename Service>
      Service* GetOrCreate() {
          auto type_id = std::type_index(typeid(Service));
          auto it = services_.find(type_id);
          if (it == services_.end()) {
              auto service = std::make_unique<Service>(this);
              Service* ptr = service.get();
              services_[type_id] = std::move(service);
              return ptr;
          }
          return static_cast<Service*>(it->second.get());
      }

  private:
      absl::flat_hash_map<std::type_index, std::unique_ptr<void>> services_;
  };
  ```
  - 时间：4-5小时

- [ ] **2.2 重写 AC3 为传播器**
  ```cpp
  // cpim/algorithms/ac3_propagator.h
  class AC3Propagator : public PropagatorInterface {
  public:
      explicit AC3Propagator(Model* model);

      absl::Status Propagate() override;
      absl::Status IncrementalPropagate(absl::Span<const IntVar* const> vars) override;
      int priority() const override { return 1; }

  private:
      Model* model_;
      Trail* trail_;  // 通过 model->GetOrCreate<Trail>() 获取

      bool Revise(const Constraint& c, IntVar* var, int level);
      bool SeekSupport(const Constraint& c, IntVar* var, int value, int level);
  };
  ```
  - 时间：6-8小时

- [ ] **2.3 实现传播器管理器**
  ```cpp
  // cpim/solver/propagation_engine.h
  class PropagationEngine {
  public:
      explicit PropagationEngine(Model* model);

      void AddPropagator(std::unique_ptr<PropagatorInterface> prop);
      absl::Status FixedPoint();  // 执行所有传播器直到不动点

  private:
      std::vector<std::unique_ptr<PropagatorInterface>> propagators_;
      std::priority_queue<PropagatorInterface*> queue_;
  };
  ```
  - 按优先级调度传播器
  - 时间：4-5小时

- [ ] **2.4 改造其他 AC 算法**
  - AC3bit, FC, SAC 等都实现 `PropagatorInterface`
  - 时间：8-10小时

- [ ] **2.5 编写传播器测试**
  ```cpp
  // tests/algorithms/ac3_test.cc
  TEST(AC3PropagatorTest, BasicPropagation) {
      ModelBuilder builder;
      auto x = builder.NewIntVar(0, 5, "x");
      auto y = builder.NewIntVar(0, 5, "y");
      builder.AddEquality(x, y);

      auto model = builder.Build();
      model->AddPropagator(std::make_unique<AC3Propagator>(model.get()));

      auto status = model->Propagate();
      EXPECT_TRUE(status.ok());
  }
  ```
  - 时间：每天 1小时

**验收标准**：
- 传播器可以动态注册和组合
- 依赖注入容器工作正常
- 所有 AC 算法都适配新接口

---

## 阶段 3：Trail 回溯系统 + Abseil 优化（2-3周）

**目标**：实现高性能增量回溯，使用 Abseil 容器优化

### 任务清单

- [ ] **3.1 实现 Trail 系统**
  ```cpp
  // cpim/base/trail.h
  namespace cpim {

  class Trail {
  public:
      explicit Trail(Model* model);

      // 开始新层
      void NewLevel();

      // 回溯到指定层
      void BacktrackTo(int level);

      // 记录可逆操作
      template<typename T>
      void SaveAndSet(T& target, const T& new_value) {
          T old_value = target;
          trail_.push_back([&target, old_value]() { target = old_value; });
          target = new_value;
      }

      int current_level() const { return level_markers_.size() - 1; }

  private:
      std::vector<std::function<void()>> trail_;
      std::vector<size_t> level_markers_;
  };

  }  // namespace cpim
  ```
  - 时间：4-5小时

- [ ] **3.2 IntVar 差分域实现**
  ```cpp
  // cpim/model/variable.h
  class IntVar {
  private:
      absl::InlinedVector<uint64_t, 8> base_domain_;  // bitset
      std::vector<absl::InlinedVector<uint64_t, 8>> removed_per_level_;

      // 缓存
      mutable absl::InlinedVector<int, 16> cached_size_;
      mutable std::vector<bool> size_valid_;

  public:
      void RemoveValue(int value, int level, Trail* trail);
      bool HasValue(int value, int level) const;
      int size(int level) const;  // 使用缓存
  };
  ```
  - 使用 `absl::InlinedVector` 避免小对象堆分配
  - 时间：6-8小时

- [ ] **3.3 使用 Abseil 容器优化**
  ```cpp
  // 替换 std::unordered_map
  absl::flat_hash_map<IntVar*, std::vector<Constraint*>> subscriptions_;

  // 替换 std::vector<bool>
  absl::InlinedVector<uint8_t, 64> flags_;

  // 使用 absl::Span 替代指针+长度
  void ProcessTuples(absl::Span<const int> tuple);
  ```
  - 时间：4-5小时

- [ ] **3.4 性能基准测试**
  ```cpp
  // tests/benchmarks/trail_benchmark.cc
  static void BM_Backtrack_Old(benchmark::State& state) {
      // 旧的全量复制
  }

  static void BM_Backtrack_Trail(benchmark::State& state) {
      // 新的 Trail 系统
  }

  BENCHMARK(BM_Backtrack_Old);
  BENCHMARK(BM_Backtrack_Trail);
  ```
  - 使用 Google Benchmark 库
  - 时间：3-4小时

**验收标准**：
- Trail 系统工作正常
- 回溯性能提升 > 30%
- 基准测试通过

---

## 阶段 4：搜索引擎与启发式框架（3-4周）

**目标**：实现灵活的搜索引擎，支持多种启发式策略

### 核心设计

```cpp
// cpim/solver/search.h
namespace cpim {

// 决策点
struct Decision {
    IntVar* var;
    int value;
    bool assign;  // true: v=a, false: v≠a
};

// 启发式接口
class VarSelectorInterface {
public:
    virtual ~VarSelectorInterface() = default;
    virtual std::optional<IntVar*> Select(
        absl::Span<IntVar* const> vars, int level) const = 0;
};

class ValSelectorInterface {
public:
    virtual ~ValSelectorInterface() = default;
    virtual std::optional<int> Select(
        const IntVar* var, int level) const = 0;
};

// 搜索引擎
class SearchEngine {
public:
    explicit SearchEngine(Model* model);

    void SetVarSelector(std::unique_ptr<VarSelectorInterface> selector);
    void SetValSelector(std::unique_ptr<ValSelectorInterface> selector);

    // 搜索单个解
    absl::StatusOr<Solution> Solve(absl::Duration time_limit);

    // 搜索所有解（使用协程，C++23）
    // std::generator<Solution> SolveAll();

private:
    absl::StatusOr<Solution> SearchImpl(int level);

    Model* model_;
    PropagationEngine* engine_;
    Trail* trail_;
    std::unique_ptr<VarSelectorInterface> var_selector_;
    std::unique_ptr<ValSelectorInterface> val_selector_;
};

}  // namespace cpim
```

### 任务清单

- [ ] **4.1 实现决策管理**
  ```cpp
  // cpim/solver/decision_builder.h
  class DecisionBuilder {
  public:
      static Decision MakeAssignDecision(IntVar* var, int value);
      static Decision MakeRefuteDecision(IntVar* var, int value);
  };
  ```
  - 时间：2-3小时

- [ ] **4.2 实现启发式选择器**
  ```cpp
  // cpim/solver/heuristics/dom_min_selector.h
  class DomMinVarSelector : public VarSelectorInterface {
  public:
      std::optional<IntVar*> Select(
          absl::Span<IntVar* const> vars, int level) const override {
          return std::ranges::min_element(vars, {},
              [level](const IntVar* v) { return v->size(level); });
      }
  };

  // cpim/solver/heuristics/min_val_selector.h
  class MinValSelector : public ValSelectorInterface {
  public:
      std::optional<int> Select(
          const IntVar* var, int level) const override {
          return var->min(level);
      }
  };
  ```
  - 实现常用启发式：DOM_MIN, DOM_WDEG, LEX, etc.
  - 时间：6-8小时

- [ ] **4.3 重写搜索引擎**
  ```cpp
  // cpim/solver/search.cc
  absl::StatusOr<Solution> SearchEngine::SearchImpl(int level) {
      // 传播
      auto status = engine_->FixedPoint();
      if (!status.ok()) {
          return absl::FailedPreconditionError("Propagation failed");
      }

      // 检查是否所有变量已赋值
      if (AllAssigned()) {
          return ExtractSolution();
      }

      // 选择变量
      auto var = var_selector_->Select(UnassignedVars(), level);
      if (!var.has_value()) {
          return absl::FailedPreconditionError("No variable to select");
      }

      // 选择值
      auto val = val_selector_->Select(*var, level);
      if (!val.has_value()) {
          return absl::FailedPreconditionError("No value to select");
      }

      // 尝试 v=a
      trail_->NewLevel();
      (*var)->Assign(*val, level + 1, trail_);
      auto result = SearchImpl(level + 1);
      if (result.ok()) return result;
      trail_->BacktrackTo(level);

      // 尝试 v≠a
      trail_->NewLevel();
      (*var)->RemoveValue(*val, level + 1, trail_);
      result = SearchImpl(level + 1);
      trail_->BacktrackTo(level);

      return result;
  }
  ```
  - 简洁的递归搜索
  - 时间：6-8小时

- [ ] **4.4 统计信息收集**
  ```cpp
  // cpim/util/stats.h
  struct SearchStatistics {
      int64_t num_branches = 0;
      int64_t num_failures = 0;
      int64_t num_solutions = 0;
      absl::Duration solve_time;

      std::string ToString() const;
  };
  ```
  - 时间：2-3小时

**验收标准**：
- 搜索引擎可以灵活配置启发式
- 代码清晰，易于扩展
- 功能正确性不变

---

## 阶段 5：现代 CMake + Bazel（1-2周）

**目标**：参考 OR-Tools，支持 CMake 和 Bazel 双构建系统

### 5.1 Modern CMake

```cmake
# CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
project(cpim VERSION 1.0.0 LANGUAGES CXX CUDA)

# 编译器要求
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 选项
option(CPIM_BUILD_TESTS "Build tests" ON)
option(CPIM_BUILD_EXAMPLES "Build examples" ON)
option(CPIM_ENABLE_CUDA "Enable CUDA support" ON)

# 依赖
find_package(absl REQUIRED)
find_package(GTest REQUIRED)
find_package(XercesC REQUIRED)

# 核心库（模块化）
add_library(cpim_base OBJECT
    cpim/base/bitset.cc
    cpim/base/trail.cc
    cpim/base/types.cc
)

add_library(cpim_model OBJECT
    cpim/model/variable.cc
    cpim/model/constraint.cc
    cpim/model/model_builder.cc
)

add_library(cpim_solver OBJECT
    cpim/solver/propagator.cc
    cpim/solver/search.cc
    cpim/solver/model.cc
)

add_library(cpim_algorithms OBJECT
    cpim/algorithms/ac3.cc
    cpim/algorithms/ac3_bit.cc
    cpim/algorithms/fc.cc
    cpim/algorithms/sac.cc
)

# 组合成主库
add_library(cpim STATIC
    $<TARGET_OBJECTS:cpim_base>
    $<TARGET_OBJECTS:cpim_model>
    $<TARGET_OBJECTS:cpim_solver>
    $<TARGET_OBJECTS:cpim_algorithms>
)

target_include_directories(cpim PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(cpim PUBLIC
    absl::flat_hash_map
    absl::flat_hash_set
    absl::span
    absl::status
    absl::statusor
    absl::strings
    absl::time
)

# 可选 CUDA 支持
if(CPIM_ENABLE_CUDA)
    add_library(cpim_cuda OBJECT
        cpim/cuda/gpu_solver.cu
    )
    target_compile_options(cpim_cuda PRIVATE
        $<$<COMPILE_LANGUAGE:CUDA>:--extended-lambda>
    )
    target_link_libraries(cpim PUBLIC cpim_cuda)
endif()

# 测试
if(CPIM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# 示例
if(CPIM_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# 安装
install(TARGETS cpim
    EXPORT cpimTargets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY cpim/
    DESTINATION include/cpim
    FILES_MATCHING PATTERN "*.h"
)
```

### 5.2 Bazel 支持（可选）

```python
# BUILD.bazel
load("@rules_cc//cc:defs.bzl", "cc_library", "cc_binary", "cc_test")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "base",
    srcs = glob(["cpim/base/*.cc"]),
    hdrs = glob(["cpim/base/*.h"]),
    deps = [
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/types:span",
    ],
)

cc_library(
    name = "model",
    srcs = glob(["cpim/model/*.cc"]),
    hdrs = glob(["cpim/model/*.h"]),
    deps = [":base"],
)

cc_library(
    name = "solver",
    srcs = glob(["cpim/solver/*.cc"]),
    hdrs = glob(["cpim/solver/*.h"]),
    deps = [
        ":base",
        ":model",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
    ],
)

cc_library(
    name = "cpim",
    deps = [
        ":base",
        ":model",
        ":solver",
        ":algorithms",
    ],
)

# 测试
cc_test(
    name = "variable_test",
    srcs = ["tests/model/variable_test.cc"],
    deps = [
        ":model",
        "@com_google_googletest//:gtest_main",
    ],
)
```

### 任务清单

- [ ] **5.1 重写 CMakeLists.txt**
  - 模块化组织，target-based
  - 时间：4-5小时

- [ ] **5.2 配置 Bazel（可选）**
  - 添加 WORKSPACE 和 BUILD 文件
  - 时间：3-4小时

- [ ] **5.3 配置 vcpkg 或 Conan**
  - 管理依赖（Abseil, GTest, XercesC）
  - 时间：2-3小时

**验收标准**：
- CMake 和 Bazel 都能编译项目
- 依赖管理自动化

---

## 阶段 6：API 设计与示例（2-3周）

**目标**：提供易用的 API 和丰富的示例

### 6.1 流式 API 设计

```cpp
// examples/n_queens.cc
#include "cpim/cpim.h"

int main() {
    const int N = 8;

    // 构建模型
    cpim::ModelBuilder builder;

    // 创建变量
    std::vector<cpim::IntVar> queens;
    for (int i = 0; i < N; ++i) {
        queens.push_back(
            builder.NewIntVar(0, N-1, absl::StrCat("Q", i)));
    }

    // 添加约束
    builder.AddAllDifferent(queens);  // 行不同

    // 对角线约束
    for (int i = 0; i < N; ++i) {
        for (int j = i + 1; j < N; ++j) {
            builder.AddNotEqual(queens[i] + i, queens[j] + j);
            builder.AddNotEqual(queens[i] - i, queens[j] - j);
        }
    }

    // 构建求解器
    auto model = builder.Build();
    cpim::SearchEngine solver(model.get());

    // 设置启发式
    solver.SetVarSelector(
        std::make_unique<cpim::DomMinVarSelector>());
    solver.SetValSelector(
        std::make_unique<cpim::MinValSelector>());

    // 求解
    auto result = solver.Solve(absl::Seconds(10));

    if (result.ok()) {
        std::cout << "Solution found!\n";
        for (int i = 0; i < N; ++i) {
            std::cout << "Q" << i << " = "
                      << result->assignment[queens[i].id()] << "\n";
        }
    } else {
        std::cout << "No solution: " << result.status() << "\n";
    }

    return 0;
}
```

### 6.2 命令行工具

```cpp
// tools/cpim_solver.cc
#include "cpim/cpim.h"
#include "parsers/xcsp3/xcsp3_parser.h"
#include <absl/flags/flag.h>
#include <absl/flags/parse.h>

ABSL_FLAG(std::string, input, "", "Input XCSP3 file");
ABSL_FLAG(std::string, algorithm, "ac3bit", "Algorithm: ac3, ac3bit, fc, sac");
ABSL_FLAG(int, time_limit, 900, "Time limit in seconds");
ABSL_FLAG(bool, all_solutions, false, "Find all solutions");

int main(int argc, char* argv[]) {
    absl::ParseCommandLine(argc, argv);

    std::string input_file = absl::GetFlag(FLAGS_input);
    if (input_file.empty()) {
        std::cerr << "Error: --input is required\n";
        return 1;
    }

    // 解析模型
    cpim::XCSP3Parser parser;
    auto model_result = parser.ParseFile(input_file);
    if (!model_result.ok()) {
        std::cerr << "Parse error: " << model_result.status() << "\n";
        return 1;
    }

    auto model = std::move(*model_result);

    // 创建求解器
    cpim::SearchEngine solver(model.get());

    // 配置算法
    std::string alg = absl::GetFlag(FLAGS_algorithm);
    // ... 根据 alg 配置传播器

    // 求解
    auto duration = absl::Seconds(absl::GetFlag(FLAGS_time_limit));
    auto result = solver.Solve(duration);

    if (result.ok()) {
        std::cout << result->ToString() << "\n";
    } else {
        std::cout << "UNSATISFIABLE\n";
    }

    return 0;
}
```

### 任务清单

- [ ] **6.1 实现流式 API**
  - 完善 ModelBuilder
  - 时间：6-8小时

- [ ] **6.2 编写示例程序**
  - N-Queens, Sudoku, Graph Coloring
  - 时间：每个 2-3小时

- [ ] **6.3 命令行工具**
  - 使用 absl::flags
  - 时间：4-5小时

- [ ] **6.4 编写文档**
  - API 文档
  - 用户指南
  - 时间：持续进行

**验收标准**：
- API 易用，代码简洁
- 示例程序完整可运行
- 文档清晰

---

## 阶段 7：高级特性（持续进行）

### 7.1 Python 绑定（可选）

```python
# python/cpim.py
import cpim_pybind

# 创建模型
model = cpim_pybind.ModelBuilder()

# 创建变量
x = model.new_int_var(0, 10, "x")
y = model.new_int_var(0, 10, "y")

# 添加约束
model.add_equality(x + y, 10)

# 求解
solver = cpim_pybind.Solver(model)
status = solver.solve()

if status.ok():
    print(f"x = {solver.value(x)}")
    print(f"y = {solver.value(y)}")
```

使用 pybind11 实现绑定。

### 7.2 Protocol Buffers 模型（可选）

```protobuf
// cpim/proto/cpim_model.proto
syntax = "proto3";

package cpim;

message IntVar {
    int32 id = 1;
    repeated int32 domain = 2;
    string name = 3;
}

message Constraint {
    enum Type {
        ALL_DIFFERENT = 0;
        TABLE = 1;
        EQUALITY = 2;
    }
    Type type = 1;
    repeated int32 var_ids = 2;
    // ... 更多字段
}

message Model {
    repeated IntVar variables = 1;
    repeated Constraint constraints = 2;
}
```

可以实现模型序列化和跨语言支持。

### 7.3 SIMD 优化（高级）

```cpp
// cpim/base/bitset_simd.h
#ifdef __AVX2__
#include <immintrin.h>

inline int popcount_avx2(const uint64_t* data, size_t n) {
    __m256i count = _mm256_setzero_si256();
    for (size_t i = 0; i < n; i += 4) {
        __m256i v = _mm256_loadu_si256((__m256i*)(data + i));
        // ... AVX2 popcount
    }
    // ...
}
#endif
```

---

## 实施策略

### 开发流程

1. **分支管理**
   - `main`：稳定版本
   - `develop`：开发分支
   - `feature/phase-X`：各阶段特性分支

2. **持续集成**
   - GitHub Actions 或 GitLab CI
   - 每次提交运行测试
   - 定期运行基准测试

3. **代码审查**
   - 每个 PR 都需要审查
   - 运行 clang-tidy, clang-format
   - 覆盖率报告

### 测试策略

```cmake
# tests/CMakeLists.txt
add_executable(cpim_tests
    base/bitset_test.cc
    base/trail_test.cc
    model/variable_test.cc
    model/constraint_test.cc
    solver/search_test.cc
    algorithms/ac3_test.cc
    # ...
)

target_link_libraries(cpim_tests
    cpim
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(cpim_tests)
```

### 性能监控

```bash
# scripts/benchmark.sh
#!/bin/bash

INSTANCES=(
    "samples/bench/queens-8.xml"
    "samples/bench/sudoku-9x9.xml"
    # ...
)

for instance in "${INSTANCES[@]}"; do
    echo "Benchmarking $instance"
    perf stat -e cycles,instructions,cache-misses \
        ./build/tools/cpim_solver --input="$instance"
done
```

---

## 预期收益对比

| 指标 | 当前 | 目标 | 提升 |
|------|------|------|------|
| **代码行数** | ~5000 | ~8000 | +60% (更清晰) |
| **编译时间** | 30s | 45s | +50% (模块化) |
| **测试覆盖率** | 0% | 80% | +80% |
| **回溯性能** | baseline | 1.5x | +50% |
| **内存安全** | 多处泄漏 | 零泄漏 | 100% |
| **API 易用性** | 2/10 | 9/10 | +350% |
| **扩展性** | 困难 | 简单 | 质变 |

---

## 风险与应对

### 风险1：学习曲线陡峭
- **影响**：Abseil 库、依赖注入等新概念需要学习
- **应对**：参考 OR-Tools 示例，逐步引入
- **时间成本**：+20%

### 风险2：重构周期长
- **影响**：6个阶段可能需要 4-5 个月
- **应对**：分阶段交付，每阶段都有可用版本
- **优先级**：阶段0-3 是核心，4-7 可选

### 风险3：性能回退
- **影响**：抽象可能带来性能损失
- **应对**：每阶段做基准测试，及时优化
- **目标**：零开销抽象

---

## 总结

基于 Google OR-Tools 的现代化改造计划核心要点：

### 📚 技术栈
- **C++20** + **Abseil** + **GoogleTest** + **CMake/Bazel**

### 🏗️ 架构模式
- **模块化**：base/model/solver/algorithms 清晰分层
- **依赖注入**：松耦合，可测试
- **流式 API**：易用，类型安全
- **传播器框架**：可插拔，可组合

### 🎯 核心改进
1. **Trail 回溯**：性能提升 50%
2. **智能指针**：内存安全 100%
3. **Abseil 优化**：缓存友好
4. **测试覆盖**：80% 以上

### ⏱️ 时间估算
- **核心阶段（0-3）**：8-12周
- **完善阶段（4-6）**：6-8周
- **高级特性（7）**：持续进行

### 🚀 立即行动
从**阶段0**开始：集成 Abseil，升级到 C++20，建立测试框架。

---

**参考资源**：
- [Google OR-Tools](https://github.com/google/or-tools)
- [Abseil C++](https://abseil.io/)
- [GoogleTest](https://google.github.io/googletest/)
- [Modern CMake](https://cliutils.gitlab.io/modern-cmake/)
