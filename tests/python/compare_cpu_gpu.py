#!/usr/bin/env python3
"""
CPU vs GPU 求解器对比测试
验证两者的搜索节点数和解完全一致
"""

import subprocess
import sys
import re
from pathlib import Path
from typing import Dict, Optional, Tuple
import time

# 项目根目录
PROJECT_ROOT = Path(__file__).parent.parent.parent
BUILD_DIR = PROJECT_ROOT / "build"

# 导入测试集定义
sys.path.insert(0, str(PROJECT_ROOT / "tests/python"))
from tier_definitions import get_tier_by_number


class SolverResult:
    """求解器结果"""
    def __init__(self):
        self.solution: Optional[str] = None
        self.positives: int = 0
        self.negatives: int = 0
        self.time_ms: float = 0.0
        self.status: str = "UNKNOWN"  # SAT, UNSAT, TIMEOUT, ERROR
        self.error_msg: Optional[str] = None


def run_cpu_solver(instance_path: str, timeout: int = 30) -> SolverResult:
    """运行 CPU 求解器"""
    result = SolverResult()
    cmd = [
        str(BUILD_DIR / "cpim_test_parser"),
        f"--bench_path={instance_path}"
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
        if "No solution found" in output or result.positives == 0:
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


def run_gpu_solver(instance_path: str, timeout: int = 30) -> SolverResult:
    """运行 GPU 求解器"""
    result = SolverResult()
    cmd = [str(BUILD_DIR / "gmodel_solver"), instance_path]

    try:
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout
        )

        output = proc.stdout + proc.stderr

        # 提取解
        solution_lines = []
        in_solution = False
        for line in output.split('\n'):
            if "=== Solution Found! ===" in line:
                in_solution = True
                continue
            if in_solution:
                var_match = re.match(r'\s*var\[(\d+)\] = (\d+)', line)
                if var_match:
                    solution_lines.append(var_match.group(2))
                elif line.strip().startswith('['):
                    break

        if solution_lines:
            result.solution = ' '.join(solution_lines)
            result.status = "SAT"

        # 检查 UNSAT
        if "=== No Solution Found ===" in output:
            result.status = "UNSAT"

        # 提取节点数统计
        positives_match = re.search(r'Positives: (\d+)', output)
        negatives_match = re.search(r'Negatives: (\d+)', output)
        if positives_match:
            result.positives = int(positives_match.group(1))
        if negatives_match:
            result.negatives = int(negatives_match.group(1))

        return result

    except subprocess.TimeoutExpired:
        result.status = "TIMEOUT"
        result.error_msg = f"Timeout after {timeout}s"
        return result
    except Exception as e:
        result.status = "ERROR"
        result.error_msg = str(e)
        return result


def compare_results(cpu: SolverResult, gpu: SolverResult, instance_name: str) -> Tuple[bool, str]:
    """对比 CPU 和 GPU 结果"""
    issues = []

    # 1. 状态一致性
    if cpu.status != gpu.status:
        issues.append(f"状态不一致: CPU={cpu.status}, GPU={gpu.status}")

    # 2. 解一致性
    if cpu.status == "SAT" and gpu.status == "SAT":
        if cpu.solution != gpu.solution:
            issues.append(f"解不一致:\n  CPU: {cpu.solution}\n  GPU: {gpu.solution}")

    # 3. 节点数一致性（仅在都成功时检查）
    if cpu.status in ["SAT", "UNSAT"] and gpu.status in ["SAT", "UNSAT"]:
        if cpu.positives != gpu.positives:
            issues.append(f"正向节点数不一致: CPU={cpu.positives}, GPU={gpu.positives}")
        if cpu.negatives != gpu.negatives:
            issues.append(f"回溯节点数不一致: CPU={cpu.negatives}, GPU={gpu.negatives}")

    # 4. 错误信息
    if cpu.status == "ERROR":
        issues.append(f"CPU 错误: {cpu.error_msg}")
    if gpu.status == "ERROR":
        issues.append(f"GPU 错误: {gpu.error_msg}")

    if issues:
        return False, '\n'.join(issues)
    return True, "✓ 一致"


def test_tier(tier: int, timeout: int = 30, verbose: bool = True):
    """测试指定层级"""
    instances = get_tier_by_number(tier)

    print(f"\n{'='*80}")
    print(f"TIER {tier} 对比测试 ({len(instances)} 个实例)")
    print(f"{'='*80}\n")

    passed = 0
    failed = 0
    skipped = 0

    results = []

    for i, instance_path in enumerate(instances, 1):
        instance_name = Path(instance_path).name

        # 检查文件是否存在
        if not Path(instance_path).exists():
            print(f"{i:2}. [SKIP] {instance_name} - 文件不存在")
            skipped += 1
            continue

        print(f"{i:2}. {instance_name:50}", end=' ', flush=True)

        # 运行 CPU 求解器
        cpu_result = run_cpu_solver(instance_path, timeout)

        # 运行 GPU 求解器
        gpu_result = run_gpu_solver(instance_path, timeout)

        # 对比结果
        is_consistent, msg = compare_results(cpu_result, gpu_result, instance_name)

        if is_consistent:
            status_str = f"{cpu_result.status:8} P={cpu_result.positives:4} N={cpu_result.negatives:4}"
            print(f"{status_str} ✓")
            passed += 1
        else:
            print(f"✗ FAILED")
            print(f"    {msg.replace(chr(10), chr(10) + '    ')}")
            failed += 1

        results.append({
            'instance': instance_name,
            'cpu': cpu_result,
            'gpu': gpu_result,
            'consistent': is_consistent,
            'message': msg
        })

    # 总结
    print(f"\n{'='*80}")
    print(f"测试总结: {passed} 通过, {failed} 失败, {skipped} 跳过")
    print(f"{'='*80}\n")

    return passed, failed, skipped, results


def main():
    import argparse

    parser = argparse.ArgumentParser(description="CPU vs GPU 求解器对比测试")
    parser.add_argument("--tier", type=int, default=0, choices=[0, 1],
                        help="测试层级 (0=快速烟测, 1=标准回归)")
    parser.add_argument("--timeout", type=int, default=30,
                        help="单个实例超时时间(秒)")
    parser.add_argument("--verbose", action="store_true",
                        help="详细输出")

    args = parser.parse_args()

    # 检查构建目录
    if not BUILD_DIR.exists():
        print(f"错误: 构建目录不存在: {BUILD_DIR}")
        sys.exit(1)

    cpu_solver = BUILD_DIR / "cpim_test_parser"
    gpu_solver = BUILD_DIR / "gmodel_solver"

    if not cpu_solver.exists():
        print(f"错误: CPU 求解器不存在: {cpu_solver}")
        sys.exit(1)

    if not gpu_solver.exists():
        print(f"错误: GPU 求解器不存在: {gpu_solver}")
        sys.exit(1)

    # 运行测试
    passed, failed, skipped, results = test_tier(args.tier, args.timeout, args.verbose)

    # 返回码
    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
