# 修改清单（中文）

## 2026-02-19

### FQ-PT：OW2 O2-A 接口与 `world_stealing` 控制面接线（default-off）

**目标**：仅完成 OW2 前置控制面（接口/flag/Host 下发）落地，不改 `FQPTOwnerFrontierKernel`
现有静态 world 分配行为，为后续 O2-B（kernel 动态领取）提供无歧义接入点。

**核心改动**：
- `include/solver/gpu/batch_probe_manager.h`
  - `FQPTControl` 新增：
    - `enable_world_stealing`
    - `world_cursor`
  - `FQPTBaselineManager` 新增：
    - `SetEnableWorldStealing(bool)`
    - 成员 `enable_world_stealing_`
    - 成员 `d_world_cursor_`
- `src/solver/gpu/batch_probe_manager.cu`
  - 新增 `SetEnableWorldStealing(bool)`（仅更新布尔开关，不触发重分配）
  - `AllocateMemory()` 新增 `d_world_cursor_` 分配与初始化
  - `FreeMemory()` 新增 `d_world_cursor_` 释放
  - `Execute()` 每次执行前清零 `d_world_cursor_`
  - `LaunchKernel()` 下发：
    - `d_control_->enable_world_stealing`
    - `d_control_->world_cursor`
- `apps/sac_benchmark.cpp`
  - 新增 CLI：`--fqpt_enable_world_stealing`
  - `ConfigureFQPTManager()` 接线 `SetEnableWorldStealing(...)`
- `tests/cpp/test_fqpt_baseline.cpp`
  - 新增 CLI：`--fqpt_enable_world_stealing`
  - `RunFQPT()` 接线 `SetEnableWorldStealing(...)`

**语义与回退**：
- O2-A 不改 kernel 调度行为（仍走 OW0 静态分配）；
- `--fqpt_enable_world_stealing=1` 且 `--fqpt_enable_world_owner=0` 时安全忽略；
- 继续保持 default-off、可回退、Stage2/legacy 默认行为不变。

### FQ-PT：OW2 O2-B 动态 world 领取内核分支（default-off）

**目标**：在 OWF kernel 内启用 `world_cursor` 动态领取分支，缓解静态 stride 在 world 工作量不均时的长尾空转。

**核心改动**：
- `src/solver/gpu/GModel.cu`
  - `FQPTOwnerFrontierKernel(...)` 中将 world 外层循环改为统一 `while` 框架：
    - `enable_world_stealing=0`：沿用 OW0 静态 `next_world += world_stride`
    - `enable_world_stealing=1`：`lane0` 通过 `atomicAdd(world_cursor, 1)` 领取 world 并 warp 广播
  - world 内 Phase0/传播/统计逻辑保持不变，避免引入语义漂移。

**语义与回退**：
- 仅当 `enable_world_owner=1 && enable_world_stealing=1 && world_cursor!=nullptr` 才走动态领取；
- 其余场景自动回退 OW0 静态映射；
- 不引入 `world_lock`，保持 Owner-World 单写者约束与 soundness。

### FQ-PT：OW3a O3A-A 统计字段与输出接线（default-off）

**目标**：先完成 OW3a 命中率统计的控制字段与输出通路接线；本阶段不实现 kernel 采样逻辑，
确保后续 O3A-B 仅需补充设备端计数更新。

**核心改动**：
- `include/solver/gpu/batch_probe_manager.h`
  - `FQPTControl` 新增：
    - `enable_cid_microbatch_profile`
    - `microbatch_profile_interval`
    - `microbatch_rounds` / `microbatch_sel_ge2_rounds` / `microbatch_sel_sum`
  - `FQPTStatistics` 新增：
    - `microbatch_rounds`
    - `microbatch_sel_ge2_rounds`
    - `microbatch_sel_sum`
    - `avg_sel_count`
  - `FQPTBaselineManager` 新增 setter：
    - `SetEnableCidMicrobatchProfile(bool)`
    - `SetMicrobatchProfileInterval(int)`
- `src/solver/gpu/batch_probe_manager.cu`
  - 新增三项 device 计数内存分配/释放/清零
  - `LaunchKernel()` 下发 OW3a 控制字段与统计指针
  - `CollectResults()` 汇总 `microbatch_*` 并计算 `avg_sel_count`
- `apps/sac_benchmark.cpp`
  - 新增 CLI：
    - `--fqpt_enable_cid_microbatch_profile`
    - `--fqpt_microbatch_profile_interval`
  - benchmark 结果新增并打印：
    - `mb_r` / `mb_ge2` / `mb_sel` / `mb_avg`
- `tests/cpp/test_fqpt_baseline.cpp`
  - 新增上述 OW3a flag 并接入 manager

**语义与回退**：
- 本阶段不改 kernel 执行路径，统计默认值保持 0；
- 继续保持 default-off，可通过 flag 一键关闭；
- O3A-B 将在此基础上补充实际采样更新逻辑。

## 2026-02-18

### FQ-PT：OW1 实验迭代（mode=2 + 可观测化，允许退化）

**目标**：将 OW1 从“单一优化开关”扩展为可扫频实验框架，允许阶段性性能退化用于摸索，
但保持正确性硬约束与 default-off 回退能力。

**核心改动**：
- `include/solver/gpu/batch_probe_manager.h`
  - `FQPTControl` 新增：
    - `ow1_scatter_mode`（`0=OW0 fallback, 1=OW1, 2=OW1_v2(match_any)`）
    - `ow1_force_scatter`（忽略 `ow1_min_degree` 强制 scatter）
    - `ow1_scatter_calls`、`ow1_fallback_calls`、`ow1_word_leader_writes`
  - `FQPTStatistics` 新增上述三项 host 统计镜像
  - `FQPTBaselineManager` 新增 setter：
    - `SetOW1ScatterMode(int)`
    - `SetOW1ForceScatter(bool)`
- `src/solver/gpu/batch_probe_manager.cu`
  - 分配/释放/清零 OW1 新统计计数器
  - `LaunchKernel()` 下发 `ow1_scatter_mode/ow1_force_scatter`
  - `CollectResults()` 汇总 `ow1_*` 统计
- `apps/sac_benchmark.cpp`
  - 新增 CLI：
    - `--fqpt_ow1_scatter_mode`
    - `--fqpt_ow1_force_scatter`
  - 输出新增统计：
    - `ow1_sc`（scatter 调用次数）
    - `ow1_fb`（fallback 调用次数）
    - `ow1_w`（leader 写回 frontier word 次数）
  - 模式名支持 `FQ-PT(OWF+OW1m2)` 以区分 `mode=2`
- `tests/cpp/test_fqpt_baseline.cpp`
  - 新增 OW1 实验参数接线，覆盖 `mode/force` 切换场景
- `src/solver/gpu/GModel.cu`
  - 新增 `FQPTPushVarNeighborsToFrontierTwoLevelWarpMatchAny(...)`（OW1_v2）
  - 新增统一分流函数 `FQPTPushVarNeighborsToFrontierTwoLevelDispatch(...)`
  - OWF 的 seed 与 `x_changed/y_changed` 回写统一走分流逻辑：
    - `mode=0` 始终 lane0 fallback
    - `mode=1/2` 按 `ow1_force_scatter` 与 `ow1_min_degree` 决定 scatter/fallback
  - 仅使用 warp 级原语；不引入 block barrier 到 OWF 主循环

