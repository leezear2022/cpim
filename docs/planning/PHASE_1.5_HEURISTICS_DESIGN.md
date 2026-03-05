# Phase 1.5: 变量选择启发式增强

## 概述

为 CPIM 求解器添加高级变量选择启发式，提升搜索效率。本阶段聚焦于 **DOM/DEG** 和 **DOM/DDEG** 两种经典启发式。

**时间估计**: 1-2 周
**状态**: 设计完成，准备实施
**优先级**: 高（在 Phase 2 之前实现）

---

## 背景：变量选择启发式

### 当前实现（最小域优先）

```cpp
// apps/gmodel_solver.cpp:129-143
int SelectVariable(int level) {
  int best_var = -1;
  int min_size = model_->max_dom_size + 1;

  for (int var = 0; var < model_->num_vars; ++var) {
    if (solution_[var] != -1) continue;  // 已赋值

    int size = model_->GetDomainSize(var);
    if (size > 0 && size < min_size) {
      min_size = size;
      best_var = var;
    }
  }

  return best_var;
}
```

**问题**：
- 只考虑域大小，忽略约束结构
- 在约束密集区域效率低
- 缺少动态反馈机制

---

## 核心启发式设计

### 1. DOM/DEG (Domain over Degree)

**公式**：
```
Score(var) = DomainSize(var) / Degree(var)
```

其中 `Degree(var)` = 包含 var 的约束数量

**直觉**：
- 选择"域小但约束多"的变量
- 约束多 → 传播效果好 → 快速剪枝
- 域小 → 搜索空间小

**示例**：
```
var[0]: domain_size=5, degree=3 → score = 5/3 = 1.67
var[1]: domain_size=4, degree=6 → score = 4/6 = 0.67 ← 优先选择
var[2]: domain_size=3, degree=1 → score = 3/1 = 3.00
```

### 2. DOM/DDEG (Domain over Dynamic Degree)

**公式**：
```
Score(var) = DomainSize(var) / DynamicDegree(var)
```

其中 `DynamicDegree(var)` = 包含 var 且至少有一个未赋值变量的约束数量

**直觉**：
- 动态排除已满足的约束
- 聚焦于"活跃"的约束
- 搜索后期更精准

**对比**：
```
# 初始状态
var[0]: degree=5, ddeg=5 → 相同

# 赋值 3 个变量后（其中 2 个约束完全满足）
var[0]: degree=5, ddeg=3 → ddeg 更准确反映当前约束压力
```

---

## 架构设计

### 可插拔启发式框架

```cpp
// src/solver/common/variable_selector.h
namespace cpim {

// 变量选择器接口
class VariableSelector {
 public:
  virtual ~VariableSelector() = default;

  // 选择下一个变量（返回 -1 表示所有变量已赋值）
  virtual int SelectVariable(
      const std::vector<int>& solution,
      const std::vector<int>& domain_sizes) = 0;

  // 通知赋值事件（用于动态启发式）
  virtual void OnAssignment(int var, int value) {}

  // 通知回溯事件（用于动态启发式）
  virtual void OnBacktrack(int var) {}
};

// 最小域优先（当前默认）
class MinDomainSelector : public VariableSelector {
 public:
  int SelectVariable(
      const std::vector<int>& solution,
      const std::vector<int>& domain_sizes) override;
};

// DOM/DEG 启发式
class DomOverDegSelector : public VariableSelector {
 public:
  explicit DomOverDegSelector(const std::vector<int>& degrees)
      : degrees_(degrees) {}

  int SelectVariable(
      const std::vector<int>& solution,
      const std::vector<int>& domain_sizes) override;

 private:
  std::vector<int> degrees_;  // 静态度数（预计算）
};

// DOM/DDEG 启发式
class DomOverDDegSelector : public VariableSelector {
 public:
  explicit DomOverDDegSelector(
      int num_vars,
      const std::vector<std::vector<int>>& var_to_constraints)
      : num_vars_(num_vars),
        var_to_constraints_(var_to_constraints),
        ddeg_(num_vars) {
    // 初始化动态度数 = 静态度数
    for (int var = 0; var < num_vars; ++var) {
      ddeg_[var] = var_to_constraints_[var].size();
    }
  }

  int SelectVariable(
      const std::vector<int>& solution,
      const std::vector<int>& domain_sizes) override;

  void OnAssignment(int var, int value) override;
  void OnBacktrack(int var) override;

 private:
  void UpdateDynamicDegree(int var, const std::vector<int>& solution);

  int num_vars_;
  std::vector<std::vector<int>> var_to_constraints_;  // var → [约束ID列表]
  std::vector<int> ddeg_;  // 动态度数
};

}  // namespace cpim
```

