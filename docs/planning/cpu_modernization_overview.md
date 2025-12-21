## CPU 现代化综合建议（汇总四份现有文档，当前仅供讨论）

### 1. 现状诊断要点
- 头文件污染与类型不安全：`include/Network.h:9` 使用 `using namespace std;`，`include/Solver.h:13` 及后续均为裸枚举，导致符号冲突风险。
- 内存与所有权散乱：`src/Network.cpp:350-442`、`src/MAC.cpp:15-37` 仍依赖 `new/delete`，`case CA_LMRPC_BIT` 漏 `break` 暗藏泄漏与逻辑缺陷。
- 状态管理成本高：`Network::NewLevel/BackTo` 在回溯时整层复制域数据，`AssignedStack::del`/`update_model_assigned` 存在无效复制，整体回溯开销偏大。
- 搜索框架可读性差：`MAC::enforce` 内含多层嵌套循环与手工回溯，启发式、传播与统计耦合严重，难以扩展。
- 数据结构与容器落后：自建 `arc_que/var_que`、`std::vector<bool>`、裸指针哈希等实现有性能隐患，缺乏缓存友好布局。
- 工程化支持不足：CMake 仍沿用全局指令，缺少目标级属性管理，单元测试/日志体系薄弱，文档与风格不统一。

### 2. 综合改进策略

#### 2.1 架构与模块化
- 用策略模式重写传播器和启发式：将 `VarSelector`、`ValSelector`、`Propagator`、`Stopper` 拆为独立接口，配合工厂注册取代 `switch`，支持按需插拔（参考 `code_improvement_suggestions.md`、`gemini_proposal.md`）。
- 引入 Trail 回溯：以“操作 + 撤销”方式记录域变更，替代 `NewLevel/BackTo` 的整层复制，既减轻内存压力又便于协程/生成器式搜索（`gemini_proposal.md`、`cpu_modernization_survey.md`）。
- 将求解流程拆分为状态机或 C++20 协程：实现 `solve()`/`search_generator()` 生成器，使前进与回溯代码线性化，利于增量求解与统计（四份文档均提到）。
- 解耦解析与求解：建立 `Problem`/`Model` 中间层，让 XCSP3 解析和求解器互不依赖，为单元测试、基准测试、其他前端打基础。

#### 2.2 内存管理与所有权
- 全面切换 `std::unique_ptr`/`std::shared_ptr`：`Network` 内部保存 `std::unique_ptr<IntVar>`、`std::unique_ptr<Tabular>`，对外通过 `std::span` 或裸指针只读视图暴露；`MAC` 析构依赖智能指针自动清理传播器。
- 清除自定义 `Shared<T>`，统一 RAII：将现有自写智能指针替换为标准库，同时为 `AssignedStack`、回溯 trail 等资源编写 RAII 包装。
- 引入对象池/PMR：对频繁创建的小对象（如元组/事件）使用 `std::pmr::monotonic_buffer_resource` 或对象池，减少分配与碎片。

#### 2.3 现代 C++ 特性与类型安全
- 强类型枚举、`[[nodiscard]]`、`constexpr`：从 `enum class ACAlgorithm` 开始，规范访问器 const/`noexcept` 标注，并借助 `[[nodiscard]]` 防止忽略状态值。
- `std::optional/std::expected` 替代魔术常量：例如 `IntVar::head/tail` 返回 `std::optional<int>`，传播器失败返回 `std::expected<ConsistencyState, Error>`。
- 结构化绑定、`std::span`、`std::string_view`：提升接口可读性，避免重复拷贝。
- Concepts 与 ranges：为策略/传播器接口定义概念约束，利用 `std::ranges` 简化变量过滤、最小值搜索等逻辑。
- 逐步引入模块与协程：在核心重构完成后评估 C++20 modules 和协程的编译器可用性，减少头文件耦合并强化搜索表达力。

