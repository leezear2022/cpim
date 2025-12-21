# CPIM 项目综合重构指南

## 文档说明

本文档综合了多个角度的代码分析结果,提供了一份完整的、可执行的重构路线图。建议按优先级顺序渐进式实施。

---

## 一、核心问题诊断

### 1.1 内存安全与所有权问题 ⚠️ **高优先级**

**问题定位**:
- `samples/main.cpp:16-18`: 手动 `new/delete`,易泄漏
- `src/Network.cpp:439-444`: 析构函数手动清理,易出错
- `src/MAC.cpp:214-216`: 手动 `delete ac_`,所有权不明
- `src/Network.cpp:350,355`: 大量裸指针 `new IntVar/Tabular`

**改进方案**:
```cpp
// 当前代码 (main.cpp:16-18)
auto* n = new Network(hm);
n->show(0);
delete n;

// 改进后
auto n = std::make_unique<Network>(hm);
n->show(0);
// 自动清理

// Network类改造
class Network {
private:
    std::vector<std::unique_ptr<IntVar>> vars_;    // 拥有所有权
    std::vector<std::unique_ptr<Tabular>> tabs_;   // 拥有所有权

public:
    // 提供非拥有视图 (C++20)
    std::span<IntVar*> vars() const {
        return {reinterpret_cast<IntVar**>(vars_.data()), vars_.size()};
    }
    std::span<Tabular*> tabs() const;
};
```

### 1.2 性能热点问题 🔥 **高优先级**

**问题定位**:
- `src/Network.cpp:395-404`: `NewLevel/BackTo` 全量复制域,O(n×dom) 开销
- `src/Network.cpp:110-114`: 域大小计算每次遍历 bitset,应缓存
- `src/AC3.cpp:99-141`: 每次 enforce 重置 stamp,应用递增 ID
- `src/Solver.cpp:114`: `update_model_assigned` 每次循环都复制整个 vector

**改进方案 - Trail回溯系统**:
```cpp
// 替换层复制为增量回溯
class TrailManager {
private:
    struct Restore {
        std::function<void()> undo;
    };
    std::vector<Restore> trail_;
    std::vector<size_t> level_marks_;  // 每层起始位置

public:
    // 记录可逆操作
    void save_and_set(u64& target, u64 new_value) {
        u64 old_value = target;
        trail_.push_back({[&target, old_value]() { target = old_value; }});
        target = new_value;
    }

    // 创建新层
    void new_level() {
        level_marks_.push_back(trail_.size());
    }

    // 回溯到指定层
    void backtrack_to(int level) {
        size_t target = level_marks_[level];
        while (trail_.size() > target) {
            trail_.back().undo();
            trail_.pop_back();
        }
        level_marks_.resize(level + 1);
    }
};

// IntVar 改用差分域
class IntVar {
private:
    bitSetVector base_domain_;           // 基础域
    std::vector<bitSetVector> removed_;  // 每层删除的值(差分)

public:
    void remove_value(int a, int level, TrailManager& trail) {
        auto [word, bit] = GetBitIdx(a);
        if (base_domain_[word].test(bit)) {
            trail.save_and_set(base_domain_[word][bit], false);
            removed_[level][word].set(bit);
        }
    }

    // 查询时合并基础域与删除集
    bool has_value(int a, int level) const {
        auto [word, bit] = GetBitIdx(a);
        if (!base_domain_[word].test(bit)) return false;
        // 检查所有祖先层是否删除过
        for (int l = 0; l <= level; ++l) {
            if (removed_[l][word].test(bit)) return false;
        }
        return true;
    }
};
```

### 1.3 类型安全问题 🔒 **中优先级**

**问题定位**:
- `include/Solver.h:13`: 裸 `enum` 污染全局命名空间
- `include/Network.h:9`: 头文件 `using namespace std;` 污染
- 大量使用 `INDEX_OVERFLOW = -1` 表示无效值