---

## 实现步骤

### Step 1: 创建 VariableSelector 基础框架

**文件**: `src/solver/common/variable_selector.h` + `.cpp`

**任务**：
1. 定义 `VariableSelector` 接口
2. 实现 `MinDomainSelector`（封装现有逻辑）
3. 添加单元测试

**验证**：用 MinDomainSelector 替换 gmodel_solver.cpp 中的内联代码，确保行为不变

---

### Step 2: 预计算静态度数

**文件**: `src/model/gmodel_adapter.cpp`

**任务**：
```cpp
// 在 GModelAdapter::Build() 中添加度数计算
std::vector<int> ComputeVariableDegrees(const IntermediateModel& model) {
  std::vector<int> degrees(model.num_variables(), 0);
  for (const auto& constraint : model.constraints()) {
    for (int var : constraint.scope()) {
      degrees[var]++;
    }
  }
  return degrees;
}

// 存储到 GModel 结构体
struct GModel {
  // ... 现有字段 ...
  std::vector<int> var_degrees;  // 新增：变量度数
};
```

**验证**：打印几个测试实例的度数分布，检查合理性

---

### Step 3: 实现 DOM/DEG

**文件**: `src/solver/common/variable_selector.cpp`

**实现**：
```cpp
int DomOverDegSelector::SelectVariable(
    const std::vector<int>& solution,
    const std::vector<int>& domain_sizes) {
  int best_var = -1;
  double best_score = std::numeric_limits<double>::max();

  for (int var = 0; var < solution.size(); ++var) {
    if (solution[var] != -1) continue;  // 已赋值

    int dom_size = domain_sizes[var];
    if (dom_size == 0) continue;  // 域为空

    int degree = degrees_[var];
    if (degree == 0) degree = 1;  // 避免除零

    double score = static_cast<double>(dom_size) / degree;
    if (score < best_score) {
      best_score = score;
      best_var = var;
    }
  }

  return best_var;
}
```

**验证**：在 gmodel_solver 中添加 `--heuristic=dom_deg` 选项，运行 TIER 0 测试

---

### Step 4: 实现 DOM/DDEG

**文件**: `src/solver/common/variable_selector.cpp`

**关键**：维护约束的"活跃"状态

```cpp
void DomOverDDegSelector::OnAssignment(int var, int value) {
  // 当变量赋值时，更新相关约束的活跃度
  for (int constraint_id : var_to_constraints_[var]) {
    // 检查约束中是否还有未赋值变量
    // 如果约束完全满足，减少所有涉及变量的 ddeg_
    // （需要访问约束的 scope）
  }
}

// 简化版本：每次选择时重新计算 ddeg
int DomOverDDegSelector::SelectVariable(
    const std::vector<int>& solution,
    const std::vector<int>& domain_sizes) {
  // 重新计算所有变量的动态度数
  std::vector<int> ddeg(num_vars_, 0);
  for (int var = 0; var < num_vars_; ++var) {
    if (solution[var] != -1) continue;

    for (int cid : var_to_constraints_[var]) {
      // 检查约束 cid 是否还有其他未赋值变量
      bool has_unassigned = false;
      for (int scope_var : constraints_[cid].scope) {
        if (scope_var != var && solution[scope_var] == -1) {
          has_unassigned = true;
          break;
        }
      }
      if (has_unassigned) {
        ddeg[var]++;
      }
    }
  }

  // 选择最小 dom/ddeg
  int best_var = -1;
  double best_score = std::numeric_limits<double>::max();

  for (int var = 0; var < num_vars_; ++var) {
    if (solution[var] != -1) continue;

    int dom_size = domain_sizes[var];
    if (dom_size == 0) continue;

    int degree = ddeg[var];
    if (degree == 0) degree = 1;

    double score = static_cast<double>(dom_size) / degree;
    if (score < best_score) {
      best_score = score;
      best_var = var;
    }
  }

  return best_var;
}
```

**优化**：后续可以增量维护 ddeg（通过 OnAssignment/OnBacktrack）

**验证**：运行 TIER 0/1，对比三种启发式的节点数

---