**语义与回退**：
- OW1 继续 default-off（需显式开启 `--fqpt_enable_ow1_frontier_scatter=1`）
- Stage2 与 legacy FQ-PT 默认行为不变
- soundness 不变：`UNKNOWN` 语义不变，正确性门槛不放松

### FQ-PT：新增 OW1（S3）frontier 邻接写回优化（default-off）

**目标**：降低 OWF 路径在高 degree 变量上的 `lane0` 串行邻接写回瓶颈，
仅优化 frontier 回写，不引入 S1/S2 变更，保持归因清晰。

**核心改动**：
- `include/solver/gpu/batch_probe_manager.h`
  - `FQPTControl` 新增：
    - `enable_ow1_frontier_scatter`
    - `ow1_min_degree`
  - `FQPTBaselineManager` 新增 setter：
    - `SetEnableOW1FrontierScatter(bool)`
    - `SetOW1MinDegree(int)`
- `src/solver/gpu/batch_probe_manager.cu`
  - `LaunchKernel()` 下发 OW1 控制字段；
  - 仅在 `enable_world_owner` 下允许 OW1 生效，其它路径保持关闭。
- `src/solver/gpu/GModel.cu`
  - 保留 `FQPTPushVarNeighborsToFrontierTwoLevel(...)` 作为 fallback；
  - 新增 warp 协作写回逻辑：按 `word` 聚合同轮 lane bit，
    由 leader 一次写回 L0/L1 frontier；
  - 调用分流：`enable_world_owner && enable_ow1_frontier_scatter && degree>=ow1_min_degree`
    走 OW1，否则走 OW0 旧路径。
- `apps/sac_benchmark.cpp` / `tests/cpp/test_fqpt_baseline.cpp`
  - 新增 CLI：
    - `--fqpt_enable_ow1_frontier_scatter`
    - `--fqpt_ow1_min_degree`
  - benchmark 模式名区分 `FQ-PT(OWF+OW1)` 与 `FQ-PT(OWF)`。

**语义与回退**：
- OW1 默认关闭（default-off）；
- 仅在 OWF 路径启用，保持 Stage2 与 legacy FQ-PT 默认行为不变；
- 保持 soundness：`UNKNOWN` 语义与 DWO/OK 判定逻辑不变。

### FQ-PT：新增 OW0（Owner-World + Two-Level Frontier）路径（default-off）

**目标**：在 `--mode=fqpt` 下引入 Owner-World 执行路径，移除全局 `(world,cid)` MPMC 与 `world_lock`
在该路径上的热冲突，把瓶颈从控制面迁回约束检查算子。

**核心改动**：
- `src/solver/gpu/GModel.cu`
  - 新增 `FQPTOwnerFrontierKernel` 与 `LaunchFQPTOwnerFrontierKernelWrapper(...)`；
  - 采用 `owner(world)=world%gridDim.x` 的 warp-per-world 静态映射；
  - 使用 two-level frontier（`frontier_A` 作为 L0，`frontier_B` 作为 L1）做摊销 O(1) pop；
  - 初始化下沉到 device Phase0（snapshot 恢复、singleton assign、seed frontier）；
  - OW0 路径仅调用 warp-only 检查函数（不进入 block-sync 检查路径）。
- `include/solver/gpu/batch_probe_manager.h` / `src/solver/gpu/batch_probe_manager.cu`
  - `FQPTControl` 新增 `enable_world_owner` 与 frontier 统计指针：
    `frontier_pop_count`、`frontier_scan_steps`；
  - `FQPTBaselineManager` 新增 `SetEnableWorldOwner(bool)`；
  - `LaunchKernel()` 新增 owner/legacy 双路径分流；
  - owner 路径下 ring/lock 指针可为空，统计聚焦 checks/deletions/frontier 扫描。
- `apps/sac_benchmark.cpp`
  - 新增 flag：`--fqpt_enable_world_owner`；
  - `RunFQPTBenchmark` 输出新增 `fpop/fscan`（frontier pop 与平均扫描步数）。
- `tests/cpp/test_fqpt_baseline.cpp`
  - 新增 `--fqpt_enable_world_owner` 参数，支持 correctness 对照测试 OW0 路径。

**语义与回退**：
- 新路径默认关闭（default-off），旧 FQPT ring+lock 路径完整保留；
- soundness 不变：`UNKNOWN` 不删值，`unknown=0` 时仍要求与 Stage2 对齐。

### 文档：新增 OW0 在 RTX 4060/4090 迁移前现状裁决稿

- 新增 `docs/planning/OW0_RTX4060_4090_STATUS_2026_02.md`：
  - 固化 OW0 当前实现边界（开关、分流、统计、回退语义）；
  - 汇总固定 5 例本地实测证据（`stage2/fqpt/owf`）与中位结论；
  - 明确 4060/4090 章节为“推断 + 代码证据”，非远端已测结论；
  - 提供可直接交给其他模型的策略任务书（S1 内存路径、S2 调度路径、S3 内核路径）。
- 更新 `docs/README.md`“关键规划文档”导航，新增上述文档入口。

## 2026-02-17

### FQ-PT：分步实施落地（D0/P0/P1/P2）

**范围**：按 `FQ-PT-CID-CTA-MB` 分步计划落地文档修订与代码实现，保持可回退、可观测、soundness 不变（UNKNOWN 不删值）。

**D0（文档修订）**：
- `docs/planning/SACGPU_NEW_CODEX_REVIEW_2026_02.md`
  - 明确 Phase1 可能平收益（`-2%~+2%`）的预期边界；
  - 增加 Phase1 -> Phase2 的 stop/go 条件；
  - 新增 back-of-envelope 估算（`R/B/W` 指标 + 低/中/高聚合三档）；
  - 附录锚点改为“函数名优先 + 行号提示”。

**P0（观测闭环与开关）**：
- `include/solver/gpu/batch_probe_manager.h`
  - `FQPTControl` 新增运行时开关：
    `enable_cid_grouping`、`enable_parallel_group_check`、
    `group_warps_per_cta`、`group_degrade_threshold`
  - `FQPTControl` 新增统计指针：
    `stale_drop_count`、`lock_fail_count`、`lock_retry_count`、
    `bucket_count`、`bucket_task_sum`、`bucket_active_warp_sum`
  - `FQPTStatistics` 新增统计字段：
    `stale_drop_count`、`lock_fail_count`、`lock_retry_count`、
    `avg_bucket_size`、`avg_bucket_utilization`
  - `FQPTBaselineManager` 新增 setter：
    `SetEnableCidGrouping`、`SetEnableParallelGroupCheck`、
    `SetGroupWarpsPerCta`、`SetGroupDegradeThreshold`
- `src/solver/gpu/batch_probe_manager.cu`
  - 新增上述统计计数的分配/释放/清零；
  - Launch 前填充 `FQPTControl` 新开关与统计指针；
  - `CollectResults` 汇总新统计并计算 `avg_bucket_*`。
- `apps/sac_benchmark.cpp`
  - 新增 FQPT 参数：
    `--fqpt_enable_cid_grouping`、`--fqpt_enable_parallel_group_check`、
    `--fqpt_group_warps`、`--fqpt_group_degrade_threshold`
  - benchmark 结果新增 `stale/lock/bucket` 输出与聚合。

