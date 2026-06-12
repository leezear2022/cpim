# FPGA CPIM Phase P6：HLS 编译时 Profiles

日期：2026-06-12

## 目标

把 `z7020_small` 从文档里的 sizing profile，落成 HLS 编译时真正使用的数组
上限。这样 Vitis `csynth` 的 LUT/FF/BRAM/DSP 数字才对应 7Z020 第一版目标，
而不是继续按 `domain=128` stress fixture 估资源。

## Profiles

`fpga_cpim/hls/cpim_hls_types.hpp` 现在支持：

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

## 本地 Smoke

CMake 新增两个 HLS profile testbench：

```text
hls_tb_z7020_small
hls_tb_z7020_probe2
```

`ctest` 会同时覆盖：

- `stress128`：保留 `domain=128` pressure/capacity 门槛。
- `z7020_small`：`domain=32/constraints=512/worlds=1/queue=512`。
- `z7020_probe2`：`domain=32/constraints=256/worlds=2/queue=512`。

## Sweep 结果

`stress128/random/vars=128/domain=128/density=0.10`：

```text
capacity=272 -> UNKNOWN
capacity=273 -> OK
semantic_min_capacity=273
chosen_depth=512
```

`z7020_small/random/vars=128/domain=32/constraints=512`：

```text
capacity=256/384/512 -> OK
queue_peak_partition=91
semantic_min_capacity=91
recommended_depth_1p25=114
recommended_depth_pow2=128
chosen_depth=128
```

这说明 `z7020_small` 的第一版 queue depth 512 不是压力瓶颈；下一步资源瓶颈
更可能来自 `bitSup` 与 subscription 存储，而不是 per-partition queue。

## Vitis HLS

`run_hls.tcl` 默认：

```bash
vitis_hls -f fpga_cpim/hls/run_hls.tcl
```

等价于：

```text
PROFILE=z7020_small
solution=z7020_small
part=xc7z020clg400-1
```

可切换：

```bash
PROFILE=z7020_probe2 vitis_hls -f fpga_cpim/hls/run_hls.tcl
PROFILE=stress128 vitis_hls -f fpga_cpim/hls/run_hls.tcl
```

无 Vitis 环境时，继续用：

```bash
python3 fpga_cpim/hls/parse_hls_reports.py \
  --solution z7020_small \
  --output build/fpga_cpim/hls_report_summary.json
```

记录 `tool_missing`，不阻塞本地 C++ / `g++` 回归。

## 验收命令

```bash
cmake --build build/fpga_cpim -j
ctest --test-dir build/fpga_cpim --output-on-failure
fpga_cpim/scripts/run_hls_trace_sweep.py \
  --jsonl build/fpga_cpim/hls_trace_p6_stress128.jsonl \
  --raw build/fpga_cpim/hls_trace_p6_stress128.out
fpga_cpim/scripts/run_hls_trace_sweep.py \
  --profile=z7020_small \
  --tiles=1 \
  --capacity-sweep=256,384,512 \
  --jsonl build/fpga_cpim/hls_trace_p6_z7020_small.jsonl \
  --raw build/fpga_cpim/hls_trace_p6_z7020_small.out
python3 -m py_compile fpga_cpim/scripts/*.py fpga_cpim/hls/*.py
git diff --check
dol lint --soft
```
