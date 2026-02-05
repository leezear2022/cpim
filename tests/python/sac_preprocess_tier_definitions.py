#!/usr/bin/env python3
"""
SAC preprocess 评测集（由 select_sac_preprocess_benches.py 数据驱动生成）。

说明：
- 这些 tier 主要用于跑 `./build/sac_benchmark --mode=full_sac`（preprocess 口径）
- 不追求覆盖所有类型；优先挑选 “删值多/传播深/可复现” 的实例用于展示推理能力与吞吐
"""

from typing import List

from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent

def _resolve_path(path: str) -> str:
    return str(PROJECT_ROOT / path)

SAC_PREPROCESS_TIER0: List[str] = [
    _resolve_path('benchmarks/marc/large-84-unsat_ext.xml'),
    _resolve_path('benchmarks/marc/large-80-unsat_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-11_ext.xml'),
    _resolve_path('benchmarks/composed-25-10-20/composed-25-10-20-6_ext.xml'),
    _resolve_path('benchmarks/composed-25-10-20/composed-25-10-20-3_ext.xml'),
    _resolve_path('benchmarks/composed-25-10-20/composed-25-10-20-5_ext.xml'),
    _resolve_path('benchmarks/composed-25-10-20/composed-25-10-20-8_ext.xml'),
    _resolve_path('benchmarks/composed-25-10-20/composed-25-10-20-1_ext.xml'),
    _resolve_path('benchmarks/composed-25-10-20/composed-25-10-20-7_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-46_ext.xml'),
    _resolve_path('benchmarks/composed-25-1-2/composed-25-1-2-4_ext.xml'),
    _resolve_path('benchmarks/latinSquare/composed-25-1-2/composed-25-1-2-4_ext.xml'),
]

SAC_PREPROCESS_TIER1: List[str] = [
    _resolve_path('benchmarks/marc/large-84-unsat_ext.xml'),
    _resolve_path('benchmarks/marc/large-80-unsat_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-11_ext.xml'),
    _resolve_path('benchmarks/composed-25-10-20/composed-25-10-20-6_ext.xml'),
    _resolve_path('benchmarks/composed-25-10-20/composed-25-10-20-3_ext.xml'),
    _resolve_path('benchmarks/composed-25-10-20/composed-25-10-20-5_ext.xml'),
    _resolve_path('benchmarks/composed-25-10-20/composed-25-10-20-8_ext.xml'),
    _resolve_path('benchmarks/composed-25-10-20/composed-25-10-20-1_ext.xml'),
    _resolve_path('benchmarks/composed-25-10-20/composed-25-10-20-7_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-46_ext.xml'),
    _resolve_path('benchmarks/composed-25-1-2/composed-25-1-2-4_ext.xml'),
    _resolve_path('benchmarks/latinSquare/composed-25-1-2/composed-25-1-2-4_ext.xml'),
    _resolve_path('benchmarks/composed-25-1-2/composed-25-1-2-9_ext.xml'),
    _resolve_path('benchmarks/composed-25-1-25/composed-25-1-25-8_ext.xml'),
    _resolve_path('benchmarks/latinSquare/composed-25-1-2/composed-25-1-2-8_ext.xml'),
    _resolve_path('benchmarks/composed-25-1-25/composed-25-1-25-2_ext.xml'),
    _resolve_path('benchmarks/latinSquare/composed-25-1-2/composed-25-1-2-2_ext.xml'),
    _resolve_path('benchmarks/latinSquare/composed-25-1-2/composed-25-1-2-7_ext.xml'),
    _resolve_path('benchmarks/composed-25-1-2/composed-25-1-2-7_ext.xml'),
    _resolve_path('benchmarks/composed-25-1-25/composed-25-1-25-7_ext.xml'),
    _resolve_path('benchmarks/composed-75-1-2/composed-75-1-2-5_ext.xml'),
    _resolve_path('benchmarks/composed-25-1-25/composed-25-1-25-0_ext.xml'),
    _resolve_path('benchmarks/composed-25-1-2/composed-25-1-2-0_ext.xml'),
    _resolve_path('benchmarks/composed-75-1-2/composed-75-1-2-9_ext.xml'),
    _resolve_path('benchmarks/composed-75-1-2/composed-75-1-2-1_ext.xml'),
    _resolve_path('benchmarks/composed-25-1-25/composed-25-1-25-6_ext.xml'),
    _resolve_path('benchmarks/latinSquare/composed-25-1-2/composed-25-1-2-1_ext.xml'),
    _resolve_path('benchmarks/composed-75-1-2/composed-75-1-2-4_ext.xml'),
    _resolve_path('benchmarks/composed-75-1-2/composed-75-1-2-7_ext.xml'),
    _resolve_path('benchmarks/driver/driverlogw-05c-sat_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-25_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-93_ext.xml'),
    _resolve_path('benchmarks/driver/driverlogw-04c-sat_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-3_ext.xml'),
    _resolve_path('benchmarks/driver/driverlogw-02c-sat_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-81_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-99_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-19_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-55_ext.xml'),
]

