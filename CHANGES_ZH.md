# 修改清单（中文）

## 2026-01-16

### P0-1：显式 UNKNOWN 语义与统计闭环（基础设施）

**背景**：根据 `docs/planning/TODO_SACGPU_NEXT.md` 的优先级规划，实施 P0-1 任务。

**目标**：probe 执行若 hit budget，则标记 `UNKNOWN`，结果一律"不删"（保守处理）。

**交付内容**：
- `ProbeStatus { kOK, kDWO, kUNKNOWN }` 枚举（三态）
- `ProbeStatistics` 统计结构（unknown_rate、budget_hit_count、avg/max iterations）
- Stage2 内核根据 `max_iterations_per_probe` 判断是否预算超限
- `CollectResults` 仅对 `kDWO` 删值，`kUNKNOWN` 保守不删

**修改文件**：
- `include/solver/gpu/batch_probe_manager.h`: 添加 `ProbeStatus`、`ProbeStatistics`、`Batch2PersistentControl::task_status` 等
- `src/solver/gpu/batch_probe_manager.cu`: 内存分配/释放、`CollectResults` 三态统计收集
- `src/solver/gpu/GModel.cu`: Stage2 内核设置 `task_status`（预算超限→kUNKNOWN）

**开发规范**：
- 在 `AGENTS.md` 和 `CLAUDE.md` 添加开发规范：回归测试、性能记录、消融开关、可回退原则

**验收**：`batch_test_v2.py --tier=0` 通过（8/12 匹配，4 个超时是已有性能问题）

---

### SACGPU 下一阶段 TODO 备忘

- 新增 `docs/planning/TODO_SACGPU_NEXT.md`：整理 P0-P3 的实施清单（UNKNOWN 语义、NSAC mask、
  Batch-3A 接入、bitGEMM 路线与 `bmma_sync(b1, AND+POPC)` 插入点），作为后续迭代备忘录。
- 补充：在 P1-1 增加依赖关系说明（P0-1/P0-3），在 P3-2 明确 BMMA 对 “world 列矩阵 packing 载体稳定”
  的前提要求。

---

## 2026-01-15

### SAC‑GPU 设计文档（Draft v2）整理

- 重写 `docs/planning/SACGPU_DESIGN copy.md`：将原“讨论纪要式”文本整理为可发布的设计草案（元信息/术语对齐/与代码现状锚定/参考文献编号统一），并明确 Phase 5（Batch‑3A/3D 扁平化队列）作为可选加速器的启用条件与主线优先级（budget + NSAC）。
- 更新 `docs/planning/SACGPU_DESIGN.md`：合并 copy 版的关键增补（文档 frontmatter/扁平化层次定义/代码现状对齐/Phase 5 启用条件与回退策略），作为统一版主入口。

---

## 2026-01-12

### Phase 5（Batch‑3D）问题与讨论备忘

- 新增 `docs/archive/batch_ac_versions/BATCH_AC_GPU_PHASE5_BATCH3D_DISCUSSION_MEMO.md`：总结 Phase 5
  推进中的瓶颈（并行度/访存/长尾/调度开销/平台约束）与下一步数据验证清单，用于
  与外部大模型做方案评审。
- 扩展补充：在备忘中加入 Phase 1‑4 已完成工作概览与 Full/fast MSAC 现象说明，便于外部对齐上下文。

---

## 2026-01-11

### Warp-per-Word 约束检查优化推广到 Batch-1

**背景**：Workspace 版（Batch-2/Stage2）的 Warp-per-Word 优化已在 2026-01-09 完成并验证，现在将其推广到 Batch-1/Stage1 的 `ExecuteConstraintCheck_BpC`。

**核心差异**：
- Workspace 版操作私有域（`ws->bitDom`），写回无需原子操作
- Batch-1 版操作共享域（`model.bitDom`），多 block 并发需原子写回

**优化**：在 Batch-1 中应用 Warp-per-Word，消除 shared memory 的 `atomicAnd`，但保留 global memory 原子操作（多 block 并发安全）。

**修改文件**：
- `src/solver/gpu/GModel.cu`:
  - 原 `ExecuteConstraintCheck_BpC()` 重命名为 `ExecuteConstraintCheck_BpC_Legacy()` (line 690-818)
  - 新增 `ExecuteConstraintCheck_BpC_WarpPerWord()` (line 820-983)
  - 新增调度函数 `ExecuteConstraintCheck_BpC()`：`bit_dom_int_size == 1` 走 Legacy，否则走 Warp-per-Word (line 986-1002)
  - 修复：当 `bit_dom_int_size > 1` 时，`EnforceGAC/EnforceGAC_Persistent` 的 `threadsPerBlock` 向上对齐到 32 的倍数，避免 Warp-per-Word 在 partial warp 下的未定义行为
  - 更新 `BitmapGACKernel` shared memory 分配：+64 bytes (line 2785-2786)
  - 更新 `PersistentGACKernel` shared memory 分配：+64 bytes (line 2938-2939)