### Step 5: 集成到求解器

**文件**: `apps/gmodel_solver.cpp`

**修改**：
```cpp
// 添加命令行选项
enum class Heuristic {
  MIN_DOMAIN,
  DOM_DEG,
  DOM_DDEG
};

int main(int argc, char* argv[]) {
  // 解析 --heuristic=min_domain|dom_deg|dom_ddeg
  Heuristic heuristic = Heuristic::MIN_DOMAIN;
  for (int i = 1; i < argc; ++i) {
    if (strncmp(argv[i], "--heuristic=", 12) == 0) {
      std::string h = argv[i] + 12;
      if (h == "dom_deg") heuristic = Heuristic::DOM_DEG;
      else if (h == "dom_ddeg") heuristic = Heuristic::DOM_DDEG;
    }
  }

  // 创建对应的 VariableSelector
  std::unique_ptr<VariableSelector> selector;
  switch (heuristic) {
    case Heuristic::MIN_DOMAIN:
      selector = std::make_unique<MinDomainSelector>();
      break;
    case Heuristic::DOM_DEG:
      selector = std::make_unique<DomOverDegSelector>(gmodel.var_degrees);
      break;
    case Heuristic::DOM_DDEG:
      selector = std::make_unique<DomOverDDegSelector>(
          gmodel.num_vars, gmodel.var_to_constraints);
      break;
  }

  // 传递给 SimpleGModelSolver
  SimpleGModelSolver solver(&gmodel, use_persistent, std::move(selector));
}
```

**修改 SimpleGModelSolver**：
```cpp
class SimpleGModelSolver {
 public:
  explicit SimpleGModelSolver(
      GModel* model,
      bool use_persistent,
      std::unique_ptr<VariableSelector> selector)
      : model_(model),
        use_persistent_(use_persistent),
        selector_(std::move(selector)) {}

 private:
  int SelectVariable(int level) {
    // 准备 domain_sizes
    std::vector<int> domain_sizes(model_->num_vars);
    for (int var = 0; var < model_->num_vars; ++var) {
      domain_sizes[var] = model_->GetDomainSize(var);
    }

    return selector_->SelectVariable(solution_, domain_sizes);
  }

  std::unique_ptr<VariableSelector> selector_;
};
```

---

## 性能对比测试

### 基准脚本

```bash
# tests/python/benchmark_heuristics.py
#!/usr/bin/env python3
"""对比不同启发式的性能"""

import subprocess
from pathlib import Path

HEURISTICS = ["min_domain", "dom_deg", "dom_ddeg"]
INSTANCES = [
    "tests/data/bench/queens-12_ext.xml",
    "tests/data/bench/langford-3-10-ext.xml",
    # ... TIER 1 实例
]

for instance in INSTANCES:
    print(f"\n{'='*60}")
    print(f"Instance: {Path(instance).name}")
    print(f"{'='*60}")

    for heuristic in HEURISTICS:
        cmd = [
            "./build/gmodel_solver",
            instance,
            f"--heuristic={heuristic}"
        ]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)

        # 提取统计信息
        output = result.stdout
        positives = extract_int(output, r'Positives: (\d+)')
        negatives = extract_int(output, r'Negatives: (\d+)')

        print(f"{heuristic:12} P={positives:6} N={negatives:6} Total={positives+negatives:6}")
```

**预期结果**（基于文献经验）：
- queens 系列：DOM/DEG 可减少 20-30% 节点数
- langford 系列：DOM/DDEG 可减少 40-60% 节点数
- 约束密集问题：启发式效果显著

---

## GPU 求解器支持（可选）

### GPU DOM/DEG 实现

由于 GPU 求解器当前是完整 MAC + 搜索一体，可以直接复用 CPU 启发式代码：

```cpp
// apps/gmodel_solver.cpp
int SelectVariable(int level) {
  // GPU 也使用 VariableSelector 接口
  std::vector<int> domain_sizes(model_->num_vars);
  for (int var = 0; var < model_->num_vars; ++var) {
    domain_sizes[var] = model_->GetDomainSize(var);
  }

  return selector_->SelectVariable(solution_, domain_sizes);
}
```

**注意**：GPU 的 DOM/DDEG 需要访问约束结构，当前 GModel 已包含完整信息。

---

## 数据结构增强

### GModel 添加约束索引

