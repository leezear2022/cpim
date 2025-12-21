# GPU GAC 持久化内核 + 约束状态机方案（Jetson 友好）

目标：将 `enforceGAC` 的“事件构建→传播→收敛”闭环搬到设备端，以一次持久化 kernel 驱动全流程，结合“约束状态机 + 设备端工作队列”避免同一约束并发执行，降低 Host↔Device 往返与多次 kernel 启动开销，提升 Jetson Orin 上的小批量利用率与稳定性。

- 适用平台：Jetson Orin (SM 8.7, CUDA 12.x)
- 适用代码：`include/cuSAC.cuh`, `src/cuSAC.cu`（不改变 `CModel` 外部接口）
- 与现有实现的关系：保留现有 `CsCheckMain` 的核心位运算逻辑与数据布局（bitDom/bitSup/MCon），重写循环/调度层。

## 1. 背景与问题
- 现状是“波次执行 + Host 同步 + `compress_Main()` 统计下一波”。当 `num_ConEvt` 较小、多轮迭代时：
  - kernel 启动 + `cudaDeviceSynchronize()` 的固定开销不可忽略；
  - 批量不足导致 SM 利用率偏低；
  - Host 上的压缩/去重引入额外往返与内存带宽消耗。
- CsCheck 本体较小（位运算为主），更适合用持久化 kernel 内部循环“吃完为止”。

## 2. 方案概述
一次启动持久化 kernel，设备端维护工作队列与约束状态机，直到收敛或失败：
- 工作队列（frontier）：保存待处理的约束 id（必要时携带受影响变量/值键 `key=(c<<16)|x`）。
- 约束状态机：保证“同一约束同一时刻最多被一个工作者处理”；处理中再次触发则折叠为一次“待重跑”。
- 设备端构建下一波：在 CsCheck 内部对受影响邻接约束调用 `schedule()` 入队（或标记重跑），无需回到 Host。
- 退出判定：队列空且活动工作者为 0（或 cooperative groups 网格屏障）→ 收敛退出；一旦空域→ 失败退出。

## 3. 核心数据结构
- `int* d_frontier;` 设备端环形队列或双缓冲（容量 ≥ 约束数，留 20–30% 冗余）
- `int d_q_head, d_q_tail;` 原子推进的队头/队尾
- `int* d_state;` 每约束 1 字节或 1 整数：0=Idle, 1=InQueue, 2=Processing
- `int* d_pending;` 每约束 0/1 标志，处理期间若再次触发则置 1
- `int d_active_workers;` 活动工作者计数（取任务 ++，释放 --）
- `int d_gac_success;` 0/1 全局标志；域清空即置 0
- 复用现有：`d_subscription`, `d_subscription_offset`, `d_MCon`, `d_ConPre`, `d_bitDom`, `d_cur_dom_size`, `texObj_BitSup`, `texObj_MCon`
- 可选：`uint32_t* d_in_queue_epoch; int cur_epoch;`（用戳记做 `(c,x)` 粒度去重）

容量建议（Jetson）：
- `d_frontier` 以“约束数 × 1.5”预分配；仅存约束 id 的版本内存最省；如需 `(c,x)` 粒度再放大 2–3 倍。

## 4. 调度与状态机语义
- 入队 `schedule(c)`（在 CsCheck 内被邻接触发调用）：
  - `old = atomicCAS(&d_state[c], 0, 1)` → old==0：首次入队，写 `d_frontier[d_q_tail++] = c`
  - `old==1`（已在队列）：忽略
  - `old==2`（处理中）：`atomicExch(&d_pending[c], 1)` 折叠为一次重跑
- 处理 `process(c)`：
  - `if (atomicCAS(&d_state[c], 1, 2) != 1) return;` // 只有队列弹出的 c 能进入处理
  - do { 执行 CsCheck(c)；在删值时对邻接约束调用 `schedule(nc)`；`again = atomicExch(&d_pending[c], 0);` } while (again)
  - `__threadfence(); atomicExch(&d_state[c], 0);` // 释放，确保写回可见
- 退出条件：
  - 非 barrier 版：当 `q_head>=q_tail && d_active_workers==0` 时允许退出（可加短暂重试避免竞态）
  - barrier 版：`cooperative_groups::this_grid().sync()` 作为轮次边界（需 cooperative launch）

正确性要点：
- 传播是幂等的；处理中再次触发折叠为一次 `pending` 不丢传播。
- bitDom 的删位为“只清零不置一”，并发修改需使用 `atomicAnd` 或者 warp 合并后一次 `atomicAnd` 写回，避免丢更新。