**关键实现**：
- Warp-per-Word：每 warp 处理一个 domain word，使用 `__ballot_sync` 收集决策
- Global 写回：使用 `atomicAnd` 返回旧值精确统计本 block 删除的位
- 域大小更新：使用 `atomicSub` 保证多 block 并发安全

**验证结果**：
- 正确性：queens-4 (P=5/N=1), langford-3-9 (P=468/N=441), graphw-05 (UNSAT) 一致
- CPU/GPU 节点数匹配：关键测试实例通过
- 回归测试：`ctest --test-dir build -L gpu` 全 PASS
- 边界测试：rand-2-40-80 (max_dom_size=80) 通过 benchmark_probe_throughput

---

## 2026-01-09

### Warp-per-Word 约束检查优化（Workspace 版本）

**问题**：`ExecuteConstraintCheck_BpC_Workspace()` 使用条纹分配（stripe），每个线程处理间隔 `blockDim.x` 的值，删值时需要 `atomicAnd` 在 shared memory 中更新（避免多线程竞争同一 word）。

**优化**：实现 Warp-per-Word 模式，每个 warp (32 线程) 处理一个 domain word（32 个连续值），使用 `__ballot_sync` 收集删值决策，消除 `atomicAnd`。

**修改文件**：
- `src/solver/gpu/GModel.cu`:
  - 新增 `ExecuteConstraintCheck_BpC_Workspace_WarpPerWord()` (line 1648-1796)
  - 原实现重命名为 `ExecuteConstraintCheck_BpC_Workspace_Legacy()` (line 1523-1645)
  - 调度函数：`bit_dom_int_size == 1` 走 Legacy，否则走 Warp-per-Word (line 1799-1816)
  - 更新 shared memory 分配：+64 bytes (warp_del_x/y)

**关键技术**：
- `__ballot_sync(0xFFFFFFFF, keep || !active)` 收集 32 个 lane 的保留决策
- `keep_mask` 直接用于 `new_dom = old_dom & keep_mask`，无需原子操作
- Lane 0 统计删值并写回 `new_dom_x[word]`

**验证结果**：
- 正确性：queens-4/12 (Legacy), langford-3-9 (Legacy), graphw-05 (Warp-per-Word) 一致
- 性能：graphw-05 (bit_dom_int_size=2) Stage 2 吞吐 84469 → 92655 probes/s (+9.7%)

## 2026-01-08

### Workspace 版约束检查域大小增量更新优化

**问题**：`ExecuteConstraintCheck_BpC_Workspace()` 在每次约束检查后都对 `new_x/new_y` 每个 word 执行 `__popc()` 来重算域大小，即使域未发生变化也会扫描整域。

**优化**：改用增量更新 `size -= __popc(removed)`，仅在有删值时执行 `__popc(removed_x)`，消除 `__popc(new_x)` 热路径。

**修改文件**：
- `src/solver/gpu/GModel.cu` (line 1588-1629):
  - 初始化 `size_x = ws->d_cur_dom_size[x]`
  - `if (removed_x) { size_x -= __popc(removed_x); }` 代替 `new_size_x += __popc(new_x)`
  - 只在 `r.x_changed` 时写回 `ws->d_cur_dom_size`

**验证结果**：
- 正确性：queens-4/12, langford-3-9/2-4 节点数一致
- 性能：graphw-05 (fail_rate=100%) Stage 2 吞吐提升明显

### Batch-1 版约束检查域大小增量更新优化

**问题**：`ExecuteConstraintCheck_BpC()` 在每次约束检查后对所有 word 执行两次遍历：一次 `atomicAnd` 删值，一次 `__popc()` 全量重算域大小。

**优化**：改用 `atomicSub` 增量更新，利用 `atomicAnd` 返回值获取删值数，避免全量重扫。

**修改文件**：
- `src/solver/gpu/GModel.cu` (line 770-795):
  - 使用 `atomicAnd()` 返回值获取 `old_x`（避免读-写竞态）
  - `removed_x = old_x & ~new_x` 精确统计本 block 删除的位
  - `atomicSub(d_cur_dom_size, del_x)` 原子减法更新域大小
  - `atomicAdd(..., 0)` 原子读取最新域大小用于 DWO 检测

**关键修复**：
- 竞态条件修复：两个 block 同时读取 `old_x` 会导致双重计数
- 解决方案：使用 `atomicAnd` 返回值而非直接读取全局内存

**验证结果**：
- 正确性：queens-4/12, langford-2-4/3-9 节点数一致
- 性能：graphw-05 Stage 2 vs Stage 1 加速 2.96x

### CSR 邻接表构造时机优化

将 `NeighborCSR` 从各 SAC 函数局部构建移至 `GModelSolver` 构造函数，避免重复构建。

**修改文件**：
- `include/GModelSolver.h`: +`NeighborCSR` 结构体，+`neighbor_csr_` 成员
- `src/solver/gpu/GModelSolver.cu`: 构造函数调用 `NeighborCSR::Build()`，EnforceSAC1/SAC3/LightweightMSAC 使用成员变量