**P1（仅分桶 + 退化路径）**：
- `src/solver/gpu/GModel.cu::FQPTBaselineKernel`
  - 加入 CTA-local 线性分桶（thread0 选取最热 `cid`）；
  - `max_bucket_size <= group_degrade_threshold` 时退化为逐任务路径；
  - 分桶统计埋点：`bucket_count/task_sum/active_warp_sum`；
  - 保持原 block 级检查语义与 commit 单点提交。

**P2（并行检查路径）**：
- `src/solver/gpu/GModel.cu`
  - 新增 `ExecuteConstraintCheck_BpC_Workspace_WarpPerWorld`（warp 级检查）；
  - `FQPTBaselineKernel` 新增“分桶后并行检查”路径：
    - 每 warp 处理 1 个 world；
    - `check_shared` 按 warp 切片；
    - 每 warp 产出 `PropagateResult`，thread0 统一 commit；
  - legacy 小域仍保留原路径回退（并行路径默认要求 `bit_dom_int_size > 1`）。
- `src/solver/gpu/GModel.cu::LaunchFQPTBaselineKernelWrapper`
  - shared memory 计算改为按 `check_words_per_warp * check_warp_slots` 动态估算，
    支持并行分组场景。

**语义保证**：
- 去重标记、pending 结算、lock/retry、UNKNOWN 语义保持不变；
- 新路径默认由开关控制，可一键回退到旧路径。

**验证与门槛判定（固定 5 例 + `--num_probes=64 --warmup=1 --iterations=5`）**：
- 构建与正确性：
  - `cmake .. && make -j$(nproc)` 通过；
  - `ctest --test-dir build -R test_fqpt_baseline --output-on-failure` 通过；
  - 5 例 `test_fqpt_baseline --input=<case> --num_probes=64` 全部通过。
- 观测闭环：
  - `sac_benchmark --mode=fqpt` 新字段可稳定输出并可解析（包含 0 值）：
    `unknown/checks/stale/lock_fail/lock_retry/bsz/butil`。
- P1 gate（以每样例 3 次重复的中位数判定）：
  - `median(P1 vs P0)=+0.43%`（满足“median 回退 <=3%”）；
  - 但 `haystacks-11` 出现 `+8.79%` 回退（不满足“单例 <=5%”）；
  - 聚合信号达标（`bsz>=1.4` 与 `butil>=0.35` 均为 `5/5`）。
  - 结论：**P1 作为 default-off 消融路径保留，不满足直接进入默认开启条件**。
- P2 gate（相对 P1）：
  - `median(P2 vs P1)=-3.79%`（有提升但未达到 `>=8%`）；
  - 结论：**并行路径默认保持关闭，仅保留开关用于后续调优/消融**。

### 文档：FQ-PT vs Batch2 归因报告（可转发版）

- 新增/重写 `gemini_doc/SACGPU_MB_implementation_review.md`：
  - 固化 P0/P1/P2 全面慢于 Batch2 的实测证据（固定 5 例口径）；
  - 汇总关键根因（任务粒度、队列/锁、分桶 O(n²)、`bit_dom_int_size==1` 门控）；
  - 给出可执行优化路线（A 止损、B 调度面重构、C 小域并行增强）；
  - 附关键代码片段，供外部模型快速复核。

## 2026-02-16

### Docs：新增 SACGPU 新方案独立裁决文档（Codex 版）

**动机**：`docs/planning/SACGPU_new.md` 与 `gemini_doc/SACGPU_new_review.md` 的观点存在交叠与表述强弱不一，
需要一份“可执行裁决稿”统一结论、落地顺序与风险边界，减少后续实现分歧。

**交付内容**：
- 新增 `docs/planning/SACGPU_NEW_CODEX_REVIEW_2026_02.md`：
  - 明确评审口径为“静态代码证据、无运行数据”；
  - 对 FQ-PT-CID-CTA-MB 方案给出逐项裁决（成立/部分成立/不成立）；
  - 固化分阶段路线（Phase 0~3）、启停条件、回退条件与验收门槛；
  - 补充“本次仅文档改动，后续接口扩展建议未实施”的边界说明；
  - 附关键代码锚点（`GModel.cu`、`batch_probe_manager.h/.cu`、`test_fqpt_baseline.cpp`）。
- 更新 `docs/README.md`：
  - 在“关键规划文档”中注册新文档入口，说明其用途为
    “FQ-PT-CID-CTA-MB 独立裁决与落地门槛”。

**本次不做**：
- 不修改任何 C++/CUDA 公共接口；
- 不跑 benchmark、不新增 smoke test 输出；
- 不回写清理 `docs/planning/SACGPU_new.md` 的草稿内容。

## 2026-02-11

### FQ-PT Baseline：摊平队列持久线程基线（Task = `(world_id, cid)`）

**动机**：Batch-3A 在低聚合度场景存在较高固定开销。为了建立可对照的“无聚合”动态队列基线，
新增 FQ-PT 路径：复用 ACgpu 约束检查核心，只替换传播基础设施为 GPU 端 persistent blocks + work queue。

**交付内容**：
- `include/solver/gpu/batch_probe_manager.h`：
  - 新增 FQ-PT 数据结构与接口：`FQPTTask`、`FQPTRingSlot(seq+task)`、`FQPTControl`、
    `FQPTStatistics`、`FQPTBaselineManager`、`LaunchFQPTBaselineKernelWrapper(...)`。
- `src/solver/gpu/GModel.cu`：
  - 新增 MPMC ring 原语（每槽 `seq`，避免 MPMC holes）；
  - 明确发布顺序：producer 写 payload 后 `__threadfence()` 再发布 `seq`；
  - 新增 `FQPTBaselineKernel`：persistent blocks 循环消费 `(world_id,cid)`，复用
    `ExecuteConstraintCheck_BpC_Workspace` 做检查/删值，删值后按 subscription 推后继任务；
  - 新增 CTA-local 缓冲（批量 pop + 本地生成缓冲 + 批量 flush）；
  - 新增 world 级互斥锁 `world_locks`，拿锁失败先本地重试；
  - 新增安全退出：`pending==0 && global queue empty && local empty`。
- `src/solver/gpu/batch_probe_manager.cu`：
  - 新增 `FQPTBaselineManager` 实现（快照保存、world 初始化、seed tasks、kernel 启动、结果回收）；
  - 队列溢出可观测：`overflow_count`，并按 world 标记 `UNKNOWN`（不删值，保持 soundness）。
- `apps/sac_benchmark.cpp`：
  - 新增 `--mode=fqpt`；
  - 新增 FQ-PT 参数：`--fqpt_num_blocks/--fqpt_queue_capacity/--fqpt_pop_batch/--fqpt_local_buffer/--fqpt_lock_retry/--fqpt_lock_backoff`；
  - 输出 `unknown/overflow/checks` 等统计字段。
- 测试与构建：
  - 新增 `tests/cpp/test_fqpt_baseline.cpp`（Stage2 对照：无 UNKNOWN 时结果全等；有 UNKNOWN 时仅要求 FQPT 的 DWO 为 Stage2 子集）；
  - `CMakeLists.txt` 新增 `test_fqpt_baseline` 目标与 `ctest` 注册。

### FQ-PT：修复高并发下偶发 timeout（pending 计数竞态）

