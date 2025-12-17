#!/usr/bin/env python3
"""
使用 OR-Tools 纯 CP 求解器（constraint_solver）求解 XCSP 2.1 格式的约束满足问题

与 CPIM 启发式对齐：
- 变量选择：MinDomain (CHOOSE_MIN_SIZE_LOWEST_MIN)
- 值选择：最小值优先 (ASSIGN_MIN_VALUE)
- 传播算法：GAC (AllowedAssignments)
"""

import xml.etree.ElementTree as ET
import sys
import time
from dataclasses import dataclass
from typing import List, Tuple, Dict

try:
    from ortools.constraint_solver import pywrapcp
    HAS_ORTOOLS_CP = True
except ImportError:
    HAS_ORTOOLS_CP = False
    print("Error: OR-Tools constraint_solver not installed")
    print("Install with: pip3 install ortools")
    sys.exit(1)


@dataclass
class SolveResult:
    """求解结果"""
    status: str        # SAT, UNSAT, TIMEOUT, ERROR
    time_ms: int       # 求解时间（毫秒）
    num_vars: int      # 变量数
    num_cons: int      # 约束数
    branches: int      # 分支数（对应 CPIM 的 num_positive）
    failures: int      # 失败数（对应 CPIM 的 num_negative）
    solutions: int     # 解数量
    solution: List[int] = None  # 第一个解（如果有）


def parse_domain(domain_str: str) -> List[int]:
    """解析域字符串，返回值列表"""
    values = []
    for part in domain_str.strip().split():
        if '..' in part:
            # 范围格式: a..b
            start, end = map(int, part.split('..'))
            values.extend(range(start, end + 1))
        else:
            values.append(int(part))
    return values


def parse_tuples(tuple_str: str) -> List[Tuple[int, ...]]:
    """解析元组字符串，返回元组列表"""
    tuples = []
    if not tuple_str:
        return tuples

    for t in tuple_str.strip().split('|'):
        if t.strip():
            tuples.append(tuple(map(int, t.strip().split())))
    return tuples


def parse_xcsp(xml_path: str) -> Tuple[Dict, Dict, Dict, List]:
    """
    解析 XCSP 文件

    Returns:
        (domains, variables, relations, constraints)
    """
    tree = ET.parse(xml_path)
    root = tree.getroot()

    # 解析域
    domains = {}
    for domain in root.findall('.//domain'):
        name = domain.get('name')
        values = parse_domain(domain.text)
        domains[name] = values

    # 解析变量
    variables = {}
    var_order = []
    for var in root.findall('.//variable'):
        name = var.get('name')
        domain_name = var.get('domain')
        variables[name] = domains[domain_name]
        var_order.append(name)

    # 解析关系
    relations = {}
    for rel in root.findall('.//relation'):
        name = rel.get('name')
        semantics = rel.get('semantics')
        tuples = parse_tuples(rel.text) if rel.text else []
        relations[name] = {'semantics': semantics, 'tuples': tuples}

    # 解析约束
    constraints = []
    for cons in root.findall('.//constraint'):
        scope = cons.get('scope').split()
        ref = cons.get('reference')
        constraints.append({'scope': scope, 'relation': ref})

    return domains, variables, var_order, relations, constraints


