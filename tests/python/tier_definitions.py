#!/usr/bin/env python3
"""
测试集定义模块 - 集中管理 CPIM 测试实例

测试分层：
- TIER0: 快速烟测（12个，<10s）- 日常开发使用
- TIER1: 标准回归（39个，<60s）- 合并前验证
- TIER2: 完整验证（79个，<300s）- 发版前测试
- TIER3: 夜间全量（1000+个）- 压力测试
"""

from pathlib import Path
from typing import List
import os

# 项目根目录
PROJECT_ROOT = Path(__file__).parent.parent.parent  # tests/python/ -> cpim/
BENCH_ROOT = PROJECT_ROOT / "benchmarks"
SAMPLE_ROOT = PROJECT_ROOT / "tests/data/bench"


def _resolve_path(path: str) -> str:
    """解析路径为绝对路径"""
    return str(PROJECT_ROOT / path)


# ============================================================================
# TIER0: 快速烟测（12 个实例，<10s）
# 用途：每次代码修改后快速验证基本功能
# 覆盖：基本功能、SAT/UNSAT、多种约束类型、不同难度
# ============================================================================

TIER0: List[str] = [
    # 1. 极小实例（可穷举验证）
    _resolve_path("tests/data/bench/test.xml"),                        # 3 变量
    _resolve_path("tests/data/bench/queens-4_ext.xml"),                # 4 变量，N-Queens

    # 2. 经典组合问题
    _resolve_path("benchmarks/langford/langford-2-4-ext.xml"),         # 8 变量，<0.5s
    _resolve_path("benchmarks/langford/langford-3-9-ext.xml"),         # 27 变量，<2s

    # 3. 不同紧度梯度
    _resolve_path("benchmarks/tightness0.1/rand-2-40-8-753-100-0_ext.xml"),   # 松散，<1s
    _resolve_path("benchmarks/tightness0.1/rand-2-40-8-753-100-5_ext.xml"),   # 松散，<1s
    _resolve_path("benchmarks/tightness0.5/rand-2-40-25-180-500-0_ext.xml"),  # 中等，<5s
    _resolve_path("benchmarks/tightness0.8/rand-2-40-80-103-800-0_ext.xml"),  # 紧密，<8s

    # 4. 多样化问题类型
    _resolve_path("benchmarks/driver/driverlogw-01c-sat_ext.xml"),    # 实际应用，<3s
    _resolve_path("benchmarks/BH-4-4/BlackHole-4-4-e-0_ext.xml"),     # BlackHole，<5s
    _resolve_path("benchmarks/composed-25-1-2/composed-25-1-2-0_ext.xml"),  # 组合问题，<8s

    # 5. UNSAT 实例（验证不可满足性检测）
    _resolve_path("benchmarks/graphs/graphw-05_ext.xml"),             # 图着色 UNSAT，<5s
]


# ============================================================================
# TIER1: 标准回归（39 个实例，<60s）
# 用途：合并前必须通过的测试
# 覆盖：TIER0 + 多样化约束、不同难度梯度
# ============================================================================