## 5. 持久化 kernel 结构（伪代码）
```
__global__ void GACPersistent(...) {
  // warp 映射与网格配置
  int lane = threadIdx.x & 31;
  int warp = (blockIdx.x * blockDim.x + threadIdx.x) >> 5;
  int nwarps = (gridDim.x * blockDim.x) >> 5;

  while (__ldg(&d_gac_success)) {
    // 取任务（grid-stride，直到队列暂空）
    int my = -1;
    if (lane == 0) {
      int i = atomicAdd(&d_q_head, 1);
      my = (i < __ldg(&d_q_tail)) ? d_frontier[i] : -1;
      if (my != -1) atomicAdd(&d_active_workers, 1);
    }
    my = __shfl_sync(0xffffffff, my, 0);

    if (my == -1) {
      // 检查是否收敛
      if (lane == 0) {
        bool empty = (__ldg(&d_q_head) >= __ldg(&d_q_tail));
        bool idle  = (__ldg(&d_active_workers) == 0);
        if (empty && idle) break; // 收敛退出
      }
      continue; // 重新尝试取任务
    }

    // 进入处理状态
    if (lane == 0) {
      if (atomicCAS(&d_state[my], 1, 2) != 1) {
        atomicSub(&d_active_workers, 1);
        my = -1;
      }
    }
    my = __shfl_sync(0xffffffff, my, 0);
    if (my == -1) continue;

    // do { CsCheck(my); } while(pending)
    do {
      CsCheck_warp(my, lane, ... /* 使用你现有位运算与纹理加载 */);
    } while (__shfl_sync(0xffffffff, (lane==0)? atomicExch(&d_pending[my], 0):0, 0));

    if (lane == 0) {
      __threadfence();
      atomicExch(&d_state[my], 0);
      atomicSub(&d_active_workers, 1);
    }
  }
}
```

- `CsCheck_warp` 内部在删值时调用 `schedule(nc)`：warp 聚合后一次 `atomicAdd` 申请连续段写入 `d_frontier`，降低原子热点。
- 若采用 `(c,x)` 粒度事件，将 `d_frontier` 换为 `uint32_t`，`state/pending` 可仍按 `c` 粒度（同一约束仅一个工作者），`pending` 表示“该约束结束后需重跑”。

## 6. 线程映射与访存
- warp-per-constraint：一个 warp 处理一个约束；lane 处理 bitset 的不同 word（`chunk += 32`），对 `bitDom/bitSup` 使用 `uint4` 矢量加载。
- block 配置：`blockDim=256`（8 个 warp），`gridDim ≈ 2–4 × SM 数`；避免 `gridDim` 受队列长度限制。
- 共享内存：把参与变量的 `bitDom` 局部块搬到 shared；Ampere 上可用 `cp.async` 预取，重叠访存与位运算。
- 原子写：对同一 `(var, word)` 的删位用 warp 内合并后单次 `atomicAnd`；跨 warp 冲突较低但仍可能，必要时可引入局部位掩码缓冲。

## 7. 设备端播种与融合决策
- 初始播种：
  - 普通 `enforceGAC()`：把所有约束置为 `InQueue` 并写入队列（或按度/域长度筛一批起始集）。
  - 决策后 `enforceGAC(var,type)`：用 `d_subscription_offset/ d_subscription` 找到 `var` 的邻接约束，逐个 `schedule(c)` 播种。
- 启发式与赋值：
  - 阶段 A（推荐先做）：CPU 选 var/val，设备端持久化传播。设备端维护 `d_cur_dom_size/degree`，可用 CUB `ArgMin` 计算 dom/deg 并仅回读 4 字节变量号。
  - 阶段 B（进阶）：设备端维护决策栈与 trail，内核内完成选择、赋值、传播与回溯（复杂度高，建议后续推进）。

## 8. Jetson Orin 侧关键实践
- 编译：`-DCMAKE_CUDA_ARCHITECTURES=87`，用 `-lineinfo` 便于 Nsight；谨慎 `--use_fast_math`。
- UM/内存池：`__managed__` + `cudaMemAdvisePreferredLocation(GPU)` + 关键点 `cudaMemPrefetchAsync`；临时缓冲改用 `cudaMallocAsync` 复用。
- 减少 Host 往返：避免 `cudaDeviceSynchronize()`；用持久化 kernel 将收敛判断放在设备侧；仅在外层决策或失败/成功回写时与 Host 交互。
- 占用调参：控制共享内存与寄存器占用，保证每 SM 8–16 个活跃 warp；用 `cudaOccupancyMaxPotentialBlockSize` 评估。

