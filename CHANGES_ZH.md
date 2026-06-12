# 修改清单（中文）

## 2026-06-12

### FPGA CPIM：P6 HLS 编译时 profile / z7020_small smoke

**目标**：把 `z7020_small` 从文档 sizing profile 落成 HLS 编译时数组上限，
避免用 `domain=128` stress fixture 的资源规模指导 7Z020 第一版。

**核心改动**：
- `cpim_hls_types.hpp` 新增 `FPGA_CPIM_HLS_PROFILE_*` 编译时 profile：
  `stress128`、`z7020_small`、`z7020_probe2`。
- `z7020_small` 固定为 `MAX_VARS=128`、`MAX_CONSTRAINTS=512`、
  `MAX_DOMAIN=32`、`MAX_WORDS=1`、`MAX_WORLDS=1`、
  `MAX_PARTITION_QUEUE=512`、`MAX_REVISE_TILES=1`。
- CMake 新增 `hls_tb_z7020_small` 和 `hls_tb_z7020_probe2`，注册到
  `ctest`。
- HLS trace rows 新增 `profile=<name>` 字段；sizing 分组按 profile 区分。
- `run_hls_trace_sweep.py` 新增 `--profile`，默认 binary 路径按 profile
  分开，避免并行 sweep 互相覆盖。
- `run_hls.tcl` 默认 `PROFILE=z7020_small`，可用环境变量切换到
  `z7020_probe2` 或 `stress128`。
- 新增记录：
  `docs/planning/FPGA_CPIM_PHASE_P6_HLS_PROFILES.md`。

**验证**：
- `cmake --build build/fpga_cpim -j`
- `ctest --test-dir build/fpga_cpim --output-on-failure`
- `fpga_cpim/scripts/run_hls_trace_sweep.py --jsonl build/fpga_cpim/hls_trace_p6_stress128.jsonl --raw build/fpga_cpim/hls_trace_p6_stress128.out`
- `fpga_cpim/scripts/run_hls_trace_sweep.py --profile=z7020_small --tiles=1 --capacity-sweep=256,384,512 --jsonl build/fpga_cpim/hls_trace_p6_z7020_small.jsonl --raw build/fpga_cpim/hls_trace_p6_z7020_small.out`
- `python3 -m py_compile fpga_cpim/scripts/*.py fpga_cpim/hls/*.py`
- `git diff --check`
- `dol lint --soft`

**结果摘要**：
- `ctest` 从 13 个测试扩到 15 个测试，新增两个 HLS profile smoke。
- `stress128/random` 仍保持 `capacity=272 UNKNOWN`、`capacity=273 OK`。
- `z7020_small/random` 在 `capacity=256/384/512` 下均 OK，
  `queue_peak_partition=91`、`chosen_depth=128`。

### FPGA CPIM：语义闭环 / NodeCommand / 7Z020 HLS 报告闭环

**目标**：从继续扩展 synthetic sweep 转向闭环验证：`VariableOwner`
多源删除语义、`NodeCommand` 搜索节点闭环、真实 benchmark profile、
fixture-specific queue sizing、Vitis HLS report parser 和 7Z020 小 profile。

**核心改动**：
- `VariableOwner` 新增批量删除合并、DWO race 稳定语义、fanout 查询和
  `PROCESSING -> RERUN_PENDING` 状态。
- 新增 `NodeCommand` / `NodeResult`，支持 root AC、branch AC、
  ACThenNSACQ、confirmed deletion、unknown probe count 和 incomplete
  reason mask。
- `parse_hls_trace.py` 为 pressure/capacity rows 增加
  `semantic_min_capacity`、`recommended_depth_1p25`、
  `recommended_depth_pow2`、`chosen_depth`、`chosen_depth_overhead`。
- HLS testbench 新增 `--fixtures=chain,random,hub`，不再只看单一 random
  pressure fixture。
- 新增 `fpga_cpim/scripts/hardware_profiles.py` 和
  `profile_benchmarks.py`，对仓库真实 XCSP benchmark 输出 profile JSONL
  与 `z7020_small` / `z7020_probe2` fit 判断。
- 新增 `fpga_cpim/hls/run_hls.tcl` 和 `parse_hls_reports.py`，有 Vitis 时
  抽取 csim/csynth/cosim、latency、II、LUT/FF/BRAM/DSP、estimated Fmax；
  无 Vitis 时输出 `tool_missing`。
- 更新 `fpga_cpim/README.md`、`fpga_cpim/hls/README.md` 和
  `docs/planning/FPGA_CPIM_NEXT_STAGE_CLOSURE.md`。

**验证**：
- `cmake --build build/fpga_cpim -j`
- `ctest --test-dir build/fpga_cpim --output-on-failure`
- `python3 -m py_compile fpga_cpim/scripts/*.py fpga_cpim/hls/*.py`
- `python3 fpga_cpim/scripts/profile_benchmarks.py --output build/fpga_cpim/benchmark_profiles.jsonl`
- `fpga_cpim/scripts/run_hls_trace_sweep.py --jsonl build/fpga_cpim/hls_trace_next.jsonl --raw build/fpga_cpim/hls_trace_next.out`
- `python3 fpga_cpim/hls/parse_hls_reports.py --output build/fpga_cpim/hls_report_summary.json`
- `git diff --check`
- `dol lint --soft`

**结果摘要**：
- `random/vars=128/domain=128/density=0.10` 仍为
  `capacity=272 UNKNOWN`、`capacity=273 OK`。
- random fixture sizing：`semantic_min=273`、`1.25x=342`、`pow2=512`、
  `chosen_depth=512`、`overhead=1.875`。
- `chain` / `hub` 在当前 capacity range 内全部 OK，报告
  `semantic_min <= min_tested_capacity`。
- 真实 benchmark profile 5 条均产出 JSONL；`haystacks-11` 因
  `constraints=615` 不 fit `z7020_small`。
- 本机 Vitis 缺失时 HLS report summary 稳定输出 `tool_missing`。

### FPGA CPIM：HLS Phase C.5 sweep 自动化 / capacity 门槛

**目标**：自动编译/运行 HLS testbench 并产出 JSONL，细扫
`capacity=272/273/274/320/384`，把 per-partition queue overflow 门槛钉准。

**核心改动**：
- 新增 `fpga_cpim/scripts/run_hls_trace_sweep.py`。
- 默认编译到 `build/fpga_cpim/hls_tb_trace`。
- 默认运行 `--pressure-only --tiles=1,2,4
  --capacity-sweep=272,273,274,320,384`。
- 复用 `parse_hls_trace.py` 输出 JSONL。
- 终端输出 `capacity_threshold max_unknown=<n> min_ok=<n>`。
- 新增记录：
  `docs/planning/FPGA_CPIM_PHASE_C5_HLS_SWEEP_AUTOMATION.md`。

**验证**：
- `fpga_cpim/scripts/run_hls_trace_sweep.py --jsonl build/fpga_cpim/hls_trace_c5.jsonl --raw build/fpga_cpim/hls_trace_c5.out`
- `cmake --build build/fpga_cpim -j`
- `ctest --test-dir build/fpga_cpim --output-on-failure`
- `python3 -m py_compile fpga_cpim/scripts/*.py`
- `git diff --check`
- `dol lint --soft`

**结果摘要**：
- `capacity=272`：`UNKNOWN`，`router_overflow=1`。
- `capacity=273/274/320/384`：`OK`。
- 当前 fixture 的精确门槛为 `273`，与
  `queue_peak_partition=273` 对齐。
- `capacity=273` 恢复完整 work：`events=1586`、
  `deleted_values=16129`。

### FPGA CPIM：HLS Phase C.4 CLI / trace 转换 / capacity sweep

**目标**：给 HLS testbench 增加 `--pressure-only`、`--tiles` 和
`--capacity-sweep` 参数，把 `hls_pressure` / `hls_capacity` 行转换成
JSONL/CSV，并扫描 per-partition queue capacity 的 `UNKNOWN` 门槛。

**核心改动**：
- `testbench_hls.cpp` 新增轻量参数解析：
  `--pressure-only`、`--tiles=<csv>`、`--capacity-sweep=<csv>|none`。
- `hls_pressure` 行新增 `capacity` 字段。
- 新增 `hls_capacity` 输出，默认扫 `64/128/256/512/1024`。
- 新增 `fpga_cpim/scripts/parse_hls_trace.py`，支持
  `--format jsonl|csv` 与 `--kind all|pressure|capacity`。
- 新增记录：
  `docs/planning/FPGA_CPIM_PHASE_C4_HLS_CLI_TRACE.md`。

**验证**：
- `g++ -std=c++17 -I fpga_cpim/hls fpga_cpim/hls/testbench_hls.cpp fpga_cpim/hls/*.cpp -o /tmp/hls_tb_c4`
- `/tmp/hls_tb_c4 --pressure-only --tiles=1,2,4 --capacity-sweep=64,128,256,512,1024`
- `fpga_cpim/scripts/parse_hls_trace.py --input /tmp/hls_c4.out --format jsonl --output /tmp/hls_c4.jsonl`
- `fpga_cpim/scripts/parse_hls_trace.py --input /tmp/hls_c4.out --format csv --kind capacity --output /tmp/hls_c4_capacity.csv`
- `cmake --build build/fpga_cpim -j`
- `ctest --test-dir build/fpga_cpim --output-on-failure`
- `python3 -m py_compile fpga_cpim/scripts/*.py`
- `git diff --check`
- `dol lint --soft`

**结果摘要**：
- `tiles=1/2/4` 仍为 `status=OK`，`events=1586`、
  `deleted_values=16129`。
- epochs 为 `1586 -> 793 -> 397`。
- capacity sweep：`64/128/256` 为 `UNKNOWN` 且
  `router_overflow=1`；`512/1024` 为 `OK`。
- 当前 fixture 的 queue capacity 门槛落在 `256` 与 `512` 之间。

### FPGA CPIM：HLS Phase C.3 tile sweep 输出

**目标**：把 HLS-friendly pressure smoke 的
`events/epochs/queue_peak/cross_events` 变成稳定一行输出，并开始做
`1/2/4` revise tile 对照。

**核心改动**：
- `testbench_hls.cpp` 将 `random/vars=128/domain=128/density≈0.10`
  pressure case 抽成可重复 fixture。
- 同一 fixture 分别运行 `num_revise_tiles=1/2/4`。
- 新增稳定前缀 `hls_pressure`，输出 constraints、status、events、
  epochs、tile steps、queue peak、local/cross events、deleted values 与
  router overflow。
- 新增记录：
  `docs/planning/FPGA_CPIM_PHASE_C3_HLS_TILE_SWEEP.md`。

**验证**：
- `g++ -std=c++17 -I fpga_cpim/hls fpga_cpim/hls/testbench_hls.cpp fpga_cpim/hls/*.cpp -o /tmp/hls_tb_c3 && /tmp/hls_tb_c3`
- `cmake --build build/fpga_cpim -j`
- `ctest --test-dir build/fpga_cpim --output-on-failure`
- `python3 -m py_compile fpga_cpim/scripts/*.py`
- `git diff --check`
- `dol lint --soft`

**结果摘要**：
- 三种 tile 数均为 `status=OK`、`constraints=793`、`events=1586`、
  `deleted_values=16129`。
- epochs 随 tile 数增加下降：`1586 -> 793 -> 397`。
- queue peak 稳定：`queue_peak_total=592`、
  `queue_peak_partition=273`。
- local/cross event 稳定：`local_events=973`、`cross_events=613`。

### FPGA CPIM：HLS Phase C.2 多 tile / 分区队列

**目标**：承接 Phase B.4 的 `density=0.10` queue pressure 观察，把
HLS-friendly core 从单队列推进到 partition-aware queue fabric，并保留
`UNKNOWN` 的保守语义。

**核心改动**：
- `cpim_top_hls` 新增 `var_partition[MAX_VARS]` 与
  `constraint_partition[MAX_CONSTRAINTS]` 输入。
- `ControlHls` 新增 `num_partitions`、`num_revise_tiles`、
  `partition_queue_capacity`。
- `ResultHls` 新增 `epochs`、`tile_steps`、`local_events`、
  `cross_events`、`queue_peak_total`、`queue_peak_partition`、
  `router_overflow`。
- `event_router_hls.cpp` 新增 per-partition 环形 event queue。
- HLS testbench 新增多 revise tile round-robin、partition queue overflow
  UNKNOWN、`vars=128/domain=128/density≈0.10` pressure smoke。
- 新增记录：
  `docs/planning/FPGA_CPIM_PHASE_C2_HLS_DATAFLOW.md`。

**验证**：
- `cmake --build build/fpga_cpim -j`
- `ctest --test-dir build/fpga_cpim --output-on-failure`
- `g++ -std=c++17 -I fpga_cpim/hls fpga_cpim/hls/testbench_hls.cpp fpga_cpim/hls/*.cpp -o /tmp/hls_tb_c2 && /tmp/hls_tb_c2`
- `python3 -m py_compile fpga_cpim/scripts/*.py`
- `git diff --check`
- `dol lint --soft`

**结果摘要**：
- `ctest` 12/12 通过。
- 手工 HLS testbench 输出 `hls_tb ok`。
- overflow / budget 命中仍返回 `UNKNOWN`，不对外产生删除。
- 当前仍是 HLS-friendly 调度 smoke，不是周期精确 RTL timing。

## 2026-05-27

### Metal SAC v3.18：fusion rounds sweep

**目标**：继续收敛 bounded command-buffer fusion，扫描 `fusion_rounds=2/4/8`，
决定显式 bounded 路径先保留哪个段长作为均衡消融设置。

**核心改动**：
- 新增独立小计划/小 changelog：
  - `docs/planning/metal_gac/CPIM_METAL_METAL_SAC_V318_FUSION_ROUNDS_SWEEP_PLAN_2026_05_27.md`
  - `docs/planning/metal_gac/CPIM_METAL_METAL_SAC_V318_FUSION_ROUNDS_SWEEP_CHANGELOG_2026_05_27.md`
- `tests/python/metal_sac_ablation.py` 新增
  `--fusion-rounds-sweep=2,4,8`。
- `[fusion summary]` 现在按 `probe_fusion/fusion_rounds` 输出
  `dispatch_per_probe`、`command_buffer_per_probe`、`non_kernel_per_probe`
  的 avg/p50/p95。
- 默认仍为 `probe_fusion=none`；不接搜索，不移除 CPU-confirmed DWO guard。

**验证**：
- `python3 -m py_compile tests/python/metal_sac_ablation.py codex-docops-logic/scripts/dol.py`
- `python3 tests/python/metal_sac_ablation.py --suite=metal-smoke --runs=1 --warmup=0 --probe-limit=64 --sac-mode=nsacq --activation-mode=neighbor --max-sac-batches=30 --outer-queue-budget=256 --verify-probe-limit=32 --probe-fusion=bounded --fusion-rounds-sweep=2,4,8 --dwo-forensics --timeout=120 --csv=out/metal_sac_v318_fusion_sweep_smoke.csv --quiet`
- `python3 tests/python/metal_sac_ablation.py --tier=0 --runs=3 --warmup=0 --probe-limit=256 --sac-mode=nsacq --activation-mode=neighbor --max-sac-batches=30 --outer-queue-budget=256 --verify-probe-limit=32 --probe-fusion=bounded --fusion-rounds-sweep=2,4,8 --dwo-forensics --timeout=120 --csv=out/metal_sac_v318_fusion_sweep_tier0.csv --quiet`
- `python3 tests/python/metal_sac_ablation.py --tier=2 --runs=3 --warmup=0 --probe-limit=0 --sac-mode=nsacq --activation-mode=neighbor --max-sac-batches=10000 --outer-queue-budget=0 --verify-probe-limit=32 --probe-fusion=bounded --fusion-rounds-sweep=2,4,8 --dwo-forensics --timeout=300 --csv=out/metal_sac_v318_fusion_sweep_tier2.csv --quiet`

**结果摘要**：
- TIER2 sweep：684 OK rows，9 个历史 unsupported rows，865,743 probes，
  65,108 confirmed DWO，0 rejected DWO，0 UNKNOWN。
- `fusion_rounds=2`：non-kernel/probe avg `0.018152ms`，p95
  `0.026859ms`，wasted rounds `154`。
- `fusion_rounds=4`：non-kernel/probe avg `0.016337ms`，p95
  `0.021674ms`，wasted rounds `438`。
- `fusion_rounds=8`：non-kernel/probe avg `0.015251ms`，p95
  `0.023821ms`，wasted rounds `1398`。
- 结论：`8` 的 command-buffer/probe 更低，但 p95 non-kernel 与浪费轮次劣于
  `4`；当前保留 `fusion_rounds=4` 作为 balanced explicit bounded setting。

## 2026-05-24

### Metal SAC v3.18：DWO forensics / command fusion / NSACQ throughput

**目标**：根据 v3.17 host-side NSACQ evidence 和外部 review 更新 Metal
GAC/SAC 大计划。当前路线没有大方向偏移；单实例 Metal GAC 性能线冻结为
stable correctness fallback，新性能主线转向 Batch/SAC/NSACQ 吞吐化。

**核心改动**：
- 新增独立小计划/小 changelog：
  - `docs/planning/metal_gac/CPIM_METAL_METAL_SAC_V318_DWO_FORENSIC_FUSION_PLAN_2026_05_24.md`
  - `docs/planning/metal_gac/CPIM_METAL_METAL_SAC_V318_DWO_FORENSIC_FUSION_CHANGELOG_2026_05_24.md`
- 更新 `METAL_GAC_LONG_TERM_OPTIMIZATION.md`：
  - GAC-only 瓶颈明确为短 kernel 周围的 command-buffer / wait /
    non-kernel fixed cost；
  - NSACQ 瓶颈明确为 raw Metal DWO 可信度、CPU-confirm guard 成本、
    batch/queue 组织；
  - v3.18 三项优先实验固定为 `dwo_forensic_oracle`、
    `command_buffer_fusion`、`nsacq_batch_policy_sacq_compare`。
- 更新 `METAL_GAC_CHANGELOG.md` 与 `METAL_MIGRATION_PLAN.md` 导航。
- 第一段实现 benchmark-only DWO forensic counters：
  - `benchmark_metal_sac --dwo_forensics=true|false`
  - `MetalBatchProbeRunner` 可按需回拷 final world bit-domain/domain-size
  - CSV 新增 raw/confirmed/rejected DWO、raw precision、domain-size/popcount
    mismatch、rejected empty/nonempty domain、first rejected probe metadata
  - `metal_sac_ablation.py --dwo-forensics|--no-dwo-forensics`
- 第二段定位并修复一个 raw DWO 误报源：
  - DWO status debug words 记录 first status transition 的
    `var/cid/dir/old_size/deletion_count/round`
  - first rejected sample 指向 `sac_probe_init_kernel` 的 missing-value path
    (`status_cid=-1`)
  - init kernel 不再在同一 dispatch 内先复制 snapshot 再由 `cid==0` 覆盖
    singleton，而是直接从 immutable snapshot 生成每个 world 的 singleton domain
- 第三段实现 benchmark-only bounded command-buffer fusion：
  - `MetalRuntime::Dispatch1DBatch` 可在一个 command buffer 内顺序编码多个
    compute dispatch
  - `MetalBatchProbeOptions::probe_fusion=none|bounded` 与 `fusion_rounds`
    控制 SAC probe fusion，默认仍为 `none`
  - `benchmark_metal_sac --probe_fusion=none|bounded --fusion_rounds=<n>`
    与 `metal_sac_ablation.py --probe-fusion --fusion-rounds` 透传消融
  - 新增 `command_buffer_count`、`command_buffer_per_probe`、
    `fused_rounds_encoded`、`fused_rounds_wasted` 等 CSV 字段
- Roadmap 保持 `cpim-metal rm: v03`，不改变 solver 默认行为。

**验证**：
- `python3 -m py_compile codex-docops-logic/scripts/dol.py`
- `python3 -m py_compile tests/python/metal_sac_ablation.py codex-docops-logic/scripts/dol.py`
- `cmake --build build_metal --target benchmark_metal_sac -j8`
- `./build_metal/benchmark_metal_sac --input=tests/data/bench/queens-4_ext.xml --runs=1 --warmup=0 --probe_limit=16 --sac_mode=nsacq --activation_mode=neighbor --max_sac_batches=2 --outer_queue_budget=16 --max_probe_rounds=1000 --verify=true --verify_probe_limit=16 --dwo_forensics=true --csv=out/metal_sac_v318_dwo_forensics_queens4.csv`
- `python3 tests/python/metal_sac_ablation.py --instances tests/data/bench/queens-4_ext.xml --runs=1 --warmup=0 --probe-limit=16 --sac-mode=nsacq --activation-mode=neighbor --max-sac-batches=2 --outer-queue-budget=16 --max-probe-rounds=1000 --verify-probe-limit=16 --dwo-forensics --timeout=60 --csv=out/metal_sac_v318_dwo_forensics_ablation_queens4.csv`
- `python3 tests/python/metal_sac_ablation.py --tier=0 --runs=3 --warmup=0 --probe-limit=256 --sac-mode=nsacq --activation-mode=neighbor --max-sac-batches=30 --outer-queue-budget=256 --max-probe-rounds=10000 --verify-probe-limit=32 --dwo-forensics --timeout=120 --csv=out/metal_sac_v318_forensics_tier0_fixed_r3.csv --quiet`
- `./build_metal/benchmark_metal_sac --input=benchmarks/driver/driverlogw-01c-sat_ext.xml --runs=3 --warmup=0 --probe_limit=256 --sac_mode=nsacq --activation_mode=neighbor --max_sac_batches=30 --outer_queue_budget=256 --max_probe_rounds=10000 --verify=true --verify_probe_limit=32 --dwo_forensics=true --csv=out/metal_sac_v318_forensics_driver_fixed.csv`
- `./build_metal/benchmark_metal_sac --input=tests/data/bench/queens-4_ext.xml --runs=1 --warmup=0 --probe_limit=16 --sac_mode=nsacq --verify=true --verify_probe_limit=16 --probe_fusion=none --csv=out/metal_sac_v318_fusion_none_smoke.csv`
- `./build_metal/benchmark_metal_sac --input=tests/data/bench/queens-4_ext.xml --runs=3 --warmup=0 --probe_limit=64 --sac_mode=nsacq --verify=true --verify_probe_limit=64 --probe_fusion=bounded --fusion_rounds=4 --dwo_forensics=true --csv=out/metal_sac_v318_fusion_bounded_smoke.csv`
- `python3 tests/python/metal_sac_ablation.py --tier=0 --runs=3 --warmup=0 --probe-limit=256 --sac-mode=nsacq --activation-mode=neighbor --max-sac-batches=30 --outer-queue-budget=256 --verify-probe-limit=32 --probe-fusion=bounded --fusion-rounds=4 --dwo-forensics --timeout=120 --csv=out/metal_sac_v318_fusion_tier0_bounded.csv --quiet`
- `python3 tests/python/metal_sac_ablation.py --tier=2 --runs=3 --warmup=0 --probe-limit=0 --sac-mode=nsacq --activation-mode=neighbor --max-sac-batches=10000 --outer-queue-budget=0 --verify-probe-limit=32 --probe-fusion=bounded --fusion-rounds=4 --dwo-forensics --timeout=300 --csv=out/metal_sac_v318_fusion_tier2_bounded.csv --quiet`
- `python3 tests/python/metal_sac_ablation.py --tier=2 --runs=3 --warmup=0 --probe-limit=0 --sac-mode=nsacq --activation-mode=neighbor --max-sac-batches=10000 --outer-queue-budget=0 --verify-probe-limit=32 --probe-fusion=none --fusion-rounds=4 --dwo-forensics --timeout=300 --csv=out/metal_sac_v318_fusion_tier2_none.csv --quiet`
- `git diff --check`
- `python3 codex-docops-logic/scripts/dol.py lint --soft`

**结果摘要**：
- 本轮第一段为 benchmark-only instrumentation，不修改 Metal solver 默认行为、GAC
  `auto`、v3.14 allowlist、CUDA path 或 search integration。
- 修复后 limited TIER0 NSACQ forensics：36 rows，852 raw DWO 全部
  confirmed，0 rejected；driver 定点 3 runs：12 raw DWO 全部 confirmed，0
  rejected。
- full TIER2 NSACQ forensics：228 OK rows，3 个历史
  `unsupported_non_binary_extension`，286,744 probes，19,885 raw DWO 全部
  confirmed，0 rejected DWO，0 UNKNOWN。
- v3.18 command fusion TIER2 对照：
  - `probe_fusion=none`：228 OK rows，3 个历史 unsupported，291,297 probes，
    24,402 confirmed DWO，0 rejected DWO，0 UNKNOWN，
    avg command-buffer/probe `0.0528`，avg non-kernel/probe `0.0310ms`
  - `probe_fusion=bounded`：228 OK rows，3 个历史 unsupported，289,657 probes，
    22,777 confirmed DWO，0 rejected DWO，0 UNKNOWN，
    avg command-buffer/probe `0.0076`，avg non-kernel/probe `0.0198ms`
- v3.18 明确：在 v3.17 的 `4,970` rejected unconfirmed Metal DWO 被解释前，
  raw Metal DWO 不能 promote，CPU-confirmed DWO guard 保持必要。

## 2026-05-09

### Metal SAC v3.17：host-side NSACQ queue

**目标**：承接 v3.16 batch probe evidence，把 benchmark-only probe 扩展成
default-off 的 host-side NSACQ 原型，验证 DWO probe 能否安全回写主 domain，并在
每批删除后重新运行 stable Metal GAC。

**核心改动**：
- 新增 `MetalSacMode { batch_probe, nsacq, sacq_adj, sacq_full }`。
- `MetalBatchProbeOptions` 新增 `allowed_constraints` mask。
- Metal SAC kernels 的 init、revise、subscription enqueue 均尊重 allowed
  constraint mask。
- `benchmark_metal_sac` 新增：
  - `--sac_mode=batch_probe|nsacq|sacq_adj|sacq_full`
  - `--max_sac_batches`
  - `--outer_queue_budget`
  - host-side remaining-value queue
  - DWO writeback 到 host snapshot
  - delete batch 后 stable Metal GAC rerun
  - NSACQ queue/delete/GAC rerun CSV 字段
- `tests/python/metal_sac_ablation.py` 新增 `--sac-mode`、
  `--max-sac-batches`、`--outer-queue-budget` 并透传新 CSV 字段。
- 新增独立小计划/小 changelog：
  - `docs/planning/metal_gac/CPIM_METAL_METAL_SAC_V317_HOST_NSACQ_PLAN_2026_05_09.md`
  - `docs/planning/metal_gac/CPIM_METAL_METAL_SAC_V317_HOST_NSACQ_CHANGELOG_2026_05_09.md`

**验证**：
- `cmake -S . -B build_metal`
- `cmake --build build_metal --target benchmark_metal_sac -j8`
- `cmake --build build_metal --target benchmark_metal_gac -j8`
- `python3 -m py_compile tests/python/metal_sac_ablation.py codex-docops-logic/scripts/dol.py`
- v3.16 regression 单例：2 probes，CPU/Metal status verify 通过。
- NSACQ 单例：2 probes，CPU/Metal status verify 通过。
- `queens-4` NSACQ：24 probes，16 OK，8 DWO，0 UNKNOWN；8 values written
  back；1 次 post-delete stable Metal GAC；CPU/Metal status verify 通过。
- metal-smoke NSACQ：5/5 OK，178 probes，8 batches，10 DWO values written
  back，0 rejected DWO。
- BH NSACQ smoke：384 probes/run，3 measured runs，0 DWO，0 UNKNOWN，
  `dispatch_per_probe=0.0182292`，CPU/Metal verify 通过。
- TIER2 NSACQ：228/231 OK rows，3 个 ERROR 均为历史
  `unsupported_non_binary_extension`；278,172 probes，0 UNKNOWN，
  12,094 confirmed DWO writeback，4,970 rejected unconfirmed Metal DWO。

**结果摘要**：
- v3.17 已覆盖完整 benchmark-only NSACQ 回路：probe -> DWO writeback ->
  post-delete GAC -> requeue。
- 默认仍是 `sac_mode=batch_probe`；不影响 `benchmark_metal_gac`、GAC
  `frontier_mode=auto`、v3.14 allowlist 或 CUDA。
