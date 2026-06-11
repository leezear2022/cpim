---
status: active
updated: 2026-05-04T00:00:00Z
type: plan
topic: cpim-metal
slug: metal-gac-v313-bucket-policy-simulation
stage: s07
---

# Metal GAC v3.13 Bucket Policy Simulation Plan

## Goal

- 在 v3.12 证明 CTA dirty hybrid 只对少数 bucket 有强信号后，不改 `auto`，
  先在 analyzer 中模拟 report-only bucket policy。
- 目标是回答：只在安全 bucket 启用 CTA，其它输入回退 `shared+flags`，整体 p95
  是否下降且没有 regression。

## Scope

- `metal_gac_analyze.py --recommend-policy` 新增 `[bucket policy simulation]`。
- 候选路径按完整参数分组，不再只按 `shared+cta_worklist` 粗分：
  - storage/frontier；
  - kernel/bitsup/reset；
  - CTA owner/queue/handoff/local/replay/dirty threshold。
- 对每个 feature bucket，只有满足以下条件才进入 eligible：
  - bucket 内实例数不少于 `--bucket-min-instances`；
  - 相对 fallback 的 `p95 <= 1.0x`；
  - regression rows 为 0。
- policy simulation 使用 eligible bucket 的候选路径，其它输入使用 fallback。
- 新增 CLI：
  - `--bucket-min-instances`，默认 3。

## Validation

- `python3 -m py_compile tests/python/metal_gac_analyze.py tests/python/metal_gac_ablation.py codex-docops-logic/scripts/dol.py`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v35_tier2_baseline.csv out/metal_gac_v35_tier2_frontier.csv out/metal_gac_v39_vebo_weighted_local16_tier2.csv out/metal_gac_v311_dirty_var_pull_tier2.csv out/metal_gac_v312_dirty_pull_hybrid8_tier2.csv --top=10 --recommend-policy --baseline-mode shared+flags --regression-threshold 1.05 --min-runs 3 --bucket-min-instances 3`
- `git diff --check`
- `python3 codex-docops-logic/scripts/dol.py lint --soft`

## Results

- combined simulation：
  - fallback：`shared+flags`；
  - eligible buckets：1；
  - selected inputs：4/76；
  - policy `p50=0.312ms p95=1.696ms`；
  - fallback `p50=0.312ms p95=2.987ms`；
  - `policy_vs_fallback p50=1.00x p95=1.00x`；
  - `better=4/76`；
  - `regressions_gt_threshold=0`。
- eligible bucket：
  - `family=BH-4-4 cons=128-511 dom=<17 bitw=<2 density=0.10-0.50`；
  - recommendation：`shared+cta_worklist word_parallel directional cpu`
    with `vebo_weighted + bounded_replay + dirty_var_pull + local=16 + replay=8 +
    dirty_min=8`；
  - bucket compared：4；
  - bucket `p50=0.33x p95=0.34x`；
  - selected：4。

## Decision

- v3.13 证明 CTA 不适合全局默认，但适合少数安全 bucket 的 report-only policy。
- 当前只作为 analyzer simulation，不改 benchmark runtime `auto`。
- 下一步若要落地 runtime bucket policy，必须先把 family/path 规则从 analyzer
  证据转成显式 allowlist，并保留 `auto` 默认不变。
