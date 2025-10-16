# CPIM 现代化备忘录

**日期**：2025-10-15
**状态**：计划阶段
**参考**：Google OR-Tools 最佳实践

---

## 📋 快速索引

- **完整计划**：`MODERNIZATION_PLAN_V2.md`
- **原始计划**：`MODERNIZATION_PLAN.md`（基础版）
- **参考文档**：`aig_docs/` 目录
- **项目说明**：`CLAUDE.md`

---

## 🎯 核心目标

将 CPIM 从**研究原型**改造为**工业级约束求解器库**，参考 Google OR-Tools 的架构：

1. ✅ **库化**：核心功能可作为库被其他程序调用
2. ✅ **模块化**：清晰的模块边界（base/model/solver/algorithms）
3. ✅ **类型安全**：强类型、智能指针、零内存泄漏
4. ✅ **高性能**：Trail 回溯、Abseil 容器、缓存优化
5. ✅ **易用性**：流式 API、丰富示例、完善文档

---

## 🛠️ 技术栈

### 核心依赖
- **C++20**：现代语言特性
- **Abseil**：Google 的 C++ 基础库（必须）
  - `absl::Span`：零开销视图
  - `absl::flat_hash_map`：高性能哈希表
  - `absl::Status/StatusOr`：错误处理
  - `absl::InlinedVector`：小对象优化
- **GoogleTest**：单元测试框架
- **Google Benchmark**：性能测试

### 构建系统
- **CMake**（主要）：3.20+，Modern CMake 风格
- **Bazel**（可选）：Google 标准构建系统

### 现有依赖（保持）
- CUDA Toolkit（GPU 加速）
- libxml2 + Xerces-C（XCSP3 解析）
- glog（日志）

---

## 📁 新的目录结构

```
cpim/
├── cpim/                      # 核心库（参考 ortools/sat/）
│   ├── base/                  # 基础设施
│   │   ├── types.h            # 基础类型
│   │   ├── bitset.h           # 位集合
│   │   ├── trail.h            # Trail 回溯系统 ⭐
│   │   └── logging.h
│   ├── model/                 # 模型层
│   │   ├── variable.h         # IntVar
│   │   ├── constraint.h       # 约束接口
│   │   ├── model.h            # Model 类（依赖注入容器）⭐
│   │   └── model_builder.h    # 流式 API ⭐
│   ├── solver/                # 求解器层
│   │   ├── propagator.h       # 传播器接口 ⭐
│   │   ├── search.h           # 搜索引擎 ⭐
│   │   └── heuristics/        # 启发式策略
│   ├── algorithms/            # 算法实现
│   │   ├── ac3.h
│   │   ├── ac3_bit.h
│   │   └── ...
│   └── util/
├── tests/                     # 单元测试（每个模块对应）
├── examples/                  # 示例程序
│   ├── n_queens.cc
│   └── sudoku.cc
├── parsers/                   # 解析器（独立）
│   └── xcsp3/
└── tools/                     # 工具
    └── cpim_solver.cc         # 命令行求解器
```

**⭐ = 核心创新点**

---

## 🚀 实施路线（7个阶段）

### 阶段 0：技术栈升级（1周）⚡ 立即开始
- [ ] 安装 Abseil（通过 vcpkg 或系统包管理器）
- [ ] 升级 CMakeLists.txt（C++20 + Abseil）
- [ ] 集成 GoogleTest
- [ ] 编写第一个 Abseil 测试验证环境

**关键命令**：
```bash
# 安装 Abseil
vcpkg install abseil

# 测试编译
cmake -B build -DCMAKE_TOOLCHAIN_FILE=[vcpkg]/scripts/buildsystems/vcpkg.cmake
cmake --build build
```

---

### 阶段 1：模块化重构（3-4周）
- [ ] 创建新的目录结构
- [ ] 定义核心接口（Variable, Constraint, Model）
- [ ] 实现 ModelBuilder（流式 API）
- [ ] 将现有代码迁移到新结构
- [ ] 编写模块测试

