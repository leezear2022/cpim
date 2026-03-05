# SAC 优化路线图

> **创建日期**: 2026-01-04
> **更新日期**: 2026-01-04
>
> **目标**: 从当前 SAC1 实现逐步演进到高效的 SAC3/MSAC
>
> **状态**: 全部完成

---

## 推荐推进顺序

```
Stage 2 Precheck → dirty set（半个 SAC3）→ 完整 SAC3 → 条件触发 MSAC
       [完成]            [完成]              [完成]           [完成]
```

---

## 第一步：Stage 2 Precheck（高优先级） [已完成]

**特点**: 最局部、风险最低、立刻见收益

**任务**:
1. 在 Stage 2 kernel 中实现 precheck 逻辑（当前未读取 `enable_precheck` 字段）
2. 统一 Stage 1/Stage 2 的 `precheck_count`/`short_circuit_count` 统计口径
3. 方便后续调参和回归测试

**预期收益**:
- 减少 50-80% 无效 GAC 传播
- 短路成功的 probe 几乎零开销

**关键文件**:
- `src/solver/gpu/GModel.cu`: Stage 2 kernel (`Batch2Stage2Kernel`)
- `include/solver/gpu/batch_probe_manager.h`: `Batch2PersistentControl` 结构体

---

## 第二步：增量任务收集（dirty set） [已完成]

**特点**: SAC3 的前置步骤，实现成本低，收益显著

**核心思想**:
- SAC1 现在每轮全量枚举 `(var, val)`
- 改为只对"域发生变化的变量 + 其邻域变量"重新生成 tasks
- 邻域定义：共享约束的变量集合

**数据结构**:
```cpp
// 在 AutoStageSelector 或 EnforceSAC1 中维护
std::vector<bool> dirty_vars;      // 域发生变化的变量
std::vector<bool> dirty_neighbors; // dirty_vars 的邻域

// 邻域查询（已有）
// GModel::constraint_scopes_cpu: 每个约束的变量对
// GModel::d_subscription / d_subscription_offset: 变量→约束订阅表
```

**实现要点**:
1. 初始化：所有变量标记为 dirty
2. 执行 probe 后：
   - 只有失败的 probe 才触发删值
   - 删值后标记该变量及其邻域为 dirty
3. 下一轮只收集 dirty 变量的 `(var, val)` 对

**预期收益**:
- probe 数量减少 50-80%（取决于问题结构）
- 相当于"半个 SAC3"的效果

---

## 第三步：SAC 预算/早停机制 [已完成]

**目的**: 避免在难例上把时间吃光，利于 MSAC 可控

**参数设计**:
```cpp
struct SACConfig {
  int max_rounds = 100;           // 最大轮次
  double time_budget_ms = 1000;   // 时间预算（毫秒）
  double min_deletion_rate = 0.01; // 删值率低于此阈值则停止
  bool early_stop_enabled = true;
};
```

**早停条件**:
1. `round >= max_rounds`
2. `elapsed_ms >= time_budget_ms`
3. `deletions_this_round / probes_this_round < min_deletion_rate`

**实现位置**:
- `GModelSolver::EnforceSAC1()` 中的 while 循环

---

## 第四步：Stage2 Manager 复用 [已完成]

**问题**: 当前每轮 SAC 都重新创建/销毁 Manager，造成 Host 开销抖动

**优化方案**:
1. 在 `EnforceSAC1()` 中创建一次 Manager，多轮复用
2. 任务队列/结果缓冲只 `Clear()` 不重新分配
3. 避免反复 `cudaMalloc/cudaFree`

**当前实现** (GModelSolver.cu):
```cpp
// 每轮创建，需要改为外层创建一次
Batch2ProbeManager stage1_manager(model_, 64);
Batch2PersistentManager stage2_manager(model_, -1);
```

**优化后**:
```cpp
// EnforceSAC1 开始时创建一次
// 多轮复用，只调用 Clear() 和 AddTask()
```

**注意**: 对 Jetson 统一内存架构尤其重要，减少页面迁移抖动

