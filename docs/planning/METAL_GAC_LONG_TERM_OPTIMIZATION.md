---
status: active
updated: 2026-05-03
---

# Metal GAC 长期优化路线

> 主轴：GAC 性能优先。先把 Metal GAC-only 后端做成可测、可消融、可回退的
> 高性能路径，再进入 SAC/Batch。CUDA 经验只迁移工程原则，不照搬 cooperative
> persistent kernel。

> 变更落账：每个小计划和每个小 changelog 都独立成文，索引见
> [Metal GAC Changelog Index](METAL_GAC_CHANGELOG.md)；全局摘要见 `CHANGES_ZH.md`。

## 1. 当前边界

- Metal v2.x 已完成 GAC correctness、prepared runner、storage/frontier/kernel/layout
  消融、auto policy、epoch worklist、reset mode 与 unsupported 分类。
- 默认稳定路径保持 `cold + shared + flags + scalar + pair + cpu reset`。
- `worklist`、`word_parallel`、`bitsup_layout=directional|auto` 已作为可消融路径落地；
  `simdgroup` 本轮仍显式回退到 `word_parallel`，由 `variant_name` 明确记录。
- 不把 global AllDifferent、predicate/intension、非二元 extension 纳入 v2 性能阶段。

## 2. Metal v2 分段路线

### v2.0：基线冻结

- 固定 benchmark matrix：`metal-smoke`、TIER0、TIER2 all-mode、TIER3 baseline。
- CSV 固定记录：
  - `runner_mode`
  - `readonly_storage`
  - `frontier_mode`
  - `kernel_variant`
  - `bitsup_layout`
  - `reset_mode`
  - `effective_frontier_mode`
  - `effective_kernel_variant`
  - `effective_bitsup_layout`
  - `variant_name`
  - `solve_ms`
  - `prepare_ms`
  - `reset_ms`
  - `reset_dispatch_ms`
  - `dispatch_ms`
  - `kernel_ms`
  - `active_constraints_total`
  - `worklist_push_count`
  - `worklist_rounds`
  - `worklist_epoch_resets`
  - `frontier_density_avg`
- 主求解指标使用 `solve_ms` 的 p50/p95/p99；性能声明至少需要 3 runs。
- `setup_ms`、`prepare_ms` 只作为初始化观测，不纳入求解性能主指标。

### v2.1：减少 setup/dispatch 固定成本

- 新增 prepared/reusable runner：一次初始化 runtime、pipeline 与只读 buffer，多次
  reset mutable state 后运行。
- benchmark 通过 `--runner_mode=cold|prepared` 对照冷启动与 prepared 成本。
- `setup_ms` 拆成 `prepare_ms + reset_ms`，用于判断初始化和 mutable reset 成本。
- `solve_ms = reset_ms + dispatch_ms`，表示单次 GAC 求解端到端成本；若只看
  Metal 提交/同步成本，则使用 `dispatch_ms`。

### v2.2：frontier 深度优化

- 已从 `flags/compact` 扩展到 `worklist/auto`：
  - `worklist` 借鉴 CUDA Batch-3A dynamic submission 的“结尾提交”思想；
  - Metal 仍保留 host round loop，不依赖 grid-wide sync；
  - worklist 使用 active constraint 双缓冲和 next frontier 去重，每轮只 dispatch revise
    kernel，不再额外 compact。
- `auto` 采用封版保守规则：TIER2 收尾数据发现较宽的 worklist auto 会让 p95
  超过 baseline 5%，word_parallel auto 会让 p50 超过 baseline 5%；因此 v2
  封版的 `frontier_mode=auto` 降级到 flags，`kernel_variant=auto` 降级到 scalar。

### v2.3：位集算子优化

- 已新增 `word_parallel` kernel：一个线程处理 `(constraint, direction, target_word)`，
  合并同一 target word 的 deletion mask 后一次 atomic clear。
- `kernel_variant=auto` 在 v2 封版时保守降级到 `scalar`；`word_parallel` 保留为
  显式消融路径，`simdgroup` 当前显式 fallback 到 `word_parallel`。
- 默认仍不改变 `scalar`。

### v2.4：内存与布局优化

- 已新增 directional bitSup buffer：`bit_sup_words[cid][dir][value][word]`。
- `--bitsup_layout=pair|directional|auto` 控制 bitSup 载体；`auto` 使用 directional。
- 如引入 texture/read-only cache，必须保留扁平 `MTLBuffer` fallback。
- 本轮不引入 texture；pair `DeviceUInt2` buffer 继续作为默认 fallback。

### v2.5：auto policy 与路径一致性

- `variant_name` 只描述真实执行路径；当 requested 与 effective 不一致时使用
  `requested->effective`。
- CSV 新增 `effective_frontier_mode/effective_kernel_variant/effective_bitsup_layout`，
  分析脚本不再解析 `variant_name`。
- scalar flags/compact 仍读 pair bitSup，因此即便 requested 为 directional，
  effective bitsup 也会记录为 `pair`；worklist/word_parallel 才记录为
  `directional`。