def solve_xcsp_cp(xml_path: str, timeout_sec: int = 30, verbose: bool = False) -> SolveResult:
    """
    使用 OR-Tools 纯 CP 求解器求解 XCSP 问题

    Args:
        xml_path: XCSP 文件路径
        timeout_sec: 超时时间（秒）
        verbose: 是否输出详细日志

    Returns:
        SolveResult 对象
    """
    if not HAS_ORTOOLS_CP:
        return SolveResult(
            status="ERROR",
            time_ms=0,
            num_vars=0,
            num_cons=0,
            branches=0,
            failures=0,
            solutions=0
        )

    try:
        # 1. 解析 XCSP
        if verbose:
            print(f"[OR-Tools CP] Parsing: {xml_path}")

        parse_start = time.time()
        domains, variables, var_order, relations, constraints = parse_xcsp(xml_path)
        parse_time = time.time() - parse_start

        if verbose:
            print(f"[OR-Tools CP] Parse time: {parse_time:.3f}s")
            print(f"[OR-Tools CP] Variables: {len(variables)}, Constraints: {len(constraints)}")

        # 2. 创建 Solver
        solver = pywrapcp.Solver("XCSP_CP_Solver")

        # 3. 创建变量
        cp_vars = {}
        for var_name in var_order:
            domain_values = variables[var_name]
            # 使用 IntVar 创建变量（支持非连续域）
            cp_vars[var_name] = solver.IntVar(
                min(domain_values),
                max(domain_values),
                var_name
            )

        # 4. 添加约束
        for cons in constraints:
            scope = cons['scope']
            rel = relations[cons['relation']]
            semantics = rel['semantics']
            tuples = rel['tuples']

            scope_vars = [cp_vars[s] for s in scope]

            if semantics == 'supports':
                # 允许的元组（GAC）
                solver.Add(solver.AllowedAssignments(scope_vars, tuples))
            else:  # conflicts
                # 禁止的元组 - 需要计算补集
                # 生成所有可能的元组
                domains = [variables[var_name] for var_name in scope]

                def generate_all_tuples(domains_list):
                    """生成域的笛卡尔积"""
                    if not domains_list:
                        return [()]
                    first = domains_list[0]
                    rest = generate_all_tuples(domains_list[1:])
                    return [(v,) + r for v in first for r in rest]

                all_tuples = generate_all_tuples(domains)
                forbidden_set = set(tuples)
                allowed_tuples = [t for t in all_tuples if t not in forbidden_set]

                solver.Add(solver.AllowedAssignments(scope_vars, allowed_tuples))

        # 5. 配置搜索策略（与 CPIM 对齐）
        var_list = [cp_vars[v] for v in var_order]

        # 关键：使用与 CPIM 相同的启发式
        # VRH_DOM_MIN (最小域优先) + VLH_MIN (最小值优先)
        db = solver.Phase(
            var_list,
            solver.CHOOSE_MIN_SIZE_LOWEST_MIN,  # 对应 CPIM 的 VRH_DOM_MIN
            solver.ASSIGN_MIN_VALUE             # 对应 CPIM 的 VLH_MIN
        )

        # 6. 设置监控和限制
        # 时间限制
        time_limit = solver.TimeLimit(timeout_sec * 1000)

        # 解收集器
        collector = solver.FirstSolutionCollector()
        collector.Add(var_list)

        # 7. 求解
        if verbose:
            print("[OR-Tools CP] Solving...")

        solve_start = time.time()
        solver.Solve(db, [collector, time_limit])
        solve_time_ms = int((time.time() - solve_start) * 1000)

        # 8. 收集结果
        num_solutions = collector.SolutionCount()
        branches = solver.Branches()
        failures = solver.Failures()

        # 提取解
        solution = None
        if num_solutions > 0:
            solution = [collector.Value(0, var) for var in var_list]

        # 判断状态
        if num_solutions > 0:
            status = "SAT"
        elif failures > 0 and branches > 0:
            # 有搜索但无解
            status = "UNSAT"
        elif solver.WallTime() >= timeout_sec * 1000:
            status = "TIMEOUT"
        else:
            status = "UNSAT"

        if verbose:
            print(f"[OR-Tools CP] Status: {status}")
            print(f"[OR-Tools CP] Time: {solve_time_ms}ms")
            print(f"[OR-Tools CP] Branches: {branches}, Failures: {failures}")
            if solution:
                print(f"[OR-Tools CP] Solution (first 20): {solution[:20]}")

        return SolveResult(
            status=status,
            time_ms=solve_time_ms,
            num_vars=len(variables),
            num_cons=len(constraints),
            branches=branches,
            failures=failures,
            solutions=num_solutions,
            solution=solution
        )

    except Exception as e:
        error_msg = str(e)[:50]
        if verbose:
            print(f"[OR-Tools CP] Error: {error_msg}")

        return SolveResult(
            status=f"ERROR:{error_msg}",
            time_ms=0,
            num_vars=0,
            num_cons=0,
            branches=0,
            failures=0,
            solutions=0
        )


# ============================================================================
# CLI 接口
# ============================================================================

def main():
    """命令行入口"""
    import argparse

    parser = argparse.ArgumentParser(
        description="使用 OR-Tools 纯 CP 求解器求解 XCSP 问题"
    )
    parser.add_argument("input", help="XCSP 文件路径")
    parser.add_argument("--timeout", type=int, default=30,
                        help="超时时间（秒），默认 30s")
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="输出详细日志")

    args = parser.parse_args()

    # 求解
    result = solve_xcsp_cp(args.input, args.timeout, args.verbose)

    # 输出结果
    if not args.verbose:
        print(f"\n=== OR-Tools CP 求解结果 ===")
        print(f"文件: {args.input}")
        print(f"状态: {result.status}")
        print(f"时间: {result.time_ms}ms")
        print(f"变量数: {result.num_vars}, 约束数: {result.num_cons}")
        print(f"分支数: {result.branches}, 失败数: {result.failures}")
        if result.solution:
            print(f"解（前 20 个变量）: {result.solution[:20]}")
        print()

    # 返回退出码
    if result.status == "SAT":
        sys.exit(0)
    elif result.status == "UNSAT":
        sys.exit(1)
    elif result.status == "TIMEOUT":
        sys.exit(2)
    else:
        sys.exit(3)


if __name__ == "__main__":
    main()