- TIER2 说明 CPU guard 是必要的：raw Metal DWO status 在部分实例上会比 CPU
  reference 更激进，因此 v3.17 只能保持 report-only；若继续推进 SAC preprocess，
  下一步应先做 deterministic/double-buffer probe，或把 CPU-confirmed DWO guard
  纳入 promote gate。

## 2026-05-04

### Metal Batch/SAC v3.16：batch probe benchmark MVP

**目标**：先做 Metal-first、default-off 的 Batch/SAC singleton probe
benchmark，验证多 world probe 是否能摊薄 v3.15 观察到的 command buffer
non-kernel 固定成本。

**核心改动**：
- 新增 `MetalSacProbeStatus { ok, dwo, unknown }`、
  `MetalSacActivationMode { neighbor, full }`、
  `MetalSacProbeTask`、`MetalSacBudget` 与 `MetalBatchProbeStats`。
- 新增 `MetalBatchProbeRunner`：
  - 输入 stable Metal GAC 的 AC snapshot；
  - 从 snapshot remaining values 生成多 world singleton probes；
  - 每个 world 独立 domain/frontier/status；
  - `UNKNOWN` 仅表示预算超限，不产生删值。
- 新增 Metal kernels：
  - `sac_probe_init_kernel`
  - `sac_probe_revise_kernel`
  - `sac_probe_frontier_kernel`
  - `sac_probe_mark_unknown_kernel`
- 新增 benchmark app `benchmark_metal_sac`，CSV 与 Metal GAC 分离。
- 新增批量脚本 `tests/python/metal_sac_ablation.py`。
- 新增独立小计划/小 changelog：
  - `docs/planning/metal_gac/CPIM_METAL_METAL_BATCH_SAC_V316_BATCH_PROBE_PLAN_2026_05_04.md`
  - `docs/planning/metal_gac/CPIM_METAL_METAL_BATCH_SAC_V316_BATCH_PROBE_CHANGELOG_2026_05_04.md`

**验证**：
- `python3 -m py_compile tests/python/metal_sac_ablation.py codex-docops-logic/scripts/dol.py`
- `cmake -S . -B build_metal`
- `cmake --build build_metal --target benchmark_metal_sac -j8`
- 单例 `gac_bitwords2.xml`：2 probes，CPU/Metal probe status verify 通过。
- BH smoke：384 probes/run，3 measured runs，CPU/Metal verify 通过。
- metal-smoke：15/15 OK rows。
- TIER2：380/383 OK rows，3 个 ERROR 均为历史
  `unsupported_non_binary_extension`；supported rows `verify_mismatches=0`，
  `UNKNOWN=0`。

**结果摘要**：
- BH smoke：
  - `dispatch_per_probe=0.0182292`
  - `non_kernel_per_probe≈0.0037-0.0039ms`
  - `probes_per_sec≈195k-202k`
- metal-smoke 小例子：
  - `avg_dispatch_per_probe=0.9536`
  - `avg_non_kernel_per_probe=0.370296ms`
- TIER2：
  - total measured probes：163,605
  - `dispatch_per_probe avg=0.0527 p50=0.0182 p95=0.1402`
  - `non_kernel_per_probe avg=0.014634ms p50=0.005242ms p95=0.023366ms`
  - `probes_per_sec avg≈144k p50≈146k p95≈255k`
- 结论：TIER2 与 BH bucket 均显示 batch singleton probe 能明显摊薄 v3.15
  观察到的 command-buffer fixed cost；v3.16 仍保持 benchmark-only。若继续推进，
  下一步进入 v3.17 host-side NSACQ、queue budget 与 DWO writeback。

### Metal GAC v3.15：dispatch timing split

**目标**：拆开 Metal command buffer 的 encode / wait / kernel /
non-kernel 时间，判断当前 GAC 性能瓶颈是 CPU encode 还是短 kernel 周围的
dispatch/sync 固定成本。

**核心改动**：
- `MetalDispatchTimings` 新增 `encode_ms`。
- `MetalGacStats` / benchmark CSV 新增：
  - `dispatch_encode_ms`
  - `dispatch_wait_ms`
  - `dispatch_non_kernel_ms`
- `metal_gac_ablation.py` 透传新字段。
- `metal_gac_analyze.py` 在 mode summary 与新增 `[dispatch timing split]`
  中显示 encode/wait/kernel/non-kernel 及 per-dispatch 成本。
- v3.14 runtime policy summary 增加 eligible/report-only 决策行。
- 新增独立小计划/小 changelog：
  - `docs/planning/metal_gac/CPIM_METAL_METAL_GAC_V315_DISPATCH_TIMING_SPLIT_PLAN_2026_05_04.md`
  - `docs/planning/metal_gac/CPIM_METAL_METAL_GAC_V315_DISPATCH_TIMING_SPLIT_CHANGELOG_2026_05_04.md`

**验证**：
- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py codex-docops-logic/scripts/dol.py`
- `cmake --build build_metal --target benchmark_metal_gac -j8`
- 单例 `gac_bitwords2.xml` CPU/Metal verify 通过。
- TIER2 `out/metal_gac_v315_dispatch_timing_split_tier2.csv`：380/383 OK，
  3 个 ERROR 均为历史 `unsupported_non_binary_extension`。

**结果摘要**：
- shared+flags baseline：`solve_ms p50=0.236 p95=5.403`。
- dispatch split：`encode=0.013ms`、`wait=0.773ms`、
  `kernel=0.105ms`、`non_kernel=0.668ms`。
- per dispatch：`encode=0.0039ms`、`wait=0.2356ms`、
  `kernel=0.0321ms`、`non_kernel=0.2035ms`。
- `non_kernel_share=0.86`。
- 结论：瓶颈不是 CPU encode，而是短 kernel 周围的 command buffer wait /
  non-kernel 固定成本；下一步优先考虑 Batch/SAC 吞吐或能摊薄 dispatch 的方案。

### Metal GAC v3.14：runtime bucket allowlist

**目标**：把 v3.13 analyzer-only BH bucket recommendation 落成 default-off
runtime policy，不改变 `frontier_mode=auto` 或稳定 fallback。

**核心改动**：
- benchmark 新增 `--policy_mode=none|bh_cta_allowlist`。
- 命中 `BH-4-4 cons=128-511 dom<17 bitw<2` 时自动选择 CTA hybrid8：
  `cta_worklist + word_parallel + directional + vebo_weighted +
  bounded_replay + dirty_var_pull + local=16 + replay=8 + dirty_min=8`。
- 未命中 allowlist 时保留用户请求路径。
- CSV / ablation / analyzer 新增 policy 字段与 runtime policy summary：
  `policy_mode`、`policy_selected`、`policy_reason`、`policy_bucket`。
- 新增独立小计划/小 changelog：
  - `docs/planning/metal_gac/CPIM_METAL_METAL_GAC_V314_RUNTIME_BUCKET_ALLOWLIST_PLAN_2026_05_04.md`
  - `docs/planning/metal_gac/CPIM_METAL_METAL_GAC_V314_RUNTIME_BUCKET_ALLOWLIST_CHANGELOG_2026_05_04.md`

**验证**：
- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py codex-docops-logic/scripts/dol.py`
- `cmake --build build_metal --target benchmark_metal_gac -j8`
- 单例 BH / fallback 均 CPU/Metal verify 通过。
- TIER2 `out/metal_gac_v314_runtime_bucket_allowlist_tier2.csv`：380/383 OK，
  3 个 ERROR 均为历史 `unsupported_non_binary_extension`。

**结果摘要**：
- selected inputs：4/76。
- selected rows：20。
- selected-vs-shared+flags：`p50=0.62x p95=0.68x`。
- better：4/4。
- regressions over 1.05：0。
- analyzer decision：`eligible`。
- 结论：CTA hybrid8 可以作为 BH-like default-off runtime allowlist probe；
  仍不进入全局 `auto`。

### Metal GAC v3.13：bucket policy simulation

**目标**：在 v3.12 显示 CTA dirty hybrid 只对少数 bucket 有强信号后，先做
analyzer report-only policy simulation，不改变 runtime `auto`。

**核心改动**：
- `metal_gac_analyze.py --recommend-policy` 新增 `[bucket policy simulation]`。
- 新增完整候选路径分组：
  - non-CTA 区分 storage/frontier/kernel/bitsup/reset；
  - CTA 额外区分 owner/queue/handoff/local/replay/dirty threshold。
- 新增 `--bucket-min-instances`，默认 3。
- 只有 bucket 内候选路径 `p95 <= 1.0x` 且 regression rows 为 0 时才进入
  eligible；其它输入回退 fallback。
- 新增独立小计划/小 changelog：
  - `docs/planning/metal_gac/METAL_GAC_V313_BUCKET_POLICY_SIMULATION_PLAN_2026_05_04.md`
  - `docs/planning/metal_gac/METAL_GAC_V313_BUCKET_POLICY_SIMULATION_CHANGELOG_2026_05_04.md`

**验证**：
- `python3 -m py_compile tests/python/metal_gac_analyze.py tests/python/metal_gac_ablation.py codex-docops-logic/scripts/dol.py`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v35_tier2_baseline.csv out/metal_gac_v35_tier2_frontier.csv out/metal_gac_v39_vebo_weighted_local16_tier2.csv out/metal_gac_v311_dirty_var_pull_tier2.csv out/metal_gac_v312_dirty_pull_hybrid8_tier2.csv --top=10 --recommend-policy --baseline-mode shared+flags --regression-threshold 1.05 --min-runs 3 --bucket-min-instances 3`

**结果摘要**：
- eligible bucket：1。
- selected inputs：4/76。
- policy：`p50=0.312ms p95=1.696ms`。
- fallback：`p50=0.312ms p95=2.987ms`。
- `regressions_gt_threshold=0`。
- 唯一 eligible bucket 为 `BH-4-4` feature bucket，推荐 CTA hybrid8 path，
  bucket 内 `p50=0.33x p95=0.34x`。
- 结论：CTA 不适合全局默认，但适合少数 allowlist bucket；本轮只做
  report-only simulation，不改 `auto`。

### Metal GAC v3.12：`dirty_pull_hybrid`

**目标**：在 v3.11 `dirty_var_pull` 小幅改善 CTA 但仍未过 gate 后，评估
dirty pull 是否应只作用于高度变量，低度变量继续 direct push。

**核心改动**：
- 新增 `MetalGacOptions::cta_dirty_pull_min_degree`。
- 新增 benchmark flag `--cta_dirty_pull_min_degree=<N>`；默认 0，完全复现 v3.11。
- CTA kernel 在 `dirty_var_pull` 下按目标变量 subscription degree 决策：
  - `degree >= N`：标记 dirty var；
  - `degree < N`：回退 direct global push。
- 新增 `dirty_pull_fallback_push_count`。
- `metal_gac_ablation.py` 新增 `--cta-dirty-pull-min-degree`。
- `metal_gac_analyze.py` 在 CTA gate 中按 dirty pull threshold 分组，并显示
  fallback push。
- 新增独立小计划/小 changelog：
  - `docs/planning/metal_gac/METAL_GAC_V312_DIRTY_PULL_HYBRID_PLAN_2026_05_04.md`
  - `docs/planning/metal_gac/METAL_GAC_V312_DIRTY_PULL_HYBRID_CHANGELOG_2026_05_04.md`

**验证**：
- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py codex-docops-logic/scripts/dol.py`
- `cmake --build build_metal --target benchmark_metal_gac -j8`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=1 --warmup=0 --verify=true --runner_mode=prepared --frontier_mode=cta_worklist --kernel_variant=word_parallel --bitsup_layout=directional --cta_owner_mode=vebo_weighted --cta_queue_mode=bounded_replay --cta_local_round_budget=16 --cta_replay_round_budget=8 --cta_handoff_mode=dirty_var_pull --cta_dirty_pull_min_degree=16 --csv=out/metal_gac_v312_dirty_pull_hybrid16_single.csv`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=cta --runs=3 --warmup=1 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=vebo_weighted --cta-queue-mode=bounded_replay --cta-local-round-budget=16 --cta-replay-round-budget=8 --cta-handoff-mode=dirty_var_pull --cta-dirty-pull-min-degree=16 --cpu-timing --timeout=60 --csv=out/metal_gac_v312_dirty_pull_hybrid16_smoke.csv`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=cta --runs=5 --warmup=2 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=vebo_weighted --cta-queue-mode=bounded_replay --cta-local-round-budget=16 --cta-replay-round-budget=8 --cta-handoff-mode=dirty_var_pull --cta-dirty-pull-min-degree=8 --cpu-timing --timeout=300 --csv=out/metal_gac_v312_dirty_pull_hybrid8_tier2.csv --quiet`

**结果摘要**：
- hybrid16 single / smoke correctness 通过。
- hybrid16 TIER2：380/383 OK，`solve_ms p50=0.576 p95=2.723`。
- hybrid8 TIER2：380/383 OK，`solve_ms avg=0.837 p50=0.488 p95=2.192`。
- 相对 v3.11 dirty-all：hybrid8 `p50 0.470 -> 0.488` 略慢，
  `p95 3.202 -> 2.192` 明显改善。
- 相对 v3.9 push：hybrid8 `p50 0.513 -> 0.488`，
  `p95 3.357 -> 2.192`。
- hybrid8 gate 仍未通过：`cta_vs_shared+flags p50=1.41x p95=3.21x`，
  `decision=report_only`。
- `BH-4-4` bucket 继续强信号：`p50_ratio=0.40 p95_ratio=0.43`，
  `regressions_gt_threshold=0`。
- 结论：hybrid 能压 CTA 尾部，但不解决全局 promote；保留 report-only，
  后续只适合做 BH-like bucket policy 或转向 dispatch/Batch/SAC。

### Metal GAC v3.11：`dirty_var_pull`

**目标**：在 v3.10 `bulk_sync_mask` 因双 dispatch 成本未通过 gate 后，
回到 CTA worklist，评估跨 owner push 是否可以由 dirty-var pull 替代。

**核心改动**：
- 新增 `MetalCtaHandoffMode { push_constraints, dirty_var_pull }`。
- 新增 benchmark flag `--cta_handoff_mode=push_constraints|dirty_var_pull`；
  默认 `push_constraints`，仅 CTA worklist 实验路径读取。
- CTA kernel 在 `dirty_var_pull` 下对跨 owner subscription 标记 dirty var，
  不直接写 global next active；same-owner subscription 仍进入 CTA local queue。
- Host 在 CTA dispatch 后扫描 dirty vars 的 subscriptions，重建下一轮 frontier。
- 新增 dirty stats：
  - `dirty_var_count`
  - `dirty_pull_scan_count`
  - `dirty_pull_hit_count`
  - `cross_push_avoided_count`
- `metal_gac_ablation.py` 新增 `--cta-handoff-mode`。
- `metal_gac_analyze.py` 在 mode summary / CTA gate 中纳入 handoff mode 与 dirty
  stats。
- 新增独立小计划/小 changelog：
  - `docs/planning/metal_gac/METAL_GAC_V311_DIRTY_VAR_PULL_PLAN_2026_05_04.md`
  - `docs/planning/metal_gac/METAL_GAC_V311_DIRTY_VAR_PULL_CHANGELOG_2026_05_04.md`

**验证**：
- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py codex-docops-logic/scripts/dol.py`
- `cmake --build build_metal --target benchmark_metal_gac -j8`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=1 --warmup=0 --verify=true --runner_mode=prepared --frontier_mode=cta_worklist --kernel_variant=word_parallel --bitsup_layout=directional --cta_owner_mode=vebo_weighted --cta_queue_mode=bounded_replay --cta_local_round_budget=16 --cta_replay_round_budget=8 --cta_handoff_mode=dirty_var_pull --csv=out/metal_gac_v311_dirty_var_pull_single.csv`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=cta --runs=3 --warmup=1 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=vebo_weighted --cta-queue-mode=bounded_replay --cta-local-round-budget=16 --cta-replay-round-budget=8 --cta-handoff-mode=dirty_var_pull --cpu-timing --timeout=60 --csv=out/metal_gac_v311_dirty_var_pull_smoke.csv`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=cta --runs=5 --warmup=2 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=vebo_weighted --cta-queue-mode=bounded_replay --cta-local-round-budget=16 --cta-replay-round-budget=8 --cta-handoff-mode=dirty_var_pull --cpu-timing --timeout=300 --csv=out/metal_gac_v311_dirty_var_pull_tier2.csv --quiet`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v35_tier2_baseline.csv out/metal_gac_v35_tier2_frontier.csv out/metal_gac_v39_vebo_weighted_local16_tier2.csv out/metal_gac_v310_bulk_sync_mask_tier2.csv out/metal_gac_v311_dirty_var_pull_tier2.csv --top=10 --recommend-policy --baseline-mode shared+flags --regression-threshold 1.05 --min-runs 3`

**结果摘要**：
- single smoke：CPU verify 通过，`deletions=78`。
- metal-smoke：15/15 OK，`avg_solve_ms=0.448`。
- TIER2：380/383 OK，3 个 ERROR 均为历史 `unsupported_non_binary_extension`。
- v3.11 absolute：`solve_ms p50=0.470 p95=3.202`。
- 相对 v3.9 `vebo_weighted local=16 replay=8`：`p50 0.513 -> 0.470`，
  `p95 3.357 -> 3.202`，45/76 个实例更快。
- Combined gate：`cta_vs_shared+flags p50=1.31x p95=3.00x`，
  `cta_vs_best_worklist p50=1.48x p95=2.98x`，
  `host_round_ratio_vs_baseline p50=1.00x`，`decision=report_only`。
- 结论：`dirty_var_pull` correctness 成立并小幅改善 CTA，但仍未通过 promote
  gate；保持 default-off，`auto` 不读取。

### Metal GAC v3.10：`bulk_sync_deletion_mask`

**目标**：从 CTA owner 路线切到 default-off `bulk_sync_mask`，评估大例子 /
传播重例子是否能从 bulk-synchronous deletion mask 受益。

**核心改动**：
- 新增 `frontier_mode=bulk_sync_mask`，默认仍为 `flags`，`auto` 不读取新路径。
- 新增两阶段 Metal kernel：
  - `gac_revise_bulk_mask_kernel` 只计算 `delete_masks[var][word]`；
  - `gac_apply_bulk_mask_kernel` 统一 apply deletion mask，更新 domain 并生成下一轮
    frontier。
- 新增 bulk stats：
  - `bulk_mask_proposed_deletion_count`
  - `bulk_mask_actual_deletion_count`
  - `bulk_mask_changed_word_count`
  - `bulk_mask_frontier_push_count`
  - `bulk_mask_rounds`
- `metal_gac_ablation.py` 新增 `--mode-preset=bulk_sync`。
- `metal_gac_analyze.py` 新增 `[bulk sync mask gate]`，并拆出 `large_any` /
  `large_prop` gate。
- Tensor-core-like 约束检查不进入 v3.10 主线；当前参考 CUDA 路线仍是 bitset、
  warp/subwarp 与 shared packing，后续若要评估 packed8/tile 需另开 microbench。
- 新增独立小计划/小 changelog：
  - `docs/planning/metal_gac/METAL_GAC_V310_BULK_SYNC_MASK_PLAN_2026_05_04.md`
  - `docs/planning/metal_gac/METAL_GAC_V310_BULK_SYNC_MASK_CHANGELOG_2026_05_04.md`

**验证**：
- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py codex-docops-logic/scripts/dol.py`
- `cmake --build build_metal --target benchmark_metal_gac -j8`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=1 --warmup=0 --verify=true --runner_mode=prepared --frontier_mode=bulk_sync_mask --kernel_variant=word_parallel --bitsup_layout=directional --csv=out/metal_gac_v310_bulk_sync_mask_single.csv`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=bulk_sync --runs=3 --warmup=1 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cpu-timing --timeout=60 --csv=out/metal_gac_v310_bulk_sync_mask_smoke.csv`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=bulk_sync --runs=5 --warmup=2 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cpu-timing --timeout=300 --csv=out/metal_gac_v310_bulk_sync_mask_tier2.csv --quiet`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v35_tier2_baseline.csv out/metal_gac_v35_tier2_frontier.csv out/metal_gac_v39_vebo_weighted_local16_tier2.csv out/metal_gac_v310_bulk_sync_mask_tier2.csv --top=10 --recommend-policy --baseline-mode shared+flags --regression-threshold 1.05 --min-runs 3`

**结果摘要**：
- single smoke：CPU verify 通过；`deletions=78`，
  `bulk_mask_proposed_deletion_count=78`，
  `bulk_mask_actual_deletion_count=78`。
- metal-smoke：15/15 OK，`avg_solve_ms=0.694`。
- TIER2：380/383 OK，3 个 ERROR 均为历史 `unsupported_non_binary_extension`。
- TIER2 absolute：`solve_ms p50=0.712 p95=10.474`。
- Combined gate：
  - `bulk_vs_shared+flags p50=1.86x p95=3.17x`；
  - `large_any p50=1.87x p95=2.98x`；
  - `large_prop p50=1.94x p95=2.76x`；
  - `dispatch_ratio_vs_baseline p50=2.00x p95=2.04x`；
  - `actual_deletion_mismatch_rows=0`。
- 结论：correctness 成立，但 bulk path 因 revise/apply 双 dispatch 成本未通过
  全局或大例子 promote gate，继续保持 report-only，不进入 `auto`。

### Metal GAC v3.9：`vebo_weighted_owner`

**目标**：在 v3.8 确认 seed/overflow/budget 协议不是主因后，进入
`vebo_weighted_owner`，评估 weighted owner load 是否能改善 CTA worklist p95。

**核心改动**：
- 新增 `--cta_owner_mode=vebo_weighted`，默认仍为 `modulo`。
- Host owner map 新增 `BuildVeboWeightedOwnerMap()`：
  - variable degree 降序生成 VEBO 风格遍历顺序；
  - constraint weight 使用 `bit_words * (degree(x) + degree(y))`；
  - soft count/weight 内优先保留邻接 locality，再按 owner weighted load、
    owner count、owner id 兜底。
- 新增 `owner_weight_balance_p95`，用于观察 weighted owner load skew。
- `metal_gac_ablation.py` / `metal_gac_analyze.py` 支持
  `cta_owner_mode=vebo_weighted` 与新 owner weight balance 字段。
- 新增独立小计划/小 changelog：
  - `docs/planning/metal_gac/METAL_GAC_V39_VEBO_WEIGHTED_OWNER_PLAN_2026_05_04.md`
  - `docs/planning/metal_gac/METAL_GAC_V39_VEBO_WEIGHTED_OWNER_CHANGELOG_2026_05_04.md`

**验证**：
- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py codex-docops-logic/scripts/dol.py`
- `cmake --build build_metal --target benchmark_metal_gac -j8`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=1 --warmup=0 --verify=true --runner_mode=prepared --frontier_mode=cta_worklist --kernel_variant=word_parallel --bitsup_layout=directional --cta_owner_mode=vebo_weighted --cta_queue_mode=bounded_replay --cta_local_round_budget=8 --cta_replay_round_budget=8 --csv=out/metal_gac_v39_vebo_weighted_single.csv`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=cta --runs=3 --warmup=1 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=vebo_weighted --cta-queue-mode=bounded_replay --cta-local-round-budget=8 --cta-replay-round-budget=8 --cpu-timing --timeout=60 --csv=out/metal_gac_v39_vebo_weighted_smoke.csv`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=cta --runs=5 --warmup=2 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=vebo_weighted --cta-queue-mode=bounded_replay --cta-local-round-budget=8 --cta-replay-round-budget=8 --cpu-timing --timeout=300 --csv=out/metal_gac_v39_vebo_weighted_tier2.csv --quiet`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=cta --runs=5 --warmup=2 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=vebo_weighted --cta-queue-mode=bounded_replay --cta-local-round-budget=16 --cta-replay-round-budget=8 --cpu-timing --timeout=300 --csv=out/metal_gac_v39_vebo_weighted_local16_tier2.csv --quiet`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v35_tier2_baseline.csv out/metal_gac_v35_tier2_frontier.csv out/metal_gac_v38_bounded_replay_tier2.csv out/metal_gac_v39_vebo_weighted_tier2.csv out/metal_gac_v39_vebo_weighted_local16_tier2.csv --top=10 --recommend-policy --baseline-mode shared+flags --regression-threshold 1.05 --min-runs 3`

**结果摘要**：
- single smoke：CPU verify 通过。
- metal-smoke：15/15 OK。
- TIER2：380/383 OK，3 个 ERROR 均为历史 `unsupported_non_binary_extension`。
- `vebo_weighted local=16 replay=8`：
  - `solve_ms p50=0.513 p95=3.325`；
  - `cta_vs_shared+flags p50=1.41x p95=3.14x`；
  - `cta_vs_best_worklist p50=1.40x p95=3.13x`；
  - `owner_balance_p95_avg=1.28`；
  - `owner_weight_balance_p95_avg=1.22`；
  - `budget_spill_p95=0`；
  - `host_round_ratio_vs_baseline p50=1.00x`；
  - `decision=report_only`。
- 结论：`vebo_weighted_owner` 比 v3.8 owner map 更有信号，但仍未通过 auto
  promote gate。若继续 CTA，应转向 owner locality/cross-push hybrid 调优。

## 2026-05-03

### Metal GAC v3.8：local budget / bounded replay 评估

**目标**：在进入 `vebo_weighted_owner` 前，先确认 v3.7 的
`cta_budget_spill_count` 是否能通过增加 local budget 或 bounded replay 解决。

**核心改动**：
- 新增 `--cta_queue_mode=bounded_replay`，默认仍为 `local_only`。
- 新增 `--cta_local_round_budget`，默认 8。
- 新增 `--cta_replay_round_budget`，默认 8，仅 `bounded_replay` 生效。
- 新增 replay stats：
  - `cta_budget_replay_rounds`
  - `cta_budget_replay_drain_count`
  - `cta_budget_replay_spill_count`
- analyzer 按 owner、queue mode、local budget、replay budget 拆分 CTA gate。
- 新增独立小计划/小 changelog：
  - `docs/planning/metal_gac/METAL_GAC_V38_BUDGET_REPLAY_PLAN_2026_05_03.md`
  - `docs/planning/metal_gac/METAL_GAC_V38_BUDGET_REPLAY_CHANGELOG_2026_05_03.md`

**验证**：
- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py codex-docops-logic/scripts/dol.py`
- `cmake --build build_metal --target benchmark_metal_gac -j8`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=1 --warmup=0 --verify=true --runner_mode=prepared --frontier_mode=cta_worklist --kernel_variant=word_parallel --bitsup_layout=directional --cta_owner_mode=static_edge_cut --cta_queue_mode=bounded_replay --cta_local_round_budget=8 --cta_replay_round_budget=8 --csv=out/metal_gac_v38_bounded_replay_single.csv`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=cta --runs=3 --warmup=1 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=static_edge_cut --cta-queue-mode=bounded_replay --cta-local-round-budget=8 --cta-replay-round-budget=8 --cpu-timing --timeout=60 --csv=out/metal_gac_v38_bounded_replay_smoke.csv`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=cta --runs=5 --warmup=2 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=static_edge_cut --cta-queue-mode=bounded_replay --cta-local-round-budget=8 --cta-replay-round-budget=8 --cpu-timing --timeout=300 --csv=out/metal_gac_v38_bounded_replay_tier2.csv --quiet`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=cta --runs=5 --warmup=2 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=static_edge_cut --cta-queue-mode=spill_replay --cta-local-round-budget=16 --cta-replay-round-budget=0 --cpu-timing --timeout=300 --csv=out/metal_gac_v38_local_budget16_tier2.csv --quiet`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v35_tier2_baseline.csv out/metal_gac_v35_tier2_frontier.csv out/metal_gac_v36_owner_map_static_tier2.csv out/metal_gac_v37_seed_overflow_tier2.csv out/metal_gac_v38_bounded_replay_tier2.csv out/metal_gac_v38_local_budget16_tier2.csv --top=10 --recommend-policy --baseline-mode shared+flags --regression-threshold 1.05 --min-runs 3`

**结果摘要**：
- single smoke：CPU verify 通过。
- metal-smoke：15/15 OK。
- `bounded_replay local=8 replay=8` TIER2：380/383 OK；
  `budget_spill_p95=0`，`replay_drain_p95=1`，
  `cta_vs_shared+flags p50=1.36x p95=3.49x`，
  `host_round_ratio_vs_baseline p50=1.00x`，`decision=report_only`。
- `spill_replay local=16` TIER2：380/383 OK；
  `budget_spill_p95=0`，
  `cta_vs_shared+flags p50=1.39x p95=3.57x`，
  `host_round_ratio_vs_baseline p50=1.00x`，`decision=report_only`。
- 结论：local budget / bounded replay 能消除 budget spill，但不能降低 host round
  或解除 p95 regression。下一步应转向 `vebo_weighted_owner`。

### Metal GAC v3.7：Seed/Overflow 拆分与 spill replay

**目标**：在进入 `vebo_weighted_owner` 前，先判断 v3.6 `owner_map_static`
失败是否主要来自 CTA queue/seed/overflow 协议。

**核心改动**：
- 新增 `--cta_queue_mode=local_only|spill_replay`，默认 `local_only`。
- 拆分 CTA overflow stats：
  - `cta_queue_overflow_count`
  - `cta_budget_spill_count`
  - `cta_seed_overflow_count`
  - `cta_overflow_count` 继续作为兼容总数。
- 新增 seed owner 统计：
  - `seed_owner_nonempty_count`
  - `seed_empty_owner_count`
  - `seed_max_owner_load`
  - `seed_owner_balance_p95`
- analyzer 按 `owner_mode + queue_mode` 拆分 CTA gate；新 CSV 优先使用
  queue/seed overflow 判断 gate，旧 CSV 回退到 `cta_overflow_count`。
- 新增独立小计划/小 changelog：
  - `docs/planning/metal_gac/METAL_GAC_V37_SEED_OVERFLOW_PLAN_2026_05_03.md`
  - `docs/planning/metal_gac/METAL_GAC_V37_SEED_OVERFLOW_CHANGELOG_2026_05_03.md`

**验证**：
- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py codex-docops-logic/scripts/dol.py`
- `cmake --build build_metal --target benchmark_metal_gac -j8`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=1 --warmup=0 --verify=true --runner_mode=prepared --frontier_mode=cta_worklist --kernel_variant=word_parallel --bitsup_layout=directional --cta_owner_mode=static_edge_cut --cta_queue_mode=spill_replay --csv=out/metal_gac_v37_seed_overflow_single.csv`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=cta --runs=3 --warmup=1 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=static_edge_cut --cta-queue-mode=spill_replay --cpu-timing --timeout=60 --csv=out/metal_gac_v37_seed_overflow_smoke.csv`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=cta --runs=5 --warmup=2 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=static_edge_cut --cta-queue-mode=spill_replay --cpu-timing --timeout=300 --csv=out/metal_gac_v37_seed_overflow_tier2.csv --quiet`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v35_tier2_baseline.csv out/metal_gac_v35_tier2_frontier.csv out/metal_gac_v36_owner_map_static_tier2.csv out/metal_gac_v37_seed_overflow_tier2.csv --top=10 --recommend-policy --baseline-mode shared+flags --regression-threshold 1.05 --min-runs 3`

