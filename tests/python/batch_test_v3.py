#!/usr/bin/env python3
"""
CPIM 三方对比测试框架 v3

对每个实例同时运行：
  1) CPIM CPU（build/cpim_test_parser）
  2) CPIM GPU（build/compare_cpu_gpu --gpu_only）
  3) OR-Tools CP（Python constraint_solver）

用途：
- 回归：检查 CPU/GPU 的 SAT/UNSAT 判定与 OR-Tools 是否一致
- 性能：记录三者耗时与搜索节点（branches/backtracks）

注意：
- 目前默认只对比 SAT/UNSAT/TIMEOUT 状态；不做解验证（需要额外的 solution verifier 支持）。
"""

import argparse
import csv
import os
import re
import subprocess
import sys
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import List, Optional

sys.path.insert(0, str(Path(__file__).parent))
from tier_definitions import get_tier_by_number  # noqa: E402
from solve_xcsp_ortools_cp import solve_xcsp_cp  # noqa: E402


@dataclass
class TripleTestResult:
    file: str
    num_vars: int
    num_cons: int

    cpu_status: str
    cpu_time_ms: int
    cpu_branches: int
    cpu_backtracks: int

    gpu_status: str
    gpu_time_ms: int
    gpu_branches: int
    gpu_backtracks: int

    ortools_status: str
    ortools_time_ms: int
    ortools_branches: int
    ortools_failures: int

    match_all: bool
    match_cpu_ortools: bool
    match_gpu_ortools: bool
    match_cpu_gpu: bool

    def __str__(self) -> str:
        cpu_str = f"{self.cpu_status}({self.cpu_time_ms}ms,{self.cpu_branches}b,{self.cpu_backtracks}f)"
        gpu_str = f"{self.gpu_status}({self.gpu_time_ms}ms,{self.gpu_branches}b,{self.gpu_backtracks}f)"
        ort_str = f"{self.ortools_status}({self.ortools_time_ms}ms,{self.ortools_branches}b,{self.ortools_failures}f)"
        ok = "OK" if self.match_all else "DIFF"
        return f"{self.file:<50} {cpu_str:<25} {gpu_str:<25} {ort_str:<25} {ok}"


def _run_cpim_cpu(xml_path: str, timeout_s: int) -> dict:
    """
    运行 CPIM CPU 求解器（cpim_test_parser）

    Returns: {status, time_ms, branches, backtracks}
    """
    cpim_path = Path(__file__).parent.parent.parent / "build" / "cpim_test_parser"
    if not cpim_path.exists():
        return {"status": "ERROR:not_built", "time_ms": 0, "branches": 0, "backtracks": 0}

    try:
        result = subprocess.run(
            [str(cpim_path), f"--bench_path={xml_path}"],
            capture_output=True,
            text=True,
            timeout=timeout_s,
        )
        output = (result.stdout or "") + (result.stderr or "")

        time_ms = 0
        branches = 0
        backtracks = 0

        time_match = re.search(r"time\s*=\s*(\d+)\s*ms", output)
        if time_match:
            time_ms = int(time_match.group(1))

        pos_match = re.search(r"positives?\s*=\s*(\d+)", output)
        if pos_match:
            branches = int(pos_match.group(1))

        neg_match = re.search(r"negatives?\s*=\s*(\d+)", output)
        if neg_match:
            backtracks = int(neg_match.group(1))

        if "did not find a solution" in output or "UNSAT" in output:
            status = "UNSAT"
        elif "MAC solution" in output or "solution found" in output:
            status = "SAT"
        elif "timed out" in output or result.returncode == 124:
            status = "TIMEOUT"
        else:
            status = "UNKNOWN"

        return {"status": status, "time_ms": time_ms, "branches": branches, "backtracks": backtracks}

    except subprocess.TimeoutExpired:
        return {"status": "TIMEOUT", "time_ms": timeout_s * 1000, "branches": 0, "backtracks": 0}
    except Exception as e:
        return {"status": f"ERROR:{str(e)[:20]}", "time_ms": 0, "branches": 0, "backtracks": 0}