TIER1: List[str] = TIER0 + [
    # Langford 扩展（2 个）
    _resolve_path("benchmarks/langford/langford-3-11-ext.xml"),       # ~10s
    _resolve_path("benchmarks/langford/slangford-3-11-ext.xml"),      # ~15s

    # Tightness0.1 松散约束（5 个）
    _resolve_path("benchmarks/tightness0.1/rand-2-40-8-753-100-1_ext.xml"),
    _resolve_path("benchmarks/tightness0.1/rand-2-40-8-753-100-2_ext.xml"),
    _resolve_path("benchmarks/tightness0.1/rand-2-40-8-753-100-3_ext.xml"),
    _resolve_path("benchmarks/tightness0.1/rand-2-40-8-753-100-10_ext.xml"),
    _resolve_path("benchmarks/tightness0.1/rand-2-40-8-753-100-15_ext.xml"),

    # Tightness0.5 中等紧度（5 个）
    _resolve_path("benchmarks/tightness0.5/rand-2-40-25-180-500-1_ext.xml"),
    _resolve_path("benchmarks/tightness0.5/rand-2-40-25-180-500-2_ext.xml"),
    _resolve_path("benchmarks/tightness0.5/rand-2-40-25-180-500-3_ext.xml"),
    _resolve_path("benchmarks/tightness0.5/rand-2-40-25-180-500-10_ext.xml"),
    _resolve_path("benchmarks/tightness0.5/rand-2-40-25-180-500-11_ext.xml"),

    # Tightness0.8 紧密约束（4 个）
    _resolve_path("benchmarks/tightness0.8/rand-2-40-80-103-800-1_ext.xml"),
    _resolve_path("benchmarks/tightness0.8/rand-2-40-80-103-800-2_ext.xml"),
    _resolve_path("benchmarks/tightness0.8/rand-2-40-80-103-800-5_ext.xml"),
    _resolve_path("benchmarks/tightness0.8/rand-2-40-80-103-800-10_ext.xml"),

    # Composed 组合问题（4 个）
    _resolve_path("benchmarks/composed-25-1-2/composed-25-1-2-1_ext.xml"),
    _resolve_path("benchmarks/composed-25-1-2/composed-25-1-2-2_ext.xml"),
    _resolve_path("benchmarks/composed-25-1-2/composed-25-1-2-3_ext.xml"),
    _resolve_path("benchmarks/composed-25-1-2/composed-25-1-2-5_ext.xml"),

    # Driver 实际问题（2 个）
    _resolve_path("benchmarks/driver/driverlogw-02c-sat_ext.xml"),
    _resolve_path("benchmarks/driver/driverlogw-04c-sat_ext.xml"),

    # BH-4-4 BlackHole（3 个）
    _resolve_path("benchmarks/BH-4-4/BlackHole-4-4-e-1_ext.xml"),
    _resolve_path("benchmarks/BH-4-4/BlackHole-4-4-e-2_ext.xml"),
    _resolve_path("benchmarks/BH-4-4/BlackHole-4-4-e-3_ext.xml"),

    # Graphs UNSAT（2 个）
    _resolve_path("benchmarks/graphs/graphw-06_ext.xml"),
    _resolve_path("benchmarks/graphs/graphw-07_ext.xml"),
]


# ============================================================================
# TIER2: 完整验证（79 个实例，<300s）
# 用途：重大修改或发版前的完整验证
# 覆盖：TIER1 + 边界情况、困难实例、大规模问题
# ============================================================================

def _get_tier2_additional() -> List[str]:
    """TIER2 额外的 40 个实例"""
    additional = []

    # Tightness0.2 系列（5 个）
    tightness02_dir = BENCH_ROOT / "tightness0.2"
    if tightness02_dir.exists():
        files = sorted(tightness02_dir.glob("*_ext.xml"))[:5]
        additional.extend([str(f) for f in files])

    # Tightness0.35 系列（5 个）
    tightness035_dir = BENCH_ROOT / "tightness0.35"
    if tightness035_dir.exists():
        files = sorted(tightness035_dir.glob("*_ext.xml"))[:5]
        additional.extend([str(f) for f in files])

    # Tightness0.9 困难实例（5 个）
    for i in [0, 1, 2, 5, 10]:
        path = BENCH_ROOT / f"tightness0.9/rand-2-40-180-84-900-{i}_ext.xml"
        if path.exists():
            additional.append(str(path))

    # Composed 大规模（5 个）
    for i in [6, 7, 8, 9]:
        path = BENCH_ROOT / f"composed-25-1-2/composed-25-1-2-{i}_ext.xml"
        if path.exists():
            additional.append(str(path))

    composed_alt_dir = BENCH_ROOT / "composed-25-10-20"
    if composed_alt_dir.exists():
        files = sorted(composed_alt_dir.glob("*_ext.xml"))[:1]
        additional.extend([str(f) for f in files])

    # Rand-2-23 系列（10 个）
    rand23_dir = SAMPLE_ROOT / "rand-2-23"
    if rand23_dir.exists():
        files = sorted(rand23_dir.glob("*.xml"))
        additional.extend([str(f) for f in files])

    # Driver 复杂实例（2 个）
    for name in ["driverlogw-08c-sat_ext.xml", "driverlogw-09-sat_ext.xml"]:
        path = BENCH_ROOT / f"driver/{name}"
        if path.exists():
            additional.append(str(path))

    # 多样化实例（8 个）
    for dir_name, count in [("rand-2-24", 3), ("rand-2-26", 2), ("rand-8-20-5", 3)]:
        dir_path = BENCH_ROOT / dir_name
        if dir_path.exists():
            files = sorted(dir_path.glob("*_ext.xml"))[:count]
            additional.extend([str(f) for f in files])

    return additional


# TIER2 实例列表（延迟计算）
_TIER2_CACHE: List[str] = None

def get_tier2() -> List[str]:
    """获取 TIER2 测试集（75 个实例）"""
    global _TIER2_CACHE
    if _TIER2_CACHE is None:
        _TIER2_CACHE = TIER1 + _get_tier2_additional()
    return _TIER2_CACHE


