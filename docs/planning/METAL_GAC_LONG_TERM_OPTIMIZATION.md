---
status: active
updated: 2026-05-03
---

# Metal GAC/SAC 长期优化路线

> 主轴：GAC 性能优先。先把 Metal GAC-only 后端做成可测、可消融、可回退的
> 高性能路径，再进入 SAC/Batch。CUDA 经验只迁移工程原则，不照搬 cooperative
> persistent kernel。

> 变更落账：每个小计划和每个小 changelog 都独立成文，索引见
> [Metal GAC Changelog Index](METAL_GAC_CHANGELOG.md)；全局摘要见 `CHANGES_ZH.md`。

## 1. 当前边界

- Metal v2.x 已完成 GAC correctness、prepared runner、storage/frontier/kernel/layout
  消融、auto policy、epoch worklist、reset mode 与 unsupported 分类。
- v3.18 结论：当前路线没有大方向偏移。单实例 Metal GAC 性能线冻结为
  stable correctness/fallback path；新的性能主线是 Batch/SAC/NSACQ 吞吐化。
- v3.18 第一段已开始落地：`benchmark_metal_sac --dwo_forensics` 能记录
  raw/confirmed/rejected DWO、raw precision、rejected-DWO domain-size/popcount
  mismatch 与 first rejected probe metadata，用于先解释 v3.17 rejected DWO。
- v3.18 第二段已定位并修复一个 raw DWO 误报源：`sac_probe_init_kernel`
  曾在同一 dispatch 内复制 snapshot 并由 `cid==0` 写 singleton，导致 missing-value
  status 可能读到未初始化 world buffer；现在 init 直接从 immutable snapshot
  生成 singleton world。
- v3.18 full TIER2 forensics：228 OK rows、3 个历史
  `unsupported_non_binary_extension`、286,744 probes、19,885 raw DWO 全部
  confirmed、0 rejected DWO、0 UNKNOWN。
- v3.18 command-buffer fusion 第一版已落地为 benchmark-only `probe_fusion=bounded`：
  默认仍为 `none`；bounded 模式把一段 `revise/frontier` 轮次预编码到一个
  command buffer 中，TIER2 对照显示 command-buffer/probe avg 从 `0.0528`
  降到 `0.0076`，non-kernel/probe avg 从 `0.0310ms` 降到 `0.0198ms`，
  supported rows 保持 228 OK、0 rejected DWO、0 UNKNOWN。
- v3.18 fusion-round sweep 已完成：TIER2 `fusion_rounds=2/4/8` 均保持
  0 rejected DWO 与 0 UNKNOWN；`8` 的 avg command-buffer/probe 最低，但 p95
  non-kernel/probe 与 wasted rounds 劣于 `4`，因此当前保留 `4` 作为 balanced
  explicit bounded setting。
- 当前瓶颈分为两层：
  - GAC-only：短 kernel 的 command-buffer / wait / non-kernel fixed cost；
  - NSACQ：raw Metal DWO 可信度、CPU-confirm guard 成本、batch/queue 组织。
- Metal v3 已启动，主线为 evidence/recommender/simdgroup gate；先输出报告和
  进入条件，不改变 v2 默认 fallback。
- v3 CPU vs Metal 对照已落地：当前 TIER2 auto 口径下 CPU GAC 明显快于 Metal
  GAC，Metal faster 为 `0/380`。
- v3.3 simdgroup gate 结论保持：当前 evidence 指向 dispatch/round 往返瓶颈，
  不进入 simdgroup kernel 实现。
- v3.4 CTA-local persistent worklist 已实现为显式实验路径：`frontier_mode=cta_worklist`
  在单次 Metal dispatch 内完成 threadgroup 局部多轮 worklist 推进；不让多个
  CTA 直接竞争同一个全局 c queue。
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
  [Metal SAC v3.18 DWO Forensics Command Fusion NSACQ Throughput Plan](metal_gac/CPIM_METAL_METAL_SAC_V318_DWO_FORENSIC_FUSION_PLAN_2026_05_24.md)。