**问题现象**：`sac_benchmark --mode=fqpt` 在中等实例/较高 block 数下偶发卡住；`test_fqpt_baseline`
也可能触发超时。

**根因**：
- 生成任务时先发布到队列、后执行 `pending++`，在高并发下可能出现“消费者先完成并 `pending--`”，造成
  `pending` 下溢，退出条件永远不满足；
- 拿锁失败路径中，`status!=OK` 的任务曾存在 pending 递减时序不一致，导致计数不稳。

**修复**（`src/solver/gpu/GModel.cu`）：
- `FQPTFlushGeneratedBuffer` 调整为“先 `pending += count`，再发布任务；发布失败则回滚 pending”；
- 统一 `status!=OK` 任务在拿锁失败路径中的完成逻辑，确保每个 task 对 pending 只结算一次。

**验证**：
- `ctest --test-dir build -R test_fqpt_baseline --output-on-failure`：通过；
- `sac_benchmark --mode=fqpt` 在 `queens-12` 的 `--fqpt_num_blocks=16/32` 复现用例不再 timeout。

### FQ-PT：`(world,cid)` 去重入队（降低重复检查风暴）

**问题现象**：即使无 timeout，FQ-PT 仍存在大量重复任务（`processed_tasks/constraint_checks` 远大于必要值），
导致调度开销居高不下。

**改动**：
- `src/solver/gpu/GModel.cu`：
  - 新增 `FQPTTryMarkConstraintQueued/FQPTIsConstraintQueued/FQPTClearConstraintQueued`；
  - 生成后继任务时先 `mark`，仅在首次入队时追加到 local/global queue；
  - 弹出任务后若发现已是陈旧重复任务（标记已清），直接结算 pending，避免再次检查；
  - 任务完成/世界终止路径补充 `clear mark`，保持标记与队列状态一致。
- `src/solver/gpu/batch_probe_manager.cu`：
  - world 初始化时清零 `frontier_A/B`；
  - seed 初始化阶段按 world 的 `frontier_A` 去重，避免初始重复入队。

**结果（同口径 smoke）**：
- 正确性：`test_fqpt_baseline` 与典型样例回归均通过（`unknown=0` 时与 Stage2 一致）；
- 任务量：`constraint_checks` 约下降 40%~50%
  - `queens-12`: `16896 -> 8448`
  - `rand-2-23`: `64680 -> 32384`
  - `haystacks-11`: `17904 -> 10032`
- 耗时：FQ-PT 相比去重前提升约 10%~30%（仍慢于 Stage2）。

## 2026-02-07

### Docs：新增 Batch-3A Walkthrough（核心思想 / 算法 / 代码实现）

**动机**：现有 Batch-3A 文档偏向“复盘/救火/设计草案”，对首次接手代码的同学不够线性；
需要一份从概念到代码落点的一站式 walkthrough，降低理解门槛与上手成本。

**交付内容**：
- 新增 `docs/planning/BATCH3A_WALKTHROUGH_2026_02.md`，覆盖：
  - Batch-3A 的聚合对象（`<cid, world_mask>`）与关键不变量（world 写入互斥、UNKNOWN 语义）
  - `Batch3AManager::Execute()` → `Batch3AKernel_MultiBlock()` → `CollectResults()` 的端到端执行链
  - 三种 mapping（0/1/2）及其代码入口
  - SAC3 中的 Batch-3A gating / fallback 逻辑与调参、验证命令
- 更新 `docs/README.md`：在“关键规划文档”中注册 walkthrough 入口。

## 2026-02-05

### Batch-3A：Dynamic Submission 队列版（去掉 kernel 内全量扫描）

**动机**：Batch‑3A 扫描版的决定性瓶颈是 kernel 内 `threadIdx.x==0` 每轮全量扫描 `cid=0..num_cons-1`
构建 `local_task_cids/local_task_masks`，成本与 `num_cons`（甚至 `num_cons*worlds`）成正比，完全无法利用稀疏性，
并且会把 mapping=2 的 shared packing/bitGEMM 原型收益淹没（详见复盘：`docs/planning/BATCH3A_POSTMORTEM_2026_01.md`）。

**交付内容**：
- `src/solver/gpu/GModel.cu`：
  - 新增 device 侧队列原语：`Batch3AEnqueueConstraintToNextQueue()` / `Batch3AEnqueueVarToNextQueue()`
  - 重写 `Batch3AKernel_MultiBlock`：用双队列 A/B + per-cid `world_mask` 聚合实现 “结尾提交（Dynamic Submission）”，
    不再进行“开头扫全约束”。
- `include/solver/gpu/batch_probe_manager.h` / `src/solver/gpu/batch_probe_manager.cu`：
  - `Batch3AControl` / `Batch3AManager` 增加 per-block 队列与 mask 缓冲区（`queue_capacity=num_cons` 作为保守起点）
  - kernel launch 前清零队列与 mask；若溢出则保持 `active_world_mask` bit（按 UNKNOWN 语义不删值）。
- 文档同步：
  - 新增 `docs/planning/BATCH3A_DYNAMIC_SUBMISSION_QUEUE_DESIGN.md`
  - `docs/planning/BATCH3A_POSTMORTEM_2026_01.md` 增加“2026-02 更新”说明
  - `docs/README.md` 注册新文档入口

**测试结果**：
- `make -j$(nproc)`：通过
- `python3 tests/python/batch_test_v2.py --tier=0`：8/12 (66%)，与历史基线一致（不匹配项仍为 CPIM 超时）
- `./build/test_batch3a`：通过
- microbench（mapping=2 / perf suite，对照 Stage2）：`out/batch3a_queue_vs_stage2_perf_10min.csv` 中 `speedup_vs_stage2≈0.03–0.12`
  （8×–30× 慢于 Stage2），说明去掉“开头扫全约束”后瓶颈主要转向 host 侧框架成本（`InitializeWorlds` 等）。

### Batch-3A：Phase0 device 初始化（跳过 host InitializeWorlds）

**动机**：Dynamic Submission 去掉了 kernel 内的“开头扫全约束”，但 microbench 仍显示 8×–30× 回退；
进一步定位表明 host 侧 `InitializeWorlds()` 的逐 world `cudaMemcpy/cudaMemset + synchronize` 是主要框架瓶颈之一。

**交付内容**：
- `src/solver/gpu/GModel.cu`：扩展 `Batch3AKernel_MultiBlock` 的 Phase0：
  - device 并行恢复 snapshot：`domain_snapshot → ws->bitDom`，`dom_size_snapshot → ws->d_cur_dom_size`
  - 初始化 `WorldWorkspace` 控制字段与 `results[w]=true`
  - singleton assign + 初始 enqueue（probe var 的 subscription）
  - 队列版不再依赖 `ws->frontier_A/B`，Phase0 不清零 frontier bitmap（避免 O(num_cons) 纯开销）
- `src/solver/gpu/batch_probe_manager.cu`：
  - `Batch3AManager::Execute()` 跳过 `InitializeWorlds()`
  - `Batch3AManager::LaunchBatch3AKernel()` 补齐全局控制初始化（`active_world_mask/global_iteration/stats`）

