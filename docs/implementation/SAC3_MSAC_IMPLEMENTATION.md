# SAC3 和条件触发 MSAC 实现文档

## 概述

本文档描述了 SAC 优化路线图中第五步（SAC3）和第六步（条件触发 MSAC）的实现细节。

### 优化目标

| 阶段 | 目标 | 核心改进 |
|------|------|----------|
| SAC3 | 队列驱动的 SAC | 从变量粒度 dirty set 改为 probe 粒度队列，减少无效 probes |
| MSAC | 搜索中的 SAC | 在搜索过程中有条件执行 SAC，提前剪枝减少搜索节点 |

---

## 1. SAC3 实现

### 1.1 核心数据结构

#### ProbeQueue 类

**文件**: [src/solver/gpu/GModelSolver.cu:326-442](../src/solver/gpu/GModelSolver.cu#L326-L442)

```cpp
class ProbeQueue {
 public:
  static constexpr int kBitsPerWord = 32;

  // 检查值是否在域中（直接位域检查）
  bool HasValue(int var, int val) const;

  // 入队所有 (var, val) 对（初始化时使用）
  void EnqueueAll();

  // 单值入队（带去重）
  bool Enqueue(int var, int val);

  // 入队邻域变量的所有值（删值后调用）
  void EnqueueNeighborhood(int var);

  // 批量出队
  int DequeueBatch(int max_count, std::vector<ProbeTask>& tasks);

 private:
  std::deque<std::pair<int, int>> queue_;     // FIFO 队列
  std::vector<std::vector<bool>> in_queue_;   // 去重标记
  std::vector<std::set<int>> neighbors_;      // 邻域表
};
```

**关键特性**:
- `in_queue_[var][val]`: O(1) 去重检查
- `EnqueueNeighborhood()`: SAC3 核心 - 删值后只入队受影响的邻域
- `HasValue()`: 直接位域检查，避免遍历域

### 1.2 EnforceSAC3 函数

**文件**: [src/solver/gpu/GModelSolver.cu:538-698](../src/solver/gpu/GModelSolver.cu#L538-L698)

**算法流程**:

```
1. 初始化 ProbeQueue，入队所有 (var, val)
2. while (!queue.Empty()):
     a. 批量出队 tasks (max 256)
     b. Stage 选择 (复用 AutoStageSelector)
     c. 执行 batch probe (Batch2PersistentManager)
     d. 处理失败的 probes:
        - RemoveValue(var, val)
        - EnqueueNeighborhood(var)  // SAC3 核心：入队邻域
     e. 保存当前域大小
     f. EnforceGAC()
     g. [增强] 追踪 GAC 级联删值的变量，入队其邻域
3. 返回删除数
```

**GAC 级联删值追踪（增强）**:

GAC 传播可能会删除更多值（级联效应），这些被修改的变量也需要入队：

```cpp
// 保存 GAC 前的域大小
std::vector<int> pre_gac_dom_sizes(model_->num_vars);
for (int v = 0; v < model_->num_vars; ++v) {
  pre_gac_dom_sizes[v] = model_->GetDomainSize(v);
}

// 执行 GAC
GacStats gac_stats = model_->EnforceGAC(false);

// 追踪 GAC 级联删值的变量
if (gac_stats.deletions > 0) {
  for (int v = 0; v < model_->num_vars; ++v) {
    if (model_->GetDomainSize(v) < pre_gac_dom_sizes[v]) {
      probe_queue.EnqueueNeighborhood(v);
    }
  }
}
```

**与 SAC1 的对比**:

| 特性 | SAC1 | SAC3 |
|------|------|------|
| 任务粒度 | 变量 (dirty set) | probe (var, val) |
| 增量更新 | 标记邻域变量 | 精确入队邻域值 |
| 去重机制 | dirty_[var] | in_queue_[var][val] |
| 无效 probe | 可能较多 | 最小化 |

### 1.3 配置接口

**文件**: [include/GModelSolver.h:11-16](../include/GModelSolver.h#L11-L16)

```cpp
enum class SACMode {
  kSAC1,    // SAC1: 每轮全量检查 (dirty set 变量粒度)
  kSAC3,    // SAC3: 队列驱动 (probe 粒度)
  kAuto     // 自动选择（默认 SAC1）
};

// 使用方式
solver.SetSACMode(SACMode::kSAC3);
```

---

## 2. 条件触发 MSAC 实现

### 2.1 配置结构

**文件**: [include/GModelSolver.h:20-27](../include/GModelSolver.h#L20-L27)

```cpp
struct GpuMSACConfig {
  bool enabled = false;              // 是否启用
  int max_level = 5;                 // 只在前 N 层执行
  int min_domain_size = 10;          // 平均域大于此值才执行
  double min_fail_rate = 0.3;        // 上层失败率高于此值才执行
  int max_probes_per_node = 100;     // 每节点最多 probe 数
  bool lightweight = true;           // 轻量级模式（仅检查邻域）
};
```

### 2.2 触发条件判断

**文件**: [src/solver/gpu/GModelSolver.cu:704-733](../src/solver/gpu/GModelSolver.cu#L704-L733)

```cpp
bool GModelSolver::ShouldEnforceMSAC(int level, int var,
                                      const GpuSearchStatistics& stats) {
  if (!msac_config_.enabled) return false;

  // 条件 1: 只在前 N 层执行
  if (level > msac_config_.max_level) return false;

  // 条件 2: 平均域大小检查
  double avg_domain = ...;
  if (avg_domain < msac_config_.min_domain_size) return false;

  // 条件 3: 上层失败率检查
  double fail_rate = last_level_failures_ / last_level_positives_;
  if (fail_rate < msac_config_.min_fail_rate) return false;

  return true;
}
```

### 2.3 轻量级 MSAC 执行

**文件**: [src/solver/gpu/GModelSolver.cu:735-807](../src/solver/gpu/GModelSolver.cu#L735-L807)

**算法**:
1. 获取刚赋值变量的邻域
2. 收集邻域变量的所有域值作为 probe 任务（限制 max_probes_per_node）
3. 执行 batch probe
4. 删除失败值，执行 GAC
5. 返回删除数（-1 表示不一致）

### 2.4 Search() 集成

**文件**: [src/solver/gpu/GModelSolver.cu:203-232](../src/solver/gpu/GModelSolver.cu#L203-L232)

```cpp
// GAC 成功后、递归搜索前
if (ShouldEnforceMSAC(new_level, var, stats)) {
  int msac_result = EnforceLightweightMSAC(var, stats);
  if (msac_result == -1) {
    // MSAC 检测到不一致，回溯
    model_->BacktrackTo(new_level - 1);
    stats.num_negative++;
    last_level_failures_++;
    continue;
  }
}

// 更新统计（用于子层的触发判断）
int prev_failures = last_level_failures_;
int prev_positives = last_level_positives_;
last_level_failures_ = 0;
last_level_positives_ = 0;

// 递归搜索
Search(new_level, stats, time_limit, start_time);

// 恢复统计
last_level_failures_ = prev_failures;
last_level_positives_ = prev_positives + 1;
```

---

## 3. 代码导航

### 3.1 头文件

| 文件 | 内容 |
|------|------|
| [include/GModelSolver.h:11-16](../include/GModelSolver.h#L11-L16) | SACMode 枚举 |
| [include/GModelSolver.h:20-27](../include/GModelSolver.h#L20-L27) | GpuMSACConfig 结构体 |
| [include/GModelSolver.h:74-76](../include/GModelSolver.h#L74-L76) | SetSACMode() 接口 |
| [include/GModelSolver.h:91-92](../include/GModelSolver.h#L91-L92) | SetMSACConfig() 接口 |
| [include/GModelSolver.h:98-100](../include/GModelSolver.h#L98-L100) | EnforceSAC3() 声明 |
| [include/GModelSolver.h:127-131](../include/GModelSolver.h#L127-L131) | MSAC 辅助函数声明 |

### 3.2 实现文件

| 文件 | 内容 |
|------|------|
| [src/solver/gpu/GModelSolver.cu:326-442](../src/solver/gpu/GModelSolver.cu#L326-L442) | ProbeQueue 类 |
| [src/solver/gpu/GModelSolver.cu:538-698](../src/solver/gpu/GModelSolver.cu#L538-L698) | EnforceSAC3() |
| [src/solver/gpu/GModelSolver.cu:704-733](../src/solver/gpu/GModelSolver.cu#L704-L733) | ShouldEnforceMSAC() |
| [src/solver/gpu/GModelSolver.cu:735-807](../src/solver/gpu/GModelSolver.cu#L735-L807) | EnforceLightweightMSAC() |
| [src/solver/gpu/GModelSolver.cu:72-103](../src/solver/gpu/GModelSolver.cu#L72-L103) | Solve() SAC 模式选择 |
| [src/solver/gpu/GModelSolver.cu:203-232](../src/solver/gpu/GModelSolver.cu#L203-L232) | Search() MSAC 集成 |

### 3.3 应用程序

| 文件 | 内容 |
|------|------|
| [apps/compare_cpu_gpu.cpp:27](../apps/compare_cpu_gpu.cpp#L27) | --sac_mode 命令行参数 |
| [apps/compare_cpu_gpu.cpp:163-167](../apps/compare_cpu_gpu.cpp#L163-L167) | SAC 模式解析 |
| [apps/compare_cpu_gpu.cpp:130](../apps/compare_cpu_gpu.cpp#L130) | SetSACMode() 调用 |

---

## 4. 使用方法

### 4.1 命令行

```bash
# SAC1 模式（默认）
./compare_cpu_gpu --input=problem.xml --sac --sac_mode=sac1 --gpu_only

# SAC3 模式
./compare_cpu_gpu --input=problem.xml --sac --sac_mode=sac3 --gpu_only
```

### 4.2 编程接口

```cpp
#include "GModelSolver.h"

// 创建求解器
GModelSolver solver(&gmodel, verbose);

// 启用 SAC 预处理
solver.SetSAC1Preprocessing(true);

// 选择 SAC3 模式
solver.SetSACMode(SACMode::kSAC3);

// 配置 MSAC（可选）
GpuMSACConfig msac_config;
msac_config.enabled = true;
msac_config.max_level = 5;
msac_config.min_fail_rate = 0.3;
solver.SetMSACConfig(msac_config);

// 求解
GpuSearchStatistics stats = solver.Solve(time_limit);
```

---

## 5. 性能验证

### 5.1 测试结果

| 问题 | SAC1 时间 | SAC3 时间 | SAC 删除 | 解一致 |
|------|----------|----------|---------|--------|
| queens-4 | 0.006s | 0.005s | 8 | ✓ |
| langford-2-4 | 0.034s | 0.032s | 20 | ✓ |
| langford-3-9 | 0.155s | 0.136s | 0 | ✓ |

### 5.2 验证命令

```bash
# 对比 SAC1 和 SAC3 结果
echo "=== SAC1 ===" && \
./compare_cpu_gpu --input=../tests/data/bench/queens-4_ext.xml --sac --sac_mode=sac1 --gpu_only 2>&1 | \
grep -E "SAC|解:|正向|回溯"

echo "=== SAC3 ===" && \
./compare_cpu_gpu --input=../tests/data/bench/queens-4_ext.xml --sac --sac_mode=sac3 --gpu_only 2>&1 | \
grep -E "SAC|解:|正向|回溯"
```

---

## 6. 设计决策

### 6.1 为什么使用 deque 而不是 vector?

ProbeQueue 使用 `std::deque` 实现 FIFO 队列：
- `pop_front()` O(1) 时间复杂度
- 避免 vector 头部删除的 O(n) 开销
- 适合队列操作模式

### 6.2 为什么使用二维 in_queue_ 而不是 set?

使用 `std::vector<std::vector<bool>>` 而不是 `std::set<pair<int,int>>`：
- O(1) 查找和更新
- 内存局部性更好
- 避免 set 的 O(log n) 开销

### 6.3 GpuMSACConfig vs MSACConfig

项目中存在两个 MSAC 配置结构体：
- `cpim::MSACConfig` (Solver.h) - CPU 求解器使用
- `cpim::GpuMSACConfig` (GModelSolver.h) - GPU 求解器使用

它们服务于不同的求解器，参数设计也有所不同。

---

## 7. 相关文档

- [SAC 优化路线图](../planning/SAC_OPTIMIZATION_ROADMAP.md)
- [Batch AC-GPU 设计](../planning/BATCH_AC_GPU_COMPREHENSIVE_DESIGN_V2.md)
- [架构概述](../architecture/ARCHITECTURE.md)
