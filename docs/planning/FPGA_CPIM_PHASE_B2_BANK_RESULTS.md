# FPGA CPIM Phase B.2 Bank/Latency 统计

## 目标

Phase B.2 在不改变传播语义的前提下，为 `SupportOracle` 增加 bitSup 访问的硬件压力估算：

- bank id：`flat_bit_sup_word_index % support_banks`。
- query-level bank conflict：一次 support query 访问的 word 数减去实际触达 bank 数。
- latency cycles：`words_touched * base_latency + bank_conflicts * conflict_penalty`。
- max bank accesses：单个 revise 输出中最热 bank 的累计访问峰值。

这些指标是估算用，不参与 DWO/UNKNOWN 判定。

## 本轮实现

- 新增 CLI 参数：
  - `--support-banks`
  - `--support-base-latency`
  - `--support-conflict-penalty`
- JSON `config` 增加 support bank 参数。
- JSON `telemetry` 增加：
  - `support_latency_cycles`
  - `support_bank_conflicts`
  - `support_max_bank_accesses`
- `run_phase_b_sweep.py` 支持 `--domains 64,128` 和 support bank 参数。
- `test_support_oracle` 增加 2-bank / 3-word 可控 conflict case。

## 本地证据

命令：

```bash
cmake --build build/fpga_cpim -j
ctest --test-dir build/fpga_cpim --output-on-failure
fpga_cpim/scripts/run_phase_b_sweep.py \
  --binary build/fpga_cpim/fpga_cpim_sim \
  --graphs chain,grid,random,hub \
  --seeds 1 \
  --vars 128 \
  --domains 64,128 \
  --density 0.05 \
  --tightness 0.5 \
  --support-banks 2 \
  --support-base-latency 1 \
  --support-conflict-penalty 3 \
  --jsonl build/fpga_cpim/phase_b2_128_sweep.jsonl
```

结果：

```text
graph  domain  constraints  bitSupKiB  queueP95  queueMax  latencyM  bankConflicts  maxBankAccess  crossRatio  hubs  bram18  uram288  unknownRate
chain  64      127.0        127.0      2.0       2         1.84      0.0            64             0.500       0.0   87.0    4.0      0.000
chain  128     127.0        508.0      2.0       2         30.72     5721523.0      256            0.500       0.0   258.0   15.0     0.000
grid   64      233.0        233.0      4.0       4         3.37      0.0            64             0.461       0.0   134.0   7.0      0.000
grid   128     233.0        932.0      4.0       4         56.38     10500780.0     256            0.461       0.0   447.0   26.0     0.000
random 64      404.0        404.0      13.0      19        6.42      0.0            64             0.391       2.0   210.0   12.0     0.000
random 128     404.0        1616.0     13.0      19        100.05    18204095.0     256            0.391       2.0   751.0   45.0     0.000
hub    64      127.0        127.0      1.0       127       1.84      0.0            64             0.500       1.0   87.0    4.0      0.000
hub    128     127.0        508.0      1.0       127       30.72     5721523.0      256            0.500       1.0   258.0   15.0     0.000
```

`dol lint --soft`：通过。

## 判断

- `domain=64` 时每行 support 只有 2 个 32-bit word；在 `support_banks=2` 下 query-level bank conflict 为 0。
- `domain=128` 时每行 support 有 4 个 32-bit word；在 `support_banks=2` 下每个完整行扫描天然产生 conflict，latency 明显放大。
- `random/domain=128` 是本轮最重压力点：`queueP95=13`、`queueMax=19`、`latencyM=100.05`、`bankConflicts=18.2M`。
- `hub/domain=128` 仍呈现尖峰队列形态：`queueP95=1`、`queueMax=127`，说明 hub 主要是 seed fanout 尖峰，而不是持续队列压力。
- `unknownRate=0`，说明统计扩展没有破坏高预算传播语义。

## 下一步

Phase B.3 已完成：

- `support_banks=2/4/8/16` 曲线见 [FPGA_CPIM_PHASE_B3_BANK_SWEEP.md](FPGA_CPIM_PHASE_B3_BANK_SWEEP.md)。
- 本轮数据支持一个简单 banking 规则：`banks >= ceil(domain / 32)`。

后续建议：

- 增加 `--partition-policy=degree|contiguous`，确认 partition 策略对 cross-event ratio 的影响。
- 对 random/domain=128 增加 `density=0.02/0.05/0.10` sweep，找出 queue 与 bank pressure 的拐点。
- 若 bank conflict 在 4/8 banks 下仍可控，再推进 HLS 多 tile dataflow；否则先考虑 bitSup row layout / banking policy。
