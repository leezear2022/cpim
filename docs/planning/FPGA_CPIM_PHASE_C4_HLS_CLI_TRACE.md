# FPGA CPIM Phase C.4 HLS CLI 与 Trace 转换

## 目标

Phase C.4 将 HLS-friendly testbench 的 pressure smoke 变成更适合 CI 和脚本采样的形式：

- 支持 `--pressure-only`。
- 支持 `--tiles=1,2,4`。
- 支持 `--capacity-sweep=64,128,256,512,1024|none`。
- 将 `hls_pressure` / `hls_capacity` 行转换为 JSONL 或 CSV。
- 扫 per-partition queue capacity 的 `UNKNOWN` 门槛。

本轮仍不接 Vitis、不写 RTL、不改软件模拟器主线。

## 本轮实现

- `testbench_hls.cpp` 新增轻量参数解析：
  - `--pressure-only`
  - `--tiles=<csv>`
  - `--capacity-sweep=<csv>|none`
  - `--help`
- `hls_pressure` 行新增 `capacity=<n>` 字段。
- 新增 `hls_capacity` 行，用同一个 pressure fixture 扫 queue capacity。
- 新增 `fpga_cpim/scripts/parse_hls_trace.py`：
  - `--format jsonl|csv`
  - `--kind all|pressure|capacity`
  - `--input` / `--output`

## 验收命令

```bash
g++ -std=c++17 -I fpga_cpim/hls \
  fpga_cpim/hls/testbench_hls.cpp fpga_cpim/hls/*.cpp \
  -o /tmp/hls_tb_c4

/tmp/hls_tb_c4 \
  --pressure-only \
  --tiles=1,2,4 \
  --capacity-sweep=64,128,256,512,1024 \
  > /tmp/hls_c4.out
```

输出：

```text
hls_pressure graph=random vars=128 domain=128 density=0.10 partitions=4 tiles=1 capacity=1024 constraints=793 status=OK events=1586 epochs=1586 tile_steps=1586 queue_peak_total=592 queue_peak_partition=273 local_events=973 cross_events=613 deleted_values=16129 router_overflow=0
hls_pressure graph=random vars=128 domain=128 density=0.10 partitions=4 tiles=2 capacity=1024 constraints=793 status=OK events=1586 epochs=793 tile_steps=1586 queue_peak_total=592 queue_peak_partition=273 local_events=973 cross_events=613 deleted_values=16129 router_overflow=0
hls_pressure graph=random vars=128 domain=128 density=0.10 partitions=4 tiles=4 capacity=1024 constraints=793 status=OK events=1586 epochs=397 tile_steps=1586 queue_peak_total=592 queue_peak_partition=273 local_events=973 cross_events=613 deleted_values=16129 router_overflow=0
hls_capacity graph=random vars=128 domain=128 density=0.10 partitions=4 tiles=4 capacity=64 constraints=793 status=UNKNOWN events=14 epochs=4 tile_steps=14 queue_peak_total=178 queue_peak_partition=64 local_events=113 cross_events=80 deleted_values=1778 router_overflow=1
hls_capacity graph=random vars=128 domain=128 density=0.10 partitions=4 tiles=4 capacity=128 constraints=793 status=UNKNOWN events=166 epochs=42 tile_steps=166 queue_peak_total=359 queue_peak_partition=128 local_events=311 cross_events=215 deleted_values=5207 router_overflow=1
hls_capacity graph=random vars=128 domain=128 density=0.10 partitions=4 tiles=4 capacity=256 constraints=793 status=UNKNOWN events=1055 epochs=264 tile_steps=1055 queue_peak_total=592 queue_peak_partition=256 local_events=944 cross_events=610 deleted_values=15875 router_overflow=1
hls_capacity graph=random vars=128 domain=128 density=0.10 partitions=4 tiles=4 capacity=512 constraints=793 status=OK events=1586 epochs=397 tile_steps=1586 queue_peak_total=592 queue_peak_partition=273 local_events=973 cross_events=613 deleted_values=16129 router_overflow=0
hls_capacity graph=random vars=128 domain=128 density=0.10 partitions=4 tiles=4 capacity=1024 constraints=793 status=OK events=1586 epochs=397 tile_steps=1586 queue_peak_total=592 queue_peak_partition=273 local_events=973 cross_events=613 deleted_values=16129 router_overflow=0
```

转换：

```bash
fpga_cpim/scripts/parse_hls_trace.py \
  --input /tmp/hls_c4.out \
  --format jsonl \
  --output /tmp/hls_c4.jsonl

fpga_cpim/scripts/parse_hls_trace.py \
  --input /tmp/hls_c4.out \
  --format csv \
  --kind capacity \
  --output /tmp/hls_c4_capacity.csv
```

完整回归：

```bash
cmake --build build/fpga_cpim -j
ctest --test-dir build/fpga_cpim --output-on-failure
```

结果：12/12 通过。

## 判断

- `tiles=1/2/4` 仍保持 work 一致：
  `events=1586`、`deleted_values=16129`。
- epoch 随 tile 数下降：
  `1586 -> 793 -> 397`。
- capacity 门槛在本 fixture 下落在 `256` 与 `512` 之间：
  - `64/128/256`: `UNKNOWN`，`router_overflow=1`
  - `512/1024`: `OK`，`router_overflow=0`
- `UNKNOWN` case 仍不对外宣称可删除；这里只是 testbench 内部统计已做过的局部工作。

## 下一步

建议进入 Phase C.5：

- 将 `parse_hls_trace.py` 接入一个小 sweep 脚本，自动编译/运行/产出 JSONL。
- 增加 `capacity=272/273/274/320/384` 的细粒度 sweep，定位刚好不 overflow 的容量。
- 后续如果准备接 Vitis，再拆分 host-only testbench helper 与 synthesis-facing core。