**核心产出**：
```cpp
// 流式 API 示例
cpim::ModelBuilder builder;
auto x = builder.NewIntVar(0, 10, "x");
builder.AddEquality(x, 5);
auto model = builder.Build();
```

---

### 阶段 2：依赖注入框架（2-3周）⭐ 关键阶段
- [ ] 实现 Model 的依赖注入容器
- [ ] 定义 PropagatorInterface
- [ ] 重写 AC3 为传播器
- [ ] 实现 PropagationEngine
- [ ] 适配其他算法（AC3bit, FC, SAC）

**核心模式**：
```cpp
class Model {
public:
    template<typename Service>
    Service* GetOrCreate();  // 依赖注入
};

class PropagatorInterface {
public:
    virtual absl::Status Propagate() = 0;
    virtual int priority() const = 0;
};
```

---

### 阶段 3：Trail 回溯系统（2-3周）⭐ 性能关键
- [ ] 实现 Trail 系统（增量回溯）
- [ ] IntVar 改用差分域
- [ ] 域大小缓存
- [ ] 使用 Abseil 容器优化
- [ ] 性能基准测试

**目标**：回溯性能提升 **30-50%**

**核心技术**：
```cpp
class Trail {
public:
    void NewLevel();
    void BacktrackTo(int level);

    template<typename T>
    void SaveAndSet(T& target, const T& new_value);
};
```

---

### 阶段 4：搜索引擎（3-4周）
- [ ] 实现决策管理
- [ ] 实现启发式选择器（DOM_MIN, WDEG, etc.）
- [ ] 重写搜索引擎（清晰、递归）
- [ ] 统计信息收集

---

### 阶段 5：Modern CMake + Bazel（1-2周）
- [ ] 重写 CMakeLists.txt（模块化、target-based）
- [ ] 配置 Bazel（可选）
- [ ] 依赖管理（vcpkg/Conan）

---

### 阶段 6：API 与示例（2-3周）
- [ ] 完善流式 API
- [ ] 编写示例（N-Queens, Sudoku）
- [ ] 命令行工具（absl::flags）
- [ ] 编写文档

---

### 阶段 7：高级特性（持续）
- [ ] Python 绑定（pybind11）
- [ ] Protocol Buffers 模型定义
- [ ] SIMD 优化

---

## 📊 预期收益

| 指标 | 当前 | 目标 | 提升 |
|------|------|------|------|
| **回溯性能** | baseline | 1.5x | +50% |
| **内存安全** | 多处泄漏 | 零泄漏 | 100% |
| **测试覆盖率** | 0% | 80% | +80% |
| **API 易用性** | 2/10 | 9/10 | +350% |
| **代码可维护性** | 困难 | 简单 | 质变 |

---

## ⚠️ 风险与应对

### 风险 1：学习曲线
- **影响**：Abseil、依赖注入等新技术需要学习
- **应对**：参考 OR-Tools 源码，逐步引入
- **时间成本**：+20%

### 风险 2：重构周期长
- **影响**：完整重构需 4-5 个月
- **应对**：分阶段交付，每阶段独立可用
- **优先级**：阶段 0-3 是核心（必须），4-7 可选

### 风险 3：性能回退
- **影响**：抽象层可能影响性能
- **应对**：每阶段做基准测试，使用零开销抽象
- **监控**：持续性能监控脚本

---

## 🎯 立即行动（第一周）

### Day 1-2：环境准备
```bash
# 1. 安装 Abseil
vcpkg install abseil gtest benchmark

# 2. 创建分支
git checkout -b modernization-phase0

# 3. 备份当前代码
git tag v0-original
```

