---
status: active
updated: 2026-02-16
---

# FQ-PT-CID-CTA-MB 独立裁决（Codex，2026-02）

> 目标：把 `docs/planning/SACGPU_new.md` 与 `gemini_doc/SACGPU_new_review.md` 收敛为可执行裁决稿。  
> 定位：工程裁决文档，不是实现草案，不替代代码与测试结果。

## 0. 评审输入与口径

- 评审输入：
  - `docs/planning/SACGPU_new.md`
  - `gemini_doc/SACGPU_new_review.md`
- 代码口径（静态对照）：
  - `src/solver/gpu/GModel.cu`
  - `include/solver/gpu/batch_probe_manager.h`
  - `src/solver/gpu/batch_probe_manager.cu`
  - `tests/cpp/test_fqpt_baseline.cpp`
- 评审边界：
  - 仅静态代码证据，不跑 benchmark，不追加 smoke test 输出。
  - 不改任何 C++/CUDA 接口与行为；本文件仅给出实现裁决与落地门槛。

## 1. 结论先行（可执行）

1. **该做**：在“不改 bitSubDom 全局布局”的约束下，`cid` 分组 CTA micro-batch 是合理下一步。  
2. **必须先做对**：`check_shared` 必须 warp 私有化；提交（commit）继续单点串行，避免状态面并发错误。  
3. **分阶段做**：第一阶段允许保留 `bit_dom_int_size==1` 的 Legacy 路径回退，不强制同批重写。  
4. **先防退化再扩并行**：线性分桶必须带“低聚合退化路径”，否则会在 `cid` 分散场景回退。  
5. **谨慎表述瓶颈**：`3-4KB shared` 不是当前可直接定性的首要瓶颈，需用实测确认是否成为 occupancy 主因。  

## 2. 观点裁决矩阵

| 观点 | 裁决 | 代码证据 | 工程影响 | 建议动作 |
|---|---|---|---|---|
| `cid` 分组 CTA micro-batch 是当前约束下的优先路径 | 成立 | `src/solver/gpu/GModel.cu:2623`（FQPT kernel 入口）；`src/solver/gpu/GModel.cu:2705`（当前一次仅取一个 task） | 现状是“单 task 控制流 + 重同步”，可压缩控制面开销 | 进入实现主线，优先做“同 cid 分桶 + 同 CTA 批处理” |
| `check_shared` 必须 warp 私有化 | 成立 | `src/solver/gpu/GModel.cu:2640`（当前 block 级 `check_shared`）；`src/solver/gpu/GModel.cu:1919`（`new_dom_x/new_dom_y` 使用共享 scratch） | 多 warp 并行多 world 时会发生共享区冲突 | 设计为 `check_shared_per_warp[WARPS_PER_CTA][check_words]` |
| SACGPU_new 对 warp 映射描述可直接照抄 | 部分成立（需纠偏） | `src/solver/gpu/GModel.cu:1932`（当前是 warp-per-word 条纹）；`src/solver/gpu/GModel.cu:1994`（thread0 汇总写回） | 复用现有检查核时，若忽略现有语义会导致实现偏差 | 明确“先适配现有 WarpPerWord 语义，再做 warp-per-world 重构” |
| “3-4KB shared 会明显压低 occupancy” | 部分否定（表述过强） | `src/solver/gpu/GModel.cu:3247`（shared 由 `check + 3*task buffer` 组成）；`include/solver/gpu/batch_probe_manager.h:1095`（默认 `local_buffer_capacity=64`） | 风险存在，但无法仅凭该量级直接定性为主瓶颈 | 改为“候选风险，需结合 occupancy 与耗时实测” |
| `bit_dom_int_size==1` 路径被忽略是重大缺口 | 成立但分阶段处理 | `src/solver/gpu/GModel.cu:2047`（小域走 Legacy） | 一次性重写全部路径风险大 | Phase 1 允许 Legacy 回退；Phase 2 再评估统一路径 |
| 线性分桶会在低聚合场景退化 | 成立 | `src/solver/gpu/GModel.cu:2629`（`cta_pop_batch`）；`src/solver/gpu/GModel.cu:2685`（批量 pop） | 若每桶近似 size=1，会引入纯分桶开销 | 强制退化规则：`max_bucket_size<=1` 走原逐任务流 |
| lock 冲突风险需要观测闭环 | 成立 | `src/solver/gpu/GModel.cu:2737`（CAS 拿锁重试）；`src/solver/gpu/GModel.cu:2763`（本地 retry） | micro-batch 增加并发拿锁点，可能拉低有效并行 | 增加最小统计：`lock_fail/retry`、`stale_drop`、`bucket_util` |
| 退出条件逻辑“当前必然错误” | 部分否定（实现细节风险） | `src/solver/gpu/GModel.cu:2701`、`src/solver/gpu/GModel.cu:2789`、`src/solver/gpu/GModel.cu:2851`（现有有块级屏障） | 当前框架并非必然错误，但改成 micro-batch 后若屏障位置错误会出错 | 在改造规范中强制“batch 全 warp 完成后才可评估退出条件” |
| 现有统计足以支撑 micro-batch 调优 | 不成立 | `include/solver/gpu/batch_probe_manager.h:1103`（`FQPTStatistics` 字段有限）；`include/solver/gpu/batch_probe_manager.h:1073`（仅 checks/deletions） | 无法识别锁冲突、陈旧任务、分桶收益 | 扩展最小统计字段（见第 4 节） |

