# CPIM 项目现代化实施计划

## 项目现状诊断

### 核心问题总结

1. **内存安全**：大量裸指针 new/delete，容易泄漏
2. **性能瓶颈**：回溯时全量复制域数据，域大小计算无缓存
3. **类型安全**：裸枚举、魔术常量（-1）、头文件污染
4. **架构耦合**：搜索流程混乱，算法创建使用 switch-case
5. **数据结构**：自建循环队列、vector<bool>、裸指针哈希
6. **工程化**：CMake 全局指令，缺少测试和文档

## 实施路线图

### 🔴 阶段 1：基础安全性修复（1-2周，立即开始）

**目标**：消除明显的内存安全隐患和编译器警告

#### 任务清单

- [ ] **1.1 头文件清理**
  - 移除 `include/Network.h:9` 的 `using namespace std;`
  - 移除 `include/Solver.h` 等文件的命名空间污染
  - 时间：2小时

- [ ] **1.2 枚举强类型化**
  ```cpp
  // include/Solver.h
  enum class ACAlgorithm {
      AC1, AC2, AC3, AC4, AC6, AC7, AC2001,
      AC3bit, AC3rm, FC, FCbit, LMRPC_BIT, RPC3, NSAC
  };

  enum class Consistency {
      AC3, AC4, AC2001, AC3bit, AC3rm
  };

  namespace Heuristic {
      enum class Var { LEX, DOM_MIN, VWDEG, DEG_MIN, DOM_DEG_MIN, DOM_DDEG_MIN, DOM_WDEG_MIN };
      enum class Val { MIN, MIN_DOM, MIN_INC, MAX_INC, VWDEG };
      enum class DecisionScheme { BI, NB };
  }
  ```
  - 修改所有使用处的枚举引用
  - 时间：3-4小时

- [ ] **1.3 修复已知 Bug**
  - 修复 `src/MAC.cpp:37` 的 `CA_LMRPC_BIT` 缺少 `break` 问题
  - 修复 `src/Solver.cpp:82` 的 `AssignedStack::del` 实现
  - 时间：1小时

- [ ] **1.4 添加编译器警告**
  ```cmake
  # CMakeLists.txt
  add_compile_options(
      -Wall -Wextra -Wpedantic
      -Wno-unused-parameter  # 暂时忽略，后续清理
  )
  ```
  - 修复所有新出现的警告
  - 时间：2-3小时

- [ ] **1.5 移除注释掉的代码**
  - 清理 `CMakeLists.txt` 中注释掉的旧配置
  - 清理 `samples/main.cu` 中注释掉的测试代码
  - 时间：1小时

**验收标准**：
- 编译通过，无警告
- 功能测试通过（手动运行几个基准实例）

---

### 🟠 阶段 2：智能指针改造（2-3周）

**目标**：用智能指针替换裸指针，明确所有权语义

#### 任务清单

- [ ] **2.1 Network 类改造**
  ```cpp
  // include/Network.h
  class Network {
  private:
      std::vector<std::unique_ptr<IntVar>> vars_;
      std::vector<std::unique_ptr<Tabular>> tabs_;

  public:
      // 提供非拥有视图
      std::span<IntVar* const> get_vars() const;
      std::span<Tabular* const> get_tabs() const;

      // 保留原有的 public 成员以减少修改
      std::vector<IntVar*> vars;    // 指向 vars_ 中的裸指针
      std::vector<Tabular*> tabs;   // 指向 tabs_ 中的裸指针
  };
  ```
  - 修改 `src/Network.cpp` 构造函数，使用 `make_unique`
  - 析构函数自动清理，删除手动 delete 代码
  - 时间：4-5小时

- [ ] **2.2 MAC 类改造**
  ```cpp
  // include/Solver.h
  class MAC {
  private:
      Network* n_;
      std::unique_ptr<AC> ac_;  // 改为智能指针
      // ...
  };
  ```
  - 修改 `src/MAC.cpp` 的 AC 对象创建
  - 析构函数删除 `delete ac_` 代码
  - 时间：2-3小时

- [ ] **2.3 主函数改造**
  ```cpp
  // samples/main.cu
  auto n = std::make_unique<Network>(hm);
  MAC mac(n.get(), ACAlgorithm::AC3bit,
          Heuristic::Var::DOM_MIN, Heuristic::Val::MIN);
  // 自动清理
  ```
  - 时间：1小时

