#!/usr/bin/env python3
"""
CPIM 批量测试框架 v2
比较 CPIM CPU 求解器与 OR-Tools 纯 CP 求解器的性能

特性：
- 使用 OR-Tools constraint_solver（纯 CP，非 CP-SAT）
- 启发式对齐：MinDomain + 最小值优先
- 详细统计对比：branches、failures、solve_time
- 可选解验证：调用 verify_search
- CSV 导出：支持数据分析
"""

import subprocess
import time
import sys
import os
import csv
import re
from dataclasses import dataclass, asdict
from typing import List, Optional
from pathlib import Path

# 导入测试集定义和 OR-Tools CP 求解器
sys.path.insert(0, str(Path(__file__).parent.parent / "tests"))
from tier_definitions import get_tier_by_number, get_tier_stats

# 导入 OR-Tools CP 求解器
from solve_xcsp_ortools_cp import solve_xcsp_cp


@dataclass
class TestResult:
    """单个测试的结果"""
    file: str
    num_vars: int
    num_cons: int

    # CPIM 结果
    cpim_status: str        # SAT, UNSAT, TIMEOUT, ERROR
    cpim_time_ms: int
    cpim_branches: int      # num_positive
    cpim_backtracks: int    # num_negative

    # OR-Tools 结果
    ortools_status: str
    ortools_time_ms: int
    ortools_branches: int
    ortools_failures: int

    # 对比结果
    match: bool             # SAT/UNSAT 是否一致
    verified: str           # OK, FAIL, N/A（解验证结果）

    def __str__(self):
        """格式化输出"""
        cpim_str = f"{self.cpim_status}({self.cpim_time_ms}ms,{self.cpim_branches}b,{self.cpim_backtracks}f)"
        ort_str = f"{self.ortools_status}({self.ortools_time_ms}ms,{self.ortools_branches}b,{self.ortools_failures}f)"
        match_str = "OK" if self.match else "DIFF"
        return f"{self.file:<50} {cpim_str:<25} {ort_str:<25} {match_str:<6} {self.verified}"


def run_cpim(xml_path: str, timeout: int) -> dict:
    """
    运行 CPIM 求解器

    Returns:
        {status, time_ms, branches, backtracks, num_vars, num_cons}
    """
    cpim_path = Path(__file__).parent.parent / "build" / "cpim_test_parser"

    if not cpim_path.exists():
        return {
            'status': 'ERROR:not_built',
            'time_ms': 0,
            'branches': 0,
            'backtracks': 0,
            'num_vars': 0,
            'num_cons': 0
        }

    try:
        # 运行 CPIM（不使用 GLOG_v，避免过多日志）
        result = subprocess.run(
            [str(cpim_path), f'--bench_path={xml_path}'],
            capture_output=True,
            text=True,
            timeout=timeout
        )

        output = result.stdout + result.stderr

        # 解析输出
        # 示例: "MAC stats: time=123 ms, positives=45, negatives=12, ..."
        time_ms = 0
        branches = 0
        backtracks = 0
        num_vars = 0
        num_cons = 0

        time_match = re.search(r'time\s*=\s*(\d+)\s*ms', output)
        if time_match:
            time_ms = int(time_match.group(1))

        pos_match = re.search(r'positive\s*=\s*(\d+)', output)
        if pos_match:
            branches = int(pos_match.group(1))

        neg_match = re.search(r'negative\s*=\s*(\d+)', output)
        if neg_match:
            backtracks = int(neg_match.group(1))

        vars_match = re.search(r'(\d+)\s+variables', output)
        if vars_match:
            num_vars = int(vars_match.group(1))

        cons_match = re.search(r'(\d+)\s+constraints', output)
        if cons_match:
            num_cons = int(cons_match.group(1))

        # 判断状态
        if 'did not find a solution' in output or 'UNSAT' in output:
            status = "UNSAT"
        elif 'MAC solution' in output or 'solution found' in output:
            status = "SAT"
        elif 'timed out' in output or result.returncode == 124:
            status = "TIMEOUT"
        else:
            status = "UNKNOWN"

        return {
            'status': status,
            'time_ms': time_ms,
            'branches': branches,
            'backtracks': backtracks,
            'num_vars': num_vars,
            'num_cons': num_cons
        }

    except subprocess.TimeoutExpired:
        return {
            'status': 'TIMEOUT',
            'time_ms': timeout * 1000,
            'branches': 0,
            'backtracks': 0,
            'num_vars': 0,
            'num_cons': 0
        }
    except Exception as e:
        return {
            'status': f'ERROR:{str(e)[:20]}',
            'time_ms': 0,
            'branches': 0,
            'backtracks': 0,
            'num_vars': 0,
            'num_cons': 0
        }