## 3. 关键争议点裁决

### 3.1 `cid`-CTA micro-batch 是否应推进

- 裁决：**推进**。  
- 前提：不改 world-major 布局、保持 soundness、保持可回退。  
- 原因：当前 FQPT 为单 task 主循环，存在明显控制面同步与调度开销空间（见 `src/solver/gpu/GModel.cu:2623`）。  

### 3.2 检查核复用方式

- 裁决：**不能直接照抄“warp-per-world + lane->word”口号**。  
- 现实：当前检查核核心是 WarpPerWord 条纹模型（`src/solver/gpu/GModel.cu:1932`）。  
- 约束：先处理共享 scratch 私有化与写回职责拆分，再做并行拓扑切换。  

### 3.3 shared memory 风险怎么表述

- 裁决：**保留风险，但禁止先验定性**。  
- 现实：shared 总量取决于 `check_words + 3*task buffer`（`src/solver/gpu/GModel.cu:3244` 至 `src/solver/gpu/GModel.cu:3247`），还受 block 数、寄存器和实际访存影响。  
- 结论：shared 压力是“应观测项”，不是“先验主瓶颈”。  

### 3.4 Legacy 路径是否必须同批重写

- 裁决：**第一阶段不强制**。  
- 现实：小域分支明确存在（`src/solver/gpu/GModel.cu:2047`）。  
- 策略：先保留 Legacy 回退确保正确性，后续再决定是否统一。  

### 3.5 线性分桶的退化处理

- 裁决：**必须定义退化路径**。  
- 规则：若 `max_bucket_size<=1` 或 `unique_cid≈pop_count`，直接走原逐任务路径。  
- 目的：避免低聚合场景中“分桶成本 > 计算收益”。  

### 3.6 lock 冲突与重试

- 裁决：**风险成立，必须统计化**。  
- 现实：当前已有锁重试与本地重试路径（`src/solver/gpu/GModel.cu:2737`、`src/solver/gpu/GModel.cu:2763`）。  
- 动作：统计 `lock_fail/retry`，作为是否继续提升并行度的硬门槛。  

### 3.7 退出条件与同步

- 裁决：**不是现有框架必错，而是改造后易错点**。  
- 强制条件：每个 micro-batch 内“检查完成 -> commit 完成 -> pending 与队列状态对齐”之后才能评估退出。  
- 现有参考：块级屏障组织在当前实现已存在（`src/solver/gpu/GModel.cu:2701`、`src/solver/gpu/GModel.cu:2851`）。  

