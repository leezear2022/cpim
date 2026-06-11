# FPGA CPIM Phase B.4 Partition Policy 与 Density 扫描

## 目标

Phase B.4 检查两个问题：

- `degree` 与 `contiguous` 两种变量 partition policy 对 cross-event ratio 的影响。
- 在 `random/domain=128/support_banks=4` 固定后，density 增长时 queue pressure 是否出现明显拐点。

本轮仍只改变统计与 sweep，不改变传播语义。

## 本轮实现

- `PartitionConfig` 新增 `PartitionPolicy { degree, contiguous }`。
- CLI 新增 `--partition-policy degree|contiguous`。
- JSON `config.partition_policy` 与 `partition.policy` 输出当前 policy。
- `run_phase_b_sweep.py` 新增：
  - `--densities`
  - `--partition-policies`
  - 输出列 `density`、`policy`。

## 本地证据

命令：

```bash
fpga_cpim/scripts/run_phase_b_sweep.py \
  --binary build/fpga_cpim/fpga_cpim_sim \
  --graphs random \
  --seeds 1 \
  --vars 128 \
  --domain 128 \
  --densities 0.02,0.05,0.10 \
  --partition-policies degree,contiguous \
  --support-banks 4 \
  --support-base-latency 1 \
  --support-conflict-penalty 3 \
  --jsonl build/fpga_cpim/phase_b4_density_policy_sweep.jsonl
```

结果：

```text
graph   domain  banks  constraints  bitSupKiB  queueP95  queueMax  density  policy      latencyM  bankConflicts  conflictRate  maxBankAccess  crossRatio  hubs  bram18  uram288  unknownRate
random  128     4      166.0        664.0      6.0       7         0.020    contiguous  18.02     0.0            0.0000        128            0.401       37.0  328.0   19.0     0.000
random  128     4      166.0        664.0      6.0       7         0.020    degree      18.02     0.0            0.0000        128            0.370       37.0  328.0   19.0     0.000
random  128     4      404.0        1616.0     13.0      19        0.050    contiguous  45.44     0.0            0.0000        128            0.386       2.0   751.0   45.0     0.000
random  128     4      404.0        1616.0     13.0      19        0.050    degree      45.44     0.0            0.0000        128            0.391       2.0   751.0   45.0     0.000
random  128     4      785.0        3140.0     35.0      43        0.100    contiguous  98.84     0.0            0.0000        128            0.381       0.0   1428.0  88.0     0.000
random  128     4      785.0        3140.0     35.0      43        0.100    degree      98.84     0.0            0.0000        128            0.373       0.0   1428.0  88.0     0.000
```

`ctest`：12/12 通过。

`dol lint --soft`：通过。

## 判断

- `support_banks=4` 对 `domain=128` 足够，本轮所有 density/policy 组合 bank conflict 都为 0。
- density 增长带来明显 queue pressure 增长：
  - `0.02`: `queueP95=6`、`queueMax=7`
  - `0.05`: `queueP95=13`、`queueMax=19`
  - `0.10`: `queueP95=35`、`queueMax=43`
- `degree` 与 `contiguous` 在 random graph 上 cross ratio 接近：
  - `density=0.02`: degree 更低，`0.370` vs `0.401`
  - `density=0.05`: contiguous 略低，`0.386` vs `0.391`
  - `density=0.10`: degree 略低，`0.373` vs `0.381`
- 当前数据不足以证明 contiguous 更好；默认仍保留 `degree`。
- `density=0.10` 是下一阶段 HLS dataflow 更合适的压力样例。

## 后续

HLS Phase C.2 已完成，见
[FPGA_CPIM_PHASE_C2_HLS_DATAFLOW.md](FPGA_CPIM_PHASE_C2_HLS_DATAFLOW.md)：

- 固定 `support_banks=ceil(domain/32)`。
- 固定默认 `partition_policy=degree`。
- 在 HLS-friendly core 中模拟多 revise tile 和 per-partition queue。
- 用 `random/vars=128/domain=128/density=0.10` 作为压力 smoke。
- 保持 overflow/budget 命中只返回 `UNKNOWN`。
