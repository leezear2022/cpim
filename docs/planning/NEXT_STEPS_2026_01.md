---
status: active
updated: 2026-01-25
---

# SACGPU 下一步：观测、验证与调参（2026-01）

> **对齐版本**：截至 2026-01-25 的代码现状（P0-3, P1-2/3 已落地）。
>
> **主文档**：更完整的工程拆解与依赖关系以 `docs/planning/TODO_SACGPU_NEXT.md` 为准；
> 本文是“下一步做什么”的**执行聚焦清单**。

## 0. 战略调整：聚焦“推理能力”主线，Batch-3A 后置

> 核心思想：不用再花精力去“证明 Batch-3A 慢”，而是先把 SAC3 的“快”和“强”在数据上打穿。

- **主线（P0/P1）** = **Preprocess 对照 + 样例集沉淀**
  - 利用 P0-3 (Unified Observability) 工具，在 `regression/perf` suite 上跑全 `sac3_preprocess`。
  - 沉淀出一组“代表性样例”（删值多/传播深/有 UNKNOWN/长尾明显），作为每次改动的**回归与性能哨兵**。
  - 证明 GPU 在这些“硬骨头”上的吞吐优势和 soundness。

- **附录/复测（可选）** = **Batch-3A**
  - Batch-3A 仅作为 default-off 的可选路径（`--sac_use_batch3a`）。
  - **复测条件**：只在 `nsac_mask=false`（否则 gating 禁用）、且 probe 传播足够深（例如 p95_iterations 明显大、或 UNKNOWN 多）、且 batch size 足够大时再测。
  - 否则无需重复验证“它慢 10-25x”。

## 1. 立即执行：主线 Preprocess 评测（P0/P1 闭环）

**目的**：利用已就位的 P0-3, P1-2, P1-3 工具，建立稳固的“推理能力”基线。

### 1.1 跑通核心 Suite

使用 `tests/python/batch_sac_benchmark.py`：
- **Regression**（日常回归）：覆盖高删值/高 probes/SAC-DWO/0 删值收敛
  ```bash
  python3 tests/python/batch_sac_benchmark.py \
    --suite=regression --mode=sac3_preprocess \
    --timeout=60 --max-sac-rounds=200 --nsac-mask=1 \
    --csv=out/sac3_regression.csv --resume --flush-every=1
  ```
- **Perf Sentinels**（性能哨兵）：吞吐/延迟/内存压力敏感
  ```bash
  python3 tests/python/batch_sac_benchmark.py \
    --suite=perf --mode=sac3_preprocess \
    --timeout=600 --max-sac-rounds=200 --nsac-mask=1 \
    --csv=out/sac3_perf_10min.csv --resume --flush-every=1
  ```

可选：跑一份 `full_sac(stage2)` 作为 baseline（与 `sac3_preprocess` 对照时建议保留同一套 suite/参数）：
```bash
python3 tests/python/batch_sac_benchmark.py \
  --suite=regression --mode=full_sac \
  --timeout=60 --max-sac-rounds=1 --nsac-mask=1 \
  --csv=out/full_sac_regression.csv --resume --flush-every=1
```

### 1.2 沉淀“硬样例”清单

根据 CSV 结果，筛选出满足以下特征的实例，更新到 `tests/python/sac_preprocess_tier_definitions.py` 的精选列表：
- **Deep Propagation**: `p95_iterations > 10` 或 `avg_iterations` 显著高于 1。
- **Effective Deletions**: `total_deletions > 0`（排除无效跑空）。
- **Long-tail Behavior**: `unknown_probes > 0`（触发 budget/stagnation，验证 P1-2 机制）。

## 2. 后续调优：针对性参数微调

仅在筛选出的“硬样例”上进行，避免在简单样例上过拟合。

- **Stagnation / Quantum** (P0-1a/b)：在 UNKNOWN 密集的样例上，观察“早停与否”对总吞吐和删值率的权衡。
- **Failure Priority** (P0-1d)：在 `p95_iterations` 大的样例上，验证分桶调度能否加速 DWO 发现。
- **Queue Budget** (P1-2)：验证在极难样例（如 `graphw`）上，budget 是否能有效防止显存/时间爆炸且保持 soundness。

## 3. Checklist（下周节奏）

- [ ] **执行**：跑完 `regression` 和 `perf` suite 的 `sac3_preprocess` 基线。
- [ ] **分析**：产出 benchmark 报告，通过数据确认 P1 改进（AutoStage, QueueBudget）的实际收益。
- [ ] **沉淀**：更新 `tests/python/sac_preprocess_tier_definitions.py`，固化一批“ SACGPU 必测集”。