- 当前小 changelog：
  [Metal SAC v3.18 DWO Forensics Command Fusion NSACQ Throughput Changelog](metal_gac/CPIM_METAL_METAL_SAC_V318_DWO_FORENSIC_FUSION_CHANGELOG_2026_05_24.md)。
- 当前补充小计划：
  [Metal SAC v3.18 Fusion Rounds Sweep Plan](metal_gac/CPIM_METAL_METAL_SAC_V318_FUSION_ROUNDS_SWEEP_PLAN_2026_05_27.md)。
- 当前补充小 changelog：
  [Metal SAC v3.18 Fusion Rounds Sweep Changelog](metal_gac/CPIM_METAL_METAL_SAC_V318_FUSION_ROUNDS_SWEEP_CHANGELOG_2026_05_27.md)。
- v3 执行状态：
  - `metal_gac_analyze.py --recommend-policy` 已作为 report-only recommender
    入口落地；
  - 新增 `kernel_share/dispatch_share/reset_share` 与
    `worklist_push_per_round` 观察项；
  - v3 TIER2 evidence：baseline `shared+flags+scalar+pair`
    `solve_ms p50=0.290 p95=4.995`，`shared+auto`
    `solve_ms p50=0.223 p95=6.293`；
  - combined recommender 显示 baseline bottleneck counts 为 `dispatch=76`，
    且 `shared+auto` p95 ratio 为 `1.54`、19 个实例超过 5% regression
    threshold，因此不提升 auto policy，不进入 simdgroup kernel 实现。
  - CPU vs Metal TIER2 auto evidence：Metal solve `p50=0.439ms p95=5.050ms`，
    CPU solve `p50=0.008438ms p95=0.071971ms`，Metal/CPU ratio
    `p50=33.65x p95=427.90x`，Metal faster `0/380`。
  - v3.4 已新增 `cta_worklist` 实验路径、CTA queue A/B、CTA stamp/tail 和
    `gac_revise_cta_worklist_kernel`；smoke 显示 `gac_bitwords2.xml` 通过 CPU
    verify，`host_round_count=1`，`cta_local_rounds=2`。
  - v3.5 已新增 CTA evidence gate：analyzer 输出 `[cta worklist gate]`，
    用 TIER2 对比 `flags/worklist/cta_worklist` 的 p50/p95/p99、host round、
    CTA overflow 与 `metal_cpu_solve_ratio`，判断是否进入 recommender；默认
    `auto` 不变。
  - v3.5 TIER2 gate 结论：`cta_worklist` 未通过。Combined gate 显示
    `cta_vs_shared+flags p50=1.27x p95=2.99x`、
    `cta_vs_best_worklist p50=1.37x p95=2.99x`、
    `host_round_ratio_vs_baseline p50=1.00x p95=1.00x`，
    `cta_overflow_count p95=0`，最终 `decision=report_only`。
  - 因 CTA 未降低 TIER2 host round 且 p95 明显回退，下一步不应把
    `cta_worklist` 纳入 `auto`；若继续 Metal 性能线，优先评估 Batch/SAC
    多任务吞吐或 owner partition 重设计。
  - v3.6 已新增 owner partition 分叉探索备忘：先记录
    `owner_map_static`、`owner_bucketed_seed`、`dirty_var_pull`、
    `hub_replication`、`hierarchical_steal`、`indirect_multiround` 六条分支及
    promote/reject gate。
  - v3.6 优先主线调整为三类：`primal_edge_cut_owner`、`vebo_weighted_owner`
    和 `bulk_sync_deletion_mask`；支撑分支用于解释 seed、steal、hub 和 indirect
    multi-round 是否值得继续。
  - v3.6 `owner_map_static` 已作为 default-off 实验路径落地：
    `--cta_owner_mode=static_edge_cut` 基于 constraint subscription adjacency
    构建 `owner_of_constraint[cid]`，CTA seed/kernel 统一读取 owner map；
    默认仍为 `modulo`，`auto` 不变。
  - v3.6 `owner_map_static` TIER2 gate 未通过：
    `cta_vs_shared+flags p50=1.83x p95=4.51x`，
    `host_round_ratio_vs_baseline p50=1.00x`，`cta_overflow_count p95=1`；
    结论保持 report-only，下一步转向 `vebo_weighted_owner` 或 seed/overflow。
  - v3.7 已新增 `--cta_queue_mode=spill_replay`，拆分
    `cta_queue_overflow_count`、`cta_budget_spill_count`、
    `cta_seed_overflow_count` 与 seed owner balance stats。
  - v3.7 TIER2 gate 结论：`queue_overflow_p95=0`、
    `seed_overflow_p95=0`、`budget_spill_p95=1`，
    `cta_vs_shared+flags p50=1.41x p95=3.54x`，
    `host_round_ratio_vs_baseline p50=1.00x`，最终仍为 report-only。
    这说明 seed/queue 真 overflow 不是主因；下一步优先评估 local budget /
    bounded replay，若 host round 仍不降再进入 `vebo_weighted_owner`。
  - v3.8 已新增 `--cta_queue_mode=bounded_replay`、
    `--cta_local_round_budget` 与 `--cta_replay_round_budget`，并记录
    `cta_budget_replay_rounds`、`cta_budget_replay_drain_count`、
    `cta_budget_replay_spill_count`。
  - v3.8 TIER2 gate 结论：`bounded_replay local=8 replay=8` 与
    `spill_replay local=16` 都能让 `budget_spill_p95=0`，但
    `host_round_ratio_vs_baseline p50=1.00x`，且 p95 仍为 baseline 的
    `3.49x` 到 `3.57x`；结论保持 report-only。下一步进入
    `vebo_weighted_owner`，不再继续优先调整 seed/overflow/budget 协议。
  - v3.9 已新增 `--cta_owner_mode=vebo_weighted`，按 variable degree 顺序和
    constraint weighted load 构建 owner map，并新增
    `owner_weight_balance_p95`。
  - v3.9 TIER2 `vebo_weighted local=16 replay=8`：
    `solve_ms p50=0.513 p95=3.325`，
    `cta_vs_shared+flags p50=1.41x p95=3.14x`，
    `owner_balance_p95_avg=1.28`，`owner_weight_balance_p95_avg=1.22`，
    `budget_spill_p95=0`，但 `host_round_ratio_vs_baseline p50=1.00x`；
    结论仍为 report-only。若继续 CTA，应围绕 owner locality/cross-push
    hybrid 调优；否则转回 Batch/SAC 多任务吞吐。
  - v3.10 改为先实现 default-off `frontier_mode=bulk_sync_mask`：
    revise 阶段只写 `delete_masks[var][word]`，apply 阶段统一更新 domain 并生成
    下一轮 frontier，目标主打大例子 / 传播重例子，而不是继续硬推 CTA owner。
  - v3.10 analyzer 新增 `[bulk sync mask gate]`，并拆出 `large_any` 与
    `large_prop` gate；TIER2 为 380/383 OK，但 combined gate 显示
    `bulk_vs_shared+flags p50=1.86x p95=3.17x`，
    `large_prop p50=1.94x p95=2.76x`，
    `dispatch_ratio_vs_baseline p50=2.00x`，因此不主打大例子收益，仍保持
    report-only。
  - v3.11 已新增 default-off `--cta_handoff_mode=dirty_var_pull`：CTA kernel
    将跨 owner subscription 改为 dirty var 标记，host 轮末扫描 dirty vars 的
    subscriptions 生成下一轮 frontier。TIER2 为 380/383 OK，相对 v3.9
    `vebo_weighted local=16 replay=8` 从 `solve_ms p50=0.513 p95=3.357`
    改为 `p50=0.470 p95=3.202`，45/76 个实例更快；但
    `cta_vs_shared+flags p50=1.31x p95=3.00x`、
    `cta_vs_best_worklist p50=1.48x p95=2.98x`，仍保持 report-only。
  - v3.12 已新增 `--cta_dirty_pull_min_degree`，让 dirty pull 只作用于
    subscription degree 达标的变量，低度变量回退 direct push。TIER2 hybrid8
    为 380/383 OK，`solve_ms avg=0.837 p50=0.488 p95=2.192`，相对 v3.11
    p95 明显下降但 p50 略慢；shared+flags gate 仍为
    `p50=1.41x p95=3.21x`，保持 report-only。BH-4-4 bucket 仍是明确正信号，
    `p50_ratio=0.40 p95_ratio=0.43`。
  - v3.13 已在 analyzer 中新增 bucket policy simulation：按完整候选路径分组，
    只选择 `p95 <= 1.0x` 且无 regression 的 bucket。Combined evidence 中只
    `BH-4-4` bucket eligible，选择 CTA hybrid8 后 policy `p50=0.312ms
    p95=1.696ms`，fallback `p50=0.312ms p95=2.987ms`，
    `regressions_gt_threshold=0`。runtime `auto` 仍不改变。
  - v3.14 已把 v3.13 BH bucket recommendation 落成 default-off runtime
    `--policy_mode=bh_cta_allowlist`：仅当 input family 为 `BH-4-4` 且
    `128 <= num_constraints <= 511`、`max_dom_size < 17`、`bit_words < 2`
    时启用 CTA hybrid8，其它输入保持用户请求路径。TIER2 为 380/383 OK，
    selected inputs 为 4/76；combined analysis 中 selected-vs-shared+flags
    `p50=0.62x p95=0.68x`，4/4 更快，regressions over 1.05 为 0。
    `frontier_mode=auto` 仍不读取该 policy。
  - v3.15 已新增 additive dispatch timing split：
    `dispatch_encode_ms`、`dispatch_wait_ms`、`dispatch_non_kernel_ms`。TIER2
    baseline 为 380/383 OK，`solve_ms p50=0.236 p95=5.403`；
    analyzer 显示 `encode=0.013ms`、`kernel=0.105ms`、
    `non_kernel=0.668ms`，`non_kernel_share=0.86`。结论是当前 GAC-only
    瓶颈不是 CPU encode，而是短 kernel 周围的 command buffer wait /
    non-kernel 固定成本；下一步应优先评估 Batch/SAC 多任务吞吐或能摊薄
    dispatch 的方案。
  - v3.16 已新增 default-off `benchmark_metal_sac` 与
    `MetalBatchProbeRunner`：stable Metal GAC 先生成 AC snapshot，再把 snapshot
    remaining values 作为 singleton probe worlds 批量执行。BH smoke 中每轮
    384 probes，CPU/Metal probe status verify 通过，`dispatch_per_probe=0.0182`，
    `non_kernel_per_probe≈0.0038ms`，说明大批量 SAC probe 能明显摊薄 v3.15 的
    command buffer non-kernel 成本。TIER2 为 380/383 OK，3 个 ERROR 均为历史
    `unsupported_non_binary_extension`，supported rows `verify_mismatches=0`、
    `UNKNOWN=0`；整体 `dispatch_per_probe avg=0.0527 p50=0.0182 p95=0.1402`，
    `non_kernel_per_probe avg=0.014634ms p50=0.005242ms p95=0.023366ms`。
    结论：v3.16 benchmark-only 成立；若继续推进，应进入 host-side NSACQ、
    queue budget 与 DWO writeback。
  - v3.17 已把 v3.16 batch probe 扩展为 default-off host-side NSACQ：
    `--sac_mode=nsacq|sacq_adj|sacq_full`，Metal batch runner 支持
    `allowed_constraints` mask，host 侧维护 remaining-value queue，DWO probe
    回写主 snapshot 后重新运行 stable Metal GAC。`queens-4` smoke 覆盖
    DWO/writeback：24 probes 中 8 DWO，删除 8 个 host snapshot values，并触发
    1 次 post-delete GAC；metal-smoke 为 5/5 OK、178 probes、10 个 DWO values
    written back。TIER2 为 228/231 OK，3 个 ERROR 均为历史
    `unsupported_non_binary_extension`；supported rows 跑 278,172 probes、0
    UNKNOWN、12,094 confirmed DWO writeback，但也有 4,970 rejected
    unconfirmed Metal DWO。结论：v3.17 语义安全 guard 成立，但 raw Metal DWO
    status 不能 promote；下一步若继续 SAC，应优先做 deterministic/double-buffer
    probe，或把 CPU-confirmed DWO guard 纳入 promote gate。
  - v3.18 大计划更新：外部 review 认为路线没有大方向偏移。单实例 GAC-only
    不再作为性能主线，保留为 stable fallback、instrumentation 与窄 bucket
    allowlist；Metal 性能资源转向 Batch/SAC/NSACQ 吞吐化。
  - v3.18 下一步固定为三项优先实验：
    `dwo_forensic_oracle`、`command_buffer_fusion`、
    `nsacq_batch_policy_sacq_compare`。其中 DWO forensic 是先决条件：在
    raw Metal DWO precision 接近可信或 rejected DWO 被明确归类前，不移除
    CPU-confirmed DWO guard，不接搜索。
  - v3.18 第一段实现了 benchmark-only DWO forensic counters 与 optional
    world-domain readback：`benchmark_metal_sac --dwo_forensics=true|false`、
    `metal_sac_ablation.py --dwo-forensics|--no-dwo-forensics`。该路径只用于
    evidence/debug，不改变 solver 默认行为。
  - v3.18 第二段用 DWO status debug words 把 first rejected sample 定位到
    init-kernel missing-value path，并修复 snapshot copy / singleton seed 的
    同 dispatch 竞争。修复后 TIER0 limited NSACQ forensics 连续 3 runs 为
    `852/852` confirmed DWO、`0` rejected DWO；driver 定点 3 runs 为 `12/12`
    confirmed DWO、`0` rejected DWO。CPU-confirmed guard 仍保留，等待 TIER2
    扩展证据。
  - v3.18 full TIER2 NSACQ forensics 已完成：228 OK rows、3 个历史
    `unsupported_non_binary_extension`、286,744 probes、19,885 raw DWO 全部
    confirmed、0 rejected、0 UNKNOWN。下一步可以讨论是否把 raw DWO 从
    “必须 CPU confirm”推进到“report-only promote gate”，但默认 guard 暂不移除。
  - v3.18 bounded command fusion 已完成第一版：
    `MetalRuntime::Dispatch1DBatch` 在一个 command buffer 内按序编码多 compute
    dispatch，`benchmark_metal_sac --probe_fusion=bounded --fusion_rounds=4`
    显式启用；TIER2 bounded 结果为 228 OK、3 个历史 unsupported、289,657
    probes、22,777 confirmed DWO、0 rejected DWO、0 UNKNOWN。相对
    `probe_fusion=none`，avg command-buffer/probe 从 `0.0528` 降到
    `0.0076`，avg non-kernel/probe 从 `0.0310ms` 降到 `0.0198ms`；
    dispatch/probe 上升是预编码轮次的预期代价，后续应扫描
    `fusion_rounds=2/4/8` 并推进 NSACQ queue/SACQ 对照。
  - v3.18 fusion-round sweep 已完成：
    TIER2 sweep 为 684 OK rows、9 个历史 unsupported、865,743 probes、
    65,108 confirmed DWO、0 rejected DWO、0 UNKNOWN。`fusion_rounds=4`
    的 non-kernel/probe p95 为 `0.021674ms`，优于 `2` 的
    `0.026859ms` 和 `8` 的 `0.023821ms`；`8` command-buffer/probe 更低，
    但 wasted rounds 为 `1398`，明显高于 `4` 的 `438`。当前结论是
    `4` 作为均衡显式设置，`8` 保持 report-only。
- 规则：路线文档只保留摘要和链接；每个可执行小计划、每个小 changelog 都必须
  单独写新文档。