## 9. 迁移步骤（低风险落地）
- P0：把 `compress_Main()` 改为设备端 compaction（CUB DeviceSelect/Partition），Host 只读回 `next_count`（4B）。
- P1：引入 `schedule()/state/pending`，仍沿用“多波次”但在设备端完成“构建下一波 + 去重”。
- P2：实现持久化 kernel（如上伪代码），合并 `CsCheckMain` 与 `CsCheckMainAfterDecision` 的入口播种逻辑。
- P3：加入 warp 聚合原子、`uint4` 矢量加载、`cp.async` 预取等微优化；引入设备端 dom/deg 选择（可选）。
- P4：A/B 与回归：与当前 `GPU_GAC_PIPELINE_PLAN.md` 的 Graph 流程做对比基准，保留 CMake 开关便于回退。

## 10. KPI 与观测
- 核心：每轮删值/事件比、events/ms、端到端 `enforceGAC()` 时长；
- 硬件：SM 占用、全局带宽、L2 命中、原子热点；
- 调度：平均队列长度、pending 命中率、连续重跑次数分布；
- Jetson 专项：Host API 时间占比、首次访问抖动（UM 迁移）与峰值功耗。

## 11. 风险与缓解
- 队列溢出：容量预估 + 背压（失败回退或分批入队）+ 统计报警；
- 原子热点：warp 聚合、分桶（按约束度/变量块）、必要时多队列分散热点；
- 公平性：同一约束 pending 连续次数超过阈值时，改为入队一次交给其他 warp；
- 调试：禁用设备 printf，使用设备端统计 + Host 采样；必要时只在单 CTA 下跑小实例定位问题。

## 12. 与 `aig_docs/GPU_GAC_PIPELINE_PLAN.md` 的对比（Jetson 视角）
- 相同点：都强调设备端去重/压缩、减少 Host 往返；都推荐 warp 级并行与对齐加载；均可支持“决策后播种”。
- 差异点：
  - 执行模型：
    - 本方案：单次持久化 kernel，设备端循环直到收敛；
    - PIPELINE 方案：以阶段/波次为单位，倾向于 CUDA Graph 捕获与回放，或后续再切 Persistent（更渐进）。
  - 并发控制：
    - 本方案：约束状态机（0/1/2 + pending）杜绝同一约束并发，处理中触发折叠为重跑；
    - PIPELINE 方案：以事件集合批处理，靠戳记/排序去重，允许同一约束在不同波次多次出现。
  - 启动/同步：
    - 本方案：无多次内核启动，收敛判断在设备侧；
    - PIPELINE 方案：Graph 虽降低启动成本，但仍以“多节点/多轮回放”为主，极小批量下仍有开销。
  - 复杂度：
    - 本方案：一次性把循环/调度搬上设备端，原子/边界条件更复杂；
    - PIPELINE 方案：更易逐步迁移，调试友好，性能上限略低。
- Jetson Orin 推荐：
  - 若优先追求“小批量高效 + 最少 Host 往返”，本方案更契合 Orin（SM 数少、kernel 启动代价更“显山露水”）。
  - 若优先追求“迭代落地与易回退”，可先按 PIPELINE 方案完成 CUB 压缩 + Graph 捕获，确认收益后再切换到本方案。

## 13. 与现有代码的衔接建议
- 内核：保留你当前 `CsCheckMain` 的位集算子与纹理访问，抽出 `CsCheck_warp(c)` 供持久化内核调用；
- 数据：复用 `d_subscription/_offset` 构建邻接；`d_ConPre/d_MCon` 不变；
- 入口：把 `enforceGAC()` 与 `enforceGAC(var,type)` 的播种逻辑改为填充 `d_frontier`，其余传播统一走持久化内核；
- 原子：对 `bitDom` 删位采用 warp 合并后 `atomicAnd`；`d_cur_dom_size` 用 `atomicSub` 或者在删位数量可统计时单次写回；
- 成功/失败：设备端写 `d_gac_success`，Host 端仅读一个 4B 标志与必要统计。

---

附：不建议的做法
- 设备端动态发射 kernel（细粒度 Overhead 大，Jetson 上收益更差）；
- 自旋锁/忙等（会浪费 warp/CTA、还可能死锁）；
- 内核内 `printf`（极大拖慢小 SoC）。

***
如需，我可以基于 `src/cuSAC.cu` 起草最小可用的 `GACPersistent` 骨架与 `schedule()` 辅助函数，保持现有数据结构与纹理绑定不变，便于你逐步替换。

