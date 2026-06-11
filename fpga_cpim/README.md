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

## HLS Phase C.2

HLS-friendly core 已支持：

- 输入 `var_partition` / `constraint_partition`。
- per-partition 环形 event queue。
- round-robin 多 revise tile 调度。
- queue peak、local/cross event、router overflow 统计。
- `vars=128/domain=128/density≈0.10` pressure smoke。

预算超限和 per-partition queue overflow 仍返回 `UNKNOWN`。

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