def run_verify_search(xml_path: str, timeout: int = 10) -> str:
    """
    运行 verify_search 验证解的正确性

    Returns:
        "OK", "FAIL", "N/A"
    """
    verify_path = Path(__file__).parent.parent / "build" / "verify_search_cpu"

    if not verify_path.exists():
        return "N/A"

    try:
        result = subprocess.run(
            [str(verify_path), f'--input={xml_path}', f'--timeout={timeout}'],
            capture_output=True,
            text=True,
            timeout=timeout + 5
        )

        output = result.stdout + result.stderr

        # 检查验证结果
        if 'CORRECT' in output or 'solution is valid' in output or 'All domains match' in output:
            return "OK"
        elif 'MISMATCH' in output or 'VIOLATION' in output or 'INCORRECT' in output:
            return "FAIL"
        else:
            return "N/A"

    except Exception:
        return "N/A"


def run_single_test(xml_path: str, timeout: int, verify: bool = False) -> TestResult:
    """运行单个测试实例"""
    basename = os.path.basename(xml_path)

    # 运行 CPIM
    cpim = run_cpim(xml_path, timeout)

    # 运行 OR-Tools CP
    ortools_result = solve_xcsp_cp(xml_path, timeout, verbose=False)

    # 解验证（仅当 CPIM 找到解时）
    verified = "N/A"
    if verify and cpim['status'] == "SAT":
        verified = run_verify_search(xml_path, timeout=10)

    # 检查一致性
    match = (cpim['status'] == ortools_result.status)

    return TestResult(
        file=basename,
        num_vars=ortools_result.num_vars if ortools_result.num_vars > 0 else cpim['num_vars'],
        num_cons=ortools_result.num_cons if ortools_result.num_cons > 0 else cpim['num_cons'],
        cpim_status=cpim['status'],
        cpim_time_ms=cpim['time_ms'],
        cpim_branches=cpim['branches'],
        cpim_backtracks=cpim['backtracks'],
        ortools_status=ortools_result.status,
        ortools_time_ms=ortools_result.time_ms,
        ortools_branches=ortools_result.branches,
        ortools_failures=ortools_result.failures,
        match=match,
        verified=verified
    )


def run_tier(tier: int, timeout: int, verify: bool = False, verbose: bool = True) -> List[TestResult]:
    """
    运行指定层级的测试

    Args:
        tier: 测试层级（0, 1, 2, 3）
        timeout: 超时时间（秒）
        verify: 是否验证解
        verbose: 是否输出详细信息

    Returns:
        测试结果列表
    """
    files = get_tier_by_number(tier)

    if verbose:
        print(f"\n{'='*100}")
        print(f"CPIM Batch Test V2 - TIER{tier} ({len(files)} instances, timeout={timeout}s)")
        print(f"{'='*100}")
        print(f"{'File':<50} {'CPIM':<25} {'OR-Tools':<25} {'Match':<6} {'Verified'}")
        print(f"{'-'*100}")

    results = []
    for i, xml_file in enumerate(files, 1):
        if verbose:
            print(f"[{i}/{len(files)}] ", end="", flush=True)

        result = run_single_test(xml_file, timeout, verify)
        results.append(result)

        if verbose:
            print(result)

    return results


