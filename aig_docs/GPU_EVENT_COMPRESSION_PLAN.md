# GAC 事件压缩（Event Compression）优化方案（Jetson / CUDA 12）

目标：降低每轮传播前“待检查约束”队列构建成本，减少 Host↔Device 往返，提升整体吞吐与稳定性。

## 角色与现状
- 事件压缩从“最近删值/赋值”的变量集合，推导出受影响的约束列表 `E = {(c, x)}`。
- 常见路径：扫描订阅（var→constraints）→ 去重（同一约束可能由多个变量触发）→ 生成紧凑队列（供 CsCheck 消费）。

## 数据结构前提（与 MODEL_DATA_PLAN 对齐）
- 订阅 CSR：`var_offsets[N+1]` + `var2cons[nnz]`（`int3` 可编码 `(c, v1, v2)` 等扩展信息）。
- 变更标记：`changed_vars[level][N]` 或“被删值位图”。
- 事件队列：设备侧环形缓冲区（size = 上限，或双缓冲），原子 head/tail 管理。
- 去重：`cons_seen[c]` 位/戳（stamp）避免重复入队。

## 方案 A：纯设备端压缩（推荐）
- 核 `build_events<<<G,B>>>`：
  - 输入：`changed_vars`/`changed_flags` 与 CSR。
  - 步骤：
    1) 每个线程处理一个变量或一段订阅区间；遍历 `var2cons` 推入 `(c, x)`。
    2) 使用 `cons_seen[c]` 的“时间戳”去重：`if (seen[c] != cur_stamp) seen[c]=cur_stamp; enqueue(c)`。
    3) 批量入队：warp 级 `__ballot_sync` + 共享内存缓存，降低 `atomicAdd` 频次。
  - 输出：`events[numEvents]`，`numEvents` 保存在 `__managed__` 计数器。
- 优点：Host 不介入，适配 persistent/graphs；并行扩展性好。
- 细节：
  - 将 `(c, x)` 压缩为 32b/64b key（如 `c<<16|x`），后续 CsCheck 内核解码。
  - 可按约束度/变量度排序 scope，先处理“便宜边”。

## 方案 B：Thrust/CUB 选择-压缩
- 两阶段：
  1) 生成标志数组：`flags[c] = any(changed var in scope(c))`（可在设备侧按 CSR 反向索引或预生成 `cons_offsets`）。
  2) `cub::DeviceSelect::Flagged`/`thrust::copy_if` 生成约束 ID 队列。
- 优点：实现快、鲁棒；缺点：需要反向 CSR（cons→vars）或额外标志构建开销。

## 方案 C：Host 侧轻量压缩（阈值触发）
- 当 `changed_vars` 很小（如单变量赋值）时，Host 按 CSR 直接构建队列，写入 UM/页锁定内存，设备读取处理。
- 触发：`changed_count < T_small`；否者走方案 A/B。
- 优点：极小工作量时更低延迟；缺点：引入 Host 参与，需谨慎切换与一致性。

## 去重与稳定性
- 建议“戳记去重”：`seen[c] != cur_stamp` 才入队；`cur_stamp` 每轮自增，避免清空数组。
- 如需 `(c,x)` 粒度，使用 `(c<<16)|x` 的位图/戳记（存储成本更高）。

## 与执行模型配合
- 固定网格 + 线程内 `if (tid<numSubs)`：避免每轮改 grid 配置，便于 **CUDA Graph**。
- **Persistent Kernel**：在设备端维护 `events` 队列，循环 `build_events → cscheck → …`，以 `grid.sync()` 协同。

## Jetson 细节
- 统一内存 + 预取：`events`、`changed`、`numEvents` 采用 `__managed__`，迭代开始前 `cudaMemPrefetchAsync` 至 GPU。
- 减少 atomics：warp 批量化入队；分片（分区）事件队列降低全局热点；必要时使用 per-block 局部队列 + 合并。
- 统计：事件数、去重命中、单轮构建时长写回，便于 Nsight 诊断。

## 伪代码示例（设备端）
```cpp
__global__ void build_events(const int* var_offsets,
                             const int* var2cons,
                             const int* changed_vars,
                             int*       events,
                             int*       num_events,
                             int*       seen, int cur_stamp) {
  int v = blockIdx.x * blockDim.x + threadIdx.x;
  if (!changed_vars[v]) return;
  int start = var_offsets[v], end = var_offsets[v+1];
  unsigned mask = 0, ballot = 0; int buf[WARP_SIZE]; int k = 0;
  for (int i=start; i<end; ++i) {
    int c = var2cons[i];
    if (atomicCAS(&seen[c], cur_stamp-1, cur_stamp) != cur_stamp) {
      buf[k++] = (c<<16) | v; // pack
      if (k == WARP_SIZE) { // 批量入队
        int base = atomicAdd(num_events, k);
        for (int t=0;t<k;++t) events[base+t] = buf[t];
        k = 0;
      }
    }
  }
  if (k) {
    int base = atomicAdd(num_events, k);
    for (int t=0;t<k;++t) events[base+t] = buf[t];
  }
}
```

## 迁移步骤
1) 设备端 CSR 订阅 + `seen` 戳记引入；2) 事件环形队列（或双缓冲）与统计；3) Graph 捕获或 Persistent；4) 小工作量阈值下 Host 快路径（可选）。

