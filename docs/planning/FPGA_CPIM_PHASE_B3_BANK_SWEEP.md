# FPGA CPIM Phase B.3 Bank 数量扫描

## 目标

Phase B.2 证明 `domain=128` 在 `support_banks=2` 下会产生明显 bank conflict。Phase B.3 进一步扫描 `support_banks=2/4/8/16`，判断冲突是否能通过增加 bank 数快速下降。

本轮仍只做模拟统计，不改变传播语义。

## 本轮实现

- `run_phase_b_sweep.py` 新增 `--support-banks-list`。
- sweep 输出新增 `banks` 和 `conflictRate`。
- JSONL 每行记录 `support_banks` 和 `support_words_touched`，便于后续离线画曲线。

## 本地证据

命令：

```bash
fpga_cpim/scripts/run_phase_b_sweep.py \
  --binary build/fpga_cpim/fpga_cpim_sim \
  --graphs chain,grid,random,hub \
  --seeds 1 \
  --vars 128 \
  --domains 64,128 \
  --density 0.05 \
  --tightness 0.5 \
  --support-banks-list 2,4,8,16 \
  --support-base-latency 1 \
  --support-conflict-penalty 3 \
  --jsonl build/fpga_cpim/phase_b3_bank_sweep.jsonl
```

结果摘要：

```text
graph  domain  banks  constraints  bitSupKiB  queueP95  queueMax  latencyM  bankConflicts  conflictRate  maxBankAccess  unknownRate
chain  64      2      127.0        127.0      2.0       2         1.84      0.0            0.0000        64             0.000
chain  64      4      127.0        127.0      2.0       2         1.84      0.0            0.0000        32             0.000
chain  64      8      127.0        127.0      2.0       2         1.84      0.0            0.0000        16             0.000
chain  64      16     127.0        127.0      2.0       2         1.84      0.0            0.0000        8              0.000
chain  128     2      127.0        508.0      2.0       2         30.72     5721523.0      0.4221        256            0.000
chain  128     4      127.0        508.0      2.0       2         13.56     0.0            0.0000        128            0.000
chain  128     8      127.0        508.0      2.0       2         13.56     0.0            0.0000        64             0.000
chain  128     16     127.0        508.0      2.0       2         13.56     0.0            0.0000        32             0.000
grid   128     2      233.0        932.0      4.0       4         56.38     10500780.0     0.4221        256            0.000
grid   128     4      233.0        932.0      4.0       4         24.88     0.0            0.0000        128            0.000
random 128     2      404.0        1616.0     13.0      19        100.05    18204095.0     0.4006        256            0.000
random 128     4      404.0        1616.0     13.0      19        45.44     0.0            0.0000        128            0.000
hub    128     2      127.0        508.0      1.0       127       30.72     5721523.0      0.4221        256            0.000
hub    128     4      127.0        508.0      1.0       127       13.56     0.0            0.0000        128            0.000
```

完整输出在 `build/fpga_cpim/phase_b3_bank_sweep.jsonl`。

## 判断

- `domain=64` 每行 2 个 32-bit word，`support_banks=2` 已经足够，冲突率为 0。
- `domain=128` 每行 4 个 32-bit word，`support_banks=2` 的冲突率约 0.40-0.42。
- `domain=128` 一旦提升到 `support_banks=4`，本轮四类图的 bank conflict 全部降为 0；继续到 8/16 banks 不再降低 latency，但会降低 `maxBankAccess`。
- 这说明第一版 HLS/RTL memory banking 可以先以 “bank 数 >= row words” 作为简单规则：`banks >= ceil(domain / 32)`。
- `random/domain=128` 仍是综合压力最大场景：queue pressure 和 bank pressure 都高于 chain/grid/hub。
- `unknownRate=0`，统计矩阵没有改变传播 soundness。

## 下一步

Phase B.4 已完成：

- 加 `--partition-policy=degree|contiguous`。
- 对 `random/domain=128` 扫 `density=0.02/0.05/0.10`。
- 同时固定 `support_banks=4`，观察 queue pressure 和 cross-event ratio 的拐点。
- 结果见 [FPGA_CPIM_PHASE_B4_PARTITION_DENSITY.md](FPGA_CPIM_PHASE_B4_PARTITION_DENSITY.md)。
- 当前数据下 partition 策略差异不大，建议推进 HLS 多 tile dataflow。
