# FPGA CPIM Phase C.3 HLS Tile Sweep 输出

## 目标

Phase C.3 将 HLS-friendly pressure smoke 变成稳定可采样输出：

- 固定 `random/vars=128/domain=128/density≈0.10`。
- 对比 `num_revise_tiles=1/2/4`。
- 输出 `events/epochs/queue_peak/cross_events` 等一行式指标。
- 保持 `UNKNOWN` 语义和 Phase C.2 的 per-partition queue 不变。

本轮仍不接 Vitis、不做 RTL timing，只补本地 `g++` testbench 对照证据。

## 本轮实现

- `testbench_hls.cpp` 将 density≈0.10 pressure case 抽成可重复 fixture。
- 同一个 fixture 分别运行 `1/2/4` revise tile。
- 每个 tile 配置输出一行稳定文本，前缀固定为 `hls_pressure`。
- 断言三种 tile 数下：
  - status 均为 `OK`。
  - constraints 数一致。
  - events 与 deleted values 一致。
  - epochs 随 tile 数增加不升高。

## 验收命令

```bash
g++ -std=c++17 -I fpga_cpim/hls \
  fpga_cpim/hls/testbench_hls.cpp fpga_cpim/hls/*.cpp \
  -o /tmp/hls_tb_c3 && /tmp/hls_tb_c3
```

输出：

```text
hls_pressure graph=random vars=128 domain=128 density=0.10 partitions=4 tiles=1 constraints=793 status=OK events=1586 epochs=1586 tile_steps=1586 queue_peak_total=592 queue_peak_partition=273 local_events=973 cross_events=613 deleted_values=16129 router_overflow=0
hls_pressure graph=random vars=128 domain=128 density=0.10 partitions=4 tiles=2 constraints=793 status=OK events=1586 epochs=793 tile_steps=1586 queue_peak_total=592 queue_peak_partition=273 local_events=973 cross_events=613 deleted_values=16129 router_overflow=0
hls_pressure graph=random vars=128 domain=128 density=0.10 partitions=4 tiles=4 constraints=793 status=OK events=1586 epochs=397 tile_steps=1586 queue_peak_total=592 queue_peak_partition=273 local_events=973 cross_events=613 deleted_values=16129 router_overflow=0
hls_tb ok
```

完整回归：

```bash
cmake --build build/fpga_cpim -j
ctest --test-dir build/fpga_cpim --output-on-failure
```

结果：12/12 通过。

## 判断

- 当前 fixture 下 tile 数增加不会改变 propagation work 数量：
  `events=1586`、`deleted_values=16129` 均一致。
- epoch 近似随 tile 数线性下降：
  - 1 tile: `1586`
  - 2 tiles: `793`
  - 4 tiles: `397`
- queue pressure 由 fixture 决定，三种 tile 数一致：
  `queue_peak_total=592`、`queue_peak_partition=273`。
- cross/local event 统计稳定：
  `cross_events=613`、`local_events=973`。

## 后续

Phase C.4 已完成，见
[FPGA_CPIM_PHASE_C4_HLS_CLI_TRACE.md](FPGA_CPIM_PHASE_C4_HLS_CLI_TRACE.md)：

- 给 HLS testbench 增加 `--tiles=1,2,4` / `--pressure-only` 轻量参数解析，
  便于 CI 只跑 pressure rows。
- 把 `hls_pressure` 行解析成 JSONL 或 CSV。
- 增加 queue capacity sweep，例如 `64/128/256/1024`，观察 overflow
  进入 `UNKNOWN` 的门槛。