SAC_PREPROCESS_TIER2: List[str] = [
    _resolve_path('benchmarks/marc/large-84-unsat_ext.xml'),
    _resolve_path('benchmarks/marc/large-80-unsat_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-11_ext.xml'),
    _resolve_path('benchmarks/composed-25-10-20/composed-25-10-20-6_ext.xml'),
    _resolve_path('benchmarks/composed-25-10-20/composed-25-10-20-3_ext.xml'),
    _resolve_path('benchmarks/composed-25-10-20/composed-25-10-20-5_ext.xml'),
    _resolve_path('benchmarks/composed-25-10-20/composed-25-10-20-8_ext.xml'),
    _resolve_path('benchmarks/composed-25-10-20/composed-25-10-20-1_ext.xml'),
    _resolve_path('benchmarks/composed-25-10-20/composed-25-10-20-7_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-46_ext.xml'),
    _resolve_path('benchmarks/composed-25-1-2/composed-25-1-2-4_ext.xml'),
    _resolve_path('benchmarks/latinSquare/composed-25-1-2/composed-25-1-2-4_ext.xml'),
    _resolve_path('benchmarks/composed-25-1-2/composed-25-1-2-9_ext.xml'),
    _resolve_path('benchmarks/composed-25-1-25/composed-25-1-25-8_ext.xml'),
    _resolve_path('benchmarks/latinSquare/composed-25-1-2/composed-25-1-2-8_ext.xml'),
    _resolve_path('benchmarks/composed-25-1-25/composed-25-1-25-2_ext.xml'),
    _resolve_path('benchmarks/latinSquare/composed-25-1-2/composed-25-1-2-2_ext.xml'),
    _resolve_path('benchmarks/latinSquare/composed-25-1-2/composed-25-1-2-7_ext.xml'),
    _resolve_path('benchmarks/composed-25-1-2/composed-25-1-2-7_ext.xml'),
    _resolve_path('benchmarks/composed-25-1-25/composed-25-1-25-7_ext.xml'),
    _resolve_path('benchmarks/composed-75-1-2/composed-75-1-2-5_ext.xml'),
    _resolve_path('benchmarks/composed-25-1-25/composed-25-1-25-0_ext.xml'),
    _resolve_path('benchmarks/composed-25-1-2/composed-25-1-2-0_ext.xml'),
    _resolve_path('benchmarks/composed-75-1-2/composed-75-1-2-9_ext.xml'),
    _resolve_path('benchmarks/composed-75-1-2/composed-75-1-2-1_ext.xml'),
    _resolve_path('benchmarks/composed-25-1-25/composed-25-1-25-6_ext.xml'),
    _resolve_path('benchmarks/latinSquare/composed-25-1-2/composed-25-1-2-1_ext.xml'),
    _resolve_path('benchmarks/composed-75-1-2/composed-75-1-2-4_ext.xml'),
    _resolve_path('benchmarks/composed-75-1-2/composed-75-1-2-7_ext.xml'),
    _resolve_path('benchmarks/driver/driverlogw-05c-sat_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-25_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-93_ext.xml'),
    _resolve_path('benchmarks/driver/driverlogw-04c-sat_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-3_ext.xml'),
    _resolve_path('benchmarks/driver/driverlogw-02c-sat_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-81_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-99_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-19_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-55_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-28_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-7_ext.xml'),
    _resolve_path('benchmarks/QCP-15/qcp-15-120-2_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-1_ext.xml'),
    _resolve_path('benchmarks/tightness0.8/rand-2-40-80-103-800-93_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-18_ext.xml'),
    _resolve_path('benchmarks/tightness0.8/rand-2-40-80-103-800-15_ext.xml'),
    _resolve_path('benchmarks/tightness0.8/rand-2-40-80-103-800-82_ext.xml'),
    _resolve_path('benchmarks/rand-2-30-15/rand-2-30-15-306-230-37_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-60_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-27_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-47_ext.xml'),
    _resolve_path('benchmarks/frb30-15/frb30-15/frb30-15-4_ext.xml'),
    _resolve_path('benchmarks/langford/langford-3-9-ext.xml'),
    _resolve_path('benchmarks/langford/langford-3-11-ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-34_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-95_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-17_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-13_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-29_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-62_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-35_ext.xml'),
    _resolve_path('benchmarks/tightness0.8/rand-2-40-80-103-800-49_ext.xml'),
    _resolve_path('benchmarks/tightness0.8/rand-2-40-80-103-800-81_ext.xml'),
    _resolve_path('benchmarks/tightness0.8/rand-2-40-80-103-800-35_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-80_ext.xml'),
    _resolve_path('benchmarks/tightness0.8/rand-2-40-80-103-800-43_ext.xml'),
    _resolve_path('benchmarks/tightness0.8/rand-2-40-80-103-800-11_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-94_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-74_ext.xml'),
    _resolve_path('benchmarks/tightness0.8/rand-2-40-80-103-800-64_ext.xml'),
    _resolve_path('benchmarks/tightness0.8/rand-2-40-80-103-800-10_ext.xml'),
    _resolve_path('benchmarks/tightness0.8/rand-2-40-80-103-800-44_ext.xml'),
    _resolve_path('benchmarks/tightness0.8/rand-2-40-80-103-800-18_ext.xml'),
    _resolve_path('benchmarks/tightness0.8/rand-2-40-80-103-800-79_ext.xml'),
    _resolve_path('benchmarks/tightness0.8/rand-2-40-80-103-800-95_ext.xml'),
    _resolve_path('benchmarks/tightness0.8/rand-2-40-80-103-800-31_ext.xml'),
    _resolve_path('benchmarks/tightness0.8/rand-2-40-80-103-800-60_ext.xml'),
    _resolve_path('benchmarks/tightness0.8/rand-2-40-80-103-800-56_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-10_ext.xml'),
]