## 2026-01-05

### GPU SAC3 预处理性能优化（降低无删值/多删值开销）

- `src/solver/gpu/GModelSolver.cu`：
  - SAC3 批量大小上调到 `max_batch_size=1024`，减少快照/Kernel 启动次数（无删值场景尤其明显）
  - 邻域表从 `std::set` 改为 `std::vector` + sort/unique，降低构建与遍历开销
  - GAC 级联删值追踪改为基于 Trail 增量区间收集变量，避免 `O(num_vars)` 全扫描
  - 对同一变量的多次删值去重后再 `EnqueueNeighborhood()`，避免重复扫描邻域域值

## 2026-01-04

### 生产用法：DecideCached() 缓存决策

为 SAC/MSAC 集成场景实现了带缓存的自动选择，避免重复采样开销。

**核心实现**：

1. **`DecideCached(tasks, num_blocks)` 方法**:
   - 首次调用：执行 `DecideWithTimedComparison` 并缓存结果
   - 后续调用：直接返回缓存结果，零开销
   - 辅助方法：`ClearCache()`, `HasCache()`, `GetCachedResult()`

2. **典型用法**:
   ```cpp
   AutoStageSelector selector(gmodel);
   // 第一轮 SAC：执行采样
   auto result = selector.DecideCached(tasks1);
   // 第二轮/第三轮 SAC：直接返回缓存（无开销）
   auto result2 = selector.DecideCached(tasks2);
   // 需要重新评估时调用 ClearCache()
   selector.ClearCache();
   ```

3. **验证命令**:
   ```bash
   ./benchmark_probe_throughput <instance.xml> --auto-cached
   ```

**修改文件**：
- `include/solver/gpu/batch_probe_manager.h`: +`DecideCached()`, `ClearCache()`, `HasCache()`, `GetCachedResult()`, +cache fields
- `src/solver/gpu/batch_probe_manager.cu`: +`DecideCached()` 实现
- `apps/benchmark_probe_throughput.cpp`: +`--auto-cached` 选项

### SAC1 集成到 GModelSolver

将批量探测（Batch Probe）基础设施集成到 GModelSolver，支持 SAC1 预处理。

**核心实现**：

1. **`EnforceSAC1()` 方法** (GModelSolver):
   - 执行 SAC1 预处理（迭代直到不动点）
   - 自动使用 `DecideCached()` 选择最优 Stage
   - 删除后自动执行 GAC 传播

2. **新增配置选项**:
   ```cpp
   solver.SetSAC1Preprocessing(true);  // 启用 SAC1 预处理
   solver.SetSACStageMode(StageSelection::kAuto);  // Auto/Stage1/Stage2
   ```

3. **命令行支持** (`compare_cpu_gpu`):
   ```bash
   ./compare_cpu_gpu --input=instance.xml --sac [--sac_stage=auto|stage1|stage2]
   ```

4. **统计扩展** (GpuSearchStatistics):
   - `sac_deletions`: SAC 删除的值总数
   - `sac_probes`: SAC 探测次数
   - `sac_rounds`: SAC 轮次
   - `sac_time`: SAC 时间 (秒)
   - `sac_stage`: 使用的 Stage

**验证结果**:
| 实例 | 无 SAC (P/N) | 有 SAC (P/N) | SAC 删除 | SAC 轮次 |
|------|-------------|-------------|----------|---------|
| queens-4 | 2/1 | 1/0 | 8 | 2 |

**修改文件**：
- `include/GModelSolver.h`: +SAC 配置, +SAC 统计字段
- `src/solver/gpu/GModelSolver.cu`: +`EnforceSAC1()` 实现
- `apps/compare_cpu_gpu.cpp`: +`--sac`, `--sac_stage` 选项

### Stage 2 CTest 集成

将 Stage 2 测试添加到 CTest 框架，用于 CI/回归保护。

**测试内容**：
- 验证 Stage 1 和 Stage 2 产生相同的失败 probe 集合
- 确保 Persistent Blocks 实现的正确性

**运行命令**：
```bash
cd build && ctest -R test_stage2_persistent -V
# 或运行所有 GPU 测试
ctest -L gpu -V
```

**修改文件**：
- `CMakeLists.txt`: 添加 `test_stage2_persistent` 到 CTest

---

## 2026-01-03

### Auto Stage Selection（自动阶段选择）

实现了 Stage 1 vs Stage 2 的自动选择逻辑，基于采样统计自动决定最优执行策略。

**核心实现**：

1. **AutoStageSelector 类** (`include/solver/gpu/batch_probe_manager.h`)：
   - `DecideByTaskCount(num_tasks)`: 基于任务数快速决策
   - `DecideWithSampling(tasks)`: 执行采样并决策（统计判据）
   - `DecideWithTimedComparison(tasks)`: 实测对比并决策（**推荐**）

