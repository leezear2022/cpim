# FPGA CPIM Phase B 容量与路由统计

## 背景

Phase A+C 已经落地独立 `fpga_cpim/` 原型：C++17 软件模拟器、Golden 对照、NSACQ 半径 1 过滤、保守 `UNKNOWN` 语义，以及 HLS-friendly 最小 testbench。

Phase B 的目标不是改变传播语义，而是补齐 FPGA 方向早期必须看的硬件形态指标：

- `bitSup` footprint。
- router queue occupancy p50/p95/max。
- subscription fanout p50/p95/max。
- greedy variable partition。
- local/cross partition event ratio。
- high-degree hub 检测。
- BRAM18 / URAM288 粗估。

## 本轮实现

- 新增 `PartitionConfig` / `PartitionStats` 和 `BuildGreedyPartition`。
- 变量按 degree 降序 greedily 分到 `num_partitions` 个 partition。
- 约束分配到端点 partition 中当前约束负载更低的一侧；跨 partition event 以 endpoint event 是否需要路由到 constraint owner 估算。
- `PropagationEngine` 将 `EventRouter::Stats()` 写回 `WorldResult`。
- CLI JSON 输出新增：
  - `telemetry.queue_occupancy_p50/p95/max`
  - `telemetry.router_events_enqueued/deduped/dropped_overflow`
  - `telemetry.partition_cross_event_ratio`
  - `partition.*`
- 新增 `fpga_cpim/scripts/run_phase_b_sweep.py` 覆盖 `chain/grid/random/hub`。

## 本地证据

命令：

```bash
cmake --build build/fpga_cpim -j
ctest --test-dir build/fpga_cpim --output-on-failure
fpga_cpim/scripts/run_phase_b_sweep.py \
  --binary build/fpga_cpim/fpga_cpim_sim \
  --seeds 3 \
  --vars 32 \
  --domain 32 \
  --density 0.2 \
  --tightness 0.5 \
  --jsonl build/fpga_cpim/phase_b_sweep.jsonl
```

结果：

```text
graph  constraints  bitSupKiB  queueP95  queueMax  crossRatio  hubs  bram18  uram288  unknownRate
chain  31.0         7.8        2.0       2         0.500       0.0   33.0    1.0      0.000
grid   52.0         13.0       4.0       4         0.462       0.0   35.0    1.0      0.000
random 95.7         23.9       20.0      27        0.385       0.0   39.7    1.0      0.000
hub    31.0         7.8        1.0       31        0.500       1.0   33.0    1.0      0.000
```

`ctest`：12/12 通过。

`dol lint --soft`：通过。

## 初步判断

- Phase B 没有发现 soundness 风险：高预算 synthetic sweep 的 `unknownRate` 为 0。
- `hub` 图的 `queueMax=31` 暴露中心变量 seed 的瞬时队列峰值，但 `queueP95=1`，压力高度集中。
- `random` 图的 `queueP95=20`、`queueMax=27`，更接近持续性 routing pressure，是下一轮 latency/bank-conflict 模拟更值得优先压测的形态。
- 当前 `bitSup` footprint 在 32x32 小规模 synthetic 上非常小，BRAM/URAM 粗估只用于验证字段链路；后续要扩到 `vars=128/domain=64/128` 才能给是否上板一个像样判断。

## 下一步建议

进入 Phase B.2，而不是直接写 RTL：

- 为 `SupportOracle` 增加 bank id、latency 和 bank conflict 统计。
- 将 queue stats 从全局 deque 进一步拆成 per-partition queue 估算。
- 对 `vars=128/domain=64` 和 `domain=128` 跑 chain/grid/random/hub sweep。
- 加 `--partition-policy`，至少比较 degree-greedy 与 contiguous partition。
- 若 large-domain sweep 的 `unknownRate` 仍为 0 且 queue/bank 指标可控，再推进 HLS dataflow 的多 tile/多 owner 版本。
