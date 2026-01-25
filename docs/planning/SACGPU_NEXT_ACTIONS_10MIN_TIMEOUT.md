---
status: active
updated: 2026-01-25
---

# SACGPU 下一步执行清单（10 分钟超时版）

本文把 `docs/planning/TODO_SACGPU_NEXT.md` 里“下一步该做什么”落成可执行的命令清单。

> 约定：所有脚本/命令的 **wall-time 超时统一设置为 10 分钟（600 秒）**，避免长跑时卡死。

## 0. 构建（一次即可）

```bash
mkdir -p build && cd build && cmake .. && make -j$(nproc)
```

确认存在：
- `build/sac_benchmark`

## 1. preprocess 主线：跑齐 tier1/tier2 基线（可中断/续跑）

目的：得到 “SAC preprocess（删值/传播深度/unknown/time）” 的稳定对照基线，后续优化才有意义。

### 1.1 tier1（39 个）

```bash
python3 tests/python/batch_sac_benchmark.py \
  --tier=1 \
  --mode=full_sac \
  --timeout=600 \
  --max-sac-rounds=1 \
  --nsac-mask=1 \
  --csv=out/sac_preprocess_tier1_10min.csv \
  --resume --flush-every=1
```

### 1.2 tier2（79 个）

```bash
python3 tests/python/batch_sac_benchmark.py \
  --tier=2 \
  --mode=full_sac \
  --timeout=600 \
  --max-sac-rounds=1 \
  --nsac-mask=1 \
  --csv=out/sac_preprocess_tier2_10min.csv \
  --resume --flush-every=1
```

说明：
- `--resume`：断了直接重跑同一条命令会跳过已完成样例（ok 记录）
- `--flush-every=1`：每跑完 1 个样例就原子落盘，最适合长跑
- `--max-sac-rounds=1`：用于快速对照；如果你要更接近“收敛到不动点”，把 rounds 调大（同时保留 `--timeout=600`）

## 2. 扩充 preprocess 评测集：抽样扫描 benchmarks（可中断/续跑）

目的：从 `benchmarks/` 里持续挖出 “删值多/传播深/会触发 unknown” 的样例，避免评测被“删值=0 的实例”主导。

### 2.1 抽样扫描（建议先跑 500 个）

```bash
python3 tests/python/select_sac_preprocess_benches.py \
  --bench-root=benchmarks \
  --bin=build/sac_benchmark \
  --nsac-mask=1 \
  --max-sac-rounds=1 \
  --instance-timeout=600 \
  --shuffle --seed=202601 \
  --limit=500 \
  --csv=out/sac_preprocess_scan_10min.csv \
  --resume --flush-every=20 \
  --write-tiers=tests/python/sac_preprocess_tier_definitions.py
```

### 2.2 断点续跑

同样命令重复执行即可续跑；如果想把之前的 error 也重跑：

```bash
python3 tests/python/select_sac_preprocess_benches.py \
  --csv=out/sac_preprocess_scan_10min.csv \
  --resume --retry-errors
```

### 2.3 定向扫描（按目录/关键词）

```bash
# 例如：只扫 tightness0.9
python3 tests/python/select_sac_preprocess_benches.py \
  --include=tightness0.9/ \
  --instance-timeout=600 \
  --csv=out/sac_preprocess_scan_tightness0.9_10min.csv \
  --resume
```

## 3. P0-3（下一阶段）：把统计输出收口成稳定格式

P0-3 已落地：`sac_benchmark` 的 preprocess 模式统一输出并可被脚本解析（含 `Avg/P95/Max iterations`），
`tests/python/batch_sac_benchmark.py` 支持 `--mode=full_sac/sac1_preprocess/sac3_preprocess`。

后续（可选增强）：
- 额外输出一行 JSON（或 CSV）以减少正则解析维护成本（属于“可用性增强”，非必须）。

## 4. 回归口径（可选）：e2e sanity

`preprocess` 的优化不一定能反映到 e2e，但建议保留 e2e sanity 防止破坏 soundness：

```bash
python3 tests/python/batch_test_v3.py --tier=0 --timeout=600 --export-csv=out/e2e_tier0_10min.csv
```