- [ ] **2.4 测试验证**
  - Valgrind 检查内存泄漏
  - 运行基准测试对比性能
  - 时间：2小时

**验收标准**：
- Valgrind 零泄漏
- 性能无明显下降（< 5%）

---

### 🟡 阶段 3：Trail 回溯系统（3-4周）

**目标**：用增量回溯替代全量域复制，大幅提升回溯性能

#### 任务清单

- [ ] **3.1 设计 Trail 系统**
  ```cpp
  // include/Trail.h
  class TrailManager {
  private:
      struct Operation {
          std::function<void()> undo;
      };
      std::vector<Operation> trail_;
      std::vector<size_t> level_markers_;

  public:
      void new_level();
      void backtrack_to(int level);

      template<typename T>
      void save_and_set(T& target, const T& new_value) {
          T old_value = target;
          trail_.push_back({[&target, old_value]() { target = old_value; }});
          target = new_value;
      }
  };
  ```
  - 时间：3-4小时

- [ ] **3.2 IntVar 差分域实现**
  ```cpp
  class IntVar {
  private:
      bitSetVector base_domain_;                // 基础域（不变）
      std::vector<bitSetVector> removed_vals_;  // 每层删除的值（差分）
      mutable std::vector<int> cached_size_;    // 缓存每层的大小
      mutable std::vector<bool> size_valid_;    // 缓存是否有效

  public:
      void RemoveValue(int a, int level, TrailManager& trail);
      bool have(int a, int level) const;
      int size(int level) const;  // 使用缓存
      void invalidate_cache(int level);
  };
  ```
  - 修改所有域操作方法
  - 时间：8-10小时

- [ ] **3.3 Network 集成 Trail**
  ```cpp
  class Network {
  private:
      TrailManager trail_;

  public:
      int NewLevel(int src);      // 使用 trail 而非复制
      void BackTo(int dest);      // 使用 trail 回溯
      TrailManager& get_trail() { return trail_; }
  };
  ```
  - 修改 `NewLevel` 和 `BackTo` 实现
  - 时间：4-5小时

- [ ] **3.4 MAC 适配 Trail**
  - 在变量赋值时记录到 trail
  - 删除手动回溯代码
  - 时间：3-4小时

- [ ] **3.5 性能基准测试**
  - 对比新旧实现的回溯性能
  - 测量内存使用
  - 时间：2-3小时

**验收标准**：
- 回溯性能提升 > 30%
- 功能正确性不变
- 内存使用合理（trail 不会无限增长）

---

### 🟢 阶段 4：类型安全与现代 C++ 特性（2-3周）

**目标**：应用 C++17/20 特性，提升类型安全和代码可读性

#### 任务清单

- [ ] **4.1 std::optional 替代魔术常量**
  ```cpp
  // include/Network.h
  class IntVar {
  public:
      std::optional<int> head(int level) const;
      std::optional<int> tail(int level) const;
      std::optional<int> next(int a, int level) const;
      std::optional<int> prev(int a, int level) const;
  };
  ```
  - 修改所有使用 `-1` 表示无效值的地方
  - 时间：4-5小时

- [ ] **4.2 std::span 零开销视图**
  ```cpp
  // include/Network.h
  class Network {
  public:
      std::span<IntVar* const> get_vars_span() const;
      std::span<Tabular* const> get_tabs_span() const;
  };

  class Tabular {
  public:
      std::span<const int> get_tuple(int idx) const;
  };
  ```
  - 时间：3-4小时

- [ ] **4.3 结构化绑定**
  ```cpp
  // 简化 bitset 索引计算
  auto [word_idx, bit_idx] = GetBitIdx(value);
  bit_doms_[level][word_idx].reset(bit_idx);
  ```
  - 时间：2小时

- [ ] **4.4 std::ranges 简化算法**
  ```cpp
  // 选择未赋值变量
  auto unassigned = n_->get_vars_span()
      | std::views::filter([p](const IntVar* v) { return !v->assigned(p); });

  // 选择最小域变量
  auto min_var = std::ranges::min_element(unassigned, {},
      [p](const IntVar* v) { return v->size(p); });
  ```
  - 时间：3-4小时

- [ ] **4.5 [[nodiscard]] 注解**
  ```cpp
  [[nodiscard]] bool enforce(std::vector<IntVar*>& x_evt, int level);
  [[nodiscard]] std::optional<int> select_value(const IntVar* v, int level) const;
  ```
  - 时间：1-2小时