**结果摘要**：
- metal-smoke：15/15 OK。
- TIER2：380/383 OK，3 个 ERROR 均为历史 `unsupported_non_binary_extension`。
- `spill_replay`：`solve_ms p50=0.478 p95=3.853`，
  `metal_cpu_solve_ratio p50=60.02x p95=403.55x`。
- Combined gate：`queue_overflow_p95=0`，`seed_overflow_p95=0`，
  `budget_spill_p95=1`，`host_round_ratio_vs_baseline p50=1.00x`，
  `decision=report_only`。
- 结论：seed/queue 真 overflow 不是主因；剩余问题集中在 local budget spill
  和 host round 未下降。下一步优先做 local budget / bounded replay，再视结果
  决定是否进入 `vebo_weighted_owner`。

### Metal GAC v3.6：`owner_map_static` 实验路径

**目标**：实现 `primal_edge_cut_owner` 的最小落地形态，用显式
`--cta_owner_mode=static_edge_cut` 替换 CTA worklist 内部的
`cid % cta_count` owner 策略，不改变默认 fallback 和 `auto`。

**核心改动**：
- `MetalGacOptions` / `benchmark_metal_gac` 新增 `cta_owner_mode`：
  `modulo|static_edge_cut`，默认保持 `modulo`。
- Host 侧为 CTA worklist 构建 `owner_of_constraint[cid]` buffer：
  - `modulo` 保持旧策略；
  - `static_edge_cut` 基于 constraint subscription adjacency 做轻量 greedy
    owner 分配，优先已分配邻居数，其次 owner load，最后 owner id。
- `gac_revise_cta_worklist_kernel` 和 CPU seed 阶段统一读取 owner map。
- CSV/analyzer 新增：
  - `cta_owner_mode`
  - `owner_map_build_ms`
  - `owner_balance_p95`
  - `owner_local_push_count`
  - `owner_cross_push_count`
- `metal_gac_ablation.py --cta-owner-mode` 负责转发实验开关；
  `metal_gac_analyze.py` 在 mode summary 和 CTA gate 中区分 owner mode。

**验证**：
- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py codex-docops-logic/scripts/dol.py`
- `cmake --build build_metal --target benchmark_metal_gac -j8`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=1 --warmup=0 --verify=true --runner_mode=prepared --frontier_mode=cta_worklist --kernel_variant=word_parallel --bitsup_layout=directional --cta_owner_mode=static_edge_cut --csv=out/metal_gac_v36_owner_map_static_single.csv`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=cta --runs=3 --warmup=1 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=static_edge_cut --cpu-timing --timeout=60 --csv=out/metal_gac_v36_owner_map_static_smoke.csv`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v36_owner_map_static_smoke.csv --top=5 --recommend-policy --baseline-mode shared+flags --regression-threshold 1.05 --min-runs 3`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=cta --runs=5 --warmup=2 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cta-owner-mode=static_edge_cut --cpu-timing --timeout=300 --csv=out/metal_gac_v36_owner_map_static_tier2.csv --quiet`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v35_tier2_baseline.csv out/metal_gac_v35_tier2_frontier.csv out/metal_gac_v36_owner_map_static_tier2.csv --top=10 --recommend-policy --baseline-mode shared+flags --regression-threshold 1.05 --min-runs 3`

**结果摘要**：
- 单例 `gac_bitwords2.xml` CPU verify 通过，`host_round_count=1`，
  `owner_balance_p95=1.000`，`owner_cross_push_count=0`。
- metal-smoke `static_edge_cut`：15/15 OK，analyzer 能按
  `owner=static_edge_cut` 独立汇总。
- TIER2 `static_edge_cut`：380/383 OK，3 个 ERROR 均为历史
  `unsupported_non_binary_extension`；`solve_ms p50=0.797 p95=3.503`。
- Combined CTA gate：`cta_vs_shared+flags p50=1.83x p95=4.51x`，
  `host_round_ratio_vs_baseline p50=1.00x`，`cta_overflow_count p95=1`，
  `decision=report_only`。
- 结论：`owner_map_static` 不通过 v3.6 promote gate；下一条应转向
  `vebo_weighted_owner` 或重新设计 overflow/seed。
- `cta_worklist` 仍是 report-only；`frontier_mode=auto` 不变。

### Metal GAC v3.6：CTA owner partition 分叉探索备忘

**目标**：承接 v3.5 `cta_worklist` TIER2 gate 未通过后的 owner partition
重设计，把后续值得探索的分支、判退门槛、证据文件和回滚锚点写入独立文档。

**核心改动**：
- 新增独立小计划/小 changelog：
  - `docs/planning/metal_gac/METAL_GAC_V36_CTA_OWNER_PARTITION_EXPLORATION_PLAN_2026_05_03.md`
  - `docs/planning/metal_gac/METAL_GAC_V36_CTA_OWNER_PARTITION_EXPLORATION_CHANGELOG_2026_05_03.md`
- 记录 6 条后续分支：
  - `owner_map_static`
  - `owner_bucketed_seed`
  - `dirty_var_pull`
  - `hub_replication`
  - `hierarchical_steal`
  - `indirect_multiround`
- 将更有希望的方向提升为 3 条优先主线：
  - `primal_edge_cut_owner`：CSP primal graph / edge-cut owner partition；
  - `vebo_weighted_owner`：VEBO-style weighted ordering，平衡 constraint work 与
    touched variables；
  - `bulk_sync_deletion_mask`：CTA 先产出 deletion masks，再 bulk-synchronous
    merge/apply。
- 每条分支都必须记录 hypothesis、实现草图、stats、smoke/TIER2 命令、
  promote gate、reject gate 与 evidence CSV。
- 更新 `METAL_GAC_CHANGELOG.md`、`METAL_GAC_LONG_TERM_OPTIMIZATION.md` 与
  `METAL_MIGRATION_PLAN.md` 的导航。

**验证**：
- `python3 -m py_compile codex-docops-logic/scripts/dol.py`
- `git diff --check`
- `python3 codex-docops-logic/scripts/dol.py lint --soft`

**结果摘要**：
- 本轮 docs-only，不修改 Metal solver，不改变 `frontier_mode=auto` 或默认
  fallback。
- v3.6 结论锚定 v3.5：`cta_vs_shared+flags p95=2.99x` 且
  `host_round_ratio_vs_baseline p50=1.00x p95=1.00x`，因此不能继续把
  `cid % cta_count` 当作默认 owner 策略。
- 下一步实现顺序建议：先 `primal_edge_cut_owner`，再
  `vebo_weighted_owner`，最后视 atomic/cross-push evidence 决定是否推进
  `bulk_sync_deletion_mask`。

### Metal GAC v3.5：CTA evidence / auto gate

**目标**：正式评估 `cta_worklist` 是否值得进入 recommender，甚至后续成为
`auto` 候选；本轮不改变默认 Metal fallback。

**核心改动**：
- 新增独立小计划/小 changelog：
  - `docs/planning/metal_gac/METAL_GAC_V35_CTA_EVIDENCE_PLAN_2026_05_03.md`
  - `docs/planning/metal_gac/METAL_GAC_V35_CTA_EVIDENCE_CHANGELOG_2026_05_03.md`
- `metal_gac_analyze.py` 新增 `[cta worklist gate]`：
  - `cta_worklist` 相对 `shared+flags` 的 p50/p95/p99 ratio；
  - `cta_worklist` 相对旧 `worklist` 的 p50/p95 ratio；
  - `host_round_count` 是否低于 baseline；
  - `cta_overflow_count`、`cta_cross_push_count`、`cta_queue_push_count`；
  - `metal_cpu_solve_ratio` 是否优于 baseline。
- 更新 `METAL_GAC_CHANGELOG.md`、`METAL_GAC_LONG_TERM_OPTIMIZATION.md` 与
  `METAL_MIGRATION_PLAN.md`。

**验证**：
- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py codex-docops-logic/scripts/dol.py`
- `cmake --build build_metal --target benchmark_metal_gac compare_cpu_metal -j`
- `ctest --test-dir build_metal -R compare_cpu_metal --output-on-failure`
- `ctest --test-dir build_metal -R benchmark_metal_gac --output-on-failure`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=cta --runs=3 --warmup=1 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cpu-timing --timeout=60 --csv=out/metal_gac_v35_smoke_cta.csv`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=baseline --runs=5 --warmup=2 --runner-mode=prepared --cpu-timing --timeout=300 --csv=out/metal_gac_v35_tier2_baseline.csv --quiet`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=frontier --runs=5 --warmup=2 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cpu-timing --timeout=300 --csv=out/metal_gac_v35_tier2_frontier.csv --quiet`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=cta --runs=5 --warmup=2 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --cpu-timing --timeout=300 --csv=out/metal_gac_v35_tier2_cta.csv --quiet`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v35_tier2_baseline.csv out/metal_gac_v35_tier2_frontier.csv out/metal_gac_v35_tier2_cta.csv --top=10 --recommend-policy --baseline-mode shared+flags --regression-threshold 1.05 --min-runs 3`
- `git diff --check`
- `python3 codex-docops-logic/scripts/dol.py lint --soft`

**结果摘要**：
- metal-smoke CTA：15/15 OK，Metal faster `0/15`。
- TIER2 baseline：380/383 rows OK，`shared+flags solve_ms p50=0.253 p95=5.109`。
- TIER2 worklist：`shared+worklist solve_ms p50=0.328 p95=5.351`。
- TIER2 CTA：380/383 rows OK，`cta_worklist solve_ms p50=0.453 p95=5.356`。
- `[cta worklist gate]`：
  - `cta_vs_shared+flags p50=1.27x p95=2.99x p99=4.04x`；
  - `cta_vs_best_worklist p50=1.37x p95=2.99x`；
  - `host_round_ratio_vs_baseline p50=1.00x p95=1.00x`；
  - `cta_overflow_count p95=0 max=0`；
  - `decision=report_only`。
- 结论：`cta_worklist` 不进入 `auto`，下一步应转向 Batch/SAC 多任务吞吐或
  owner partition 重设计，不进入 simdgroup。

### Metal GAC v3.3-v3.4：CTA-local persistent worklist 实验路径

**目标**：针对 CPU vs Metal evidence 暴露的 dispatch/round 往返瓶颈，实现
`cta_worklist` 实验路径；v3.3 simdgroup gate 继续保持关闭。

**核心改动**：
- `MetalFrontierMode` / `benchmark_metal_gac --frontier_mode` 新增
  `cta_worklist`。
- 新增 `gac_revise_cta_worklist_kernel`：
  - 每个 CTA/threadgroup 使用独立 queue A/B；
  - CTA-local stamp 去重；
  - 单 dispatch 内最多 8 轮 local worklist；
  - 跨 CTA 传播写 global next active list，由 host outer loop 重新播种。
- `MetalGacStats`、benchmark CSV、ablation CSV 与 analyzer 新增：
  - `cta_local_rounds`
  - `cta_queue_push_count`
  - `cta_cross_push_count`
  - `cta_overflow_count`
  - `host_round_count`
- `metal_gac_ablation.py --mode-preset=cta` 新增 CTA 实验扫描入口。
- 新增独立小计划/小 changelog：
  - `docs/planning/metal_gac/CPIM_METAL_METAL_GAC_V3_CTA_WORKLIST_PLAN_2026_05_03.md`
  - `docs/planning/metal_gac/CPIM_METAL_METAL_GAC_V3_CTA_WORKLIST_CHANGELOG_2026_05_03.md`
- 不让多个 CTA 直接竞争同一个全局 c queue；默认 fallback 与 `auto` policy
  不变。
- 更新 `METAL_GAC_CHANGELOG.md`、`METAL_GAC_LONG_TERM_OPTIMIZATION.md` 与
  `METAL_MIGRATION_PLAN.md` 的下一步链接和状态摘要。

**验证**：
- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py codex-docops-logic/scripts/dol.py`
- `cmake --build build_metal --target benchmark_metal_gac compare_cpu_metal -j`
- `ctest --test-dir build_metal -R compare_cpu_metal --output-on-failure`
- `ctest --test-dir build_metal -R benchmark_metal_gac --output-on-failure`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=3 --warmup=1 --verify=true --runner_mode=prepared --frontier_mode=cta_worklist --kernel_variant=word_parallel --bitsup_layout=directional --csv=out/metal_gac_v34_cta_worklist_smoke.csv`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=cta --runs=2 --warmup=1 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --timeout=60 --csv=out/metal_gac_v34_cta_smoke.csv`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v34_cta_smoke.csv --top=5 --recommend-policy --baseline-mode shared+flags --regression-threshold 1.05 --min-runs=2`
- `git diff --check`
- `python3 codex-docops-logic/scripts/dol.py lint --soft`

**结果摘要**：
- `compare_cpu_metal`：5/5 passed。
- `benchmark_metal_gac`：1/1 passed。
- `gac_bitwords2.xml` CTA smoke：CPU verify 通过，`host_round_count=1`，
  `cta_local_rounds=2`，`cta_queue_push_count=1`，`cta_overflow_count=0`。
- `metal-smoke --mode-preset=cta`：10/10 OK，5 个实例全部 CPU verify 通过。

### Metal GAC v3：CPU vs Metal 正式对照

**目标**：把 CPU GAC 求解时间纳入 Metal benchmark/CSV/analyzer，正式回答
“Metal 和 CPU 哪个快”。

**核心改动**：
- `benchmark_metal_gac` 新增：
  - `--cpu_timing`
  - `--cpu_warmup`
  - `--cpu_runs`
- benchmark CSV 与 ablation CSV 新增：
  - `cpu_timing_enabled`
  - `cpu_solve_ms`
  - `cpu_iterations`
  - `cpu_deletions`
  - `cpu_inconsistent`
  - `metal_cpu_solve_ratio`
  - `metal_faster_than_cpu`
- `metal_gac_analyze.py` 新增 `[metal vs cpu]` section，明确
  `metal_cpu_solve_ratio < 1.0` 表示 Metal 更快。
- 新增独立小计划/小 changelog：
  - `docs/planning/metal_gac/METAL_GAC_V3_CPU_METAL_COMPARISON_PLAN_2026_05_03.md`
  - `docs/planning/metal_gac/METAL_GAC_V3_CPU_METAL_COMPARISON_CHANGELOG_2026_05_03.md`

**验证**：
- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py codex-docops-logic/scripts/dol.py`
- `cmake --build build_metal --target benchmark_metal_gac compare_cpu_metal -j`
- `ctest --test-dir build_metal -R compare_cpu_metal --output-on-failure`
- `ctest --test-dir build_metal -R benchmark_metal_gac --output-on-failure`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=3 --warmup=1 --verify=true --cpu_timing=true --runner_mode=prepared --frontier_mode=flags --kernel_variant=scalar --bitsup_layout=pair --csv=out/metal_gac_v3_cpu_metal_smoke.csv`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=auto --runs=5 --warmup=2 --runner-mode=prepared --cpu-timing --timeout=60 --csv=out/metal_gac_v3_cpu_metal_smoke_auto.csv`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=auto --runs=5 --warmup=2 --runner-mode=prepared --cpu-timing --timeout=300 --csv=out/metal_gac_v3_cpu_metal_tier2_auto.csv --quiet`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v3_cpu_metal_smoke_auto.csv out/metal_gac_v3_cpu_metal_tier2_auto.csv --top=10 --recommend-policy --baseline-mode shared+flags --regression-threshold 1.05 --min-runs 3`

**结果摘要**：
- metal-smoke auto：25/25 OK，Metal faster `0/25`。
- TIER2 auto：380/383 rows OK，3 个 ERROR 均为
  `unsupported_non_binary_extension`。
- TIER2 auto：Metal solve `p50=0.439ms p95=5.050ms`，CPU solve
  `p50=0.008438ms p95=0.071971ms`。
- Metal/CPU ratio：`p50=33.65x p95=427.90x`，Metal faster `0/380`。
- 结论：当前 GAC-only Metal 不比 CPU 快；后续 Metal 性能线应优先减少
  dispatch 往返，或转向更适合 GPU 批量化的 SAC/Batch 工作负载。

### Metal GAC v3：evidence recommender 与 simdgroup gate

**目标**：启动 Metal GAC v3，但不直接修改 v2 默认 fallback；先用可复跑
CSV evidence 判断哪些实例适合激进路径，以及 simdgroup/threadgroup staging 是否
值得进入真实实现。

**核心改动**：
- DocOps roadmap 从 `v02` bump 到 `v03`，并新增独立小计划/小 changelog：
  - `docs/planning/metal_gac/METAL_GAC_V3_POLICY_RECOMMENDER_PLAN_2026_05_03.md`
  - `docs/planning/metal_gac/METAL_GAC_V3_POLICY_RECOMMENDER_CHANGELOG_2026_05_03.md`
- `metal_gac_analyze.py` 新增 report-only recommender：
  - `--recommend-policy`
  - `--baseline-mode shared+flags`
  - `--regression-threshold 1.05`
  - `--min-runs 3`
- mode summary 新增 `kernel_share`、`dispatch_share`、`reset_share` 与
  `worklist_push_per_round`，帮助判断瓶颈是在 kernel、host dispatch 还是 reset。
- recommender 输出全局 candidate、bucket candidate、baseline bottleneck counts
  与超过阈值的 regression 样例；推荐只作为报告，不改变 Metal solver 默认路径。

**验证**：
- `python3 -m py_compile tests/python/metal_gac_analyze.py`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v2x_tier2_all.csv out/metal_gac_v2x_tier2_auto.csv --top=3 --recommend-policy --baseline-mode shared+flags --regression-threshold 1.05 --min-runs 3`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=all --runs=5 --warmup=2 --runner-mode=prepared --timeout=300 --csv=out/metal_gac_v3_tier2_all.csv --quiet`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=auto --runs=5 --warmup=2 --runner-mode=prepared --timeout=300 --csv=out/metal_gac_v3_tier2_auto.csv --quiet`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v3_tier2_all.csv out/metal_gac_v3_tier2_auto.csv --top=10 --recommend-policy --baseline-mode shared+flags --regression-threshold 1.05 --min-runs 3`

**结果摘要**：
- v3 TIER2 all：2280/2298 rows OK，18 个 ERROR 均为
  `unsupported_non_binary_extension`。
- v3 TIER2 auto：380/383 rows OK，3 个 ERROR 均为
  `unsupported_non_binary_extension`。
- baseline `shared+flags+scalar+pair`：`solve_ms p50=0.290 p95=4.995`；
  `shared+auto`：`solve_ms p50=0.223 p95=6.293`。
- combined recommender 显示 `baseline_bottleneck_counts dispatch=76`；
  `shared+auto` 的 `p95_ratio=1.54` 且 19 个实例超过 5% regression threshold，
  因此 v3 不提升 auto policy，不进入 simdgroup kernel 实现。

### Metal GAC 文档落账：每个小计划/小 changelog 独立成文

**目标**：把 Metal GAC v2.x 的封版结论、下一步计划和细粒度 changelog
全部落到独立文档，避免只停留在对话、一次性执行结果或聚合页里。

**核心改动**：
- 新增 `docs/planning/METAL_GAC_CHANGELOG.md` 作为 Metal GAC 小计划/小
  changelog 索引页。
- 新增独立小 changelog 文档：
  - `docs/planning/metal_gac/METAL_GAC_V1_BASELINE_CHANGELOG_2026_05_02.md`
  - `docs/planning/metal_gac/METAL_GAC_V20_V21_CHANGELOG_2026_05_03.md`
  - `docs/planning/metal_gac/METAL_GAC_V22_V24_CHANGELOG_2026_05_03.md`
  - `docs/planning/metal_gac/METAL_GAC_V25_V29_CHANGELOG_2026_05_03.md`
- 新增独立小计划文档：
  - `docs/planning/metal_gac/METAL_GAC_V2_GUARD_V3_ENTRY_PLAN_2026_05_03.md`
- DocOps Logic 插件新增通用 microdoc 能力：
  - `dol doc new --kind plan|changelog`
  - `templates/small-plan.md`
  - `templates/small-changelog.md`
  - `skills/doc/SKILL.md`
  - 本仓库 `codex-docops-logic/` 与已安装 local plugin cache 同步更新。
- 新增 DocOps Logic microdocs 功能自己的独立小计划/小 changelog：
  - `docs/planning/metal_gac/CPIM_METAL_DOCOPS_MICRODOCS_PLAN_2026_05_03.md`
  - `docs/planning/metal_gac/CPIM_METAL_DOCOPS_MICRODOCS_CHANGELOG_2026_05_03.md`
- 更新 `docs/planning/METAL_GAC_LONG_TERM_OPTIMIZATION.md`：
  - 增加 changelog 落账入口；
  - 将“下一步：v2 Guard 与 v3 入口”收敛为独立小计划链接。
- 更新 `docs/planning/METAL_MIGRATION_PLAN.md`，补充 Metal GAC 小 changelog
  的文档位置。
- 更新 `docs/README.md`，把 `METAL_GAC_CHANGELOG.md` 纳入关键规划文档导航。

**文档规则**：
- 大 changelog：`CHANGES_ZH.md`，记录跨模块和可发布摘要。
- 小 changelog：每个版本段一个新文档，放在 `docs/planning/metal_gac/`。
- 小计划：每个执行计划一个新文档，放在 `docs/planning/metal_gac/`。
- `docs/planning/METAL_GAC_CHANGELOG.md` 只做索引，不承载多个小计划/小
  changelog 的正文。
- 路线/迁移文档必须同步更新状态或链接，不允许只有 changelog。

### Metal v2.5-v2.9：auto、epoch worklist、blit reset 与 v2 封版

**目标**：把 Metal GAC v2 从已有可消融路径收敛为可自动选择、可解释、
可回退、可封版的性能后端。

**核心改动**：
- `MetalGacOptions` / `benchmark_metal_gac` 新增
  `reset_mode=cpu|blit|auto`；默认仍为 `cpu`。
- `MetalGacStats` / CSV 新增：
  - `reset_dispatch_ms`
  - `effective_frontier_mode`
  - `effective_kernel_variant`
  - `effective_bitsup_layout`
  - `worklist_push_count`
  - `worklist_rounds`
  - `worklist_epoch_resets`
- `frontier_mode=auto` / `kernel_variant=auto` 改为封版保守策略：TIER2 收尾数据
  发现较宽的 worklist auto 会让 p95 超过 baseline 5%，word_parallel auto 会让
  p50 超过 baseline 5%，因此 v2 auto 降级到 `flags + scalar`；`simdgroup`
  继续 fallback 到 word_parallel，并通过 effective 字段记录。
- scalar flags/compact 实际仍读 pair bitSup；当 requested directional 但执行路径
  未使用 directional 时，effective bitsup 明确记录为 `pair`。
- worklist 路径新增 epoch/stamp 去重：每轮递增 epoch，不再 memset 全量
  next frontier；溢出时安全清零并计入 `worklist_epoch_resets`。
- `MetalPreparedGacRunner` 增加初始 mutable snapshot buffer；blit reset 使用
  command buffer copy 初始 `bit_dom/domain_sizes/frontier/active_list` 并 clear
  stats/next buffers。
- `metal_gac_ablation.py` 新增 `--mode-preset=auto` 与 `--reset-mode`，TIER2 中
  已知 unsupported non-binary extension 不再作为性能扫描失败。
- `metal_gac_analyze.py` 新增 recommended policy summary，按 family、
  `num_constraints`、`max_dom_size`、`bit_words`、`frontier_density_avg` 分桶，
  并报告 auto 命中、胜出和劣化超过 5% 的实例。
- 更新 Metal GAC 长期优化路线与迁移计划；v2 封版结论为不在 v2 强行实现
  simdgroup reduction，进入 v3 需先由 word_parallel 数据证明瓶颈。

**验证**：
- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py`
- `cmake --build build_metal --target benchmark_metal_gac compare_cpu_metal -j`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=1 --warmup=0 --verify=true --runner_mode=prepared --frontier_mode=worklist --kernel_variant=word_parallel --bitsup_layout=directional --reset_mode=blit --csv=out/metal_gac_v2x_worklist_word_blit_smoke.csv`
- `ctest --test-dir build_metal -R compare_cpu_metal --output-on-failure`
- `ctest --test-dir build_metal -R benchmark_metal_gac --output-on-failure`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=all --runs=2 --warmup=1 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --timeout=60 --csv=out/metal_gac_v2x_smoke_all.csv`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=auto --runs=3 --warmup=1 --runner-mode=prepared --timeout=60 --csv=out/metal_gac_v2x_smoke_auto.csv --quiet`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=all --runs=5 --warmup=2 --runner-mode=prepared --timeout=300 --csv=out/metal_gac_v2x_tier2_all.csv --quiet`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=auto --runs=5 --warmup=2 --runner-mode=prepared --timeout=300 --csv=out/metal_gac_v2x_tier2_auto.csv --quiet`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v2x_tier2_all.csv out/metal_gac_v2x_tier2_auto.csv --top=10`
- `git diff --check`

**结果摘要**：
- worklist + word_parallel + directional + blit reset smoke 通过 CPU verify；
  CSV 已包含 effective path、reset 与 worklist 统计字段。
- `metal-smoke` all-mode 60/60 OK；auto 15/15 OK。
- TIER2 all-mode：2280/2298 rows OK，18 个 ERROR 均为
  `unsupported_non_binary_extension`，脚本返回码为 0。