**测试结果**：
- `make -j$(nproc)`：通过
- `python3 tests/python/batch_test_v2.py --tier=0`：8/12 (66%)，与历史基线一致
- `./build/test_batch3a`：通过
- microbench smoke（perf suite 取 3 个实例，mapping=2，对照 Stage2）：
  - `out/batch3a_queue_phase0init_smoke.csv`：`speedup_vs_stage2≈0.17–0.26`
  - 含义：回退显著收敛，但仍慢于 Stage2，后续仍需继续拆解剩余瓶颈

## 2026-01-29

### P2-2c：Batch-3A microbench 升级为 Stage2 对照（同口径 speedup）

**动机**：此前 P2-2c 只跑 `--mode=batch3a`，只能比较不同 `G/padding` 的相对曲线，无法回答
“Batch-3A（mapping=0/1/2）相对 Stage2 的真实 speedup 在哪些实例上成立”。这会直接影响
“要不要继续推进更重的 SoA/flatten”决策。

**交付内容**：
- `tests/python/batch_batch3a_microbench.py`：
  - 新增 `--include-stage2`：先跑 `--mode=stage2` baseline，再跑 `--mode=batch3a`；
  - 新增 `--mappings/--subwarp-sizes/--padding-values`：支持一次跑全消融组合；
  - CSV 增加 `stage2_avg_time_ms` 与 `speedup_vs_stage2` 字段，便于直接做 go/no-go。
  - 跑通 `suite=perf` 的 10 分钟对照样例（输出 `out/batch3a_vs_stage2_perf_10min.csv`），用于判断是否值得继续推进 SoA/flatten。
  - 跑通 `suite=stress` 的大实例门槛验证（输出 `out/batch3a_vs_stage2_stress_20min.csv`）：
    - `large-80/84`：Batch‑3A（mapping=2）相对 Stage2 的 `speedup_vs_stage2` 约 0.028–0.042（24×–36× 慢）
    - `large-92`：模型构建阶段触发 CUDA OOM（Stage2 baseline 无法获得）

### Suite：perf 哨兵剥离大实例到 stress（避免被解析/建模 wall-time 污染）

**调整**：
- `tests/python/sac_preprocess_tier_definitions.py`：
  - `SAC_PREPROCESS_PERF_SENTINELS` 移除 `benchmarks/marc/large-80-unsat_ext.xml` / `large-84-unsat_ext.xml`
  - 二者移动到 `SAC_PREPROCESS_STRESS`（在 60s perf 预算下容易 timeout，更适合单独拉高 timeout 跑）

### Docs：P2 任务状态与门槛更新

- `docs/planning/TODO_SACGPU_NEXT.md`：
  - 标记 P2-2b/P2-2c 已落地；
  - 补充 “何时进入 SoA/flatten” 的 go/no-go 门槛（基于 `speedup_vs_stage2` 的硬数据）。

### Docs：新增 Batch-3A 性能回退复盘文档（便于外部复核）

- 新增 `docs/planning/BATCH3A_POSTMORTEM_2026_01.md`：汇总 Batch‑3A 在 perf/stress 上结构性慢于 Stage2 的主要瓶颈
  （kernel 内单线程全量扫描构建任务 + host 分批初始化/同步 + mapping=2 pack/unpack 开销），并附关键代码片段与复现实验命令。
- 更新 `docs/README.md`：注册上述复盘文档入口。

## 2026-01-25

### P0-3：统一观测入口落地（full_sac vs SAC1/SAC3 preprocess 同口径）

**动机**：此前 preprocess 跑批主要基于 `sac_benchmark --mode=full_sac`（Stage2 全域扫一轮/多轮），
但 `GModelSolver::EnforceSAC1/EnforceSAC3`（flatten SACQ/NSACQ 的主线实现）没有可直接跑批/可解析的入口，
导致“改了 SAC3 但测不到”，P0-3 统计闭环缺口未补齐。

**交付内容**：
- `apps/sac_benchmark.cpp`：
  - 新增 preprocess 模式：`--mode=sac1_preprocess` / `--mode=sac3_preprocess`
  - 统一输出可解析字段：`Total time/Total probes/Total deletions/Avg/P95/Max iterations/Unknown probes/Status`
- `include/GModelSolver.h` / `src/solver/gpu/GModelSolver.cu`：
  - 新增 `EnableSacProbeStats()` 观测开关与 `GetLastSac*()` 统计读取接口（默认关闭，避免影响搜索阶段）
  - SAC3 增加 `max_rounds` 的 soft-budget 截断（与 `full_sac --max_sac_rounds` 的 TIMEOUT 语义对齐）
- `tests/python/select_sac_preprocess_benches.py` / `tests/python/batch_sac_benchmark.py`：
  - 支持 `--mode=full_sac/sac1_preprocess/sac3_preprocess`
  - CSV 增加 `mode` 与 `p95_iterations` 字段，并改用 `(mode,nsac,max_rounds,path)` 作为 resume key
- 文档同步：
  - 更新 `docs/planning/TODO_SACGPU_NEXT.md`：P0-3 标记完成并说明 A/B 对照路径
  - 更新 `docs/guides/SAC_PREPROCESS_GUIDE.md`：补充 `--mode=sac3_preprocess` 用法与 `P95 iterations` 指标
  - 更新 `docs/planning/SACGPU_NEXT_ACTIONS_10MIN_TIMEOUT.md`：补齐 `--mode` 并移除“P0-3 未完成”的过时段落

**工程修复**：
- `CMakeLists.txt`：`sac_benchmark` 增加链接 `src/solver/gpu/GModelSolver.cu`（否则新增模式会出现链接缺符号）。

**测试结果**：
- `python3 tests/python/batch_test_v2.py --tier=0`：8/12 (66%)，与历史基线一致，无退化（不匹配项仍为 CPIM 超时）。

### P1：外层队列预算 + Stage 选择分桶缓存（长尾可控/并行度吃满）

**动机**：在 flatten SACQ/NSACQ（SAC3 preprocess）路径上，除了 per-probe 的 `UNKNOWN`/停滞/量子外，还需要
host 侧的“外层队列预算”来避免 requeue/queue 爆炸；同时 AutoStageSelector 的单一全局缓存会被首次 batch 的规模污染，
导致后续小 batch 仍走 Stage2（欠饱和/高开销），或反过来大 batch 仍走 Stage1。

**交付内容**：
- `include/GModelSolver.h` / `src/solver/gpu/GModelSolver.cu`：
  - 新增 `GModelSolver::SacQueueBudgetConfig`（`max_total_probes/max_queue_size/max_total_requeues/max_requeues_per_var`）
  - SAC3 支持 `max_total_probes` 截断（达到上限 early_stop；只会少删，不会多删）
  - ProbeQueue 支持 queue cap 与 requeue cap（溢出丢弃入队/停止扩张），并在 verbose 下输出 budget 统计
- `include/solver/gpu/batch_probe_manager.h` / `src/solver/gpu/batch_probe_manager.cu`：
  - `AutoStageSelector::DecideCached()` 改为 **按任务量分桶缓存（medium/large）**
  - 每次调用先走 `DecideByTaskCount()`：小任务直接 Stage1，不再受缓存污染
- `apps/sac_benchmark.cpp`：preprocess 模式增加 queue-budget flags，便于消融与 hard-case 控制

**测试结果**：
- `python3 tests/python/batch_test_v2.py --tier=0`：8/12 (66%)，与历史基线一致，无退化（不匹配项仍为 CPIM 超时）。