**改进方案**:
```cpp
// 强类型枚举 (include/Solver.h)
enum class ACAlgorithm {
    AC1, AC2, AC3, AC4, AC6, AC7, AC2001,
    AC3bit, AC3rm, FC, FCbit, LMRPC_BIT, RPC3, NSAC
};

enum class Consistency {
    AC3, AC4, AC2001, AC3bit, AC3rm
};

namespace Heuristic {
    enum class VarOrder { LEX, DOM_MIN, VWDEG, DEG_MIN, DOM_DEG_MIN, DOM_DDEG_MIN, DOM_WDEG_MIN };
    enum class ValOrder { MIN, MIN_DOM, MIN_INC, MAX_INC, VWDEG };
}

// 使用 std::optional 替代特殊值 (C++17)
std::optional<int> IntVar::head(int level) const {
    for (size_t i = 0; i < num_bit_; ++i) {
        if (bit_doms_[level][i].any())
            return GetValue(i, FirstOne(bit_doms_[level][i]));
    }
    return std::nullopt;  // 类型安全的"无值"
}

// 调用端
if (auto val = var->head(level)) {
    process(*val);
}
```

### 1.4 架构设计问题 🏗️ **中优先级**

**问题定位**:
- `src/MAC.cpp:14-40`: switch-case 手动创建算法,违反开闭原则
- `src/MAC.cpp:100-155`: 搜索流程嵌套 while 循环,状态管理混乱
- AC 继承层次混乱 (AC3bit 继承 AC3,FCbit 继承 AC3bit,但 FC 继承 AC3)

**改进方案 - 策略模式 + 工厂**:
```cpp
// 定义统一接口
class IConsistencyAlgorithm {
public:
    virtual ~IConsistencyAlgorithm() = default;
    [[nodiscard]] virtual bool enforce(int level) = 0;
    [[nodiscard]] virtual bool propagate(std::span<const IntVal> events, int level) = 0;
};

// 策略接口
class IVarSelector {
public:
    virtual ~IVarSelector() = default;
    [[nodiscard]] virtual IntVar* select(std::span<IntVar*> vars, int level) const = 0;
};

class IValSelector {
public:
    virtual ~IValSelector() = default;
    [[nodiscard]] virtual std::optional<int> select(IntVar* var, int level) const = 0;
};

// 工厂模式
class PropagatorFactory {
public:
    using FactoryFunc = std::function<std::unique_ptr<IConsistencyAlgorithm>(Network*)>;

    static std::unique_ptr<IConsistencyAlgorithm> create(ACAlgorithm alg, Network* n) {
        static const absl::flat_hash_map<ACAlgorithm, FactoryFunc> factories = {
            {ACAlgorithm::AC3, [](Network* net) { return std::make_unique<AC3>(net); }},
            {ACAlgorithm::AC3bit, [](Network* net) { return std::make_unique<AC3bit>(net); }},
            {ACAlgorithm::FC, [](Network* net) { return std::make_unique<FC>(net); }},
            // ...
        };
        return factories.at(alg)(n);
    }
};

// MAC 重构
class MAC {
private:
    std::unique_ptr<IConsistencyAlgorithm> propagator_;
    std::unique_ptr<IVarSelector> var_selector_;
    std::unique_ptr<IValSelector> val_selector_;

public:
    MAC(Network* n, ACAlgorithm alg,
        Heuristic::VarOrder var_h, Heuristic::ValOrder val_h)
        : propagator_(PropagatorFactory::create(alg, n))
        , var_selector_(VarSelectorFactory::create(var_h, n))
        , val_selector_(ValSelectorFactory::create(val_h, n)) {}
};
```

---

## 二、现代C++特性应用路线图

### 2.1 C++17 特性 (立即可用)

**优先引入**:
```cpp
// 1. 结构化绑定
auto [word_idx, bit_idx] = GetBitIdx(a);
bit_doms_[p][word_idx].reset(bit_idx);

// 2. std::optional 处理可选值
std::optional<int> find_support(const IntConVal& c_val, int level);

// 3. std::string_view 避免字符串拷贝
void log_message(std::string_view msg);

// 4. if/switch 初始化语句
if (auto var = select_variable(level); var->assigned()) {
    // ...
}

// 5. constexpr if 优化模板
template<typename T>
void process() {
    if constexpr (std::is_integral_v<T>) {
        // 整数特化
    } else {
        // 通用路径
    }
}

// 6. std::variant 类型安全的 Union (替代 ExpType 枚举)
using ExpValue = std::variant<int, double, std::string, IntVar*>;
```

