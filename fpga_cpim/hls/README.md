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

## Phase C.3 输出

`hls_tb` 会输出可脚本采样的 pressure rows：

```text
hls_pressure graph=random vars=128 domain=128 density=0.10 partitions=4 tiles=1 constraints=793 status=OK events=1586 epochs=1586 tile_steps=1586 queue_peak_total=592 queue_peak_partition=273 local_events=973 cross_events=613 deleted_values=16129 router_overflow=0
hls_pressure graph=random vars=128 domain=128 density=0.10 partitions=4 tiles=2 constraints=793 status=OK events=1586 epochs=793 tile_steps=1586 queue_peak_total=592 queue_peak_partition=273 local_events=973 cross_events=613 deleted_values=16129 router_overflow=0
hls_pressure graph=random vars=128 domain=128 density=0.10 partitions=4 tiles=4 constraints=793 status=OK events=1586 epochs=397 tile_steps=1586 queue_peak_total=592 queue_peak_partition=273 local_events=973 cross_events=613 deleted_values=16129 router_overflow=0
```

其中 `events` 是实际消费的 dirty events，`epochs` 是 tile 调度轮数，
`queue_peak_total` / `queue_peak_partition` 是 per-partition queue 峰值。