# -----------------------------------------------------------------------------
# 手工精选：用于“每次改动都跑”的回归/性能哨兵集
#
# 说明：
# - tier0/1/2 偏向“数据驱动生成”的展示集，可能会随筛选 CSV 刷新而变化；
# - 下列 suite 追求“覆盖路径 + 可复现 + 运行时间可控”，更适合当作日常回归与关键节点性能对照。
# -----------------------------------------------------------------------------

# 覆盖：高删值（TIMEOUT@round=1）、高 probes 吞吐、SAC-DWO、0 删值收敛、较高耗时样例。
SAC_PREPROCESS_REGRESSION: List[str] = [
    # 高删值（展示 pruning 能力）
    _resolve_path('benchmarks/composed-25-10-20/composed-25-10-20-2_ext.xml'),
    _resolve_path('benchmarks/composed-25-10-20/composed-25-10-20-6_ext.xml'),
    _resolve_path('benchmarks/composed-25-10-20/composed-25-10-20-3_ext.xml'),
    # 高 probes（吞吐/调度压力）
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-11_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-46_ext.xml'),
    # SAC-DWO（失败路径）
    _resolve_path('benchmarks/composed-25-1-2/composed-25-1-2-4_ext.xml'),
    _resolve_path('benchmarks/composed-75-1-2/composed-75-1-2-5_ext.xml'),
    # 0 删值但收敛（常见“没 pruning”的实例，验证收敛路径与吞吐）
    _resolve_path('benchmarks/langford/langford-3-11-ext.xml'),
    # 较高耗时（latency 哨兵）
    _resolve_path('benchmarks/QCP-15/qcp-15-120-2_ext.xml'),
    _resolve_path('benchmarks/driver/driverlogw-05c-sat_ext.xml'),
    # UNSAT（快速 DWO 路径；避免把“大实例解析/建模耗时”混入日常回归）
    _resolve_path('benchmarks/composed-25-1-2/composed-25-1-2-0_ext.xml'),
]

# 关键节点性能对照：偏“吞吐/延迟/内存”哨兵（建议 10 分钟超时 + 可续跑）。
SAC_PREPROCESS_PERF_SENTINELS: List[str] = [
    # 高 probes（吞吐）
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-11_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-46_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-25_ext.xml'),
    _resolve_path('benchmarks/tightness0.9/rand-2-40-180-84-900-90_ext.xml'),
    # latency（driver 系）
    _resolve_path('benchmarks/driver/driverlogw-05c-sat_ext.xml'),
    _resolve_path('benchmarks/driver/driverlogw-04c-sat_ext.xml'),
    _resolve_path('benchmarks/driver/driverlogw-02c-sat_ext.xml'),
    # 结构化实例（常见实际类）
    _resolve_path('benchmarks/langford/langford-3-11-ext.xml'),
    _resolve_path('benchmarks/QCP-15/qcp-15-120-14_ext.xml'),
]

# 可选：压力/已知问题样例（不建议纳入日常回归）。
SAC_PREPROCESS_STRESS: List[str] = [
    # 大规模 UNSAT（内存压力 + DWO；在 60s perf 预算下可能 timeout，建议放到 stress）
    _resolve_path('benchmarks/marc/large-80-unsat_ext.xml'),
    _resolve_path('benchmarks/marc/large-84-unsat_ext.xml'),
    # Jetson Orin 8G 上可能 OOM（已在 scan_10min 里出现过）。
    _resolve_path('benchmarks/marc/large-92-unsat_ext.xml'),
]

# 已知解析/输出问题样例（用于扫描时 exclude；避免污染统计）。
SAC_PREPROCESS_KNOWN_BAD: List[str] = [
    _resolve_path('benchmarks/graphs/graphw-05_ext.xml'),
    _resolve_path('benchmarks/graphs/graphw-06_ext.xml'),
    _resolve_path('benchmarks/graphs/graphw-07_ext.xml'),
    _resolve_path('benchmarks/graphs/graphw-12_ext.xml'),
    _resolve_path('benchmarks/graphs/graphw-13_ext.xml'),
]