### 2.2 C++20 特性 (强烈推荐)

**核心特性**:
```cpp
// 1. std::span 零开销视图
void process_variables(std::span<IntVar* const> vars);
void process_tuples(std::span<const int> tuple);

// 2. Concepts 约束模板
template<std::integral T>
class NaiveBitSet { /* ... */ };

template<typename T>
concept Propagator = requires(T p, int level) {
    { p.enforce(level) } -> std::convertible_to<bool>;
};

// 3. Ranges 简化算法
auto unassigned = vars
    | std::views::filter([level](auto* v) { return !v->assigned(level); });
auto min_var = std::ranges::min_element(unassigned, {},
    [level](auto* v) { return v->size(level); });

// 4. [[likely]] / [[unlikely]] 分支预测
bool IntVar::have(int a, int p) const {
    if (a == Limits::INDEX_OVERFLOW) [[unlikely]] {
        return false;
    }
    // 快速路径
}

// 5. std::bit 位操作
#include <bit>
int popcount = std::popcount(bitset);
int first_one = std::countr_zero(bitset);

// 6. 协程改造搜索 (见下节详述)
std::generator<Solution> MAC::search(int level);
```

### 2.3 C++20 协程重构搜索 🚀 **创新方案**

**目标**: 彻底简化回溯逻辑,使搜索代码线性化

```cpp
#include <generator>  // C++23 或第三方实现

// RAII 风格的赋值管理
class ScopedAssignment {
    Network& net_;
    AssignedStack& stack_;
    int level_;

public:
    ScopedAssignment(Network& n, AssignedStack& s, int lvl)
        : net_(n), stack_(s), level_(lvl) {
        net_.new_level(level_);
    }

    void assign(const IntVal& decision) {
        stack_.push(decision);
        net_.assign(decision.v, decision.a, level_);
    }

    void refute(const IntVal& decision) {
        net_.remove(decision.v, decision.a, level_);
    }

    ~ScopedAssignment() {
        net_.backtrack_to(level_);  // 自动回溯
    }
};

// 协程化的搜索
class MAC {
public:
    std::generator<Solution> solve() {
        if (!propagator_->enforce(0)) {
            co_return;  // 初始传播失败
        }

        // 委托给递归生成器
        for (auto& sol : search_impl(0)) {
            co_yield sol;
        }
    }

private:
    std::generator<Solution> search_impl(int level) {
        // 检查是否所有变量已赋值
        if (assigned_stack_.full()) {
            co_yield extract_solution();
            co_return;
        }

        // 选择变量和值
        IntVar* var = var_selector_->select(n_->vars(), level);
        if (!var) co_return;

        auto val_opt = val_selector_->select(var, level);
        if (!val_opt) co_return;

        IntVal decision{var, *val_opt, true};

        // 尝试分支1: v = a
        {
            ScopedAssignment assign(n_, assigned_stack_, level);
            assign.assign(decision);

            if (propagator_->enforce(level + 1)) {
                for (auto& sol : search_impl(level + 1)) {
                    co_yield sol;
                }
            }
        }  // 自动回溯

        // 尝试分支2: v ≠ a
        {
            ScopedAssignment assign(n_, assigned_stack_, level);
            assign.refute(decision);

            if (propagator_->enforce(level + 1)) {
                for (auto& sol : search_impl(level + 1)) {
                    co_yield sol;
                }
            }
        }  // 自动回溯
    }
};

// 使用方式
MAC solver(network, ACAlgorithm::AC3bit,
           Heuristic::VarOrder::DOM_MIN, Heuristic::ValOrder::MIN);

for (const auto& solution : solver.solve()) {
    std::cout << "Found solution: " << solution << '\n';
    if (only_first_solution) break;
}
```