## 4. 落地路线与门槛（Phase 0~3）

### Phase 0：观测与开关先行（不改语义）

- 目标：先建立“改了是否变好”的观测闭环。
- 动作：
  - 增加最小统计字段（只增统计，不改传播语义）。
  - 增加运行时总开关（默认关闭新路径）。
- 启停：
  - 默认 `off`。
  - 仅开发验证时启用。
- 回退：
  - 任意异常立即切回现有 FQPT 单任务路径。

### Phase 1：CTA 分桶 + 退化路径（不改检查核拓扑）

- 目标：先验证“分桶调度是否有收益”，不一次性引入 warp 并行检查改写。
- 预期边界：
  - Phase 1 可能出现“几乎无收益（`-2%~+2%`）”。
  - 原因：不改检查核拓扑时，收益主要来自 `cid` 局部性和控制流重排，可能不足以覆盖分桶开销。
- 默认参数（首轮）：
  - `block_size=256`（沿用 `src/solver/gpu/GModel.cu:3242`）
  - `cta_pop_batch=32`（实验值，区别于默认 4）
  - `local_buffer_capacity=64`（沿用默认）
- 必要约束：
  - 低聚合退化路径必须启用。
  - `pending` 与 `queued mark` 语义保持现状。
- Stop/Go（Phase 1 -> Phase 2）：
  - 正确性必须通过；
  - 性能允许平收益：`median` 不回退超过 `3%`，且单例最大回退不超过 `5%`；
  - 聚合信号至少命中其一：`avg_bucket_size >= 1.4` 的样例数 ≥ `3/5`，或
    `avg_bucket_utilization >= 0.35` 的样例数 ≥ `3/5`。
  - 不满足则停止进入 Phase 2，保留 Phase 1 路径为 default-off 消融项。

### Phase 2：并行检查（warp 私有 scratch + 单点 commit）

- 目标：将同 `cid` 多 world 的检查并行化。
- 必做：
  - `check_shared` warp 私有化。
  - commit 保持 thread0/warp0 串行提交，保持状态一致性。
- 暂不强制：
  - Legacy 小域路径统一重写。

### Phase 3：调参与可选增强（数据驱动）

- 目标：在正确性不回退前提下做吞吐调优。
- 可选项：
  - `K` 从 32 向 64 试探。
  - 仅当线性分桶成为热点时再考虑 block 内排序（例如 CUB）。
- 禁止项：
  - 无数据支撑时直接引入重型结构改造。

### Back-of-Envelope 估算（用于优先级判断）

固定指标：
- `R`：同 `cid` 聚合率（同一批任务中可形成有效 bucket 的比例）
- `B`：平均桶大小（`avg_bucket_size`）
- `W`：并行 warp 数（`group_warps_per_cta`）

粗略估算关系：
- `Phase1` 主要收益上限近似与 `R * (B-1)` 正相关；
- `Phase2` 额外收益上限近似与 `R * min(B, W)` 正相关；
- 当 `B` 接近 1 时，`Phase1` 可能仅带来噪声级变化。

三档场景（用于先验预期，不替代实测）：

| 场景 | 参数假设 | Phase 1 预期 | Phase 2 预期 |
|---|---|---|---|
| 低聚合 | `R≈0.15, B≈1.3` | `-2% ~ +2%` | `+3% ~ +8%` |
| 中聚合 | `R≈0.35, B≈2.0` | `0% ~ +6%` | `+10% ~ +20%` |
| 高聚合 | `R≈0.60, B≈3.2` | `+4% ~ +12%` | `+18% ~ +35%` |

### 验收门槛（后续实现必须满足）

1. 正确性门槛：
   - `tests/cpp/test_fqpt_baseline.cpp:171` 的 `unknown=0` 对齐 Stage2 约束必须成立。
2. 语义门槛：
   - 保持 UNKNOWN 不删值（soundness 不变）。
3. 观测门槛：
   - 至少可见 `lock_fail/retry`、`stale_drop`、`bucket_util`。