- TIER2 auto：380/383 rows OK，3 个 ERROR 均为
  `unsupported_non_binary_extension`，脚本返回码为 0。
- TIER2 baseline `shared+flags+scalar+pair`：`solve_ms p50=0.261 p95=5.132`；
  auto 封版路径 `flags+scalar+pair`：`solve_ms p50=0.259 p95=5.079`，满足
  auto p50/p95 不比 baseline 慢 5% 的门槛。
- 默认 stable fallback 仍为 `cold + shared + flags + scalar + pair + cpu reset`。

### Metal v2.2-v2.4：worklist、word-parallel 与 directional bitSup

**目标**：把 `worklist/word_parallel/auto` 从 fallback 开关推进为可消融的真实
Metal GAC 性能路径，并保留默认 stable fallback。

**核心改动**：
- `DeviceModelLayout` 新增 `bit_sup_words`，按
  `bit_sup_words[cid][dir][value][word]` 存储 directional bitSup。
- `MetalGacOptions` / `benchmark_metal_gac` 新增 `bitsup_layout=pair|directional|auto`。
- `MetalGacSolver` 新增：
  - worklist active constraint 双缓冲；
  - directional bitSup buffer；
  - worklist revise pipeline；
  - word-parallel flags / active-list / worklist pipelines。
- `.metal` 新增：
  - `gac_revise_worklist_kernel`
  - `gac_revise_word_flags_kernel`
  - `gac_revise_word_active_kernel`
  - `gac_revise_word_worklist_kernel`
- `frontier_mode=worklist` 每轮只 dispatch revise kernel，并用 `next_flags` 去重；
  `frontier_mode=auto` 在 `num_constraints >= 128` 且 directional bitSup 可用时走
  worklist，否则走 flags。
- `kernel_variant=word_parallel` 启用 word-level revise；
  `kernel_variant=auto` 和 `simdgroup` 当前落到 word_parallel，并通过
  `variant_name` 记录。
- `metal_gac_ablation.py` / `metal_gac_analyze.py` 新增 `bitsup_layout` 维度。
- `metal_gac_ablation.py --mode-preset=all` 扩展为
  `shared/private × flags/compact/worklist`，确保 worklist 进入批量扫描矩阵。
- 更新 Metal v2 长期优化路线与迁移计划。

**验证**：
- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py`
- `cmake --build build_metal --target benchmark_metal_gac compare_cpu_metal -j`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=3 --warmup=1 --verify=true --runner_mode=prepared --frontier_mode=worklist --kernel_variant=word_parallel --bitsup_layout=directional --csv=out/metal_gac_v24_worklist_word_directional_smoke.csv`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=1 --warmup=0 --verify=true --runner_mode=prepared --frontier_mode=auto --kernel_variant=simdgroup --bitsup_layout=auto --csv=out/metal_gac_v24_auto_simdgroup_smoke.csv`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=all --runs=2 --warmup=1 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --timeout=60 --csv=out/metal_gac_v24_smoke.csv`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=all --runs=5 --warmup=2 --runner-mode=prepared --kernel-variant=word_parallel --bitsup-layout=directional --timeout=300 --csv=out/metal_gac_v24_tier2.csv --quiet`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v24_smoke.csv out/metal_gac_v24_tier2.csv --top=10`
- `ctest --test-dir build_metal -R compare_cpu_metal --output-on-failure`
- `ctest --test-dir build_metal -R benchmark_metal_gac --output-on-failure`
- `python3 codex-docops-logic/scripts/dol.py lint --soft`
- `git diff --check`

**结果摘要**：
- worklist + word_parallel + directional smoke 通过 CPU verify。
- `auto+simdgroup+auto` 明确记录为
  `auto+simdgroup+auto->flags+word_parallel+directional`。
- `metal-smoke` all-mode：60/60 OK。
- TIER2 all-mode：2280/2298 rows OK，18 个 ERROR 均为
  `unsupported_non_binary_extension`，与历史支持面边界一致。
- TIER2 shared+flags word_parallel+directional：`solve_ms avg=0.819 p50=0.340 p95=5.362 p99=5.891`。
- TIER2 shared+worklist word_parallel+directional：
  `solve_ms avg=0.826 p50=0.356 p95=5.274 p99=5.917`。

**兼容性与语义**：
- 默认仍为 `cold + shared + flags + scalar + pair`。
- pair bitSup、scalar flags/compact 旧路径保留。
- 本轮不实现真正 simdgroup reduction，不引入 Metal texture。
- CUDA/Jetson 路径不受影响。

### Metal v2.0-v2.1：GAC 长期优化路线与 prepared runner

**目标**：按 GAC 性能优先路线开启 Metal v2，先冻结 benchmark/CSV 观测口径，
再用 prepared runner 拆出初始化与 mutable reset 成本，为后续 worklist、simdgroup
与布局优化铺消融入口。

**核心改动**：
- 新增 `docs/planning/METAL_GAC_LONG_TERM_OPTIMIZATION.md`，记录 v2.0-v2.4
  长期路线、CUDA 经验映射、验收命令和回退原则。
- `MetalGacOptions` 新增：
  - `runner_mode = cold|prepared`
  - `kernel_variant = scalar|word_parallel|simdgroup|auto`
  - `frontier_mode` 预留 `worklist|auto`，当前回退到 stable `flags + scalar`。
- 新增 `MetalPreparedGacRunner`，支持一次 `Prepare()` 后多次 `Run()`，每次仅重置
  mutable state。
- `MetalGacStats` / `benchmark_metal_gac` CSV 新增：
  - `runner_mode`
  - `kernel_variant`
  - `variant_name`
  - `solve_ms`
  - `prepare_ms`
  - `reset_ms`
  - `active_constraints_total`
  - `frontier_density_avg`
- `metal_gac_ablation.py` 透传 `--runner-mode` 与 `--kernel-variant`，批量 CSV
  保留新增字段。
- `metal_gac_analyze.py` 将主分析指标切换为 `solve_ms`；旧 CSV 没有该字段时
  自动回退到 `elapsed_ms`。
- 更新 Metal 迁移计划和 docs 导航，记录 v2.0/v2.1 状态。

**验证**：
- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py`
- `cmake --build build_metal --target benchmark_metal_gac compare_cpu_metal -j`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=2 --warmup=1 --verify=true --runner_mode=prepared --csv=out/metal_gac_v21_prepared_smoke.csv`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=1 --warmup=0 --verify=true --runner_mode=prepared --frontier_mode=worklist --kernel_variant=simdgroup --csv=out/metal_gac_v21_fallback_smoke.csv`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=baseline --runs=1 --warmup=0 --runner-mode=prepared --kernel-variant=scalar --timeout=60 --csv=out/metal_gac_v21_ablation_prepared_smoke.csv --quiet`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v21_prepared_smoke.csv out/metal_gac_v21_ablation_prepared_smoke.csv --top=3`
- `ctest --test-dir build_metal -R compare_cpu_metal --output-on-failure`
- `ctest --test-dir build_metal -R benchmark_metal_gac --output-on-failure`
- `python3 codex-docops-logic/scripts/dol.py lint --soft`
- `git diff --check`

**结果摘要**：
- prepared smoke 通过 CPU verify，`variant_name=flags+scalar`；
- fallback smoke 通过 CPU verify，`variant_name=worklist+simdgroup->flags+scalar`；
- `metal-smoke` baseline prepared 扫描 5/5 OK；
- Metal compare CTest 5/5 通过，benchmark smoke 1/1 通过。
- 求解主指标改为 `solve_ms = reset_ms + dispatch_ms`；`setup_ms/prepare_ms`
  只记录初始化成本。

**兼容性与语义**：
- 默认仍为 `cold + shared + flags + scalar`；
- `worklist`、`word_parallel`、`simdgroup`、`auto` 目前只作为可观测消融开关，
  未实现专用 kernel 前通过 `variant_name` 记录回退路径；
- 不修改 CUDA/Jetson 路径。

### Metal v2.1：求解时间统计口径修正

**目标**：把 Metal GAC 性能主指标从 wall-clock `elapsed_ms` 调整为只统计求解阶段
的 `solve_ms`，避免初始化成本混入 GAC 算子对比。

**核心改动**：
- `MetalGacStats` 新增 `solve_ms`。
- `benchmark_metal_gac` CSV 新增 `solve_ms`，stdout 优先输出
  `solve_ms p50/p95/p99`；`elapsed_ms` 继续保留为兼容观测。
- `solve_ms = reset_ms + dispatch_ms`：
  - `reset_ms` 表示单次求解前恢复 mutable state；
  - `dispatch_ms` 表示实际提交 Metal command buffer 并等待完成的成本；
  - `setup_ms/prepare_ms` 只记录初始化成本，不进入求解主指标。
- `metal_gac_ablation.py` 汇总切换为 `avg_solve_ms`。
- `metal_gac_analyze.py` 主分析指标切换为 `solve_ms`，旧 CSV 缺该字段时回退
  `elapsed_ms`。
- 更新 Metal v2 长期优化文档与迁移计划中的指标口径说明。

**验证**：
- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py`
- `cmake --build build_metal --target benchmark_metal_gac -j`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=3 --warmup=1 --verify=true --runner_mode=prepared --csv=out/metal_gac_v21_solve_ms_smoke.csv`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v21_solve_ms_smoke.csv --top=3`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=baseline --runs=1 --warmup=0 --runner-mode=prepared --kernel-variant=scalar --timeout=60 --csv=out/metal_gac_v21_solve_ms_ablation.csv --quiet`
- `ctest --test-dir build_metal -R benchmark_metal_gac --output-on-failure`
- `git diff --check`

**结果摘要**：
- `benchmark_metal_gac` stdout 已输出 `solve_ms p50/p95/p99`；
- `out/metal_gac_v21_solve_ms_smoke.csv` 已包含 `solve_ms` 字段；
- `metal_gac_analyze.py` 以 `solve_ms` 汇总，prepared smoke 3/3 OK；
- `metal_gac_ablation.py` 以 `avg_solve_ms` 汇总，metal-smoke baseline 5/5 OK。

### Metal v1.8：parser 支持面与 unsupported 分类修正

**目标**：修复 TIER3 支持面中的 parser 问题，同时避免把 pairwise 分解误记为
global constraint GAC 支持。

**核心改动**：
- `LibXml2Parser::ParseDomainValues()` 支持混合离散域 range token：
  - 例如 `0..2 6..7 12` 会展开为枚举域；
  - 原有连续 range 和枚举值语义保持不变。
- `LibXml2Parser::ParseConstraints()` 对 `reference="global:allDifferent"` 明确返回
  `UNIMPLEMENTED`。
- predicate/intension `P*` reference 明确返回 `UNIMPLEMENTED`，不再误报为
  invalid relation。
- 不再把 `AllDifferent` 展开成 pairwise binary `!=` supports，因为这不是 global
  AllDifferent GAC。
- `metal_gac_ablation.py` / `metal_gac_analyze.py` 新增
  `unsupported_predicate_intension` 分类。
- 更新 Metal 迁移计划和 docs 导航，记录 v1.8 支持面边界。

**验证**：
- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py`
- `cmake --build build_metal --target benchmark_metal_gac compare_cpu_metal -j`
- `./build_metal/benchmark_metal_gac --input=benchmarks/queenAttacking/queenAttacking-5.xml --runs=1 --warmup=0 --verify=true --csv=out/metal_gac_v18_queen_attacking5.csv`
- `./build_metal/benchmark_metal_gac --input=benchmarks/fapp26-30/fapp26-30/fapp26/fapp26-2300-0.xml --runs=1 --warmup=0 --verify=true --csv=out/metal_gac_v18_fapp_check.csv`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=all --runs=2 --warmup=1 --timeout=60 --csv=out/metal_gac_v18_smoke.csv`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=all --runs=3 --warmup=1 --timeout=300 --csv=out/metal_gac_v18_tier2_all.csv --quiet`
- `python3 tests/python/metal_gac_ablation.py --tier=3 --mode-preset=baseline --runs=1 --warmup=0 --timeout=180 --csv=out/metal_gac_v18_tier3_baseline.csv --quiet`
- `ctest --test-dir build_metal -R compare_cpu_metal --output-on-failure`
- `cmake --build build_metal --target cpim_gac_cpu -j`

**结果摘要**：
- TIER2 all-mode 保持 76/79 实例 OK，剩余 3 个仍为非二元 extension unsupported。
- TIER3 baseline 保持 818/1065 实例 OK。
- TIER3 剩余失败分类为：
  - `unsupported_predicate_intension`: 197
  - `unsupported_non_binary_extension`: 40
  - `unsupported_global_alldifferent`: 10

**兼容性与语义**：
- 不修改 Metal kernel；
- `AllDifferent` 保持 unsupported，不新增 global constraint API；
- predicate/intension 与非二元 extension 仍保持 fail-fast。

### Metal v1.7：扫描分析闭环与错误分类

**目标**：把补齐 benchmark 数据后的 TIER 扫描从“临时分析”推进为可复用流程，
并让 TIER3 的失败原因稳定分类，便于判断下一步支持面优化。

**核心改动**：
- `benchmark_metal_gac` CSV 新增 `dispatch_count` 字段，与 stdout 的
  `dispatch_count=...` 保持一致。
- `tests/python/metal_gac_ablation.py`：
  - CSV 新增 `error_category`；
  - 透传 benchmark CSV 的 `dispatch_count`；
  - 将 missing、unsupported、parse、timeout、verification mismatch 等失败归类。
- 新增 `tests/python/metal_gac_analyze.py`：
  - 汇总 OK/ERROR/TIMEOUT/MISSING；
  - 按 mode 输出 avg/p50/p95/p99、setup/dispatch/kernel、dispatch_count；
  - 按 family 和 error category 汇总失败；
  - 输出 `private/compact` 相对 `shared+flags` 的倍率和胜出个数；
  - 输出最慢实例列表，辅助定位后续优化目标。
- `LibXml2Parser` 遇到 constraint 引用无效 relation ID 时返回
  `INVALID_ARGUMENT`，不再触发 `CHECK` 让 benchmark 子进程 fatal。
- 更新 Metal 迁移计划和 docs 导航，记录 v1.7 的分析入口与验证结果。

**验证**：
- `python3 -m py_compile tests/python/metal_gac_ablation.py tests/python/metal_gac_analyze.py`
- `cmake --build build_metal --target benchmark_metal_gac -j`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=all --runs=2 --warmup=1 --timeout=60 --csv=out/metal_gac_v17_smoke.csv`
- `./build_metal/benchmark_metal_gac --input=benchmarks/fapp26-30/fapp26-30/fapp26/fapp26-2300-0.xml --runs=1 --warmup=0 --verify=true --csv=out/metal_gac_v17_invalid_relation_check.csv`
- `python3 tests/python/metal_gac_ablation.py --tier=2 --mode-preset=all --runs=3 --warmup=1 --timeout=300 --csv=out/metal_gac_v17_tier2_all.csv --quiet`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v17_tier2_all.csv --top=8`
- `python3 tests/python/metal_gac_ablation.py --tier=3 --mode-preset=baseline --runs=1 --warmup=0 --timeout=120 --csv=out/metal_gac_v17_tier3_baseline.csv --quiet`
- `python3 tests/python/metal_gac_analyze.py out/metal_gac_v17_tier3_baseline.csv --top=12`

**结果摘要**：
- TIER2 all-mode：76/79 实例 OK，912 rows verified；3 个 `rand-8-20-5`
  归类为 `unsupported_non_binary_extension`。
- TIER3 baseline：818/1065 实例 OK，247 个 ERROR，0 timeout；失败分类为
  `parse_invalid_relation_id`、`unsupported_non_binary_extension`、
  `parse_discontiguous_domain`、`unsupported_global_alldifferent`。
- TIER2 上 `shared+flags` 仍是默认最优：`shared+compact` 平均约 1.20x，
  `private+flags` 平均约 1.30x，`private+compact` 平均约 1.48x。

**兼容性与语义**：
- 不修改 Metal GAC kernel correctness 语义；
- CSV schema 向后扩展，旧 CSV 仍可由 `metal_gac_analyze.py` 读取；
- CUDA/Jetson 回归仍需在 CUDA 机器上单独执行。

### Metal v1.6：tier-aware ablation 扫描脚本

**目标**：借用现有 TIER0~TIER3 测试用例分级，把 Metal GAC 的 shared/private
storage 与 flags/compact frontier 消融从单点 smoke 扩展到批量 correctness 扫描。

**核心改动**：
- 新增 `tests/python/metal_gac_ablation.py`：
  - 复用 `tests/python/tier_definitions.py` 的 TIER0~TIER3 实例列表；
  - 提供 `--suite=tier|metal-smoke`，其中 `metal-smoke` 覆盖本地签入的小型 Metal
    correctness fixtures 与 bench smoke；
  - 提供 `--mode-preset=baseline|storage|frontier|all`，批量运行
    `benchmark_metal_gac` 的 storage/frontier 组合；
  - 支持 `--record-missing`，将未签入的外部 `benchmarks/...` 用例记录为
    `MISSING`，不中断整批扫描；
  - 自动尝试把 `benchmarks/...` 同名路径映射到 `tests/data/bench/...` 的本地样例。
- 输出 CSV 增加批量扫描上下文：
  - `tier`
  - `instance_index`
  - `status = OK|MISSING|TIMEOUT|ERROR`
  - `process_ms`
  - benchmark 原有 device、规模、迭代、删值、timing 与 verify 字段。
- 更新 Metal 迁移计划和 docs 导航，记录 v1.6 的测试边界与命令。

**验证**：
- `python3 -m py_compile tests/python/metal_gac_ablation.py`
- `python3 tests/python/metal_gac_ablation.py --suite=metal-smoke --mode-preset=all --runs=2 --warmup=1 --timeout=60 --csv=out/metal_gac_v16_smoke.csv`
- `python3 tests/python/metal_gac_ablation.py --tier=0 --mode-preset=baseline --runs=1 --warmup=0 --timeout=60 --csv=out/metal_gac_v16_tier0_baseline_smoke.csv --record-missing`

**兼容性与语义**：
- v1.6 不修改 Metal kernel、CPU/CUDA CLI 或 CMake target；
- correctness 仍由 `benchmark_metal_gac --verify=true` 调用 CPU oracle 校验；
- 当前仓库未签入的外部 benchmark 会记录为 `MISSING`，补齐数据后可用同一脚本扩大到
  TIER1~TIER3。

### Metal v1.4-v1.5：readonly storage 与 compact frontier 消融

**目标**：在不进入 SAC/Batch 的前提下，为 Metal GAC 增加两条可回退的性能消融路径：
readonly metadata 的 shared/private storage，以及 active constraints compact frontier。

**核心改动**：
- `MetalRuntime` 新增 private buffer 上传能力：
  - 使用 shared staging buffer + blit command buffer 初始化 `MTLStorageModePrivate`；
  - 仍保持 mutable state 固定 shared，避免 CPU 结果读取与 GPU 写回路径复杂化。
- `MetalGacOptions` 新增：
  - `readonly_storage = shared|private`
  - `frontier_mode = flags|compact`
- `MetalGacStats` 新增输出：
  - `readonly_storage`
  - `frontier_mode`
- `.metal` kernel 新增：
  - `gac_compact_frontier_kernel`
  - `gac_revise_compact_kernel`
- `benchmark_metal_gac` 新增 flags：
  - `--readonly_storage=shared|private`
  - `--frontier_mode=flags|compact`
  - CSV 新增 `readonly_storage` 与 `frontier_mode` 字段。
- `benchmark_metal_gac_smoke` 改为覆盖 `private + compact`，保持 CPU verify。
- 更新 Metal 迁移计划与 docs 导航，记录 v1.4/v1.5 的默认值、消融命令与回退边界。

**验证**：
- `cmake -S . -B build_metal -DCPIM_ENABLE_CUDA=OFF -DCPIM_ENABLE_METAL=ON -DCMAKE_BUILD_TYPE=Release`
- `cmake --build build_metal --target compare_cpu_metal benchmark_metal_gac -j`
- `ctest --test-dir build_metal -R compare_cpu_metal --output-on-failure`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=5 --warmup=2 --verify=true --readonly_storage=shared --frontier_mode=flags --csv=out/metal_gac_v14_shared_flags.csv`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=5 --warmup=2 --verify=true --readonly_storage=private --frontier_mode=compact --csv=out/metal_gac_v15_private_compact.csv`
- `./build_metal/benchmark_metal_gac --input=tests/data/bench/queens-4_ext.xml --runs=5 --warmup=2 --verify=true --readonly_storage=private --frontier_mode=compact`
- `ctest --test-dir build_metal -R benchmark_metal_gac --output-on-failure`
- `cmake -S . -B build_cpu -DCPIM_ENABLE_CUDA=OFF -DCPIM_ENABLE_METAL=OFF -DCMAKE_BUILD_TYPE=Release`
- `cmake --build build_cpu --target cpim_gac_cpu -j`

**兼容性与语义**：
- 默认仍为 `readonly_storage=shared` 与 `frontier_mode=flags`；
- private/compact 均通过 benchmark flag 消融，不改变 `compare_cpu_metal` 默认路径；
- correctness 继续以 CPU verify 为准，不声明性能收益；
- CUDA/Jetson 回归仍需在 CUDA 机器上单独执行。

### Metal v1.2-v1.3：运行时抽象与性能观测

**目标**：把 Metal GAC 从单一 solver 内部实现推进为可复用 runtime，并建立
GAC-only 性能观测入口；仍不进入 MAC/SAC/Batch。

**核心改动**：
- 新增 Metal runtime 封装：
  - `MetalRuntime` 负责创建默认 Metal device、加载 `.metallib`、创建 pipeline 与提交
    1D dispatch；
  - `MetalBuffer` / `MetalPipeline` 以 C++ move-only RAII 对象隐藏 Objective-C
    `id<MTL...>` 类型。
- `MetalGacSolver` 改用 runtime 分配 shared buffer、绑定 kernel 参数和提交每轮 dispatch，
  correctness 对比语义保持 v1.1 不变。
- `MetalGacStats` 新增：
  - `dispatch_count`
  - `setup_ms`
  - `dispatch_ms`
  - `kernel_ms`
  - `gpu_timing_available`
- 新增 `benchmark_metal_gac`：
  - 支持 `--input`、`--metallib`、`--runs`、`--warmup`、`--max_iterations`、`--csv`、
    `--verify`、`--verbose`；
  - 输出 `elapsed_ms`、`dispatch_ms`、`kernel_ms` 的 p50/p95/p99；
  - CSV 每个 measured run 一行，并记录 device、输入规模、迭代/删值和 verify 状态。
- `CMakeLists.txt` 在 `CPIM_METAL_ENABLED` 下构建 `benchmark_metal_gac`，并注册
  `benchmark_metal_gac_smoke`。
- 更新 Metal 迁移计划和 docs 导航，记录 v1.2/v1.3 边界、接口与验证命令。

**验证**：
- `cmake -S . -B build_metal -DCPIM_ENABLE_CUDA=OFF -DCPIM_ENABLE_METAL=ON -DCMAKE_BUILD_TYPE=Release`
- `cmake --build build_metal --target cpim_test_parser cpim_gac_cpu compare_cpu_metal benchmark_metal_gac -j`
- `ctest --test-dir build_metal -R compare_cpu_metal --output-on-failure`
- `./build_metal/benchmark_metal_gac --input=tests/data/metal/gac_bitwords2.xml --runs=5 --warmup=2 --verify=true --csv=out/metal_gac_v13_smoke.csv`
- `./build_metal/benchmark_metal_gac --input=tests/data/bench/queens-4_ext.xml --runs=5 --warmup=2 --verify=true`
- `ctest --test-dir build_metal -R benchmark_metal_gac --output-on-failure`
- `cmake -S . -B build_cpu -DCPIM_ENABLE_CUDA=OFF -DCPIM_ENABLE_METAL=OFF -DCMAKE_BUILD_TYPE=Release`
- `cmake --build build_cpu --target cpim_gac_cpu -j`
- `python3 codex-docops-logic/scripts/dol.py lint --soft`
- `python3 codex-docops-logic/scripts/dol.py solve --stub --mode check`
- `git diff --check`

**兼容性与语义**：
- 不新增统一 `--backend`，不改变 CPU/CUDA CLI；
- 不实现 `MTLStorageModePrivate`、simdgroup/texture 优化或 SAC 数据流；
- CUDA/Jetson 回归仍需在 CUDA 机器上单独执行。

### Metal v1.1：正确性加固与自动回归

**目标**：把 Metal GAC 从可运行 MVP 加固为可自动回归的 correctness backend，
仍只覆盖 GAC，不进入 MAC/SAC/Batch。

**核心改动**：
- `MetalGacStats` 新增 `device_name`，`compare_cpu_metal` 输出
  `[Metal] device=<name> ...`，便于确认使用的是 macOS Metal device。
- 调整 CPU/Metal 对比语义：
  - 先比较 inconsistent 判定；
  - 两边都 inconsistent 时只检查结果形状，不强制逐 word 对齐最终 `bit_dom`；
  - 两边都 consistent 时继续严格比较 `domain_sizes` 与 `bit_dom`；
  - `budget_exceeded` 仍视为失败。
- 新增 Metal correctness fixtures：
  - `tests/data/metal/gac_inconsistent.xml`：二元 supports 链触发空域；
  - `tests/data/metal/gac_bitwords2.xml`：域大小 40，覆盖 `bit_words=2`。
- `CMakeLists.txt` 在 `CPIM_METAL_ENABLED` 下注册 Metal CTest：
  `compare_cpu_metal_queens4`、`compare_cpu_metal_deletion`、
  `compare_cpu_metal_inconsistent`、`compare_cpu_metal_bitwords2`、
  `compare_cpu_metal_manifest`。
- 更新 `docs/planning/METAL_MIGRATION_PLAN.md` 与 `docs/README.md`，记录 v1.1
  状态、fixtures、CTest 策略与 inconsistent 对比语义。

**验证**：
- `cmake -S . -B build_metal -DCPIM_ENABLE_CUDA=OFF -DCPIM_ENABLE_METAL=ON -DCMAKE_BUILD_TYPE=Release`
- `cmake --build build_metal --target cpim_test_parser cpim_gac_cpu compare_cpu_metal -j`
- `./build_metal/compare_cpu_metal --input=tests/data/bench/queens-4_ext.xml`
- `./build_metal/compare_cpu_metal --input=tests/data/bench/test.xml`
- `./build_metal/compare_cpu_metal --input=tests/data/metal/gac_inconsistent.xml`
- `./build_metal/compare_cpu_metal --input=tests/data/metal/gac_bitwords2.xml`
- `./build_metal/compare_cpu_metal --input=tests/data/bench/BMPath.xml`
- `ctest --test-dir build_metal -R compare_cpu_metal --output-on-failure`
- `cmake -S . -B build_cpu -DCPIM_ENABLE_CUDA=OFF -DCPIM_ENABLE_METAL=OFF -DCMAKE_BUILD_TYPE=Release`
- `cmake --build build_cpu --target cpim_gac_cpu -j`
- `python3 codex-docops-logic/scripts/dol.py lint --soft`
- `python3 codex-docops-logic/scripts/dol.py solve --stub --mode check`
- `git diff --check`

**兼容性与语义**：
- 不新增统一 `--backend`，不改变 CPU/CUDA CLI；
- 不抽 `MetalRuntime`，runtime 抽象推迟到 Metal v1.2；
- CUDA/Jetson 回归仍需在 CUDA 机器上单独执行。

## 2026-05-01

### Metal v1：独立运行时 + DeviceLayout 的 GAC MVP

**目标**：在 Apple Silicon macOS 上落地一条可运行的 Metal GAC correctness
路径，同时保持 Jetson/CUDA 主线可选启用、不改造 `GModel/GModelSolver`。

**核心改动**：
- `CMakeLists.txt`
  - 将 CUDA 从全局必选改为 `CPIM_ENABLE_CUDA=AUTO|ON|OFF`；
  - 新增 `CPIM_ENABLE_METAL=AUTO|ON|OFF`，在 Apple + Metal 工具链可用时构建
    `compare_cpu_metal`；
  - `libunwind` 改为 optional，`glog` 找不到系统包时通过 FetchContent 拉取。