**优势**:
- 代码结构即搜索树结构,极易理解
- 回溯逻辑完全由RAII保证,消除人工错误
- 支持懒惰求解(找到一个解就停止)
- 便于实现重启、超时等控制

### 2.4 C++23 特性 (前瞻性)

```cpp
// 1. std::expected 错误处理
std::expected<int, ErrorCode> find_support(const IntConVal& c_val) {
    if (/* 错误 */) {
        return std::unexpected(ErrorCode::INVALID_CONSTRAINT);
    }
    return support_value;
}

// 2. std::flat_map 提升缓存局部性
std::flat_map<IntVar*, std::vector<Tabular*>> subscription_;

// 3. std::mdspan 多维数组视图
std::mdspan<int, std::extents<int, std::dynamic_extent, std::dynamic_extent>> matrix;
```

---

## 三、性能优化专题

### 3.1 数据结构优化

**SoA (Structure of Arrays) 布局**:
```cpp
// 当前 AoS (Array of Structures)
struct IntVar {
    int id;
    int domain_size;
    bool assigned;
    bitSetVector domain;
};
std::vector<IntVar> vars;  // 访问 domain_size 时跨越整个结构

// 改进 SoA
class VariableManager {
private:
    std::vector<int> ids_;
    std::vector<int> domain_sizes_;      // 连续存储
    std::vector<bool> assigned_;
    std::vector<bitSetVector> domains_;

public:
    int domain_size(size_t idx) const { return domain_sizes_[idx]; }
};
```

**缓存域大小**:
```cpp
class IntVar {
private:
    mutable std::vector<int> cached_size_;  // 每层缓存
    mutable std::vector<bool> size_valid_;

public:
    int size(int level) const {
        if (!size_valid_[level]) {
            cached_size_[level] = compute_size(level);
            size_valid_[level] = true;
        }
        return cached_size_[level];
    }

    void invalidate_cache(int level) {
        size_valid_[level] = false;
    }
};
```

### 3.2 算法优化

**残基支持 (Residues)**:
```cpp
class AC3Residues : public AC3 {
private:
    // 为每个 (约束, 变量, 值) 记录上次找到的支持
    struct Residue {
        int tuple_index = -1;
        uint64_t timestamp = 0;
    };
    absl::flat_hash_map<std::tuple<Tabular*, IntVar*, int>, Residue> residues_;
    uint64_t global_timestamp_ = 0;

protected:
    bool seek_support(Tabular* c, IntVar* x, int a, int level) override {
        auto key = std::make_tuple(c, x, a);
        auto& res = residues_[key];

        // 检查残基是否仍然有效
        if (res.timestamp == global_timestamp_) {
            if (c->is_valid_tuple(res.tuple_index, level)) {
                return true;  // 残基仍有效,无需搜索
            }
        }

        // 从残基位置开始搜索
        int start = (res.tuple_index + 1) % c->size();
        for (int i = 0; i < c->size(); ++i) {
            int idx = (start + i) % c->size();
            if (c->is_valid_tuple(idx, level)) {
                res.tuple_index = idx;
                res.timestamp = global_timestamp_;
                return true;
            }
        }
        return false;
    }

public:
    void on_backtrack() override {
        ++global_timestamp_;  // 使所有残基失效
    }
};
```

**约束元组索引**:
```cpp
class Tabular {
private:
    // 为每个 var=val 预计算支持的元组索引
    absl::flat_hash_map<std::pair<int, int>, std::vector<int>> support_index_;

public:
    void build_support_index() {
        for (int tuple_idx = 0; tuple_idx < tuples_.size(); ++tuple_idx) {
            for (int var_pos = 0; var_pos < scope_.size(); ++var_pos) {
                int val = tuples_[tuple_idx][var_pos];
                support_index_[{var_pos, val}].push_back(tuple_idx);
            }
        }
    }

    std::span<const int> get_support_tuples(int var_pos, int val) const {
        auto it = support_index_.find({var_pos, val});
        return it != support_index_.end() ? std::span(it->second) : std::span<const int>{};
    }
};
```

### 3.3 容器优化

