# CPIM FPGA Binary Table Propagation Prototype

这是 CPIM 的独立 FPGA 方向原型目录。第一版只验证 binary table constraint 的传播引擎形态，不接现有 CUDA/Metal/GPU 主线，也不接真实搜索。

## Build

```bash
cmake -S fpga_cpim -B build/fpga_cpim
cmake --build build/fpga_cpim -j
ctest --test-dir build/fpga_cpim --output-on-failure
```

示例：

```bash
./build/fpga_cpim/fpga_cpim_sim \
  --instance synthetic \
  --vars 32 \
  --domain 32 \
  --density 0.2 \
  --mode nsacq \
  --worlds 4 \
  --partitions 4 \
  --json out.json
```

## 第一版架构

```text
Host CPU:
  parsing / search / heuristic / fallback

FPGA prototype:
  bitSup support oracle
  revise tile
  variable owner
  event router
  epoch termination
  multi-world probe controller

Output:
  OK / DWO / UNKNOWN
```

当前实现是 C++17 软件/周期级模拟器，加一个可用普通 `g++` 编译的 HLS-friendly 核心 testbench。

HLS-friendly testbench：

```bash
g++ -std=c++17 -I fpga_cpim/hls \
  fpga_cpim/hls/testbench_hls.cpp fpga_cpim/hls/*.cpp \
  -o hls_tb
./hls_tb
```

## 为什么需要 VariableOwner

Constraint tile 只读 domains 并产生 delete mask。`VariableOwner` 是唯一负责 domain update、delta、DWO 和 fanout 的模块。

直接让多个 constraint tile 并发 `AND` global domain 在数学上仍是 monotonic，但工程上会丢失精确 delta、DWO 归属、subscription fanout 触发和终止计数一致性。并行传播需要明确的状态一致性模型；snapshot / temporary GAC 一类做法都说明，local revise 输出与 global commit 必须分层，到 fixed point 后才等价于目标一致性。因此模拟器显式区分 `ReviseTile` 的 delete mask 与 `VariableOwner` 的 commit。

## UNKNOWN 语义

```text
UNKNOWN is conservative.
UNKNOWN means "hardware did not prove DWO within resource/budget limits".
UNKNOWN must never remove a value.
```

这与现有 CPIM SACGPU 的 `kUNKNOWN` 语义一致：预算超限、未收敛、队列满、局部 buffer 满或 revise/support 限额命中时，probe 对外不产生 deletion。

全局安全不变量：

```text
UNKNOWN / AliveIncomplete / overflow / budget hit 不允许产生未验证删除。
所有 confirmed_deletions 必须能被 Golden AC/NSACQ 在同模式下复现。
```

## NSACQ 语义

第一版只实现 `nsac_radius=1`。对 focal variable `i`，允许传播的约束是 `{i} ∪ neighbors(i)` 诱导子图内的 binary constraints。该 mask 只过滤 event enqueue，不改 `bitSup & other_domain` 的单约束支持检查。

## Phase B 统计

模拟器输出硬件形态相关指标：

- queue occupancy p50/p95/max。
- fanout p50/p95/max。
- support oracle latency cycles。
- support oracle bank conflicts。
- support oracle max bank accesses。
- greedy variable partition。
- local/cross partition event ratio。
- high-degree hub count。
- BRAM18 / URAM288 粗估。

快速 sweep：

```bash
fpga_cpim/scripts/run_phase_b_sweep.py \
  --binary build/fpga_cpim/fpga_cpim_sim \
  --seeds 3 \
  --vars 32 \
  --domain 32 \
  --graphs chain,grid,random,hub \
  --partitions 4 \
  --support-banks 4
```

大一点的 bank 压力 sweep：

```bash
fpga_cpim/scripts/run_phase_b_sweep.py \
  --binary build/fpga_cpim/fpga_cpim_sim \
  --graphs chain,grid,random,hub \
  --seeds 1 \
  --vars 128 \
  --domains 64,128 \
  --density 0.05 \
  --support-banks-list 2,4,8,16 \
  --support-base-latency 1 \
  --support-conflict-penalty 3
```

经验规则：当前 bitSup word 为 32-bit，第一版 banking 可先令
`support_banks >= ceil(domain / 32)`。在 128 变量 synthetic sweep 中，
`domain=128` 从 2 banks 提升到 4 banks 后，query-level bank conflict 降为 0。

Partition / density sweep：

```bash
fpga_cpim/scripts/run_phase_b_sweep.py \
  --binary build/fpga_cpim/fpga_cpim_sim \
  --graphs random \
  --seeds 1 \
  --vars 128 \
  --domain 128 \
  --densities 0.02,0.05,0.10 \
  --partition-policies degree,contiguous \
  --support-banks 4
```