### v2.6：worklist epoch 去清零

- worklist 把 `next_flags` 作为 epoch/stamp 表使用，每轮只递增 epoch，不再
  memset 全量 frontier。
- kernel enqueue 使用 `atomic_exchange(epoch)` 去重，`stats[2]` 仍是下一轮
  active count。
- 新增 `worklist_push_count`、`worklist_rounds`、`worklist_epoch_resets`。

### v2.7：分析与 auto recommender

- `metal_gac_ablation.py --mode-preset=auto` 直接扫描
  `frontier_mode=auto kernel_variant=auto bitsup_layout=auto`。
- `metal_gac_analyze.py` 增加 recommended policy summary，按 family、
  `num_constraints`、`max_dom_size`、`bit_words`、`frontier_density_avg` 分桶，
  并报告 auto 命中、胜出和劣化超过 5% 的实例。
- TIER2 中已知 unsupported non-binary extension 继续分类记录，不计作性能失败。

### v2.8：GPU-side reset / blit reset

- `MetalPreparedGacRunner` 保留初始 mutable snapshot buffer。
- `--reset_mode=cpu|blit|auto` 控制 reset；默认仍为 `cpu`，`auto` 在 mutable
  reset 字节数较大时选择 blit。
- `reset_dispatch_ms` 单独记录 blit copy/fill command buffer 时间；主指标仍为
  `solve_ms = reset_ms + dispatch_ms`。

### v2.9：v2 封版与 simdgroup 决策

- v2 不强行实现复杂 simdgroup reduction；`simdgroup` 保留为
  `word_parallel` fallback。
- v2 封版 auto acceptance：TIER2 baseline `shared+flags+scalar+pair`
  `solve_ms p50=0.261 p95=5.132`，auto 封版路径 `flags+scalar+pair`
  `solve_ms p50=0.259 p95=5.079`。
- v3 触发条件：word_parallel 数据证明 bitSup intersection / domain atomic 是
  主瓶颈，且 `kernel_ms` p95 至少连续两组 tier 数据明显高于 host dispatch/reset
  成本，再评估 simdgroup/threadgroup staging。

## 3. CUDA 经验映射

| CUDA 经验 | Metal v2 处理 |
|----------|---------------|
| Persistent kernel 减少 host 往返 | 保持 host round loop，先用 prepared runner 降低固定成本 |
| Dynamic submission/worklist | v2.2 引入 Metal worklist；预算/溢出保守回退 |
| Stage2/Batch 消融开关 | Metal 所有新路径都走 runtime flag 和 CSV 记录 |
| 位集/warp 聚合 | 先落地 word-parallel；simdgroup 保留为后续 |
| UMA 相位控制 | 用 shared/private storage、CPU/blit reset 与明确 reset/dispatch 边界表达 |

## 4. 验收与回退

- correctness：
  - `ctest --test-dir build_metal -R compare_cpu_metal --output-on-failure`
  - `ctest --test-dir build_metal -R benchmark_metal_gac --output-on-failure`
  - `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=all --runs=2 --warmup=1 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --timeout=60 --csv=out/metal_gac_v2x_smoke_all.csv`
  - `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=auto --runs=3 --warmup=1 --runner-mode=prepared --timeout=60 --csv=out/metal_gac_v2x_smoke_auto.csv`
  - `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=1 --warmup=0 --verify=true --runner_mode=prepared --frontier_mode=worklist --kernel_variant=word_parallel --bitsup_layout=directional --reset_mode=blit --csv=out/metal_gac_v2x_worklist_word_blit_smoke.csv`
- performance：
  - `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=all --runs=5 --warmup=2 --runner-mode=prepared --timeout=300 --csv=out/metal_gac_v2x_tier2_all.csv --quiet`
  - `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=auto --runs=5 --warmup=2 --runner-mode=prepared --timeout=300 --csv=out/metal_gac_v2x_tier2_auto.csv --quiet`
  - `python3 tests/python/metal_gac_analyze.py out/metal_gac_v2x_tier2_all.csv out/metal_gac_v2x_tier2_auto.csv --top=10`
- 回退：
  - 默认路径必须始终可设回 `--runner_mode=cold --readonly_storage=shared --frontier_mode=flags --kernel_variant=scalar --bitsup_layout=pair --reset_mode=cpu`。
  - CUDA/Jetson 路径不受 Metal v2 开关影响。

## 5. 下一步

- 当前小计划：
  [Metal GAC v2 Guard / v3 Entry Plan](metal_gac/METAL_GAC_V2_GUARD_V3_ENTRY_PLAN_2026_05_03.md)。
- 当前小 changelog：
  [Metal GAC v2.5-v2.9 Changelog](metal_gac/METAL_GAC_V25_V29_CHANGELOG_2026_05_03.md)。
- 规则：路线文档只保留摘要和链接；每个可执行小计划、每个小 changelog 都必须
  单独写新文档。