---

## 第五步：完整 SAC3 [已完成]

**实现文档**: [SAC3_MSAC_IMPLEMENTATION.md](../implementation/SAC3_MSAC_IMPLEMENTATION.md)

**特点**: 使用队列管理需要重检的 `(var, val)` 对

**与 SAC1 的区别**:
- SAC1：每轮检查所有未删除的 `(var, val)`
- SAC3：只检查队列中的 `(var, val)`，删值后将受影响的对入队

**数据结构**:
```cpp
// Qsac 风格的队列（参考 CPU 实现）
class ProbeQueue {
  std::vector<std::pair<int, int>> queue_;  // (var, val) 对
  std::vector<std::vector<bool>> in_queue_; // in_queue_[var][val]

  void Push(int var, int val);
  std::pair<int, int> Pop();
  bool Empty() const;
};
```

**GPU 批量化**:
- 不是单个处理，而是批量从队列取出 N 个 probe
- 执行 batch probe 后，将受影响的对批量入队
- 可以复用 Stage 1/Stage 2 基础设施

---

## 第六步：条件触发 MSAC [已完成]

**实现文档**: [SAC3_MSAC_IMPLEMENTATION.md](../implementation/SAC3_MSAC_IMPLEMENTATION.md)

**原则**: 不要每层都跑完整 MSAC，做"条件触发版"

**触发条件**（满足任一即可）:
1. **浅层**: `level < threshold_level`（如前 5 层）
2. **小域**: 当前变量域大小 < threshold_size（如 < 10）
3. **高失败率节点**: 上一层 GAC 删值多

**配置参数**:
```cpp
struct MSACConfig {
  bool enabled = false;
  int max_level = 5;              // 只在前 N 层执行
  int min_domain_size = 10;       // 域大于此值才执行
  double min_fail_rate = 0.3;     // 上层失败率高于此值才执行
};
```

**集成位置**:
- `GModelSolver::Search()` 中，在 GAC 传播后、递归前

---

## 验收指标

| 阶段 | 验收标准 | 状态 |
|------|----------|------|
| Stage 2 Precheck | TIER0/TIER1 正确性 100%；precheck 统计一致 | ✅ |
| dirty set | probe 数量减少 ≥50%；SAC 时间减少 ≥30% | ✅ |
| 预算/早停 | 难例不超时；简单例不被截断 | ✅ |
| Manager 复用 | 多轮 SAC 无内存抖动；Host 开销 -50% | ✅ |
| 完整 SAC3 | SAC 时间比 SAC1 减少 10-15% | ✅ |
| 条件 MSAC | 搜索节点可控；集成不破坏正确性 | ✅ |

---

## 参考资料

- **实现文档**: [SAC3_MSAC_IMPLEMENTATION.md](../implementation/SAC3_MSAC_IMPLEMENTATION.md) - SAC3 和 MSAC 完整实现细节
- CPU SAC 实现: `include/Solver.h` (SAC1, SAC3, Qsac 类)
- Batch Probe 基础设施: `include/solver/gpu/batch_probe_manager.h`
- SAC1/SAC3 实现: `src/solver/gpu/GModelSolver.cu`
- 设计文档: `docs/planning/SAC_OPTIMIZATION_DESIGN.md`

---

## 关键代码位置

| 功能 | 文件 | 行号 |
|------|------|------|
| SACMode 枚举 | include/GModelSolver.h | 11-16 |
| GpuMSACConfig | include/GModelSolver.h | 20-27 |
| ProbeQueue 类 | src/solver/gpu/GModelSolver.cu | 326-442 |
| EnforceSAC3() | src/solver/gpu/GModelSolver.cu | 538-698 |
| ShouldEnforceMSAC() | src/solver/gpu/GModelSolver.cu | 704-733 |
| EnforceLightweightMSAC() | src/solver/gpu/GModelSolver.cu | 735-807 |
| Search() MSAC 集成 | src/solver/gpu/GModelSolver.cu | 203-232 |
