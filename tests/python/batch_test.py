#!/usr/bin/env python3
"""
CPIM 求解器批量测试脚本
比较 OR-Tools 和 CPIM CPU 求解器的结果
"""
import subprocess
import time
import sys
import os
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from typing import Optional, List
import re

# OR-Tools imports
try:
    from ortools.sat.python import cp_model
    HAS_ORTOOLS = True
except ImportError:
    HAS_ORTOOLS = False
    print("Warning: OR-Tools not installed")


@dataclass
class TestResult:
    file: str
    num_vars: int
    num_cons: int
    ortools_result: str  # SAT, UNSAT, TIMEOUT, ERROR
    ortools_time_ms: int
    cpim_result: str
    cpim_time_ms: int
    verified: str  # OK, FAIL, N/A


def parse_domain(domain_str):
    values = []
    for part in domain_str.strip().split():
        if '..' in part:
            start, end = map(int, part.split('..'))
            values.extend(range(start, end + 1))
        else:
            values.append(int(part))
    return values


def parse_tuples(tuple_str):
    tuples = []
    for t in tuple_str.strip().split('|'):
        if t.strip():
            tuples.append(tuple(map(int, t.strip().split())))
    return tuples


def solve_ortools(xml_path: str, timeout_sec: int = 30) -> tuple:
    """Run OR-Tools solver. Returns (result, time_ms, num_vars, num_cons)"""
    if not HAS_ORTOOLS:
        return ("ERROR", 0, 0, 0)

    try:
        tree = ET.parse(xml_path)
        root = tree.getroot()

        # Parse domains
        domains = {}
        for domain in root.findall('.//domain'):
            name = domain.get('name')
            values = parse_domain(domain.text)
            domains[name] = values

        # Parse variables
        variables = {}
        var_order = []
        for var in root.findall('.//variable'):
            name = var.get('name')
            domain_name = var.get('domain')
            variables[name] = domains[domain_name]
            var_order.append(name)

        # Parse relations
        relations = {}
        for rel in root.findall('.//relation'):
            name = rel.get('name')
            semantics = rel.get('semantics')
            tuples = parse_tuples(rel.text) if rel.text else []
            relations[name] = {'semantics': semantics, 'tuples': tuples}

        # Parse constraints
        constraints = []
        for cons in root.findall('.//constraint'):
            scope = cons.get('scope').split()
            ref = cons.get('reference')
            constraints.append({'scope': scope, 'relation': ref})

        num_vars = len(variables)
        num_cons = len(constraints)

        # Create model
        model = cp_model.CpModel()
        cp_vars = {}
        for var_name in var_order:
            domain = variables[var_name]
            cp_vars[var_name] = model.NewIntVarFromDomain(
                cp_model.Domain.FromValues(domain), var_name)

        # Add constraints
        for cons in constraints:
            scope = cons['scope']
            rel = relations[cons['relation']]
            semantics = rel['semantics']
            tuples = rel['tuples']
            scope_vars = [cp_vars[s] for s in scope]
            if semantics == 'supports':
                model.AddAllowedAssignments(scope_vars, tuples)
            else:
                model.AddForbiddenAssignments(scope_vars, tuples)

        # Solve
        solver = cp_model.CpSolver()
        solver.parameters.max_time_in_seconds = timeout_sec
        solver.parameters.log_search_progress = False

        start = time.time()
        status = solver.Solve(model)
        elapsed_ms = int((time.time() - start) * 1000)

        if status == cp_model.OPTIMAL or status == cp_model.FEASIBLE:
            return ("SAT", elapsed_ms, num_vars, num_cons)
        elif status == cp_model.INFEASIBLE:
            return ("UNSAT", elapsed_ms, num_vars, num_cons)
        else:
            return ("TIMEOUT", elapsed_ms, num_vars, num_cons)

    except Exception as e:
        return (f"ERROR:{str(e)[:20]}", 0, 0, 0)


def solve_cpim(xml_path: str, timeout_sec: int = 30) -> tuple:
    """Run CPIM solver. Returns (result, time_ms, verified)"""
    cpim_path = os.path.join(os.path.dirname(__file__), '../build/cpim_test_parser')
    if not os.path.exists(cpim_path):
        return ("ERROR:not_built", 0, "N/A")

    try:
        env = os.environ.copy()
        env['GLOG_v'] = '1'  # Enable verification output

        result = subprocess.run(
            [cpim_path, f'--bench_path={xml_path}'],
            capture_output=True,
            text=True,
            timeout=timeout_sec,
            env=env
        )

        output = result.stdout + result.stderr

        # Parse time
        time_ms = 0
        time_match = re.search(r'MAC stats: time=(\d+) ms', output)
        if time_match:
            time_ms = int(time_match.group(1))

        # Parse result
        if 'did not find a solution' in output:
            # UNSAT - no solution exists
            return ("UNSAT", time_ms, "N/A")
        elif 'MAC solution' in output and 'original values' in output:
            # SAT - solution found, check verification
            if 'All constraints satisfied' in output:
                return ("SAT", time_ms, "OK")
            elif 'VIOLATION' in output:
                return ("SAT", time_ms, "FAIL")
            else:
                return ("SAT", time_ms, "N/A")
        elif 'timed out' in output:
            return ("TIMEOUT", time_ms, "N/A")
        else:
            return ("UNKNOWN", time_ms, "N/A")

    except subprocess.TimeoutExpired:
        return ("TIMEOUT", timeout_sec * 1000, "N/A")
    except Exception as e:
        return (f"ERROR:{str(e)[:20]}", 0, "N/A")