def _run_cpim_gpu(xml_path: str, timeout_s: int) -> dict:
    """
    运行 CPIM GPU 求解器（compare_cpu_gpu --gpu_only）

    Returns: {status, time_ms, branches, backtracks}
    """
    gpu_path = Path(__file__).parent.parent.parent / "build" / "compare_cpu_gpu"
    if not gpu_path.exists():
        return {"status": "ERROR:not_built", "time_ms": 0, "branches": 0, "backtracks": 0}

    timeout_ms = timeout_s * 1000
    cmd = [str(gpu_path), f"--input={xml_path}", f"--time_limit={timeout_ms}", "--gpu_only"]

    try:
        t0 = time.time()
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout_s + 10,  # 允许解析/建模额外开销
        )
        wall_ms = int((time.time() - t0) * 1000)
        output = (result.stdout or "") + (result.stderr or "")

        # 解析统计（优先使用 solver 输出的求解时间；缺失则用 wall time）
        time_ms = wall_ms
        time_match = re.search(r"求解时间:\s*([0-9.]+)\s*s", output)
        if time_match:
            try:
                time_ms = int(float(time_match.group(1)) * 1000.0)
            except ValueError:
                pass

        branches = 0
        backtracks = 0
        pos_match = re.search(r"正向节点:\s*(\d+)", output)
        if pos_match:
            branches = int(pos_match.group(1))
        neg_match = re.search(r"回溯节点:\s*(\d+)", output)
        if neg_match:
            backtracks = int(neg_match.group(1))

        # 状态
        timeout_match = re.search(r"超时:\s*(是|否)", output)
        timed_out = (timeout_match is not None and timeout_match.group(1) == "是")

        sol_match = re.search(r"找到解数:\s*(\d+)", output)
        num_solutions = int(sol_match.group(1)) if sol_match else 0

        if timed_out:
            status = "TIMEOUT"
        elif num_solutions > 0:
            status = "SAT"
        elif result.returncode != 0:
            status = "ERROR:nonzero_exit"
        else:
            status = "UNSAT"

        return {"status": status, "time_ms": time_ms, "branches": branches, "backtracks": backtracks}

    except subprocess.TimeoutExpired:
        return {"status": "TIMEOUT", "time_ms": timeout_s * 1000, "branches": 0, "backtracks": 0}
    except Exception as e:
        return {"status": f"ERROR:{str(e)[:20]}", "time_ms": 0, "branches": 0, "backtracks": 0}


def run_single(xml_path: str, timeout_s: int) -> TripleTestResult:
    basename = os.path.basename(xml_path)

    cpu = _run_cpim_cpu(xml_path, timeout_s)
    gpu = _run_cpim_gpu(xml_path, timeout_s)
    ort = solve_xcsp_cp(xml_path, timeout_s, verbose=False)

    match_cpu_ort = (cpu["status"] == ort.status)
    match_gpu_ort = (gpu["status"] == ort.status)
    match_cpu_gpu = (cpu["status"] == gpu["status"])
    match_all = match_cpu_ort and match_gpu_ort

    return TripleTestResult(
        file=basename,
        num_vars=ort.num_vars,
        num_cons=ort.num_cons,
        cpu_status=cpu["status"],
        cpu_time_ms=cpu["time_ms"],
        cpu_branches=cpu["branches"],
        cpu_backtracks=cpu["backtracks"],
        gpu_status=gpu["status"],
        gpu_time_ms=gpu["time_ms"],
        gpu_branches=gpu["branches"],
        gpu_backtracks=gpu["backtracks"],
        ortools_status=ort.status,
        ortools_time_ms=ort.time_ms,
        ortools_branches=ort.branches,
        ortools_failures=ort.failures,
        match_all=match_all,
        match_cpu_ortools=match_cpu_ort,
        match_gpu_ortools=match_gpu_ort,
        match_cpu_gpu=match_cpu_gpu,
    )


def export_csv(results: List[TripleTestResult], csv_path: str) -> None:
    with open(csv_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(asdict(results[0]).keys()))
        writer.writeheader()
        for r in results:
            writer.writerow(asdict(r))

    print(f"Results exported to: {csv_path}")