# ============================================================================
# TIER3: 夜间全量测试（1000+ 个实例）
# 用途：发现潜在问题、性能回归
# 覆盖：所有支持的测试实例（排除 predicates/WCSP）
# ============================================================================

def get_tier3() -> List[str]:
    """
    获取 TIER3 测试集（动态扫描）

    排除规则：
    - 不支持的约束类型：queens, haystacks, jobShop*, coloring (使用 predicates 或 WCSP)
    - 非 XML 文件
    """
    exclude_dirs = {"queens", "haystacks", "jobShop", "coloring"}
    exclude_keywords = ["_wcsp", "pred"]

    all_files = []

    # 扫描 benchmarks/ 目录
    if BENCH_ROOT.exists():
        for xml_file in BENCH_ROOT.rglob("*.xml"):
            # 检查是否在排除目录中
            if any(excl in xml_file.parts for excl in exclude_dirs):
                continue

            # 检查文件名是否包含排除关键词
            if any(kw in xml_file.name for kw in exclude_keywords):
                continue

            all_files.append(str(xml_file))

    # 添加 tests/data/bench/ 目录（包括 rand-2-23）
    if SAMPLE_ROOT.exists():
        for xml_file in SAMPLE_ROOT.rglob("*.xml"):
            if any(kw in xml_file.name for kw in exclude_keywords):
                continue
            all_files.append(str(xml_file))

    return sorted(set(all_files))  # 去重并排序


# ============================================================================
# 辅助函数
# ============================================================================

def get_tier_by_number(tier: int) -> List[str]:
    """根据层级编号获取测试集"""
    if tier == 0:
        return TIER0
    elif tier == 1:
        return TIER1
    elif tier == 2:
        return get_tier2()
    elif tier == 3:
        return get_tier3()
    else:
        raise ValueError(f"Invalid tier number: {tier}. Must be 0, 1, 2, or 3.")


def get_tier_stats() -> dict:
    """获取各层级的统计信息"""
    return {
        "TIER0": {
            "count": len(TIER0),
            "description": "快速烟测",
            "timeout": "10s",
            "usage": "日常开发"
        },
        "TIER1": {
            "count": len(TIER1),
            "description": "标准回归",
            "timeout": "60s",
            "usage": "合并前验证"
        },
        "TIER2": {
            "count": len(get_tier2()),
            "description": "完整验证",
            "timeout": "300s",
            "usage": "发版前测试"
        },
        "TIER3": {
            "count": len(get_tier3()),
            "description": "夜间全量",
            "timeout": "900s",
            "usage": "压力测试"
        }
    }


# ============================================================================
# CLI 测试接口
# ============================================================================

if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description="CPIM 测试集定义模块")
    parser.add_argument("--tier", type=int, choices=[0, 1, 2, 3],
                        help="显示指定层级的测试实例")
    parser.add_argument("--stats", action="store_true",
                        help="显示统计信息")
    parser.add_argument("--check", action="store_true",
                        help="检查测试文件是否存在")

    args = parser.parse_args()

    if args.stats:
        stats = get_tier_stats()
        print("\n=== CPIM 测试集统计 ===\n")
        for tier_name, info in stats.items():
            print(f"{tier_name}:")
            print(f"  实例数: {info['count']}")
            print(f"  描述: {info['description']}")
            print(f"  超时: {info['timeout']}")
            print(f"  用途: {info['usage']}")
            print()

    elif args.tier is not None:
        files = get_tier_by_number(args.tier)
        print(f"\n=== TIER{args.tier} 测试实例（{len(files)} 个）===\n")
        for i, f in enumerate(files, 1):
            exists = "✓" if os.path.exists(f) else "✗"
            print(f"{i:3}. [{exists}] {os.path.relpath(f, PROJECT_ROOT)}")

    elif args.check:
        print("\n=== 检查测试文件完整性 ===\n")
        for tier in [0, 1, 2]:
            files = get_tier_by_number(tier)
            missing = [f for f in files if not os.path.exists(f)]

            if missing:
                print(f"TIER{tier}: {len(missing)}/{len(files)} 个文件缺失")
                for f in missing[:5]:  # 只显示前 5 个
                    print(f"  - {os.path.relpath(f, PROJECT_ROOT)}")
                if len(missing) > 5:
                    print(f"  ... 还有 {len(missing) - 5} 个文件")
            else:
                print(f"TIER{tier}: ✓ 全部 {len(files)} 个文件存在")
        print()

    else:
        parser.print_help()