**验收标准**：
- 编译通过，零警告
- 代码可读性提升
- 性能无下降

---

### 🔵 阶段 5：架构重构 - 策略模式（3-4周）

**目标**：解耦算法和策略，支持灵活扩展

#### 任务清单

- [ ] **5.1 定义策略接口**
  ```cpp
  // include/Strategies.h
  class IConsistencyAlgorithm {
  public:
      virtual ~IConsistencyAlgorithm() = default;
      [[nodiscard]] virtual bool enforce(std::span<IntVar*> events, int level) = 0;
  };

  class IVarSelector {
  public:
      virtual ~IVarSelector() = default;
      [[nodiscard]] virtual IntVar* select(std::span<IntVar* const> vars, int level) const = 0;
  };

  class IValSelector {
  public:
      virtual ~IValSelector() = default;
      [[nodiscard]] virtual std::optional<int> select(const IntVar* var, int level) const = 0;
  };
  ```
  - 时间：2-3小时

- [ ] **5.2 实现工厂模式**
  ```cpp
  // include/Factories.h
  class PropagatorFactory {
  public:
      static std::unique_ptr<IConsistencyAlgorithm> create(
          ACAlgorithm alg, Network* net);
  };

  class VarSelectorFactory {
  public:
      static std::unique_ptr<IVarSelector> create(
          Heuristic::Var var_h, Network* net);
  };

  class ValSelectorFactory {
  public:
      static std::unique_ptr<IValSelector> create(
          Heuristic::Val val_h, Network* net);
  };
  ```
  - 使用 `absl::flat_hash_map` 或 `std::unordered_map` 注册工厂函数
  - 时间：4-5小时

- [ ] **5.3 MAC 类重构**
  ```cpp
  class MAC {
  private:
      Network* n_;
      std::unique_ptr<IConsistencyAlgorithm> propagator_;
      std::unique_ptr<IVarSelector> var_selector_;
      std::unique_ptr<IValSelector> val_selector_;

  public:
      MAC(Network* n, ACAlgorithm alg,
          Heuristic::Var var_h, Heuristic::Val val_h)
          : n_(n)
          , propagator_(PropagatorFactory::create(alg, n))
          , var_selector_(VarSelectorFactory::create(var_h, n))
          , val_selector_(ValSelectorFactory::create(val_h, n)) {}

      SearchStatistics enforce(int time_limit);
  };
  ```
  - 删除原有的 switch-case 代码
  - 时间：3-4小时

- [ ] **5.4 各算法适配接口**
  - AC3, AC3bit, FC, FCbit 等继承 `IConsistencyAlgorithm`
  - 时间：6-8小时

- [ ] **5.5 测试新架构**
  - 验证所有算法仍然工作
  - 时间：2-3小时

**验收标准**：
- 添加新算法只需实现接口并注册工厂
- 无需修改 MAC 核心代码

---

### 🟣 阶段 6：性能优化与工程化（持续进行）

**目标**：应用高级优化技术，建立工程化体系

#### 6.1 性能优化

- [ ] **残基支持 (Residues)**
  ```cpp
  class AC3Residues : public AC3 {
  private:
      struct Residue {
          int tuple_idx = -1;
          uint64_t timestamp = 0;
      };
      absl::flat_hash_map<std::tuple<Tabular*, IntVar*, int>, Residue> residues_;
      uint64_t global_timestamp_ = 0;

  protected:
      bool seek_support(const IntConVal& c_val, int level) override;
  };
  ```
  - 为 AC3/AC3bit 添加残基缓存
  - 时间：4-5小时

- [ ] **约束元组索引**
  ```cpp
  class Tabular {
  private:
      // var_pos -> value -> [tuple_indices]
      absl::flat_hash_map<std::pair<int, int>, std::vector<int>> support_index_;

  public:
      void build_support_index();
      std::span<const int> get_support_tuples(int var_pos, int val) const;
  };
  ```
  - 预计算每个 (变量, 值) 的支持元组
  - 时间：3-4小时

- [ ] **SoA 数据布局**（可选，长期优化）
  - 将 `IntVar` 拆分为结构化数组
  - 时间：6-8小时

#### 6.2 Modern CMake