def run_tests(test_files: List[str], timeout_sec: int = 30):
    """Run tests on all files and print results"""
    results = []

    print(f"\n{'='*80}")
    print(f"CPIM Solver Batch Test - {len(test_files)} files, timeout={timeout_sec}s")
    print(f"{'='*80}")
    print(f"{'File':<45} {'OR-Tools':<12} {'CPIM':<12} {'Match':<6} {'Verified'}")
    print(f"{'-'*80}")

    for f in test_files:
        basename = os.path.basename(f)

        # OR-Tools test
        ort_result, ort_time, num_vars, num_cons = solve_ortools(f, timeout_sec)

        # CPIM test
        cpim_result, cpim_time, verified = solve_cpim(f, timeout_sec)

        # Check match
        match = "OK" if ort_result == cpim_result else "DIFF"
        if ort_result.startswith("ERROR") or cpim_result.startswith("ERROR"):
            match = "ERR"

        print(f"{basename:<45} {ort_result}({ort_time}ms) {cpim_result}({cpim_time}ms) {match:<6} {verified}")

        results.append(TestResult(
            file=basename,
            num_vars=num_vars,
            num_cons=num_cons,
            ortools_result=ort_result,
            ortools_time_ms=ort_time,
            cpim_result=cpim_result,
            cpim_time_ms=cpim_time,
            verified=verified
        ))

    # Summary
    print(f"\n{'='*80}")
    print("Summary:")
    matched = sum(1 for r in results if r.ortools_result == r.cpim_result)
    verified_ok = sum(1 for r in results if r.verified == "OK")
    verified_fail = sum(1 for r in results if r.verified == "FAIL")
    print(f"  Matched: {matched}/{len(results)}")
    print(f"  Verified OK: {verified_ok}, FAIL: {verified_fail}")
    print(f"{'='*80}\n")

    return results


# Test file sets
TIER0 = [
    "benchmarks/XMLFile.xml",
    "benchmarks/langford/langford-3-9-ext.xml",
    "benchmarks/langford/langford-3-11-ext.xml",
    "benchmarks/langford/langford-2-4-ext.xml",
]

TIER1 = [
    "benchmarks/driver/driverlogw-09-sat_ext.xml",
    "benchmarks/BH-4-4/BlackHole-4-4-e-0_ext.xml",
    "benchmarks/BH-4-4/BlackHole-4-4-e-3_ext.xml",
    "benchmarks/tightness0.1/rand-2-40-8-753-100-0_ext.xml",
    "benchmarks/tightness0.1/rand-2-40-8-753-100-1_ext.xml",
    "benchmarks/tightness0.1/rand-2-40-8-753-100-2_ext.xml",
]

TIER2 = [
    "benchmarks/tightness0.8/rand-2-40-80-103-800-0_ext.xml",
    "benchmarks/tightness0.8/rand-2-40-80-103-800-1_ext.xml",
    "benchmarks/tightness0.9/rand-2-40-180-84-900-0_ext.xml",
    "benchmarks/graphs/graphw-05_ext.xml",
    "benchmarks/graphs/graphw-06_ext.xml",
]


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description='CPIM Batch Test')
    parser.add_argument('--tier', type=int, default=0, choices=[0, 1, 2, 99],
                        help='Test tier (0=smoke, 1=quick, 2=standard, 99=all)')
    parser.add_argument('--timeout', type=int, default=30, help='Timeout per instance (seconds)')
    parser.add_argument('--file', type=str, help='Test single file')
    args = parser.parse_args()

    os.chdir(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

    if args.file:
        run_tests([args.file], args.timeout)
    elif args.tier == 0:
        run_tests(TIER0, args.timeout)
    elif args.tier == 1:
        run_tests(TIER1, args.timeout)
    elif args.tier == 2:
        run_tests(TIER2, args.timeout)
    elif args.tier == 99:
        run_tests(TIER0 + TIER1 + TIER2, args.timeout)