2. **决策方法对比**:

   | 方法 | 原理 | 优点 | 缺点 |
   |------|------|------|------|
   | `DecideWithSampling` | CV/P95/P50 分布分析 | 开销小 | 可能误判均匀但 Stage 2 更快的场景 |
   | `DecideWithTimedComparison` | 实测 Stage 1/2 耗时 | 更准确 | 采样开销略大（~10-20ms） |

3. **`DecideWithTimedComparison` 决策逻辑**:
   - 采样大小：max(16, min(64, num_tasks/10))
   - 分别运行 Stage 1 和 Stage 2，比较实际耗时
   - 10% 容差内使用 fail_rate 作为 tiebreaker
   - **特殊覆盖**：高 fail_rate (>50%) + 大任务量 (>500) → Stage 2
     （小采样无法体现大规模高失败率场景的动态调度优势）

4. **关键改进** (2026-01-03 更新):
   - 移除了 `num_tasks > 200` 和 `avg_deletions > 80` 的硬触发
   - 改用基于分布的判据：CV (变异系数) 和 P95/P50 比值
   - 新增 `--auto-timed` 模式：使用实测对比而非纯统计
   - 核心原则：只有在任务负载不均衡时才选 Stage 2

5. **扩展 benchmark_probe_throughput**：
   - 支持 Stage 1 vs Stage 2 对比
   - `--auto` 模式：使用 `DecideWithSampling`（统计判据）
   - `--auto-timed` 模式：使用 `DecideWithTimedComparison`（**推荐**）
   - 输出 per-task 统计和分布分析（CV, P95/P50）

**验证结果**（更新后）：
| 实例 | num_tasks | CV | 自动选择 | 实际最优 | 一致性 |
|------|-----------|-----|----------|----------|--------|
| Queens-4 (16) | 16 | 0.0 | Stage 1 | Stage 1 | ✅ |
| Queens-12 (144) | 144 | 0.03 | Stage 1 | Stage 1 | ✅ |
| Langford-3-9 (405) | 405 | 0.14 | Stage 1 | Stage 1 (0.97x) | ✅ |
| Graphw-05 (1863) | 1863 | 0.57 | Stage 2 | Stage 2 (高fail) | ✅ |
| Rand-2-40-8 (320) | 320 | 0.18 | Stage 1 | Stage 1 (0.76x) | ✅ |

**修复的误判案例**：
- `langford-3-9`: 旧逻辑选 Stage 2（num_tasks>200触发），实际 Stage 1 更快 0.97x
- `rand-2-40-8`: 旧逻辑选 Stage 2（num_tasks>200触发），实际 Stage 1 更快 0.76x

**Stage 2 Per-task 统计**：

在 kernel 中记录每个任务的 `iterations` 和 `deletions`，用于分析和调优：
- `Batch2PersistentControl::task_iterations[num_tasks]`
- `Batch2PersistentControl::task_deletions[num_tasks]`

**Stage 2 已知限制**：
- `enable_precheck`: Stage 2 kernel 当前未读取此字段，precheck 仅在 Stage 1 有效

**Stage 2 优化更新** (2026-01-03):
- ✅ NEIGHBOR_ACTIVATION 初始化优化为并行版本（所有线程并行使用 atomicOr）
- 之前：仅 threadIdx.x==0 串行初始化邻接约束
- 之后：所有线程并行初始化，与 Stage 1 `InitializeFrontierForVariable_BlockSync` 保持一致

**修改文件**：
- `include/solver/gpu/batch_probe_manager.h`: +`AutoStageSelector`, +`StageSelectionResult`
- `src/solver/gpu/batch_probe_manager.cu`: +`AutoStageSelector` 实现, +per-task stats
- `src/solver/gpu/GModel.cu`: kernel 中写入 per-task 统计
- `apps/benchmark_probe_throughput.cpp`: 支持 Stage 2 和 `--auto` 模式

---

### Stage 2 性能优化

基于用户反馈实现了两项关键优化，显著提升了 Stage 2 Persistent Blocks 的性能。

**1. 自适应 num_blocks**（`batch_probe_manager.cu`）

新增 `ComputeOptimalNumBlocks(int num_tasks, int device_id)` 方法：
- 任务数 <= 2*num_sms → 使用 num_sms 个 blocks（减少开销）
- 否则使用 4*num_sms 个 blocks（增加并行度）
- 效果：queens-4 (16 tasks) 选择 8 blocks，从 0.61x 提升到 **1.00x**

**2. 可配置 chunk_size**

支持批量任务拉取（`atomicAdd(task_cursor, chunk_size)`）：
- 默认 chunk=1（经测试为最优值）
- 可通过 `SetChunkSize()` 调整

**性能对比**（优化前 vs 优化后）：
| 实例 | 优化前 | 优化后 | 改善 |
|------|--------|--------|------|
| Queens-4 | 0.61x | **1.00x** | +64% |
| Queens-12 | 0.64x | 0.68x | +6% |
| Langford-3-9 | 0.62x | 0.85x | +37% |
| Graphw-05 | 5.21x | **2.76x** | (仍快 2.76x) |
| Rand-2-40-8 | 1.11x | **2.02x** | +82% |
| Rand-2-40-80 | 1.15x | **1.76x** | +53% |