#### 2.4 数据结构与性能
- 位域与 SoA 布局优化：重构 `IntVar`，把值域数组、bitset、元信息拆分为结构化数组，结合 `std::popcount`/`std::countr_zero`（`<bit>`）提升位运算效率。
- 残基 (residue) + 时间戳：为 `(constraint, var, value)` 缓存上次找到的支持，结合 `uint32_t` 级时间戳避免反复遍历。
- 支持索引预计算：在 `Tabular` 层构建稀疏索引或压缩位集，将 `seek_support` 从 O(|D|×|T|) 降到接近 O(1) 跳转。
- 现代容器替换：
  - `arc_que/var_que` → `std::deque`/`std::queue` 或小型环形缓冲封装。
  - `std::vector<bool>` → `std::vector<uint8_t>`/`absl::InlinedVector`。
  - 指针哈希 → `absl::flat_hash_map` 或 `boost::container::small_vector` + 自定义哈希。
- 缓存域大小与事件列表：避免在 `IntVar::size` 内遍历 bitset；维护增量事件队列减少 `x_evt_` 重建。

#### 2.5 搜索与算法策略
- 统一启发式更新：实现 DOM/WDEG 权重增量更新、最小支持数值启发，与变量选择策略解耦。
- 引入分支管理策略：支持 `BI/NB`、重启、剪枝策略的组合，通过策略接口实现可配置流水线。
- 增量邻接数据：用版本戳管理订阅表，减少重复构建；支持在 trail 中记录结构变化以便回滚。
- 提供性能计数与追踪：集中封装 `SearchStatistics`，可选地用原子或 `std::span` 输出统计，便于后续并行化。

#### 2.6 构建、调试与流程
- 过渡到 Modern CMake：使用 `target_sources`、`target_include_directories`、`target_link_libraries` 管理依赖，清理重复条目，增加 `CMAKE_CXX_STANDARD 20` 和统一警告级别。
- 建立测试与基准体系：在 `tests/` 引入 GTest，配合轻量级 XCSP3 样例覆盖新的策略与数据结构；添加 `ctest`/`benchmark` 钩子。
- 日志与配置抽象：统一 Logger 接口（适配 glog/空实现），支持按需开关调试输出；提供配置文件或命令行解析模块化配置。
- 风格与文档：统一命名、删除死代码、补充 Doxygen 或 Markdown 说明；在 `gemini_doc` 继续维护设计决策记录。

### 3. 建议的实施路线
1. **基础清理**：去除 `using namespace`、枚举强类型化、修复 `switch` fallthrough、补齐 `[[nodiscard]]` 等低风险改动，同时引入统一编码规范。
2. **所有权与容器**：替换核心类的裸指针与自制队列，落地 `unique_ptr`/`span`，修正 `AssignedStack`/`x_evt_` 等数据结构问题。
3. **Trail 与传播框架**：实现回溯 trail，重写 `IntVar` 差分域存储，并对 AC3/FC 等传播器接入 residues 与索引缓存。
4. **搜索与策略化**：拆分 `MAC::enforce`，实现策略接口与生成器式搜索，加入启发式权重更新与可扩展分支策略。
5. **性能精修**：重构 `Tabular` 索引、SoA 布局、缓存统计；加入并行/协程实验性优化。
6. **工程保障**：完成 Modern CMake、单元测试、基准、日志抽象和文档化，确保后续演进可持续。

### 4. 配套措施与风险控制
- **编译器/标准库要求**：推荐默认 C++20，评估 Clang/GCC/MSVC 支持；对协程、`std::expected` 等特性视团队与环境逐步启用。
- **迁移策略**：采用 feature flag/配置切换，逐模块迁移，保留旧路径以便回退；关键改动后运行 `ctest` 与基准对照。
- **性能监控**：为 Trail、残基、SoA 等大变更准备基准工具，记录 CPU/GPU 模式下的差异，避免性能回退。
- **知识沉淀**：持续补充 `gemini_doc`，记录设计决策、实验结果与 TODO，降低团队沟通成本。

> 以上内容整合自 `code_improvement_suggestions.md`、`cpp_modernization_suggestions.md`、`cpu_modernization_survey.md` 与 `gemini_proposal.md`，仅作为后续重构讨论的统一参考稿。
