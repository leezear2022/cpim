# HLS-friendly Core

这里放第一版可综合风格 C++ 原型。普通本地验证使用：

```bash
g++ -std=c++17 -I fpga_cpim/hls fpga_cpim/hls/testbench_hls.cpp fpga_cpim/hls/*.cpp -o hls_tb
./hls_tb
```

约束：

- core 文件不使用 STL 容器。
- 固定最大参数。
- 静态数组。
- 溢出或预算命中返回 `UNKNOWN`。
- Phase C.2 支持多 revise tile 调度和 per-partition event queue。

## Phase C.2 接口

`cpim_top_hls` 额外接收：

- `var_partition[MAX_VARS]`
- `constraint_partition[MAX_CONSTRAINTS]`

`ControlHls` 额外控制：

- `num_partitions`
- `num_revise_tiles`
- `partition_queue_capacity`

`ResultHls` 额外输出：

- `epochs`
- `tile_steps`
- `local_events`
- `cross_events`
- `queue_peak_total`
- `queue_peak_partition`
- `router_overflow`

testbench 覆盖：

- equality OK。
- less-than singleton DWO。
- budget UNKNOWN。
- per-partition queue overflow UNKNOWN。
- 多 revise tile round-robin 调度。
- `vars=128/domain=128/density≈0.10` pressure smoke。