def print_summary(results: List[TestResult], show_stats: bool = False):
    """打印测试摘要"""
    print(f"\n{'='*100}")
    print("Summary:")

    # 基本统计
    total = len(results)
    matched = sum(1 for r in results if r.match)
    verified_ok = sum(1 for r in results if r.verified == "OK")
    verified_fail = sum(1 for r in results if r.verified == "FAIL")

    print(f"  Total: {total}")
    print(f"  Matched: {matched}/{total} ({matched*100//total}%)")
    print(f"  Verified: OK={verified_ok}, FAIL={verified_fail}")

    # 不匹配的实例
    mismatches = [r for r in results if not r.match]
    if mismatches:
        print(f"\n  Mismatches ({len(mismatches)}):")
        for r in mismatches:
            print(f"    - {r.file}: CPIM={r.cpim_status}, OR-Tools={r.ortools_status}")

    # 验证失败的实例
    verify_fails = [r for r in results if r.verified == "FAIL"]
    if verify_fails:
        print(f"\n  Verification Failures ({len(verify_fails)}):")
        for r in verify_fails:
            print(f"    - {r.file}")

    # 详细统计（可选）
    if show_stats:
        print(f"\n  Performance Statistics:")

        # 计算平均值（排除 ERROR 和 TIMEOUT）
        valid_results = [r for r in results
                         if not r.cpim_status.startswith('ERROR')
                         and not r.ortools_status.startswith('ERROR')
                         and r.cpim_status != 'TIMEOUT'
                         and r.ortools_status != 'TIMEOUT']

        if valid_results:
            cpim_avg_time = sum(r.cpim_time_ms for r in valid_results) / len(valid_results)
            ort_avg_time = sum(r.ortools_time_ms for r in valid_results) / len(valid_results)
            cpim_avg_branches = sum(r.cpim_branches for r in valid_results) / len(valid_results)
            ort_avg_branches = sum(r.ortools_branches for r in valid_results) / len(valid_results)

            print(f"    CPIM avg time: {cpim_avg_time:.1f}ms")
            print(f"    OR-Tools avg time: {ort_avg_time:.1f}ms")
            print(f"    CPIM avg branches: {cpim_avg_branches:.1f}")
            print(f"    OR-Tools avg branches: {ort_avg_branches:.1f}")

            if ort_avg_time > 0:
                speedup = (ort_avg_time - cpim_avg_time) / ort_avg_time * 100
                print(f"    CPIM speedup: {speedup:+.1f}%")

    print(f"{'='*100}\n")


def export_csv(results: List[TestResult], csv_path: str):
    """导出结果到 CSV 文件"""
    with open(csv_path, 'w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=[
            'file', 'num_vars', 'num_cons',
            'cpim_status', 'cpim_time_ms', 'cpim_branches', 'cpim_backtracks',
            'ortools_status', 'ortools_time_ms', 'ortools_branches', 'ortools_failures',
            'match', 'verified'
        ])
        writer.writeheader()
        for r in results:
            writer.writerow(asdict(r))

    print(f"Results exported to: {csv_path}\n")


# ============================================================================
# CLI 接口
# ============================================================================

def main():
    """命令行入口"""
    import argparse

    parser = argparse.ArgumentParser(
        description="CPIM 批量测试框架 v2 - 对比 CPIM 与 OR-Tools 纯 CP"
    )
    parser.add_argument('--tier', type=int, required=True, choices=[0, 1, 2, 3],
                        help='测试层级：0=烟测(12), 1=标准(35), 2=完整(75), 3=全量(1000+)')
    parser.add_argument('--timeout', type=int, default=30,
                        help='每个实例的超时时间（秒），默认 30s')
    parser.add_argument('--verify', action='store_true',
                        help='使用 verify_search 验证 CPIM 的解')
    parser.add_argument('--stats', action='store_true',
                        help='显示详细统计信息')
    parser.add_argument('--export-csv', type=str,
                        help='导出结果到 CSV 文件')
    parser.add_argument('--quiet', action='store_true',
                        help='静默模式（只显示摘要）')

    args = parser.parse_args()

    # 检查 CPIM 是否已编译
    cpim_path = Path(__file__).parent.parent / "build" / "cpim_test_parser"
    if not cpim_path.exists():
        print(f"Error: CPIM not built. Please run: cd build && make -j4")
        sys.exit(1)

    # 运行测试
    results = run_tier(
        tier=args.tier,
        timeout=args.timeout,
        verify=args.verify,
        verbose=not args.quiet
    )

    # 打印摘要
    print_summary(results, show_stats=args.stats)

    # 导出 CSV
    if args.export_csv:
        export_csv(results, args.export_csv)

    # 返回退出码
    matched = sum(1 for r in results if r.match)
    if matched == len(results):
        sys.exit(0)  # 全部匹配
    else:
        sys.exit(1)  # 有不匹配


if __name__ == "__main__":
    main()