```cpp
// 1. 使用 std::deque 替代手工循环队列
std::deque<arc> arc_queue_;

// 2. 使用 absl::flat_hash_map 提升查找性能
absl::flat_hash_map<IntVar*, std::vector<Tabular*>> subscription_;

// 3. 避免 std::vector<bool>
std::vector<uint8_t> flags_;  // 替代 vector<bool>

// 4. 小对象使用 std::array
template<size_t N>
using SmallVector = std::array<int, N>;

// 5. 对象池复用小对象
template<typename T>
class ObjectPool {
    std::vector<std::unique_ptr<T>> pool_;
    std::vector<T*> free_list_;

public:
    T* acquire() {
        if (free_list_.empty()) {
            pool_.push_back(std::make_unique<T>());
            return pool_.back().get();
        }
        T* obj = free_list_.back();
        free_list_.pop_back();
        return obj;
    }

    void release(T* obj) {
        free_list_.push_back(obj);
    }
};
```

---

## 四、实施路线图

### 阶段1: 安全性与稳定性 (2-3周)

**目标**: 消除内存泄漏和未定义行为

- [ ] **1.1** 清理头文件: 移除 `using namespace std;`,枚举改 `enum class`
- [ ] **1.2** 智能指针改造: `Network/MAC` 使用 `unique_ptr` 管理所有权
- [ ] **1.3** 修复已知Bug:
  - `src/Solver.cpp:82` 的 `AssignedStack::del` 正确实现 erase
  - `src/MAC.cpp:15-37` 修复 `CA_LMRPC_BIT` 的 fallthrough
- [ ] **1.4** 添加 `[[nodiscard]]` 到所有返回状态的函数
- [ ] **1.5** 建立最小 GTest 测试框架

**验收标准**: Valgrind 零泄漏, AddressSanitizer 无错误

### 阶段2: 性能优化基础 (3-4周)

**目标**: Trail回溯 + 域缓存 + 残基支持

- [ ] **2.1** 实现 `TrailManager` 类
- [ ] **2.2** `IntVar` 改用差分域表示
- [ ] **2.3** 域大小缓存 + 失效机制
- [ ] **2.4** AC3 系列添加残基支持
- [ ] **2.5** 性能基准测试 (与原版对比)

**验收标准**: 回溯性能提升 30% 以上

### 阶段3: 架构重构 (4-5周)

**目标**: 策略化 + 工厂模式 + 接口清晰化

- [ ] **3.1** 定义 `IConsistencyAlgorithm/IVarSelector/IValSelector` 接口
- [ ] **3.2** 实现 `PropagatorFactory/VarSelectorFactory/ValSelectorFactory`
- [ ] **3.3** 重构 `MAC` 类,解耦策略
- [ ] **3.4** 统一 `AC/AC3/AC3bit/FC` 继承体系
- [ ] **3.5** 约束元组建立支持索引

**验收标准**: 添加新启发式只需实现接口,无需修改核心代码

### 阶段4: 现代化特性应用 (2-3周)

**目标**: C++17/20 特性全面应用

- [ ] **4.1** `std::optional` 替代 `INDEX_OVERFLOW`
- [ ] **4.2** `std::span` 改造所有容器视图接口
- [ ] **4.3** 结构化绑定简化代码
- [ ] **4.4** Ranges 简化算法代码
- [ ] **4.5** `std::bit` 优化位操作

**验收标准**: 编译器警告级别 `-Wall -Wextra -Wpedantic` 零警告

### 阶段5: 协程搜索 (选做,3-4周)

**目标**: 使用协程彻底简化搜索逻辑

- [ ] **5.1** 实现 `ScopedAssignment` RAII类
- [ ] **5.2** 将 `MAC::solve` 改为生成器
- [ ] **5.3** 递归搜索改为协程
- [ ] **5.4** 测试懒惰求解与多解枚举

**验收标准**: 搜索代码行数减少 50%,可读性显著提升

### 阶段6: 高级优化 (持续进行)

**目标**: SoA布局 + SIMD + 并行