### Suite：Regression 用例去重/控时（便于日常跑通）

**动机**：`benchmarks/marc/large-80-unsat_ext.xml` 的 preprocess 本体很快（~200ms），但解析/建模 wall-time 可达 ~60s，
会导致 `--timeout=60` 的 regression 批跑不稳定（即使算法没卡住）。

**调整**：
- `tests/python/sac_preprocess_tier_definitions.py`：
  - `SAC_PREPROCESS_REGRESSION` 移除 `large-80-unsat_ext.xml`（仍保留在 `perf`）
  - 增加 `composed-25-1-2-0_ext.xml` 作为“快速 UNSAT/DWO”回归样例

## 2026-01-27

### P2-1：Batch-3A Route-A（Subwarp-per-World）算子形态试上限

**动机**：Batch-3A 的现有 Warp-per-World 在 `bit_dom_int_size` 较小（例如 6/8/12）时容易出现 lane 利用率低，
但直接做 AoS 下的 “Thread-per-World” 会把 `bitDom` 访问退化成跨 world 的 stride 访存（UMA 上大概率更慢）。
因此先落地 **Subwarp-per-World**（在不改 AoS 布局前提下的低风险向量化），用于评估是否值得推进后续 SoA/packing（P2-2）。

**交付内容**：
- `include/solver/gpu/batch_probe_manager.h` / `src/solver/gpu/batch_probe_manager.cu`：
  - Batch-3A 新增 `check_mapping/subwarp_size`（`kWarpPerWorld` vs `kSubwarpPerWorld`，`subwarp_size=4/8/16`）
  - `Batch3AManager` 增加对应 setter，保持默认不变（便于消融）
- `src/solver/gpu/GModel.cu`：
  - 新增 `ExecuteConstraintCheck_Aggregated_SubwarpPerWorld()` 并在 Batch-3A kernel 中按 `check_mapping` 分发
  - 修复 Batch-3A wrapper 的 dynamic shared memory 计算：仅计入 `bitSup + del_buffer`，避免把静态 shared 数组重复计入
- `apps/sac_benchmark.cpp` / `tests/cpp/test_batch3a.cpp`：
  - 新增 flags：`--batch3a_check_mapping` / `--batch3a_subwarp_size`，用于 A/B 消融对照

**测试结果**：
- `python3 tests/python/batch_test_v2.py --tier=0`：8/12 (66%)，与历史基线一致，无退化（不匹配项仍为 CPIM 超时）。
- `./build/test_batch3a`：`mapping=0` 与 `mapping=1` 均通过，且与 Stage2 结果一致。

### P2-2：Batch-3A Route-B 原型（warp-per-word + lane-per-world + shared dom packing）

**动机**：P2-1 的 Subwarp-per-World 依然受 AoS（`[world][var][word]`）带来的跨 world stride 访存影响，
理论性能上限偏低。P2-2 先落地一个 **不改全局 layout** 的原型：在 shared memory 中把 dom 打包成 `[word][world]` 矩阵，
让热点访问在 block 内变成“像 SoA”。

**交付内容**：
- `include/solver/gpu/batch_probe_manager.h`：`Batch3ACheckMapping` 新增 `kWarpPerWordLaneWorld=2`
- `src/solver/gpu/GModel.cu`：
  - 新增 `ExecuteConstraintCheck_Aggregated_WarpPerWordLaneWorld()`：计算阶段 lane→world，应用阶段保留 warp-per-world
  - Batch-3A dispatch 支持 `mapping=2`
  - wrapper 对 `mapping=2` 调整 `worlds_per_block` 默认值与 dynamic shared memory 预算
- `apps/sac_benchmark.cpp` / `tests/cpp/test_batch3a.cpp`：`--batch3a_check_mapping` 扩展支持 `2`

**测试结果**：
- `python3 tests/python/batch_test_v2.py --tier=0`：8/12 (66%)，与历史基线一致，无退化（不匹配项仍为 CPIM 超时）。
- `./build/test_batch3a --batch3a_check_mapping=2`：与 Stage2 结果一致。

- P2-2 调参入口（G 参数化）：
  - `include/solver/gpu/batch_probe_manager.h` / `src/solver/gpu/batch_probe_manager.cu` / `src/solver/gpu/GModel.cu`：
    - `Batch3AControl` 新增 `requested_worlds_per_block`（0=auto）
    - wrapper 在 `mapping=2` 时支持覆盖 `G`
  - `apps/sac_benchmark.cpp` / `tests/cpp/test_batch3a.cpp`：
    - 新增 `--batch3a_worlds_per_block=0|1..32`（默认 0，不改变现有行为）
  - P2-2c microbench 工具：
    - 新增脚本 `tests/python/batch_batch3a_microbench.py`：批量跑 `./build/sac_benchmark --mode=batch3a` 扫 `G` 并导出 CSV（支持 `--resume`/原子 flush）。
  - P2-2b shared padding：
    - `--batch3a_shmem_padding=0|1`（默认 0；仅对 `mapping=2` 的 shared packing 生效），通过 pitch padding（`G+1`）降低 bank conflict 风险。

- 新增规划文档 `docs/planning/DGPU_MEMORY_PLAN.md`：给出从 Jetson UMA 迁移到 PCIe dGPU（RTX 4060/4090/2080 等）
  的内存后端设计（GPU-resident、pinned+async 双缓冲、delta 删除 GPU 应用、可消融/可回退/可观测），用于后续减少/隐藏 memcpy。
- 更新文档导航 `docs/README.md`：注册 dGPU 内存规划文档入口。

## 2026-01-21

### Preprocess：补齐“回归/性能哨兵”评测集（suite）+ 澄清 SAC vs NSAC 口径

**动机**：tier0/1/2 更偏“展示集”（数据驱动生成、可能随筛选刷新），不一定适合作为“每次改动都跑”的回归与关键节点性能对照。
同时，近期评测 CSV 容易把 `status=TIMEOUT` 误解为 wall-time 超时；并且很多跑批默认 `nsac_mask=1`，
需要在教程里明确“跑的是 SAC 外框 + NSAC 传播半径”。

**交付内容**：
- `tests/python/sac_preprocess_tier_definitions.py`：新增手工精选 suite
  - `SAC_PREPROCESS_REGRESSION`：日常回归覆盖（高删值/高 probes/SAC-DWO/0 删值收敛/较高耗时）
  - `SAC_PREPROCESS_PERF_SENTINELS`：关键节点性能哨兵（吞吐/延迟/内存压力）
  - `SAC_PREPROCESS_STRESS`：压力样例（例如 Jetson 上可能 OOM 的大实例）
  - `SAC_PREPROCESS_KNOWN_BAD`：已知解析/输出问题样例（扫描时建议 exclude）
- `tests/python/batch_sac_benchmark.py`：新增 `--suite` 参数（可直接跑 regression/perf/stress 等）
- `docs/guides/SAC_PREPROCESS_GUIDE.md`：
  - 新增“这里跑的是 SAC 还是 NSAC？”解释
  - 增加 `--suite=regression/perf` 的推荐命令模板

**修改文件**：
- `tests/python/sac_preprocess_tier_definitions.py`
- `tests/python/batch_sac_benchmark.py`
- `docs/guides/SAC_PREPROCESS_GUIDE.md`
- `CHANGES_ZH.md`

## 2026-01-19

