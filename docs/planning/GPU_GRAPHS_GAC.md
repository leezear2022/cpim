# 使用 CUDA Graph / Persistent Kernel 优化 enforceGAC（Jetson 友好）

本文分解将 GAC 传播循环 Graph 化/常驻化的几种可落地做法，并给出实现骨架与取舍建议。

## 目标
- 降低 Host↔Device 往返与 kernel 启动开销，提升 Orin 等 SoC 场景吞吐。
- 保持语义不变：传播依事件队列迭代直至稳定或失败（与当前 enforceGAC 等价）。

## 现状（基线）
- Host 端循环：`compress → CsCheck → compress …`，每轮 1+ 个 kernel + `cudaDeviceSynchronize()`，有明显启动与同步开销。
- 停止条件：设备端统计（事件数=0 / `GAC_success=false`）。

## 方案 A：Graph‑replay（流捕获）
- 思路：对“单轮传播体”做流捕获，实例化为图后在 Host 端按标志回放（replay）。
- 固定网格：为 CsCheck 设定固定上限网格，线程内部 if/stride 处理 `tid < d_num_ConEvt`，避免每轮更新节点参数。
- 设备端停止标志：`__managed__`/UM 变量 `d_num_ConEvt`、`GAC_success`；或额外 kernel 写入布尔 `d_should_continue`。
- 伪代码：
```cpp
cudaStreamBeginCapture(s, cudaStreamCaptureModeGlobal);
  compressKernel<<<G1,B1>>>(..., d_num_ConEvt);
  csCheckKernel<<<Gmax,B2>>>(..., d_num_ConEvt /*if (tid < n)*/);
  reduceFlagKernel<<<1,W>>>(..., d_num_ConEvt, GAC_success, d_should_continue);
cudaStreamEndCapture(s, &graph);
cudaGraphInstantiate(&exec, graph, nullptr, nullptr, 0);

do {
  cudaGraphLaunch(exec, s);
  cudaStreamSynchronize(s); // 或事件+UM 轮询
} while (*d_should_continue);
```
- 优点：改动小、收益立竿见影；缺点：仍有 Host 轮询控制（但已无频繁内核设置）。

## 方案 B：Graph + 参数更新 / HostNode（可变网格）
- 回读/读取 `d_num_ConEvt`，在图回放前用 `cudaGraphExecKernelNodeSetParams` 更新 CsCheck 的 grid/block。
- 也可通过 HostNode 在图中插入“读数→更新节点”步骤。
- 优点：工作量匹配更紧；缺点：Host 节点引入 CPU 介入，复杂度更高，Jetson 上收益未必显著。

## 方案 C：Persistent Kernel（常驻内核）
- 将 `while(num_ConEvt>0 && GAC_success)` 放入常驻内核，使用 cooperative groups / grid.sync 进行轮次同步：
```cpp
__global__ void gac_persistent(State s) {
  grid_group g = this_grid();
  do {
    compress_device(s);  // 设备侧构建事件
    g.sync();
    cscheck_device(s);   // 固定网格，按 stride 消费事件
    g.sync();
  } while (s.num_ConEvt > 0 && s.GAC_success);
}
```
- Host 仅一次 `cudaLaunchCooperativeKernel`；优点：彻底消除 Host 控制；缺点：需要协作式 launch，需谨慎占用调参。

## 方案 D：条件/循环图节点（CUDA 12+）
- 使用条件/循环节点在图内表达“继续/退出”逻辑，可进一步减少 Host 介入。
- 风险：API 相对新，需验证 SoC 驱动/工具链支持与稳定性。

## Jetson 工程化要点
- 统一内存（UM）：`__managed__` + `cudaMemAdviseSetPreferredLocation(GPU)` + `cudaMemPrefetchAsync`；避免首次访问迁移抖动。
- 事件与依赖：使用 `cudaEvent` 计时与同步，移除 `cudaDeviceSynchronize()`。
- 固定网格 + 线程内边界：避免频繁 graph param update；或仅在阈值变化大时更新。
- 去除内核 `printf`：改为设备侧统计计数写回，必要时按周期回传。

## 迁移路径建议
1. Phase A：Graph‑replay 骨架落地（固定网格 + UM 标志位），对比基线收益。
2. Phase B：试验 Persistent Kernel；若收益明显、实现稳定，保留宏开关切换。
3. Phase C：评估条件/循环节点；在 Jetson 驱动条件允许下引入以进一步收敛 Host 参与。

## 验证指标（KPI）
- 单轮传播耗时、迭代次数、总求解时间；SM 占用、全局带宽、L2 命中；Host API 时间占比（Nsight Systems/Compute）。