def print_summary(results: List[TripleTestResult], show_stats: bool) -> None:
    total = len(results)
    match_all = sum(1 for r in results if r.match_all)
    match_cpu_ort = sum(1 for r in results if r.match_cpu_ortools)
    match_gpu_ort = sum(1 for r in results if r.match_gpu_ortools)
    match_cpu_gpu = sum(1 for r in results if r.match_cpu_gpu)

    print("\n" + "=" * 100)
    print("Summary:")
    print(f"  Total: {total}")
    print(f"  All matched: {match_all}/{total} ({match_all * 100 // max(1, total)}%)")
    print(f"  CPU vs OR-Tools: {match_cpu_ort}/{total}")
    print(f"  GPU vs OR-Tools: {match_gpu_ort}/{total}")
    print(f"  CPU vs GPU: {match_cpu_gpu}/{total}")

    mismatches = [r for r in results if not r.match_all]
    if mismatches:
        print(f"\n  Mismatches (all-three not matched): {len(mismatches)}")
        for r in mismatches:
            print(
                f"    - {r.file}: CPU={r.cpu_status}, GPU={r.gpu_status}, OR={r.ortools_status}"
            )

    if show_stats:
        def _avg(values: List[int]) -> float:
            return float(sum(values)) / max(1, len(values))

        cpu_valid = [r for r in results if r.cpu_status not in ("TIMEOUT",) and not r.cpu_status.startswith("ERROR")]
        gpu_valid = [r for r in results if r.gpu_status not in ("TIMEOUT",) and not r.gpu_status.startswith("ERROR")]
        ort_valid = [r for r in results if r.ortools_status not in ("TIMEOUT",) and not r.ortools_status.startswith("ERROR")]

        print("\n  Performance Statistics (exclude TIMEOUT/ERROR):")
        if cpu_valid:
            print(f"    CPU avg time: {_avg([r.cpu_time_ms for r in cpu_valid]):.1f}ms")
        if gpu_valid:
            print(f"    GPU avg time: {_avg([r.gpu_time_ms for r in gpu_valid]):.1f}ms")
        if ort_valid:
            print(f"    OR-Tools avg time: {_avg([r.ortools_time_ms for r in ort_valid]):.1f}ms")

    print("=" * 100 + "\n")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="CPIM 三方对比测试 v3：CPU vs GPU vs OR-Tools CP"
    )
    parser.add_argument("--tier", type=int, required=True, choices=[0, 1, 2, 3])
    parser.add_argument("--timeout", type=int, default=30, help="每个实例超时（秒）")
    parser.add_argument("--quiet", action="store_true", help="静默模式（只输出摘要）")
    parser.add_argument("--stats", action="store_true", help="输出统计信息（平均耗时等）")
    parser.add_argument("--export-csv", type=str, default="", help="导出 CSV 路径")
    args = parser.parse_args()

    files = get_tier_by_number(args.tier)
    if not args.quiet:
        print("=" * 100)
        print(f"CPIM Triple Test V3 - TIER{args.tier} ({len(files)} instances, timeout={args.timeout}s)")
        print("=" * 100)
        print(f"{'File':<50} {'CPIM-CPU':<25} {'CPIM-GPU':<25} {'OR-Tools':<25} {'Match'}")
        print("-" * 100)

    results: List[TripleTestResult] = []
    for i, xml_file in enumerate(files, 1):
        if not Path(xml_file).exists():
            if not args.quiet:
                print(f"[{i}/{len(files)}] {Path(xml_file).name:<50} SKIP (missing)")
            continue

        if not args.quiet:
            print(f"[{i}/{len(files)}] ", end="", flush=True)

        r = run_single(xml_file, args.timeout)
        results.append(r)
        if not args.quiet:
            print(r)

    print_summary(results, show_stats=args.stats)

    if args.export_csv and results:
        export_csv(results, args.export_csv)

    # 返回码：三者状态全一致才算通过
    all_ok = all(r.match_all for r in results)
    return 0 if all_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
