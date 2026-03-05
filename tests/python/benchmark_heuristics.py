#!/usr/bin/env python3
"""
对比不同变量选择启发式的性能
"""

import subprocess
import sys
import re
from pathlib import Path
from typing import Dict, Optional

# 项目根目录
PROJECT_ROOT = Path(__file__).parent.parent.parent
BUILD_DIR = PROJECT_ROOT / "build"

HEURISTICS = ["min_domain", "dom_deg", "dom_ddeg", "dom_wdeg", "vsids"]

class HeuristicResult:
    """启发式测试结果"""
    def __init__(self):
        self.positives: int = 0
        self.negatives: int = 0
        self.status: str = "UNKNOWN"  # SAT, UNSAT, TIMEOUT, ERROR
        self.error_msg: Optional[str] = None


def run_gmodel_solver(instance_path: str, heuristic: str, timeout: int = 30) -> HeuristicResult:
    """运行 GPU 求解器"""
    result = HeuristicResult()
    cmd = [
        str(BUILD_DIR / "gmodel_solver"),
        instance_path,
        f"--heuristic={heuristic}"
    ]

    try:
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout
        )

        output = proc.stdout + proc.stderr

        # 检查求解状态
        if "=== Solution Found! ===" in output:
            result.status = "SAT"
        elif "=== No Solution Found ===" in output:
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


def benchmark_instance(instance_path: str, timeout: int = 30):
    """对单个实例测试所有启发式"""
    instance_name = Path(instance_path).name

    if not Path(instance_path).exists():
        print(f"[SKIP] {instance_name} - 文件不存在")
        return

    print(f"\n{'='*80}")
    print(f"实例: {instance_name}")
    print(f"{'='*80}")

    results = {}
    for heuristic in HEURISTICS:
        print(f"  测试 {heuristic:12} ... ", end='', flush=True)
        result = run_gmodel_solver(instance_path, heuristic, timeout)
        results[heuristic] = result

        if result.status == "SAT":
            total_nodes = result.positives + result.negatives
            print(f"{result.status:8} P={result.positives:4} N={result.negatives:4} Total={total_nodes:5}")
        elif result.status == "TIMEOUT":
            print(f"{result.status:8}")
        else:
            print(f"{result.status:8} {result.error_msg or ''}")

    # 计算最佳启发式
    sat_results = {h: r for h, r in results.items() if r.status == "SAT"}
    if sat_results:
        best_heuristic = min(sat_results.keys(),
                           key=lambda h: sat_results[h].positives + sat_results[h].negatives)
        best_total = sat_results[best_heuristic].positives + sat_results[best_heuristic].negatives

        print(f"\n  最佳启发式: {best_heuristic} (Total={best_total})")

        # 显示节点数减少比例
        for h, r in sat_results.items():
            if h != best_heuristic:
                total = r.positives + r.negatives
                reduction = (total - best_total) / best_total * 100 if best_total > 0 else 0
                print(f"    {h}: +{reduction:.1f}% 节点数")


def main():
    import argparse

    parser = argparse.ArgumentParser(description="变量选择启发式性能对比")
    parser.add_argument("instances", nargs='+', help="测试实例路径列表")
    parser.add_argument("--timeout", type=int, default=30,
                        help="单个实例超时时间(秒)")

    args = parser.parse_args()

    # 检查构建目录
    if not BUILD_DIR.exists():
        print(f"错误: 构建目录不存在: {BUILD_DIR}")
        sys.exit(1)

    gpu_solver = BUILD_DIR / "gmodel_solver"
    if not gpu_solver.exists():
        print(f"错误: GPU 求解器不存在: {gpu_solver}")
        sys.exit(1)

    # 对每个实例进行基准测试
    for instance_path in args.instances:
        benchmark_instance(instance_path, args.timeout)

    print(f"\n{'='*80}")
    print("测试完成")
    print(f"{'='*80}")


if __name__ == "__main__":
    main()