4. 回退门槛：
   - 新路径任意异常可无损回退到现有稳定路径。

### 接口/类型影响（本次提交 vs 后续建议）

- 本次提交：**仅文档改动，不修改任何 C++/CUDA 公共接口**。
- 后续建议（未实施）：
  - `FQPTControl` 扩展最小统计指针：`stale_drop_count`、`lock_fail_count`、`lock_retry_count`、`bucket_active_warp_count`、`bucket_total_warp_count`。
  - `FQPTStatistics` 扩展主机侧镜像字段：`stale_drop_count`、`lock_fail_count`、`lock_retry_count`、`avg_bucket_utilization`。

## 5. 风险与退化策略（高优先级）

| 风险 | 触发信号 | 退化策略 |
|---|---|---|
| 分桶后性能回退 | `bucket_util` 低，耗时上升 | 启用 `max_bucket_size<=1` 退化路径 |
| 锁冲突扩大 | `lock_fail/retry` 激增 | 降低并行度或缩小每批并发 world 数 |
| `pending`/mark 失衡 | 退出异常或任务计数异常 | 强制单点 commit，保持“清 mark 与 pending 结算”原子语义 |
| shared/寄存器资源导致驻留下降 | 吞吐下降、延迟上升 | 回退到较低并行配置，优先保正确性 |
| UNKNOWN 增多导致有效剪枝下降 | unknown_rate 升高 | 收紧重试/分桶策略并优先回退稳定路径 |

## 6. 附录代码锚点

- FQPT 主 kernel 与当前单任务流：
  - `src/solver/gpu/GModel.cu::FQPTBaselineKernel`（line hint: `2769`）
  - `src/solver/gpu/GModel.cu::FQPTBaselineKernel`（line hint: `2945`）
- 当前 `check_shared` 与检查核共享 scratch：
  - `src/solver/gpu/GModel.cu::FQPTBaselineKernel`（line hint: `2798`）
  - `src/solver/gpu/GModel.cu::ExecuteConstraintCheck_BpC_Workspace_WarpPerWord`
    （line hint: `1919`）
- 当前 WarpPerWord 语义与写回职责：
  - `src/solver/gpu/GModel.cu::ExecuteConstraintCheck_BpC_Workspace_WarpPerWord`
    （line hint: `1932`）
  - `src/solver/gpu/GModel.cu::ExecuteConstraintCheck_BpC_Workspace_WarpPerWord`
    （line hint: `1994`）
- Legacy 小域分支：
  - `src/solver/gpu/GModel.cu::ExecuteConstraintCheck_BpC_Workspace`
    （line hint: `2047`）
- 锁重试与 retry 路径：
  - `src/solver/gpu/GModel.cu::FQPTBaselineKernel`（line hint: `2975`）
  - `src/solver/gpu/GModel.cu::FQPTBaselineKernel`（line hint: `3010`）
- 退出条件与关键屏障位置：
  - `src/solver/gpu/GModel.cu::FQPTBaselineKernel`（line hint: `2861`）
  - `src/solver/gpu/GModel.cu::FQPTBaselineKernel`（line hint: `2867`）
  - `src/solver/gpu/GModel.cu::FQPTBaselineKernel`（line hint: `2939`）
- kernel shared memory 计算：
  - `src/solver/gpu/GModel.cu::LaunchFQPTBaselineKernelWrapper`
    （line hint: `3649`）
  - `src/solver/gpu/GModel.cu::LaunchFQPTBaselineKernelWrapper`
    （line hint: `3654`）
- FQPT 控制与统计结构：
  - `include/solver/gpu/batch_probe_manager.h::FQPTControl`
    （line hint: `1042`）
  - `include/solver/gpu/batch_probe_manager.h::FQPTStatistics`
    （line hint: `1123`）
- 正确性回归约束（unknown=0 对齐）：
  - `tests/cpp/test_fqpt_baseline.cpp::TestFQPTBaseline`
    （line hint: `171`）