- 新增后端无关 `DeviceModelLayout`：
  - 使用纯 C++ `DeviceInt2`、`DeviceUInt2`、`DeviceUInt3`；
  - 从归一化 `IntermediateModel` 构建 `bit_dom`、`domain_sizes`、`bit_sup`、
    constraint scope 与 subscription CSR。
- 新增 Metal GAC 后端：
  - `MetalGacSolver` 使用 `MTLStorageModeShared` buffer 和 host 多轮 dispatch；
  - `.metal` kernel 采用标量 atomic 路径处理 `(constraint, direction, value)`；
  - 不使用 CUDA texture、warp ballot 或 cooperative grid sync 的直接替代。
- 新增 `compare_cpu_metal`：
  - 支持直接 XML 和 bench manifest 首个实例；
  - 对比 CPU/Metal 的 `domain_sizes` 与 `bit_dom`，不一致时返回非 0。
- `GacCpuRunner` 增加只读结果访问器，并按 bitset 容量扫描域值，避免删除低位后跳过高位。
- `include/Timer.h` 在无 CUDA runtime 时只暴露 CPU timer，解除 Mac CPU 构建对 CUDA SDK
  的头文件依赖。
- 修正 `tests/data/bench/BMPath.xml` 的示例路径，使其指向当前存在的
  `tests/data/bench/queens-12_ext.xml`。

**验证**：
- `cmake -S . -B build_metal -DCPIM_ENABLE_CUDA=OFF -DCPIM_ENABLE_METAL=ON -DCMAKE_BUILD_TYPE=Release`
- `cmake --build build_metal --target cpim_test_parser cpim_gac_cpu compare_cpu_metal -j`
- `./build_metal/compare_cpu_metal --input=tests/data/bench/queens-4_ext.xml`
- `./build_metal/compare_cpu_metal --input=tests/data/bench/test.xml`
- `./build_metal/compare_cpu_metal --input=tests/data/bench/BMPath.xml`
- `./build_metal/cpim_test_parser --bench_path=tests/data/bench/queens-4_ext.xml`
- `./build_metal/cpim_gac_cpu --input=tests/data/bench/queens-4_ext.xml --max_print=2`

**兼容性与语义**：
- Metal v1 只支持归一化后的二元 extension supports 约束；
- CUDA target 仍保留，但本机未执行 Jetson/CUDA 验证；
- 不新增统一 `--backend` CLI，不实现 MAC/SAC/Batch Metal 路径。

### 文档：新增 macOS Metal 统一内存迁移计划

**目标**：明确从 Jetson/CUDA UMA 路径迁移到 Apple Silicon Metal 后端的工程路线，
并把迁移边界限定为“先文档计划、后代码落地”，避免直接替换 CUDA 造成主线风险。

**核心改动**：
- 新增 `docs/planning/METAL_MIGRATION_PLAN.md`：
  - 梳理当前 CUDA/Jetson UMA 依赖点，包括 CMake、`GModel.cuh`、`gmodel_adapter.cu`、
    `GModel.cu` 与 Batch/SAC manager。
  - 给出 Metal 目标架构：`MTLBuffer` shared/private 策略、Metal backend 目录草案、
    `.metal` kernel 与 Objective-C++/metal-cpp 调用层取舍。
  - 定义分阶段路线：Mac CPU 可构建、后端类型隔离、Metal GAC MVP、SAC/Batch 迁移、
    性能对照。
  - 明确 `cudaMallocManaged`、CUDA texture、warp ballot、cooperative grid sync 等机制
    不能机械替换，需要按 Metal 语义重新设计。
- 更新 `docs/README.md`：在关键规划文档中注册 Metal 迁移计划入口。

**兼容性与语义**：
- 本次仅新增/更新文档；
- 不修改 CMake、C++、CUDA、public API、CLI flag 或测试代码；
- Metal 相关接口名仅作为后续设计草案。

## 2026-02-21

### FQ-PT：OW5 O5-C 收敛优化（auto policy + 两层 gate）

**目标**：在不改 public 接口的前提下，降低 OW5 热路径固定开销，并通过
运行时策略覆盖（tile/stealing）提升大样例 gate 表现。

**核心改动**：
- `src/solver/gpu/GModel.cu`
  - 新增 `bit_dom_int_size==1` 专用快路径：
    - `ExecuteConstraintCheck_BpC_Workspace_SubwarpPerWorld_BitDom1Fast(...)`
    - 仅 subwarp leader 执行删值与 dom_size 更新，其它 lane 不参与计算。
  - OW5 world stealing 新增 warp 本地批量领取缓存：
    - 通过 shared chunk (`base/next/lock`) 降低 `world_cursor` 全局原子频率。
  - OW5 主循环移除冗余局部同步（仅保留必要同步），保持语义不变。
- `src/solver/gpu/batch_probe_manager.cu`
  - 新增 OW5 运行时策略缓存（manager 私有）：
    - `ow5_policy_ready/effective_tile/effective_stealing`。
  - 首次 OW5 执行增加轻量校准（A/B/C 三候选）：
    - A: `(tile=8, stealing=0)`
    - B: `(tile=16, stealing=0)`
    - C: `(tile=8, stealing=1)`
    - 使用 CUDA event 计时，选择最快配置并在后续复用。
  - OW5 开启时允许覆盖用户 `tile/stealing` 设置，并在每次 `Execute()` 最多打印一次：
    - `OW5 policy override: requested -> effective`
- `tests/python/ow5_gate_eval.py`
  - 增加“两层门控”：
    - quick gate（3例，阈值 `median<=1.03`）
    - quick 通过后才跑 full gate（9+3）
  - `main.csv` 增加 `stage=quick/full` 字段。
  - `summary.md` 区分 quick/full 结果与是否跳过 full。

**兼容性与语义**：
- 不新增/删除 `FQPTControl` / `FQPTStatistics` / CLI 字段；
- OW5 仍 `default-off`；
- Stage2 / OW3 / OW4 路径不变；
- `UNKNOWN` 语义不变。

### FQ-PT：OW5 Subwarp Multi-World 主线接入（default-off）

**目标**：在 FQPT Owner-World 路径落地 OW5（subwarp multi-world），
用于 `bit_dom_int_size==1` 场景的 lane 利用率提升与 OW3 转轨评估。

**核心改动**：
- `include/solver/gpu/batch_probe_manager.h`
  - `FQPTControl` 新增：
    - `enable_subwarp_multiworld`
    - `subwarp_tile_size`（4/8/16）
  - `FQPTBaselineManager` 新增 setter：
    - `SetEnableSubwarpMultiworld(bool)`
    - `SetSubwarpTileSize(int)`
- `src/solver/gpu/batch_probe_manager.cu`
  - `LaunchKernel()` 新增 OW5 优先级：
    - `effective_subwarp = owner && ow5 && (bit_dom_int_size==1)`
    - `effective_microbatch = owner && ow3 && !effective_subwarp`
  - `world_stealing` 与 OW5 可并存（不再因 OW5 被关闭）。
  - `OW5+OW3`、`OW5+OW1`、`OW5 在非 small-domain` 时增加 warning 诊断。
- `src/solver/gpu/GModel.cu`
  - `FQPTOwnerFrontierKernel(...)` 新增分支优先级：
    - `OW5 -> OW3 -> OW2/OW1`
  - 新增 `ExecuteConstraintCheck_BpC_Workspace_SubwarpPerWorld(...)`：
    - subwarp 内局部同步（`__syncwarp(subwarp_mask)`）
    - 无 `__syncthreads()` 热路径
    - 保持与现有 check 语义一致（删值、DWO、push 触发）
  - OW5 分支采用 subwarp world 领取（stealing 或静态 stride），
    并禁用 OW1 scatter（统一 leader 标量 push）。
  - Owner kernel launch 的 shared memory 按
    `block_warps * worlds_per_warp` 扩展 scratch 槽位。
- `apps/sac_benchmark.cpp` / `tests/cpp/test_fqpt_baseline.cpp`
  - 新增 CLI：
    - `--fqpt_enable_subwarp_multiworld`
    - `--fqpt_subwarp_tile`
  - benchmark 模式名新增：
    - `FQ-PT(OWF+OW5t4|t8|t16)`

**统计口径**：
- 不新增 public 统计字段；
- OW5 继续复用 `checks/deletions/processed/frontier_*`；
- microbatch 统计在 OW5 路径保持 0（OW5 优先后 OW3 不生效）。

**语义与回退**：
- default-off；
- Stage2 路径不变；
- `UNKNOWN` 语义不变；
- 若 OW5 gate 失败，保持实验开关，不改变默认主路径。

### FQ-PT：OW3-lite 无轮次同步抢救路径（default-off）

**目标**：保留 OW3 同 `cid` 机会复用方向，同时移除 O3B-B/O3B-C 的 CTA 轮次锁步框架，
把高频 block 同步成本降到热路径之外。

**核心改动**：
- `src/solver/gpu/GModel.cu`
  - `FQPTOwnerFrontierKernel(...)` 的 `enable_cid_microbatch=1` 分支重构为
    warp 独立推进 + 机会主义分组（OW3-lite）：
    - 删除旧的 CTA round 驱动子循环与 `mb_cta_active_count` 锁步调度；
    - 使用 `mb_live_seq/mb_live_round/mb_live_cid` 的 seqlock 快照做无 barrier 观察；
    - 每个 warp 对自身 `candidate cid` 独立执行，不再 parked，不再等待同轮对齐；
    - `microbatch_max_rounds` 改为按 world 的 aligned 事件上限，触发本地 degrade 计数；
    - OW3-lite 热路径不新增 `__syncthreads()`。
- `src/solver/gpu/batch_probe_manager.cu`
  - `LaunchKernel()` 允许 `world_stealing` 与 `cid_microbatch` 共存下发（不再互斥）。
  - 低对齐率诊断文案更新为 `OW3-lite`。

**统计口径说明**：
- `microbatch_aligned_rounds` / `microbatch_degrade_rounds`：
  由“CTA round”口径调整为“warp 执行事件”口径。
- `microbatch_parked_warps`：
  为兼容保留字段；OW3-lite 路径固定累计 0。

**语义与回退**：
- 继续 default-off（需显式 `--fqpt_enable_cid_microbatch=1`）；
- `UNKNOWN` 语义不变；
- Stage2/OW2/OW1 主路径不变；
- 若 OW3-lite gate 不达标，按计划转 OW5 主线。

## 2026-02-19

### FQ-PT：OW3b O3B-C 大例子驱动性能收敛（default-off）

**目标**：在不改接口与默认语义的前提下，降低 O3B-B 在中大样例上的同步/park 额外开销，
为后续 `OW3b vs OW5` 决策提供更稳定的数据基线。

**核心改动**：
- `src/solver/gpu/GModel.cu`
  - `FQPTOwnerFrontierKernel(...)` 的 `enable_cid_microbatch=1` 分支改为
    “多数对齐 + 混合执行”：
    - 仍保留 `W<=8` 的 `O(W^2)` 选 `sel_cid/sel_count`；
    - 对齐条件改为：
      `sel_cid>=0 && sel_count>=microbatch_min_sel && sel_count*2>=active_selected`；
    - aligned 轮中未命中 warp 不再 parked，直接执行自身 `candidate_cid`；
    - `pending_cid` 从热路径移除（不再参与调度）；
    - `microbatch_max_rounds` 改为 per-warp 本地 fallback，不再触发整轮强制 degrade。
  - OW3a 采样逻辑保持兼容，不重定义统计口径。
- `src/solver/gpu/batch_probe_manager.cu`
  - `CollectResults()` 增加低对齐率诊断日志（`VLOG(1)`）：
    - 条件：`enable_cid_microbatch=1 && microbatch_align_ratio<0.3`
    - 输出：`align_ratio`、`aligned/degrade`、`parked_per_round`、
      `round_cap_fallbacks`。

**语义与回退**：
- 仍保持 default-off，需显式 `--fqpt_enable_cid_microbatch=1`；
- `UNKNOWN` 语义不变；
- Stage2 / OW2 / OW1 路径不变。

### FQ-PT：OW3b O3B-B 严格 CTA 对齐执行（default-off）

**目标**：在 O3B-A 控制面基础上，落地真实 micro-batch 执行逻辑（`sel_cid` 选桶、
`min_sel` 退化、`max_rounds` 止损），并补齐 O3B 统计链路。

**核心改动**：
- `src/solver/gpu/GModel.cu`
  - `FQPTOwnerFrontierKernel(...)` 新增双路径：
    - `enable_cid_microbatch=0`：保留原 OW2/OW3a warp 独立路径；
    - `enable_cid_microbatch=1`：启用严格 CTA lockstep micro-batch 路径。
  - micro-batch 路径要点：
    - CTA 轮次内按 `W=min(block_warps, microbatch_warps)` 做 `O(W^2)` 选 `sel_cid`；
    - `sel_count < microbatch_min_sel` 走 degrade（各 warp 执行自己的 candidate）；
    - `sel_count >= microbatch_min_sel` 走 aligned（仅命中 warp 执行，未命中 parked 到 `pending_cid`）；
    - `microbatch_max_rounds > 0` 且超限时触发 round-cap fallback。
  - O3B 统计在 kernel 内累计并写回：
    - `microbatch_aligned_rounds`
    - `microbatch_degrade_rounds`
    - `microbatch_parked_warps`
    - `microbatch_round_cap_fallbacks`
- `src/solver/gpu/batch_probe_manager.cu`
  - `LaunchKernel()` 采用“micro-batch 优先”：
    - `effective_microbatch = owner && microbatch`
    - `effective_world_stealing = owner && stealing && !effective_microbatch`
  - 同时开启 `stealing + microbatch` 时打印一次 warning，并自动禁用 stealing。
  - `CollectResults()` 新增派生统计：
    - `microbatch_align_ratio`
    - `microbatch_parked_per_round`
- `apps/sac_benchmark.cpp`
  - 输出增加：
    - `mb_align` / `mb_deg` / `mb_park` / `mb_cap` / `mb_ar`
  - 模式名补充 OW3b 后缀（如 `FQ-PT(OWF+OW3b)` / `FQ-PT(OWF+OW1m2+OW3b)`）。

**语义与回退**：
- default-off：需显式 `--fqpt_enable_cid_microbatch=1` 才启用；
- Stage2 路径不变；
- `owner=0 && microbatch=1` 自动忽略；
- `UNKNOWN` 语义保持不变（不引入额外删值路径）。

### FQ-PT：OW3b O3B-A 控制开关与框架接线（default-off）

**目标**：完成 OW3b 的控制面与 kernel 占位框架，保持现有求解语义不变，为 O3B-B
（真实 micro-batch 对齐执行）提供直接落点。

**核心改动**：
- `include/solver/gpu/batch_probe_manager.h`
  - `FQPTControl` 新增：
    - `enable_cid_microbatch`
    - `microbatch_min_sel`
    - `microbatch_warps`
    - `microbatch_max_rounds`
  - `FQPTBaselineManager` 新增 setter：
    - `SetEnableCidMicrobatch(bool)`
    - `SetMicrobatchMinSel(int)`
    - `SetMicrobatchWarps(int)`
    - `SetMicrobatchMaxRounds(int)`
- `src/solver/gpu/batch_probe_manager.cu`
  - `LaunchKernel()` 新增 OW3b 控制字段下发；
  - 仅在 `enable_world_owner=1` 时允许 `enable_cid_microbatch` 生效。
- `apps/sac_benchmark.cpp`
  - 新增 CLI：
    - `--fqpt_enable_cid_microbatch`
    - `--fqpt_microbatch_min_sel`
    - `--fqpt_microbatch_warps`
    - `--fqpt_microbatch_max_rounds`
  - `ConfigureFQPTManager()` 完整接线对应 setter。
- `tests/cpp/test_fqpt_baseline.cpp`
  - 新增同名 OW3b flag 并在 `RunFQPT()` 接线。
- `src/solver/gpu/GModel.cu`
  - `FQPTOwnerFrontierKernel(...)` 增加 OW3b 占位分支：
    - 读取并消费 `enable/min_sel/warps/max_rounds`
    - 当前仅预留框架，不改变现有单 warp check 路径
    - 不引入 block 级同步

**语义与回退**：
- `default-off`：不带新 flag 时行为与此前版本一致；
- `owner=0 && microbatch=1` 自动忽略；
- `owner=1 && microbatch=1` 当前仅走框架占位，结果语义保持一致。

### FQ-PT：OW2 O2-A 接口与 `world_stealing` 控制面接线（default-off）

**目标**：仅完成 OW2 前置控制面（接口/flag/Host 下发）落地，不改 `FQPTOwnerFrontierKernel`
现有静态 world 分配行为，为后续 O2-B（kernel 动态领取）提供无歧义接入点。

**核心改动**：
- `include/solver/gpu/batch_probe_manager.h`
  - `FQPTControl` 新增：
    - `enable_world_stealing`
    - `world_cursor`
  - `FQPTBaselineManager` 新增：
    - `SetEnableWorldStealing(bool)`
    - 成员 `enable_world_stealing_`
    - 成员 `d_world_cursor_`
- `src/solver/gpu/batch_probe_manager.cu`
  - 新增 `SetEnableWorldStealing(bool)`（仅更新布尔开关，不触发重分配）
  - `AllocateMemory()` 新增 `d_world_cursor_` 分配与初始化
  - `FreeMemory()` 新增 `d_world_cursor_` 释放
  - `Execute()` 每次执行前清零 `d_world_cursor_`
  - `LaunchKernel()` 下发：
    - `d_control_->enable_world_stealing`
    - `d_control_->world_cursor`
- `apps/sac_benchmark.cpp`
  - 新增 CLI：`--fqpt_enable_world_stealing`
  - `ConfigureFQPTManager()` 接线 `SetEnableWorldStealing(...)`
- `tests/cpp/test_fqpt_baseline.cpp`
  - 新增 CLI：`--fqpt_enable_world_stealing`
  - `RunFQPT()` 接线 `SetEnableWorldStealing(...)`

**语义与回退**：
- O2-A 不改 kernel 调度行为（仍走 OW0 静态分配）；
- `--fqpt_enable_world_stealing=1` 且 `--fqpt_enable_world_owner=0` 时安全忽略；
- 继续保持 default-off、可回退、Stage2/legacy 默认行为不变。

### FQ-PT：OW2 O2-B 动态 world 领取内核分支（default-off）

**目标**：在 OWF kernel 内启用 `world_cursor` 动态领取分支，缓解静态 stride 在 world 工作量不均时的长尾空转。

**核心改动**：
- `src/solver/gpu/GModel.cu`
  - `FQPTOwnerFrontierKernel(...)` 中将 world 外层循环改为统一 `while` 框架：
    - `enable_world_stealing=0`：沿用 OW0 静态 `next_world += world_stride`
    - `enable_world_stealing=1`：`lane0` 通过 `atomicAdd(world_cursor, 1)` 领取 world 并 warp 广播
  - world 内 Phase0/传播/统计逻辑保持不变，避免引入语义漂移。

**语义与回退**：
- 仅当 `enable_world_owner=1 && enable_world_stealing=1 && world_cursor!=nullptr` 才走动态领取；
- 其余场景自动回退 OW0 静态映射；
- 不引入 `world_lock`，保持 Owner-World 单写者约束与 soundness。

### FQ-PT：OW3a O3A-A 统计字段与输出接线（default-off）

**目标**：先完成 OW3a 命中率统计的控制字段与输出通路接线；本阶段不实现 kernel 采样逻辑，
确保后续 O3A-B 仅需补充设备端计数更新。

**核心改动**：
- `include/solver/gpu/batch_probe_manager.h`
  - `FQPTControl` 新增：
    - `enable_cid_microbatch_profile`
    - `microbatch_profile_interval`
    - `microbatch_rounds` / `microbatch_sel_ge2_rounds` / `microbatch_sel_sum`
  - `FQPTStatistics` 新增：
    - `microbatch_rounds`
    - `microbatch_sel_ge2_rounds`
    - `microbatch_sel_sum`
    - `avg_sel_count`
  - `FQPTBaselineManager` 新增 setter：
    - `SetEnableCidMicrobatchProfile(bool)`
    - `SetMicrobatchProfileInterval(int)`
- `src/solver/gpu/batch_probe_manager.cu`
  - 新增三项 device 计数内存分配/释放/清零
  - `LaunchKernel()` 下发 OW3a 控制字段与统计指针
  - `CollectResults()` 汇总 `microbatch_*` 并计算 `avg_sel_count`
- `apps/sac_benchmark.cpp`
  - 新增 CLI：
    - `--fqpt_enable_cid_microbatch_profile`
    - `--fqpt_microbatch_profile_interval`
  - benchmark 结果新增并打印：
    - `mb_r` / `mb_ge2` / `mb_sel` / `mb_avg`
- `tests/cpp/test_fqpt_baseline.cpp`
  - 新增上述 OW3a flag 并接入 manager

**语义与回退**：
- 本阶段不改 kernel 执行路径，统计默认值保持 0；
- 继续保持 default-off，可通过 flag 一键关闭；
- O3A-B 将在此基础上补充实际采样更新逻辑。

### FQ-PT：OW3a O3A-B 内核命中率采样更新（default-off）

**目标**：在 OWF kernel 内补齐 OW3a 的实际采样逻辑，更新 `microbatch_*` 统计计数，
继续保持不改变 check/commit 语义。

**核心改动**：
- `src/solver/gpu/GModel.cu`
  - 在 `FQPTOwnerFrontierKernel(...)` 中新增 OW3a 采样分支：
    - 仅在 `enable_cid_microbatch_profile=1` 且统计指针有效时启用
    - 以 `microbatch_profile_interval` 对 `local_frontier_pops` 做间隔采样
    - 每次采样记录当前 warp 的 `(round, cid)`，扫描同 CTA 的 warp 快照估算 `sel_count`
    - 原子更新：
      - `microbatch_rounds += 1`
      - `microbatch_sel_sum += sel_count`
      - `microbatch_sel_ge2_rounds += (sel_count >= 2)`
  - 采样逻辑仅发生在 lane0，不改变现有 OWF 的 world 调度、check 执行与结果提交路径。

**语义与回退**：
- 仍为 default-off（需显式 `--fqpt_enable_cid_microbatch_profile=1`）；
- 统计仅在启用时更新，禁用时路径与 O2-B 一致；
- O3A-B 仅提供命中率观测，不引入 OW3b 的 micro-batch 对齐执行。

## 2026-02-18

### FQ-PT：OW1 实验迭代（mode=2 + 可观测化，允许退化）

**目标**：将 OW1 从“单一优化开关”扩展为可扫频实验框架，允许阶段性性能退化用于摸索，
但保持正确性硬约束与 default-off 回退能力。

**核心改动**：
- `include/solver/gpu/batch_probe_manager.h`
  - `FQPTControl` 新增：
    - `ow1_scatter_mode`（`0=OW0 fallback, 1=OW1, 2=OW1_v2(match_any)`）
    - `ow1_force_scatter`（忽略 `ow1_min_degree` 强制 scatter）
    - `ow1_scatter_calls`、`ow1_fallback_calls`、`ow1_word_leader_writes`
  - `FQPTStatistics` 新增上述三项 host 统计镜像
  - `FQPTBaselineManager` 新增 setter：
    - `SetOW1ScatterMode(int)`
    - `SetOW1ForceScatter(bool)`
- `src/solver/gpu/batch_probe_manager.cu`
  - 分配/释放/清零 OW1 新统计计数器
  - `LaunchKernel()` 下发 `ow1_scatter_mode/ow1_force_scatter`
  - `CollectResults()` 汇总 `ow1_*` 统计
- `apps/sac_benchmark.cpp`
  - 新增 CLI：
    - `--fqpt_ow1_scatter_mode`
    - `--fqpt_ow1_force_scatter`
  - 输出新增统计：
    - `ow1_sc`（scatter 调用次数）
    - `ow1_fb`（fallback 调用次数）
    - `ow1_w`（leader 写回 frontier word 次数）
  - 模式名支持 `FQ-PT(OWF+OW1m2)` 以区分 `mode=2`
- `tests/cpp/test_fqpt_baseline.cpp`
  - 新增 OW1 实验参数接线，覆盖 `mode/force` 切换场景
- `src/solver/gpu/GModel.cu`
  - 新增 `FQPTPushVarNeighborsToFrontierTwoLevelWarpMatchAny(...)`（OW1_v2）
  - 新增统一分流函数 `FQPTPushVarNeighborsToFrontierTwoLevelDispatch(...)`
  - OWF 的 seed 与 `x_changed/y_changed` 回写统一走分流逻辑：
    - `mode=0` 始终 lane0 fallback
    - `mode=1/2` 按 `ow1_force_scatter` 与 `ow1_min_degree` 决定 scatter/fallback
  - 仅使用 warp 级原语；不引入 block barrier 到 OWF 主循环

**语义与回退**：
- OW1 继续 default-off（需显式开启 `--fqpt_enable_ow1_frontier_scatter=1`）
- Stage2 与 legacy FQ-PT 默认行为不变
- soundness 不变：`UNKNOWN` 语义不变，正确性门槛不放松

### FQ-PT：新增 OW1（S3）frontier 邻接写回优化（default-off）

**目标**：降低 OWF 路径在高 degree 变量上的 `lane0` 串行邻接写回瓶颈，
仅优化 frontier 回写，不引入 S1/S2 变更，保持归因清晰。

**核心改动**：
- `include/solver/gpu/batch_probe_manager.h`
  - `FQPTControl` 新增：
    - `enable_ow1_frontier_scatter`
    - `ow1_min_degree`
  - `FQPTBaselineManager` 新增 setter：
    - `SetEnableOW1FrontierScatter(bool)`
    - `SetOW1MinDegree(int)`
- `src/solver/gpu/batch_probe_manager.cu`
  - `LaunchKernel()` 下发 OW1 控制字段；
  - 仅在 `enable_world_owner` 下允许 OW1 生效，其它路径保持关闭。
- `src/solver/gpu/GModel.cu`
  - 保留 `FQPTPushVarNeighborsToFrontierTwoLevel(...)` 作为 fallback；
  - 新增 warp 协作写回逻辑：按 `word` 聚合同轮 lane bit，
    由 leader 一次写回 L0/L1 frontier；
  - 调用分流：`enable_world_owner && enable_ow1_frontier_scatter && degree>=ow1_min_degree`
    走 OW1，否则走 OW0 旧路径。
- `apps/sac_benchmark.cpp` / `tests/cpp/test_fqpt_baseline.cpp`
  - 新增 CLI：
    - `--fqpt_enable_ow1_frontier_scatter`
    - `--fqpt_ow1_min_degree`
  - benchmark 模式名区分 `FQ-PT(OWF+OW1)` 与 `FQ-PT(OWF)`。

**语义与回退**：
- OW1 默认关闭（default-off）；
- 仅在 OWF 路径启用，保持 Stage2 与 legacy FQ-PT 默认行为不变；
- 保持 soundness：`UNKNOWN` 语义与 DWO/OK 判定逻辑不变。

### FQ-PT：新增 OW0（Owner-World + Two-Level Frontier）路径（default-off）

**目标**：在 `--mode=fqpt` 下引入 Owner-World 执行路径，移除全局 `(world,cid)` MPMC 与 `world_lock`
在该路径上的热冲突，把瓶颈从控制面迁回约束检查算子。

**核心改动**：
- `src/solver/gpu/GModel.cu`
  - 新增 `FQPTOwnerFrontierKernel` 与 `LaunchFQPTOwnerFrontierKernelWrapper(...)`；
  - 采用 `owner(world)=world%gridDim.x` 的 warp-per-world 静态映射；
  - 使用 two-level frontier（`frontier_A` 作为 L0，`frontier_B` 作为 L1）做摊销 O(1) pop；
  - 初始化下沉到 device Phase0（snapshot 恢复、singleton assign、seed frontier）；
  - OW0 路径仅调用 warp-only 检查函数（不进入 block-sync 检查路径）。
- `include/solver/gpu/batch_probe_manager.h` / `src/solver/gpu/batch_probe_manager.cu`
  - `FQPTControl` 新增 `enable_world_owner` 与 frontier 统计指针：
    `frontier_pop_count`、`frontier_scan_steps`；
  - `FQPTBaselineManager` 新增 `SetEnableWorldOwner(bool)`；
  - `LaunchKernel()` 新增 owner/legacy 双路径分流；
  - owner 路径下 ring/lock 指针可为空，统计聚焦 checks/deletions/frontier 扫描。