### Day 3-4：集成测试
```cpp
// tests/abseil_test.cc
#include <gtest/gtest.h>
#include <absl/container/flat_hash_map.h>
#include <absl/strings/str_cat.h>

TEST(AbseilTest, FlatHashMap) {
    absl::flat_hash_map<int, std::string> map;
    map[1] = "one";
    EXPECT_EQ(map[1], "one");
}

TEST(AbseilTest, StrCat) {
    std::string s = absl::StrCat("x", 1, " = ", 10);
    EXPECT_EQ(s, "x1 = 10");
}
```

### Day 5：CMake 升级
```cmake
# CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
project(cpim VERSION 1.0.0 LANGUAGES CXX CUDA)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(absl REQUIRED)
find_package(GTest REQUIRED)

# 测试目标
enable_testing()
add_executable(abseil_test tests/abseil_test.cc)
target_link_libraries(abseil_test
    GTest::gtest_main
    absl::flat_hash_map
    absl::strings
)

gtest_discover_tests(abseil_test)
```

### Day 6-7：验证与文档
- 运行测试：`ctest --output-on-failure`
- 编译原项目，确保兼容
- 更新 CLAUDE.md

---

## 📚 参考资源

### 代码参考
- [Google OR-Tools](https://github.com/google/or-tools) - 主要参考
- [CP-SAT Solver](https://github.com/google/or-tools/tree/main/ortools/sat) - 约束求解器实现

### 技术文档
- [Abseil C++](https://abseil.io/) - 官方文档
- [GoogleTest User's Guide](https://google.github.io/googletest/)
- [Modern CMake](https://cliutils.gitlab.io/modern-cmake/)

### 内部文档
- `aig_docs/cpu_modernization_overview.md` - 现代化建议汇总
- `aig_docs/cpim_modernization_plan.md` - 原始现代化计划
- `aig_docs/CPIM_COMPREHENSIVE_REFACTORING_GUIDE.md` - 重构详细指南

---

## 📝 进度跟踪

### 当前状态
- [x] 分析现有代码
- [x] 学习 OR-Tools 架构
- [x] 制定现代化计划 V2
- [ ] **→ 阶段 0：技术栈升级**（下一步）

### 里程碑
- [ ] 2025-10 月底：完成阶段 0-1
- [ ] 2025-11 月底：完成阶段 2-3（核心）
- [ ] 2025-12 月底：完成阶段 4-5
- [ ] 2026-01 月：完成阶段 6，发布 v1.0

---

## 💡 关键决策记录

### 为什么选择 Abseil？
1. **Google 标准**：OR-Tools、TensorFlow 等大型项目都用
2. **性能优秀**：`flat_hash_map` 比 `std::unordered_map` 快 30%
3. **零开销抽象**：`absl::Span` 编译后等同于裸指针
4. **C++ stdlib 增强**：填补标准库空白

### 为什么用依赖注入？
1. **解耦合**：模块间通过接口通信
2. **可测试**：可以注入 mock 对象
3. **灵活性**：运行时组合不同策略
4. **OR-Tools 实践**：已被验证的成功模式

### 为什么分 7 个阶段？
1. **降低风险**：每阶段独立验证
2. **渐进交付**：阶段 0-3 完成即可用
3. **灵活调整**：后续阶段可根据需要调整
4. **持续集成**：每阶段合并后其他人可继续开发

---

## 🔗 快速链接

- **当前项目**：`/home/lee/Codes/cpim/`
- **计划文档**：
  - 完整计划：`MODERNIZATION_PLAN_V2.md`
  - 本备忘录：`MODERNIZATION_MEMO.md`
- **参考代码**：
  - OR-Tools：https://github.com/google/or-tools
  - CP-SAT：https://github.com/google/or-tools/tree/main/ortools/sat
- **技术文档**：
  - Abseil：https://abseil.io/
  - GoogleTest：https://google.github.io/googletest/

---

**更新日期**：2025-10-15
**下次审查**：完成阶段 0 后

---

_"Good code is its own best documentation." - Steve McConnell_