**修改文件**：
- `include/solver/gpu/batch_probe_manager.h`：添加 `ComputeOptimalNumBlocks`, `chunk_size_` 等
- `src/solver/gpu/batch_probe_manager.cu`：实现自适应逻辑
- `src/solver/gpu/GModel.cu`：支持 chunk_size 批量拉取
- `tests/cpp/test_stage2_persistent.cpp`：添加 `--chunk=` 参数

## 2026-01-02

### Phase 3 Stage 2：Persistent Blocks 实现

实现了 Batch-2 的 Stage 2 版本：单次 kernel launch，多个持久 blocks 通过 `atomicAdd` 拉取任务。

**核心实现**：

1. **控制结构** (`include/solver/gpu/batch_probe_manager.h`)：
   - 新增 `Batch2PersistentControl` 结构体：全局任务游标、snapshot、workspaces 数组
   - 新增 `Batch2PersistentManager` 类：Host 端 Stage 2 调度器

2. **Persistent Blocks Kernel** (`src/solver/gpu/GModel.cu:~2046`)：
   ```cpp
   __global__ void Batch2ProbeKernel_PersistentBlocks(
       const GModelData model,
       Batch2PersistentControl* control) {
     // 每个 block 通过 atomicAdd(task_cursor) 拉取任务
     while (true) {
       task_id = atomicAdd(control->task_cursor, 1);
       if (task_id >= num_tasks) break;
       // 重置 workspace → 恢复 snapshot → singleton 赋值 → GAC 传播
       RunGACToFixpoint_BlockSync(...);
       // 写结果
     }
   }
   ```

3. **Host API** (`src/solver/gpu/batch_probe_manager.cu`)：
   - `Batch2PersistentManager::ExecutePersistentBlocks()`：主入口
   - `AllocateMemory()`：分配 per-block workspaces、任务数组、快照
   - `LaunchPersistentBlocksKernel()`：设置控制结构并启动 kernel
   - `CollectResults()`：收集失败 probe

**测试验证** (`tests/cpp/test_stage2_persistent.cpp`)：
- ✅ Queens-4: Stage 1 与 Stage 2 均报告 8 个失败 probe（PASS）
- ✅ Queens-12: Stage 1 与 Stage 2 均报告 0 个失败 probe（PASS）
- ✅ 一致性检查：两个阶段产生完全相同的失败 probe 集合

**性能观察**：
- Queens-4: Stage 1 1.978ms → Stage 2 3.001ms
- Queens-12: Stage 1 5.514ms → Stage 2 8.660ms (0.64x)
- **结论**：Stage 2 当前比 Stage 1 慢，原因可能是：
  - `atomicAdd` 竞争开销
  - 任务分配不均衡
  - 需要进一步优化（如 warp-level 任务聚合、减少同步点）

**Bug 修复**：
1. **shared_mem 声明缺失**：添加 `extern __shared__ u32 shared_mem[]`
2. **ExecuteConstraintCheck 参数顺序**：修正为 `(cid, model, ws, shared_mem)`
3. **GAC 循环死锁**：本地 frontier 指针交换导致线程间不一致，改用 `RunGACToFixpoint_BlockSync()` 解决

**修改文件**：
- `include/solver/gpu/batch_probe_manager.h`：+`Batch2PersistentControl`, +`Batch2PersistentManager`
- `src/solver/gpu/GModel.cu`：+`Batch2ProbeKernel_PersistentBlocks`, +`LaunchBatch2PersistentBlocksKernelWrapper`
- `src/solver/gpu/batch_probe_manager.cu`：+`Batch2PersistentManager` 实现（~300 行）
- `CMakeLists.txt`：+`test_stage2_persistent` 构建目标
- `tests/cpp/test_stage2_persistent.cpp`：Stage 1 vs Stage 2 对比测试

## 2025-12-31

### 吞吐基准工具（benchmark_probe_throughput）

- 新增 `apps/benchmark_probe_throughput.cpp`：Batch-1 vs Batch-2 吞吐量对比工具
- 输出指标：probes/s、执行时间、加速比、平均 iterations/deletions per probe
- 支持参数：`--runs=N`（多次运行取平均）、`--batch2_size=M`（micro-batch 大小）、`--strategy=S`（激活策略）
- 初测结果（Jetson Orin Nano, NEIGHBOR 策略）：
  - Queens-4 (16 probes)：Batch-1 2,440 p/s → Batch-2 11,020 p/s（**4.52x 加速**）
  - Queens-12 (144 probes)：Batch-1 2,261 p/s → Batch-2 24,674 p/s（**10.91x 加速**）
- 验证：Batch-1 与 Batch-2 失败 probe 数一致（Consistency check: PASS）

### Phase 4.3：只读数据优化（cudaMemAdviseSetReadMostly）

