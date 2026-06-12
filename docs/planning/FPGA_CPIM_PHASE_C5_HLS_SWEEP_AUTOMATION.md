# FPGA CPIM Phase C.5 HLS Sweep 自动化

## 目标

Phase C.5 将 HLS trace 流程收成一个可重复脚本：

- 自动编译 HLS-friendly testbench。
- 自动运行 `--pressure-only`。
- 默认细扫 `capacity=272,273,274,320,384`。
- 复用 `hls_pressure` / `hls_capacity` 解析逻辑，直接输出 JSONL。
- 打印 capacity overflow 门槛摘要。

本轮仍不接 Vitis、不写 RTL、不改变 HLS core 语义。

## 本轮实现

- 新增 `fpga_cpim/scripts/run_hls_trace_sweep.py`。
- 默认编译输出：`build/fpga_cpim/hls_tb_trace`。
- 默认 JSONL 输出：`build/fpga_cpim/hls_trace_sweep.jsonl`。
- 默认 raw 输出：`build/fpga_cpim/hls_trace_sweep.out`。
- 支持参数：
  - `--tiles`
  - `--capacity-sweep`
  - `--jsonl`
  - `--raw`
  - `--binary`
  - `--cxx`
  - `--no-build`

## 验收命令

```bash
fpga_cpim/scripts/run_hls_trace_sweep.py \
  --jsonl build/fpga_cpim/hls_trace_c5.jsonl \
  --raw build/fpga_cpim/hls_trace_c5.out
```

输出摘要：

```text
kind tiles capacity status events epochs queuePeakTotal queuePeakPart routerOverflow
pressure 1 1024 OK 1586 1586 592 273 0
pressure 2 1024 OK 1586 793 592 273 0
pressure 4 1024 OK 1586 397 592 273 0
capacity 4 272 UNKNOWN 1057 265 592 272 1
capacity 4 273 OK 1586 397 592 273 0
capacity 4 274 OK 1586 397 592 273 0
capacity 4 320 OK 1586 397 592 273 0
capacity 4 384 OK 1586 397 592 273 0
capacity_threshold max_unknown=272 min_ok=273
```

完整回归：

```bash
cmake --build build/fpga_cpim -j
ctest --test-dir build/fpga_cpim --output-on-failure
python3 -m py_compile fpga_cpim/scripts/*.py
git diff --check
dol lint --soft
```

结果：12/12 通过。

## 判断

- 当前 fixture 的 per-partition queue capacity 精确门槛为 `273`：
  - `272`: `UNKNOWN`，`router_overflow=1`
  - `273/274/320/384`: `OK`
- 这个门槛与 full-capacity run 的 `queue_peak_partition=273` 对齐。
- `queue_peak_total=592`，说明 total pending 不等于单 partition 容量需求；
  HLS queue sizing 需要按 worst partition peak 估算。
- `capacity=273` 时 work 恢复完整：
  `events=1586`、`deleted_values=16129`。

## 下一步

建议进入 Phase C.6：

- 增加第二个压力 fixture，例如 hub/random-high-degree，避免只对一个 random
  fixture 调参。
- 给 `run_hls_trace_sweep.py` 增加多 fixture 输出字段。
- 将 capacity sizing 规则写成 `peak_partition + safety_margin`，先试
  `margin=16/32/64`。
