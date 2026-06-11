# FPGA CPIM Phase C.2 HLS 多 Tile 与 Partition Queue

## 目标

Phase C.2 把 Phase B 的 partition/queue 观察落到 HLS-friendly core：

- 保持固定数组、无 STL 容器、无动态内存。
- 新增 per-partition event queue。
- 新增多 revise tile round-robin 调度。
- 用 `random/vars=128/domain=128/density≈0.10` 做压力 smoke。
- 预算超限或 queue overflow 仍只返回 `UNKNOWN`。

本轮不接真实 Vitis、不写 RTL、不改变软件模拟器传播语义。

## 本轮实现

- `ControlHls` 新增：
  - `num_partitions`
  - `num_revise_tiles`
  - `partition_queue_capacity`
- `ResultHls` 新增：
  - `epochs`
  - `tile_steps`
  - `local_events`
  - `cross_events`
  - `queue_peak_total`
  - `queue_peak_partition`
  - `router_overflow`
- `cpim_top_hls` 新增输入：
  - `var_partition[MAX_VARS]`
  - `constraint_partition[MAX_CONSTRAINTS]`
- `event_router_hls.cpp` 新增 per-partition 环形队列 enqueue/dequeue。
- `testbench_hls.cpp` 新增：
  - 多 revise tile partition smoke。
  - per-partition queue overflow UNKNOWN。
  - `vars=128/domain=128/density≈0.10` pressure smoke。

## 验收命令

```bash
cmake --build build/fpga_cpim -j
ctest --test-dir build/fpga_cpim --output-on-failure
```

结果：12/12 通过。

```bash
g++ -std=c++17 -I fpga_cpim/hls \
  fpga_cpim/hls/testbench_hls.cpp fpga_cpim/hls/*.cpp \
  -o /tmp/hls_tb_c2 && /tmp/hls_tb_c2
```

结果：

```text
hls_tb ok
```

## 判断

- HLS core 已从单队列推进到 partition-aware queue fabric。
- `num_revise_tiles=2/4` 已能在本地 testbench 中消费多个 partition queue。
- per-partition queue overflow 与 event/revise/epoch budget 一样返回 `UNKNOWN`。
- `density≈0.10` pressure smoke 能覆盖 128 变量、128 domain、约 10% random graph 的 bitSup 和队列压力。
- 当前仍是调度语义 smoke，不是周期精确 RTL timing。

## 下一步

建议进入 Phase C.3：

- 给 HLS core 增加可导出的 `TraceLite` 结构，记录每个 world 的 queue peak、epochs、events、cross/local event。
- 增加单 tile vs 2 tile vs 4 tile 的 HLS testbench 对照。
- 将 `density≈0.10` pressure smoke 的结果打印为稳定一行，便于后续 CI/脚本采样。
- 若后续接 Vitis，再把 fallback integer 类型替换为真实 `ap_uint` 并加 synthesis-only pragma。