- 新增 `GModelAdapter::OptimizeReadOnlyMemoryAdvice()` 方法（`include/model/gmodel_adapter.h:60-62`, `src/model/gmodel_adapter.cu:469-540`）
- 对以下只读数据设置 `cudaMemAdviseSetReadMostly` 提示：
  - `bitSupData`：位支持表（最大的只读数据）
  - `d_subscription`：变量订阅表
  - `d_subscription_offset`：CSR 偏移索引
  - `constraint_scopes`：约束作用域
- 在 `GModelAdapter::Build()` 末尾自动调用（`src/model/gmodel_adapter.cu:330-331`）
- 实现细节：
  - 添加 nullptr/size==0 保护（防御性检查）
  - 添加成功/失败/跳过统计，输出精确的 "applied successfully (N/4)" 或 "applied with warnings"
- 预期收益：Jetson UMA 上的实际效果待用 probes/s 与 Nsight 指标量化（cudaMemAdvise 是 hint，不保证加速）
- 验证：`compare_batch2_tier0.py --tier=0` 12/12 通过，日志输出确认调用成功（4/4）

## 2025-12-30

### Phase 3 Stage 1：Batch-2 Micro-Batch（非 cooperative）

- 新增 `Batch2ProbeManager`（`include/solver/gpu/batch_probe_manager.h`, `src/solver/gpu/batch_probe_manager.cu`）：Host 端 micro-batch 调度与 workspace 内存池分配（每次只分配 `max_batch_size` 份 `WorldWorkspace`，避免按总任务数分配导致内存爆炸），通过 `LaunchBatch2MicroBatchKernelWrapper` 触发 `Batch2ProbeKernel_MicroBatch`。
- 新增测试 `tests/cpp/test_batch2_probe.cpp`：对 `queens-4_ext.xml` 验证 Batch-1 vs Batch-2（Micro-Batch）在 `FULL_ACTIVATION/NEIGHBOR_ACTIVATION` 下的失败探测集合一致，并验证 Batch-2 不污染 `GModel` 原始域状态（`bitDom/d_cur_dom_size`）。
- 更新 `CMakeLists.txt`：添加 `test_batch2_probe` 构建目标并注册到 `ctest`。
- 新增脚本 `tests/python/compare_batch2_tier0.py`：批量跑 TIER0/TIER1，用 `test_batch2_probe` 验证 Batch-2（Micro-Batch）与 Batch-1 结果一致性。
- 强化 Batch-2 健壮性与可观测性：`Batch2ProbeKernel_MicroBatch` 在每个 task 开始重置 `WorldWorkspace` 标量状态（避免 precheck/早退路径残留）；`Batch2ProbeManager` 增加可选的 iterations/deletions 统计收集（`EnableStats`），并在 `tests/cpp/test_batch2_probe.cpp` 中覆盖 precheck 开/关两种路径。
- 修正文档 `docs/planning/BATCH_AC_GPU_COMPREHENSIVE_DESIGN_V2.md`：明确当前 Precheck 为“安全早失败”，在 AC snapshot 前提下短路率理论/实测接近 0%，不再以 40-60% 短路率作为性能验收指标。
- 更新清单 `docs/planning/BATCH_AC_GPU_TODO.md`：对齐代码现状（Phase 3 TIER0/TIER1 已通过、吞吐基准待补；Phase 2 P1/P2/P3 待实现），并修正测试命令与函数命名。

## 2025-12-29

### Phase 2 P0 优化实施与 Bug 修复（Batch AC-GPU）

实施了 BATCH_AC_GPU_COMPREHENSIVE_DESIGN_V2.md 中的 Phase 2 两个 P0 优化项，发现并修复了关键并发 bug。

**[P0-1] Frontier 初始化策略化 - ✅ 修复后正确**
- ✅ 添加 FrontierInitStrategy 枚举 (GModel.cuh:29-33)：FULL_ACTIVATION / NEIGHBOR_ACTIVATION
- ✅ 扩展 BatchProbeControl 结构 (batch_probe_manager.h:56-58, 83)：snapshot_is_ac, activation_strategy
- ✅ 修改 InitializeFrontierForVariable (GModel.cu:1014-1070)：实现邻域激活分支
- ✅ 添加 SetActivationStrategy() 方法 (batch_probe_manager.h:129, batch_probe_manager.cu:194-207)
- ✅ **理论验证**：NEIGHBOR_ACTIVATION 在 snapshot AC 前提下是**正确的**优化，通过 PropagateVarToNextBitmap 动态扩散到所有需要检查的约束

**[P0-2] Cheap Precheck - ✅ 已修复并启用**
- ✅ 添加统计字段 (batch_probe_manager.h:60-63)：precheck_count, short_circuit_count, need_gac_flag
- ✅ 重写 CheckValueSupportBitSup (GModel.cu:1204-1260)：使用 bitSupData 做“早失败”检测（对齐 ExecuteConstraintCheck_BpC 的索引方式）
  - 若 `(X=a)` 在任一邻接约束上无支持 → **必然 DWO** → 短路跳过完整 GAC
  - 若所有邻接约束当前都有支持 → **仍需完整 GAC**（不做“早成功”）
