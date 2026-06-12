# FPGA CPIM 下一阶段闭环记录

日期：2026-06-12

## 主线

本阶段从继续堆 synthetic sweep，转向三个闭环：

1. 语义闭环：`VariableOwner` 支持多源删除合并、DWO race、fanout 和
   rerun pending。
2. 节点闭环：`NodeCommand` 能表达 root AC、branch AC、ACThenNSACQ 和
   incomplete reason。
3. HLS 闭环：本地 `g++ hls_tb` 保留 pressure/capacity sweep；有 Vitis 时
   `run_hls.tcl` 必须能产出 `csynth`，`parse_hls_reports.py` 抽取摘要。

全局安全不变量：

```text
UNKNOWN / AliveIncomplete / overflow / budget hit 不允许产生未验证删除。
所有 confirmed_deletions 必须能被 Golden AC/NSACQ 在同模式下复现。
```

## Queue Sizing Rule

当前 random pressure fixture：

```text
fixture=random/vars=128/domain=128/density=0.10/partitions=4
semantic_min_capacity = 273
recommended_depth_1p25 = 342
recommended_depth_pow2 = 512
chosen_depth = 512
chosen_depth_overhead = 1.875
```

`chain` 和 `hub` 在 `272/273/274/320/384` sweep 内全部 OK，因此记录为
`semantic_min <= min_tested_capacity`，不强行要求 UNKNOWN。

## 7Z020 Profiles

第一硬件目标固定为：

```text
z7020_small:
  MAX_VARS=128
  MAX_CONSTRAINTS=512
  MAX_DOMAIN=32
  MAX_WORDS=1
  MAX_WORLDS=1
  REVISION_TILES=1
  OWNER_TILES=1
  QUEUE_DEPTH=512
```

保留 probe 双 world 变体：

```text
z7020_probe2:
  MAX_VARS=128
  MAX_CONSTRAINTS=256
  MAX_DOMAIN=32
  MAX_WORDS=1
  MAX_WORLDS=2
  REVISION_TILES=1
  OWNER_TILES=1
  QUEUE_DEPTH=512
```

`domain=128` 只作为 stress fixture，不作为 Z7020 第一版目标。

## 真实 Benchmark Profile

新增：

```bash
python3 fpga_cpim/scripts/profile_benchmarks.py \
  --output build/fpga_cpim/benchmark_profiles.jsonl
```

默认覆盖：

- `tests/data/bench/rand-2-23/rand-23-23-253-131-50021_ext.xml`
- `benchmarks/tightness0.2/rand-2-40-11-414-200-0_ext.xml`
- `benchmarks/tightness0.35/rand-2-40-16-250-350-0_ext.xml`
- `tests/data/bench/queens-12_ext.xml`
- `benchmarks/haystacks/haystacks-11.xml`

当前 profile 结论：

- `rand-2-23`、`tightness0.35`、`queens-12` fit `z7020_small` 和
  `z7020_probe2`。
- `tightness0.2` fit `z7020_small`，不 fit `z7020_probe2`。
- `haystacks-11` 变量数可放下，但约束数 615 超过 `z7020_small` 512，因此
  第一版不 fit。

## 验收命令

```bash
cmake --build build/fpga_cpim -j
ctest --test-dir build/fpga_cpim --output-on-failure
python3 -m py_compile fpga_cpim/scripts/*.py fpga_cpim/hls/*.py
python3 fpga_cpim/scripts/profile_benchmarks.py \
  --output build/fpga_cpim/benchmark_profiles.jsonl
fpga_cpim/scripts/run_hls_trace_sweep.py \
  --jsonl build/fpga_cpim/hls_trace_next.jsonl \
  --raw build/fpga_cpim/hls_trace_next.out
python3 fpga_cpim/hls/parse_hls_reports.py \
  --output build/fpga_cpim/hls_report_summary.json
git diff --check
dol lint --soft
```

本机没有 Vitis 时，HLS report summary 应显示 `tool_missing`，不阻塞 C++
原型。
