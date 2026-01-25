---
status: active
---

# SAC preprocess 评测教程（SAC/MSAC/NSAC）

本教程面向“推理/编码能力”的 preprocess 口径评测，强调 **删值/传播深度/吞吐**，
而非搜索阶段的求解效率。核心入口是 `sac_benchmark` 的 preprocess 模式（`full_sac` /
`sac1_preprocess` / `sac3_preprocess`）以及配套的批量脚本。

## 1. 基础构建

```bash
mkdir -p build && cd build && cmake .. && make -j$(nproc)
```

确保生成 `build/sac_benchmark`。

## 2. 单实例手动跑（最小示例）

```bash
./build/sac_benchmark --input=benchmarks/langford/langford-2-4-ext.xml \
  --mode=full_sac --max_sac_rounds=5 --nsac_mask=true --verbose
```

### 2.1 这里跑的是 SAC 还是 NSAC？

- `--mode=full_sac`：外层是“对当前所有 `(var,val)` 做 singleton probe”的 **SAC 外框**（可多轮）。
- `--mode=sac1_preprocess`：GPU **SAC1**（dirty-set 变量粒度，多轮）。
- `--mode=sac3_preprocess`：GPU **SAC3 / (N)SACQ**（probe 队列驱动，按 batch 推进）。
- `--nsac_mask=true`：每个 probe 内部的传播被限制在 `Xi + N(Xi)` 诱导子图上（`allowed-constraints mask`），
  这是 **NSAC 风格的 singleton test**（sound but incomplete，通常更 GPU-friendly）。

所以你跑出来的 CSV 里 `nsac_mask=1` 时，本质是“**SAC 外框 + NSAC 传播半径**”。
若想跑“全图传播的更强版本”（更接近传统 full SAC），把 `--nsac_mask=false`（或脚本参数 `--nsac-mask=0`）。

输出关键字段：
- `Total deletions`：删值总数（核心指标）
- `Avg/P95/Max iterations`：传播深度/长尾强度（P0-3 统一观测口径）
- `Unknown probes`：预算/停滞触发比例（观测闭环）

## 3. 自动筛选 “对 SAC 有意义” 的评测集

脚本：`tests/python/select_sac_preprocess_benches.py`

### 快速抽样（可中断/续跑）
```bash
python3 tests/python/select_sac_preprocess_benches.py \
  --limit=500 \
  --instance-timeout=10 \
  --max-sac-rounds=1 \
  --nsac-mask=1 \
  --csv=out/sac_preprocess_scan.csv \
  --resume --flush-every=10 \
  --write-tiers=tests/python/sac_preprocess_tier_definitions.py
```

### 断点续跑
同样命令重复执行即可续跑；若想重跑错误样例，加 `--retry-errors`。

### 只从已有 CSV 重新生成 tier
```bash
python3 tests/python/select_sac_preprocess_benches.py \
  --from-csv=out/sac_preprocess_scan.csv \
  --write-tiers=tests/python/sac_preprocess_tier_definitions.py
```

### 常用过滤
```bash
# 只扫 tightness0.9 目录
python3 tests/python/select_sac_preprocess_benches.py \
  --include=tightness0.9/ --instance-timeout=3 --max-sac-rounds=1 \
  --csv=out/sac_preprocess_scan_tightness0.9.csv --resume
```

## 4. 按 preprocess tier 批量跑（推荐）

脚本：`tests/python/batch_sac_benchmark.py`

```bash
python3 tests/python/batch_sac_benchmark.py \
  --tier=0 --mode=full_sac --timeout=10 --max-sac-rounds=1 --nsac-mask=1 \
  --csv=out/sac_preprocess_tier0.csv
```

### 4.1 推荐的“回归/性能哨兵”suite（更适合每次改动都跑）

相比 tier0/1/2（偏“展示集”），suite 更强调“覆盖路径 + 可复现 + 运行时间可控”：

```bash
# 日常回归：覆盖高删值/高 probes/SAC-DWO/0 删值收敛/较高耗时
python3 tests/python/batch_sac_benchmark.py \
  --suite=regression --mode=full_sac --timeout=60 --max-sac-rounds=1 --nsac-mask=1 \
  --csv=out/sac_preprocess_regression.csv --resume --flush-every=1

# 性能哨兵：更偏吞吐/延迟/内存压力（建议 10 分钟超时）
python3 tests/python/batch_sac_benchmark.py \
  --suite=perf --mode=full_sac --timeout=600 --max-sac-rounds=1 --nsac-mask=1 \
  --csv=out/sac_preprocess_perf_10min.csv --resume --flush-every=1
```

### 4.2 对照：Stage2(full_sac) vs SAC3(sac3_preprocess)

```bash
# Stage2：全域扫一轮（SAC 外框）
python3 tests/python/batch_sac_benchmark.py \
  --suite=regression --mode=full_sac --timeout=60 --max-sac-rounds=1 --nsac-mask=1 \
  --csv=out/sac_preprocess_regression_full_sac.csv --resume --flush-every=1

# SAC3：probe 队列驱动（flatten NSACQ/SACQ 的主线入口）
python3 tests/python/batch_sac_benchmark.py \
  --suite=regression --mode=sac3_preprocess --timeout=60 --max-sac-rounds=200 --nsac-mask=1 \
  --csv=out/sac_preprocess_regression_sac3.csv --resume --flush-every=1
```

### 可中断/续跑（推荐长跑时开启）

```bash
python3 tests/python/batch_sac_benchmark.py \
  --tier=2 --timeout=600 --max-sac-rounds=1 --nsac-mask=1 \
  --csv=out/sac_preprocess_tier2_10min.csv \
  --resume --flush-every=1
```

## 5. MSAC 模式（可选）

`sac_benchmark` 支持模拟 MSAC：

```bash
./build/sac_benchmark --input=<bench> \
  --mode=full_sac --assign_count=5 --msac_mode=fast
```

- `--assign_count`：先赋值若干变量后再跑 SAC（模拟搜索中的 MSAC）
- `--msac_mode`：`fast`/`full`/`parallel`

## 6. 超时含义说明

- `--instance-timeout` / `--timeout`：**wall-time** 超时（秒）
- `--max-sac-rounds`：**算法轮数上限**（到上限会显示 `Status: TIMEOUT`，但不代表 wall-time 超时）
  - `full_sac`：外层 SAC 轮数
  - `sac1_preprocess`：SAC1 外层轮数
  - `sac3_preprocess`：SAC3 的 batch 次数（soft-budget；建议用 wall-time 控制上限）

## 7. 输出文件

- 扫描 CSV：`out/sac_preprocess_scan*.csv`
- preprocess tier 列表：`tests/python/sac_preprocess_tier_definitions.py`
- batch 结果 CSV：`out/sac_preprocess_tier*.csv`