```cpp
// include/GModel.cuh
struct GModel {
  // ... 现有字段 ...

  // Phase 1.5 新增：启发式支持
  std::vector<int> var_degrees;  // 变量度数
  std::vector<std::vector<int>> var_to_constraints;  // var → [约束ID]
  std::vector<std::vector<int>> constraint_scopes;   // 约束 → [变量列表]
};
```

### GModelAdapter 构建索引

```cpp
// src/model/gmodel_adapter.cpp
GModel GModelAdapter::Build(const IntermediateModel& model, ...) {
  // ... 现有代码 ...

  // 构建变量→约束索引
  gmodel.var_to_constraints.resize(num_vars);
  for (int cid = 0; cid < num_constraints; ++cid) {
    for (int var : model.constraints()[cid].scope()) {
      gmodel.var_to_constraints[var].push_back(cid);
    }
  }

  // 存储约束 scope（用于 DDEG 计算）
  gmodel.constraint_scopes.resize(num_constraints);
  for (int cid = 0; cid < num_constraints; ++cid) {
    gmodel.constraint_scopes[cid] = model.constraints()[cid].scope();
  }

  // 计算度数
  gmodel.var_degrees.resize(num_vars);
  for (int var = 0; var < num_vars; ++var) {
    gmodel.var_degrees[var] = gmodel.var_to_constraints[var].size();
  }

  return gmodel;
}
```

---

## 实施时间表

| 阶段 | 任务 | 时间 | 验证标准 |
|------|------|------|----------|
| 1 | VariableSelector 框架 | 1 天 | MinDomainSelector 替换内联代码，TIER 0 节点数不变 |
| 2 | GModel 度数计算 | 0.5 天 | 打印度数分布，手工验证 queens-4 |
| 3 | DOM/DEG 实现 | 1 天 | TIER 0 测试通过，节点数减少 |
| 4 | DOM/DDEG 实现 | 2 天 | TIER 0/1 对比测试，性能报告 |
| 5 | 性能对比测试 | 1 天 | 生成完整 benchmark 报告 |
| 6 | 文档与总结 | 0.5 天 | 更新 CLAUDE.md |

**总计**：6 天（1.2 周）

---

## 测试与验证

### 正确性验证

```bash
# 1. 确保所有启发式找到相同的解
./build/gmodel_solver tests/data/bench/queens-4_ext.xml --heuristic=min_domain
./build/gmodel_solver tests/data/bench/queens-4_ext.xml --heuristic=dom_deg
./build/gmodel_solver tests/data/bench/queens-4_ext.xml --heuristic=dom_ddeg

# 2. 对比解的一致性（可能有多解，但都应是有效解）
python3 tests/python/verify_solutions.py
```

### 性能验证

```bash
# TIER 0/1 完整对比
python3 tests/python/benchmark_heuristics.py --tier=0
python3 tests/python/benchmark_heuristics.py --tier=1

# 输出格式：
# Instance              MinDomain  DOM/DEG  DOM/DDEG  Speedup
# queens-12_ext         1023       821      768       1.33x
# langford-3-10         8234       5123     3876      2.12x
```

---

## 预期收益

### 性能提升

**文献数据**（Lecoutre et al., 2004）：
- DOM/DEG: 平均减少 25% 节点数
- DOM/DDEG: 平均减少 40% 节点数（结构化问题）

**CPIM 预期**：
- queens-12: 1000+ → 700-800 节点
- langford-3-11: 18000+ → 10000-12000 节点
- 约束密集问题（composed, driver）：30-50% 改进

### 代码质量

- ✅ 可插拔架构，易于添加新启发式
- ✅ CPU/GPU 代码复用
- ✅ 为 Phase 2 Propagator 框架铺路

---

## 参考文献

1. **Bessière & Régin (1996)**: "MAC and Combined Heuristics: Two Reasons to Forsake FC (and CBJ?) on Hard Problems"
2. **Boussemart et al. (2004)**: "Boosting Systematic Search by Weighting Constraints" (DOM/WDEG 原始论文)
3. **Lecoutre et al. (2004)**: "A Greedy Approach to Establish Singleton Arc Consistency"

---

## 下一步行动

**立即开始实施 Step 1**：
1. 创建 `src/solver/common/variable_selector.h`
2. 实现 `VariableSelector` 接口和 `MinDomainSelector`
3. 修改 `apps/gmodel_solver.cpp` 使用新接口
4. 运行 TIER 0 验证行为一致性

准备好了吗？