- [ ] **6.1** SoA 数据布局改造
- [ ] **6.2** SIMD 优化 bitset 操作
- [ ] **6.3** 并行传播器 (多线程AC)
- [ ] **6.4** CMake 现代化 (target-based)

---

## 五、CMake 现代化建议

### 5.1 当前问题

- 全局 `include_directories/link_directories`
- 手动管理源文件列表
- 重复条目 (`src/AC3rm.cpp` 两次)
- 注释掉的旧代码过多

### 5.2 改进方案

```cmake
cmake_minimum_required(VERSION 3.20)
project(cpim LANGUAGES CXX CUDA)

# 编译器要求
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 警告级别
add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wconversion -Wsign-conversion
    -Wold-style-cast -Wcast-align
)

# 核心源文件 (去重)
set(CPIM_CORE_SOURCES
    src/Network.cpp
    src/Solver.cpp
    src/AC3.cpp
    src/AC3bit.cpp
    src/AC3rm.cpp  # 只保留一次
    src/FC.cpp
    src/FCbit.cpp
    src/MAC.cpp
    src/SAC1.cpp
    src/NSAC.cpp
    src/RPC3.cpp
    # ...
)

# 创建核心库 (方便测试链接)
add_library(cpim_core STATIC ${CPIM_CORE_SOURCES})

# Target-based 属性设置
target_include_directories(cpim_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(cpim_core
    PUBLIC
        xcsp3parser
        ${XercesC_LIBRARIES}
        ${LIBXML2_LIBRARIES}
        absl::flags
        absl::log
)

# 可执行文件
if(CPIM_ENABLE_CUDA)
    add_executable(cpim samples/main.cu)
    set_target_properties(cpim PROPERTIES CUDA_ARCHITECTURES "87")
else()
    add_executable(cpim samples/main.cpp)
endif()

target_link_libraries(cpim PRIVATE cpim_core)

# 测试
if(GTest_FOUND)
    enable_testing()

    add_executable(cpim_tests
        tests/network_test.cpp
        tests/solver_test.cpp
    )

    target_link_libraries(cpim_tests
        PRIVATE
            cpim_core
            GTest::gtest
            GTest::gtest_main
    )

    include(GoogleTest)
    gtest_discover_tests(cpim_tests)
endif()
```

---

## 六、代码审查检查清单

每次提交前检查:

### 内存安全
- [ ] 无裸 `new/delete`,全用智能指针
- [ ] 无悬垂指针/引用
- [ ] RAII 管理所有资源

### 类型安全
- [ ] 使用 `enum class`
- [ ] `std::optional` 处理可选值
- [ ] 无隐式类型转换

### 性能
- [ ] 容器按引用传递,优先用 `std::span`
- [ ] 小对象 noexcept move
- [ ] 热路径避免堆分配

### 可读性
- [ ] 函数单一职责,不超过 50 行
- [ ] 命名清晰 (避免 `a/b/c`)
- [ ] 删除注释掉的代码

### 测试
- [ ] 关键功能有单元测试
- [ ] 性能变化有基准测试

---

## 七、参考资料

### C++ 现代化
- [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines)
- [Abseil C++ Tips](https://abseil.io/tips/)

### 约束求解
- *Handbook of Constraint Programming* - Rossi et al.
- *Constraint Processing* - Rina Dechter

### 性能优化
- *C++ Concurrency in Action* - Anthony Williams
- *Optimizing Software in C++* - Agner Fog

---

## 八、总结

本重构计划覆盖了:
1. **内存安全**: 智能指针 + RAII
2. **性能**: Trail回溯 + 残基支持 + SoA布局
3. **架构**: 策略模式 + 工厂模式
4. **现代化**: C++17/20/23 特性
5. **创新**: 协程化搜索

**预期收益**:
- **可维护性**: +80% (代码结构清晰,易扩展)
- **性能**: +30-50% (回溯优化,缓存命中率提升)
- **安全性**: 消除内存泄漏与未定义行为
- **开发效率**: +50% (测试覆盖,快速迭代)

建议从**阶段1**开始,渐进式实施,每个阶段完成后进行充分测试。