## HLS Phase C

HLS-friendly core 已支持：

- 编译时 profile：`stress128`、`z7020_small`、`z7020_probe2`。
- 输入 `var_partition` / `constraint_partition`。
- per-partition 环形 event queue。
- round-robin 多 revise tile 调度。
- queue peak、local/cross event、router overflow 统计。
- `vars=128/domain=128/density≈0.10` pressure smoke。
- `1/2/4` revise tile 对照的稳定 `hls_pressure` 输出。
- `--pressure-only`、`--tiles`、`--capacity-sweep` 参数与
  `parse_hls_trace.py` JSONL/CSV 转换。
- `run_hls_trace_sweep.py` 自动编译/运行/产出 JSONL；当前 pressure fixture
  的 per-partition queue capacity 门槛为 `273`。
- `chain/random/hub` 三类最小 fixture，分别覆盖低 fanout、普通压力和高
  fanout 极端。
- JSONL sizing 字段：`semantic_min_capacity`、
  `recommended_depth_1p25`、`recommended_depth_pow2`、`chosen_depth`、
  `chosen_depth_overhead`。

预算超限和 per-partition queue overflow 仍返回 `UNKNOWN`。

当前 random fixture 的 sizing rule：

```text
fixture=random/vars=128/domain=128/density=0.10
semantic_min_capacity = 273
recommended_depth_1p25 = ceil(273 * 1.25) = 342
recommended_depth_pow2 = 512
chosen_depth = 512
chosen_depth_overhead = 1.875
```

## NodeCommand 闭环

`NodeCommand` 是搜索节点下发到 FPGA 原型的第一版模拟接口，当前支持：

- root AC。
- branch assignment 后 AC cascade。
- AC 后按 `kAllVars` / `kFocalVar` / `kNeighborhoodOfFocal` 启动 NSACQ probes。
- `UNKNOWN` probe 只增加 `unknown_probe_count` 和 incomplete reason，不产生
  deletion。
- confirmed singleton DWO deletion 必须再由 Golden probe 复现，才进入
  `confirmed_deletions`。

`kRunBranchProbes` 只保留接口空间，当前明确返回 `AliveIncomplete`。

## 真实 Benchmark Profile

真实 benchmark 第一阶段只做 profile，不接完整 XCSP propagation：

```bash
python3 fpga_cpim/scripts/profile_benchmarks.py \
  --output build/fpga_cpim/benchmark_profiles.jsonl
```

输出字段包括 `vars`、`max_domain`、`constraints`、`binary_ratio`、
`avg_fanout`、`max_fanout`、`estimated_bitSup_bytes` 和
`z7020_profile_fit`。

## 7Z020 Profiles

第一硬件落点固定为小 profile，`domain=128` 只作为 stress fixture。

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

本地 HLS profile smoke 已注册到 `ctest`：

```text
hls_tb              -> stress128
hls_tb_z7020_small  -> z7020_small
hls_tb_z7020_probe2 -> z7020_probe2
```

`run_hls.tcl` 默认使用 `z7020_small`，可通过环境变量
`PROFILE=z7020_probe2` 或 `PROFILE=stress128` 切换。

## 和 SAT-FPGA BCP 的关系

相似点：

- 都是事件驱动传播。
- 都需要硬件本地 pending / cursor / dispatcher。
- 都需要明确读写仲裁和终止条件。
- 都把软件动态队列改成硬件可见的数据流。

不同点：

- SAT BCP 处理 watched literals、trail、learned clauses、conflict analysis。
- CPIM 第一版处理 fixed binary table constraints，不做 learning、不做 backjump。
- CPIM 的 `bitSup` 是 read-only fabric，约束图固定，因此比完整 CDCL BCP 简单。
- CPIM 有多 world SAC/QSAC/NSACQ，domain state 复制和 `UNKNOWN` 语义是 SAT BCP 没有的重点。

VeriSAT 显示 FPGA SAT solver 的内存结构、传播引擎、仲裁、pipeline 都需要硬件定制；同时 HLS 会让代码到硬件的对应关系更难追踪。因此本项目先做 C++ simulator + HLS-friendly core，再决定哪些模块手写 RTL。

## 第一版不做

- 完整 CP solver。
- 非二元约束、global constraints、intension constraints。
- learned clauses、dynamic table update。
- 256 worlds bit-matrix。
- full async token termination。
- RRAM/CAM 后端。
- FPGA shell / AXI host driver。
- constraint tile 直接写 global domain。
- 在 `UNKNOWN` / overflow 后输出 deletion。
