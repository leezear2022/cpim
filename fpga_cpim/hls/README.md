# HLS-friendly Core

这里放第一版可综合风格 C++ 原型。普通本地验证使用：

```bash
g++ -std=c++17 -I fpga_cpim/hls fpga_cpim/hls/testbench_hls.cpp fpga_cpim/hls/*.cpp -o hls_tb
./hls_tb
```

只输出 pressure / capacity rows：

```bash
./hls_tb \
  --pressure-only \
  --tiles=1,2,4 \
  --capacity-sweep=64,128,256,512,1024 \
  --fixtures=chain,random,hub
```

约束：

- core 文件不使用 STL 容器。
- 固定最大参数。
- HLS 数组大小由编译时 profile 固定，不靠运行时参数改变资源规模。
- 静态数组。
- 溢出或预算命中返回 `UNKNOWN`。
- Phase C.2 支持多 revise tile 调度和 per-partition event queue。

## Compile-time Profiles

`cpim_hls_types.hpp` 支持三个编译时 profile：

```text
stress128:
  MAX_VARS=256
  MAX_CONSTRAINTS=1024
  MAX_DOMAIN=128
  MAX_WORDS=4
  MAX_WORLDS=4
  MAX_PARTITION_QUEUE=1024
  MAX_REVISE_TILES=4

z7020_small:
  MAX_VARS=128
  MAX_CONSTRAINTS=512
  MAX_DOMAIN=32
  MAX_WORDS=1
  MAX_WORLDS=1
  MAX_PARTITION_QUEUE=512
  MAX_REVISE_TILES=1

z7020_probe2:
  MAX_VARS=128
  MAX_CONSTRAINTS=256
  MAX_DOMAIN=32
  MAX_WORDS=1
  MAX_WORLDS=2
  MAX_PARTITION_QUEUE=512
  MAX_REVISE_TILES=1
```

本地编译示例：

```bash
g++ -std=c++17 -I fpga_cpim/hls \
  -DFPGA_CPIM_HLS_PROFILE=FPGA_CPIM_HLS_PROFILE_Z7020_SMALL \
  fpga_cpim/hls/testbench_hls.cpp fpga_cpim/hls/*.cpp \
  -o hls_tb_z7020_small
./hls_tb_z7020_small --profile=z7020_small --tiles=1 \
  --capacity-sweep=512 --fixtures=chain,random,hub
```

CMake 回归会同时构建并运行：

- `hls_tb`：默认 `stress128`。
- `hls_tb_z7020_small`。
- `hls_tb_z7020_probe2`。

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
- `chain/random/hub` 最小 fixture pressure/capacity sweep。

## Phase C.3 输出

`hls_tb` 会输出可脚本采样的 pressure rows：

```text
hls_pressure graph=random vars=128 domain=128 density=0.10 partitions=4 tiles=1 capacity=1024 constraints=793 status=OK events=1586 epochs=1586 tile_steps=1586 queue_peak_total=592 queue_peak_partition=273 local_events=973 cross_events=613 deleted_values=16129 router_overflow=0
hls_pressure graph=random vars=128 domain=128 density=0.10 partitions=4 tiles=2 capacity=1024 constraints=793 status=OK events=1586 epochs=793 tile_steps=1586 queue_peak_total=592 queue_peak_partition=273 local_events=973 cross_events=613 deleted_values=16129 router_overflow=0
hls_pressure graph=random vars=128 domain=128 density=0.10 partitions=4 tiles=4 capacity=1024 constraints=793 status=OK events=1586 epochs=397 tile_steps=1586 queue_peak_total=592 queue_peak_partition=273 local_events=973 cross_events=613 deleted_values=16129 router_overflow=0
```

其中 `events` 是实际消费的 dirty events，`epochs` 是 tile 调度轮数，
`queue_peak_total` / `queue_peak_partition` 是 per-partition queue 峰值。

转换为 JSONL / CSV：

```bash
./hls_tb --pressure-only > /tmp/hls.out
fpga_cpim/scripts/parse_hls_trace.py --input /tmp/hls.out --format jsonl
fpga_cpim/scripts/parse_hls_trace.py --input /tmp/hls.out --format csv --kind capacity
```

自动编译 / 运行 / JSONL：

```bash
fpga_cpim/scripts/run_hls_trace_sweep.py \
  --capacity-sweep=272,273,274,320,384
```

指定 7Z020 小 profile：

```bash
fpga_cpim/scripts/run_hls_trace_sweep.py \
  --profile=z7020_small \
  --tiles=1 \
  --capacity-sweep=256,384,512
```

当前 `random/vars=128/domain=128/density=0.10` fixture 下，
per-partition queue capacity 门槛为 `273`：`272` overflow 返回
`UNKNOWN`，`273` 起恢复 `OK`。

JSONL sizing 字段会随每条 row 输出：

```json
{
  "queue_peak_partition": 273,
  "semantic_min_capacity": 273,
  "recommended_depth_1p25": 342,
  "recommended_depth_pow2": 512,
  "chosen_depth": 512,
  "chosen_depth_overhead": 1.875
}
```

## Vitis HLS Report Loop

有 Vitis 环境时：

```bash
vitis_hls -f fpga_cpim/hls/run_hls.tcl
python3 fpga_cpim/hls/parse_hls_reports.py \
  --project fpga_cpim_hls \
  --solution z7020_small \
  --output build/fpga_cpim/hls_report_summary.json
```

`run_hls.tcl` 默认跑 `z7020_small` / `xc7z020clg400-1`，执行
`csim_design` 和 `csynth_design`。设置 `RUN_COSIM=1` 时追加
`cosim_design`。

切换 profile：

```bash
PROFILE=z7020_probe2 vitis_hls -f fpga_cpim/hls/run_hls.tcl
PROFILE=stress128 vitis_hls -f fpga_cpim/hls/run_hls.tcl
```

无 Vitis 环境时，parser 仍输出稳定 JSON，并把 `tool_status` /
`csynth` 标记为 `tool_missing`，不阻塞 C++ simulator 和本地 `g++`
testbench。

工具栈层次：

- Level 0：`fpga_cpim_sim` 语义模拟。
- Level 1：本地 `g++ hls_tb`。
- Level 2：Vitis HLS `csim/csynth`。
- Level 3：Vitis HLS `cosim`、dataflow/deadlock 检查。
- Level 4a：Verilator + C++ wrapper，快速 RTL regression。
- Level 4b：cocotb + Verilator/xsim，随机 backpressure / ready-valid /
  AXI-like stream 测试。
- Level 5：Vivado xsim，Xilinx IP / block design / post-synth。
- Level 6：7Z020 板上 ARM + PL。