### Docs：对齐“下一步工作”与最新评测结论（区分 e2e 与 preprocess 口径）

**背景**：近期评测表明 Batch-3A 在现有用例上 A/B 测试慢 10–25x；同时部分 hard bench 的 e2e TIMEOUT 主要发生在搜索阶段。
但本阶段的研究目标更偏向“preprocess/推理能力（SAC/MSAC/NSAC）的速度与上限”，需要把评测口径从 e2e 求解切回 preprocess，
并挑选“删值多/传播深”的实例。

**交付内容**：
- 更新 `docs/planning/NEXT_STEPS_2026_01.md`：把主线明确为“preprocess 指标闭环（P0-3）+ 构建对 SAC 有意义的评测集”，并把 e2e 搜索仅作为 sanity/回归口径；同时将 Batch-3A/bitGEMM/BMMA 后置为设门槛的研究路线。
- 更新 `docs/planning/TODO_SACGPU_NEXT.md`：在现状快照与 P1-1 条目中补充 Batch-3A 的评测结论，建议默认关闭仅保留消融开关。
- 新增 preprocess 评测集筛选/跑批脚本：
  - `tests/python/select_sac_preprocess_benches.py`：扫描 `benchmarks/` 跑 `sac_benchmark --mode=full_sac`，输出 CSV 并生成 `SAC_PREPROCESS_TIER0/1/2`
    - 支持 `--resume/--flush-every`：可中断/续跑，进度周期性落盘（原子写入）
    - 支持 `--shuffle/--limit/--include/--exclude`：可抽样或按目录定向筛选
  - `tests/python/batch_sac_benchmark.py`：按 preprocess tier 批量运行 `sac_benchmark` 并导出 CSV（支持 `--resume/--flush-every`）
  - `tests/python/sac_preprocess_tier_definitions.py`：预置的 preprocess tier 列表（可用筛选脚本刷新）
- 新增教程并注册到文档导航：
  - `docs/guides/SAC_PREPROCESS_GUIDE.md`
  - `docs/README.md`
  - `docs/planning/SACGPU_NEXT_ACTIONS_10MIN_TIMEOUT.md`

**修改文件**：
- `docs/planning/NEXT_STEPS_2026_01.md`
- `docs/planning/TODO_SACGPU_NEXT.md`
- `docs/guides/SAC_PREPROCESS_GUIDE.md`
- `docs/README.md`
- `docs/planning/SACGPU_NEXT_ACTIONS_10MIN_TIMEOUT.md`
- `tests/python/select_sac_preprocess_benches.py`
- `tests/python/batch_sac_benchmark.py`
- `tests/python/sac_preprocess_tier_definitions.py`

## 2026-01-18

### Search：修复 `time_limit` 超时判断 + 搜索阶段增量 GAC

**目标**：让 `compare_cpu_gpu` 的 GPU 搜索按 `--time_limit` 正确停止；并把每个搜索节点的 GAC
从“全量激活所有约束”改为“只激活刚赋值变量的邻接约束”，降低搜索阶段传播开销（语义不变）。

**交付内容**：
- `GModelSolver::Solve()`：计算 deadline 并传入递归搜索
- `GModelSolver::Search()`：用 `std::chrono::steady_clock` 做超时判断（修复原先每次新建 Timer 导致超时永不触发）
- 搜索节点传播：`model_->EnforceGAC(false, var)`（增量 GAC）

**修改文件**：
- `include/GModelSolver.h`
- `src/solver/gpu/GModelSolver.cu`

### Tests：对齐 TIER1/TIER2 实例数量说明

`tests/python/tier_definitions.py` 的文档注释中，TIER1/TIER2 数量已对齐到当前实际列表
（TIER1=39，TIER2=79），避免后续跑批时误判“缺例/多例”。

**修改文件**：
- `tests/python/tier_definitions.py`

### Tests：新增三方对比（CPIM CPU vs CPIM GPU vs OR-Tools CP）

**目标**：在同一套 TIER 用例下，同时跑 CPU/GPU/OR-Tools 三条链路，便于回归与定位
“CPU/GPU 之间不一致”以及“与 OR-Tools 判定不一致”的实例。

**交付内容**：
- 新增 `tests/python/batch_test_v3.py`：三方对比测试脚本（默认 GPU 走 `build/compare_cpu_gpu --gpu_only`）
- 更新 `docs/planning/TODO_SACGPU_NEXT.md`：最小回归清单加入三方对比入口

**修改文件**：
- `tests/python/batch_test_v3.py`
- `docs/planning/TODO_SACGPU_NEXT.md`

## 2026-01-17

### P0-1d：失败概率优先（Failure Priority / bucketed queue）

**目标**：优先执行“更可能 DWO”的 probes，让删值尽早发生，从而减少后续传播成本并抑制长尾；只改变调度顺序，
不改变 soundness 语义（仍然只对 `kDWO` 删值，`kUNKNOWN` 不删）。

**交付内容**：
- `GModelSolver::FailurePriorityConfig`：运行时开关与权重/桶数配置
- `EnforceSAC3()`：queue mode 下支持按 bucket 出队，并在 verbose 下输出各 bucket 的 DWO 命中率统计
- `apps/compare_cpu_gpu.cpp`：增加参数用于启用/调参：
  - `--sac_failure_priority`
  - `--sac_failure_priority_buckets`
  - `--sac_failure_priority_w_dom / --sac_failure_priority_w_deg / --sac_failure_priority_w_hist`
  - `--sac_failure_priority_min_hist_probes`

**修改文件**：
- `include/GModelSolver.h`
- `src/solver/gpu/GModelSolver.cu`
- `apps/compare_cpu_gpu.cpp`
- `docs/planning/TODO_SACGPU_NEXT.md`

---

### P0-2：NSAC `allowed-constraints mask`（真邻域子图）

**目标**：singleton test 的传播严格限制在 `Xi + N(Xi)` 诱导子图（NSAC），减少传播半径抑制长尾；语义仍然 sound
（只对 `kDWO` 删值，预算/停滞触发为 `kUNKNOWN` 不删）。

**交付内容**：
- `GModel::BuildAllowedMasks()`：为每个 focal variable 预计算 allowed constraints 位图
- Stage2/Stage1 的 BlockSync GAC：在 frontier 扩张（`PropagateVarToNextBitmap`）阶段按 focal var 过滤，避免传播越过邻域子图
- `GModelSolver::NSACMaskConfig` + `--sac_nsac_mask`：运行时开关（关闭回退到原全图传播）

**修改文件**：
- `include/GModel.cuh`
- `src/solver/gpu/GModel.cu`
- `include/GModelSolver.h`
- `src/solver/gpu/GModelSolver.cu`
- `include/solver/gpu/batch_probe_manager.h`
- `src/solver/gpu/batch_probe_manager.cu`
- `apps/compare_cpu_gpu.cpp`
- `docs/planning/TODO_SACGPU_NEXT.md`

**验收**：`batch_test_v2.py --tier=0` 通过（8/12 匹配，与之前一致；4 个超时为既有性能问题）

---

### P1-1：Batch-3A 接入 SAC3 主路径（可选加速器 + NSAC gating）

**目标**：把 Batch-3A（约束聚合）作为 SAC3 的可选后端接入主路径，用于评估其在 `nsac_mask=false` 下的吞吐/长尾收益；
同时保持默认 NSAC 的语义一致性与可回退性。

