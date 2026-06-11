---
status: active
updated: 2026-05-04T10:23:53Z
type: plan
topic: cpim-metal
slug: metal-gac-v314-runtime-bucket-allowlist
stage: s07
---

# Metal GAC v3.14 Runtime Bucket Allowlist Plan

## Goal

- 将 v3.13 analyzer-only bucket policy simulation 落成 default-off runtime
  probe。
- 不改变 `frontier_mode=auto`、默认 `flags` fallback 或现有 CTA 实验开关。
- 只在已证明无 regression 的 BH-like bucket 上启用 CTA hybrid8，其它输入原样回退。

## Scope

- 包含：
  - 新增 benchmark flag `--policy_mode=none|bh_cta_allowlist`；
  - runtime allowlist 命中时改写 Metal options 为 v3.13 推荐 CTA path；
  - CSV / ablation / analyzer 记录 policy 选择原因；
  - TIER2 对照 shared+flags baseline。
- 不包含：
  - 修改 solver `auto` policy；
  - 扩大 allowlist 到非 BH-4-4 family；
  - 重新实现 CTA owner/queue/handoff。

## Tasks

- `bh_cta_allowlist` 命中条件：
  - input path family 包含 `BH-4-4`；
  - `128 <= num_constraints <= 511`；
  - `max_dom_size < 17`；
  - `bit_words < 2`。
- 命中后使用 CTA hybrid8：
  - `readonly_storage=shared`；
  - `frontier_mode=cta_worklist`；
  - `kernel_variant=word_parallel`；
  - `bitsup_layout=directional`；
  - `reset_mode=cpu`；
  - `cta_owner_mode=vebo_weighted`；
  - `cta_queue_mode=bounded_replay`；
  - `cta_handoff_mode=dirty_var_pull`；
  - `cta_local_round_budget=16`；
  - `cta_replay_round_budget=8`；
  - `cta_dirty_pull_min_degree=8`。
- CSV 新增：
  - `policy_mode`
  - `policy_selected`
  - `policy_reason`
  - `policy_bucket`
- `metal_gac_ablation.py` 新增：
  - `--policy-mode`
  - `--mode-preset=bucket_policy`
- `metal_gac_analyze.py` 新增 runtime policy summary：
  - selected rows / selected inputs；
  - selection reasons；
  - selected-vs-fallback ratio 与 regression rows。

## Validation

- 静态与构建：
  - `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py codex-docops-logic/scripts/dol.py`
  - `cmake --build build_metal --target benchmark_metal_gac -j8`
- 单例：
  - `./build_metal/benchmark_metal_gac --input=benchmarks/BH-4-4/BlackHole-4-4-e-0_ext.xml --runs=1 --warmup=0 --verify=true --runner_mode=prepared --frontier_mode=flags --kernel_variant=scalar --bitsup_layout=pair --policy_mode=bh_cta_allowlist --csv=out/metal_gac_v314_runtime_bucket_allowlist_bh_single.csv`
  - `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=1 --warmup=0 --verify=true --runner_mode=prepared --frontier_mode=flags --kernel_variant=scalar --bitsup_layout=pair --policy_mode=bh_cta_allowlist --csv=out/metal_gac_v314_runtime_bucket_allowlist_fallback_single.csv`
- TIER2：
  - `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=bucket_policy --policy-mode=bh_cta_allowlist --runs=5 --warmup=2 --runner-mode=prepared --kernel-variant=scalar --bitsup-layout=pair --cpu-timing --timeout=300 --csv=out/metal_gac_v314_runtime_bucket_allowlist_tier2.csv --quiet`
  - `python3 tests/python/metal_gac_analyze.py out/metal_gac_v35_tier2_baseline.csv out/metal_gac_v314_runtime_bucket_allowlist_tier2.csv --top=10 --baseline-mode shared+flags --regression-threshold 1.05 --min-runs 3`

## Rollback

- 不传 `--policy_mode=bh_cta_allowlist` 即完全回到旧 benchmark 行为。
- 未命中 allowlist 时保留用户请求的 storage/frontier/kernel/layout/reset/options。
- `frontier_mode=auto` 不读取该 policy。

## Links

- changelog:
  [Metal GAC v3.14 Runtime Bucket Allowlist Changelog](CPIM_METAL_METAL_GAC_V314_RUNTIME_BUCKET_ALLOWLIST_CHANGELOG_2026_05_04.md)
- roadmap:
  [Metal GAC Long Term Optimization](../METAL_GAC_LONG_TERM_OPTIMIZATION.md)