- [ ] **改造 CMakeLists.txt**
  ```cmake
  cmake_minimum_required(VERSION 3.20)
  project(cpim LANGUAGES CXX CUDA)

  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)

  # 核心库
  add_library(cpim_core STATIC
      src/Network.cpp
      src/Solver.cpp
      src/AC3.cpp
      src/AC3bit.cpp
      src/AC3rm.cpp
      src/FC.cpp
      src/MAC.cpp
      # ...
  )

  target_include_directories(cpim_core PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  )

  target_link_libraries(cpim_core PUBLIC
      xcsp3parser
      ${XercesC_LIBRARIES}
      ${LIBXML2_LIBRARIES}
  )

  # 可执行文件
  add_executable(cpim samples/main.cu)
  target_link_libraries(cpim PRIVATE cpim_core)

  # 测试
  if(BUILD_TESTING)
      enable_testing()
      add_subdirectory(tests)
  endif()
  ```
  - 删除全局 `include_directories` / `link_directories`
  - 使用 target-based 属性管理
  - 时间：3-4小时

#### 6.3 测试框架

- [ ] **建立单元测试**
  ```cpp
  // tests/network_test.cpp
  #include <gtest/gtest.h>
  #include "Network.h"

  TEST(NetworkTest, CreateNetwork) {
      // ...
  }

  TEST(IntVarTest, RemoveValue) {
      // ...
  }
  ```
  - 为核心类编写测试
  - 集成到 CMake 的 `ctest`
  - 时间：每周 2-3小时，持续进行

#### 6.4 文档与代码风格

- [ ] **统一代码风格**
  - 使用 `.clang-format` 配置
  - 统一命名规范（变量 `snake_case`，类 `PascalCase`）
  - 时间：持续进行

- [ ] **补充文档**
  - 更新 `CLAUDE.md`
  - 添加关键类的注释
  - 时间：持续进行

---

## 实施策略

### 分支管理

- `main` 分支：保持稳定
- `modernization` 分支：进行重构
- 每个阶段创建子分支：`mod-phase1`, `mod-phase2`, ...
- 每个阶段完成后合并到 `modernization`，充分测试后合并到 `main`

### 测试策略

每个阶段完成后：
1. 运行所有基准实例，验证结果正确性
2. 对比性能（时间、内存）
3. Valgrind / AddressSanitizer 检查内存问题
4. 如果有回退，分析原因并调整

### 性能监控

建立基准测试脚本：
```bash
#!/bin/bash
# benchmark.sh
for instance in samples/bench/*.xml; do
    echo "Testing $instance"
    /usr/bin/time -v ./cpim "$instance" 2>&1 | grep -E "time|memory"
done
```

记录每个阶段的性能数据，确保无明显退化。

---

## 预期收益

### 代码质量
- **内存安全**：消除泄漏和悬垂指针
- **类型安全**：强类型枚举、std::optional
- **可维护性**：清晰的架构、策略模式

### 性能提升
- **回溯优化**：30-50% 性能提升（Trail 系统）
- **缓存优化**：域大小缓存、残基支持
- **内存效率**：智能指针管理、减少复制

### 开发效率
- **扩展性**：添加新算法无需修改核心代码
- **调试性**：类型安全、明确错误
- **测试覆盖**：单元测试保障质量

---

## 风险与应对

### 风险1：性能回退
- **应对**：每阶段做性能基准测试，出现回退及时调整
- **预案**：保留旧代码路径，使用 feature flag 切换

### 风险2：重构时间超预期
- **应对**：按阶段推进，优先完成高价值阶段（1-3）
- **预案**：阶段4-6可以延后或选择性实施

### 风险3：引入新 Bug
- **应对**：充分测试，使用 sanitizers 检测
- **预案**：每个阶段独立分支，问题发现时可以回滚

---

## 总结

本计划分6个阶段渐进式现代化 CPIM 项目：

1. **阶段1**（1-2周）：基础安全修复 → 消除警告和明显问题
2. **阶段2**（2-3周）：智能指针改造 → 内存安全
3. **阶段3**（3-4周）：Trail回溯系统 → 性能提升
4. **阶段4**（2-3周）：现代C++特性 → 类型安全
5. **阶段5**（3-4周）：策略模式重构 → 架构清晰
6. **阶段6**（持续）：高级优化与工程化 → 持续改进

**总计**：约 3-4 个月完成核心现代化，后续持续优化。

**立即行动**：从阶段1开始，先清理头文件和枚举，快速见效。