- `apps/sac_benchmark.cpp`
  - 新增 flag：`--fqpt_enable_world_owner`；
  - `RunFQPTBenchmark` 输出新增 `fpop/fscan`（frontier pop 与平均扫描步数）。
- `tests/cpp/test_fqpt_baseline.cpp`
  - 新增 `--fqpt_enable_world_owner` 参数，支持 correctness 对照测试 OW0 路径。

**语义与回退**：
- 新路径默认关闭（default-off），旧 FQPT ring+lock 路径完整保留；
- soundness 不变：`UNKNOWN` 不删值，`unknown=0` 时仍要求与 Stage2 对齐。

### 文档：新增 OW0 在 RTX 4060/4090 迁移前现状裁决稿

- 新增 `docs/planning/OW0_RTX4060_4090_STATUS_2026_02.md`：
  - 固化 OW0 当前实现边界（开关、分流、统计、回退语义）；
  - 汇总固定 5 例本地实测证据（`stage2/fqpt/owf`）与中位结论；
  - 明确 4060/4090 章节为“推断 + 代码证据”，非远端已测结论；
  - 提供可直接交给其他模型的策略任务书（S1 内存路径、S2 调度路径、S3 内核路径）。
- 更新 `docs/README.md`“关键规划文档”导航，新增上述文档入口。

## 2026-02-17

### FQ-PT：分步实施落地（D0/P0/P1/P2）

**范围**：按 `FQ-PT-CID-CTA-MB` 分步计划落地文档修订与代码实现，保持可回退、可观测、soundness 不变（UNKNOWN 不删值）。

**D0（文档修订）**：
- `docs/planning/SACGPU_NEW_CODEX_REVIEW_2026_02.md`
  - 明确 Phase1 可能平收益（`-2%~+2%`）的预期边界；
  - 增加 Phase1 -> Phase2 的 stop/go 条件；
  - 新增 back-of-envelope 估算（`R/B/W` 指标 + 低/中/高聚合三档）；
  - 附录锚点改为“函数名优先 + 行号提示”。

**P0（观测闭环与开关）**：
- `include/solver/gpu/batch_probe_manager.h`
  - `FQPTControl` 新增运行时开关：
    `enable_cid_grouping`、`enable_parallel_group_check`、
    `group_warps_per_cta`、`group_degrade_threshold`
  - `FQPTControl` 新增统计指针：
    `stale_drop_count`、`lock_fail_count`、`lock_retry_count`、
    `bucket_count`、`bucket_task_sum`、`bucket_active_warp_sum`
  - `FQPTStatistics` 新增统计字段：
    `stale_drop_count`、`lock_fail_count`、`lock_retry_count`、
    `avg_bucket_size`、`avg_bucket_utilization`
  - `FQPTBaselineManager` 新增 setter：
    `SetEnableCidGrouping`、`SetEnableParallelGroupCheck`、
    `SetGroupWarpsPerCta`、`SetGroupDegradeThreshold`
- `src/solver/gpu/batch_probe_manager.cu`
  - 新增上述统计计数的分配/释放/清零；
  - Launch 前填充 `FQPTControl` 新开关与统计指针；
  - `CollectResults` 汇总新统计并计算 `avg_bucket_*`。
- `apps/sac_benchmark.cpp`
  - 新增 FQPT 参数：
    `--fqpt_enable_cid_grouping`、`--fqpt_enable_parallel_group_check`、
    `--fqpt_group_warps`、`--fqpt_group_degrade_threshold`
  - benchmark 结果新增 `stale/lock/bucket` 输出与聚合。

**P1（仅分桶 + 退化路径）**：
- `src/solver/gpu/GModel.cu::FQPTBaselineKernel`
  - 加入 CTA-local 线性分桶（thread0 选取最热 `cid`）；
  - `max_bucket_size <= group_degrade_threshold` 时退化为逐任务路径；
  - 分桶统计埋点：`bucket_count/task_sum/active_warp_sum`；
  - 保持原 block 级检查语义与 commit 单点提交。

**P2（并行检查路径）**：
- `src/solver/gpu/GModel.cu`
  - 新增 `ExecuteConstraintCheck_BpC_Workspace_WarpPerWorld`（warp 级检查）；
  - `FQPTBaselineKernel` 新增“分桶后并行检查”路径：
    - 每 warp 处理 1 个 world；
    - `check_shared` 按 warp 切片；
    - 每 warp 产出 `PropagateResult`，thread0 统一 commit；
  - legacy 小域仍保留原路径回退（并行路径默认要求 `bit_dom_int_size > 1`）。
- `src/solver/gpu/GModel.cu::LaunchFQPTBaselineKernelWrapper`
  - shared memory 计算改为按 `check_words_per_warp * check_warp_slots` 动态估算，
    支持并行分组场景。

**语义保证**：
- 去重标记、pending 结算、lock/retry、UNKNOWN 语义保持不变；
- 新路径默认由开关控制，可一键回退到旧路径。

**验证与门槛判定（固定 5 例 + `--num_probes=64 --warmup=1 --iterations=5`）**：
- 构建与正确性：
  - `cmake .. && make -j$(nproc)` 通过；
  - `ctest --test-dir build -R test_fqpt_baseline --output-on-failure` 通过；
  - 5 例 `test_fqpt_baseline --input=<case> --num_probes=64` 全部通过。
- 观测闭环：
  - `sac_benchmark --mode=fqpt` 新字段可稳定输出并可解析（包含 0 值）：
    `unknown/checks/stale/lock_fail/lock_retry/bsz/butil`。
- P1 gate（以每样例 3 次重复的中位数判定）：
  - `median(P1 vs P0)=+0.43%`（满足“median 回退 <=3%”）；
  - 但 `haystacks-11` 出现 `+8.79%` 回退（不满足“单例 <=5%”）；
  - 聚合信号达标（`bsz>=1.4` 与 `butil>=0.35` 均为 `5/5`）。
  - 结论：**P1 作为 default-off 消融路径保留，不满足直接进入默认开启条件**。
- P2 gate（相对 P1）：
  - `median(P2 vs P1)=-3.79%`（有提升但未达到 `>=8%`）；
  - 结论：**并行路径默认保持关闭，仅保留开关用于后续调优/消融**。

### 文档：FQ-PT vs Batch2 归因报告（可转发版）

- 新增/重写 `gemini_doc/SACGPU_MB_implementation_review.md`：
  - 固化 P0/P1/P2 全面慢于 Batch2 的实测证据（固定 5 例口径）；
  - 汇总关键根因（任务粒度、队列/锁、分桶 O(n²)、`bit_dom_int_size==1` 门控）；
  - 给出可执行优化路线（A 止损、B 调度面重构、C 小域并行增强）；
  - 附关键代码片段，供外部模型快速复核。

## 2026-02-16

### Docs：新增 SACGPU 新方案独立裁决文档（Codex 版）

**动机**：`docs/planning/SACGPU_new.md` 与 `gemini_doc/SACGPU_new_review.md` 的观点存在交叠与表述强弱不一，
需要一份“可执行裁决稿”统一结论、落地顺序与风险边界，减少后续实现分歧。

**交付内容**：
- 新增 `docs/planning/SACGPU_NEW_CODEX_REVIEW_2026_02.md`：
  - 明确评审口径为“静态代码证据、无运行数据”；
  - 对 FQ-PT-CID-CTA-MB 方案给出逐项裁决（成立/部分成立/不成立）；
  - 固化分阶段路线（Phase 0~3）、启停条件、回退条件与验收门槛；
  - 补充“本次仅文档改动，后续接口扩展建议未实施”的边界说明；
  - 附关键代码锚点（`GModel.cu`、`batch_probe_manager.h/.cu`、`test_fqpt_baseline.cpp`）。
- 更新 `docs/README.md`：
  - 在“关键规划文档”中注册新文档入口，说明其用途为
    “FQ-PT-CID-CTA-MB 独立裁决与落地门槛”。

**本次不做**：
- 不修改任何 C++/CUDA 公共接口；
- 不跑 benchmark、不新增 smoke test 输出；
- 不回写清理 `docs/planning/SACGPU_new.md` 的草稿内容。

## 2026-02-11

### FQ-PT Baseline：摊平队列持久线程基线（Task = `(world_id, cid)`）

**动机**：Batch-3A 在低聚合度场景存在较高固定开销。为了建立可对照的“无聚合”动态队列基线，
新增 FQ-PT 路径：复用 ACgpu 约束检查核心，只替换传播基础设施为 GPU 端 persistent blocks + work queue。

**交付内容**：
- `include/solver/gpu/batch_probe_manager.h`：
  - 新增 FQ-PT 数据结构与接口：`FQPTTask`、`FQPTRingSlot(seq+task)`、`FQPTControl`、
    `FQPTStatistics`、`FQPTBaselineManager`、`LaunchFQPTBaselineKernelWrapper(...)`。
- `src/solver/gpu/GModel.cu`：
  - 新增 MPMC ring 原语（每槽 `seq`，避免 MPMC holes）；
  - 明确发布顺序：producer 写 payload 后 `__threadfence()` 再发布 `seq`；
  - 新增 `FQPTBaselineKernel`：persistent blocks 循环消费 `(world_id,cid)`，复用
    `ExecuteConstraintCheck_BpC_Workspace` 做检查/删值，删值后按 subscription 推后继任务；
  - 新增 CTA-local 缓冲（批量 pop + 本地生成缓冲 + 批量 flush）；
  - 新增 world 级互斥锁 `world_locks`，拿锁失败先本地重试；
  - 新增安全退出：`pending==0 && global queue empty && local empty`。
- `src/solver/gpu/batch_probe_manager.cu`：
  - 新增 `FQPTBaselineManager` 实现（快照保存、world 初始化、seed tasks、kernel 启动、结果回收）；
  - 队列溢出可观测：`overflow_count`，并按 world 标记 `UNKNOWN`（不删值，保持 soundness）。
- `apps/sac_benchmark.cpp`：
  - 新增 `--mode=fqpt`；
  - 新增 FQ-PT 参数：`--fqpt_num_blocks/--fqpt_queue_capacity/--fqpt_pop_batch/--fqpt_local_buffer/--fqpt_lock_retry/--fqpt_lock_backoff`；
  - 输出 `unknown/overflow/checks` 等统计字段。
- 测试与构建：
  - 新增 `tests/cpp/test_fqpt_baseline.cpp`（Stage2 对照：无 UNKNOWN 时结果全等；有 UNKNOWN 时仅要求 FQPT 的 DWO 为 Stage2 子集）；
  - `CMakeLists.txt` 新增 `test_fqpt_baseline` 目标与 `ctest` 注册。

### FQ-PT：修复高并发下偶发 timeout（pending 计数竞态）

**问题现象**：`sac_benchmark --mode=fqpt` 在中等实例/较高 block 数下偶发卡住；`test_fqpt_baseline`
也可能触发超时。

**根因**：
- 生成任务时先发布到队列、后执行 `pending++`，在高并发下可能出现“消费者先完成并 `pending--`”，造成
  `pending` 下溢，退出条件永远不满足；
- 拿锁失败路径中，`status!=OK` 的任务曾存在 pending 递减时序不一致，导致计数不稳。

**修复**（`src/solver/gpu/GModel.cu`）：
- `FQPTFlushGeneratedBuffer` 调整为“先 `pending += count`，再发布任务；发布失败则回滚 pending”；
- 统一 `status!=OK` 任务在拿锁失败路径中的完成逻辑，确保每个 task 对 pending 只结算一次。

**验证**：
- `ctest --test-dir build -R test_fqpt_baseline --output-on-failure`：通过；
- `sac_benchmark --mode=fqpt` 在 `queens-12` 的 `--fqpt_num_blocks=16/32` 复现用例不再 timeout。

### FQ-PT：`(world,cid)` 去重入队（降低重复检查风暴）

**问题现象**：即使无 timeout，FQ-PT 仍存在大量重复任务（`processed_tasks/constraint_checks` 远大于必要值），
导致调度开销居高不下。

**改动**：
- `src/solver/gpu/GModel.cu`：
  - 新增 `FQPTTryMarkConstraintQueued/FQPTIsConstraintQueued/FQPTClearConstraintQueued`；
  - 生成后继任务时先 `mark`，仅在首次入队时追加到 local/global queue；
  - 弹出任务后若发现已是陈旧重复任务（标记已清），直接结算 pending，避免再次检查；
  - 任务完成/世界终止路径补充 `clear mark`，保持标记与队列状态一致。
- `src/solver/gpu/batch_probe_manager.cu`：
  - world 初始化时清零 `frontier_A/B`；
  - seed 初始化阶段按 world 的 `frontier_A` 去重，避免初始重复入队。

**结果（同口径 smoke）**：
- 正确性：`test_fqpt_baseline` 与典型样例回归均通过（`unknown=0` 时与 Stage2 一致）；
- 任务量：`constraint_checks` 约下降 40%~50%
  - `queens-12`: `16896 -> 8448`
  - `rand-2-23`: `64680 -> 32384`
  - `haystacks-11`: `17904 -> 10032`
- 耗时：FQ-PT 相比去重前提升约 10%~30%（仍慢于 Stage2）。

## 2026-02-07

### Docs：新增 Batch-3A Walkthrough（核心思想 / 算法 / 代码实现）

**动机**：现有 Batch-3A 文档偏向“复盘/救火/设计草案”，对首次接手代码的同学不够线性；
需要一份从概念到代码落点的一站式 walkthrough，降低理解门槛与上手成本。

**交付内容**：
- 新增 `docs/planning/BATCH3A_WALKTHROUGH_2026_02.md`，覆盖：
  - Batch-3A 的聚合对象（`<cid, world_mask>`）与关键不变量（world 写入互斥、UNKNOWN 语义）
  - `Batch3AManager::Execute()` → `Batch3AKernel_MultiBlock()` → `CollectResults()` 的端到端执行链
  - 三种 mapping（0/1/2）及其代码入口
  - SAC3 中的 Batch-3A gating / fallback 逻辑与调参、验证命令
- 更新 `docs/README.md`：在“关键规划文档”中注册 walkthrough 入口。

## 2026-02-05

### Batch-3A：Dynamic Submission 队列版（去掉 kernel 内全量扫描）

**动机**：Batch‑3A 扫描版的决定性瓶颈是 kernel 内 `threadIdx.x==0` 每轮全量扫描 `cid=0..num_cons-1`
构建 `local_task_cids/local_task_masks`，成本与 `num_cons`（甚至 `num_cons*worlds`）成正比，完全无法利用稀疏性，
并且会把 mapping=2 的 shared packing/bitGEMM 原型收益淹没（详见复盘：`docs/planning/BATCH3A_POSTMORTEM_2026_01.md`）。

**交付内容**：
- `src/solver/gpu/GModel.cu`：
  - 新增 device 侧队列原语：`Batch3AEnqueueConstraintToNextQueue()` / `Batch3AEnqueueVarToNextQueue()`
  - 重写 `Batch3AKernel_MultiBlock`：用双队列 A/B + per-cid `world_mask` 聚合实现 “结尾提交（Dynamic Submission）”，
    不再进行“开头扫全约束”。
- `include/solver/gpu/batch_probe_manager.h` / `src/solver/gpu/batch_probe_manager.cu`：
  - `Batch3AControl` / `Batch3AManager` 增加 per-block 队列与 mask 缓冲区（`queue_capacity=num_cons` 作为保守起点）
  - kernel launch 前清零队列与 mask；若溢出则保持 `active_world_mask` bit（按 UNKNOWN 语义不删值）。
- 文档同步：
  - 新增 `docs/planning/BATCH3A_DYNAMIC_SUBMISSION_QUEUE_DESIGN.md`
  - `docs/planning/BATCH3A_POSTMORTEM_2026_01.md` 增加“2026-02 更新”说明
  - `docs/README.md` 注册新文档入口

**测试结果**：
- `make -j$(nproc)`：通过
- `python3 tests/python/batch_test_v2.py --tier=0`：8/12 (66%)，与历史基线一致（不匹配项仍为 CPIM 超时）
- `./build/test_batch3a`：通过
- microbench（mapping=2 / perf suite，对照 Stage2）：`out/batch3a_queue_vs_stage2_perf_10min.csv` 中 `speedup_vs_stage2≈0.03–0.12`
  （8×–30× 慢于 Stage2），说明去掉“开头扫全约束”后瓶颈主要转向 host 侧框架成本（`InitializeWorlds` 等）。

### Batch-3A：Phase0 device 初始化（跳过 host InitializeWorlds）

**动机**：Dynamic Submission 去掉了 kernel 内的“开头扫全约束”，但 microbench 仍显示 8×–30× 回退；
进一步定位表明 host 侧 `InitializeWorlds()` 的逐 world `cudaMemcpy/cudaMemset + synchronize` 是主要框架瓶颈之一。

**交付内容**：
- `src/solver/gpu/GModel.cu`：扩展 `Batch3AKernel_MultiBlock` 的 Phase0：
  - device 并行恢复 snapshot：`domain_snapshot → ws->bitDom`，`dom_size_snapshot → ws->d_cur_dom_size`
  - 初始化 `WorldWorkspace` 控制字段与 `results[w]=true`
  - singleton assign + 初始 enqueue（probe var 的 subscription）
  - 队列版不再依赖 `ws->frontier_A/B`，Phase0 不清零 frontier bitmap（避免 O(num_cons) 纯开销）
- `src/solver/gpu/batch_probe_manager.cu`：
  - `Batch3AManager::Execute()` 跳过 `InitializeWorlds()`
  - `Batch3AManager::LaunchBatch3AKernel()` 补齐全局控制初始化（`active_world_mask/global_iteration/stats`）

**测试结果**：
- `make -j$(nproc)`：通过
- `python3 tests/python/batch_test_v2.py --tier=0`：8/12 (66%)，与历史基线一致
- `./build/test_batch3a`：通过
- microbench smoke（perf suite 取 3 个实例，mapping=2，对照 Stage2）：
  - `out/batch3a_queue_phase0init_smoke.csv`：`speedup_vs_stage2≈0.17–0.26`
  - 含义：回退显著收敛，但仍慢于 Stage2，后续仍需继续拆解剩余瓶颈

## 2026-01-29

### P2-2c：Batch-3A microbench 升级为 Stage2 对照（同口径 speedup）

**动机**：此前 P2-2c 只跑 `--mode=batch3a`，只能比较不同 `G/padding` 的相对曲线，无法回答
“Batch-3A（mapping=0/1/2）相对 Stage2 的真实 speedup 在哪些实例上成立”。这会直接影响
“要不要继续推进更重的 SoA/flatten”决策。

**交付内容**：
- `tests/python/batch_batch3a_microbench.py`：
  - 新增 `--include-stage2`：先跑 `--mode=stage2` baseline，再跑 `--mode=batch3a`；
  - 新增 `--mappings/--subwarp-sizes/--padding-values`：支持一次跑全消融组合；
  - CSV 增加 `stage2_avg_time_ms` 与 `speedup_vs_stage2` 字段，便于直接做 go/no-go。
  - 跑通 `suite=perf` 的 10 分钟对照样例（输出 `out/batch3a_vs_stage2_perf_10min.csv`），用于判断是否值得继续推进 SoA/flatten。
  - 跑通 `suite=stress` 的大实例门槛验证（输出 `out/batch3a_vs_stage2_stress_20min.csv`）：
    - `large-80/84`：Batch‑3A（mapping=2）相对 Stage2 的 `speedup_vs_stage2` 约 0.028–0.042（24×–36× 慢）
    - `large-92`：模型构建阶段触发 CUDA OOM（Stage2 baseline 无法获得）

### Suite：perf 哨兵剥离大实例到 stress（避免被解析/建模 wall-time 污染）

**调整**：
- `tests/python/sac_preprocess_tier_definitions.py`：
  - `SAC_PREPROCESS_PERF_SENTINELS` 移除 `benchmarks/marc/large-80-unsat_ext.xml` / `large-84-unsat_ext.xml`
  - 二者移动到 `SAC_PREPROCESS_STRESS`（在 60s perf 预算下容易 timeout，更适合单独拉高 timeout 跑）

### Docs：P2 任务状态与门槛更新

- `docs/planning/TODO_SACGPU_NEXT.md`：
  - 标记 P2-2b/P2-2c 已落地；
  - 补充 “何时进入 SoA/flatten” 的 go/no-go 门槛（基于 `speedup_vs_stage2` 的硬数据）。

### Docs：新增 Batch-3A 性能回退复盘文档（便于外部复核）

- 新增 `docs/planning/BATCH3A_POSTMORTEM_2026_01.md`：汇总 Batch‑3A 在 perf/stress 上结构性慢于 Stage2 的主要瓶颈
  （kernel 内单线程全量扫描构建任务 + host 分批初始化/同步 + mapping=2 pack/unpack 开销），并附关键代码片段与复现实验命令。
- 更新 `docs/README.md`：注册上述复盘文档入口。

## 2026-01-25

### P0-3：统一观测入口落地（full_sac vs SAC1/SAC3 preprocess 同口径）

**动机**：此前 preprocess 跑批主要基于 `sac_benchmark --mode=full_sac`（Stage2 全域扫一轮/多轮），
但 `GModelSolver::EnforceSAC1/EnforceSAC3`（flatten SACQ/NSACQ 的主线实现）没有可直接跑批/可解析的入口，
导致“改了 SAC3 但测不到”，P0-3 统计闭环缺口未补齐。

**交付内容**：
- `apps/sac_benchmark.cpp`：
  - 新增 preprocess 模式：`--mode=sac1_preprocess` / `--mode=sac3_preprocess`
  - 统一输出可解析字段：`Total time/Total probes/Total deletions/Avg/P95/Max iterations/Unknown probes/Status`
- `include/GModelSolver.h` / `src/solver/gpu/GModelSolver.cu`：
  - 新增 `EnableSacProbeStats()` 观测开关与 `GetLastSac*()` 统计读取接口（默认关闭，避免影响搜索阶段）
  - SAC3 增加 `max_rounds` 的 soft-budget 截断（与 `full_sac --max_sac_rounds` 的 TIMEOUT 语义对齐）
- `tests/python/select_sac_preprocess_benches.py` / `tests/python/batch_sac_benchmark.py`：
  - 支持 `--mode=full_sac/sac1_preprocess/sac3_preprocess`
  - CSV 增加 `mode` 与 `p95_iterations` 字段，并改用 `(mode,nsac,max_rounds,path)` 作为 resume key
- 文档同步：
  - 更新 `docs/planning/TODO_SACGPU_NEXT.md`：P0-3 标记完成并说明 A/B 对照路径
  - 更新 `docs/guides/SAC_PREPROCESS_GUIDE.md`：补充 `--mode=sac3_preprocess` 用法与 `P95 iterations` 指标
  - 更新 `docs/planning/SACGPU_NEXT_ACTIONS_10MIN_TIMEOUT.md`：补齐 `--mode` 并移除“P0-3 未完成”的过时段落

**工程修复**：
- `CMakeLists.txt`：`sac_benchmark` 增加链接 `src/solver/gpu/GModelSolver.cu`（否则新增模式会出现链接缺符号）。

**测试结果**：
- `python3 tests/python/batch_test_v2.py --tier=0`：8/12 (66%)，与历史基线一致，无退化（不匹配项仍为 CPIM 超时）。

### P1：外层队列预算 + Stage 选择分桶缓存（长尾可控/并行度吃满）

**动机**：在 flatten SACQ/NSACQ（SAC3 preprocess）路径上，除了 per-probe 的 `UNKNOWN`/停滞/量子外，还需要
host 侧的“外层队列预算”来避免 requeue/queue 爆炸；同时 AutoStageSelector 的单一全局缓存会被首次 batch 的规模污染，
导致后续小 batch 仍走 Stage2（欠饱和/高开销），或反过来大 batch 仍走 Stage1。

**交付内容**：
- `include/GModelSolver.h` / `src/solver/gpu/GModelSolver.cu`：
  - 新增 `GModelSolver::SacQueueBudgetConfig`（`max_total_probes/max_queue_size/max_total_requeues/max_requeues_per_var`）
  - SAC3 支持 `max_total_probes` 截断（达到上限 early_stop；只会少删，不会多删）
  - ProbeQueue 支持 queue cap 与 requeue cap（溢出丢弃入队/停止扩张），并在 verbose 下输出 budget 统计
- `include/solver/gpu/batch_probe_manager.h` / `src/solver/gpu/batch_probe_manager.cu`：
  - `AutoStageSelector::DecideCached()` 改为 **按任务量分桶缓存（medium/large）**
  - 每次调用先走 `DecideByTaskCount()`：小任务直接 Stage1，不再受缓存污染
- `apps/sac_benchmark.cpp`：preprocess 模式增加 queue-budget flags，便于消融与 hard-case 控制

**测试结果**：
- `python3 tests/python/batch_test_v2.py --tier=0`：8/12 (66%)，与历史基线一致，无退化（不匹配项仍为 CPIM 超时）。

### Suite：Regression 用例去重/控时（便于日常跑通）

**动机**：`benchmarks/marc/large-80-unsat_ext.xml` 的 preprocess 本体很快（~200ms），但解析/建模 wall-time 可达 ~60s，
会导致 `--timeout=60` 的 regression 批跑不稳定（即使算法没卡住）。

**调整**：
- `tests/python/sac_preprocess_tier_definitions.py`：
  - `SAC_PREPROCESS_REGRESSION` 移除 `large-80-unsat_ext.xml`（仍保留在 `perf`）
  - 增加 `composed-25-1-2-0_ext.xml` 作为“快速 UNSAT/DWO”回归样例

## 2026-01-27

### P2-1：Batch-3A Route-A（Subwarp-per-World）算子形态试上限

**动机**：Batch-3A 的现有 Warp-per-World 在 `bit_dom_int_size` 较小（例如 6/8/12）时容易出现 lane 利用率低，
但直接做 AoS 下的 “Thread-per-World” 会把 `bitDom` 访问退化成跨 world 的 stride 访存（UMA 上大概率更慢）。
因此先落地 **Subwarp-per-World**（在不改 AoS 布局前提下的低风险向量化），用于评估是否值得推进后续 SoA/packing（P2-2）。

**交付内容**：
- `include/solver/gpu/batch_probe_manager.h` / `src/solver/gpu/batch_probe_manager.cu`：
  - Batch-3A 新增 `check_mapping/subwarp_size`（`kWarpPerWorld` vs `kSubwarpPerWorld`，`subwarp_size=4/8/16`）
  - `Batch3AManager` 增加对应 setter，保持默认不变（便于消融）
- `src/solver/gpu/GModel.cu`：
  - 新增 `ExecuteConstraintCheck_Aggregated_SubwarpPerWorld()` 并在 Batch-3A kernel 中按 `check_mapping` 分发
  - 修复 Batch-3A wrapper 的 dynamic shared memory 计算：仅计入 `bitSup + del_buffer`，避免把静态 shared 数组重复计入
- `apps/sac_benchmark.cpp` / `tests/cpp/test_batch3a.cpp`：
  - 新增 flags：`--batch3a_check_mapping` / `--batch3a_subwarp_size`，用于 A/B 消融对照

**测试结果**：
- `python3 tests/python/batch_test_v2.py --tier=0`：8/12 (66%)，与历史基线一致，无退化（不匹配项仍为 CPIM 超时）。
- `./build/test_batch3a`：`mapping=0` 与 `mapping=1` 均通过，且与 Stage2 结果一致。

### P2-2：Batch-3A Route-B 原型（warp-per-word + lane-per-world + shared dom packing）

**动机**：P2-1 的 Subwarp-per-World 依然受 AoS（`[world][var][word]`）带来的跨 world stride 访存影响，
理论性能上限偏低。P2-2 先落地一个 **不改全局 layout** 的原型：在 shared memory 中把 dom 打包成 `[word][world]` 矩阵，
让热点访问在 block 内变成“像 SoA”。

**交付内容**：
- `include/solver/gpu/batch_probe_manager.h`：`Batch3ACheckMapping` 新增 `kWarpPerWordLaneWorld=2`
- `src/solver/gpu/GModel.cu`：
  - 新增 `ExecuteConstraintCheck_Aggregated_WarpPerWordLaneWorld()`：计算阶段 lane→world，应用阶段保留 warp-per-world
  - Batch-3A dispatch 支持 `mapping=2`
  - wrapper 对 `mapping=2` 调整 `worlds_per_block` 默认值与 dynamic shared memory 预算