**关键策略（最小风险）**：
- 运行时开关：`--sac_use_batch3a`
- **NSAC gating**：当 `nsac_mask_enabled=true` 时自动禁用 Batch-3A 并回退 Stage2（保证默认路径始终“真 NSAC”）
- UNKNOWN 输出：Batch-3A 若在 `max_iterations` 内未收敛（`active_world_mask` 仍有 bit），对这些 probes 标记为 UNKNOWN（不删，可进入 deferred recheck）

**交付内容**：
- `GModelSolver::Batch3AConfig`：Batch-3A 运行时配置
- `GModelSolver::EnforceSAC3()`：在满足条件时走 Batch-3A，否则回退原 Stage1/Stage2
- `Batch3AManager::Execute(..., unknown_vars, unknown_values)`：支持返回 UNKNOWN probes
- 修复 Batch-3A 初始化 `active_world_mask` 在 `num_worlds==32` 时的移位未定义行为

**修改文件**：
- `include/GModelSolver.h`
- `src/solver/gpu/GModelSolver.cu`
- `include/solver/gpu/batch_probe_manager.h`
- `src/solver/gpu/batch_probe_manager.cu`
- `apps/compare_cpu_gpu.cpp`
- `docs/planning/TODO_SACGPU_NEXT.md`

---

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

### P0-1a：停滞检测（Stagnation Detection）

**背景**：比 `max_iterations` 硬截断更智能的长尾检测，用多指标判定"停滞"。

**检测指标**：
- `Δdeletions`：连续 k 轮 deletions==0（停滞计数）
- `frontier_popcount`：活跃约束数
- `deletions / work_cnt`：单位工作产出率

**交付内容**：
- `WorldWorkspace` 添加字段：`stagnation_count`, `last_deletions`, `last_frontier_popcount`, `work_cnt`
- `Batch2PersistentControl` 添加配置：`stagnation_threshold`, `min_productivity`, `enable_stagnation_check`
- `Batch2PersistentManager` 添加 API：`SetStagnationThreshold()`, `SetMinProductivity()`, `EnableStagnationCheck()`
- `RunGACToFixpoint_BlockSync` 每轮后检测停滞条件，触发时提前退出并标记 UNKNOWN

**修改文件**：
- `include/solver/gpu/batch_probe_manager.h`: WorldWorkspace 停滞字段、Control 停滞配置、Manager API
- `src/solver/gpu/batch_probe_manager.cu`: 传递停滞参数到 control
- `src/solver/gpu/GModel.cu`: RunGACToFixpoint_BlockSync 停滞检测逻辑
- `docs/planning/TODO_SACGPU_NEXT.md`: 更新 P0-1 子任务

**验收**：`batch_test_v2.py --tier=0` 通过（8/12 匹配，与之前一致）

---

### P0-1b：时间片调度基础设施（Timeslice Scheduling Infrastructure）

**背景**：为长尾 probe 提供"工作量子"限制，防止单个 probe 阻塞整个批次。

**交付内容**：
- `GACTimesliceState` 结构体（预留完整暂停/恢复用）
- `WorldWorkspace` 添加字段：`total_constraints_checked`, `quantum_exceeded`
- `Batch2PersistentControl` 添加配置：`quantum_cid`, `enable_quantum_check`
- `Batch2PersistentManager` 添加 API：`SetQuantumCid()`, `EnableQuantumCheck()`
- `RunGACToFixpoint_BlockSync` 添加工作量子检查逻辑

**修改文件**：
- `include/solver/gpu/batch_probe_manager.h`: GACTimesliceState、WorldWorkspace 时间片字段、Control/Manager API
- `src/solver/gpu/batch_probe_manager.cu`: 传递时间片参数到 control
- `src/solver/gpu/GModel.cu`: RunGACToFixpoint_BlockSync 工作量子检查

**当前状态**：基础设施完成，默认关闭（`enable_quantum_check=0`）。完整的 yield/resume 逻辑待 Batch-3A 载体稳定后实现。

**验收**：`batch_test_v2.py --tier=0` 通过（8/12 匹配，与之前一致）

---

### P0-1c：延后复查队列（Deferred Recheck Queue）

**背景**：P0-1a/P0-1b 让 Stage2 能识别并提前退出长尾 probe（标记 `kUNKNOWN`），但 SAC3 顶层此前只收集 `kDWO`，
导致 UNKNOWN probes 被直接丢弃，无法在邻域发生删值变化后重检。

**交付内容**：
- `GModelSolver::DeferredRecheckConfig`：deferred queue 的运行时配置（开关/上限/重检次数/超期）。
- `DeferredProbeQueue`（在 `EnforceSAC3()` 内部集成）：UNKNOWN probes 入队，邻域 epoch 变化后出队复查。
- `Batch2PersistentManager::ExecutePersistentBlocks()` 增强接口：额外返回 UNKNOWN probes 列表（var/value）。
- SAC3 主循环集成：
  - 维护 `nb_epoch[var]`（邻域 epoch），当删值/GAC 级联删值发生时对 `var` 及其邻居递增；
  - queue 模式下优先调度 deferred ready probes，并用 regular queue 补满 batch；
  - 统计 `deferred_in/out/hit/stale/overflow`（verbose 输出）。

**修改文件**：
- `include/GModelSolver.h`: 新增 `DeferredRecheckConfig`
- `src/solver/gpu/GModelSolver.cu`: `DeferredProbeQueue` + `EnforceSAC3()` 集成
- `include/solver/gpu/batch_probe_manager.h`: Stage2 manager 增强接口（返回 UNKNOWN probes）
- `src/solver/gpu/batch_probe_manager.cu`: 收集 UNKNOWN probes 并返回给 host

**验收**：`batch_test_v2.py --tier=0` 通过（8/12 匹配，与之前一致；4 个超时为既有性能问题）

---

### 修复：`sac_benchmark` Full SAC 下的 kernel launch `invalid argument`

**问题**：`sac_benchmark --mode=full_sac` 会一次性提交大量 probes；Stage2（Persistent Blocks）
在 auto-tune 重新分配 workspaces 后，可能出现 `num_tasks > max_tasks_`，导致任务数组 `cudaMemcpy`
越界写，最终在 kernel launch 时报 `invalid argument`。

**修复**：在 `Batch2PersistentManager::ExecutePersistentBlocks()` 中加入防御性扩容：当
`num_tasks > max_tasks_` 时自动 `ReserveTaskCapacity()`，确保任务/结果数组容量充足。

**修改文件**：
- `src/solver/gpu/batch_probe_manager.cu`

---

### SACGPU 下一阶段 TODO 备忘

- 新增 `docs/planning/TODO_SACGPU_NEXT.md`：整理 P0-P3 的实施清单（UNKNOWN 语义、NSAC mask、
  Batch-3A 接入、bitGEMM 路线与 `bmma_sync(b1, AND+POPC)` 插入点），作为后续迭代备忘录。
- 补充：在 P1-1 增加依赖关系说明（P0-1/P0-3），在 P3-2 明确 BMMA 对 “world 列矩阵 packing 载体稳定”
  的前提要求。
- 补充：细化 P0-1c“延后复查队列（Deferred Recheck）”方案（版本/邻域 epoch 两档实现），并更新现状快照避免与代码状态不一致。

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