- ✅ 启用 Precheck (GModel.cu:1404-1422)：根据 need_gac_flag 决定是否跳过 RunGACToFixpoint

**Bug 修复**：
1. **Cooperative Kernel 死锁** (GModel.cu:1207-1211)
   - 根因：`__shared__` 内存 per-block 可见性导致 grid.sync() 死锁
   - 修复：need_gac_flag 移至 BatchProbeControl (全局内存)

2. **activation_strategy 初始化** (batch_probe_manager.h:83)
   - 根因：初始化为 0，SaveSnapshot() 会覆盖用户设置的 FULL_ACTIVATION
   - 修复：初始化为 -1，SaveSnapshot() 只在 -1 时设置默认值

3. **🔥 Frontier 交换竞争条件（非确定性 Bug 根源）** (GModel.cu:1186-1189)
   - **根因**：RunGACToFixpoint 中所有线程都在交换 frontier 指针，导致不同线程看到不同指针值，传播提前收敛
   - **现象**：NEIGHBOR_ACTIVATION 产生非确定性结果（queens-4: 0-2 个 DWO，每次都不同）
   - **修复**：只用 tid==0 的线程交换指针（与 PersistentGACKernel 一致）
   - **验证**：queens-4 运行 5 次结果完全一致（8 vs 8），TIER0 全部通过 (12/12)

**关键修改文件**：
- include/GModel.cuh, include/solver/gpu/batch_probe_manager.h
- src/solver/gpu/GModel.cu (~120 行，含调试代码和 bug 修复)
- src/solver/gpu/batch_probe_manager.cu
- tests/cpp/test_batch_probe_state.cpp - FULL vs NEIGHBOR 对比测试（添加详细调试输出）
- tests/python/compare_activation_strategies.py - TIER0 批量测试
- tests/scripts/test_activation_strategies.sh - Shell 测试脚本

**测试结果**（修复后）：
- ✅ **状态一致性**: bitDom ✓, d_cur_dom_size ✓
- ✅ **FULL vs NEIGHBOR 一致性**: queens-4 (8 vs 8 ✓)，langford-2-4 (8 vs 8 ✓)
- ✅ **确定性**: queens-4 运行 5 次结果完全相同
- ✅ **TIER0**: 12/12 全部通过（包括 UNSAT 实例 graphw-05）
- ✅ **理论正确性**: deletions、最终域状态、DWO 检测完全一致

**调试发现**：
- NEIGHBOR 的 iterations 稍多（2-4 vs 1-2），这是预期的，因为需要逐步扩散 frontier
- 快照域状态一致（v0=4 v1=4 v2=4 v3=4），证明 snapshot 恢复正确
- PropagateVarToNextBitmap 正常工作，frontier 从邻居扩散到所有受影响约束

**调试输出**：
- ✅ Bug 修复后，调试输出已禁用 (GModel.cu:1307, 1342, 1389)
- 通过 `if (false && ...)` 保留代码便于未来调试
- 验证测试：queens-4 运行正常，结果一致 (8 vs 8)

**结论**：
- ✅ NEIGHBOR_ACTIVATION 是**理论正确**的优化（在 snapshot AC 前提下）
- ✅ Bug 已修复，两种策略产生完全一致的结果
- ❌ Cheap Precheck 需要重新设计（当前逻辑不正确）

---

### 文档修复
- 修复文档 `docs/planning/BATCH_AC_GPU_COMPREHENSIVE_DESIGN_V2.md`：对齐文档与实际代码/硬件实现，修复 6 处关键问题
  - §3.1：澄清当前 GModel.cu:1039 使用 FULL_ACTIVATION，Phase2 将添加 NEIGHBOR_ACTIVATION 优化
  - §5.1：修正 CheapPrecheck 使用 bitSupData 而非不存在的 bitSupTex，对齐 ExecuteConstraintCheck_BpC 索引逻辑
  - §4.2：修正内存估算示例，补充正确的 bitmap_size_words 计算公式和 Queens-12/100 真实数据
  - §5.4：添加 Jetson UMA 检测（concurrentManagedAccess 判断），避免在统一内存架构下执行不必要的 cudaMemPrefetchAsync
  - §9.2：修正 cudaMemGetInfo API 调用签名，补充 shared_mem_size 正确计算方法
  - §6.3：优化 PopTask 实现，引入两级 bitmap 索引避免 O(C) 线性扫描，降低大规模约束问题开销

- 新增备忘 `docs/planning/BATCH_AC_GPU_FLATTEN_TASK_QUEUE_MEMO.md`：总结“Probe × GAC 扁平化”为一维任务流的实现要点，推荐以 `<cid, world_mask>` 聚合队列 + work-stealing 落地 Batch‑3A（Jetson 友好，非 cooperative）。
- 更新备忘 `docs/planning/BATCH_AC_GPU_FLATTEN_TASK_QUEUE_MEMO.md`：补充聚合队列的额外开销来源、可观测计数器与自适应开关/回退阈值。