- `apps/sac_benchmark.cpp` / `tests/cpp/test_batch3a.cpp`：`--batch3a_check_mapping` 扩展支持 `2`

**测试结果**：
- `python3 tests/python/batch_test_v2.py --tier=0`：8/12 (66%)，与历史基线一致，无退化（不匹配项仍为 CPIM 超时）。
- `./build/test_batch3a --batch3a_check_mapping=2`：与 Stage2 结果一致。

- P2-2 调参入口（G 参数化）：
  - `include/solver/gpu/batch_probe_manager.h` / `src/solver/gpu/batch_probe_manager.cu` / `src/solver/gpu/GModel.cu`：
    - `Batch3AControl` 新增 `requested_worlds_per_block`（0=auto）
    - wrapper 在 `mapping=2` 时支持覆盖 `G`
  - `apps/sac_benchmark.cpp` / `tests/cpp/test_batch3a.cpp`：
    - 新增 `--batch3a_worlds_per_block=0|1..32`（默认 0，不改变现有行为）
  - P2-2c microbench 工具：
    - 新增脚本 `tests/python/batch_batch3a_microbench.py`：批量跑 `./build/sac_benchmark --mode=batch3a` 扫 `G` 并导出 CSV（支持 `--resume`/原子 flush）。
  - P2-2b shared padding：
    - `--batch3a_shmem_padding=0|1`（默认 0；仅对 `mapping=2` 的 shared packing 生效），通过 pitch padding（`G+1`）降低 bank conflict 风险。

- 新增规划文档 `docs/planning/DGPU_MEMORY_PLAN.md`：给出从 Jetson UMA 迁移到 PCIe dGPU（RTX 4060/4090/2080 等）
  的内存后端设计（GPU-resident、pinned+async 双缓冲、delta 删除 GPU 应用、可消融/可回退/可观测），用于后续减少/隐藏 memcpy。
- 更新文档导航 `docs/README.md`：注册 dGPU 内存规划文档入口。

## 2026-01-21

### Preprocess：补齐“回归/性能哨兵”评测集（suite）+ 澄清 SAC vs NSAC 口径

**动机**：tier0/1/2 更偏“展示集”（数据驱动生成、可能随筛选刷新），不一定适合作为“每次改动都跑”的回归与关键节点性能对照。
同时，近期评测 CSV 容易把 `status=TIMEOUT` 误解为 wall-time 超时；并且很多跑批默认 `nsac_mask=1`，
需要在教程里明确“跑的是 SAC 外框 + NSAC 传播半径”。

**交付内容**：
- `tests/python/sac_preprocess_tier_definitions.py`：新增手工精选 suite
  - `SAC_PREPROCESS_REGRESSION`：日常回归覆盖（高删值/高 probes/SAC-DWO/0 删值收敛/较高耗时）
  - `SAC_PREPROCESS_PERF_SENTINELS`：关键节点性能哨兵（吞吐/延迟/内存压力）
  - `SAC_PREPROCESS_STRESS`：压力样例（例如 Jetson 上可能 OOM 的大实例）
  - `SAC_PREPROCESS_KNOWN_BAD`：已知解析/输出问题样例（扫描时建议 exclude）
- `tests/python/batch_sac_benchmark.py`：新增 `--suite` 参数（可直接跑 regression/perf/stress 等）
- `docs/guides/SAC_PREPROCESS_GUIDE.md`：
  - 新增“这里跑的是 SAC 还是 NSAC？”解释
  - 增加 `--suite=regression/perf` 的推荐命令模板

**修改文件**：
- `tests/python/sac_preprocess_tier_definitions.py`
- `tests/python/batch_sac_benchmark.py`
- `docs/guides/SAC_PREPROCESS_GUIDE.md`
- `CHANGES_ZH.md`

## 2026-01-19

### Docs：对齐“下一步工作”与最新评测结论（区分 e2e 与 preprocess 口径）

**背景**：近期评测表明 Batch-3A 在现有用例上 A/B 测试慢 10–25x；同时部分 hard bench 的 e2e TIMEOUT 主要发生在搜索阶段。
但本阶段的研究目标更偏向“preprocess/推理能力（SAC/MSAC/NSAC）的速度与上限”，需要把评测口径从 e2e 求解切回 preprocess，
并挑选“删值多/传播深”的实例。

**交付内容**：
- 更新 `docs/planning/NEXT_STEPS_2026_01.md`：把主线明确为“preprocess 指标闭环（P0-3）+ 构建对 SAC 有意义的评测集”，并把 e2e 搜索仅作为 sanity/回归口径；同时将 Batch-3A/bitGEMM/BMMA 后置为设门槛的研究路线。
- 更新 `docs/planning/TODO_SACGPU_NEXT.md`：在现状快照与 P1-1 条目中补充 Batch-3A 的评测结论，建议默认关闭仅保留消融开关。
- 新增 preprocess 评测集筛选/跑批脚本：
  - `tests/python/select_sac_preprocess_benches.py`：扫描 `benchmarks/` 跑 `sac_benchmark --mode=full_sac`，输出 CSV 并生成 `SAC_PREPROCESS_TIER0/1/2`
    - 支持 `--resume/--flush-every`：可中断/续跑，进度周期性落盘（原子写入）
    - 支持 `--shuffle/--limit/--include/--exclude`：可抽样或按目录定向筛选
  - `tests/python/batch_sac_benchmark.py`：按 preprocess tier 批量运行 `sac_benchmark` 并导出 CSV（支持 `--resume/--flush-every`）
  - `tests/python/sac_preprocess_tier_definitions.py`：预置的 preprocess tier 列表（可用筛选脚本刷新）
- 新增教程并注册到文档导航：
  - `docs/guides/SAC_PREPROCESS_GUIDE.md`
  - `docs/README.md`
  - `docs/planning/SACGPU_NEXT_ACTIONS_10MIN_TIMEOUT.md`

**修改文件**：
- `docs/planning/NEXT_STEPS_2026_01.md`
- `docs/planning/TODO_SACGPU_NEXT.md`
- `docs/guides/SAC_PREPROCESS_GUIDE.md`
- `docs/README.md`
- `docs/planning/SACGPU_NEXT_ACTIONS_10MIN_TIMEOUT.md`
- `tests/python/select_sac_preprocess_benches.py`
- `tests/python/batch_sac_benchmark.py`
- `tests/python/sac_preprocess_tier_definitions.py`

## 2026-01-18

### Search：修复 `time_limit` 超时判断 + 搜索阶段增量 GAC

**目标**：让 `compare_cpu_gpu` 的 GPU 搜索按 `--time_limit` 正确停止；并把每个搜索节点的 GAC
从“全量激活所有约束”改为“只激活刚赋值变量的邻接约束”，降低搜索阶段传播开销（语义不变）。

**交付内容**：
- `GModelSolver::Solve()`：计算 deadline 并传入递归搜索
- `GModelSolver::Search()`：用 `std::chrono::steady_clock` 做超时判断（修复原先每次新建 Timer 导致超时永不触发）
- 搜索节点传播：`model_->EnforceGAC(false, var)`（增量 GAC）

**修改文件**：
- `include/GModelSolver.h`
- `src/solver/gpu/GModelSolver.cu`

### Tests：对齐 TIER1/TIER2 实例数量说明

`tests/python/tier_definitions.py` 的文档注释中，TIER1/TIER2 数量已对齐到当前实际列表
（TIER1=39，TIER2=79），避免后续跑批时误判“缺例/多例”。

**修改文件**：
- `tests/python/tier_definitions.py`

### Tests：新增三方对比（CPIM CPU vs CPIM GPU vs OR-Tools CP）

**目标**：在同一套 TIER 用例下，同时跑 CPU/GPU/OR-Tools 三条链路，便于回归与定位
“CPU/GPU 之间不一致”以及“与 OR-Tools 判定不一致”的实例。

**交付内容**：
- 新增 `tests/python/batch_test_v3.py`：三方对比测试脚本（默认 GPU 走 `build/compare_cpu_gpu --gpu_only`）
- 更新 `docs/planning/TODO_SACGPU_NEXT.md`：最小回归清单加入三方对比入口

**修改文件**：
- `tests/python/batch_test_v3.py`
- `docs/planning/TODO_SACGPU_NEXT.md`

## 2026-01-17

### P0-1d：失败概率优先（Failure Priority / bucketed queue）

**目标**：优先执行“更可能 DWO”的 probes，让删值尽早发生，从而减少后续传播成本并抑制长尾；只改变调度顺序，
不改变 soundness 语义（仍然只对 `kDWO` 删值，`kUNKNOWN` 不删）。

**交付内容**：
- `GModelSolver::FailurePriorityConfig`：运行时开关与权重/桶数配置
- `EnforceSAC3()`：queue mode 下支持按 bucket 出队，并在 verbose 下输出各 bucket 的 DWO 命中率统计
- `apps/compare_cpu_gpu.cpp`：增加参数用于启用/调参：
  - `--sac_failure_priority`
  - `--sac_failure_priority_buckets`
  - `--sac_failure_priority_w_dom / --sac_failure_priority_w_deg / --sac_failure_priority_w_hist`
  - `--sac_failure_priority_min_hist_probes`

**修改文件**：
- `include/GModelSolver.h`
- `src/solver/gpu/GModelSolver.cu`
- `apps/compare_cpu_gpu.cpp`
- `docs/planning/TODO_SACGPU_NEXT.md`

---

### P0-2：NSAC `allowed-constraints mask`（真邻域子图）

**目标**：singleton test 的传播严格限制在 `Xi + N(Xi)` 诱导子图（NSAC），减少传播半径抑制长尾；语义仍然 sound
（只对 `kDWO` 删值，预算/停滞触发为 `kUNKNOWN` 不删）。

**交付内容**：
- `GModel::BuildAllowedMasks()`：为每个 focal variable 预计算 allowed constraints 位图
- Stage2/Stage1 的 BlockSync GAC：在 frontier 扩张（`PropagateVarToNextBitmap`）阶段按 focal var 过滤，避免传播越过邻域子图
- `GModelSolver::NSACMaskConfig` + `--sac_nsac_mask`：运行时开关（关闭回退到原全图传播）

**修改文件**：
- `include/GModel.cuh`
- `src/solver/gpu/GModel.cu`
- `include/GModelSolver.h`
- `src/solver/gpu/GModelSolver.cu`
- `include/solver/gpu/batch_probe_manager.h`
- `src/solver/gpu/batch_probe_manager.cu`
- `apps/compare_cpu_gpu.cpp`
- `docs/planning/TODO_SACGPU_NEXT.md`

**验收**：`batch_test_v2.py --tier=0` 通过（8/12 匹配，与之前一致；4 个超时为既有性能问题）

---

### P1-1：Batch-3A 接入 SAC3 主路径（可选加速器 + NSAC gating）

**目标**：把 Batch-3A（约束聚合）作为 SAC3 的可选后端接入主路径，用于评估其在 `nsac_mask=false` 下的吞吐/长尾收益；
同时保持默认 NSAC 的语义一致性与可回退性。

**关键策略（最小风险）**：
- 运行时开关：`--sac_use_batch3a`
- **NSAC gating**：当 `nsac_mask_enabled=true` 时自动禁用 Batch-3A 并回退 Stage2（保证默认路径始终“真 NSAC”）
- UNKNOWN 输出：Batch-3A 若在 `max_iterations` 内未收敛（`active_world_mask` 仍有 bit），对这些 probes 标记为 UNKNOWN（不删，可进入 deferred recheck）

**交付内容**：
- `GModelSolver::Batch3AConfig`：Batch-3A 运行时配置
- `GModelSolver::EnforceSAC3()`：在满足条件时走 Batch-3A，否则回退原 Stage1/Stage2
- `Batch3AManager::Execute(..., unknown_vars, unknown_values)`：支持返回 UNKNOWN probes
- 修复 Batch-3A 初始化 `active_world_mask` 在 `num_worlds==32` 时的移位未定义行为

**修改文件**：
- `include/GModelSolver.h`
- `src/solver/gpu/GModelSolver.cu`
- `include/solver/gpu/batch_probe_manager.h`
- `src/solver/gpu/batch_probe_manager.cu`
- `apps/compare_cpu_gpu.cpp`
- `docs/planning/TODO_SACGPU_NEXT.md`

---

## 2026-01-16

### P0-1：显式 UNKNOWN 语义与统计闭环（基础设施）

**背景**：根据 `docs/planning/TODO_SACGPU_NEXT.md` 的优先级规划，实施 P0-1 任务。

**目标**：probe 执行若 hit budget，则标记 `UNKNOWN`，结果一律"不删"（保守处理）。

**交付内容**：
- `ProbeStatus { kOK, kDWO, kUNKNOWN }` 枚举（三态）
- `ProbeStatistics` 统计结构（unknown_rate、budget_hit_count、avg/max iterations）
- Stage2 内核根据 `max_iterations_per_probe` 判断是否预算超限
- `CollectResults` 仅对 `kDWO` 删值，`kUNKNOWN` 保守不删

**修改文件**：
- `include/solver/gpu/batch_probe_manager.h`: 添加 `ProbeStatus`、`ProbeStatistics`、`Batch2PersistentControl::task_status` 等
- `src/solver/gpu/batch_probe_manager.cu`: 内存分配/释放、`CollectResults` 三态统计收集
- `src/solver/gpu/GModel.cu`: Stage2 内核设置 `task_status`（预算超限→kUNKNOWN）

**开发规范**：
- 在 `AGENTS.md` 和 `CLAUDE.md` 添加开发规范：回归测试、性能记录、消融开关、可回退原则

**验收**：`batch_test_v2.py --tier=0` 通过（8/12 匹配，4 个超时是已有性能问题）

---

### P0-1a：停滞检测（Stagnation Detection）

**背景**：比 `max_iterations` 硬截断更智能的长尾检测，用多指标判定"停滞"。

**检测指标**：
- `Δdeletions`：连续 k 轮 deletions==0（停滞计数）
- `frontier_popcount`：活跃约束数
- `deletions / work_cnt`：单位工作产出率

**交付内容**：
- `WorldWorkspace` 添加字段：`stagnation_count`, `last_deletions`, `last_frontier_popcount`, `work_cnt`
- `Batch2PersistentControl` 添加配置：`stagnation_threshold`, `min_productivity`, `enable_stagnation_check`
- `Batch2PersistentManager` 添加 API：`SetStagnationThreshold()`, `SetMinProductivity()`, `EnableStagnationCheck()`
- `RunGACToFixpoint_BlockSync` 每轮后检测停滞条件，触发时提前退出并标记 UNKNOWN

**修改文件**：
- `include/solver/gpu/batch_probe_manager.h`: WorldWorkspace 停滞字段、Control 停滞配置、Manager API
- `src/solver/gpu/batch_probe_manager.cu`: 传递停滞参数到 control
- `src/solver/gpu/GModel.cu`: RunGACToFixpoint_BlockSync 停滞检测逻辑
- `docs/planning/TODO_SACGPU_NEXT.md`: 更新 P0-1 子任务

**验收**：`batch_test_v2.py --tier=0` 通过（8/12 匹配，与之前一致）

---

### P0-1b：时间片调度基础设施（Timeslice Scheduling Infrastructure）

**背景**：为长尾 probe 提供"工作量子"限制，防止单个 probe 阻塞整个批次。

**交付内容**：
- `GACTimesliceState` 结构体（预留完整暂停/恢复用）
- `WorldWorkspace` 添加字段：`total_constraints_checked`, `quantum_exceeded`
- `Batch2PersistentControl` 添加配置：`quantum_cid`, `enable_quantum_check`
- `Batch2PersistentManager` 添加 API：`SetQuantumCid()`, `EnableQuantumCheck()`
- `RunGACToFixpoint_BlockSync` 添加工作量子检查逻辑

**修改文件**：
- `include/solver/gpu/batch_probe_manager.h`: GACTimesliceState、WorldWorkspace 时间片字段、Control/Manager API
- `src/solver/gpu/batch_probe_manager.cu`: 传递时间片参数到 control
- `src/solver/gpu/GModel.cu`: RunGACToFixpoint_BlockSync 工作量子检查

**当前状态**：基础设施完成，默认关闭（`enable_quantum_check=0`）。完整的 yield/resume 逻辑待 Batch-3A 载体稳定后实现。

**验收**：`batch_test_v2.py --tier=0` 通过（8/12 匹配，与之前一致）

---

### P0-1c：延后复查队列（Deferred Recheck Queue）

**背景**：P0-1a/P0-1b 让 Stage2 能识别并提前退出长尾 probe（标记 `kUNKNOWN`），但 SAC3 顶层此前只收集 `kDWO`，
导致 UNKNOWN probes 被直接丢弃，无法在邻域发生删值变化后重检。

**交付内容**：
- `GModelSolver::DeferredRecheckConfig`：deferred queue 的运行时配置（开关/上限/重检次数/超期）。
- `DeferredProbeQueue`（在 `EnforceSAC3()` 内部集成）：UNKNOWN probes 入队，邻域 epoch 变化后出队复查。
- `Batch2PersistentManager::ExecutePersistentBlocks()` 增强接口：额外返回 UNKNOWN probes 列表（var/value）。
- SAC3 主循环集成：
  - 维护 `nb_epoch[var]`（邻域 epoch），当删值/GAC 级联删值发生时对 `var` 及其邻居递增；
  - queue 模式下优先调度 deferred ready probes，并用 regular queue 补满 batch；
  - 统计 `deferred_in/out/hit/stale/overflow`（verbose 输出）。

**修改文件**：
- `include/GModelSolver.h`: 新增 `DeferredRecheckConfig`
- `src/solver/gpu/GModelSolver.cu`: `DeferredProbeQueue` + `EnforceSAC3()` 集成
- `include/solver/gpu/batch_probe_manager.h`: Stage2 manager 增强接口（返回 UNKNOWN probes）
- `src/solver/gpu/batch_probe_manager.cu`: 收集 UNKNOWN probes 并返回给 host

**验收**：`batch_test_v2.py --tier=0` 通过（8/12 匹配，与之前一致；4 个超时为既有性能问题）

---

### 修复：`sac_benchmark` Full SAC 下的 kernel launch `invalid argument`

**问题**：`sac_benchmark --mode=full_sac` 会一次性提交大量 probes；Stage2（Persistent Blocks）
在 auto-tune 重新分配 workspaces 后，可能出现 `num_tasks > max_tasks_`，导致任务数组 `cudaMemcpy`
越界写，最终在 kernel launch 时报 `invalid argument`。

**修复**：在 `Batch2PersistentManager::ExecutePersistentBlocks()` 中加入防御性扩容：当
`num_tasks > max_tasks_` 时自动 `ReserveTaskCapacity()`，确保任务/结果数组容量充足。

**修改文件**：
- `src/solver/gpu/batch_probe_manager.cu`

---

### SACGPU 下一阶段 TODO 备忘

- 新增 `docs/planning/TODO_SACGPU_NEXT.md`：整理 P0-P3 的实施清单（UNKNOWN 语义、NSAC mask、
  Batch-3A 接入、bitGEMM 路线与 `bmma_sync(b1, AND+POPC)` 插入点），作为后续迭代备忘录。
- 补充：在 P1-1 增加依赖关系说明（P0-1/P0-3），在 P3-2 明确 BMMA 对 “world 列矩阵 packing 载体稳定”
  的前提要求。
- 补充：细化 P0-1c“延后复查队列（Deferred Recheck）”方案（版本/邻域 epoch 两档实现），并更新现状快照避免与代码状态不一致。

---

## 2026-01-15

### SAC‑GPU 设计文档（Draft v2）整理

- 重写 `docs/planning/SACGPU_DESIGN copy.md`：将原“讨论纪要式”文本整理为可发布的设计草案（元信息/术语对齐/与代码现状锚定/参考文献编号统一），并明确 Phase 5（Batch‑3A/3D 扁平化队列）作为可选加速器的启用条件与主线优先级（budget + NSAC）。
- 更新 `docs/planning/SACGPU_DESIGN.md`：合并 copy 版的关键增补（文档 frontmatter/扁平化层次定义/代码现状对齐/Phase 5 启用条件与回退策略），作为统一版主入口。

---

## 2026-01-12

### Phase 5（Batch‑3D）问题与讨论备忘

- 新增 `docs/archive/batch_ac_versions/BATCH_AC_GPU_PHASE5_BATCH3D_DISCUSSION_MEMO.md`：总结 Phase 5
  推进中的瓶颈（并行度/访存/长尾/调度开销/平台约束）与下一步数据验证清单，用于
  与外部大模型做方案评审。
- 扩展补充：在备忘中加入 Phase 1‑4 已完成工作概览与 Full/fast MSAC 现象说明，便于外部对齐上下文。

---

## 2026-01-11

### Warp-per-Word 约束检查优化推广到 Batch-1

**背景**：Workspace 版（Batch-2/Stage2）的 Warp-per-Word 优化已在 2026-01-09 完成并验证，现在将其推广到 Batch-1/Stage1 的 `ExecuteConstraintCheck_BpC`。

**核心差异**：
- Workspace 版操作私有域（`ws->bitDom`），写回无需原子操作
- Batch-1 版操作共享域（`model.bitDom`），多 block 并发需原子写回

**优化**：在 Batch-1 中应用 Warp-per-Word，消除 shared memory 的 `atomicAnd`，但保留 global memory 原子操作（多 block 并发安全）。

**修改文件**：
- `src/solver/gpu/GModel.cu`:
  - 原 `ExecuteConstraintCheck_BpC()` 重命名为 `ExecuteConstraintCheck_BpC_Legacy()` (line 690-818)
  - 新增 `ExecuteConstraintCheck_BpC_WarpPerWord()` (line 820-983)
  - 新增调度函数 `ExecuteConstraintCheck_BpC()`：`bit_dom_int_size == 1` 走 Legacy，否则走 Warp-per-Word (line 986-1002)
  - 修复：当 `bit_dom_int_size > 1` 时，`EnforceGAC/EnforceGAC_Persistent` 的 `threadsPerBlock` 向上对齐到 32 的倍数，避免 Warp-per-Word 在 partial warp 下的未定义行为
  - 更新 `BitmapGACKernel` shared memory 分配：+64 bytes (line 2785-2786)
  - 更新 `PersistentGACKernel` shared memory 分配：+64 bytes (line 2938-2939)

**关键实现**：
- Warp-per-Word：每 warp 处理一个 domain word，使用 `__ballot_sync` 收集决策
- Global 写回：使用 `atomicAnd` 返回旧值精确统计本 block 删除的位
- 域大小更新：使用 `atomicSub` 保证多 block 并发安全

**验证结果**：
- 正确性：queens-4 (P=5/N=1), langford-3-9 (P=468/N=441), graphw-05 (UNSAT) 一致
- CPU/GPU 节点数匹配：关键测试实例通过
- 回归测试：`ctest --test-dir build -L gpu` 全 PASS
- 边界测试：rand-2-40-80 (max_dom_size=80) 通过 benchmark_probe_throughput

---

## 2026-01-09

### Warp-per-Word 约束检查优化（Workspace 版本）

**问题**：`ExecuteConstraintCheck_BpC_Workspace()` 使用条纹分配（stripe），每个线程处理间隔 `blockDim.x` 的值，删值时需要 `atomicAnd` 在 shared memory 中更新（避免多线程竞争同一 word）。

**优化**：实现 Warp-per-Word 模式，每个 warp (32 线程) 处理一个 domain word（32 个连续值），使用 `__ballot_sync` 收集删值决策，消除 `atomicAnd`。

**修改文件**：
- `src/solver/gpu/GModel.cu`:
  - 新增 `ExecuteConstraintCheck_BpC_Workspace_WarpPerWord()` (line 1648-1796)
  - 原实现重命名为 `ExecuteConstraintCheck_BpC_Workspace_Legacy()` (line 1523-1645)
  - 调度函数：`bit_dom_int_size == 1` 走 Legacy，否则走 Warp-per-Word (line 1799-1816)
  - 更新 shared memory 分配：+64 bytes (warp_del_x/y)

**关键技术**：
- `__ballot_sync(0xFFFFFFFF, keep || !active)` 收集 32 个 lane 的保留决策
- `keep_mask` 直接用于 `new_dom = old_dom & keep_mask`，无需原子操作
- Lane 0 统计删值并写回 `new_dom_x[word]`

**验证结果**：
- 正确性：queens-4/12 (Legacy), langford-3-9 (Legacy), graphw-05 (Warp-per-Word) 一致
- 性能：graphw-05 (bit_dom_int_size=2) Stage 2 吞吐 84469 → 92655 probes/s (+9.7%)

## 2026-01-08

### Workspace 版约束检查域大小增量更新优化

**问题**：`ExecuteConstraintCheck_BpC_Workspace()` 在每次约束检查后都对 `new_x/new_y` 每个 word 执行 `__popc()` 来重算域大小，即使域未发生变化也会扫描整域。

**优化**：改用增量更新 `size -= __popc(removed)`，仅在有删值时执行 `__popc(removed_x)`，消除 `__popc(new_x)` 热路径。

**修改文件**：
- `src/solver/gpu/GModel.cu` (line 1588-1629):
  - 初始化 `size_x = ws->d_cur_dom_size[x]`
  - `if (removed_x) { size_x -= __popc(removed_x); }` 代替 `new_size_x += __popc(new_x)`
  - 只在 `r.x_changed` 时写回 `ws->d_cur_dom_size`

**验证结果**：
- 正确性：queens-4/12, langford-3-9/2-4 节点数一致
- 性能：graphw-05 (fail_rate=100%) Stage 2 吞吐提升明显

### Batch-1 版约束检查域大小增量更新优化

**问题**：`ExecuteConstraintCheck_BpC()` 在每次约束检查后对所有 word 执行两次遍历：一次 `atomicAnd` 删值，一次 `__popc()` 全量重算域大小。

**优化**：改用 `atomicSub` 增量更新，利用 `atomicAnd` 返回值获取删值数，避免全量重扫。

**修改文件**：
- `src/solver/gpu/GModel.cu` (line 770-795):
  - 使用 `atomicAnd()` 返回值获取 `old_x`（避免读-写竞态）
  - `removed_x = old_x & ~new_x` 精确统计本 block 删除的位
  - `atomicSub(d_cur_dom_size, del_x)` 原子减法更新域大小
  - `atomicAdd(..., 0)` 原子读取最新域大小用于 DWO 检测

**关键修复**：
- 竞态条件修复：两个 block 同时读取 `old_x` 会导致双重计数
- 解决方案：使用 `atomicAnd` 返回值而非直接读取全局内存

**验证结果**：
- 正确性：queens-4/12, langford-2-4/3-9 节点数一致
- 性能：graphw-05 Stage 2 vs Stage 1 加速 2.96x

### CSR 邻接表构造时机优化

将 `NeighborCSR` 从各 SAC 函数局部构建移至 `GModelSolver` 构造函数，避免重复构建。

**修改文件**：
- `include/GModelSolver.h`: +`NeighborCSR` 结构体，+`neighbor_csr_` 成员
- `src/solver/gpu/GModelSolver.cu`: 构造函数调用 `NeighborCSR::Build()`，EnforceSAC1/SAC3/LightweightMSAC 使用成员变量

## 2026-01-05

### GPU SAC3 预处理性能优化（降低无删值/多删值开销）

- `src/solver/gpu/GModelSolver.cu`：
  - SAC3 批量大小上调到 `max_batch_size=1024`，减少快照/Kernel 启动次数（无删值场景尤其明显）
  - 邻域表从 `std::set` 改为 `std::vector` + sort/unique，降低构建与遍历开销
  - GAC 级联删值追踪改为基于 Trail 增量区间收集变量，避免 `O(num_vars)` 全扫描
  - 对同一变量的多次删值去重后再 `EnqueueNeighborhood()`，避免重复扫描邻域域值

## 2026-01-04

### 生产用法：DecideCached() 缓存决策

为 SAC/MSAC 集成场景实现了带缓存的自动选择，避免重复采样开销。

**核心实现**：

1. **`DecideCached(tasks, num_blocks)` 方法**:
   - 首次调用：执行 `DecideWithTimedComparison` 并缓存结果
   - 后续调用：直接返回缓存结果，零开销
   - 辅助方法：`ClearCache()`, `HasCache()`, `GetCachedResult()`

