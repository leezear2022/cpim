#!/usr/bin/env python3
"""
CPU AC 算法对比测试
比较不同一致性算法（AC3bit, RPC3, lMaxRPC, NSAC）在相同实例上的表现
"""

import subprocess
import sys
import re
from pathlib import Path
from typing import Dict, Optional, Tuple, List
from dataclasses import dataclass
import time

# 项目根目录
PROJECT_ROOT = Path(__file__).parent.parent.parent
BUILD_DIR = PROJECT_ROOT / "build"

# 导入测试集定义
sys.path.insert(0, str(PROJECT_ROOT / "tests/python"))
from tier_definitions import get_tier_by_number


@dataclass
class AlgorithmResult:
    """AC 算法求解结果"""
    algorithm: str
    solution: Optional[str] = None
    positives: int = 0
    negatives: int = 0
    time_ms: float = 0.0
    status: str = "UNKNOWN"  # SAT, UNSAT, TIMEOUT, ERROR
    error_msg: Optional[str] = None


def run_with_ac_algorithm(instance_path: str, ac_algorithm: str, timeout: int = 30) -> AlgorithmResult:
    """使用指定 AC 算法运行 CPU 求解器"""
    result = AlgorithmResult(algorithm=ac_algorithm)
    cmd = [
        str(BUILD_DIR / "cpim_test_parser"),
        f"--bench_path={instance_path}",
        f"--ac_algorithm={ac_algorithm}"
    ]

    try:
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout
        )

        output = proc.stdout + proc.stderr

        # 提取解
        solution_match = re.search(r'MAC solution \(canonical indices\): (.+)', output)
        if solution_match:
            result.solution = solution_match.group(1).strip()
            result.status = "SAT"

        # 提取节点数
        stats_match = re.search(r'MAC stats: time=(\d+) ms, positives=(\d+), negatives=(\d+)', output)
        if stats_match:
            result.time_ms = float(stats_match.group(1))
            result.positives = int(stats_match.group(2))
            result.negatives = int(stats_match.group(3))

        # 检查 UNSAT
        if "No solution found" in output or (result.positives == 0 and result.negatives == 0):
            result.status = "UNSAT"

        return result

    except subprocess.TimeoutExpired:
        result.status = "TIMEOUT"
        result.error_msg = f"Timeout after {timeout}s"
        return result
    except Exception as e:
        result.status = "ERROR"
        result.error_msg = str(e)
        return result


def compare_algorithms(results: Dict[str, AlgorithmResult], instance_name: str) -> Tuple[bool, str]:
    """对比不同 AC 算法的结果"""
    issues = []

    # 获取所有成功的算法
    successful = {alg: r for alg, r in results.items() if r.status in ["SAT", "UNSAT"]}

    if not successful:
        return False, "所有算法都失败"

    # 使用第一个成功的算法作为参考
    reference_alg = list(successful.keys())[0]
    reference = successful[reference_alg]

    # 1. 状态一致性
    for alg, result in successful.items():
        if result.status != reference.status:
            issues.append(f"{alg} 状态不一致: {result.status} (参考: {reference.status})")

    # 2. 解一致性（仅在 SAT 时检查）
    if reference.status == "SAT":
        for alg, result in successful.items():
            if result.solution != reference.solution:
                issues.append(f"{alg} 解不一致")

    # 3. 节点数对比（显示差异，但不视为错误）
    # 强一致性算法应该有更少的节点数
    node_counts = []
    for alg in ["AC3bit", "RPC3", "lMaxRPC", "NSAC"]:
        if alg in successful:
            r = successful[alg]
            total = r.positives + r.negatives
            node_counts.append(f"{alg}:{total}")

    if issues:
        return False, '\n'.join(issues)

    return True, f"✓ 一致 (节点数: {', '.join(node_counts)})"


def test_tier_with_algorithms(tier: int, algorithms: List[str], timeout: int = 30):
    """测试指定层级的所有实例，对比不同 AC 算法"""
    instances = get_tier_by_number(tier)

    print(f"\n{'='*100}")
    print(f"TIER {tier} AC 算法对比测试 ({len(instances)} 个实例)")
    print(f"算法: {', '.join(algorithms)}")
    print(f"{'='*100}\n")

    # 表头
    print(f"{'实例':40} {'状态':10} {'AC3bit':12} {'RPC3':12} {'lMaxRPC':12} {'NSAC':12} {'结果':20}")
    print(f"{'-'*100}")

    passed = 0
    failed = 0
    skipped = 0

    for i, instance_path in enumerate(instances, 1):
        instance_name = Path(instance_path).stem

        # 检查文件是否存在
        if not Path(instance_path).exists():
            print(f"{instance_name:40} SKIP")
            skipped += 1
            continue

        # 运行所有算法
        results = {}
        for alg in algorithms:
            results[alg] = run_with_ac_algorithm(instance_path, alg, timeout)

        # 对比结果
        is_consistent, msg = compare_algorithms(results, instance_name)

        # 格式化输出
        status = results[algorithms[0]].status

        def format_result(r: AlgorithmResult) -> str:
            if r.status == "TIMEOUT":
                return "TIMEOUT"
            elif r.status == "ERROR":
                return "ERROR"
            elif r.status in ["SAT", "UNSAT"]:
                total = r.positives + r.negatives
                return f"P{r.positives}+N{r.negatives}={total}"
            return "?"

        ac3bit_str = format_result(results["AC3bit"]) if "AC3bit" in results else "-"
        rpc3_str = format_result(results["RPC3"]) if "RPC3" in results else "-"
        lmax_str = format_result(results["lMaxRPC"]) if "lMaxRPC" in results else "-"
        nsac_str = format_result(results["NSAC"]) if "NSAC" in results else "-"

        result_str = "✓" if is_consistent else "✗ FAILED"

        print(f"{instance_name:40} {status:10} {ac3bit_str:12} {rpc3_str:12} {lmax_str:12} {nsac_str:12} {result_str:20}")

        if is_consistent:
            passed += 1
        else:
            failed += 1
            print(f"    {msg.replace(chr(10), chr(10) + '    ')}")

    # 总结
    print(f"\n{'='*100}")
    print(f"测试总结: {passed} 通过, {failed} 失败, {skipped} 跳过")
    print(f"{'='*100}\n")

    return passed, failed, skipped


def main():
    import argparse

    parser = argparse.ArgumentParser(description="CPU AC 算法对比测试")
    parser.add_argument("--tier", type=int, default=0, choices=[0, 1, 2],
                        help="测试层级 (0=快速烟测, 1=标准回归, 2=完整测试)")
    parser.add_argument("--algorithms", type=str, default="AC3bit,RPC3,lMaxRPC",
                        help="要测试的算法，逗号分隔（默认: AC3bit,RPC3,lMaxRPC）")
    parser.add_argument("--timeout", type=int, default=30,
                        help="单个实例超时时间(秒)")

    args = parser.parse_args()

    # 检查构建目录
    if not BUILD_DIR.exists():
        print(f"错误: 构建目录不存在: {BUILD_DIR}")
        sys.exit(1)

    cpu_solver = BUILD_DIR / "cpim_test_parser"
    if not cpu_solver.exists():
        print(f"错误: CPU 求解器不存在: {cpu_solver}")
        sys.exit(1)

    # 解析算法列表
    algorithms = [a.strip() for a in args.algorithms.split(",")]

    # 运行测试
    passed, failed, skipped = test_tier_with_algorithms(args.tier, algorithms, args.timeout)

    # 返回码
    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
