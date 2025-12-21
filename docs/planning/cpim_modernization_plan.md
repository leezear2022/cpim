# CPIM 代码现代化计划

## 当前代码的现代化问题分析

### 1. 裸指针问题
```cpp
// 当前代码 - 不够现代
Tabular* constraint_;
IntVar* var_;
int value_;

// 问题：
// - 没有明确的 ownership 语义
// - 容易造成悬空指针
// - 缺乏类型安全
```

### 2. 返回裸指针向量
```cpp
// 当前代码 - 不够现代
std::vector<IntVar*> get_vars() const;

// 问题：
// - 返回裸指针向量，ownership 不明确
// - 调用者不知道是否需要释放内存
// - 容易造成内存泄漏
```

### 3. 复杂的裸指针映射
```cpp
// 当前代码 - 不够现代
std::unordered_map<IntVar*, std::vector<Tabular*>> subscription;

// 问题：
// - 键和值都是裸指针
// - 复杂的嵌套结构
// - 难以维护和调试
```

## 现代化改进方案

### 阶段 1: 基础现代化（已完成）
- ✅ 使用 `std::unique_ptr` 管理所有权
- ✅ 使用 `std::span` 提供零开销视图
- ✅ 启用 C++20 标准

### 阶段 2: 类型安全和错误处理

#### 2.1 使用 `std::optional` 替代特殊值
```cpp
// 当前代码
int head(int level) const {
    // 返回 -1 表示无效
    if (empty) return -1;
    return first_value;
}

// 现代化代码
std::optional<int> head(int level) const {
    if (empty) return std::nullopt;
    return first_value;
}

// 使用方式
if (auto val = var->head(level)) {
    process(*val);
}
```

#### 2.2 使用 `std::expected` 进行错误处理
```cpp
// 当前代码
bool assign_variable(int var_id, int value) {
    // 返回 false 表示失败，但不知道原因
    if (invalid_var) return false;
    if (invalid_value) return false;
    return true;
}

// 现代化代码
enum class AssignmentError {
    InvalidVariable,
    InvalidValue,
    DomainEmpty
};

std::expected<void, AssignmentError> assign_variable(int var_id, int value) {
    if (invalid_var) return std::unexpected(AssignmentError::InvalidVariable);
    if (invalid_value) return std::unexpected(AssignmentError::InvalidValue);
    return {};
}

// 使用方式
auto result = network.assign_variable(1, 5);
if (!result) {
    switch (result.error()) {
        case AssignmentError::InvalidVariable:
            std::cerr << "无效变量\n";
            break;
        case AssignmentError::InvalidValue:
            std::cerr << "无效值\n";
            break;
    }
}
```

### 阶段 3: 容器和数据结构现代化

#### 3.1 使用 `std::ranges` 简化算法
```cpp
// 当前代码
std::vector<IntVar*> get_unassigned_vars(int level) const {
    std::vector<IntVar*> result;
    for (auto* var : vars) {
        if (!var->assigned(level)) {
            result.push_back(var);
        }
    }
    return result;
}

// 现代化代码
auto get_unassigned_vars(int level) const {
    return get_vars_span() | std::views::filter([level](const IntVar* var) {
        return !var->assigned(level);
    });
}
```

#### 3.2 使用 `std::flat_map` 提升性能
```cpp
// 当前代码
std::unordered_map<IntVar*, std::vector<Tabular*>> subscription;

// 现代化代码
std::flat_map<IntVar*, std::vector<Tabular*>> subscription;
// 或者使用 SoA 布局
struct SubscriptionData {
    std::vector<IntVar*> variables;
    std::vector<std::vector<Tabular*>> constraints;
    std::vector<size_t> offsets;
};
```

### 阶段 4: 高级 C++20/23 特性

#### 4.1 使用 Concepts 约束模板
```cpp
template<typename T>
concept VariableLike = requires(T t) {
    { t.id() } -> std::convertible_to<int>;
    { t.size(int{}) } -> std::convertible_to<int>;
    { t.assigned(int{}) } -> std::convertible_to<bool>;
};

template<VariableLike T>
class ModernNetwork {
    // 使用 Concepts 确保类型安全
};
```

#### 4.2 使用协程简化搜索逻辑
```cpp
// 当前代码 - 复杂的递归搜索
bool solve(int level) {
    if (all_assigned()) return true;
    
    IntVar* var = select_variable();
    for (int value : var->domain()) {
        if (assign_and_propagate(var, value)) {
            if (solve(level + 1)) return true;
            backtrack();
        }
    }
    return false;
}

// 现代化代码 - 协程生成器
std::generator<Solution> solve() {
    if (!propagate()) co_return;
    
    if (all_assigned()) {
        co_yield extract_solution();
        co_return;
    }
    
    IntVar* var = select_variable();
    for (int value : var->domain()) {
        ScopedAssignment assign(var, value);
        if (propagate()) {
            for (auto& sol : solve()) {
                co_yield sol;
            }
        }
    }
}
```

## 实施计划

### 立即可以实施的改进

1. **使用 `std::optional` 替代特殊值**
   - `IntVar::head()` 和 `IntVar::tail()` 方法
   - `IntVar::next()` 和 `IntVar::prev()` 方法
   - 时间投入：1-2小时

2. **使用 `std::ranges` 简化算法**
   - 变量过滤和查找
   - 约束检查
   - 时间投入：2-3小时

3. **使用 `std::expected` 进行错误处理**
   - 变量赋值操作
   - 约束传播
   - 时间投入：3-4小时

### 中期改进

4. **使用 Concepts 约束模板**
   - 定义变量和约束的概念
   - 模板函数类型安全
   - 时间投入：2-3小时

5. **使用 `std::flat_map` 优化性能**
   - 订阅关系存储
   - 邻居关系存储
   - 时间投入：2-3小时

### 长期改进

6. **使用协程简化搜索**
   - 搜索逻辑重构
   - 回溯管理
   - 时间投入：5-6小时

7. **使用 SoA 布局优化内存**
   - 变量数据重组
   - 缓存友好访问
   - 时间投入：4-5小时

## 预期收益

### 代码质量
- **类型安全**：消除裸指针和特殊值
- **错误处理**：明确的错误类型和传播
- **可读性**：使用现代 C++ 特性

### 性能提升
- **零开销抽象**：`std::span` 和 `std::ranges`
- **缓存友好**：SoA 布局和 `std::flat_map`
- **编译时优化**：Concepts 和 `constexpr`

### 开发效率
- **调试友好**：明确的错误信息
- **测试覆盖**：类型安全的接口
- **维护性**：清晰的代码结构

## 总结

当前 CPIM 项目中的代码确实还不够现代化，主要问题包括：

1. **裸指针使用**：缺乏明确的 ownership 语义
2. **特殊值表示**：使用 -1 等魔术数字表示无效状态
3. **复杂的嵌套结构**：难以维护和调试
4. **缺乏错误处理**：没有明确的错误类型

通过逐步应用现代 C++ 特性，可以显著提升代码质量、性能和可维护性。建议从基础的类型安全改进开始，逐步引入更高级的特性。