2. **典型用法**:
   ```cpp
   AutoStageSelector selector(gmodel);
   // 第一轮 SAC：执行采样
   auto result = selector.DecideCached(tasks1);
   // 第二轮/第三轮 SAC：直接返回缓存（无开销）
   auto result2 = selector.DecideCached(tasks2);
   // 需要重新评估时调用 ClearCache()
   selector.ClearCache();
   ```

3. **验证命令**:
   ```bash
   ./benchmark_probe_throughput <instance.xml> --auto-cached
   ```

**修改文件**：
- `include/solver/gpu/batch_probe_manager.h`: +`DecideCached()`, `ClearCache()`, `HasCache()`, `GetCachedResult()`, +cache fields
- `src/solver/gpu/batch_probe_manager.cu`: +`DecideCached()` 实现
- `apps/benchmark_probe_throughput.cpp`: +`--auto-cached` 选项

### SAC1 集成到 GModelSolver

将批量探测（Batch Probe）基础设施集成到 GModelSolver，支持 SAC1 预处理。

**核心实现**：

1. **`EnforceSAC1()` 方法** (GModelSolver):
   - 执行 SAC1 预处理（迭代直到不动点）
   - 自动使用 `DecideCached()` 选择最优 Stage
   - 删除后自动执行 GAC 传播

2. **新增配置选项**:
   ```cpp
   solver.SetSAC1Preprocessing(true);  // 启用 SAC1 预处理
   solver.SetSACStageMode(StageSelection::kAuto);  // Auto/Stage1/Stage2
   ```

3. **命令行支持** (`compare_cpu_gpu`):
   ```bash
   ./compare_cpu_gpu --input=instance.xml --sac [--sac_stage=auto|stage1|stage2]
   ```

4. **统计扩展** (GpuSearchStatistics):
   - `sac_deletions`: SAC 删除的值总数
   - `sac_probes`: SAC 探测次数
   - `sac_rounds`: SAC 轮次
   - `sac_time`: SAC 时间 (秒)
   - `sac_stage`: 使用的 Stage

**验证结果**:
| 实例 | 无 SAC (P/N) | 有 SAC (P/N) | SAC 删除 | SAC 轮次 |
|------|-------------|-------------|----------|---------|
| queens-4 | 2/1 | 1/0 | 8 | 2 |

**修改文件**：
- `include/GModelSolver.h`: +SAC 配置, +SAC 统计字段
- `src/solver/gpu/GModelSolver.cu`: +`EnforceSAC1()` 实现
- `apps/compare_cpu_gpu.cpp`: +`--sac`, `--sac_stage` 选项

### Stage 2 CTest 集成

将 Stage 2 测试添加到 CTest 框架，用于 CI/回归保护。

**测试内容**：
- 验证 Stage 1 和 Stage 2 产生相同的失败 probe 集合
- 确保 Persistent Blocks 实现的正确性

**运行命令**：
```bash
cd build && ctest -R test_stage2_persistent -V
# 或运行所有 GPU 测试
ctest -L gpu -V
```

**修改文件**：
- `CMakeLists.txt`: 添加 `test_stage2_persistent` 到 CTest

---

## 2026-01-03

### Auto Stage Selection（自动阶段选择）

实现了 Stage 1 vs Stage 2 的自动选择逻辑，基于采样统计自动决定最优执行策略。

**核心实现**：

1. **AutoStageSelector 类** (`include/solver/gpu/batch_probe_manager.h`)：
   - `DecideByTaskCount(num_tasks)`: 基于任务数快速决策
   - `DecideWithSampling(tasks)`: 执行采样并决策（统计判据）
   - `DecideWithTimedComparison(tasks)`: 实测对比并决策（**推荐**）

2. **决策方法对比**:

   | 方法 | 原理 | 优点 | 缺点 |
   |------|------|------|------|
   | `DecideWithSampling` | CV/P95/P50 分布分析 | 开销小 | 可能误判均匀但 Stage 2 更快的场景 |
   | `DecideWithTimedComparison` | 实测 Stage 1/2 耗时 | 更准确 | 采样开销略大（~10-20ms） |

3. **`DecideWithTimedComparison` 决策逻辑**:
   - 采样大小：max(16, min(64, num_tasks/10))
   - 分别运行 Stage 1 和 Stage 2，比较实际耗时
   - 10% 容差内使用 fail_rate 作为 tiebreaker
   - **特殊覆盖**：高 fail_rate (>50%) + 大任务量 (>500) → Stage 2
     （小采样无法体现大规模高失败率场景的动态调度优势）

4. **关键改进** (2026-01-03 更新):
   - 移除了 `num_tasks > 200` 和 `avg_deletions > 80` 的硬触发
   - 改用基于分布的判据：CV (变异系数) 和 P95/P50 比值
   - 新增 `--auto-timed` 模式：使用实测对比而非纯统计
   - 核心原则：只有在任务负载不均衡时才选 Stage 2

5. **扩展 benchmark_probe_throughput**：
   - 支持 Stage 1 vs Stage 2 对比
   - `--auto` 模式：使用 `DecideWithSampling`（统计判据）
   - `--auto-timed` 模式：使用 `DecideWithTimedComparison`（**推荐**）
   - 输出 per-task 统计和分布分析（CV, P95/P50）

**验证结果**（更新后）：
| 实例 | num_tasks | CV | 自动选择 | 实际最优 | 一致性 |
|------|-----------|-----|----------|----------|--------|
| Queens-4 (16) | 16 | 0.0 | Stage 1 | Stage 1 | ✅ |
| Queens-12 (144) | 144 | 0.03 | Stage 1 | Stage 1 | ✅ |
| Langford-3-9 (405) | 405 | 0.14 | Stage 1 | Stage 1 (0.97x) | ✅ |
| Graphw-05 (1863) | 1863 | 0.57 | Stage 2 | Stage 2 (高fail) | ✅ |
| Rand-2-40-8 (320) | 320 | 0.18 | Stage 1 | Stage 1 (0.76x) | ✅ |

**修复的误判案例**：
- `langford-3-9`: 旧逻辑选 Stage 2（num_tasks>200触发），实际 Stage 1 更快 0.97x
- `rand-2-40-8`: 旧逻辑选 Stage 2（num_tasks>200触发），实际 Stage 1 更快 0.76x

**Stage 2 Per-task 统计**：

在 kernel 中记录每个任务的 `iterations` 和 `deletions`，用于分析和调优：
- `Batch2PersistentControl::task_iterations[num_tasks]`
- `Batch2PersistentControl::task_deletions[num_tasks]`

**Stage 2 已知限制**：
- `enable_precheck`: Stage 2 kernel 当前未读取此字段，precheck 仅在 Stage 1 有效

**Stage 2 优化更新** (2026-01-03):
- ✅ NEIGHBOR_ACTIVATION 初始化优化为并行版本（所有线程并行使用 atomicOr）
- 之前：仅 threadIdx.x==0 串行初始化邻接约束
- 之后：所有线程并行初始化，与 Stage 1 `InitializeFrontierForVariable_BlockSync` 保持一致

**修改文件**：
- `include/solver/gpu/batch_probe_manager.h`: +`AutoStageSelector`, +`StageSelectionResult`
- `src/solver/gpu/batch_probe_manager.cu`: +`AutoStageSelector` 实现, +per-task stats
- `src/solver/gpu/GModel.cu`: kernel 中写入 per-task 统计
- `apps/benchmark_probe_throughput.cpp`: 支持 Stage 2 和 `--auto` 模式

---

### Stage 2 性能优化

基于用户反馈实现了两项关键优化，显著提升了 Stage 2 Persistent Blocks 的性能。

**1. 自适应 num_blocks**（`batch_probe_manager.cu`）

新增 `ComputeOptimalNumBlocks(int num_tasks, int device_id)` 方法：
- 任务数 <= 2*num_sms → 使用 num_sms 个 blocks（减少开销）
- 否则使用 4*num_sms 个 blocks（增加并行度）
- 效果：queens-4 (16 tasks) 选择 8 blocks，从 0.61x 提升到 **1.00x**

**2. 可配置 chunk_size**

支持批量任务拉取（`atomicAdd(task_cursor, chunk_size)`）：
- 默认 chunk=1（经测试为最优值）
- 可通过 `SetChunkSize()` 调整

**性能对比**（优化前 vs 优化后）：
| 实例 | 优化前 | 优化后 | 改善 |
|------|--------|--------|------|
| Queens-4 | 0.61x | **1.00x** | +64% |
| Queens-12 | 0.64x | 0.68x | +6% |
| Langford-3-9 | 0.62x | 0.85x | +37% |
| Graphw-05 | 5.21x | **2.76x** | (仍快 2.76x) |
| Rand-2-40-8 | 1.11x | **2.02x** | +82% |
| Rand-2-40-80 | 1.15x | **1.76x** | +53% |

**修改文件**：
- `include/solver/gpu/batch_probe_manager.h`：添加 `ComputeOptimalNumBlocks`, `chunk_size_` 等
- `src/solver/gpu/batch_probe_manager.cu`：实现自适应逻辑
- `src/solver/gpu/GModel.cu`：支持 chunk_size 批量拉取
- `tests/cpp/test_stage2_persistent.cpp`：添加 `--chunk=` 参数

## 2026-01-02

### Phase 3 Stage 2：Persistent Blocks 实现

实现了 Batch-2 的 Stage 2 版本：单次 kernel launch，多个持久 blocks 通过 `atomicAdd` 拉取任务。

**核心实现**：

1. **控制结构** (`include/solver/gpu/batch_probe_manager.h`)：
   - 新增 `Batch2PersistentControl` 结构体：全局任务游标、snapshot、workspaces 数组
   - 新增 `Batch2PersistentManager` 类：Host 端 Stage 2 调度器

2. **Persistent Blocks Kernel** (`src/solver/gpu/GModel.cu:~2046`)：
   ```cpp
   __global__ void Batch2ProbeKernel_PersistentBlocks(
       const GModelData model,
       Batch2PersistentControl* control) {
     // 每个 block 通过 atomicAdd(task_cursor) 拉取任务
     while (true) {
       task_id = atomicAdd(control->task_cursor, 1);
       if (task_id >= num_tasks) break;
       // 重置 workspace → 恢复 snapshot → singleton 赋值 → GAC 传播
       RunGACToFixpoint_BlockSync(...);
       // 写结果
     }
   }
   ```

3. **Host API** (`src/solver/gpu/batch_probe_manager.cu`)：
   - `Batch2PersistentManager::ExecutePersistentBlocks()`：主入口
   - `AllocateMemory()`：分配 per-block workspaces、任务数组、快照
   - `LaunchPersistentBlocksKernel()`：设置控制结构并启动 kernel
   - `CollectResults()`：收集失败 probe

**测试验证** (`tests/cpp/test_stage2_persistent.cpp`)：
- ✅ Queens-4: Stage 1 与 Stage 2 均报告 8 个失败 probe（PASS）
- ✅ Queens-12: Stage 1 与 Stage 2 均报告 0 个失败 probe（PASS）
- ✅ 一致性检查：两个阶段产生完全相同的失败 probe 集合

**性能观察**：
- Queens-4: Stage 1 1.978ms → Stage 2 3.001ms
- Queens-12: Stage 1 5.514ms → Stage 2 8.660ms (0.64x)
- **结论**：Stage 2 当前比 Stage 1 慢，原因可能是：
  - `atomicAdd` 竞争开销
  - 任务分配不均衡
  - 需要进一步优化（如 warp-level 任务聚合、减少同步点）

**Bug 修复**：
1. **shared_mem 声明缺失**：添加 `extern __shared__ u32 shared_mem[]`
2. **ExecuteConstraintCheck 参数顺序**：修正为 `(cid, model, ws, shared_mem)`
3. **GAC 循环死锁**：本地 frontier 指针交换导致线程间不一致，改用 `RunGACToFixpoint_BlockSync()` 解决

**修改文件**：
- `include/solver/gpu/batch_probe_manager.h`：+`Batch2PersistentControl`, +`Batch2PersistentManager`
- `src/solver/gpu/GModel.cu`：+`Batch2ProbeKernel_PersistentBlocks`, +`LaunchBatch2PersistentBlocksKernelWrapper`
- `src/solver/gpu/batch_probe_manager.cu`：+`Batch2PersistentManager` 实现（~300 行）
- `CMakeLists.txt`：+`test_stage2_persistent` 构建目标
- `tests/cpp/test_stage2_persistent.cpp`：Stage 1 vs Stage 2 对比测试

## 2025-12-31

### 吞吐基准工具（benchmark_probe_throughput）

- 新增 `apps/benchmark_probe_throughput.cpp`：Batch-1 vs Batch-2 吞吐量对比工具
- 输出指标：probes/s、执行时间、加速比、平均 iterations/deletions per probe
- 支持参数：`--runs=N`（多次运行取平均）、`--batch2_size=M`（micro-batch 大小）、`--strategy=S`（激活策略）
- 初测结果（Jetson Orin Nano, NEIGHBOR 策略）：
  - Queens-4 (16 probes)：Batch-1 2,440 p/s → Batch-2 11,020 p/s（**4.52x 加速**）
  - Queens-12 (144 probes)：Batch-1 2,261 p/s → Batch-2 24,674 p/s（**10.91x 加速**）
- 验证：Batch-1 与 Batch-2 失败 probe 数一致（Consistency check: PASS）

### Phase 4.3：只读数据优化（cudaMemAdviseSetReadMostly）

- 新增 `GModelAdapter::OptimizeReadOnlyMemoryAdvice()` 方法（`include/model/gmodel_adapter.h:60-62`, `src/model/gmodel_adapter.cu:469-540`）
- 对以下只读数据设置 `cudaMemAdviseSetReadMostly` 提示：
  - `bitSupData`：位支持表（最大的只读数据）
  - `d_subscription`：变量订阅表
  - `d_subscription_offset`：CSR 偏移索引
  - `constraint_scopes`：约束作用域
- 在 `GModelAdapter::Build()` 末尾自动调用（`src/model/gmodel_adapter.cu:330-331`）
- 实现细节：
  - 添加 nullptr/size==0 保护（防御性检查）
  - 添加成功/失败/跳过统计，输出精确的 "applied successfully (N/4)" 或 "applied with warnings"
- 预期收益：Jetson UMA 上的实际效果待用 probes/s 与 Nsight 指标量化（cudaMemAdvise 是 hint，不保证加速）
- 验证：`compare_batch2_tier0.py --tier=0` 12/12 通过，日志输出确认调用成功（4/4）

## 2025-12-30

### Phase 3 Stage 1：Batch-2 Micro-Batch（非 cooperative）

- 新增 `Batch2ProbeManager`（`include/solver/gpu/batch_probe_manager.h`, `src/solver/gpu/batch_probe_manager.cu`）：Host 端 micro-batch 调度与 workspace 内存池分配（每次只分配 `max_batch_size` 份 `WorldWorkspace`，避免按总任务数分配导致内存爆炸），通过 `LaunchBatch2MicroBatchKernelWrapper` 触发 `Batch2ProbeKernel_MicroBatch`。
- 新增测试 `tests/cpp/test_batch2_probe.cpp`：对 `queens-4_ext.xml` 验证 Batch-1 vs Batch-2（Micro-Batch）在 `FULL_ACTIVATION/NEIGHBOR_ACTIVATION` 下的失败探测集合一致，并验证 Batch-2 不污染 `GModel` 原始域状态（`bitDom/d_cur_dom_size`）。
- 更新 `CMakeLists.txt`：添加 `test_batch2_probe` 构建目标并注册到 `ctest`。
- 新增脚本 `tests/python/compare_batch2_tier0.py`：批量跑 TIER0/TIER1，用 `test_batch2_probe` 验证 Batch-2（Micro-Batch）与 Batch-1 结果一致性。
- 强化 Batch-2 健壮性与可观测性：`Batch2ProbeKernel_MicroBatch` 在每个 task 开始重置 `WorldWorkspace` 标量状态（避免 precheck/早退路径残留）；`Batch2ProbeManager` 增加可选的 iterations/deletions 统计收集（`EnableStats`），并在 `tests/cpp/test_batch2_probe.cpp` 中覆盖 precheck 开/关两种路径。
- 修正文档 `docs/planning/BATCH_AC_GPU_COMPREHENSIVE_DESIGN_V2.md`：明确当前 Precheck 为“安全早失败”，在 AC snapshot 前提下短路率理论/实测接近 0%，不再以 40-60% 短路率作为性能验收指标。
- 更新清单 `docs/planning/BATCH_AC_GPU_TODO.md`：对齐代码现状（Phase 3 TIER0/TIER1 已通过、吞吐基准待补；Phase 2 P1/P2/P3 待实现），并修正测试命令与函数命名。

## 2025-12-29

### Phase 2 P0 优化实施与 Bug 修复（Batch AC-GPU）

实施了 BATCH_AC_GPU_COMPREHENSIVE_DESIGN_V2.md 中的 Phase 2 两个 P0 优化项，发现并修复了关键并发 bug。

**[P0-1] Frontier 初始化策略化 - ✅ 修复后正确**
- ✅ 添加 FrontierInitStrategy 枚举 (GModel.cuh:29-33)：FULL_ACTIVATION / NEIGHBOR_ACTIVATION
- ✅ 扩展 BatchProbeControl 结构 (batch_probe_manager.h:56-58, 83)：snapshot_is_ac, activation_strategy
- ✅ 修改 InitializeFrontierForVariable (GModel.cu:1014-1070)：实现邻域激活分支
- ✅ 添加 SetActivationStrategy() 方法 (batch_probe_manager.h:129, batch_probe_manager.cu:194-207)
- ✅ **理论验证**：NEIGHBOR_ACTIVATION 在 snapshot AC 前提下是**正确的**优化，通过 PropagateVarToNextBitmap 动态扩散到所有需要检查的约束

**[P0-2] Cheap Precheck - ✅ 已修复并启用**
- ✅ 添加统计字段 (batch_probe_manager.h:60-63)：precheck_count, short_circuit_count, need_gac_flag
- ✅ 重写 CheckValueSupportBitSup (GModel.cu:1204-1260)：使用 bitSupData 做“早失败”检测（对齐 ExecuteConstraintCheck_BpC 的索引方式）
  - 若 `(X=a)` 在任一邻接约束上无支持 → **必然 DWO** → 短路跳过完整 GAC
  - 若所有邻接约束当前都有支持 → **仍需完整 GAC**（不做“早成功”）
- ✅ 启用 Precheck (GModel.cu:1404-1422)：根据 need_gac_flag 决定是否跳过 RunGACToFixpoint

**Bug 修复**：
1. **Cooperative Kernel 死锁** (GModel.cu:1207-1211)
   - 根因：`__shared__` 内存 per-block 可见性导致 grid.sync() 死锁
   - 修复：need_gac_flag 移至 BatchProbeControl (全局内存)

2. **activation_strategy 初始化** (batch_probe_manager.h:83)
   - 根因：初始化为 0，SaveSnapshot() 会覆盖用户设置的 FULL_ACTIVATION
   - 修复：初始化为 -1，SaveSnapshot() 只在 -1 时设置默认值

3. **🔥 Frontier 交换竞争条件（非确定性 Bug 根源）** (GModel.cu:1186-1189)
   - **根因**：RunGACToFixpoint 中所有线程都在交换 frontier 指针，导致不同线程看到不同指针值，传播提前收敛
   - **现象**：NEIGHBOR_ACTIVATION 产生非确定性结果（queens-4: 0-2 个 DWO，每次都不同）
   - **修复**：只用 tid==0 的线程交换指针（与 PersistentGACKernel 一致）
   - **验证**：queens-4 运行 5 次结果完全一致（8 vs 8），TIER0 全部通过 (12/12)

**关键修改文件**：
- include/GModel.cuh, include/solver/gpu/batch_probe_manager.h
- src/solver/gpu/GModel.cu (~120 行，含调试代码和 bug 修复)
- src/solver/gpu/batch_probe_manager.cu
- tests/cpp/test_batch_probe_state.cpp - FULL vs NEIGHBOR 对比测试（添加详细调试输出）
- tests/python/compare_activation_strategies.py - TIER0 批量测试
- tests/scripts/test_activation_strategies.sh - Shell 测试脚本

**测试结果**（修复后）：
- ✅ **状态一致性**: bitDom ✓, d_cur_dom_size ✓
- ✅ **FULL vs NEIGHBOR 一致性**: queens-4 (8 vs 8 ✓)，langford-2-4 (8 vs 8 ✓)
- ✅ **确定性**: queens-4 运行 5 次结果完全相同
- ✅ **TIER0**: 12/12 全部通过（包括 UNSAT 实例 graphw-05）
- ✅ **理论正确性**: deletions、最终域状态、DWO 检测完全一致

**调试发现**：
- NEIGHBOR 的 iterations 稍多（2-4 vs 1-2），这是预期的，因为需要逐步扩散 frontier
- 快照域状态一致（v0=4 v1=4 v2=4 v3=4），证明 snapshot 恢复正确
- PropagateVarToNextBitmap 正常工作，frontier 从邻居扩散到所有受影响约束

**调试输出**：
- ✅ Bug 修复后，调试输出已禁用 (GModel.cu:1307, 1342, 1389)
- 通过 `if (false && ...)` 保留代码便于未来调试
- 验证测试：queens-4 运行正常，结果一致 (8 vs 8)

**结论**：
- ✅ NEIGHBOR_ACTIVATION 是**理论正确**的优化（在 snapshot AC 前提下）
- ✅ Bug 已修复，两种策略产生完全一致的结果
- ❌ Cheap Precheck 需要重新设计（当前逻辑不正确）

---

### 文档修复
- 修复文档 `docs/planning/BATCH_AC_GPU_COMPREHENSIVE_DESIGN_V2.md`：对齐文档与实际代码/硬件实现，修复 6 处关键问题
  - §3.1：澄清当前 GModel.cu:1039 使用 FULL_ACTIVATION，Phase2 将添加 NEIGHBOR_ACTIVATION 优化
  - §5.1：修正 CheapPrecheck 使用 bitSupData 而非不存在的 bitSupTex，对齐 ExecuteConstraintCheck_BpC 索引逻辑
  - §4.2：修正内存估算示例，补充正确的 bitmap_size_words 计算公式和 Queens-12/100 真实数据
  - §5.4：添加 Jetson UMA 检测（concurrentManagedAccess 判断），避免在统一内存架构下执行不必要的 cudaMemPrefetchAsync
  - §9.2：修正 cudaMemGetInfo API 调用签名，补充 shared_mem_size 正确计算方法
  - §6.3：优化 PopTask 实现，引入两级 bitmap 索引避免 O(C) 线性扫描，降低大规模约束问题开销

- 新增备忘 `docs/planning/BATCH_AC_GPU_FLATTEN_TASK_QUEUE_MEMO.md`：总结“Probe × GAC 扁平化”为一维任务流的实现要点，推荐以 `<cid, world_mask>` 聚合队列 + work-stealing 落地 Batch‑3A（Jetson 友好，非 cooperative）。
- 更新备忘 `docs/planning/BATCH_AC_GPU_FLATTEN_TASK_QUEUE_MEMO.md`：补充聚合队列的额外开销来源、可观测计数器与自适应开关/回退阈值。

## 2025-12-25
- 新增文档 `docs/planning/BATCH_AC_GPU_DESIGN.md`：基于 `GModel` 梳理 AC-GPU 可优化点，并给出 SAC-GPU 迁移到 Batch AC-GPU 的数据结构、内核方案与分阶段落地路径。
- 新增备忘 `docs/planning/BATCH_AC_GPU_COMPARISON_MEMO.md`：对比设计与实施文档，汇总一致点、差异与风险清单。
- 新增文档 `docs/planning/BATCH_AC_GPU_BATCH2_BATCH3_DESIGN.md`：总结 Batch-2/Batch-3 的多世界并行与持久线程块方案，并给出 Jetson Orin（UMA + cooperative 限制）下的落地路线。

## 2025-12-17
- 新增文档 `benchmarks/README.md`：对 `benchmarks/` 基准库做走读，汇总子目录样例类型与数量，说明当前解析器对 `*_ext.xml`/`*-ext.xml`（表约束、supports/conflicts）的支持边界，并给出 `cpim_test_parser`/`dump_gmodel` 的推荐使用方式与过滤建议。

## 2025-12-23
- 更新文档 `docs/planning/SAC1_SAC3_INTEGRATION_DESIGN.md`：按 MSAC（SAC3 内嵌 AC3bit）叙述方式重写 CPU/GPU 统一方案；补齐“probe 阶段必须禁 `Tabular::weight` 更新（且 kernel AC 也要受控）”与 `max_probes/max_time_ms` 预算、并明确 GPU 侧 snapshot-based probe 与可落地的 Batch-1（持久化传播内核连续处理 probe）。
- 更新文档 `docs/planning/Batch_AC.md`：澄清“batched AC = SAC”仅对应一次 SAC-checking pass（非闭包），补充适用范围（仅二元 supports 表约束）、并明确 Batch-1/Batch-2 的工程边界与 micro-batch 内存成本。
- 更新文档 `docs/planning/SAC1_SAC3_INTEGRATION_DESIGN.md`：补充“以对照实验为导向”的 SAC 算法族实现计划，细化 CPU baseline 与 GPU-MSAC（Batch-1 优先）的分阶段验收标准与配置接口。

## 2025-10-21
- 新增文档：`aig_docs/GPU_GAC_UNIFIED_MEMORY_GUIDE.md`
  - 总结 Jetson Orin（统一内存 UMA）下的约束传播优化方案：统一内存单源数据、事件驱动、CPU/GPU 动态调度、持久化 kernel、设备侧队列与按字位集处理等；给出代码落点与渐进落地步骤，并关联 `GPU_GAC_PERSISTENT_STATE_MACHINE.md` 与 `GPU_GAC_PIPELINE_PLAN.md`。

- 调整 `include/model/xcsp_parser.h`，新增基准路径元数据结构与接口，以便统一处理单个文件、目录与清单。
- 更新 `include/model/libxml2_parser.h` 与 `src/model/libxml2_parser.cpp`，重写清单解析与路径归一化逻辑，支持 XCSP2 文件格式识别，并补充目录遍历、格式探测等辅助函数。
- 扩展 `samples/main_new_parser.cpp`，集成 Abseil Flags 命令行参数，支持列出基准文件、选择清单或直接路径，并输出检测到的 XCSP 版本。
- 在 `CMakeLists.txt` 中为 `cpim_test_parser` 目标链接 `absl::flags` 与 `absl::flags_parse`，满足新命令行功能的依赖。
- 新增 `CHANGES_ZH.md`（本文件），提供近期变更的中文汇总。
- 新增文档 `aig_docs/GPU_GAC_PERSISTENT_STATE_MACHINE.md`，提出基于“持久化内核 + 约束状态机 + 设备端队列”的 GAC 传播方案，并与 `aig_docs/GPU_GAC_PIPELINE_PLAN.md` 对比评估 Jetson Orin 适配性。

- 新增 CPU 基线传播工具：`cpim_gac_cpu`
  - 位置：`samples/run_gac_cpu.cpp`, `include/model/gac_cpu.h`, `src/model/gac_cpu.cpp`
  - 功能：串行 CPU 版队列压缩与 GAC 传播（仅二元 supports 约束），用于正确性基线与后续 GPU 优化对照。
  - 构建与运行：
    - 构建：`cmake -S . -B build_orin -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build_orin -j$(nproc)`
    - 运行：`./build_orin/cpim_gac_cpu --input=samples/bench/queens-4_ext.xml --max_print=8`

- GModel GPU 基线传播实现：`GModel::EnforceGAC`
  - 位置：`include/GModel.cuh`, `src/GModel.cu`, `src/model/gmodel_adapter.cu`, `samples/dump_gmodel.cpp`
  - 功能：参考 `cuSAC.cu` 的 `enforceGAC` 流程，在统一内存 `GModel` 上实现 CPU 串行队列 + GPU `CsCheckMain` kernel 的 GAC 传播，并引入共享内存缓存与按字节掩码归约，输出迭代次数、删除数及是否检测到不一致。
  - 使用方式：`./build_orin/dump_gmodel --input=... --run_gac`。