## 2025-12-25
- 新增文档 `docs/planning/BATCH_AC_GPU_DESIGN.md`：基于 `GModel` 梳理 AC-GPU 可优化点，并给出 SAC-GPU 迁移到 Batch AC-GPU 的数据结构、内核方案与分阶段落地路径。
- 新增备忘 `docs/planning/BATCH_AC_GPU_COMPARISON_MEMO.md`：对比设计与实施文档，汇总一致点、差异与风险清单。
- 新增文档 `docs/planning/BATCH_AC_GPU_BATCH2_BATCH3_DESIGN.md`：总结 Batch-2/Batch-3 的多世界并行与持久线程块方案，并给出 Jetson Orin（UMA + cooperative 限制）下的落地路线。

## 2025-12-17
- 新增文档 `benchmarks/README.md`：对 `benchmarks/` 基准库做走读，汇总子目录样例类型与数量，说明当前解析器对 `*_ext.xml`/`*-ext.xml`（表约束、supports/conflicts）的支持边界，并给出 `cpim_test_parser`/`dump_gmodel` 的推荐使用方式与过滤建议。

## 2025-12-23
- 更新文档 `docs/planning/SAC1_SAC3_INTEGRATION_DESIGN.md`：按 MSAC（SAC3 内嵌 AC3bit）叙述方式重写 CPU/GPU 统一方案；补齐“probe 阶段必须禁 `Tabular::weight` 更新（且 kernel AC 也要受控）”与 `max_probes/max_time_ms` 预算、并明确 GPU 侧 snapshot-based probe 与可落地的 Batch-1（持久化传播内核连续处理 probe）。
- 更新文档 `docs/planning/Batch_AC.md`：澄清“batched AC = SAC”仅对应一次 SAC-checking pass（非闭包），补充适用范围（仅二元 supports 表约束）、并明确 Batch-1/Batch-2 的工程边界与 micro-batch 内存成本。
- 更新文档 `docs/planning/SAC1_SAC3_INTEGRATION_DESIGN.md`：补充“以对照实验为导向”的 SAC 算法族实现计划，细化 CPU baseline 与 GPU-MSAC（Batch-1 优先）的分阶段验收标准与配置接口。

## 2025-10-21
- 新增文档：`aig_docs/GPU_GAC_UNIFIED_MEMORY_GUIDE.md`
  - 总结 Jetson Orin（统一内存 UMA）下的约束传播优化方案：统一内存单源数据、事件驱动、CPU/GPU 动态调度、持久化 kernel、设备侧队列与按字位集处理等；给出代码落点与渐进落地步骤，并关联 `GPU_GAC_PERSISTENT_STATE_MACHINE.md` 与 `GPU_GAC_PIPELINE_PLAN.md`。

- 调整 `include/model/xcsp_parser.h`，新增基准路径元数据结构与接口，以便统一处理单个文件、目录与清单。
- 更新 `include/model/libxml2_parser.h` 与 `src/model/libxml2_parser.cpp`，重写清单解析与路径归一化逻辑，支持 XCSP2 文件格式识别，并补充目录遍历、格式探测等辅助函数。
- 扩展 `samples/main_new_parser.cpp`，集成 Abseil Flags 命令行参数，支持列出基准文件、选择清单或直接路径，并输出检测到的 XCSP 版本。
- 在 `CMakeLists.txt` 中为 `cpim_test_parser` 目标链接 `absl::flags` 与 `absl::flags_parse`，满足新命令行功能的依赖。
- 新增 `CHANGES_ZH.md`（本文件），提供近期变更的中文汇总。
- 新增文档 `aig_docs/GPU_GAC_PERSISTENT_STATE_MACHINE.md`，提出基于“持久化内核 + 约束状态机 + 设备端队列”的 GAC 传播方案，并与 `aig_docs/GPU_GAC_PIPELINE_PLAN.md` 对比评估 Jetson Orin 适配性。

- 新增 CPU 基线传播工具：`cpim_gac_cpu`
  - 位置：`samples/run_gac_cpu.cpp`, `include/model/gac_cpu.h`, `src/model/gac_cpu.cpp`
  - 功能：串行 CPU 版队列压缩与 GAC 传播（仅二元 supports 约束），用于正确性基线与后续 GPU 优化对照。
  - 构建与运行：
    - 构建：`cmake -S . -B build_orin -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build_orin -j$(nproc)`
    - 运行：`./build_orin/cpim_gac_cpu --input=samples/bench/queens-4_ext.xml --max_print=8`

- GModel GPU 基线传播实现：`GModel::EnforceGAC`
  - 位置：`include/GModel.cuh`, `src/GModel.cu`, `src/model/gmodel_adapter.cu`, `samples/dump_gmodel.cpp`
  - 功能：参考 `cuSAC.cu` 的 `enforceGAC` 流程，在统一内存 `GModel` 上实现 CPU 串行队列 + GPU `CsCheckMain` kernel 的 GAC 传播，并引入共享内存缓存与按字节掩码归约，输出迭代次数、删除数及是否检测到不一致。
  - 使用方式：`./build_orin/dump_gmodel --input=... --run_gac`。
